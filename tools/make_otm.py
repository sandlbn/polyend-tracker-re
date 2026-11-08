#!/usr/bin/env python3
"""[I] Build a module into a loadable .otm file.

  make_otm.py build --module DIR --obj-dir DIR --out FILE.otm --cxx CXX --cxxflags "FLAGS"
                    [--exports src/core/modules/module_exports.txt] [--hot FILE]
                    [--reference BASE --ref-out FILE.bin
                     [--ref-fast FBASE --ref-fast-out FILE.bin --ref-cut BYTES]]
      Compiles the module's sources (DIR/module.toml) with the firmware's flags,
      links them with a stub per imported core function and writes the .otm
      (src/core/modules/module_loader.h has the format). --hot names the
      functions of the row, the code the loader may put in the fast block
      (mangled, one per line with an optional weight, hottest first, '#'
      comments; written by tools/synth_hot.py --emit-hot): linked at 0 in that
      order, a cut after each, the rest at kOtmMainLink; without it everything
      is the main part, linked at 0. --reference also links the module at BASE
      (the row's first --ref-cut bytes at FBASE, its tail after the bss) with
      export k at fake_export(k) and writes the memory images (main part +
      zeroed bss + tail; the fast part) that otmLoad must produce for those
      bases and that cut: tests/module_loader_test.cpp compares them.
  make_otm.py abi-header --cxx CXX --cxxflags "FLAGS" --out FILE.h
      The core's side of the versioning (layout hash, target flags hash) as a
      header for the device build.
  make_otm.py exports-cpp --out FILE.cpp
      The core's export table (otmExports, module_exports.txt order).

Refused: static constructors (.init_array), relocation types other than
R_ARM_ABS32 / R_ARM_THM_CALL / R_ARM_THM_JUMP24 / R_ARM_NONE, and core symbols
missing from module_exports.txt (the error names them).
"""
import argparse
import bisect
import os
import re
import shlex
import struct
import subprocess
import sys
import tempfile
import tomllib
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXPORTS = os.path.join(ROOT, "src/core/modules/module_exports.txt")

MAGIC = 0x334D544F          # "OTM3"
HEADER_BYTES = 56
MAIN_LINK = 0x00400000      # module_loader.h kOtmMainLink (main part's link address with a row)
FIX_BRANCH, FIX_FAST = 254, 255
MAX_EXPORTS = 253
CORE_API = 1                # modules/module.h kModuleApi

# The headers module code may include (modules/module.h): their preprocessed
# text is the interface, inline helpers and struct layouts included.
ABI_HEADERS = [
    "modules/module.h", "modules/module_ui.h", "modules/synth_descriptor.h",
    "audio/synth_engine.h", "audio/inserts.h", "audio/dsp.h", "audio/bbd_chorus.h",
    "audio/va_dsp.h", "audio/chip_dsp.h", "audio/fm_dsp.h", "audio/ring_io.h", "fastrun.h",
]

R_ARM_NONE, R_ARM_ABS32, R_ARM_THM_CALL, R_ARM_THM_JUMP24 = 0, 2, 10, 30


def fail(msg):
    print(f"make_otm: {msg}", file=sys.stderr)
    sys.exit(1)


def elftools():
    """pyelftools, needed by build only (abi-header and exports-cpp run in every firmware build)."""
    try:
        from elftools.elf.elffile import ELFFile
        from elftools.elf.relocation import RelocationSection
    except ImportError:
        fail("building a module needs pyelftools: python3 -m pip install pyelftools "
             "(system-wide: make runs this tool with python3 -I, which skips the user site)")
    return ELFFile, RelocationSection


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        fail(f"{' '.join(cmd[:3])} ... failed:\n{r.stderr}")
    return r.stdout


def tool(cxx, name):
    """arm-none-eabi-g++ -> arm-none-eabi-<name>."""
    return re.sub(r"g\+\+$", name, cxx)


def fake_export(k):
    """Address of export k (1-based) in --reference links and the host test."""
    return 0x0E000000 + 16 * k + 1


def read_exports(path):
    syms = []
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                syms.append(line)
    if len(syms) > MAX_EXPORTS:
        fail(f"more than {MAX_EXPORTS} exports")
    return syms


def target_flags(cxxflags):
    """The flags that change the code's ABI: CPU, FPU, float ABI, C++ dialect."""
    keep = [t for t in shlex.split(cxxflags)
            if t.startswith(("-mcpu", "-mthumb", "-mfloat-abi", "-mfpu", "-std=", "-fshort", "-fno-rtti",
                             "-fno-exceptions", "-fpack-struct"))]
    return " ".join(keep)


def abi_hashes(cxx, cxxflags):
    """(layout hash, target flags hash) of this core (see ABI_HEADERS)."""
    src = "".join(f'#include "{h}"\n' for h in ABI_HEADERS)
    flags = [t for t in shlex.split(cxxflags) if t not in ("-MMD", "-MD", "-g") and not t.startswith("-fstack-usage")]
    r = subprocess.run([cxx, *flags, "-E", "-P", "-x", "c++", "-"], input=src, capture_output=True, text=True)
    if r.returncode != 0:
        fail(f"preprocessing the module headers failed:\n{r.stderr}")
    text = " ".join(r.stdout.split())   # whitespace and line breaks don't matter
    return zlib.crc32(text.encode()), zlib.crc32(target_flags(cxxflags).encode())


def mangle_entry(name):
    # ot::mod::<name>() returning IModule& (modules/module.h ModuleFactory).
    return f"_ZN2ot3mod{len(name)}{name}Ev"


def compile_module(mdir, obj_dir, cxx, cxxflags):
    with open(os.path.join(mdir, "module.toml"), "rb") as f:
        m = tomllib.load(f)
    src_dir = os.path.join(mdir, "src")
    os.makedirs(obj_dir, exist_ok=True)
    objs = []
    for s in m["sources"]:
        o = os.path.join(obj_dir, s + ".o")
        run([cxx, *shlex.split(cxxflags), f"-I{src_dir}", "-c", os.path.join(src_dir, s), "-o", o])
        objs.append(o)
    return m, objs


def undefined_symbols(ld, objs, entry, tmp):
    ELFFile, _ = elftools()
    r = os.path.join(tmp, "partial.o")
    run([ld, "-r", "--gc-sections", "-e", entry, *objs, "-o", r])
    with open(r, "rb") as f:
        elf = ELFFile(f)
        if any(s.name.startswith((".init_array", ".ctors", ".preinit_array")) for s in elf.iter_sections()):
            fail("the module has static constructors (.init_array): use constant initialisation")
        st = elf.get_section_by_name(".symtab")
        return sorted({s.name for s in st.iter_symbols() if s["st_shndx"] == "SHN_UNDEF" and s.name})


def read_hot(path):
    """[(name, weight)] in row order (weight 0 when the line has none)."""
    rows = []
    with open(path) as f:
        for line in f:
            p = line.split("#", 1)[0].split()
            if p:
                rows.append((p[0], float(p[1]) if len(p) > 1 else 0.0))
    return rows


def hot_patterns(objs, rows):
    """The row: [(linker input-section pattern, weight)] in --hot order, one
    per section holding a named function (-ffunction-sections: .text.<name>;
    .fastrun code is one section per file and moves as a whole, at its first
    function's place with the weights summed). Sections aligned to more than
    8 stay in the main part: every cut is a multiple of 8."""
    where, base = {}, {}   # name -> pattern; base name -> its patterns (see clone_base)
    for o in objs:
        with open(o, "rb") as f:
            elf = ELFFile(f)
            st = elf.get_section_by_name(".symtab")
            for sym in st.iter_symbols():
                if sym["st_info"]["type"] != "STT_FUNC" or not isinstance(sym["st_shndx"], int):
                    continue
                sec = elf.get_section(sym["st_shndx"])
                if sec["sh_addralign"] > 8:
                    continue
                n = sec.name
                pat = f"*(.text.{sym.name})" if n == f".text.{sym.name}" else f"*/{os.path.basename(o)}({n})"
                where[sym.name] = pat
                if not re.search(r"\.cold(\.\d+)?$", sym.name):
                    base.setdefault(clone_base(sym.name), []).append(pat)
    pats, weight, missing = [], {}, []
    for name, w in rows:
        # The same function under another clone name (the bench and the module
        # build inline and split differently): every clone of it, weight shared.
        found = [where[name]] if name in where else base.get(clone_base(name), [])
        if not found:
            missing.append(name)
            continue
        for pat in found:
            if pat not in weight:
                pats.append(pat)
                weight[pat] = 0.0
            weight[pat] += w / len(found)
    return [(p, weight[p]) for p in pats], missing


def clone_base(name):
    """A function's name without GCC's clone suffixes (.part.0, .isra.0, .constprop.1, ...)."""
    return re.sub(r"(\.(part|isra|constprop|lto_priv|cold)\.\d+)+$", "", name)


def write_stubs(path, funcs):
    lines = ["    .syntax unified", "    .thumb", '    .section .text.otm_stubs,"ax",%progbits', "    .balign 4"]
    for s in funcs:
        lines += [f"    .global {s}", "    .thumb_func", f"{s}: ldr.w pc, [pc, #0]", f"    .word __imp_{s}"]
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def build(args):
    ELFFile, RelocationSection = elftools()
    exports = read_exports(args.exports)
    index = {s: k + 1 for k, s in enumerate(exports)}
    ld = tool(args.cxx, "ld")
    mdir = os.path.abspath(args.module)
    m, objs = compile_module(mdir, args.obj_dir, args.cxx, args.cxxflags)
    entry = mangle_entry(m["entry"])
    with tempfile.TemporaryDirectory() as tmp:
        undef = undefined_symbols(ld, objs, entry, tmp)
        missing = [s for s in undef if s not in index]
        if missing:
            fail(f"{m['id']} needs core symbols not in module_exports.txt: {' '.join(missing)}")
        # Which imports are called (need a stub) and which are only addressed (data).
        called = set()
        partial = os.path.join(tmp, "partial.o")
        with open(partial, "rb") as f:
            elf = ELFFile(f)
            st = elf.get_section_by_name(".symtab")
            for sec in elf.iter_sections():
                if not isinstance(sec, RelocationSection) or ".debug" in sec.name:
                    continue
                for r in sec.iter_relocations():
                    if r["r_info_type"] in (R_ARM_THM_CALL, R_ARM_THM_JUMP24):
                        n = st.get_symbol(r["r_info_sym"]).name
                        if n in index:
                            called.add(n)
        funcs = sorted(called)
        stubs_s = os.path.join(tmp, "stubs.S")
        write_stubs(stubs_s, funcs)
        stubs_o = os.path.join(tmp, "stubs.o")
        run([tool(args.cxx, "as"), "-mcpu=cortex-m4", "-mthumb", stubs_s, "-o", stubs_o])
        allobjs = objs + [stubs_o]
        # Imports: a called symbol resolves to its stub, whose word is __imp_<sym>;
        # a data symbol keeps its own name, defined absolute in the link script.
        data_imports = [s for s in undef if s not in called]
        imp_names = funcs + data_imports

        row = []
        if args.hot:
            row, missing = hot_patterns(objs, read_hot(args.hot))
            if missing:
                print(f"make_otm: {m['id']}: {len(missing)} hot functions not in the module (inlined or renamed): "
                      f"{' '.join(missing[:6])}{' ...' if len(missing) > 6 else ''}", file=sys.stderr)
        main_link = MAIN_LINK if row else 0
        elf0 = os.path.join(tmp, "m0.elf")
        link(ld, allobjs, entry, funcs, data_imports, 0, main_link, row, None, lambda s: 0, elf0)
        image, row_bytes, cuts, bss, align, entry_off, fixups, nexp = read_linked(elf0, entry, index, main_link)
        if row and not row_bytes:
            row, main_link = [], 0   # none of the hot functions made it: one part, as without --hot
            link(ld, allobjs, entry, funcs, data_imports, 0, 0, [], None, lambda s: 0, elf0)
            image, row_bytes, cuts, bss, align, entry_off, fixups, nexp = read_linked(elf0, entry, index, 0)
        # Each cut with the weight of the row up to it (millionths).
        acc, cut_words = 0.0, []
        for (_, w), end in zip(row, cuts):
            acc += w
            cut_words += [end, min(int(round(acc * 1e6)), 1000000)]
        layout, abiflags = abi_hashes(args.cxx, args.cxxflags)
        hdr = struct.pack("<IHHII8sIIIIIIII", MAGIC, HEADER_BYTES, m.get("api", CORE_API), layout, abiflags,
                          m["id"].encode().ljust(8, b"\0"), len(image), bss, align, entry_off, len(fixups), nexp,
                          row_bytes, len(cuts))
        body = (hdr + image + b"".join(struct.pack("<I", x) for x in fixups)
                + b"".join(struct.pack("<I", x) for x in cut_words))
        with open(args.out, "wb") as f:
            f.write(body + struct.pack("<I", zlib.crc32(body)))
        print(f"{m['id']}: {args.out}: image {len(image)} B (row {row_bytes} B in {len(cuts)} cuts) + bss {bss} B, "
              f"{len(fixups)} fixups, imports {' '.join(imp_names) or '-'}")
        if args.reference is not None:
            base = int(args.reference, 0)
            fbase = int(args.ref_fast, 0) if args.ref_fast else 0
            cut = int(args.ref_cut, 0) if args.ref_cut else 0
            if cut and cut not in cuts:
                fail(f"--ref-cut {cut} is not a cut ({' '.join(map(str, cuts))})")
            n_fast = cuts.index(cut) + 1 if cut else 0
            elfr = os.path.join(tmp, "mref.elf")
            # module_loader.h otmTailOffset: after the main part and its bss, at a multiple of 8.
            tail_base = base + ((len(image) - row_bytes + bss + 7) & ~7)
            link(ld, allobjs, entry, funcs, data_imports, fbase, base, row[:n_fast], row[n_fast:],
                 lambda s: fake_export(index[s]), elfr, tail_base)
            with open(elfr, "rb") as f:
                e = ELFFile(f)
                o = e.get_section_by_name(".otm")
                img = o.data()
                b = e.get_section_by_name(".otm_bss")
                nb = b["sh_addr"] + b["sh_size"] - (base + len(img)) if b and b["sh_size"] else 0
                fs = e.get_section_by_name(".otm_fast")
                fimg = fs.data() if fs is not None and fs["sh_type"] != "SHT_NOBITS" else b""
                ts = e.get_section_by_name(".otm_tail")
                timg = ts.data() if ts is not None and ts["sh_type"] != "SHT_NOBITS" else b""
                if timg and ts["sh_addr"] != base + ((len(img) + nb + 7) & ~7):
                    fail(f"the tail was linked at {ts['sh_addr']:#x}, not after the bss")
            main = img + bytes(nb)
            if timg:
                main += bytes(-len(main) % 8) + timg
            with open(args.ref_out, "wb") as f:
                f.write(main)
            if args.ref_fast_out:
                with open(args.ref_fast_out, "wb") as f:
                    f.write(fimg + bytes(-len(fimg) % 8))


def row_body(pieces, first=0):
    """A row's pieces, each at a multiple of 8, __otm_cut_<k> after each."""
    return " ".join(f". = ALIGN(8); {p} . = ALIGN(8); __otm_cut_{first + k} = .;" for k, (p, _) in enumerate(pieces))


def link(ld, objs, entry, funcs, data_imports, fast_base, base, fast, tail, value, out, tail_base=0):
    """Links the module: the row's pieces `fast` ([(input-section pattern,
    weight)]) at fast_base, the rest at base; the pieces `tail` (a --reference
    cut) at tail_base, after the bss; stub words __imp_<f> and data imports at
    value(s). The row's sections come first in the script: ld gives an input
    section to the first output section that names it."""
    script = out + ".ld"
    with open(script, "w") as f:
        for s in funcs:
            f.write(f"__imp_{s} = {value(s):#x};\n")
        for s in data_imports:
            f.write(f"{s} = {value(s):#x};\n")
        fast_sec = f"  .otm_fast {fast_base:#x} : {{ {row_body(fast)} }}\n" if fast else ""
        tail_sec = f"  .otm_tail {tail_base:#x} : {{ {row_body(tail, len(fast))} }}\n" if tail else ""
        f.write(f"""SECTIONS {{
{fast_sec}{tail_sec}  .otm {base:#x} : {{ *(.text.otm_stubs) *(.text .text.* .fastrun .fastrun.*) *(.rodata .rodata.*) *(.data .data.*) . = ALIGN(8); }}
  .otm_bss (NOLOAD) : ALIGN(8) {{ *(.bss .bss.* COMMON) }}
  /DISCARD/ : {{ *(.ARM.exidx* .ARM.extab* .comment .note*) }}
}}
""")
    run([ld, "-q", "--gc-sections", "--no-warn-rwx-segments", "-e", entry, "-T", script, *objs, "-o", out])


def branch_offset(img, off):
    """Displacement of the Thumb-2 BL / B.W at img[off] (module_loader.cpp branchOffset)."""
    a, b = struct.unpack_from("<HH", img, off)
    if (a & 0xF800) != 0xF000 or (b & 0xD000) not in (0xD000, 0x9000):
        return None
    s_, j1, j2 = (a >> 10) & 1, (b >> 13) & 1, (b >> 11) & 1
    i1, i2 = ~(j1 ^ s_) & 1, ~(j2 ^ s_) & 1
    v = s_ << 24 | i1 << 23 | i2 << 22 | (a & 0x3FF) << 12 | (b & 0x7FF) << 1
    return v - (1 << 25) if s_ else v


def read_linked(path, entry, index, main_link):
    """(image, row bytes, cuts, bss bytes, align, entry offset, fixups, exports
    needed) of a link with the row at 0 and the main part at main_link."""
    ELFFile, _ = elftools()
    with open(path, "rb") as f:
        elf = ELFFile(f)
        fs = elf.get_section_by_name(".otm_fast")
        fimg = fs.data() if fs is not None else b""
        fimg += bytes(-len(fimg) % 8)
        fast_end = len(fimg)
        otm = elf.get_section_by_name(".otm")
        mimg = otm.data()
        if otm["sh_addr"] != main_link:
            fail(f".otm linked at {otm['sh_addr']:#x}, not {main_link:#x}")
        image = fimg + mimg
        b = elf.get_section_by_name(".otm_bss")
        # From the end of .otm: the bss starts at the next multiple of 8.
        bss = b["sh_addr"] + b["sh_size"] - (main_link + len(mimg)) if b and b["sh_size"] else 0
        align = 8
        for sec in elf.iter_sections():
            if sec["sh_flags"] & 2 and sec["sh_addralign"] > align:   # SHF_ALLOC
                align = sec["sh_addralign"]

        def part(addr):   # (is fast, image offset) of a link address; None outside both parts
            if fast_end and addr < fast_end:
                return True, addr
            if main_link <= addr < main_link + len(mimg) + bss + 1:
                return False, fast_end + addr - main_link
            if fast_end and addr == fast_end:   # one past the fast part's end
                return True, addr
            return None

        st = elf.get_section_by_name(".symtab")
        cutsym = {int(s.name[len("__otm_cut_"):]): s["st_value"] for s in st.iter_symbols()
                  if s.name.startswith("__otm_cut_")}
        cuts = [cutsym[k] for k in sorted(cutsym)] if fast_end else []
        if cuts and (cuts[-1] != fast_end or any(c & 7 for c in cuts) or cuts != sorted(cuts)):
            fail(f"the row's cuts {cuts} don't end at the row's end {fast_end}")
        if fast_end and fast_end >= MAIN_LINK:
            fail("the row is bigger than the main part's link address")

        def segment(addr):   # which piece of the row an address is in (-1: the main part)
            return bisect.bisect_right(cuts, addr) if fast_end and addr < fast_end else -1

        ent = next(s for s in st.iter_symbols() if s.name == entry)["st_value"]
        ep = part(ent & ~1)
        if ep is None:
            fail("entry point outside the image")
        entry_off = ep[1] | (ent & 1)
        fixups, nexp = [], 0
        for rname in (".rel.otm_fast", ".rel.otm"):
            rel = elf.get_section_by_name(rname)
            for r in (rel.iter_relocations() if rel else []):
                t = r["r_info_type"]
                if t == R_ARM_NONE:
                    continue
                pl = part(r["r_offset"])
                if pl is None:
                    fail(f"relocation at {r['r_offset']:#x} outside the image")
                p_fast, off = pl
                if off + 4 > len(image) or off >= 1 << 24:
                    fail(f"relocation at {off:#x} outside the image")
                if t in (R_ARM_THM_CALL, R_ARM_THM_JUMP24):
                    s = st.get_symbol(r["r_info_sym"])
                    if s["st_shndx"] in ("SHN_ABS", "SHN_UNDEF"):
                        fail(f"a branch to {s.name} outside the image (no stub)")
                    d = branch_offset(image, off)
                    if d is None:
                        fail(f"relocation type {t} at {off:#x} is not a BL / B.W")
                    tp = part(r["r_offset"] + 4 + d)
                    if tp is None:
                        fail(f"a branch at {off:#x} lands outside the image")
                    # A branch out of its row piece may cross a cut: the loader
                    # decides. Inside one piece, or main to main, it moves with its code.
                    if segment(r["r_offset"]) != segment(r["r_offset"] + 4 + d):
                        fixups.append(off | FIX_BRANCH << 24)
                    continue
                if t != R_ARM_ABS32:
                    fail(f"relocation type {t} at {off:#x} is not supported")
                s = st.get_symbol(r["r_info_sym"])
                if s["st_shndx"] == "SHN_ABS":
                    name = s.name[len("__imp_"):] if s.name.startswith("__imp_") else s.name
                    if name not in index:
                        fail(f"absolute symbol {s.name} is not an export")
                    kind = index[name]
                    # The link wrote S + A with S = 0: the word is the addend.
                    nexp = max(nexp, kind)
                else:
                    word = struct.unpack_from("<I", image, off)[0]
                    wp = part(word & ~1)
                    if wp is None and fast_end:
                        fail(f"the word at {off:#x} ({word:#x}) points outside the image")
                    kind = FIX_FAST if wp and wp[0] else 0
                fixups.append(off | kind << 24)
    return image, fast_end, cuts, bss, align, entry_off, sorted(fixups), nexp


def abi_header(args):
    layout, abiflags = abi_hashes(args.cxx, args.cxxflags)
    exports = read_exports(args.exports)
    text = (
        "// Generated by tools/make_otm.py abi-header -- do not edit.\n"
        "#pragma once\n#include <cstdint>\n"
        f"constexpr uint32_t kOtmLayoutHash = {layout:#010x}u;\n"
        f"constexpr uint32_t kOtmAbiFlags = {abiflags:#010x}u;\n"
        f"constexpr uint32_t kOtmExportCount = {len(exports)}u;\n"
    )
    old = open(args.out).read() if os.path.exists(args.out) else None
    if old != text:
        os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
        with open(args.out, "w") as f:
            f.write(text)


def exports_cpp(args):
    """The core's export table: .word per symbol (C++ member functions included)."""
    syms = read_exports(args.exports)
    lines = ["// Generated by tools/make_otm.py exports-cpp -- do not edit (src/core/modules/module_exports.txt).",
             "// [I] Export k of a loadable module is otmExports[k - 1].",
             'asm(".section .rodata.otm_exports,\\"a\\",%progbits\\n"',
             '    ".balign 4\\n"',
             '    ".global otmExports\\n"',
             '    "otmExports:\\n"']
    lines += [f'    ".word {s}\\n"' for s in syms]
    lines += ['    ".text\\n");', ""]
    text = "\n".join(lines)
    old = open(args.out).read() if os.path.exists(args.out) else None
    if old != text:
        os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
        with open(args.out, "w") as f:
            f.write(text)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--module", required=True)
    b.add_argument("--obj-dir", required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--cxx", required=True)
    b.add_argument("--cxxflags", required=True)
    b.add_argument("--exports", default=EXPORTS)
    b.add_argument("--hot")
    b.add_argument("--reference")
    b.add_argument("--ref-out")
    b.add_argument("--ref-fast")
    b.add_argument("--ref-fast-out")
    b.add_argument("--ref-cut")
    a = sub.add_parser("abi-header")
    a.add_argument("--cxx", required=True)
    a.add_argument("--cxxflags", required=True)
    a.add_argument("--exports", default=EXPORTS)
    a.add_argument("--out", required=True)
    e = sub.add_parser("exports-cpp")
    e.add_argument("--exports", default=EXPORTS)
    e.add_argument("--out", required=True)
    args = p.parse_args()
    if args.cmd == "build":
        if (args.reference is None) != (args.ref_out is None):
            fail("--reference and --ref-out go together")
        build(args)
    elif args.cmd == "exports-cpp":
        exports_cpp(args)
    else:
        abi_header(args)


if __name__ == "__main__":
    main()
