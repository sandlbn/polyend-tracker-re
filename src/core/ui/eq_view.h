// Shared drawing and text for the 4-band EQ (OpenTracker extension): the
// Track EQ page (screen_mixer.cpp) and the Sample Editor EQ4 effect
// (screen_editor.cpp) show the same response graph and value texts.
#pragma once

#include "audio/track_eq.h"
#include "ui/widgets.h"

namespace ot {
namespace eqv {

void freqText(int hz, char* out, int cap);                                        // "80 Hz" / "2.50 kHz"
void gainText(const audio::EqBand& b, int index, char* out, int cap);             // gain, or the cut slope / "-"
void qText(const audio::EqBand& b, char* out, int cap);                           // "0.71"

// Response curve of the enabled bands on a log axis (20 Hz .. 20 kHz, +-15 dB)
// with one marker per band (filled = on, `sel` larger), in the panel (x, y, w, h).
void drawGraph(Gfx& g, const Theme& th, const audio::TrackEqParams& tp, int sel, int x, int y, int w, int h);

}  // namespace eqv
}  // namespace ot
