#!/usr/bin/env python3
"""OpenTracker performance checks (docs/PLAN_2X.md "Gate", make perf-gate).

  telemetry_check.py log <device.log> [--render-avg 45] [--render-max 70]
                         [--loop-max-ms 50] [--loading-bytes 65536] [--cpu-peak 70]
      Checks the 5 s telemetry windows of a captured diag log (docs/HARDWARE_TESTING.md):
        * render avg  < 45 % of the 522240-cycle block budget (windows with audio blocks),
        * render max  < 70 %,
        * loop max    < 50 ms, except in loading windows,
        * 2.x "telemetry: cpu" lines (1 Hz, docs/specs/2x-02 §4): peak < 70 %.
      A window is "loading" when its "telemetry: io" line (logged right before the
      render line of the same window, 2.0.0+) shows >= --loading-bytes SD bytes
      read or written, when it has a "telemetry: load" line (a background project
      job ran in it), or when a line in the window contains "loading". Logs
      without io lines (1.9.x) apply the loop check to every window.
  telemetry_check.py bench --baseline tests/perf_baseline.txt <bench_m4 output>
      bench-m4 instruction counts per block against the baseline: fail above +5 %
      (--tolerance). Placeholder ("-") or missing scenarios are reported, never fail.
      Absolute engine limits: every acd.* / vap* / fat* / perc* line that prints "limit N"
      (docs/OPTIMIZATION_2X.md item 10: ACD and VAP Init <= 12k, worst <= 18k
      instructions per voice per block, FAT Init <= 12k, worst <= 22k, PERC <= 30k per
      slot incl. its peak block)
      fails above N.
  telemetry_check.py size [--size-tool arm-none-eabi-size] [--nm-tool arm-none-eabi-nm] <firmware.elf>
      Flash use = .text + .data (Berkeley size): fail above 720,000 B, warn above
      600,000 B (docs/VERIFY_M1.md R4, docs/MINI_PLAN.md §5).
      With nm (default: the size tool's sibling): no double-precision libm in the
      image, and the stack headroom _estack - _ebss: fail below 90 KiB
      (docs/OPTIMIZATION_2X.md OP-4 / OP-5).
  telemetry_check.py cpi <device.log> <bench_m4 output> [--top 5]
      CPI calibration (docs/OPTIMIZATION_2X.md "CPU round 3"): device cycles per bench-m4
      instruction, per phase and over the render, from the device's peak blocks (the `pk`
      part of the 2.x cpu lines, costliest --top blocks by render cost without the seq
      phase) against the bench's song 5 replay worst blocks ("peak: worst blocks" lines).
      The seq phase is left out (the replay has no sequencer). `log --bench <output>`
      prints the same lines after the log report.
  telemetry_check.py selftest
      Runs both checkers on built-in samples (keeps the parser honest).
Exit status: 0 pass, 1 threshold failure, 2 usage / unreadable input.
"""
import argparse
import re
import sys

BUDGET = 522240

RENDER_RE = re.compile(
    r"telemetry: render cyc/block min (\d+) avg (\d+) max (\d+) \([^)]*\) blocks (\d+)"
    r" \| loop max (\d+)\.(\d+) ms \(n (\d+)\)")
IO_RE = re.compile(r"telemetry: io sd cmds (\d+) read (\d+) B \((\d+) KB/s\) write (\d+) B \((\d+) KB/s\)"
                   r"(?: direct (\d+) bounced (\d+))?(?: \| cache sdram (\w+) \(selftest ([\w-]*)\))?")
UI_RE = re.compile(r"telemetry: ui (\d+) frames \((\d+)\.(\d+) fps\) render avg (\d+)\.(\d+) max (\d+)\.(\d+) ms")
LOAD_RE = re.compile(r"telemetry: load op (\d+) run (\d) passes (\d+)")
TS_RE = re.compile(r"^\[\s*(\d+)\.(\d+)\]")
CPU_RE = re.compile(r"telemetry: cpu avg=(\d+)\.(\d)% peak=(\d+)\.(\d)% overruns=(\d+) cap=(\d+)/(\d+)"
                    r" synth_voices=(\d+) cyc_per_voice=(\d+)")
# Peak-block attribution after the cpu line (spec 2x-02 §4, audio/block_profile.h).
PK_RE = re.compile(r"\| pk (\d+)k: (.*?) \| ev (.*)$")
BENCH_RE = re.compile(r"^\s+([A-Za-z]\w*)\s+(\d+) insn")
# bench-m4 "peak" worst-block attribution (tests/audio_bench.cpp), phases in M4 instructions.
BENCH_PK_RE = re.compile(r"^\s+block\s+\d+ \(song pos \d+, step \d+\):\s+(\d+) M4 insn, est\.\s+[\d.]+% \| (.*?) \| (.*)$")
# Device cycles per bench-m4 instruction, render phases of song 5's peak blocks (no seq):
# diag-185237 (2.x 57cbf5e, .fastrun 13.7 KB; 5 peak blocks, 427k cycles) against bench-m4's
# replay worst blocks (220k insn). Update it with `telemetry_check.py cpi` after every device
# run; docs/OPTIMIZATION_2X.md "CPU round 3" (predicted ~1.7 with the round-3 .fastrun).
DEVICE_CPI = 1.94
LIMIT_RE = re.compile(r"^\s+((?:acd|vap|fat|wtfm|perc|pswitch|stream)[\w.]*)\s+(\d+(?:\.\d+)?)\s.*\blimit (\d+)\b")
FLASH_FAIL = 720000
FLASH_WARN = 600000
STACK_MIN = 90 * 1024
# newlib double-precision libm entry points / kernels (the float ones end in f).
DOUBLE_LIBM = {"__ieee754_pow", "__ieee754_exp", "__ieee754_log", "__ieee754_log10", "__ieee754_sqrt",
               "__ieee754_rem_pio2", "__kernel_rem_pio2", "__kernel_sin", "__kernel_cos", "pow", "exp", "log",
               "log10", "sin", "cos", "tan", "sqrt", "floor", "ceil", "lround", "round", "trunc", "fmod"}


def windows(lines):
    """Yields dicts: one per render line, with the io/ui/marker data of its window."""
    pending_io, marker = None, False
    out = []
    for n, line in enumerate(lines, 1):
        m = IO_RE.search(line)
        if m:
            pending_io = {"cmds": int(m.group(1)), "read": int(m.group(2)), "write": int(m.group(4)),
                          "cache": m.group(8), "cachetest": m.group(9)}
            continue
        m = LOAD_RE.search(line)
        if m:
            marker = True   # logged only for windows in which a project job ran
            continue
        m = RENDER_RE.search(line)
        if m:
            ts = TS_RE.search(line)
            out.append({
                "line": n, "t": f"{ts.group(1)}.{ts.group(2)}" if ts else "?",
                "avg": int(m.group(2)), "max": int(m.group(3)), "blocks": int(m.group(4)),
                "loop_us": int(m.group(5)) * 1000 + int(m.group(6)), "io": pending_io, "marker": marker,
            })
            pending_io, marker = None, False
            continue
        m = UI_RE.search(line)
        if m and out:
            out[-1]["ui"] = (int(m.group(1)), float(f"{m.group(2)}.{m.group(3)}"),
                             float(f"{m.group(4)}.{m.group(5)}"), float(f"{m.group(6)}.{m.group(7)}"))
            continue
        if "telemetry:" not in line and "loading" in line.lower():
            marker = True
    return out


def phase_parts(text, unit_k):
    """'seq 16k on 4k/14k ctl 11k ...' (device, k = 1000 cycles) or 'seq 920 on 4960/4120 ...'
    (bench, instructions) -> [(name, value)]; values in thousands."""
    toks = text.split()
    parts = []
    i = 0

    def num(v):
        v = v.rstrip("k") if unit_k else v
        return float(v) if unit_k else float(v) / 1000.0

    while i + 1 < len(toks):
        name, val = toks[i], toks[i + 1]
        if name == "on" and "/" in val:     # "on 8k/9k": sample / synth note work
            a, b = val.split("/")
            parts += [("on.smp", num(a)), ("on.syn", num(b))]
        elif (val.endswith("k") and val[:-1].isdigit()) if unit_k else val.isdigit():
            parts.append((name, num(val)))
        i += 2
    return parts


def pk_parts(line):
    """The peak block of a cpu line: {"total": k, "parts": [(name, k)], "ev": text} or None."""
    m = PK_RE.search(line)
    if not m:
        return None
    parts = [(n, int(v)) for n, v in phase_parts(m.group(2), True)]
    return {"total": int(m.group(1)), "parts": parts, "ev": m.group(3)}


def bench_pk_blocks(lines):
    """bench-m4 'peak: worst blocks' lines -> [{"total": k insn, "parts": [(name, k insn)]}]."""
    out = []
    for line in lines:
        m = BENCH_PK_RE.match(line)
        if m:
            out.append({"total": int(m.group(1)) / 1000.0, "parts": phase_parts(m.group(2), False)})
    return out


def phase_key(name):
    """Engine slots by engine name (VAP / ACD / PERC ...); '-' slots dropped."""
    return None if name == "-" else name


def cpi_report(log_lines, bench_lines, top=5, quiet=False):
    """Device cycles per bench instruction: per phase and over the render (seq excluded).
    Returns (overall factor or None, {phase: factor})."""
    dev = [c["pk"] for c in cpu_lines(log_lines) if c["pk"] and c["voices"] > 0]
    if not dev:
        dev = [c["pk"] for c in cpu_lines(log_lines) if c["pk"]]
    bench = bench_pk_blocks(bench_lines)
    if not dev or not bench:
        if not quiet:
            print("cpi: need device pk lines and bench-m4 'peak' worst-block lines")
        return None, {}

    def render(p):
        return sum(v for n, v in p["parts"] if n != "seq" and phase_key(n))

    dev = sorted(dev, key=render, reverse=True)[:top]

    def mean_parts(blocks):
        acc = {}
        for b in blocks:
            for n, v in b["parts"]:
                k = phase_key(n)
                if k and k != "seq":
                    acc[k] = acc.get(k, 0.0) + v
        return {k: v / len(blocks) for k, v in acc.items()}

    d, b = mean_parts(dev), mean_parts(bench)
    per = {k: d[k] / b[k] for k in d if k in b and b[k] >= 1.0}
    dsum = sum(d[k] for k in per)
    bsum = sum(b[k] for k in per)
    overall = dsum / bsum if bsum else None
    if not quiet:
        print(f"cpi: device {len(dev)} peak blocks (render {dsum:.0f}k cycles, seq excluded) vs bench "
              f"{len(bench)} worst blocks ({bsum:.0f}k M4 insn): {overall:.2f} device cycles per bench insn")
        print("cpi: per phase " + ", ".join(f"{k} {per[k]:.2f} ({d[k]:.0f}k/{b[k]:.1f}k)"
                                             for k in sorted(per, key=lambda k: -d[k])))
    return overall, per


def cpu_lines(lines):
    """Yields dicts for the 2.x 1 Hz "telemetry: cpu" lines (spec 2x-02 §4)."""
    out = []
    for n, line in enumerate(lines, 1):
        m = CPU_RE.search(line)
        if not m:
            continue
        ts = TS_RE.search(line)
        out.append({
            "line": n, "t": f"{ts.group(1)}.{ts.group(2)}" if ts else "?",
            "avg": float(f"{m.group(1)}.{m.group(2)}"), "peak": float(f"{m.group(3)}.{m.group(4)}"),
            "overruns": int(m.group(5)), "cap": int(m.group(6)), "budget": int(m.group(7)),
            "voices": int(m.group(8)), "cyc_per_voice": int(m.group(9)),
            "pk": pk_parts(line),
        })
    return out


def check_log(lines, render_avg=45.0, render_max=70.0, loop_max_ms=50.0, loading_bytes=65536, quiet=False,
              cpu_peak=70.0):
    ws = windows(lines)
    cpus = cpu_lines(lines)
    fails, loading, audio = [], 0, 0
    worst_avg = worst_max = worst_loop = 0.0
    for w in ws:
        io = w["io"]
        is_loading = w["marker"] or (io is not None and (io["read"] >= loading_bytes or io["write"] >= loading_bytes))
        loading += is_loading
        where = f"line {w['line']} [{w['t']}]"
        if w["blocks"]:
            audio += 1
            a, mx = 100.0 * w["avg"] / BUDGET, 100.0 * w["max"] / BUDGET
            worst_avg, worst_max = max(worst_avg, a), max(worst_max, mx)
            if a >= render_avg:
                fails.append(f"{where}: render avg {a:.1f}% >= {render_avg:g}%")
            if mx >= render_max:
                fails.append(f"{where}: render max {mx:.1f}% >= {render_max:g}%")
        loop_ms = w["loop_us"] / 1000.0
        if not is_loading:
            worst_loop = max(worst_loop, loop_ms)
            if loop_ms >= loop_max_ms:
                fails.append(f"{where}: loop max {loop_ms:.3f} ms >= {loop_max_ms:g} ms (not loading)")
    for c in cpus:
        if c["peak"] >= cpu_peak:
            fails.append(f"line {c['line']} [{c['t']}]: cpu peak {c['peak']:.1f}% >= {cpu_peak:g}%")
    if not quiet:
        if cpus:
            print(f"telemetry: {len(cpus)} cpu lines; worst peak {max(c['peak'] for c in cpus):.1f}%, avg "
                  f"{max(c['avg'] for c in cpus):.1f}%, overruns {max(c['overruns'] for c in cpus)}, "
                  f"min cap {min(c['cap'] for c in cpus)}/{cpus[0]['budget']}")
            pks = [c for c in cpus if c["pk"]]
            if pks:   # the costliest peak block and what it was made of
                w = max(pks, key=lambda c: c["pk"]["total"])
                top = sorted(w["pk"]["parts"], key=lambda kv: -kv[1])[:5]
                print(f"telemetry: worst peak block {w['pk']['total']}k cycles (line {w['line']} [{w['t']}]): "
                      + ", ".join(f"{n} {v}k" for n, v in top) + f" | {w['pk']['ev']}")
        caches = sorted({f"{w['io']['cache']}/{w['io']['cachetest']}" for w in ws if w["io"] and w["io"]["cache"]})
        print(f"telemetry: {len(ws)} windows ({audio} with audio, {loading} loading); worst render avg "
              f"{worst_avg:.1f}% max {worst_max:.1f}%, worst loop {worst_loop:.3f} ms outside loading"
              + (f"; cache {', '.join(caches)}" if caches else ""))
        uis = [w["ui"] for w in ws if "ui" in w]
        if uis:
            print(f"telemetry: ui fps min {min(u[1] for u in uis):.1f}, render max {max(u[3] for u in uis):.2f} ms")
        for f in fails:
            print("FAIL " + f)
    if not ws and not quiet:
        print("telemetry: no telemetry windows found (was a USB terminal open?)")
    return len(ws) > 0 and not fails, fails, ws


def parse_baseline(lines):
    base = {}
    for line in lines:
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        name, val = line.split()[:2]
        base[name] = None if val == "-" else int(val)
    return base


def check_bench(baseline_lines, output_lines, tolerance=5.0, quiet=False):
    base = parse_baseline(baseline_lines)
    got = {}
    for line in output_lines:
        m = BENCH_RE.match(line)
        if m:
            got[m.group(1)] = int(m.group(2))
    fails = []
    if not got:
        fails.append("no scenario lines in the bench-m4 output")
    for name, ref in base.items():
        cur = got.get(name)
        if ref is None:
            msg = f"{name:8s} placeholder" + (f" (bench now prints {cur} insn: add it to the baseline)" if cur else "")
        elif cur is None:
            msg = f"{name:8s} MISSING from bench output (baseline {ref})"
            fails.append(msg)
        else:
            d = 100.0 * (cur - ref) / ref
            msg = f"{name:8s} {cur:8d} insn vs {ref:8d} ({d:+.1f}%)"
            if d > tolerance:
                fails.append(msg + f" > +{tolerance:g}%")
                msg = "FAIL " + msg
            elif d < -tolerance:
                msg += "  (faster: update tests/perf_baseline.txt)"
        if not quiet:
            print("bench-m4: " + msg)
    for name in got:
        if name not in base and not quiet:
            print(f"bench-m4: {name:8s} {got[name]:8d} insn (no baseline entry)")
    if not quiet:   # device estimate from the calibrated CPI (cpi command, docs/OPTIMIZATION_2X.md)
        for name in ("peakavg", "peak"):
            if name in got:
                est = got[name] * DEVICE_CPI
                print(f"bench-m4: {name:8s} device est. {est / 1000:.0f}k cycles ({100.0 * est / BUDGET:.1f}% of "
                      f"{BUDGET}) at {DEVICE_CPI:g} cycles/insn (render without seq)")
    limits = 0
    for line in output_lines:
        m = LIMIT_RE.match(line)
        if not m:
            continue
        limits += 1
        name, val, lim = m.group(1), float(m.group(2)), int(m.group(3))
        msg = f"{name:8s} {val:8.0f} insn, limit {lim}"
        if val > lim:
            fails.append(msg + " exceeded")
            msg = "FAIL " + msg
        if not quiet:
            print("bench-m4: " + msg)
    if got and not limits:
        fails.append("no engine limit lines (acd / vap / perc) in the bench-m4 output")
    return not fails, fails


def check_size(size_output, quiet=False):
    """Berkeley `size` output -> (ok, flash bytes). Fails above FLASH_FAIL."""
    for line in size_output:
        f = line.split()
        if len(f) >= 4 and f[0].isdigit() and f[1].isdigit():
            flash = int(f[0]) + int(f[1])
            ok = flash <= FLASH_FAIL
            if not quiet:
                state = "FAIL" if not ok else ("WARN" if flash > FLASH_WARN else "ok")
                print(f"flash: {state} .text+.data {flash} B ({flash / 1000:.1f} kB); "
                      f"fail > {FLASH_FAIL} B, warn > {FLASH_WARN} B")
            return ok, flash
    if not quiet:
        print("flash: no size line found")
    return False, 0


def check_symbols(nm_output, quiet=False):
    """`nm` output -> (ok, stack headroom bytes): no double libm, headroom >= STACK_MIN."""
    syms, found = {}, []
    for line in nm_output:
        f = line.split()
        if len(f) == 3:
            syms[f[2]] = (f[1], int(f[0], 16))
            if f[1] in "Tt" and f[2] in DOUBLE_LIBM:
                found.append(f[2])
    ok = not found
    if not quiet:
        print("libm: " + (f"FAIL double-precision functions linked: {' '.join(sorted(found))}" if found
                          else "ok float only"))
    head = 0
    if "_estack" in syms and "_ebss" in syms:
        head = syms["_estack"][1] - syms["_ebss"][1]
        if head < STACK_MIN:
            ok = False
        if not quiet:
            print(f"stack: {'FAIL' if head < STACK_MIN else 'ok'} headroom {head} B ({head / 1024:.1f} KiB, "
                  f"_estack - _ebss); fail < {STACK_MIN} B")
    elif not quiet:
        print("stack: _estack / _ebss not found")
    return ok, head


SAMPLE_OK = """\
[  12.000] sd: f_mount result 0
[  20.001] telemetry: io sd cmds 812 read 4194304 B (819 KB/s) write 0 B (0 KB/s) direct 120 bounced 2 | cache sdram on (selftest ok) | sd->sdram dma read direct write direct
[  20.002] telemetry: render cyc/block min 20000 avg 30000 max 40000 (7.6% of 522240) blocks 1723 | loop max 140.500 ms (n 90) | SDRAM free 600000 B | i2c errors i2c0 0 i2c2 0
[  20.003] telemetry: ui 75 frames (15.0 fps) render avg 4.00 max 6.10 ms, last frame 1800 DL words
[  25.001] telemetry: io sd cmds 0 read 0 B (0 KB/s) write 0 B (0 KB/s) direct 0 bounced 0 | cache sdram on (selftest ok) | sd->sdram dma read direct write direct
[  25.002] telemetry: render cyc/block min 150000 avg 193000 max 250000 (47.8% of 522240) blocks 1723 | loop max 31.250 ms (n 160) | SDRAM free 600000 B | i2c errors i2c0 0 i2c2 0
[  26.000] telemetry: cpu avg=37.2% peak=48.0% overruns=0 cap=4/4 synth_voices=0 cyc_per_voice=0
"""
SAMPLE_BAD = """\
[  27.001] telemetry: io sd cmds 3 read 1536 B (0 KB/s) write 0 B (0 KB/s) direct 1 bounced 0 | cache sdram on (selftest ok) | sd->sdram dma read direct write bounced
[  27.001] telemetry: load op 2 run 1 passes 40 units 90 total 300000 us max pass 9000 us (stage 3/1) max unit 4000 us (stage 3) | open 1 us n1 read 1 us n1 0 KB write 0 us n0 0 KB dir 1 us n1 decode 0 us commit 0 us
[  27.002] telemetry: render cyc/block min 20000 avg 30000 max 40000 (7.6% of 522240) blocks 1723 | loop max 90.000 ms (n 90) | SDRAM free 1 B | i2c errors i2c0 0 i2c2 0
[  30.002] telemetry: render cyc/block min 150000 avg 240000 max 380000 (72.7% of 522240) blocks 1723 | loop max 61.000 ms (n 80) | SDRAM free 1 B | i2c errors i2c0 0 i2c2 0 RENDER>70%
[  31.000] telemetry: cpu avg=52.0% peak=72.7% overruns=0 cap=3/4 synth_voices=3 cyc_per_voice=21000 | pk 380k: seq 5k on 8k/9k ctl 5k smp 160k syn 15k VAP 85k ACD 27k PERC 21k dly 8k rev 40k out 36k | ev on 8 VAP 4 ACD 1 PERC 2 rl 0 fc 0 mc 0 st 6 pd 2 df 3 vx 10/8
"""
SAMPLE_BENCH = """\
audio_bench (cortex-m4 -O2 under QEMU, instruction counts):
  idle     11808 insn +     0 SDRAM acc/block -> est.   11808 cycles (  2.3% of 522240)
  play8   165000 insn +  1522 SDRAM acc/block -> est.  206395 cycles ( 39.5% of 522240)
  vapinit     9025 insn per voice per block (limit 12000)
"""
SAMPLE_BENCH_LIMITS = """\
  idle     11808 insn +     0 SDRAM acc/block -> est.   11808 cycles (  2.3% of 522240)
  vapworst   17148 insn per voice per block (limit 18000)
    synth.fw     3100.0  (framework overhead per block, limit 3000 M4 insn)
    perc.pk     30100.0  (PERC peak block incl. the hits, limit 30000 M4 insn)
    acd.max   16004.0  (one ACD voice, limit 18000 M4 insn)
"""


def selftest():
    ok, fails, ws = check_log(SAMPLE_OK.splitlines(), quiet=True)
    assert ok and len(ws) == 2, (ok, fails)
    assert ws[0]["io"]["read"] == 4194304 and ws[0]["io"]["cache"] == "on"   # loading window: 140 ms allowed
    ok, fails, _ = check_log((SAMPLE_OK + SAMPLE_BAD).splitlines(), quiet=True)
    assert not ok and len(fails) == 4, fails     # avg 46 %, max 72.8 %, loop 61 ms (90 ms: job running), cpu peak
    assert any("cpu peak 72.7%" in f for f in fails), fails
    cs = cpu_lines(SAMPLE_OK.splitlines())
    assert len(cs) == 1 and cs[0]["peak"] == 48.0 and cs[0]["cap"] == 4 and cs[0]["budget"] == 4, cs
    assert cs[0]["pk"] is None
    pk = cpu_lines(SAMPLE_BAD.splitlines())[0]["pk"]
    assert pk["total"] == 380 and ("smp", 160) in pk["parts"] and ("on.syn", 9) in pk["parts"], pk
    assert ("PERC", 21) in pk["parts"] and pk["ev"].startswith("on 8 VAP 4"), pk
    ok, fails, _ = check_log([], quiet=True)
    assert not ok
    bench_pk = ["    block  6672 (song pos 2, step 160):  224440 M4 insn, est.  57.8% | seq 920 on 4960/4120 ctl 5080 "
                "smp 86280 syn 9880 VAP 41920 ACD 13960 PERC 10160 dly 7440 rev 20560 out 18600 | on 8 VAP 3"]
    assert bench_pk_blocks(bench_pk)[0]["parts"][0] == ("seq", 0.92)
    overall, per = cpi_report(SAMPLE_BAD.splitlines(), bench_pk, quiet=True)
    assert abs(per["rev"] - 40 / 20.56) < 1e-9 and abs(per["smp"] - 160 / 86.28) < 1e-9, per
    assert "seq" not in per and 1.5 < overall < 2.5, overall
    base = ["idle 11808", "play8 154632", "synth4 -"]
    ok, fails = check_bench(base, SAMPLE_BENCH.splitlines(), quiet=True)
    assert not ok and len(fails) == 1 and "play8" in fails[0], fails   # 165000 = +6.7 %
    ok, fails = check_bench(["idle 11808"], SAMPLE_BENCH_LIMITS.splitlines(), quiet=True)
    assert not ok and len(fails) == 1 and "perc.pk" in fails[0], fails   # 30100 > 30000
    ok, _ = check_size(["   text    data     bss     dec     hex filename",
                        " 612000    6700  180000  798700   c2fec x.elf"], quiet=True)
    assert ok
    ok, flash = check_size([" 715000    6700  180000  901700   dc244 x.elf"], quiet=True)
    assert not ok and flash == 721700
    nm = ["0006c138 T powf", "20030000 B _estack", "20017000 B _ebss", "0006b400 t helper"]
    ok, head = check_symbols(nm, quiet=True)
    assert ok and head == 0x19000, head
    ok, _ = check_symbols(nm + ["0006b500 T __ieee754_pow"], quiet=True)
    assert not ok
    ok, head = check_symbols(["20030000 B _estack", "20023820 B _ebss"], quiet=True)
    assert not ok and head == 51168
    print("telemetry_check: selftest ok")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    lg = sub.add_parser("log")
    lg.add_argument("path")
    lg.add_argument("--render-avg", type=float, default=45.0)
    lg.add_argument("--render-max", type=float, default=70.0)
    lg.add_argument("--loop-max-ms", type=float, default=50.0)
    lg.add_argument("--loading-bytes", type=int, default=65536)
    lg.add_argument("--cpu-peak", type=float, default=70.0)
    bn = sub.add_parser("bench")
    bn.add_argument("--baseline", required=True)
    bn.add_argument("--tolerance", type=float, default=5.0)
    bn.add_argument("output")
    sz = sub.add_parser("size")
    sz.add_argument("--size-tool", default="arm-none-eabi-size")
    sz.add_argument("--nm-tool", default=None)
    sz.add_argument("elf")
    cp = sub.add_parser("cpi")
    cp.add_argument("log")
    cp.add_argument("bench")
    cp.add_argument("--top", type=int, default=5)
    lg.add_argument("--bench", default=None)
    sub.add_parser("selftest")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "log":
            with open(a.path, errors="replace") as f:
                log_lines = f.read().splitlines()
                ok, _, _ = check_log(log_lines, a.render_avg, a.render_max, a.loop_max_ms,
                                     a.loading_bytes, cpu_peak=a.cpu_peak)
            if a.bench:
                with open(a.bench, errors="replace") as b:
                    cpi_report(log_lines, b.read().splitlines())
        elif a.cmd == "cpi":
            with open(a.log, errors="replace") as f, open(a.bench, errors="replace") as b:
                overall, _ = cpi_report(f.read().splitlines(), b.read().splitlines(), a.top)
            ok = overall is not None
        elif a.cmd == "bench":
            with open(a.baseline) as b, open(a.output, errors="replace") as o:
                ok, _ = check_bench(b.read().splitlines(), o.read().splitlines(), a.tolerance)
        elif a.cmd == "size":
            import subprocess
            r = subprocess.run([a.size_tool, a.elf], capture_output=True, text=True)
            if r.returncode != 0:
                print(f"telemetry_check: {a.size_tool} failed: {r.stderr.strip()}", file=sys.stderr)
                return 2
            ok, _ = check_size(r.stdout.splitlines())
            nm = a.nm_tool or (a.size_tool[:-4] + "nm" if a.size_tool.endswith("size") else "nm")
            r = subprocess.run([nm, a.elf], capture_output=True, text=True)
            if r.returncode != 0:
                print(f"telemetry_check: {nm} failed: {r.stderr.strip()}", file=sys.stderr)
                return 2
            ok_sym, _ = check_symbols(r.stdout.splitlines())
            ok = ok and ok_sym
        else:
            selftest()
            ok = True
    except OSError as e:
        print(f"telemetry_check: {e}", file=sys.stderr)
        return 2
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
