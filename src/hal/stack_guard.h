// OpenTracker -- stack guard.
//
// _ebss.._estack holds newlib's heap (growing up from _ebss) and the one
// main stack (MSP, growing down from _estack; the audio DMA interrupt renders
// on it too). Without a guard, a stack overflow silently overwrites the heap
// and the top of .bss. The guard is a no-access window of the K66 system MPU
// (SYSMPU) for the core's two bus masters, between a fixed heap reserve and
// the stack:
//
//   _ebss | heap reserve (kHeapReserveBytes) | [module pool] | guard (kStackGuardBytes) | stack ... _estack
//
// [I] The module pool (hal/module_pool.h) is code loaded from the
// card. It borrows the bottom of the stack's room: setModulePoolBytes() moves
// the guard up over it (or back down) while the audio is stopped, and never
// leaves the stack less than kStackFloorBytes. Size 0 = the stock layout.
//
// - The heap stops at the guard: _sbrk (stack_guard.cpp) refuses to grow
//   into it (ENOMEM), as Teensy's _sbrk did near the stack pointer.
// - A stack overflow into the guard faults. On this single-stack design the
//   core cannot push the exception frame either (it would land in the guard),
//   so it locks up and the K66 resets with RCM_SRS1 LOCKUP set. The next boot
//   logs that reset reason (stackGuardInit), so an overflow is a clean reset
//   with a logged cause instead of silent corruption. A stray access into the
//   guard that is not a stack overflow is an ordinary BusFault with the
//   diag fault report.
// - The guard is larger than the deepest single stack frame of the firmware
//   plus an interrupt's exception frame (telemetry_check.py frames), so no
//   frame can step over it.
#pragma once

#include <stdint.h>

namespace ot {
namespace hal {

constexpr uint32_t kHeapReserveBytes = 4096;   // measured heap use 0.2 KB (telemetry, 2026-10-20)
constexpr uint32_t kStackGuardBytes = 8192;    // > the deepest frame (5,672 B, startOpenProject)

// [I] The stack never gets less than this when the module pool grows
// (measured peak 10.9 KB on v2 plus margin; 2026-11-06).
#if defined(OT_POOL_TEST) && OT_POOL_TEST
constexpr uint32_t kStackFloorBytes = 15 * 1024;   // [I] measurement images only (peak ~7 KB)
#else
constexpr uint32_t kStackFloorBytes = 25 * 1024;
#endif

// Lowest guard address (32-byte aligned, the SYSMPU granule) and the first
// address above it, where the stack's room begins.
uintptr_t stackGuardStart();
uintptr_t stackGuardEnd();

// [I] The module pool: its fixed base (above the heap reserve,
// 32-byte aligned), its size now, and the largest size the stack floor allows.
uintptr_t modulePoolBase();
uint32_t modulePoolBytes();
uint32_t modulePoolMaxBytes();
// Resizes the pool (rounded up to 32 B) and moves the guard with it. False,
// and nothing changes, when the stack would drop below kStackFloorBytes or
// the live stack pointer is too close to the new guard. Main loop, audio
// stopped; the stack room it frees is painted for the high-water mark.
bool setModulePoolBytes(uint32_t bytes);

// Logs the previous reset's reason when it was a core lockup, then arms the
// guard (SYSMPU regions) unless arm is false (/Firmware/ot_noguard.txt on the
// card: main.cpp). After diagInit() and the restore check, before the app.
void stackGuardInit(bool arm);
bool stackGuardArmed();
// The reset before this boot was a core lockup (RCM_SRS1 LOCKUP): a stack
// overflow into the guard, or another double fault.
bool lastResetWasLockup();

#if defined(OT_STACK_GUARD_TEST) && OT_STACK_GUARD_TEST
}  // namespace hal
class IStorage;
namespace hal {
// Test builds only (make OT_STACK_GUARD_TEST=1): when /Firmware/ot_guardtest.txt
// exists, deletes it (one shot: no boot loop) and overflows the stack on
// purpose; the guard must turn that into a lockup reset. The next boot logs
// "stack guard test: PASSED" when the reset came from the lockup.
void stackGuardSelfTest(IStorage& fs);
#endif

}  // namespace hal
}  // namespace ot
