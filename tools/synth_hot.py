#!/usr/bin/env python3
"""[I] Hot code per synth module (#21 shared SRAM_L pool).

usage: synth_hot.py <elf> <hot.txt> [--modules-dir DIR] [--fastrun-elf ELF] [--detail ID]
                    [--emit-hot DIR --share 0.95]

Folds tests/bench_m4/hot_plugin.c output (make -C tests bench-m4-synth-hot
PROFILE=allsynths) into functions, gives each function to the module whose
directory holds its source (the outermost frame of addr2line -i; code from
core headers counts as "core"), and prints per module: executed
instructions, code size, the bytes that hold 90 / 95 / 99 % of its
instructions (hottest first), and how much a greedy pick by instructions per
byte covers at 2 / 4 / 6 / 8 KB. With --fastrun-elf (a device image), it also
shows how much of each module the image's .fastrun functions cover.
--emit-hot writes DIR/<module>.hot per module: its hottest functions until
they hold --share of its instructions, for tools/make_otm.py --hot."""
import argparse
import bisect
import collections
import os
import subprocess
import sys

TOOLS = "arm-none-eabi-"


def functions(elf):
    out = subprocess.run([TOOLS + "nm", "-S", "--defined-only", elf], capture_output=True, text=True,
                         check=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split(None, 3)
        if len(p) == 4 and p[2] in "tTwW":
            a = int(p[0], 16) & ~1
            if int(p[1], 16) > syms.get(a, (0, ""))[0]:
                syms[a] = (int(p[1], 16), p[3])
    return sorted((a, s, n) for a, (s, n) in syms.items())


def owners(elf, addrs, modules_dir):
    """Source file of each function start: the outermost inline frame."""
    # addr2line -a prints each address before its frames, so the output splits per address.
    res = {}
    for i in range(0, len(addrs), 400):
        chunk = addrs[i:i + 400]
        lines = subprocess.run([TOOLS + "addr2line", "-i", "-a", "-e", elf] + ["0x%x" % a for a in chunk],
                               capture_output=True, text=True, check=True).stdout.splitlines()
        cur = None
        for ln in lines:
            if ln.startswith("0x") and ":" not in ln:
                cur = int(ln, 16)
                res[cur] = "?"
            elif cur is not None:
                res[cur] = ln.rsplit(":", 1)[0]   # last frame wins = outermost
    mods = {}
    md = os.path.abspath(modules_dir) + os.sep
    for a, f in res.items():
        f = os.path.abspath(f) if f not in ("?", "??") else "?"
        if f.startswith(md):
            rest = f[len(md):].split(os.sep)
            mods[a] = rest[1] if len(rest) > 2 and rest[0] == "modules" else "?"
            if len(rest) > 2 and rest[2] == "bench":
                mods[a] = "bench"
        elif "/audio/engines/dx7" in f:
            mods[a] = "dx7"   # core engine for now (the DX7 module plan moves it out)
        elif "/tests/" in f:
            mods[a] = "bench"
        else:
            mods[a] = "core"
    return mods


def fastrun_names(elf):
    out = subprocess.run([TOOLS + "nm", "-S", "--defined-only", elf], capture_output=True, text=True,
                         check=True).stdout
    lo = hi = None
    rows = []
    for line in out.splitlines():
        p = line.split()
        if p[-1] == "__fastrun_start":
            lo = int(p[0], 16)
        elif p[-1] == "__fastrun_end":
            hi = int(p[0], 16)
        if len(p) == 4 and p[2] in "tTwW":
            rows.append((int(p[0], 16) & ~1, p[3]))
    return {n for a, n in rows if lo is not None and lo <= a < hi}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("hot")
    ap.add_argument("--modules-dir", default=os.path.join(os.path.dirname(__file__), "../../opentracker-synths"))
    ap.add_argument("--fastrun-elf")
    ap.add_argument("--detail", action="append", default=[])
    ap.add_argument("--emit-hot")
    ap.add_argument("--share", type=float, default=0.95)
    a = ap.parse_args()

    syms = functions(a.elf)
    starts = [s[0] for s in syms]
    per = collections.Counter()
    for line in open(a.hot):
        st, _end, cnt, n = line.split()
        addr = int(st, 16) & ~1
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and addr < syms[i][0] + max(syms[i][1], 2):
            per[i] += int(cnt) * int(n)
    hot = [i for i in per if per[i]]
    own = owners(a.elf, [syms[i][0] for i in hot], a.modules_dir)
    fr = fastrun_names(a.fastrun_elf) if a.fastrun_elf else set()

    by = collections.defaultdict(list)   # module -> [(insns, size, name)]
    for i in hot:
        by[own.get(syms[i][0], "?")].append((per[i], syms[i][1], syms[i][2]))

    def cover(rows, share):
        tot = sum(r[0] for r in rows)
        got = b = 0
        for ins, size, _ in sorted(rows, key=lambda r: -r[0]):
            if got >= share * tot:
                break
            got += ins
            b += size
        return b

    def greedy(rows, budget):
        tot = sum(r[0] for r in rows) or 1
        used = got = 0
        for ins, size, _ in sorted(rows, key=lambda r: -r[0] / max(r[1], 1)):
            if size and used + size <= budget:
                used += size
                got += ins
        return 100.0 * got / tot

    print(f"{'module':8} {'insns':>11} {'hot B':>7} {'90%':>6} {'95%':>6} {'99%':>6}"
          f" {'2K':>5} {'4K':>5} {'6K':>5} {'8K':>5}" + ("  .fastrun now" if fr else ""))
    order = sorted(by, key=lambda m: (m in ("core", "bench", "?"), m))
    for m in order:
        rows = by[m]
        tot = sum(r[0] for r in rows)
        size = sum(r[1] for r in rows)
        line = (f"{m:8} {tot:11d} {size:7d} {cover(rows, .90):6d} {cover(rows, .95):6d} {cover(rows, .99):6d}"
                f" {greedy(rows, 2048):4.0f}% {greedy(rows, 4096):4.0f}% {greedy(rows, 6144):4.0f}%"
                f" {greedy(rows, 8192):4.0f}%")
        if fr:
            frb = sum(r[1] for r in rows if r[2] in fr)
            fri = sum(r[0] for r in rows if r[2] in fr)
            if frb:
                line += f"  {frb:5d} B {100.0 * fri / max(tot, 1):4.0f}%"
        print(line)
    if a.emit_hot:
        os.makedirs(a.emit_hot, exist_ok=True)
        for m in order:
            if m in ("core", "bench", "?"):
                continue
            rows = sorted(by[m], key=lambda r: -r[0])
            tot = sum(r[0] for r in rows) or 1
            got = size = 0
            lines = [f"# {m}: hottest functions until {100 * a.share:.0f}% of its executed instructions "
                     f"(tools/synth_hot.py, make -C tests bench-m4-synth-hot)"]
            for ins, sz, name in rows:
                if got >= a.share * tot:
                    break
                got += ins
                size += sz
                lines.append(f"{name}  # {100.0 * ins / tot:.1f}% {sz} B")
            lines.append(f"# {size} B, {100.0 * got / tot:.1f}%")
            with open(os.path.join(a.emit_hot, m + ".hot"), "w") as f:
                f.write("\n".join(lines) + "\n")
    for m in a.detail:
        rows = sorted(by.get(m, []), key=lambda r: -r[0])
        tot = sum(r[0] for r in rows) or 1
        print(f"\n{m}: hottest functions")
        out = subprocess.run([TOOLS + "c++filt"], input="\n".join(r[2] for r in rows[:30]), capture_output=True,
                             text=True).stdout.splitlines()
        for (ins, size, name), dn in zip(rows[:30], out):
            mark = " *" if name in fr else ""
            print(f"  {100.0 * ins / tot:5.1f}% {size:6d} B  {dn[:100]}{mark}")


if __name__ == "__main__":
    sys.exit(main())
