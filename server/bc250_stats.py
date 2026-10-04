#!/usr/bin/env python3
"""bc250-stats: tiny read-only HTTP endpoint with live system stats for the AMD BC-250.

    GET /stats.txt            flat key=value lines: everything the display's home page needs
    GET /stats                the same, as JSON (plus the detail sections), for humans and scripts
    GET /x/<cpu|gpu|mem|ai>.txt   extra detail for one display page (only fetched while that page is open)
    GET /hist?m=a,b&step=4&n=150  history: the last n*step seconds averaged into n buckets, one line per metric
    GET /health               "ok"

Standard library only, nothing here needs root. Sensors are located by hwmon *name* rather than
index, because hwmonN numbers can change between boots.
"""
import argparse
import collections
import glob
import json
import os
import re
import socket
import sys
import threading
import time
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HWMON = os.environ.get("BC250_STATS_HWMON", "/sys/class/hwmon")   # override only for testing against a fake tree
TICK = os.sysconf("SC_CLK_TCK")
PAGE = os.sysconf("SC_PAGE_SIZE")
OLLAMA = "http://127.0.0.1:11434"
HIST_LEN = 3600  # one sample per second -> one hour
HIST_KEYS = ["cpu_usage", "gpu_usage", "cpu_temp", "gpu_temp", "vrm_temp", "power_w", "cpu_mhz", "gpu_mhz",
             "ram_pct", "vram_pct", "gtt_pct", "rx_bps", "tx_bps", "fan_rpm"]
SLOW_EVERY = 3  # seconds between process scans / ollama polls
# hwmon chips: the first one found wins. AMD is what this was built on; the others are best-effort.
CPU_CHIPS = ("k10temp", "zenpower", "coretemp")
CPU_TEMP_LABELS = ("Tctl", "Tdie", "Package id 0")
SIO_CHIPS = ("nct6686", "nct6687", "nct6683", "nct6775", "nct6776", "nct6779", "nct6791", "nct6792", "nct6793",
             "nct6795", "nct6796", "nct6797", "nct6798", "nct6799", "it8603", "it8613", "it8620", "it8625",
             "it8628", "it8686", "it8688", "it8712", "it8728", "it8771", "it8772", "it8792")
SYSTEM_TEMP_LABELS = ("System", "SYSTIN", "Motherboard", "MB")
VRM_TEMP_LABELS = ("VRM MOS", "VRM", "MOS")
# Wine/Proton helper processes that are not "the game"
IGNORE_EXE = {
    "services.exe", "winedevice.exe", "plugplay.exe", "svchost.exe", "rpcss.exe", "explorer.exe", "conhost.exe",
    "wineboot.exe", "start.exe", "tabtip.exe", "winemenubuilder.exe", "steam.exe", "steamwebhelper.exe",
    "epicgameslauncher.exe", "epicwebhelper.exe", "crashpad_handler.exe", "crashreportclient.exe",
    "xalia.exe", "umu.exe", "winedbg.exe", "iexplore.exe", "launcher.exe",
}
BIG_PROC_BYTES = 100 * 1024 * 1024  # a game's main process is big; only these get their command line looked at


# ---------------------------------------------------------------- sysfs helpers
def read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def num(path, scale=1.0, ndigits=1):
    v = read(path)
    try:
        r = round(float(v) / scale, ndigits)
    except (TypeError, ValueError):
        return None
    return int(r) if ndigits == 0 else r


def find_hwmon(*names):
    for d in sorted(glob.glob(f"{HWMON}/hwmon*")):
        if read(f"{d}/name") in names:
            return d
    return None


def labelled(hw, prefix, scale=1.0):
    """{label: value} for temp*/fan*/in*... inputs of a hwmon dir that have a label."""
    out = {}
    if not hw:
        return out
    for lab in glob.glob(f"{hw}/{prefix}*_label"):
        name = read(lab)
        raw = read(lab.replace("_label", "_input"))
        if name and raw is not None:
            try:
                out[name] = float(raw) / scale
            except ValueError:
                pass
    return out


def clean(s):
    """Values go into key=value lines, so keep them to one line without '='."""
    return re.sub(r"[=\r\n]+", "_", str(s)).strip()


# ---------------------------------------------------------------- cpu / mem / net
def cpu_times():
    """{'cpu': (idle, total), 'cpu0': (...), ...} from /proc/stat."""
    out = {}
    with open("/proc/stat") as f:
        for line in f:
            if not line.startswith("cpu"):
                break
            parts = line.split()
            vals = [int(x) for x in parts[1:9]]
            out[parts[0]] = (vals[3] + vals[4], sum(vals))
    return out


def cpu_max_mhz():
    best = None
    for p in glob.glob("/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq"):
        v = read(p)
        if v and v.isdigit():
            best = max(best or 0, int(v))
    return round(best / 1000) if best else None


def default_iface():
    """Interface that holds the default route."""
    try:
        with open("/proc/net/route") as f:
            next(f)
            for line in f:
                p = line.split()
                if p[1] == "00000000" and int(p[3], 16) & 2:
                    return p[0]
    except (OSError, IndexError, ValueError):
        pass
    return None


def net_bytes(iface):
    rx = read(f"/sys/class/net/{iface}/statistics/rx_bytes") if iface else None
    tx = read(f"/sys/class/net/{iface}/statistics/tx_bytes") if iface else None
    return (int(rx), int(tx)) if rx and tx else None


def meminfo():
    out = {}
    with open("/proc/meminfo") as f:
        for line in f:
            k, v = line.split(":", 1)
            out[k] = int(v.split()[0]) * 1024
    return out


def pct(used, total):
    return round(100.0 * used / total, 1) if used is not None and total else None


# ---------------------------------------------------------------- sampler
class Sampler(threading.Thread):
    """Samples once per second so requests are instant and rates are real deltas."""

    def __init__(self):
        super().__init__(daemon=True)
        self.lock = threading.Lock()
        self.main = {}
        self.extra = {"cpu": {}, "gpu": {}, "mem": {}, "ai": {}}
        self.hist = {k: collections.deque(maxlen=HIST_LEN) for k in HIST_KEYS}
        self.peaks = {}
        self.host = socket.gethostname()
        # slow-moving state, refreshed every SLOW_EVERY seconds
        self.proc_prev, self.proc_t = {}, time.monotonic()
        self.top_cpu, self.top_mem = [], []
        self.activity = ("idle", "")
        self.ollama = {"up": False, "loaded": [], "installed": [], "tags_t": 0.0}

    # ------------------------------------------------------------ main loop
    def run(self):
        prev_cpu = cpu_times()
        iface = default_iface()
        prev_net = net_bytes(iface)
        prev_t = time.monotonic()
        next_t = prev_t + 0.5
        tick = 0
        while True:
            time.sleep(max(0.0, next_t - time.monotonic()))
            next_t += 1.0
            try:
                now = time.monotonic()
                dt = max(now - prev_t, 1e-3)

                cur_cpu = cpu_times()
                usage = {}
                for name, (idle, total) in cur_cpu.items():
                    p_idle, p_total = prev_cpu.get(name, (idle, total))
                    d_total = total - p_total
                    usage[name] = round(100.0 * (1 - (idle - p_idle) / d_total), 1) if d_total > 0 else 0.0
                prev_cpu = cur_cpu

                iface = default_iface() or iface
                cur_net = net_bytes(iface)
                rx = tx = None
                if cur_net and prev_net:
                    rx = max(0, round((cur_net[0] - prev_net[0]) / dt))
                    tx = max(0, round((cur_net[1] - prev_net[1]) / dt))
                prev_net, prev_t = cur_net, now

                if tick % SLOW_EVERY == 0:
                    self.slow_sample(now)
                tick += 1

                self.sample(usage, iface, rx, tx)
            except Exception as e:  # keep the thread alive no matter what
                print(f"sample failed: {e!r}", file=sys.stderr, flush=True)

    # ------------------------------------------------------------ slow stuff (processes, ollama)
    def slow_sample(self, now):
        try:
            self.scan_procs(now)
        except Exception as e:
            print(f"proc scan failed: {e!r}", file=sys.stderr, flush=True)
        try:
            self.poll_ollama()
        except Exception as e:
            print(f"ollama poll failed: {e!r}", file=sys.stderr, flush=True)
        self.activity = self.detect_activity()

    def scan_procs(self, now):
        cur, watch, exes = {}, [], []
        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue
            try:
                with open(f"/proc/{pid}/stat") as f:
                    data = f.read()
            except OSError:
                continue
            comm = data[data.index("(") + 1:data.rindex(")")]
            rest = data[data.rindex(")") + 2:].split()
            cur[pid] = (comm, int(rest[11]) + int(rest[12]), int(rest[21]) * PAGE)
            if comm in ("reaper", "gamescope", "gamescope-wl"):
                watch.append((pid, comm))
            if int(rest[21]) * PAGE >= BIG_PROC_BYTES:
                exes.append((pid, comm, int(rest[21]) * PAGE))
        dt = max(now - self.proc_t, 1e-3)
        by_cpu, by_mem = collections.defaultdict(float), collections.defaultdict(int)
        for pid, (comm, jiffies, rss) in cur.items():
            by_mem[comm] += rss
            prev = self.proc_prev.get(pid)
            if prev and prev[0] == comm:
                by_cpu[comm] += (jiffies - prev[1]) / TICK / dt * 100
        self.proc_prev, self.proc_t = cur, now
        self.top_cpu = sorted(by_cpu.items(), key=lambda kv: -kv[1])[:3]
        self.top_mem = sorted(by_mem.items(), key=lambda kv: -kv[1])[:3]
        self.watch, self.exes = watch, exes

    def running_game(self):
        """Name of the running game, whichever launcher started it. Only ever returns a name, never a command line.

        1. Steam: `reaper SteamLaunch ... steamapps/common/<Game>/` -> the folder name.
        2. Wine/Proton (Heroic, Lutris, Bottles...): the biggest process whose argv[0] is a Windows .exe that is
           not a Wine helper. (The kernel cuts process names to 15 chars, so the name alone is not enough.)
        3. Gamescope without either of the above.
        """
        for pid, comm in getattr(self, "watch", []):
            if comm != "reaper":
                continue
            for folder in re.findall(r"steamapps/common/([^/\x00]+)/", read_bytes(f"/proc/{pid}/cmdline")):
                if not folder.lower().startswith(("proton", "steamlinuxruntime", "steam linux runtime")):
                    return folder
        best = None
        for pid, comm, rss in getattr(self, "exes", []):
            path = read_arg0(pid)
            if not path.lower().endswith(".exe"):
                continue
            exe = re.split(r"[\\/]", path)[-1]
            if exe.lower() in IGNORE_EXE or "xalia" in path.lower() or "/proton" in path.lower().replace("\\", "/"):
                continue
            if best is None or rss > best[0]:
                best = (rss, path, exe)
        if best:
            _, path, exe = best
            for marker in ("Heroic", r"steamapps[\\/]common", "Games"):   # most specific first
                m = re.search(r"[\\/]" + marker + r"[\\/]([^\\/]+)[\\/]", path, re.I)
                if m:
                    return m.group(1)                   # the install folder reads better than LOF2-Win64-Shipping
            return re.sub(r"[-_](Win64|Win32|x64)?[-_]?Shipping$|[-_](Win64|Win32)$", "", exe[:-4], flags=re.I)
        if any(c.startswith("gamescope") for _, c in getattr(self, "watch", [])):
            return "Gamescope"
        return None

    def detect_activity(self):
        game = self.running_game()
        if game:
            return ("game", game)
        if self.ollama["loaded"]:
            return ("llm", self.ollama["loaded"][0]["name"])
        return ("idle", "")

    def poll_ollama(self):
        o = self.ollama
        try:
            ps = http_json(f"{OLLAMA}/api/ps")
            o["up"] = True
            o["loaded"] = [{
                "name": m.get("name") or m.get("model", "?"),
                "size": m.get("size", 0),
                "vram": m.get("size_vram", 0),
                "expires": seconds_until(m.get("expires_at")),
            } for m in ps.get("models", [])]
            if time.monotonic() - o["tags_t"] > 30:
                tags = http_json(f"{OLLAMA}/api/tags")
                o["installed"] = [{"name": m.get("name", "?"), "size": m.get("size", 0)} for m in tags.get("models", [])]
                o["tags_t"] = time.monotonic()
        except Exception:
            o["up"], o["loaded"] = False, []

    # ------------------------------------------------------------ one-second snapshot
    def sample(self, usage, iface, rx, tx):
        gpu_hw = find_hwmon("amdgpu")
        cpu_hw = find_hwmon(*CPU_CHIPS)
        sio_hw = find_hwmon(*SIO_CHIPS)
        nvme_hw = find_hwmon("nvme")
        gpu_dev = os.path.realpath(f"{gpu_hw}/device") if gpu_hw else None

        def gpu(name):
            return read(f"{gpu_dev}/{name}") if gpu_dev else None

        def gpu_int(name):
            v = gpu(name)
            return int(v) if v and v.isdigit() else None

        mem = meminfo()
        mem_used = mem["MemTotal"] - mem["MemAvailable"]
        temps = labelled(sio_hw, "temp", 1000)

        fans = {}
        if sio_hw:
            for p in glob.glob(f"{sio_hw}/fan*_input"):
                idx, rpm = re.search(r"fan(\d+)_input", p).group(1), num(p, 1, 0)
                if rpm:
                    raw = num(f"{sio_hw}/pwm{idx}", 1, 0)
                    fans[labelled_name(sio_hw, f"fan{idx}")] = (rpm, round(raw * 100 / 255) if raw is not None else None)
        top_fan = max(fans.values(), key=lambda v: v[0]) if fans else (0, None)

        st = os.statvfs("/")
        used = (st.f_blocks - st.f_bfree) * st.f_frsize
        avail_total = used + st.f_bavail * st.f_frsize

        gpu_mhz = num(f"{gpu_hw}/freq1_input", 1e6, 0) if gpu_hw else None
        gpu_w = num(f"{gpu_hw}/power1_input", 1e6) if gpu_hw else None
        if gpu_w is None and gpu_hw:
            gpu_w = num(f"{gpu_hw}/power1_average", 1e6)
        cpu_temp = cpu_package_temp(cpu_hw)
        gpu_temp = num(f"{gpu_hw}/temp1_input", 1000) if gpu_hw else None
        vrm = first_of(temps, VRM_TEMP_LABELS)
        sysT = first_of(temps, SYSTEM_TEMP_LABELS)
        nvme = num(f"{nvme_hw}/temp1_input", 1000) if nvme_hw else None
        vram_u, vram_t = gpu_int("mem_info_vram_used"), gpu_int("mem_info_vram_total")
        gtt_u, gtt_t = gpu_int("mem_info_gtt_used"), gpu_int("mem_info_gtt_total")
        mhz = cpu_max_mhz()
        load = read("/proc/loadavg").split()

        for k, v in (("cpu_temp", cpu_temp), ("gpu_temp", gpu_temp), ("vrm_temp", vrm), ("system_temp", sysT),
                     ("nvme_temp", nvme), ("power_w", gpu_w)):
            if v is not None and v > self.peaks.get(k, -1):
                self.peaks[k] = v

        kind, name = self.activity
        o = self.ollama
        main = {
            "ts": int(time.time()),
            "host": self.host,
            "uptime_s": int(float(read("/proc/uptime").split()[0])),
            "cpu": {"usage": usage.get("cpu"), "temp": cpu_temp, "mhz": mhz, "load1": float(load[0])},
            "gpu": {
                "usage": gpu_int("gpu_busy_percent"), "temp": gpu_temp, "mhz": gpu_mhz, "power_w": gpu_w,
                "vram_used": vram_u, "vram_total": vram_t, "gtt_used": gtt_u, "gtt_total": gtt_t,
            },
            "mem": {"used": mem_used, "total": mem["MemTotal"]},
            "board": {
                "system_temp": sysT, "vrm_temp": vrm, "nvme_temp": nvme,
                "fan_rpm": top_fan[0], "fan_pwm": top_fan[1],
            },
            "net": {"iface": iface, "rx_bps": rx, "tx_bps": tx},
            "disk": {"root_pct": pct(used, avail_total)},
            "peak": {k: v for k, v in self.peaks.items() if k.endswith("_temp")},
            "act": {"kind": kind, "name": clean(name)},
            "llm": {"n": len(o["loaded"]), "name": clean(o["loaded"][0]["name"]) if o["loaded"] else None,
                    "size": o["loaded"][0]["size"] if o["loaded"] else None},
        }

        dpm = parse_dpm(gpu("pp_dpm_sclk"))
        volts = labelled(gpu_hw, "in")
        extra = {
            "cpu": {
                **{f"core{i}": usage.get(f"cpu{i}") for i in range(len(usage) - 1)},
                "cores": len(usage) - 1, "load1": float(load[0]), "load5": float(load[1]), "load15": float(load[2]),
                **top_entries(self.top_cpu, "{:.0f}"),
            },
            "gpu": {
                "vddgfx_mv": volts.get("vddgfx"), "vddnb_mv": volts.get("vddnb"),
                "power_avg_w": num(f"{gpu_hw}/power1_average", 1e6) if gpu_hw else None,
                "dpm_cur": dpm["cur"], "dpm_n": len(dpm["mhz"]),
                **{f"dpm_{i}": m for i, m in enumerate(dpm["mhz"])},
            },
            "mem": {
                "used": mem_used, "total": mem["MemTotal"], "cached": mem.get("Cached"), "buffers": mem.get("Buffers"),
                "free": mem.get("MemFree"), "swap_used": mem.get("SwapTotal", 0) - mem.get("SwapFree", 0),
                "swap_total": mem.get("SwapTotal"), **top_entries(self.top_mem, "{}", mem=True),
            },
            "ai": {
                "ollama": "up" if o["up"] else "down", "loaded_n": len(o["loaded"]), "installed_n": len(o["installed"]),
                **{f"m{i}_{k}": clean(v) if k == "name" else v
                   for i, m in enumerate(o["loaded"][:2]) for k, v in m.items()},
                **{f"i{i}_{k}": clean(v) if k == "name" else v
                   for i, m in enumerate(sorted(o["installed"], key=lambda m: -m["size"])[:5]) for k, v in m.items()},
            },
        }

        values = {
            "cpu_usage": usage.get("cpu"), "gpu_usage": gpu_int("gpu_busy_percent"), "cpu_temp": cpu_temp,
            "gpu_temp": gpu_temp, "vrm_temp": vrm, "power_w": gpu_w, "cpu_mhz": mhz, "gpu_mhz": gpu_mhz,
            "ram_pct": pct(mem_used, mem["MemTotal"]), "vram_pct": pct(vram_u, vram_t), "gtt_pct": pct(gtt_u, gtt_t),
            "rx_bps": rx, "tx_bps": tx, "fan_rpm": top_fan[0],
        }
        with self.lock:
            self.main, self.extra = main, extra
            for k, v in values.items():
                self.hist[k].append(v)

    # ------------------------------------------------------------ history
    def history(self, metrics, step, n):
        lines = []
        with self.lock:
            series = {m: list(self.hist[m]) for m in metrics if m in self.hist}
        for m, vals in series.items():
            window = vals[max(0, len(vals) - step * n):]
            buckets = []
            for end in range(len(window), 0, -step):  # newest bucket first, so it is always complete
                chunk = [v for v in window[max(0, end - step):end] if v is not None]
                buckets.append(sum(chunk) / len(chunk) if chunk else None)
            buckets.reverse()
            lines.append(f"{m}=" + ",".join("" if b is None else f"{b:.1f}" for b in buckets))
        return "\n".join(lines) + "\n"

    def get(self):
        with self.lock:
            return self.main, self.extra


# ---------------------------------------------------------------- small helpers
def read_bytes(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


def first_of(values, names):
    """First of `names` that exists in the {label: value} dict, rounded; None if none do."""
    for n in names:
        if n in values:
            return round(values[n], 1)
    return None


def cpu_package_temp(hw):
    """k10temp reports Tctl as temp1; coretemp's package sensor is labelled 'Package id 0'."""
    if not hw:
        return None
    labels = labelled(hw, "temp", 1000)
    t = first_of(labels, CPU_TEMP_LABELS)
    return t if t is not None else num(f"{hw}/temp1_input", 1000)


def read_arg0(pid):
    """First command-line argument only (the executable); the rest may contain launch tokens, so it is never read."""
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            return f.read(512).split(b"\0", 1)[0].decode("utf-8", "replace")
    except OSError:
        return ""


def labelled_name(hw, key):
    return read(f"{hw}/{key}_label") or key


def parse_dpm(text):
    mhz, cur = [], None
    for line in (text or "").splitlines():
        m = re.match(r"(\d+):\s*(\d+)\s*Mhz\s*(\*)?", line, re.I)
        if m:
            mhz.append(int(m.group(2)))
            if m.group(3):
                cur = int(m.group(1))
    return {"mhz": mhz, "cur": cur}


def top_entries(items, fmt, mem=False):
    out = {}
    for i, (name, val) in enumerate(items, 1):
        out[f"top{i}_name"] = clean(name)
        out[f"top{i}_val"] = val if mem else fmt.format(val)
    return out


def http_json(url, timeout=0.5):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.load(r)


def seconds_until(iso):
    if not iso:
        return None
    try:
        t = datetime.fromisoformat(iso)
        return max(0, int((t - datetime.now(timezone.utc)).total_seconds()))
    except ValueError:
        return None


# ---------------------------------------------------------------- http
def flatten(d, prefix=""):
    """{'cpu': {'temp': 45}} -> {'cpu_temp': 45}; drops None and lists."""
    out = {}
    for k, v in d.items():
        key = re.sub(r"[^a-z0-9]+", "_", f"{prefix}{k}".lower()).strip("_")
        if isinstance(v, dict):
            out.update(flatten(v, key + "_"))
        elif v is not None and not isinstance(v, list):
            out[key] = v
    return out


def kv_lines(d):
    return "".join(f"{k}={v}\n" for k, v in flatten(d).items())


def make_handler(sampler):
    class Handler(BaseHTTPRequestHandler):
        def _send(self, code, body, ctype="text/plain"):
            data = body.encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            path, q = url.path, urllib.parse.parse_qs(url.query)
            main, extra = sampler.get()
            if path == "/health":
                return self._send(200, "ok\n")
            if not main:
                return self._send(503, "warming up\n")
            if path == "/stats":
                return self._send(200, json.dumps({**main, "x": extra}, indent=2) + "\n", "application/json")
            if path == "/stats.txt":
                return self._send(200, kv_lines(main))
            m = re.fullmatch(r"/x/(\w+)\.txt", path)
            if m and m.group(1) in extra:
                return self._send(200, kv_lines(extra[m.group(1)]))
            if path == "/hist":
                try:
                    metrics = [s for s in q.get("m", [""])[0].split(",") if s][:4]
                    step = min(max(int(q.get("step", ["1"])[0]), 1), 60)
                    n = min(max(int(q.get("n", ["150"])[0]), 1), 300)
                except ValueError:
                    return self._send(400, "bad query\n")
                return self._send(200, sampler.history(metrics, min(step, HIST_LEN // n), n))
            self._send(404, "not found\n")

        def log_message(self, fmt, *args):  # one request per second would flood the journal
            pass

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8250)
    args = ap.parse_args()

    sampler = Sampler()
    sampler.start()
    srv = ThreadingHTTPServer((args.host, args.port), make_handler(sampler))
    srv.daemon_threads = True
    print(f"bc250-stats listening on {args.host}:{args.port}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
