// OpenTracker -- stack high-water mark (telemetry "stack peak"). stackPaint()
// fills the unused stack (from the stack guard's end up to just below the
// caller's frame) with diag::kStackPaint; stackPeakBytes() finds the lowest
// word since overwritten. Below the guard sits newlib's heap reserve (malloc,
// e.g. for printf of floats; stack_guard.h), so heap and stack never share
// words. Interrupts share the
// stack (MSP), so the peak covers ISR frames too. Writes made before
// stackPaint() (startup code, static constructors) are not counted.
#pragma once

#include <stdint.h>

namespace ot {
namespace hal {

// First thing in main().
void stackPaint();
// The stack's room: _estack - the guard's end (stack_guard.h).
uint32_t stackSizeBytes();
// Heap in use: __brkval - _ebss.
uint32_t heapBytes();
// Deepest use since stackPaint(), in bytes from _estack. Scans up from the guard
// (about 25k words, ~0.2 ms): call it from the 5 s telemetry, not per frame.
uint32_t stackPeakBytes();

}  // namespace hal
}  // namespace ot
