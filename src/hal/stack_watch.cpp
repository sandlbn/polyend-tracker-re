// OpenTracker -- stack high-water mark (stack_watch.h).
#include "stack_watch.h"

#include "stack_guard.h"
#include "telemetry.h"

extern "C" uint32_t _ebss, _estack;   // opentracker.ld
extern "C" char* __brkval;            // heap top (third_party/teensy3/mk20dx128.c; _sbrk: stack_guard.cpp)

namespace ot {
namespace hal {

// Leaves this much below the live stack pointer untouched: the paint loop's
// own spills, should the compiler place any there.
static constexpr uint32_t kPaintGuardBytes = 64;

// The stack's room starts above the guard (stack_guard.h); the scan never
// reads the guard, which faults once it is armed.
static uint32_t* stackLow() { return reinterpret_cast<uint32_t*>(stackGuardEnd()); }

__attribute__((noinline)) void stackPaint() {
    uint32_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    volatile uint32_t* p = stackLow();
    volatile uint32_t* const top = reinterpret_cast<volatile uint32_t*>((sp - kPaintGuardBytes) & ~3u);
    while (p < top) *p++ = diag::kStackPaint;
}

uint32_t stackSizeBytes() {
    return uint32_t(reinterpret_cast<uintptr_t>(&_estack) - stackGuardEnd());
}

uint32_t heapBytes() {
    return uint32_t(reinterpret_cast<uintptr_t>(__brkval) - reinterpret_cast<uintptr_t>(&_ebss));
}

uint32_t stackPeakBytes() {
    uint32_t* const lo = stackLow();
    const size_t untouched = diag::untouchedStackWords(lo, &_estack);
    return uint32_t(reinterpret_cast<uintptr_t>(&_estack) - reinterpret_cast<uintptr_t>(lo)) - uint32_t(untouched * 4u);
}

}  // namespace hal
}  // namespace ot
