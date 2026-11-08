#!/usr/bin/env python3
"""[I] Build a module into a loadable .otm file.

  make_otm.py build --module DIR --obj-dir DIR --out FILE.otm --cxx CXX --cxxflags "FLAGS"
                    [--exports src/core/modules/module_exports.txt]
                    [--reference BASE --ref-out FILE.bin]
      Compiles the module's sources (DIR/module.toml) with the firmware's flags,
      links them at address 0 with a stub per imported core function and writes
      the .otm (src/core/modules/module_loader.h has the format). --reference also
      links the module at BASE with export k at fake_export(k) and writes the
      memory image (image + zeroed bss) that otmLoad must produce for that base:
      tests/module_loader_test.cpp compares the two.
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

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXPORTS = os.path.join(ROOT, "src/core/modules/module_exports.txt")

MAGIC = 0x314D544F          # "OTM1"
HEADER_BYTES = 48
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
    if len(syms) > 255:
        fail("more than 255 exports")
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
    r = os.path.join(tmp, "partial.o")
    run([ld, "-r", "--gc-sections", "-e", entry, *objs, "-o", r])
    with open(r, "rb") as f:
        elf = ELFFile(f)
        if any(s.name.startswith((".init_array", ".ctors", ".preinit_array")) for s in elf.iter_sections()):
            fail("the module has static constructors (.init_array): use constant initialisation")
        st = elf.get_section_by_name(".symtab")
        return sorted({s.name for s in st.iter_symbols() if s["st_shndx"] == "SHN_UNDEF" and s.name})


def write_stubs(path, funcs):
    lines = ["    .syntax unified", "    .thumb", '    .section .text.otm_stubs,"ax",%progbits', "    .balign 4"]
    for s in funcs:
        lines += [f"    .global {s}", "    .thumb_func", f"{s}: ldr.w pc, [pc, #0]", f"    .word __imp_{s}"]
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def build(args):
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

        elf0 = os.path.join(tmp, "m0.elf")
        link(ld, allobjs, entry, funcs, data_imports, 0, lambda s: 0, elf0)
        image, bss, align, entry_off, fixups, nexp = read_linked(elf0, entry, index)
        layout, abiflags = abi_hashes(args.cxx, args.cxxflags)
        hdr = struct.pack("<IHHII8sIIIIII", MAGIC, HEADER_BYTES, m.get("api", CORE_API), layout, abiflags,
                          m["id"].encode().ljust(8, b"\0"), len(image), bss, align, entry_off, len(fixups), nexp)
        body = hdr + image + b"".join(struct.pack("<I", x) for x in fixups)
        with open(args.out, "wb") as f:
            f.write(body + struct.pack("<I", zlib.crc32(body)))
        print(f"{m['id']}: {args.out}: image {len(image)} B + bss {bss} B, {len(fixups)} fixups, "
              f"imports {' '.join(imp_names) or '-'}")
        if args.reference is not None:
            base = int(args.reference, 0)
            elfr = os.path.join(tmp, "mref.elf")
            link(ld, allobjs, entry, funcs, data_imports, base, lambda s: fake_export(index[s]), elfr)
            with open(elfr, "rb") as f:
                e = ELFFile(f)
                img = e.get_section_by_name(".otm").data()
                b = e.get_section_by_name(".otm_bss")
                nb = b["sh_size"] if b else 0
            with open(args.ref_out, "wb") as f:
                f.write(img + bytes(nb))


def link(ld, objs, entry, funcs, data_imports, base, value, out):
    """Links the module at base: stub words __imp_<f> and data imports at value(s)."""
    script = out + ".ld"
    with open(script, "w") as f:
        for s in funcs:
            f.write(f"__imp_{s} = {value(s):#x};\n")
        for s in data_imports:
            f.write(f"{s} = {value(s):#x};\n")
        f.write(f"""SECTIONS {{
  . = {base:#x};
  .otm : {{ *(.text.otm_stubs) *(.text .text.* .fastrun .fastrun.*) *(.rodata .rodata.*) *(.data .data.*) }}
  .otm_bss (NOLOAD) : ALIGN(8) {{ *(.bss .bss.* COMMON) }}
  /DISCARD/ : {{ *(.ARM.exidx* .ARM.extab* .comment .note*) }}
}}
""")
    run([ld, "-q", "--gc-sections", "--no-warn-rwx-segments", "-e", entry, "-T", script, *objs, "-o", out])


def read_linked(path, entry, index):
    """(image, bss bytes, align, entry offset, fixups, exports needed) of a link at 0."""
    with open(path, "rb") as f:
        elf = ELFFile(f)
        otm = elf.get_section_by_name(".otm")
        image = otm.data()
        b = elf.get_section_by_name(".otm_bss")
        bss = b["sh_size"] if b else 0
        align = 8
        for sec in elf.iter_sections():
            if sec["sh_flags"] & 2 and sec["sh_addralign"] > align:   # SHF_ALLOC
                align = sec["sh_addralign"]
        st = elf.get_section_by_name(".symtab")
        ent = next(s for s in st.iter_symbols() if s.name == entry)["st_value"]
        rel = elf.get_section_by_name(".rel.otm")
        fixups, nexp = [], 0
        for r in (rel.iter_relocations() if rel else []):
            t = r["r_info_type"]
            if t in (R_ARM_NONE, R_ARM_THM_CALL, R_ARM_THM_JUMP24):
                if t != R_ARM_NONE:
                    s = st.get_symbol(r["r_info_sym"])
                    if s["st_shndx"] in ("SHN_ABS", "SHN_UNDEF"):
                        fail(f"a branch to {s.name} outside the image (no stub)")
                continue
            if t != R_ARM_ABS32:
                fail(f"relocation type {t} at {r['r_offset']:#x} is not supported")
            off = r["r_offset"]
            if off + 4 > len(image) or off >= 1 << 24:
                fail(f"relocation at {off:#x} outside the image")
            s = st.get_symbol(r["r_info_sym"])
            if s["st_shndx"] == "SHN_ABS":
                name = s.name[len("__imp_"):] if s.name.startswith("__imp_") else s.name
                if name not in index:
                    fail(f"absolute symbol {s.name} is not an export")
                kind = index[name]
                # The link wrote S + A with S = 0: the word is the addend.
                nexp = max(nexp, kind)
            else:
                kind = 0
            fixups.append(off | kind << 24)
    return image, bss, align, ent, sorted(fixups), nexp


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
    b.add_argument("--reference")
    b.add_argument("--ref-out")
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
