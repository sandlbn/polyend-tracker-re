// Timing API for module benchmarks (tests/audio_bench.cpp; a module lists
// its bench sources in module.toml `bench_sources`, `bench_entry` names the
// function, tools/profile.py generates the runner). A module bench prints
//   "    <id>.<what> <value>  (<what it measures>, limit <N> M4 insn)"
// lines; make perf-gate fails any line above its limit (bench-m4).
#pragma once

namespace otbench {
double nowUnits();       // M4 instructions under QEMU (bench-m4), microseconds on the host
int scale();             // divide the block count by this (fewer blocks under emulation)
const char* unit();      // "M4 insn" / "us"
}  // namespace otbench

// Generated (build/<profile>/gen/module_bench.cpp): runs every module bench.
void runModuleBenches();
