// Step-FX text for the pattern grid (spec 03 §3.6 "Display", spec 06 §1.2
// "Cell text", 0x14918 / 0x146a4 / 0xcb83c).
#pragma once

#include <cstdint>

namespace ot {

// Value text for (type, value), at most 3 characters + NUL (spec 16 §A.8),
// per the display transforms of spec 03 §3.6: Tempo 2v BPM, v == 4 ->
// "STP"; Roll: type char + 2-digit hits per step; Chord: interval digits;
// Arp: direction + rate; Panning v-50; Micro-tune v-99; Tune v-24; Slice
// v+1; LFOs: rate labels; Break "BRK"; Off "OFF"; Reverse ">>>" / "<<<";
// others: v.
void fxValueText(uint8_t type, uint8_t value, char out[5]);

// Pattern cell text: FX symbol + right-aligned 3-char value ("V100",
// "P-25", "TSTP", "!OFF"), 4 chars + NUL; empty slot "----".
void fxCellText(uint8_t type, uint8_t value, char out[8]);

// Help text for the FX picker's description window (manual: FX popup
// F7/F8). The wording is OpenTracker's own, written from the spec 03 §3.6
// meanings.
const char* fxDescription(uint8_t type);
// The FX help window text (spec 16 §B.4, manual p.142): the description, a
// blank line and the value range ("From -50 to 50") from the engine range;
// no range for None / Off / Break. Returns a static buffer (UI thread).
const char* fxHelpText(uint8_t type);

constexpr int kFxCellChars = 4;

}  // namespace ot
