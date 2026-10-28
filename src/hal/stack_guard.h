// OpenTracker -- stack guard.
//
// _ebss.._estack holds newlib's heap (growing up from _ebss) and the one
// main stack (MSP, growing down from _estack; the audio DMA interrupt renders
// on it too). Without a guard, a stack overflow silently overwrites the heap
// and the top of .bss. The guard is a no-access window of the K66 system MPU
// (SYSMPU) for the core's two bus masters, between a fixed heap reserve and
// the stack:
//
//   _ebss | heap reserve (kHeapReserveBytes) | guard (kStackGuardBytes) | stack ... _estack
//
// - The heap stops at the guard: _sbrk (stack_guard.cpp) refuses to grow
//   into it (ENOMEM), as Teensy's _sbrk did near the stack pointer.
// - A stack overflow into the guard faults. On this single-stack design the
//   core cannot push the exception frame either (it would land in the guard),
//   so it locks up and the K66 resets with RCM_SRS1 LOCKUP set. The next boot
//   logs that reset reason (stackGuardInit), so an overflow is a clean reset
//   with a logged cause instead of silent corruption. A stray access into the
//   guard that is not a stack overflow is an ordinary BusFault with the
//   diag fault report; diag.cpp adds the SYSMPU error address to it.
// - The guard is larger than the deepest single stack frame of the firmware
//   plus an interrupt's exception frame (telemetry_check.py frames), so no
//   frame can step over it.
#pragma once

#include <stdint.h>

namespace ot {
namespace hal {

constexpr uint32_t kHeapReserveBytes = 4096;   // measured heap use 0.2 KB (telemetry, 2026-10-20)
constexpr uint32_t kStackGuardBytes = 8192;    // > the deepest frame (5,672 B, startOpenProject)

// Lowest guard address (32-byte aligned, the SYSMPU granule) and the first
// address above it, where the stack's room begins.
uintptr_t stackGuardStart();
uintptr_t stackGuardEnd();

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
