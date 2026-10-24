#!/usr/bin/env python3
"""Memory use of two firmware images side by side, as a Markdown table for a
CI job summary:

  tools/size_report.py BASE.elf HEAD.elf [--size-tool arm-none-eabi-size] [--nm-tool ...]

Rows: flash (.text + .data, as telemetry_check.py size), the SRAM sections
(.data with the SRAM code .fastrun, .bss, .dma, USB buffers), .sdram_bss, and
the stack headroom _estack - _ebss with its limit (telemetry_check.STACK_MIN),
then the free space: flash to its limit, SRAM over the stack rule and, when
the image has the symbols, the .sdram_bss region (_sdram_bss_limit) and the
rest of the SDRAM (the app layout's end otSdramLayoutEnd). Prints only; the
limits are
checked by telemetry_check.py size and the linker.
"""
import argparse
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from telemetry_check import FLASH_FAIL, FLASH_WARN, STACK_MIN  # noqa: E402

SRAM_SECTIONS = [".data", ".bss", ".dma", ".usbdescriptortable", ".dmabuffers", ".usbbuffers"]
SDRAM_TOP = 0x70000000 + 16 * 1024 * 1024   # board::kSdramBase + kSdramSize


def read_word(elf, addr):
    """The 32-bit little-endian word at `addr` in an ELF32 file's loaded sections, or None."""
    with open(elf, "rb") as f:
        data = f.read()
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    for i in range(shnum):
        _, kind, _, sh_addr, sh_off, sh_size = struct.unpack_from("<IIIIII", data, shoff + i * shentsize)
        if kind != 8 and sh_addr <= addr and addr + 4 <= sh_addr + sh_size:   # 8 = SHT_NOBITS
            return struct.unpack_from("<I", data, sh_off + addr - sh_addr)[0]
    return None


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise OSError(f"{cmd[0]} failed: {r.stderr.strip()}")
    return r.stdout


def measure(elf, size_tool, nm_tool):
    """ELF -> {row name: bytes}."""
    sections = {}
    for line in run([size_tool, "-A", elf]).splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith(".") and parts[1].isdigit():
            sections[parts[0]] = int(parts[1])
    berkeley = run([size_tool, elf]).splitlines()[1].split()
    syms = {}
    for line in run([nm_tool, elf]).splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    m = {
        "Flash (.text + .data)": int(berkeley[0]) + int(berkeley[1]),
        "SRAM total": sum(sections.get(s, 0) for s in SRAM_SECTIONS),
        "  .data (incl. .fastrun code)": sections.get(".data", 0),
        "  .fastrun code in SRAM": syms.get("__fastrun_end", 0) - syms.get("__fastrun_start", 0),
        "  .bss": sections.get(".bss", 0),
        "  .dma + USB buffers": sum(sections.get(s, 0) for s in SRAM_SECTIONS[2:]),
        "SDRAM .sdram_bss": sections.get(".sdram_bss", 0),
    }
    if "_estack" in syms and "_ebss" in syms:
        m["Stack headroom (_estack - _ebss)"] = syms["_estack"] - syms["_ebss"]
    # Free space. Older images lack the two SDRAM symbols: their rows show "–".
    m[f"Flash free (to {FLASH_FAIL:,} B)"] = FLASH_FAIL - m["Flash (.text + .data)"]
    if "Stack headroom (_estack - _ebss)" in m:
        m[f"SRAM free (headroom over the {STACK_MIN:,} B stack rule)"] = m["Stack headroom (_estack - _ebss)"] - STACK_MIN
    if "_sdram_bss_limit" in syms and "_esdram_bss" in syms:
        m["SDRAM free in .sdram_bss"] = syms["_sdram_bss_limit"] - syms["_esdram_bss"]
        layout_end = read_word(elf, syms["otSdramLayoutEnd"]) if "otSdramLayoutEnd" in syms else None
        if layout_end is not None and "_ssdram_bss" in syms:
            m["SDRAM free outside it (layout gap + above)"] = (
                syms["_ssdram_bss"] - layout_end + SDRAM_TOP - syms["_sdram_bss_limit"])
    return m


def table(base, head):
    out = ["| | base | this PR | change |", "|---|---:|---:|---:|"]
    for key in head:
        b, h = base.get(key), head[key]
        if b is None:
            out.append(f"| {key.strip()} | – | {h:,} B | |")
            continue
        d = h - b
        change = "0" if d == 0 else f"**{d:+,} B**"
        name = key.strip() if not key.startswith("  ") else "&nbsp;&nbsp;" + key.strip()
        out.append(f"| {name} | {b:,} B | {h:,} B | {change} |")
    return out


def notes(head):
    out = []
    flash = head["Flash (.text + .data)"]
    if flash > FLASH_FAIL:
        out.append(f"- :x: flash {flash:,} B is over the {FLASH_FAIL:,} B limit")
    elif flash > FLASH_WARN:
        out.append(f"- flash {flash:,} B: {FLASH_FAIL - flash:,} B left before the {FLASH_FAIL:,} B limit")
    room = head.get("Stack headroom (_estack - _ebss)")
    if room is not None:
        mark = ":x: " if room < STACK_MIN else ""
        out.append(f"- {mark}stack headroom {room:,} B: {room - STACK_MIN:+,} B against the {STACK_MIN:,} B limit")
    free = head.get("SDRAM free in .sdram_bss")
    if free is not None:
        out.append(f"- .sdram_bss {head['SDRAM .sdram_bss']:,} B: {free:,} B left in its region")
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base")
    ap.add_argument("head")
    ap.add_argument("--size-tool", default="arm-none-eabi-size")
    ap.add_argument("--nm-tool", default=None)
    ap.add_argument("--title", default="Firmware memory")
    a = ap.parse_args(argv)
    nm = a.nm_tool or (a.size_tool[:-4] + "nm" if a.size_tool.endswith("size") else "nm")
    try:
        base = measure(a.base, a.size_tool, nm)
        head = measure(a.head, a.size_tool, nm)
    except (OSError, IndexError, ValueError) as e:
        print(f"size_report: {e}", file=sys.stderr)
        return 2
    print(f"### {a.title}\n")
    print("\n".join(table(base, head)))
    n = notes(head)
    if n:
        print()
        print("\n".join(n))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
