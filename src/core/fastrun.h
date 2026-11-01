// OT_FASTRUN_FN: hot audio code that runs from SRAM_L instead of flash on the
// device (docs/OPTIMIZATION_2X.md item 6, build flag OT_FASTRUN, default 1).
//
// Flash code runs through the 8 KB LMEM code cache. A sample voice's render
// (Voice::render / renderSt + the player, ~7-10 KB with their callees) does
// not fit it, so every one of the eight track voices fetches its code from
// flash again in each block. The tagged functions are copied to SRAM_L with
// .data at boot (src/hal/opentracker.ld: `.fastrun`, __fastrun_start /
// __fastrun_end; the boot log prints the size). Calls between flash and
// SRAM_L go through linker veneers. Budget: the stack headroom stays
// >= 90 KiB (make perf-gate); round 3 made room by moving main-loop data
// to the SDRAM (sdram_bss.h) and holds ~35 KB of code. The pick, its
// measurement and the boot line's areas are in docs/OPTIMIZATION_2X.md
// ("CPU round 3"). Host builds, benches and OT_FASTRUN=0 leave the code in
// .text. Never tag memcpy (the startup code copies .data with it) or code
// that runs before the .data copy.
#pragma once

// OT_FASTRUN_INLINE(tag): the same for an inline / template function with
// external linkage (a COMDAT group): it needs a section of its own
// (".fastrun.<tag>"), or GCC reports a section type conflict with the plain
// .fastrun functions of the same translation unit.
#if defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE && defined(OT_FASTRUN) && OT_FASTRUN
#define OT_FASTRUN_FN __attribute__((section(".fastrun"), noinline, noclone))
#define OT_FASTRUN_INLINE(tag) __attribute__((section(".fastrun." #tag), noinline, noclone))
#else
#define OT_FASTRUN_FN
#define OT_FASTRUN_INLINE(tag)
#endif
