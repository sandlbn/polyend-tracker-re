#!/usr/bin/env python3
"""[I] Build a module into a loadable .otm file.

  make_otm.py build --module DIR --obj-dir DIR --out FILE.otm --cxx CXX --cxxflags "FLAGS"
                    [--exports src/core/modules/module_exports.txt] [--hot FILE]
                    [--reference BASE --ref-out FILE.bin [--ref-fast FBASE --ref-fast-out FILE.bin]]
      Compiles the module's sources (DIR/module.toml) with the firmware's flags,
      links them with a stub per imported core function and writes the .otm
      (src/core/modules/module_loader.h has the format). --hot names the
      functions of the fast part (mangled, one per line, '#' comments; written
      by tools/synth_hot.py --emit-hot): linked at 0, the rest at kOtmMainLink;
      without it everything is the main part, linked at 0. --reference also
      links the module at BASE (the fast part at FBASE) with export k at
      fake_export(k) and writes the memory image (main part + zeroed bss; the
      fast part) that otmLoad must produce for those bases:
      tests/module_loader_test.cpp compares them.
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

MAGIC = 0x324D544F          # "OTM2"
HEADER_BYTES = 52
MAIN_LINK = 0x00400000      # module_loader.h kOtmMainLink (main part's link address with a fast part)
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
    names = []
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                names.append(line)
    return names


def hot_patterns(objs, names):
    """Linker input-section patterns for the fast part: the section holding
    each named function (-ffunction-sections: .text.<name>; .fastrun code is
    one section per file and moves as a whole)."""
    want, found, pats = set(names), set(), []
    for o in objs:
        with open(o, "rb") as f:
            elf = ELFFile(f)
            st = elf.get_section_by_name(".symtab")
            for sym in st.iter_symbols():
                if sym.name not in want or sym["st_info"]["type"] != "STT_FUNC" or not isinstance(sym["st_shndx"], int):
                    continue
                sec = elf.get_section(sym["st_shndx"]).name
                pat = f"*(.text.{sym.name})" if sec == f".text.{sym.name}" else f"*/{os.path.basename(o)}({sec})"
                if pat not in pats:
                    pats.append(pat)
                found.add(sym.name)
    missing = [n for n in names if n not in found]
    return pats, missing


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

        fast = []
        if args.hot:
            fast, missing = hot_patterns(objs, read_hot(args.hot))
            if missing:
                print(f"make_otm: {m['id']}: {len(missing)} hot functions not in the module (inlined or renamed): "
                      f"{' '.join(missing[:6])}{' ...' if len(missing) > 6 else ''}", file=sys.stderr)
        main_link = MAIN_LINK if fast else 0
        elf0 = os.path.join(tmp, "m0.elf")
        link(ld, allobjs, entry, funcs, data_imports, 0, main_link, fast, lambda s: 0, elf0)
        image, fast_bytes, bss, align, entry_off, fixups, nexp = read_linked(elf0, entry, index, main_link)
        if fast and not fast_bytes:
            fast, main_link = [], 0   # none of the hot functions made it: one part, as without --hot
            link(ld, allobjs, entry, funcs, data_imports, 0, 0, [], lambda s: 0, elf0)
            image, fast_bytes, bss, align, entry_off, fixups, nexp = read_linked(elf0, entry, index, 0)
        layout, abiflags = abi_hashes(args.cxx, args.cxxflags)
        hdr = struct.pack("<IHHII8sIIIIIII", MAGIC, HEADER_BYTES, m.get("api", CORE_API), layout, abiflags,
                          m["id"].encode().ljust(8, b"\0"), len(image), bss, align, entry_off, len(fixups), nexp,
                          fast_bytes)
        body = hdr + image + b"".join(struct.pack("<I", x) for x in fixups)
        with open(args.out, "wb") as f:
            f.write(body + struct.pack("<I", zlib.crc32(body)))
        print(f"{m['id']}: {args.out}: image {len(image)} B (fast part {fast_bytes} B) + bss {bss} B, "
              f"{len(fixups)} fixups, imports {' '.join(imp_names) or '-'}")
        if args.reference is not None:
            base = int(args.reference, 0)
            fbase = int(args.ref_fast, 0) if args.ref_fast else 0
            elfr = os.path.join(tmp, "mref.elf")
            link(ld, allobjs, entry, funcs, data_imports, fbase, base, fast, lambda s: fake_export(index[s]), elfr)
            with open(elfr, "rb") as f:
                e = ELFFile(f)
                o = e.get_section_by_name(".otm")
                img = o.data()
                b = e.get_section_by_name(".otm_bss")
                nb = b["sh_size"] if b else 0
                # Memory as the linker laid it out, from base to the bss end
                # (a gap before the bss shows up as a size mismatch).
                gap = b["sh_addr"] - (o["sh_addr"] + len(img)) if b else 0
                fs = e.get_section_by_name(".otm_fast")
                fimg = fs.data() if fs is not None and fs["sh_type"] != "SHT_NOBITS" else b""
            with open(args.ref_out, "wb") as f:
                f.write(img + bytes(gap + nb))
            if args.ref_fast_out:
                with open(args.ref_fast_out, "wb") as f:
                    f.write(fimg + bytes(-len(fimg) % 4))


def link(ld, objs, entry, funcs, data_imports, fast_base, base, fast, value, out):
    """Links the module: the fast part (input-section patterns `fast`) at
    fast_base, the rest at base; stub words __imp_<f> and data imports at value(s)."""
    script = out + ".ld"
    with open(script, "w") as f:
        for s in funcs:
            f.write(f"__imp_{s} = {value(s):#x};\n")
        for s in data_imports:
            f.write(f"{s} = {value(s):#x};\n")
        fast_sec = f"  .otm_fast {fast_base:#x} : {{ {' '.join(fast)} }}\n" if fast else ""
        f.write(f"""SECTIONS {{
{fast_sec}  .otm {base:#x} : {{ *(.text.otm_stubs) *(.text .text.* .fastrun .fastrun.*) *(.rodata .rodata.*) *(.data .data.*) . = ALIGN(8); }}
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
    """(image, fast bytes, bss bytes, align, entry offset, fixups, exports needed)
    of a link with the fast part at 0 and the main part at main_link."""
    ELFFile, _ = elftools()
    with open(path, "rb") as f:
        elf = ELFFile(f)
        fs = elf.get_section_by_name(".otm_fast")
        fimg = fs.data() if fs is not None else b""
        fimg += bytes(-len(fimg) % 4)
        fast_end = len(fimg)
        otm = elf.get_section_by_name(".otm")
        mimg = otm.data()
        if otm["sh_addr"] != main_link:
            fail(f".otm linked at {otm['sh_addr']:#x}, not {main_link:#x}")
        image = fimg + mimg
        b = elf.get_section_by_name(".otm_bss")
        bss = b["sh_size"] if b else 0
        # The loader zeroes the bss right after the image (otmLoad), and the
        # next module goes after image + bss: no gap between the two.
        if b and b["sh_addr"] != main_link + len(mimg):
            fail(f".otm_bss at {b['sh_addr']:#x}, not right after the {len(mimg)} B main part")
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
                    if tp[0] != p_fast:
                        fixups.append(off | FIX_BRANCH << 24)   # crosses the parts
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
    return image, fast_end, bss, align, entry_off, sorted(fixups), nexp


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
