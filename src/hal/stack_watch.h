// OpenTracker -- stack high-water mark (telemetry "stack peak"). stackPaint()
// fills the unused stack (the heap top up to just below the caller's frame)
// with diag::kStackPaint; stackPeakBytes() finds the lowest word since
// overwritten. _ebss.._estack is shared with newlib's heap (malloc, e.g. for
// printf of floats), which grows up from _ebss through Teensy's _sbrk
// (__brkval); the scan starts at the current heap top. Interrupts share the
// stack (MSP), so the peak covers ISR frames too. Writes made before
// stackPaint() (startup code, static constructors) are not counted.
#pragma once

#include <stdint.h>

namespace ot {
namespace hal {

// First thing in main().
void stackPaint();
// _estack - _ebss (heap + stack).
uint32_t stackSizeBytes();
// Heap in use: __brkval - _ebss.
uint32_t heapBytes();
// Deepest use since stackPaint(), in bytes from _estack. Scans up from the heap top
// (about 25k words, ~0.2 ms): call it from the 5 s telemetry, not per frame.
uint32_t stackPeakBytes();

}  // namespace hal
}  // namespace ot
