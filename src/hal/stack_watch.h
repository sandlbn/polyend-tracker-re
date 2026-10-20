// OpenTracker -- stack high-water mark (telemetry "stack peak"). stackPaint()
// fills the unused stack (_ebss up to just below the caller's frame) with
// diag::kStackPaint; stackPeakBytes() finds the lowest word since overwritten.
// The stack is the only user of _ebss.._estack (no heap), and interrupts share
// it (MSP), so the peak covers ISR frames too. Writes made before stackPaint()
// (startup code, static constructors) are not counted.
#pragma once

#include <stdint.h>

namespace ot {
namespace hal {

// First thing in main().
void stackPaint();
// _estack - _ebss.
uint32_t stackSizeBytes();
// Deepest use since stackPaint(), in bytes from _estack. Scans up from _ebss
// (about 25k words, ~0.2 ms): call it from the 5 s telemetry, not per frame.
uint32_t stackPeakBytes();

}  // namespace hal
}  // namespace ot
