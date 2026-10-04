# 🖥️ BC-250 CYD Monitor

**A tiny always-on touchscreen dashboard for your Linux box, built from a $10 ESP32 "Cheap Yellow Display".**
Temperatures, clocks, power, RAM/VRAM, what game or local model is running, history graphs, an RGB status LED that goes
red before things get hot, and a screen saver (bouncing DVD logo, XP pipes…) for when the machine is off.

![ESP32](https://img.shields.io/badge/ESP32-CYD%202432S028R-informational)
![Python](https://img.shields.io/badge/server-Python%203%20stdlib-blue)
![Linux](https://img.shields.io/badge/host-Linux-lightgrey)
![License](https://img.shields.io/badge/license-MIT-green)

Built for the **AMD BC-250** (the PS5-APU mining board that became a tiny Linux gaming/LLM box), but the server only
reads standard Linux interfaces, so it works on most machines - see [Does it only work on a BC-250?](#does-it-only-work-on-a-bc-250).

```
┌─────────────────────────────────────────┐
│ ● my-pc                       up 1d 08h │
│ ┌──────────┐ ┌──────────┐ ┌──────────┐  │
│ │CPU   21% │ │GPU  100% │ │POWER     │  │
│ │ 69°C     │ │ 67°C     │ │ 124 W    │  │
│ │ 3.5 GHz  │ │ 1960 MHz │ │ fan 1458 │  │
│ │▓▓░░░░░░░ │ │▓▓▓▓▓▓▓▓▓ │ │ ~~~/\~~~ │  │
│ └──────────┘ └──────────┘ └──────────┘  │
│ ┌──────────┐ ┌──────────┐ ┌──────────┐  │
│ │RAM   58% │ │VRAM  93% │ │GAME      │  │
│ │ 8.6 GB   │ │ 476 MB   │ │ Layersof │  │
│ │ of 14.8  │ │ GTT 2.9  │ │ Fear2    │  │
│ │▓▓▓▓░░░░░ │ │▓▓▓▓▓▓▓▓▓ │ │ GPU 100% │  │
│ └──────────┘ └──────────┘ └──────────┘  │
│               ● ○ ○ ○                   │
└─────────────────────────────────────────┘
```
<!-- Add your own photos here, e.g. docs/home.jpg, docs/cpu.jpg, docs/thermals.jpg, docs/away.jpg -->

## ✨ Features

- **Home page** with six live tiles: CPU, GPU, power (with a mini graph), RAM, VRAM + GTT, and an **Activity** tile that
  says *GAME* (and which one), *LLM* (and which model, via [ollama](https://ollama.com)) or *IDLE*.
- **Detail pages** (tap a tile): CPU (per-core bars, load, top process), GPU (clocks, voltages, DPM level, fan),
  Memory (RAM / VRAM / GTT, cache, swap). Each has switchable **1 min / 10 min / 1 h graphs**.
- **Thermals page**: every sensor with a bar, the peak since the service started, fan RPM and PWM.
- **AI page**: the model loaded in ollama right now and what's installed.
- **Status LED + overheat alert**: the onboard RGB LED follows your hottest sensor; at 85 °C the whole screen
  turns red and flashes.
- **Away mode**: when the host is off or unreachable the screen runs a screen saver - **DVD logo** (with the corner-hit
  moment), **XP-style pipes**, **starfield**, **Matrix rain** - or turns the backlight off.
- **5 colour themes**, 2 number fonts, brightness / auto-dim / LED brightness, touch calibration - all in Settings and
  saved across reboots.
- Server is **one Python file, standard library only**, about 1 % of a CPU core.

## 🧩 How it works

```
 Linux host (the box you monitor)                 ESP32-2432S028R "CYD"
┌─────────────────────────────────┐   WiFi     ┌────────────────────────────────┐
│ server/bc250_stats.py           │  ───────►  │ cyd_monitor/  (Arduino, C++)   │
│  reads /sys, /proc once a second│  HTTP GET  │  polls once a second           │
│  keeps 1 h of history in RAM    │  :8250     │  draws tiles, graphs, themes   │
│  detects game / ollama model    │            │  touch UI, LED, screen savers  │
└─────────────────────────────────┘            └────────────────────────────────┘
```

The display polls `/stats.txt` (about 0.6 KB of plain `key=value` lines) every second, plus one detail endpoint and one
history endpoint only while the matching page is open. Nothing is sent from the display to the host, the service is
read-only and runs unprivileged.

## 🛒 What you need

- A Linux machine to monitor (see compatibility below) with Python 3.
- An **ESP32-2432S028R "Cheap Yellow Display"** (2.8" 320×240 touch, widely sold as "CYD").
- A 2.4 GHz WiFi network that both can reach.
- [Arduino IDE](https://www.arduino.cc/en/software) 2.x with the **esp32** board package 3.x (tested with 3.3.12) and the
  **LovyanGFX** library (tested with 1.2.32), installed from the Library Manager.

## 🚀 Quick start

### 1. The host (the machine you want to monitor)

```bash
git clone https://github.com/<you>/bc250-cyd-monitor
cd bc250-cyd-monitor/server
bash install.sh
```

`install.sh` asks for sudo, installs a systemd service, and - if ufw is active - opens port 8250 **to your LAN only**.
Check it from any other computer: `curl http://<host-ip>:8250/stats` (JSON, readable in a browser too).
Remove it again with `bash uninstall.sh`.

To try it without installing anything: `python3 server/bc250_stats.py --port 8250`.

### 2. The display

1. Copy `cyd_monitor/secrets.example.h` to `cyd_monitor/secrets.h` and put your WiFi in it (git-ignored).
2. Open `cyd_monitor/config.h` and set `STATS_HOST` to the host's IP address. Give that machine a fixed IP or a
   DHCP reservation on your router.
3. Open `cyd_monitor/cyd_monitor.ino` in the Arduino IDE.
   Board **ESP32 Dev Module**, **Tools → Upload Speed → 115200** (the default 921600 fails on this board).
4. Upload. On first boot tap the screen within 10 s to run the **touch calibration** (tap the four corner arrows).
   It is saved; Settings → *Recalibrate* redoes it, and holding **BOOT** while resetting forces it.

## 👆 Using it

| Gesture | Does |
|---|---|
| Tap a tile | Open its detail page (the Activity tile opens the GPU page during a game, otherwise AI) |
| Tap a chip / `1m` `10m` `1h` | Change what the graph shows / its time range |
| Tap the chevron, or swipe right | Back to Home |
| Swipe left / right | Home ↔ Thermals ↔ AI ↔ Settings |

### LED and overheat alert

Hottest of CPU, GPU and VRM (NVMe counts 5 °C lower, because SSDs run hotter). Levels are in `config.h`.

| Temperature | LED | Screen |
|---|---|---|
| below 60 °C | 🟢 green | normal |
| 60 – 70 °C | 🟡 yellow | normal |
| 70 – 80 °C | 🔴 red | normal |
| 80 – 85 °C | 🔴 slow blink | normal |
| 85 °C and up | 🔴 fast blink | **red theme, flashing frame, "OVERHEAT" banner** |
| host unreachable | 🔵 slow blue pulse | away mode |

A 3 °C hysteresis stops the alert from flickering around a threshold.

### Away mode

After the host has been unreachable for 12 s (`AWAY_AFTER_MS`) the dashboard is replaced by a screen saver. Pick it in
**Settings → Offline screen**: *DVD logo*, *Pipes*, *Starfield*, *Matrix*, *Cycle* (rotates every 75 s) or *Screen off*
(tap to wake for 20 s). **Preview away** shows it for 25 s without switching anything off. A tap skips to the next one,
and the dashboard comes back by itself when the host does.

## 🐧 Does it only work on a BC-250?

No. The **server** reads standard Linux interfaces and every sensor is optional: anything it cannot find is simply left
out of the data and shown as `--` on the display. It has only been tested on a BC-250 running CachyOS; the other rows
below come from reading the code and from a test against a fake sysfs tree.

| Data | Comes from | On other machines |
|---|---|---|
| CPU usage, per-core load, clock, load average, RAM, swap, disk, network, uptime, top processes | `/proc`, `/sys` | ✅ any Linux |
| CPU temperature | hwmon `k10temp` / `zenpower` / `coretemp` | ✅ AMD and Intel |
| GPU load, temp, clock, power, VRAM, GTT, voltages, DPM level | `amdgpu` driver | ✅ any AMD GPU/APU · ❌ NVIDIA / Intel GPUs show `--` |
| Fans, board / VRM temperature | hwmon Super I/O chip (`nct67xx`, `it87` families) | ⚠️ only if the driver is loaded and the labels match |
| NVMe temperature | hwmon `nvme` | ✅ |
| Game detection | Steam (`reaper`) or any Wine / Proton `.exe` | ✅ |
| Local model | ollama on `127.0.0.1:11434` | ✅ if you run it |

Sensors are found by hwmon *name*, not number, so reboots don't break them. Adding another chip or label is a one-line
change at the top of `bc250_stats.py` (`CPU_CHIPS`, `SIO_CHIPS`, `SYSTEM_TEMP_LABELS`…).

The **display** only cares about the HTTP format below, so any program that serves the same `key=value` lines can drive it.
The firmware targets the ESP32-2432S028R. Other CYD revisions may need a different panel driver in `panel.h`
(see the board notes).

## ⚙️ Configuration

| File | What |
|---|---|
| `cyd_monitor/secrets.h` | WiFi name and password |
| `cyd_monitor/config.h` | Host IP and port, screen rotation, colour order, temperature levels, away delay |
| `server/bc250_stats.py --port N` | Port (also `PORT=N bash install.sh`) |

<details>
<summary><b>📡 HTTP API</b></summary>

All endpoints are `GET`, read-only, no authentication. Keep the port on your LAN (the installer does this for ufw).

| URL | Returns |
|---|---|
| `/stats.txt` | flat `key=value` lines for the home page |
| `/stats` | the same plus the detail sections, as JSON |
| `/x/cpu.txt` `/x/gpu.txt` `/x/mem.txt` `/x/ai.txt` | extra detail for one page (per-core load, voltages, memory breakdown, ollama models) |
| `/hist?m=gpu_temp,cpu_usage&step=4&n=150` | history, one line per metric: the last `n × step` seconds averaged into `n` values (1 hour is kept) |
| `/health` | `ok` |

Example `/stats.txt`:

```
host=my-pc
uptime_s=113717
cpu_usage=32.3
cpu_temp=66.2
cpu_mhz=3543
gpu_usage=81
gpu_temp=65.0
gpu_power_w=137.4
gpu_vram_used=471093248
mem_used=10605617152
board_fan_rpm=1418
peak_gpu_temp=67.0
act_kind=game
act_name=LayersofFear2
```

History metrics: `cpu_usage gpu_usage cpu_temp gpu_temp vrm_temp power_w cpu_mhz gpu_mhz ram_pct vram_pct gtt_pct rx_bps tx_bps fan_rpm`.
</details>

## 🔧 Board notes and troubleshooting

- **Upload fails after "Changing baud rate"** → Tools → Upload Speed → 115200.
- **Screen stays black** → this CYD's display answers the ID query with `81 81 B3`, which LovyanGFX's autodetect does not
  recognise, so it never initialises the panel. That is why the panel is configured by hand in `panel.h` (ST7789 driver).
  Don't switch to `LGFX_AUTODETECT`. If you have a different CYD revision, an ILI9341 panel class is the first thing to try.
- **Red and blue swapped** → flip `TFT_RGB_ORDER` in `config.h`. **Colours look like a photo negative** → `TFT_INVERT`.
- **Picture upside down** → `SCREEN_ROTATION` 1 ↔ 3.
- **Touch is off or inverted** → Settings → Recalibrate (or hold BOOT while resetting).
- **"Waiting for the BC250" forever** → wrong `STATS_HOST`, a firewall, or the display and host are on different
  networks (guest WiFi, VLAN, AP isolation). The third line of that screen shows the exact error.
- **Serial Monitor** (115200) prints a status line every 10 s. Debug keys: `0`-`6` jump to a page, `t` cycles themes,
  `a` previews away mode, `n` the next animation, `o` pretends the host is offline.
- Arduino defines `F()` and `TX` itself, so don't name your own variables that.

## 📁 Project layout

```
server/        bc250_stats.py, systemd unit, install.sh / uninstall.sh
cyd_monitor/   the Arduino sketch
  cyd_monitor.ino   setup, main loop, touch gestures, LED, calibration
  config.h          the things you may want to edit
  panel.h           display + touch hardware setup
  theme.h           colours, themes, fonts, flicker-free drawing helpers
  data.h            networking task (runs on the second core) and data parsing
  ui.h              pages: home, details, thermals, AI, settings
  away.h            the screen savers
```

## 💡 Ideas not built yet

A Server page (docker containers / systemd services), Wake-on-LAN from the display, live FPS (MangoHud only writes its
numbers to a file when you stop recording, so it would need a Vulkan layer or gamescope).

## 🙏 Credits and license

Display and touch driver: [LovyanGFX](https://github.com/lovyan03/LovyanGFX), which also provides the bundled fonts.
Board docs that helped: the [amd-bc250-docs](https://elektricm.github.io/amd-bc250-docs/) and the
[BC-250 Control Center](https://github.com/movacx/bc250-control-center).

MIT licensed, see [LICENSE](LICENSE).
