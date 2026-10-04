#!/usr/bin/env python3
"""Take a screenshot of the CYD: asks the firmware for its screen over USB and writes a PNG.

    python3 tools/screenshot.py                       # auto-detects the port, saves screenshot.png
    python3 tools/screenshot.py -p /dev/cu.usbserial-130 -o home.png --scale 2

Close the Arduino IDE's Serial Monitor first (only one program can use the port). Takes about 15 seconds.
No third-party packages needed (macOS / Linux).
"""
import argparse, glob, os, select, struct, sys, termios, time, tty, zlib


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/ttyUSB*") + glob.glob("/dev/cu.wchusbserial*"))
    if not ports:
        sys.exit("No USB serial port found. Plug the CYD in or pass -p /dev/...")
    return ports[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port")
    ap.add_argument("-o", "--out", default="screenshot.png")
    ap.add_argument("--scale", type=int, default=1, help="enlarge the picture, e.g. 2 for 640x480")
    ap.add_argument("--send", default="", help="keys to send first, e.g. 4 to open the CPU page (see the debug keys in the README)")
    ap.add_argument("--wait", type=float, default=2.5, help="seconds to wait after --send before capturing")
    ap.add_argument("--swap-rb", action="store_true", help="swap red and blue if the colours come out wrong")
    args = ap.parse_args()

    fd = os.open(args.port or find_port(), os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    a = termios.tcgetattr(fd); a[4] = a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)

    def read_until(cond, timeout):
        buf, end = b"", time.time() + timeout
        while time.time() < end:
            if select.select([fd], [], [], 0.2)[0]:
                try: buf += os.read(fd, 65536)
                except BlockingIOError: continue
                if cond(buf): return buf
        sys.exit("Timed out. Is the Serial Monitor closed, and is the firmware new enough (it needs the 's' command)?")

    time.sleep(0.3)
    if args.send:
        os.write(fd, args.send.encode())
        time.sleep(args.wait)

    END = b"\nEND\n"
    for attempt in range(1, 5):                      # a stray byte on the line shifts the picture: detect and retry
        os.write(fd, b"s")
        buf = read_until(lambda b: b"SHOT " in b and b"\n" in b[b.index(b"SHOT "):], 10)
        i = buf.index(b"SHOT ")
        hdr_end = buf.index(b"\n", i)
        w, h = map(int, buf[i + 5:hdr_end].split())
        need = w * h * 2
        data = buf[hdr_end + 1:]
        print(f"receiving {w}x{h} (attempt {attempt}) ...", flush=True)
        while END not in data:
            data += read_until(lambda b: True, 20)
        payload = data[:data.index(END)]
        if len(payload) == need:
            data = payload
            break
        print(f"  corrupted transfer ({len(payload)} of {need} bytes), retrying", flush=True)
        time.sleep(1)
    else:
        sys.exit("Could not get a clean screenshot after 4 attempts.")

    rgb = bytearray(w * h * 3)
    for p in range(w * h):
        v = data[2 * p] | (data[2 * p + 1] << 8)
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        r, g, b = (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)
        if args.swap_rb: r, b = b, r
        rgb[3 * p:3 * p + 3] = bytes((r, g, b))
    if args.scale > 1:
        s = args.scale
        big = bytearray()
        for y in range(h):
            row = b"".join(bytes(rgb[3 * (y * w + x):3 * (y * w + x) + 3]) * s for x in range(w))
            big += row * s
        write_png(args.out, w * s, h * s, bytes(big))
    else:
        write_png(args.out, w, h, bytes(rgb))
    print("saved", args.out)


if __name__ == "__main__":
    main()
