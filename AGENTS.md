# AGENTS.md

OpenTracker: reverse-engineered firmware for the original Polyend Tracker (NXP K66,
Cortex-M4, 256 KB SRAM, 16 MB SDRAM). This file lists the rules that are easy to break,
for coding agents (Claude Code reads it through `CLAUDE.md`) and for people.

## Clean room

OpenTracker is a clean-room implementation. Polyend has published its own firmware as
Tracker CE (`polyend/tracker-ce`) under a noncommercial licence; its code stays out of
OpenTracker (one MIT exception below).

- Never clone, fetch, open, search or quote CE source code, headers or binaries, and don't
  port, translate or paraphrase them. This includes code search, web pages that show CE
  code, and snippets pasted into a prompt.
- CE's user-facing docs (manual, changelog, release notes) and on-device behaviour are
  fine as a reference, written up in your own words.
- Developer docs that describe internals or contain code count as source. Plain hardware
  facts from them (pins, part numbers, buses, memory sizes) are fine.
- `.ptfx` interface docs (file format, plugin API) are fine: under the SDK exception below
  when they're MIT, and as interface facts when they're not. How CE's own code loads
  plugins stays out.
- The one exception is CE's sample editor SDK (the `.ptfx` plugin SDK), which is MIT
  licensed. It may be read and reused, as long as it goes in `third_party/` with its
  licence and copyright notice. Check the licence of the exact version you take. The
  exception covers only the SDK: nothing else in the CE repository.
- Spec references cite stock behaviour or published docs, never CE code.
- If a task looks like it needs CE source, stop and say so instead of looking.

## Build and test

- Firmware: `make clean && make -j`. Don't pass `VERSION`: the Makefile default is the
  release version (run `make clean` whenever it changes).
- Host tests: `make host-test`. Simulator: `make sim`, then `make -C sim check`
  (see `sim/README.md`).
- Audio changes: `make perf-gate`. Size checks: `tools/telemetry_check.py size` and `frames`.
- Test in the simulator and on the host first. Use the device only for sound, real CPU and
  stack use, pads/LEDs, codec and SD timing.
- Games are off by default (`make OT_GAMES=1` builds them in). Say which build you
  measured when you quote memory numbers.
- Stack guard: `/Firmware/ot_noguard.txt` on the card turns it off, and
  `make OT_STACK_GUARD_TEST=1` builds the overflow self-test image.
- Memory tools: `tools/size_report.py <base.elf> <head.elf>` prints the same table as the CI
  size report. `pahole -C App build/opentracker.elf` shows struct layouts.

## Code rules

- **No static constructors.** No namespace-scope object or pointer with a dynamic
  initializer (e.g. `T* const p = f();` with `f()` in another file). Use `constexpr`/
  `constinit`, an `extern` array in a header with `inline` helpers, or init in `start()`.
  They run before SDRAM is up: `tools/check_ctors.py` fails the build if one touches SDRAM.
  Check for new ones: `arm-none-eabi-nm -C build/opentracker.elf | grep _GLOBAL__sub_I`,
  compared with the base branch.
- **Memory:** only per-block audio state goes in SRAM. Data, UI state and buffers go in
  SDRAM (`.sdram_bss`, trivially constructible). New audio code starts in flash, not
  `OT_FASTRUN_FN`, unless `make perf-gate` shows it's needed. Check the CI size report on
  every PR.
- **Audio path:** no allocation, no I/O, no locks.
- `src/core` is platform-independent (firmware, sim, host tests); `src/hal` is the K66 layer.
- Mark OpenTracker-only behaviour with `[I]` comments, and reverse-engineered stock
  behaviour with spec references.

## Compatibility

- Stock files (`project.mt`, patterns, `.pti`, `.patch`) keep their exact stock/Mini
  layout. OpenTracker-only data goes in side files or extension records, so stock
  firmware still loads the project and ignores the rest.

## Before opening a PR

1. `make host-test` and `make -C sim check`.
2. `make perf-gate` if anything on the audio path changed.
3. Check the size-report delta and the `_GLOBAL__sub_I` count against the base.
4. Update `README.md` / `sim/README.md` when behaviour or tooling changes.
