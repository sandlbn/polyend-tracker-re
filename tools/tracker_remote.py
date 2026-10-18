#!/usr/bin/env python3
"""Drive a Tracker running OpenTracker (or `opentracker-sim --serve`) over its
USB serial port: remote control drive mode (src/core/remote/remote.h).

On the Tracker, set Config > USB > Remote control to On first.

  tools/tracker_remote.py PORT SCRIPT [SCRIPT ...] [--out DIR] [--log FILE]
  tools/tracker_remote.py PORT -c "tap PERFORMANCE; wait 500; screenshot perf.png"

Script lines (the simulator's headless language, sim/README.md, plus checks;
'#' starts a comment, ';' separates commands on one line):

  wait MS                       let MS milliseconds pass
  press NAME / release NAME     button down / up (names: src/core/ui/buttons.h,
  tap NAME [MS]                 any case, e.g. PLAY, F1, SampleRecorder, or a
  hold NAME [MS]                number); tap = down, MS (60), up, 60 ms
  pad N down|up|tap [VEL]       pad 0..47
  enc DELTA                     encoder detents
  screenshot FILE.png           the shown frame (relative to --out)
  status                        print the status line
  perf                          print the performance fields
  expect KEY OP VALUE           status field check, OP one of == != < <= > >= ~ (contains)
  expect-screen NAME            screen name (spaces allowed)
  expect-playing / expect-stopped
  expect-sound N                output peak since the last status query >= N
  sound-reset                   start a new peak window
  expect-notice TEXT            the on-screen notice contains TEXT
  echo TEXT

Everything the Tracker sends (including the periodic telemetry lines) is kept
in --log, ready for `tools/telemetry_check.py log FILE` or
`make perf-gate LOG=FILE`. Exit status: 0 all checks passed, 1 a check
failed, 2 connection / usage error.
"""
import argparse
import base64
import os
import re
import select
import struct
import sys
import termios
import threading
import time
import tty
import zlib

KEEPALIVE_S = 0.5
HERE = os.path.dirname(os.path.abspath(__file__))
BUTTONS_H = os.path.join(HERE, "..", "src", "core", "ui", "buttons.h")


def load_buttons(path=BUTTONS_H):
    names = {}
    with open(path) as f:
        text = f.read()
    for decl in re.findall(r"constexpr\s+uint8_t\s+([^;]+);", text):
        for part in decl.split(","):
            m = re.match(r"\s*(\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*$", part)
            if m:
                names[m.group(1).lower()] = int(m.group(2), 0)
    return names


class Fail(Exception):
    pass


class Link:
    """Raw serial link; a reader thread splits lines, a keepalive thread holds drive mode."""

    def __init__(self, port, log_path=None):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
        try:
            tty.setraw(self.fd)
        except termios.error:
            pass
        self.lock = threading.Lock()
        self.cond = threading.Condition()
        self.lines = []
        self.log = open(log_path, "w") if log_path else None
        self.running = True
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.keep = threading.Thread(target=self._keepalive, daemon=True)

    def _read(self):
        buf = b""
        while self.running:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if not r:
                continue
            try:
                data = os.read(self.fd, 4096)
            except OSError:
                time.sleep(0.05)
                continue
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.rstrip(b"\r").decode("utf-8", "replace")
                if self.log:
                    self.log.write(line + "\n")
                    self.log.flush()
                with self.cond:
                    self.lines.append(line)
                    self.cond.notify_all()

    def _keepalive(self):
        while self.running:
            self.send("@RC DRIVE")
            time.sleep(KEEPALIVE_S)

    def send(self, line):
        with self.lock:
            os.write(self.fd, (line + "\n").encode())

    def wait_for(self, pred, timeout, start=0):
        """First line (index >= start) whose payload after '@RC ' satisfies pred."""
        end = time.time() + timeout
        i = start
        with self.cond:
            while True:
                while i < len(self.lines):
                    line = self.lines[i]
                    i += 1
                    k = line.find("@RC ")
                    if k >= 0 and pred(line[k + 4:]):
                        return line[k + 4:], i
                left = end - time.time()
                if left <= 0:
                    return None, i
                self.cond.wait(left)

    def mark(self):
        with self.cond:
            return len(self.lines)

    def connect(self, timeout=5.0):
        start = self.mark()
        self.keep.start()
        got, _ = self.wait_for(lambda p: p == "DRIVING", timeout, start)
        if got is None:
            raise Fail("no answer: is Config > USB > Remote control On, and nothing else using the port?")

    def close(self):
        self.running = False
        try:
            self.send("@RC BYE")
        except OSError:
            pass
        time.sleep(0.1)
        os.close(self.fd)
        if self.log:
            self.log.close()


def parse_status(payload):
    fields = {}
    for tok in payload.split()[1:]:
        if "=" in tok:
            k, v = tok.split("=", 1)
            fields[k] = v.replace("_", " ")
    return fields


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def decode_shot(w, h, data):
    out = bytearray()
    i = 0
    for _ in range(h):
        x = 0
        while x < w:
            if i + 4 > len(data):
                raise Fail("screenshot data too short")
            n = data[i] + 1
            px = bytes(data[i + 1:i + 4])
            if x + n > w:
                raise Fail("screenshot run crosses a row")
            out += px * n
            x += n
            i += 4
    if i != len(data):
        raise Fail("screenshot has extra data")
    return bytes(out)


class Runner:
    def __init__(self, link, out_dir, buttons):
        self.link = link
        self.out = out_dir
        self.buttons = buttons
        self.failures = 0
        self.last = {}

    def button(self, name):
        try:
            return int(name, 0)
        except ValueError:
            pass
        b = self.buttons.get(name.lower())
        if b is None:
            raise Fail("unknown button %r" % name)
        if name.lower() == "power":
            raise Fail("the power switch is not remote-controlled")
        return b

    def status(self):
        start = self.link.mark()
        self.link.send("@RC Q")
        got, _ = self.link.wait_for(lambda p: p.startswith("S "), 10.0, start)
        if got is None:
            raise Fail("no status answer")
        self.last = parse_status(got)
        return self.last

    def check(self, ok, what):
        if ok:
            print("ok    " + what)
        else:
            self.failures += 1
            print("FAIL  " + what)

    def screenshot(self, path):
        start = self.link.mark()
        self.link.send("@RC SHOT")
        head, i = self.link.wait_for(lambda p: p.startswith("SHOT"), 5.0, start)
        if head is None or head == "SHOT FAIL":
            raise Fail("screenshot not available")
        _, w, h = head.split()
        w, h = int(w), int(h)
        data = bytearray()
        while True:
            got, i = self.link.wait_for(lambda p: p.startswith("D ") or p.startswith("SHOT "), 30.0, i)
            if got is None:
                raise Fail("screenshot timed out")
            if got.startswith("D "):
                data += base64.b64decode(got[2:])
                continue
            if got == "SHOT FAIL":
                raise Fail("screenshot failed on the device")
            parts = got.split()   # SHOT END <bytes> <crc>
            if parts[1] != "END":
                continue
            if int(parts[2]) != len(data) or int(parts[3], 16) != (zlib.crc32(bytes(data)) & 0xFFFFFFFF):
                raise Fail("screenshot transfer damaged (%d bytes, expected %s)" % (len(data), parts[2]))
            break
        rgb = decode_shot(w, h, data)
        full = os.path.join(self.out, path)
        os.makedirs(os.path.dirname(full) or ".", exist_ok=True)
        write_png(full, w, h, rgb)
        print("saved " + full)

    def run(self, cmd):
        words = cmd.split()
        if not words:
            return
        op, args = words[0].lower(), words[1:]
        L = self.link
        if op == "wait":
            time.sleep(int(args[0]) / 1000.0)
        elif op in ("press", "release"):
            L.send("@RC B%d %d" % (1 if op == "press" else 0, self.button(args[0])))
        elif op in ("tap", "hold"):
            b = self.button(args[0])
            ms = int(args[1]) if len(args) > 1 else (60 if op == "tap" else 600)
            L.send("@RC B1 %d" % b)
            time.sleep(ms / 1000.0)
            L.send("@RC B0 %d" % b)
            time.sleep(0.06 if op == "tap" else 0.0)
        elif op == "pad":
            p, what = int(args[0]), args[1].lower()
            vel = int(args[2]) if len(args) > 2 else 100
            if what in ("down", "tap"):
                L.send("@RC P1 %d %d" % (p, vel))
            if what == "tap":
                time.sleep(0.06)
            if what in ("up", "tap"):
                L.send("@RC P0 %d" % p)
            if what == "tap":
                time.sleep(0.06)
        elif op == "enc":
            d = int(args[0])
            while d:
                step = max(-127, min(127, d))
                L.send("@RC E %d" % step)
                d -= step
        elif op == "screenshot":
            self.screenshot(args[0])
        elif op == "status":
            s = self.status()
            print("status " + " ".join("%s=%s" % kv for kv in s.items()))
        elif op == "perf":
            s = self.status()
            print("perf   cpu avg %s / peak %s permille, overruns %s, synth voices %s/%s (cuts %s), free %s KiB"
                  % (s.get("cpuA"), s.get("cpuP"), s.get("ovr"), s.get("gov"), s.get("govB"), s.get("cuts"),
                     s.get("freeKB")))
        elif op == "expect":
            key, rel, want = args[0], args[1], " ".join(args[2:])
            have = self.status().get(key)
            ok = False
            if have is not None:
                if rel == "~":
                    ok = want.lower() in have.lower()
                elif rel in ("==", "!="):
                    ok = (have == want) == (rel == "==")
                else:
                    a, b = float(have), float(want)
                    ok = {"<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b}[rel]
            self.check(ok, "%s %s %s (is %s)" % (key, rel, want, have))
        elif op == "expect-screen":
            want = " ".join(args)
            have = self.status().get("scr")
            self.check(have == want, "screen %s (is %s)" % (want, have))
        elif op in ("expect-playing", "expect-stopped"):
            have = self.status().get("play")
            self.check(have == ("1" if op == "expect-playing" else "0"), "%s (play=%s)" % (op[7:], have))
        elif op == "expect-sound":
            have = int(self.status().get("peak", "0"))
            self.check(have >= int(args[0]), "sound >= %s (peak %d)" % (args[0], have))
        elif op == "sound-reset":
            self.status()
        elif op == "expect-notice":
            want = " ".join(args)
            have = self.status().get("note", "")
            self.check(want.lower() in have.lower(), "notice ~ %r (is %r)" % (want, have))
        elif op == "echo":
            print(" ".join(args))
        else:
            raise Fail("unknown command %r" % op)

    def run_text(self, text):
        for raw in text.splitlines():
            line = raw.split("#", 1)[0]
            for cmd in line.split(";"):
                cmd = cmd.strip()
                if cmd:
                    self.run(cmd)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("scripts", nargs="*")
    ap.add_argument("-c", dest="inline", help="commands, ';'-separated")
    ap.add_argument("--out", default=".", help="folder for screenshots")
    ap.add_argument("--log", help="keep every received line (telemetry included) here")
    a = ap.parse_args()
    if not a.scripts and not a.inline:
        ap.error("give a script or -c")
    try:
        link = Link(a.port, a.log)
    except OSError as e:
        print("cannot open %s: %s" % (a.port, e), file=sys.stderr)
        return 2
    runner = Runner(link, a.out, load_buttons())
    try:
        link.connect()
        for path in a.scripts:
            with open(path) as f:
                runner.run_text(f.read())
        if a.inline:
            runner.run_text(a.inline)
    except Fail as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    finally:
        link.close()
    print("%d check(s) failed" % runner.failures if runner.failures else "all checks passed")
    return 1 if runner.failures else 0


if __name__ == "__main__":
    sys.exit(main())
