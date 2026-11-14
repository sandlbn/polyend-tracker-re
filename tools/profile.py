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

[I] Module entries: `in_tree = true` takes the path from the
repository root (modules/<name>) instead of MODULES_DIR. `card = true` makes a
card module: the device image leaves it out (make card builds it as
build/<profile>/otm/<name>.otm for the card's Modules/ folder), while host
builds (gen --host: tests, sim, tools) compile it in, as a device with the
card's modules would have it. Budgets count only the image's modules.
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


def load(profile_path, modules_dir, host=True):
    """The profile and its modules; host=False leaves the card modules out (as
    the "card" list of the result's profile)."""
    prof = load_toml(profile_path)
    root = os.path.dirname(os.path.dirname(os.path.abspath(profile_path)))
    prof["card"] = []
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
    engine_ids, section_tags, effect_ids = {}, {}, {}
    for entry in prof.get("modules", []):
        mid, rel = entry.get("id"), entry.get("path")
        if not mid or not rel:
            fail(f"{profile_path}: module entries need id and path")
        mdir = os.path.join(root if entry.get("in_tree") else modules_dir, rel)
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
        # Per-instance effects (IModule::effect): 4-character ids, unique in the profile.
        for fx in man.get("effects", []):
            if not isinstance(fx, str) or not ID_RE.match(fx):
                fail(f"module {mid}: effect id {fx!r}: 4 characters A-Z / 0-9")
            if fx in effect_ids:
                fail(f"module {mid}: effect id {fx} also in module {effect_ids[fx]}")
            effect_ids[fx] = mid
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
        m = {
            "id": mid, "dir": os.path.abspath(mdir), "man": man, "card": bool(entry.get("card")),
            "srcs": [os.path.abspath(p) for p in srcs], "tests": [os.path.abspath(p) for p in tests],
            "benches": [os.path.abspath(p) for p in benches], "bench_entry": bench_entry,
        }
        if m["card"]:
            prof["card"].append(m)
        if host or not m["card"]:
            mods.append(m)
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


# Synth slot inserts: on when a module offers an effect (module.toml effects)
# and the profile does not set slot_inserts = false. The core then reserves
# one delay line per synth slot in the SDRAM block (3 x 512 B).
SLOT_INSERT_SDRAM = 3 * 512


def slot_inserts(prof, mods):
    return bool(prof.get("slot_inserts", True)) and any(m["man"].get("effects") for m in mods)


def core_sdram(prof, mods):
    return SLOT_INSERT_SDRAM if slot_inserts(prof, mods) else 0


def budget_check(prof, mods):
    """[(what, used, limit, per-module sizes)] for every limit the profile sets,
    and the failure messages (with the module to leave out)."""
    rows, errors = [], []
    checks = [("flash", "flash_modules_max", lambda m: int(m["man"].get("flash_max_bytes", 0))),
              ("SRAM", "sram_modules_max", sram_of),
              ("SDRAM", "sdram_modules_max", lambda m: int(m["man"].get("sdram_bytes", 0)))]
    for what, key, size in checks:
        used = sum(size(m) for m in mods) + (core_sdram(prof, mods) if what == "SDRAM" else 0)
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
    prof, mods = load(args.profile, args.modules_dir, host=False)
    print(f"profile {prof['name']} ({prof['variant']}, \"{prof['display_name']}\"): {len(mods)} modules")
    print(f"  {'id':4s} {'flash_max':>9s} {'sram':>6s} {'fastrun':>7s} {'sdram':>7s} {'cpu/unit':>8s}")
    for m in mods:
        g = m["man"].get
        print(f"  {m['id']:4s} {int(g('flash_max_bytes', 0)):9d} {int(g('sram_audio_bytes', 0)):6d} "
              f"{int(g('fastrun_bytes', 0)):7d} {int(g('sdram_bytes', 0)):7d} {int(g('cpu_insn_per_unit', 0)):8d}")
    if core_sdram(prof, mods):
        print(f"  core slot inserts: {core_sdram(prof, mods)} B SDRAM")
    rows, errors = budget_check(prof, mods)
    for what, used, limit in rows:
        print(f"  {what:5s} {used:7d} B declared" + (f" of {limit} B ({limit - used:+d} B)" if limit is not None
                                                     else " (no limit in the profile)"))
    for e in errors:
        print(f"profile: error: {e}", file=sys.stderr)
    if errors:
        sys.exit(1)


def hot_pool_bytes(prof, profile_path):
    """[I] hot_pool_bytes: the shared fast pool's block (a multiple of 8). The
    room the profile's built-in synths' .fastrun code took without the pool,
    so the pool costs the stack nothing (13696 = full's)."""
    n = prof.get("hot_pool_bytes", 13696)
    if not isinstance(n, int) or n <= 0 or n % 8:
        fail(f"{profile_path}: hot_pool_bytes must be a positive multiple of 8")
    return n


def card_room(n):
    """[I] Card file / card effect table sizes: n plus room for more, in steps
    of 8 (0 -> 8, 16 -> 24): the loader's tables are sized by the profile."""
    return (n // 8 + 1) * 8


def gen(args):
    prof, mods = load(args.profile, args.modules_dir, host=args.host)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    rev = repo_rev(args.modules_dir) if mods else "none"
    sums = {k: sum(int(m["man"].get(k, 0)) for m in mods) for k in BUDGETS}
    _, errors = budget_check(prof, [m for m in mods if not m["card"]])
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
        # [I] the shared fast pool's SRAM_L block (make OT_HOT_POOL=1)
        f"OT_HOT_POOL_BYTES := {hot_pool_bytes(prof, args.profile)}",
        f"MOD_IDS := {' '.join(ids)}",
        f"MOD_SRC_DIRS := {' '.join(src_dirs)}",
        f"MOD_TEST_DIRS := {' '.join(test_dirs)}",
        f"MOD_SRCS := {' '.join(os.path.basename(p) for m in mods for p in m['srcs'])} module_registry.cpp",
        f"MOD_SRC_PATHS := {' '.join(p for m in mods for p in m['srcs'])} {reg}",
        f"MOD_HDRS := {' '.join(os.path.join(d, '*.h') for d in src_dirs)}",
        f"MOD_TEST_SRCS := {' '.join(os.path.basename(p) for m in mods for p in m['tests'])}",
        f"MOD_BENCH_PATHS := {' '.join(p for m in mods for p in m['benches'])} {os.path.join(out, 'module_bench.cpp')}",
        f"MOD_INCS := -I{out} {' '.join('-I' + d for d in src_dirs)}",
        # [I] The card modules' directories (make card), in profile order.
        f"MOD_CARD_DIRS := {' '.join(m['dir'] for m in prof['card'])}",
    ]
    for m in mods:
        mk.append(f"MOD_OBJ_NAMES_{m['id']} := "
                  + " ".join(os.path.basename(p) + ".o" for p in m["srcs"]))
        # [I] hot rows (make OT_HOT_POOL=1): the module's directory and entry point.
        mk.append(f"MOD_DIR_{m['id']} := {m['dir']}")
        mk.append(f"MOD_ENTRY_{m['id']} := {m['man']['entry']}")
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
        f"#define OT_PROFILE_SDRAM_BYTES {sums['sdram_bytes'] + core_sdram(prof, mods)}",
        f"#define OT_SLOT_INSERTS {1 if slot_inserts(prof, mods) else 0}",
        # [I] modules/module_files.h: Modules/*.otm files and card effects listed
        f"#define OT_PROFILE_CARD_FILES {card_room(len(prof['card']))}",
        f"#define OT_PROFILE_CARD_FX "
        f"{card_room(sum(1 for m in prof['card'] if 'synthEngine' not in m['man'].get('caps', [])))}",
        f'#define OT_MODULES_REV "{rev}"',
    ] + [f"#define OT_MOD_{i} 1" for i in ids]
    changed |= write_if_changed(os.path.join(out, "ot_profile.h"), "\n".join(h) + "\n")

    r = ["// Generated by tools/profile.py -- do not edit.",
         '#include "modules/module.h"', "", "namespace ot {", "namespace mod {", ""]
    r += [f"IModule& {m['man']['entry']}();" for m in mods]
    r += ["", "const ModuleFactory kModuleFactories[] = {"
          + "".join(f"&{m['man']['entry']}, " for m in mods) + "nullptr};",
          # [I] The card modules among them (host builds compile them in).
          "const char* const kCardModuleIds[] = {"
          + "".join(f'"{m["id"]}", ' for m in mods if m["card"]) + "nullptr};",
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
    _, mods = load(args.profile, args.modules_dir, host=False)
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
            p.add_argument("--host", action="store_true", help="[I] compile the card modules in (host builds)")
        elif name == "sizes":
            p.add_argument("--size-output", required=True)
    args = ap.parse_args()
    {"gen": gen, "sizes": sizes, "budget": budget}[args.cmd](args)


if __name__ == "__main__":
    main()
