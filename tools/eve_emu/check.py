#!/usr/bin/env python3
"""RAM_DL check on the FT81x emulator: replays frames saved by
opentracker-sim --frames on Bridgetek's EVE emulator and fails when a frame
faults the coprocessor, builds more than 2048 RAM_DL words, or differs from
the UI's own count by more than --tol words.

  tools/eve_emu/check.py FRAMES_DIR [FRAMES_DIR ...] [--tol W] [--cache DIR]

Each FRAMES_DIR holds NAME.otf files and index.txt ("NAME WORDS": the UI's
RAM_DL count of that frame without the driver's font setup). `make -C sim
eve-check` records them from the display budget script and runs this.

The emulator (bt8xxemu.dll, Windows x64, Bridgetek's licence: free for EVE
development, not redistributable on its own) is downloaded at a pinned
commit and checked by SHA-256, never committed. It runs under Wine; the
replay program is cross-compiled with MinGW (x86_64-w64-mingw32-g++).
"""
import argparse
import hashlib
import os
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# github.com/Bridgetek/EVE-Emulator, "Release emulator v5.3.35".
EMU_COMMIT = "516f4d511b205f9cc620814d8e7387b975599d3c"
EMU_FILES = {
    "bin/bt8xxemu.dll": "faaa77891a3417a40263874397eb22bf33377db53cd2efe1c3f0a8039498faab",
    "bin/zlib.dll": "999eb98a1916a098c6c88f726a12b1702fea88bdaf04d3c0f281ec2d2630ff6d",
    "include/bt8xxemu.h": "67842625a5234860734f4df760df1bd736123307e3ddb4153483a916fc597616",
    "include/bt8xxemu_inttypes.h": "a771f1e1a614c2d297f3f963a4ef575b41cd1589ca147ce134daed504b0dfb6e",
}
DRIVER_WORDS = 36   # Ui::kDriverDlWords: the font setup after CMD_DLSTART (custom fonts only)
RAM_DL_WORDS = 2048


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def fetch(cache):
    os.makedirs(cache, exist_ok=True)
    for rel, want in EMU_FILES.items():
        dst = os.path.join(cache, os.path.basename(rel))
        if os.path.exists(dst) and sha256(dst) == want:
            continue
        url = "https://raw.githubusercontent.com/Bridgetek/EVE-Emulator/%s/%s" % (EMU_COMMIT, rel)
        print("downloading %s" % url)
        with urllib.request.urlopen(url, timeout=60) as r, open(dst + ".part", "wb") as f:
            f.write(r.read())
        got = sha256(dst + ".part")
        if got != want:
            os.remove(dst + ".part")
            sys.exit("checksum mismatch for %s: %s" % (rel, got))
        os.replace(dst + ".part", dst)


def newer(target, *sources):
    return os.path.exists(target) and all(os.path.getmtime(target) >= os.path.getmtime(s) for s in sources)


def build(cache):
    otf2cmd = os.path.join(cache, "otf2cmd")
    fonts = [os.path.join(ROOT, "src/core/ui/fonts/fonts.cpp")]
    src = os.path.join(HERE, "otf2cmd.cpp")
    if not newer(otf2cmd, src, *fonts):
        subprocess.check_call(["g++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-I" + os.path.join(ROOT, "src/core"),
                               "-I" + os.path.join(ROOT, "src"), "-o", otf2cmd, src] + fonts)
    replay = os.path.join(cache, "replay.exe")
    src = os.path.join(HERE, "replay.cpp")
    if not newer(replay, src):
        subprocess.check_call(["x86_64-w64-mingw32-g++", "-O1", "-std=c++17", "-Wall", "-I" + cache, "-o", replay, src,
                               os.path.join(cache, "bt8xxemu.dll"), "-static"])
    return otf2cmd, replay


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--tol", type=int, default=8, help="allowed |emulator - (UI count + driver setup)| words (8)")
    ap.add_argument("--cache", default=os.path.join(ROOT, "build/eve_emu"), help="emulator + tools (build/eve_emu)")
    a = ap.parse_args()
    cache = os.path.abspath(a.cache)
    fetch(cache)
    otf2cmd, replay = build(cache)

    failed = 0
    rows = []
    for d in a.dirs:
        d = os.path.abspath(d)
        index = {}
        with open(os.path.join(d, "index.txt")) as f:
            for line in f:
                name, words = line.split()
                index[name] = int(words)   # a repeated name: the last frame
        otfs = [os.path.join(d, n + ".otf") for n in sorted(index)]
        # NAME.text.otf: the same screen with its text as CMD_TEXT, the pixel
        # reference for the glyph runs.
        refs = {n for n in index if os.path.exists(os.path.join(d, n + ".text.otf"))}
        otfs += [os.path.join(d, n + ".text.otf") for n in sorted(refs)]
        cmd_dir = os.path.join(d, "cmd")
        os.makedirs(cmd_dir, exist_ok=True)
        conv = subprocess.run([otf2cmd, cmd_dir, os.path.join(cmd_dir, "fonts.bin")] + otfs,
                              capture_output=True, text=True)
        sys.stderr.write(conv.stderr)
        if conv.returncode:
            failed += 1
        sets = {}
        for line in conv.stdout.splitlines():
            name, font_set, _ = line.split()
            sets[name] = int(font_set)   # NAME.text: the reference
        with open(os.path.join(cmd_dir, "list.txt"), "w") as f:
            for name in sorted(index):
                f.write("%s %s.cmd%s\n" % (name, name, " %s.text.cmd" % name if name in refs else ""))
        # No Mono / Gecko install prompts (a headless runner would wait on them).
        env = dict(os.environ, WINEDEBUG="-all", WINEDLLOVERRIDES="mscoree,mshtml=")
        # The DLLs next to the program: Wine looks there first.
        for dll in ("bt8xxemu.dll", "zlib.dll"):
            dst = os.path.join(cmd_dir, dll)
            if not os.path.exists(dst):
                os.symlink(os.path.join(cache, dll), dst)
        run = subprocess.run(["wine", replay, "fonts.bin", "list.txt"], cwd=cmd_dir, env=env,
                             capture_output=True, text=True, timeout=1200)
        got = {}
        for line in run.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 4 and parts[0] in index:
                # NAME SENT DL STATUS [same | DIFF N X0 Y0 X1 Y1 | NOSHOT STATUS]
                got[parts[0]] = (int(parts[2]) // 4, parts[3], " ".join(parts[4:]))
        if run.returncode or len(got) != len(index):
            sys.stderr.write(run.stderr[-2000:])
            print("%s: replay failed (exit %d, %d of %d frames)" % (d, run.returncode, len(got), len(index)))
            failed += 1
        for name in sorted(index):
            if name not in got:
                continue
            hw, status, pixels = got[name]
            est = index[name] + (DRIVER_WORDS if sets.get(name, -1) >= 0 else 0)
            ok = (status == "ok" and hw <= RAM_DL_WORDS and abs(hw - est) <= a.tol
                  and (name not in refs or pixels == "same"))
            failed += not ok
            rows.append((os.path.basename(d), name, est, hw, status + (" pixels " + pixels if pixels else ""), ok))

    print("%-14s %-22s %6s %6s %6s  %s" % ("dir", "frame", "UI", "emu", "diff", "status"))
    for d, name, est, hw, status, ok in rows:
        print("%-14s %-22s %6d %6d %+6d  %s%s" % (d[:14], name[:22], est, hw, hw - est, status, "" if ok else "  <-- FAIL"))
    worst = max((abs(r[3] - r[2]) for r in rows), default=0)
    peak = max((r[3] for r in rows), default=0)
    print("%d frames, largest %d of %d words, worst difference %d words (allowed %d)" %
          (len(rows), peak, RAM_DL_WORDS, worst, a.tol))
    compared = [r for r in rows if "pixels" in r[4]]
    if compared:
        print("%d frames compared with their CMD_TEXT form: %d identical" %
              (len(compared), sum("pixels same" in r[4] for r in compared)))
    print("%d failure(s)" % failed if failed else "all frames fit and match")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
