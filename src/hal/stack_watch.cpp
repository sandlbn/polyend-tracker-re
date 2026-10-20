// OpenTracker -- stack high-water mark (stack_watch.h).
#include "stack_watch.h"

#include "telemetry.h"

extern "C" uint32_t _ebss, _estack;   // opentracker.ld

namespace ot {
namespace hal {

// Leaves this much below the live stack pointer untouched: the paint loop's
// own spills, should the compiler place any there.
static constexpr uint32_t kPaintGuardBytes = 64;

__attribute__((noinline)) void stackPaint() {
    uint32_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    volatile uint32_t* p = &_ebss;
    volatile uint32_t* const top = reinterpret_cast<volatile uint32_t*>((sp - kPaintGuardBytes) & ~3u);
    while (p < top) *p++ = diag::kStackPaint;
}

uint32_t stackSizeBytes() {
    return uint32_t(reinterpret_cast<uintptr_t>(&_estack) - reinterpret_cast<uintptr_t>(&_ebss));
}

uint32_t stackPeakBytes() {
    const size_t untouched = diag::untouchedStackWords(&_ebss, &_estack);
    return stackSizeBytes() - uint32_t(untouched * 4u);
}

}  // namespace hal
}  // namespace ot
