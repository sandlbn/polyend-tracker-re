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
they hold --share of its instructions, for tools/make_otm.py --hot.

--modes NAMES (make -C tests bench-m4-synth-modes, tests/bench_m4/synth_modes.cpp):
<hot> holds one count per phase (every mode of every synth) and NAMES the
bench output naming them. Per module: the hottest functions of each of its
phases until --share of that phase, joined; --emit-hot writes the joined
lists. Functions from core headers (inline / template, which each module
compiles into its own code) count for the module whose phase runs them. The
table shows what the Init patch's list alone covers of the module's worst
phase, and what the joined list costs."""
import argparse
import bisect
import collections
import os
import subprocess
import sys
import tomllib

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
        elif f.endswith(".h") and "/src/core/" in f:
            mods[a] = "core.h"   # inline / template code: a copy in each module that uses it
        else:
            mods[a] = "core"
    return mods


def engine_modules(modules_dirs):
    """Engine id -> module directory name, from the module.toml files."""
    out = {}
    for md in modules_dirs:
        root = os.path.join(md, "modules")
        if not os.path.isdir(root):
            continue
        for d in sorted(os.listdir(root)):
            path = os.path.join(root, d, "module.toml")
            if os.path.isfile(path):
                with open(path, "rb") as f:
                    for e in tomllib.load(f).get("engines", []):
                        out[int(e)] = d
    return out


def modes(a, syms, starts):
    """--modes: per-phase hot sets joined per module (see the module doc)."""
    names = {}   # data phase j (the plugin's "P j") -> (engine id, name, what); P 0 = startup
    for line in open(a.modes):
        p = line.split()
        if len(p) == 5 and p[0] == "hotphase":
            names[int(p[1]) + 1] = (int(p[2]), p[3], p[4])
    phases, cur = {}, None
    for line in open(a.hot):
        if line.startswith("P "):
            cur = phases.setdefault(int(line.split()[1]), collections.Counter())
            continue
        st, _end, cnt, n = line.split()
        addr = int(st, 16) & ~1
        i = bisect.bisect_right(starts, addr) - 1
        if cur is not None and i >= 0 and addr < syms[i][0] + max(syms[i][1], 2):
            cur[i] += int(cnt) * int(n)
    hot = sorted({i for c in phases.values() for i in c})
    own = owners(a.elf, [syms[i][0] for i in hot], a.modules_dir)
    repo = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    eng = engine_modules([a.modules_dir, repo])

    def pick(counts, share):   # {func: insns} -> the hottest funcs until share
        rows = [(v, i) for i, v in counts.items()]
        tot = sum(r[0] for r in rows) or 1
        got, out = 0, []
        for ins, i in sorted(rows, key=lambda r: -r[0]):
            if got >= share * tot:
                break
            got += ins
            out.append(i)
        return out

    per_mod = collections.defaultdict(list)   # module -> [(what, {func: insns})]
    for j, c in sorted(phases.items()):
        if j not in names or names[j][1] == "end":
            continue
        m = eng.get(names[j][0], names[j][1].lower())
        rows = {i: ins for i, ins in c.items() if own.get(syms[i][0]) in (m, "core.h")}
        per_mod[m].append((names[j][2], rows))
    # The fixed-budget picks: functions by instructions per byte over all of
    # the module's modes (equal weight per mode), until the budget is full.
    budgets = (2048, 4096, 6144)

    def budget_pick(ph, budget):
        w = collections.Counter()
        for _, rows in ph:
            tot = sum(rows.values()) or 1
            for i, v in rows.items():
                w[i] += v / tot
        used, out = 0, set()
        for i in sorted(w, key=lambda i: -w[i] / max(syms[i][1], 1)):
            if used + syms[i][1] <= budget:
                used += syms[i][1]
                out.add(i)
        return out

    def worst_cov(ph, chosen):
        worst, wname = 101.0, ""
        for w, rows in ph:
            tot = sum(rows.values()) or 1
            cov = 100.0 * sum(v for i, v in rows.items() if i in chosen) / tot
            if cov < worst:
                worst, wname = cov, w
        return worst, wname

    print(f"{'module':8} {'phases':>6} {'init B':>7} {'init worst':>18} {'joined B':>8} {'all-phase 95% B':>15}"
          + "".join(f" {'worst@' + str(b // 1024) + 'K':>9}" for b in budgets) + "  mean@4K")
    for m in sorted(per_mod):
        ph = per_mod[m]
        init = next((r for w, r in ph if w == "init"), ph[0][1])
        init_set = set(pick(init, a.share))
        worst, wname = 101.0, ""
        joined = set()
        total = collections.Counter()
        for w, rows in ph:
            tot = sum(rows.values()) or 1
            cov = 100.0 * sum(v for i, v in rows.items() if i in init_set) / tot
            if cov < worst:
                worst, wname = cov, w
            joined |= set(pick(rows, a.share))
            total.update(rows)
        size = lambda xs: sum(syms[i][1] for i in xs)
        agg = pick(total, a.share)
        cols = ""
        for b in budgets:
            cols += f" {worst_cov(ph, budget_pick(ph, b))[0]:8.0f}%"
        p4 = budget_pick(ph, 4096)
        mean = sum(100.0 * sum(v for i, v in rows.items() if i in p4) / (sum(rows.values()) or 1)
                   for _, rows in ph) / len(ph)
        print(f"{m:8} {len(ph):6d} {size(init_set):7d} {worst:5.0f}% {wname[:11]:>11} {size(joined):8d} {size(agg):15d}"
              + cols + f"  {mean:6.0f}%")
        if a.emit_hot:
            os.makedirs(a.emit_hot, exist_ok=True)
            tot = sum(total.values()) or 1
            lines = [f"# {m}: the hottest functions of each mode until {100 * a.share:.0f}% of that mode's "
                     f"instructions, joined (tools/synth_hot.py --modes, make -C tests bench-m4-synth-modes)"]
            for i in sorted(joined, key=lambda i: -total[i]):
                lines.append(f"{syms[i][2]}  # {100.0 * total[i] / tot:.1f}% {syms[i][1]} B")
            lines.append(f"# {size(joined)} B, {len(ph)} modes")
            with open(os.path.join(a.emit_hot, m + ".hot"), "w") as f:
                f.write("\n".join(lines) + "\n")
    return 0


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
    ap.add_argument("--modes", help="bench output naming the phases (bench-m4-synth-modes)")
    a = ap.parse_args()

    syms = functions(a.elf)
    starts = [s[0] for s in syms]
    if a.modes:
        return modes(a, syms, starts)
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
