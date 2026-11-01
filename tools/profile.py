#!/usr/bin/env python3
"""Build profiles: which modules an image compiles in (docs/PLAN_3X.md §7).

  profile.py gen   --profile profiles/full.toml --modules-dir DIR --out build/full/gen
      Reads the profile and each module's module.toml, checks them, and writes
      (only when the content changed, so make sees stable mtimes):
        modules.mk           make variables for the device, host-test, sim and tool builds
        ot_profile.h         OT_PROFILE_* / OT_MOD_<ID> defines
        module_registry.cpp  the constant factory table ModuleHost walks
  profile.py sizes --profile ... --modules-dir DIR --size-output FILE
      Per-module flash (text + data of its objects, from arm-none-eabi-size
      output) against each module's flash_max_bytes.
  profile.py budget --profile ... --modules-dir DIR
      The declared budgets per module and their sums: flash against the
      profile's flash_modules_max, static SRAM (sram_audio_bytes +
      fastrun_bytes: it comes off the stack headroom 1:1) against the
      optional sram_modules_max; on a failure, which module to leave out to
      fit. gen runs the same check. [3.x] The module SDRAM (sdram_bytes: the
      block ModuleHost::sdram() hands out, OT_PROFILE_SDRAM_BYTES in
      .sdram_bss) against the optional sdram_modules_max.
"""
import argparse
import os
import re
import subprocess
import sys
import tomllib

CORE_API = 1
ID_RE = re.compile(r"^[A-Z0-9]{4}$")
# The image's user-facing flavour name (boot splash, Config "Current version", diag log):
# plain ASCII the UI fonts have (no characters a C string, a printf format or make would
# read: board.cpp pastes it into a format string), single spaces, at most 10 characters so
# "OpenTracker <version> <name>" clears the "Current version" label (tests/flavour_name_test.cpp).
DISPLAY_RE = re.compile(r"[A-Za-z0-9](?:[A-Za-z0-9+.-]| (?=[^ ])){0,9}")
IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
BUDGETS = ("flash_max_bytes", "sram_audio_bytes", "sdram_bytes", "fastrun_bytes")


def fail(msg):
    print(f"profile: error: {msg}", file=sys.stderr)
    sys.exit(1)


def load_toml(path):
    try:
        with open(path, "rb") as f:
            return tomllib.load(f)
    except FileNotFoundError:
        fail(f"{path} not found")
    except tomllib.TOMLDecodeError as e:
        fail(f"{path}: {e}")


def load(profile_path, modules_dir):
    prof = load_toml(profile_path)
    for key in ("name", "variant"):
        if not isinstance(prof.get(key), str):
            fail(f"{profile_path}: '{key}' missing")
    if not re.fullmatch(r"[a-z]", prof["variant"]):
        fail(f"{profile_path}: variant must be one lower-case letter (24-char image name)")
    # display_name: the flavour name the device shows ("Techno"); default: the name, capitalised.
    defaulted = "display_name" not in prof
    disp = prof.setdefault("display_name", prof["name"].capitalize())
    if not isinstance(disp, str) or not DISPLAY_RE.fullmatch(disp):
        fail(f"{profile_path}: display_name {disp!r}"
             + (" (default from name; set display_name)" if defaulted else "")
             + ": 1-10 characters A-Z a-z 0-9 + . - and single spaces, starting with a letter or digit")
    mods, ids, basenames = [], set(), {}
    engine_ids, section_tags = {}, {}
    for entry in prof.get("modules", []):
        mid, rel = entry.get("id"), entry.get("path")
        if not mid or not rel:
            fail(f"{profile_path}: module entries need id and path")
        mdir = os.path.join(modules_dir, rel)
        man = load_toml(os.path.join(mdir, "module.toml"))
        if man.get("id") != mid:
            fail(f"{mdir}/module.toml: id {man.get('id')!r} != profile id {mid!r}")
        if not ID_RE.match(mid):
            fail(f"module id {mid!r}: 4 characters A-Z / 0-9")
        if mid in ids:
            fail(f"module {mid} listed twice")
        ids.add(mid)
        if man.get("api") != CORE_API:
            fail(f"module {mid}: api {man.get('api')!r}, the core provides {CORE_API}")
        if not IDENT_RE.match(man.get("entry", "")):
            fail(f"module {mid}: entry must be a C++ identifier")
        srcs = [os.path.join(mdir, "src", s) for s in man.get("sources", [])]
        tests = [os.path.join(mdir, "tests", t) for t in man.get("tests", [])]
        benches = [os.path.join(mdir, "bench", b) for b in man.get("bench_sources", [])]
        bench_entry = man.get("bench_entry", "")
        if benches and not IDENT_RE.match(bench_entry):
            fail(f"module {mid}: bench_sources need a bench_entry (C++ identifier)")
        # Engine ids (synth modules) and project-data tags (section) must be unique.
        for e in man.get("engines", []):
            if not isinstance(e, int) or not (e in (1, 2, 4, 5, 6) or 0x80 <= e <= 0xFF):
                fail(f"module {mid}: engine id {e!r} is neither a Tracker Mini id (1 2 4 5 6) nor 0x80..0xFF")
            if e in engine_ids:
                fail(f"module {mid}: engine id {e} also in module {engine_ids[e]}")
            engine_ids[e] = mid
        tag = man.get("section_tag")
        if tag is not None:
            if not isinstance(tag, int) or not (0x4000 <= tag <= 0x7FFF):
                fail(f"module {mid}: section_tag {tag!r} outside 0x4000..0x7FFF")
            if tag in section_tags:
                fail(f"module {mid}: section_tag {tag:#06x} also in module {section_tags[tag]}")
            section_tags[tag] = mid
        if not srcs:
            fail(f"module {mid}: no sources")
        for p in srcs + tests + benches:
            if not os.path.isfile(p):
                fail(f"module {mid}: {p} not found")
            b = os.path.basename(p)
            if b in basenames:
                fail(f"module {mid}: file name {b} also in module {basenames[b]} (names must be unique)")
            basenames[b] = mid
        mods.append({
            "id": mid, "dir": os.path.abspath(mdir), "man": man,
            "srcs": [os.path.abspath(p) for p in srcs], "tests": [os.path.abspath(p) for p in tests],
            "benches": [os.path.abspath(p) for p in benches], "bench_entry": bench_entry,
        })
    return prof, mods


def repo_rev(modules_dir):
    try:
        out = subprocess.run(["git", "-C", modules_dir, "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", modules_dir, "status", "--porcelain"],
                               capture_output=True, text=True, check=True).stdout.strip()
        return out + ("+" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "none"


def write_if_changed(path, text):
    try:
        with open(path) as f:
            if f.read() == text:
                return False
    except FileNotFoundError:
        pass
    with open(path, "w") as f:
        f.write(text)
    return True


def sram_of(m):
    return int(m["man"].get("sram_audio_bytes", 0)) + int(m["man"].get("fastrun_bytes", 0))


def budget_check(prof, mods):
    """[(what, used, limit, per-module sizes)] for every limit the profile sets,
    and the failure messages (with the module to leave out)."""
    rows, errors = [], []
    checks = [("flash", "flash_modules_max", lambda m: int(m["man"].get("flash_max_bytes", 0))),
              ("SRAM", "sram_modules_max", sram_of),
              ("SDRAM", "sdram_modules_max", lambda m: int(m["man"].get("sdram_bytes", 0)))]
    for what, key, size in checks:
        used = sum(size(m) for m in mods)
        limit = prof.get(key)
        rows.append((what, used, limit))
        if limit is not None and used > limit:
            over = used - limit
            fits = sorted((m for m in mods if size(m) >= over), key=size)
            hint = (f"; leave out {fits[0]['id']} ({size(fits[0])} B) to fit" if fits
                    else "; no single module frees enough")
            errors.append(f"profile {prof['name']}: modules declare {used} B {what}, {key} {limit} B{hint}")
    return rows, errors


def budget(args):
    prof, mods = load(args.profile, args.modules_dir)
    print(f"profile {prof['name']} ({prof['variant']}, \"{prof['display_name']}\"): {len(mods)} modules")
    print(f"  {'id':4s} {'flash_max':>9s} {'sram':>6s} {'fastrun':>7s} {'sdram':>7s} {'cpu/unit':>8s}")
    for m in mods:
        g = m["man"].get
        print(f"  {m['id']:4s} {int(g('flash_max_bytes', 0)):9d} {int(g('sram_audio_bytes', 0)):6d} "
              f"{int(g('fastrun_bytes', 0)):7d} {int(g('sdram_bytes', 0)):7d} {int(g('cpu_insn_per_unit', 0)):8d}")
    rows, errors = budget_check(prof, mods)
    for what, used, limit in rows:
        print(f"  {what:5s} {used:7d} B declared" + (f" of {limit} B ({limit - used:+d} B)" if limit is not None
                                                     else " (no limit in the profile)"))
    for e in errors:
        print(f"profile: error: {e}", file=sys.stderr)
    if errors:
        sys.exit(1)


def gen(args):
    prof, mods = load(args.profile, args.modules_dir)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    rev = repo_rev(args.modules_dir) if mods else "none"
    sums = {k: sum(int(m["man"].get(k, 0)) for m in mods) for k in BUDGETS}
    _, errors = budget_check(prof, mods)
    if errors:
        fail(errors[0])
    ids = [m["id"] for m in mods]
    src_dirs = sorted({os.path.dirname(p) for m in mods for p in m["srcs"]})
    test_dirs = sorted({os.path.dirname(p) for m in mods for p in m["tests"]})
    reg = os.path.join(out, "module_registry.cpp")

    mk = [
        "# Generated by tools/profile.py -- do not edit.",
        f"OT_PROFILE_NAME := {prof['name']}",
        f"OT_PROFILE_VARIANT := {prof['variant']}",
        f"MOD_IDS := {' '.join(ids)}",
        f"MOD_SRC_DIRS := {' '.join(src_dirs)}",
        f"MOD_TEST_DIRS := {' '.join(test_dirs)}",
        f"MOD_SRCS := {' '.join(os.path.basename(p) for m in mods for p in m['srcs'])} module_registry.cpp",
        f"MOD_SRC_PATHS := {' '.join(p for m in mods for p in m['srcs'])} {reg}",
        f"MOD_HDRS := {' '.join(os.path.join(d, '*.h') for d in src_dirs)}",
        f"MOD_TEST_SRCS := {' '.join(os.path.basename(p) for m in mods for p in m['tests'])}",
        f"MOD_BENCH_PATHS := {' '.join(p for m in mods for p in m['benches'])} {os.path.join(out, 'module_bench.cpp')}",
        f"MOD_INCS := -I{out} {' '.join('-I' + d for d in src_dirs)}",
    ]
    for m in mods:
        mk.append(f"MOD_OBJ_NAMES_{m['id']} := "
                  + " ".join(os.path.basename(p) + ".o" for p in m["srcs"]))
    changed = write_if_changed(os.path.join(out, "modules.mk"), "\n".join(mk) + "\n")

    desc = " ".join(f"{m['id']} {m['man'].get('version', '0')}" for m in mods)
    h = [
        "// Generated by tools/profile.py -- do not edit.",
        "#pragma once",
        f'#define OT_PROFILE_NAME "{prof["name"]}"',
        f'#define OT_PROFILE_VARIANT "{prof["variant"]}"',
        f'#define OT_PROFILE_DISPLAY_NAME "{prof["display_name"]}"',
        f'#define OT_PROFILE_MODULES "{desc}"',
        f"#define OT_PROFILE_MODULE_COUNT {len(mods)}",
        f"#define OT_PROFILE_SDRAM_BYTES {sums['sdram_bytes']}",
        f'#define OT_MODULES_REV "{rev}"',
    ] + [f"#define OT_MOD_{i} 1" for i in ids]
    changed |= write_if_changed(os.path.join(out, "ot_profile.h"), "\n".join(h) + "\n")

    r = ["// Generated by tools/profile.py -- do not edit.",
         '#include "modules/module.h"', "", "namespace ot {", "namespace mod {", ""]
    r += [f"IModule& {m['man']['entry']}();" for m in mods]
    r += ["", "const ModuleFactory kModuleFactories[] = {"
          + "".join(f"&{m['man']['entry']}, " for m in mods) + "nullptr};",
          "", "}  // namespace mod", "}  // namespace ot"]
    entries = [m["bench_entry"] for m in mods if m["benches"]]
    b = ["// Generated by tools/profile.py -- do not edit.", '#include "bench_api.h"', ""]
    b += [f"void {e}();" for e in entries]
    b += ["", "void runModuleBenches() {"] + [f"    {e}();" for e in entries] + ["}"]
    changed |= write_if_changed(os.path.join(out, "module_bench.cpp"), "\n".join(b) + "\n")
    changed |= write_if_changed(reg, "\n".join(r) + "\n")

    if changed and not args.quiet:
        print(f"profile: {prof['name']} ({prof['variant']}) {len(mods)} modules"
              + (f" [{' '.join(ids)}], flash_max {sums['flash_max_bytes']} B, modules rev {rev}" if mods else ""),
              file=sys.stderr)


def sizes(args):
    _, mods = load(args.profile, args.modules_dir)
    per_obj = {}
    with open(args.size_output) as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 6 and parts[0].isdigit():
                per_obj[os.path.basename(parts[5])] = int(parts[0]) + int(parts[1])
    bad = False
    for m in mods:
        names = [os.path.basename(p) + ".o" for p in m["srcs"]]
        missing = [n for n in names if n not in per_obj]
        if missing:
            fail(f"module {m['id']}: no size for {' '.join(missing)}")
        used = sum(per_obj[n] for n in names)
        cap = int(m["man"].get("flash_max_bytes", 0))
        ok = used <= cap
        bad |= not ok
        print(f"module: {'ok  ' if ok else 'FAIL'} {m['id']} {used:6d} B flash (max {cap} B)")
    if bad:
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("gen", "sizes", "budget"):
        p = sub.add_parser(name)
        p.add_argument("--profile", required=True)
        p.add_argument("--modules-dir", required=True)
        if name == "gen":
            p.add_argument("--out", required=True)
            p.add_argument("--quiet", action="store_true")
        elif name == "sizes":
            p.add_argument("--size-output", required=True)
    args = ap.parse_args()
    {"gen": gen, "sizes": sizes, "budget": budget}[args.cmd](args)


if __name__ == "__main__":
    main()
