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
  frame FILE.otf                the display commands of a fresh frame (relative to --out);
                                opentracker-sim --frame FILE.otf --screenshot OUT.png draws it
  text                          print the text on screen (from a fresh frame)
  expect-text TEXT              some text on screen contains TEXT (any case)
  expect-no-text TEXT           no text on screen contains TEXT
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


# Display command export (src/core/remote/display_tap.h): little-endian words.
CMD_TEXT, CMD_NUMBER, CMD_GRADIENT = 0xFFFFFF0C, 0xFFFFFF2E, 0xFFFFFF0B
TAG_FONTSET, TAG_IMAGE, TAG_RGB332, TAG_FRAGMENT = 0xFFFFFFE0, 0xFFFFFFE2, 0xFFFFFFE3, 0xFFFFFFE4
HINT_GLYPHS, HINT_STRING = 0xFE, 0xFD   # top byte; glyph runs drawn as VERTEX2II
FIXED_ARGS = {0xFFFFFF00: 0, 0xFFFFFF01: 0, CMD_NUMBER: 3, CMD_GRADIENT: 4, TAG_FONTSET: 1, TAG_IMAGE: 3,
              TAG_RGB332: 4, TAG_FRAGMENT: 1}


def frame_texts(data):
    """[(x, y, font, text)] of a frame: CMD_TEXT, CMD_NUMBER and glyph runs (hints)."""
    n = len(data) // 4
    words = struct.unpack("<%dI" % n, data[:n * 4])
    out = []
    i = 0

    def s16(v):
        return v - 0x10000 if v & 0x8000 else v

    def string(k):
        b = bytearray()
        while k < n:
            w = words[k]
            k += 1
            for sh in (0, 8, 16, 24):
                c = (w >> sh) & 255
                if c == 0:
                    return b.decode("latin-1"), k
                b.append(c)
        return b.decode("latin-1"), k

    def sext(v, bits):
        v &= (1 << bits) - 1
        return v - (1 << bits) if v & (1 << (bits - 1)) else v

    translate = 0
    while i < n:
        w = words[i]
        i += 1
        top = w >> 24
        if top == 0x2B:   # VERTEX_TRANSLATE_X
            translate = sext(w, 17)
            continue
        if top in (HINT_GLYPHS, HINT_STRING):
            chars = ""
            if top == HINT_STRING:
                length = (w >> 8) & 255
                raw = b"".join(struct.pack("<I", v) for v in words[i:i + (length + 3) // 4])
                chars = raw[:length].decode("latin-1")
                i += (length + 3) // 4
            # Position: the run's first glyph; HINT_GLYPHS spells the cells.
            x = y = None
            tr = translate
            for g in words[i:]:
                if g >> 24 == 0x2B:
                    tr = sext(g, 17)
                    continue
                if g >> 30 != 2:
                    break
                if x is None:
                    x, y = ((g >> 21) & 0x1FF) + tr // 16, (g >> 12) & 0x1FF
                if top != HINT_GLYPHS:
                    break
                chars += chr(g & 0x7F)
            out.append((x or 0, y or 0, (w >> 16) & 255, chars))
            continue
        if w < 0xFFFFFF00:
            continue
        if w == CMD_TEXT:
            xy, f = words[i], words[i + 1]
            t, i = string(i + 2)
            out.append((s16(xy & 0xFFFF), s16(xy >> 16), f & 255, t))
        elif w == CMD_NUMBER:
            xy, f, v = words[i], words[i + 1], words[i + 2]
            out.append((s16(xy & 0xFFFF), s16(xy >> 16), f & 255, str(v - (1 << 32) if v & 0x80000000 else v)))
            i += 3
        elif w in FIXED_ARGS:
            i += FIXED_ARGS[w]
        else:
            raise Fail("unknown command 0x%08X in the frame" % w)
    return out


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

    def frame(self):
        """A fresh frame's command stream (bytes); the device redraws for it."""
        start = self.link.mark()
        self.link.send("@RC FRAME")
        head, i = self.link.wait_for(lambda p: p.startswith("F "), 5.0, start)
        if head is None or head == "F FAIL":
            raise Fail("frame not available")
        data = bytearray()
        while True:
            got, i = self.link.wait_for(lambda p: p.startswith("D ") or p.startswith("F END"), 10.0, i)
            if got is None:
                raise Fail("frame timed out")
            if got.startswith("D "):
                data += base64.b64decode(got[2:])
                continue
            parts = got.split()   # F END <bytes> <crc>
            if int(parts[2]) != len(data) or int(parts[3], 16) != (zlib.crc32(bytes(data)) & 0xFFFFFFFF):
                raise Fail("frame transfer damaged (%d bytes, expected %s)" % (len(data), parts[2]))
            break
        if head.split()[3] == "1":
            print("note  the frame overflowed the device's buffer: cut")
        return bytes(data)

    def texts(self):
        return [t for t in frame_texts(self.frame()) if t[3].strip()]

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
        elif op == "frame":
            data = self.frame()
            full = os.path.join(self.out, args[0])
            os.makedirs(os.path.dirname(full) or ".", exist_ok=True)
            with open(full, "wb") as f:
                f.write(data)
            print("saved %s (%d bytes)" % (full, len(data)))
        elif op == "text":
            for x, y, font, t in sorted(self.texts(), key=lambda t: (t[1], t[0])):
                print("text  %4d %4d  %s" % (x, y, t))
        elif op in ("expect-text", "expect-no-text"):
            want = " ".join(args)
            hits = [t[3] for t in self.texts() if want.lower() in t[3].lower()]
            if op == "expect-text":
                self.check(bool(hits), "text ~ %r%s" % (want, " (%r)" % hits[0] if hits else ""))
            else:
                self.check(not hits, "no text ~ %r%s" % (want, " (%r)" % hits[0] if hits else ""))
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
