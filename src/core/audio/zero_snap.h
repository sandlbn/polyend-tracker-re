// Snap to zero (spec 13 §H.2, spec 04 §2.3). Hook used by the voice when it
// converts the start and loop markers to frames, and by the Sample Playback
// screen for the marker colour. The stored markers never change.
//
// Search window ±64 frames around the target; a frame p is a crossing when
// x[p] == 0 or x[p] and x[p+1] differ in sign; the nearer of the forward and
// backward hits wins (forward on a tie). No hit: the exact frame.
#pragma once

#include <cstdint>

namespace ot {
namespace audio {

constexpr int32_t kZeroSnapWindow = 64;

// Project option "Snap to zero" (.mt 0x602, RAM 0x2000BECD), copied in by the
// engine when it applies the project settings.
extern volatile bool gSnapToZero;

// Nearest crossing to `frame` within the window, or `frame`.
int32_t snapToZero(const int16_t* x, uint32_t len, int32_t frame);
// Marker indicator: flag off -> the wave crosses zero in the frames the
// marker's step covers (up to the next step); flag on -> a crossing exists in
// the window at a position other than its edges.
bool markerOnCrossing(const int16_t* x, uint32_t len, uint16_t marker, bool snapOn);

}  // namespace audio
}  // namespace ot
