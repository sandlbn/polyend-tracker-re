// Sequencer constant tables (spec 03 §2.6, §3.5, §3.6).
#include "sequencer/seq_tables.h"

#include "model/fx.h"

namespace ot {
namespace seq {

// spec 03 §3.5 Roll: hits {1,2,3,4,6,8,12,16} per step.
const uint8_t kRollPeriod[8] = {48, 24, 16, 12, 8, 6, 4, 3};
// spec 03 §3.5 Arp.
const uint16_t kArpPeriod[11] = {384, 288, 192, 144, 96, 48, 24, 16, 12, 8, 6};
// spec 03 §2.6 step repeater.
const uint16_t kRepeatPeriod[15] = {768, 576, 384, 288, 192, 144, 96, 48, 24, 16, 12, 8, 6, 4, 3};

// spec 03 §3.5 MIDI Chord table.
const uint8_t kChordIntervals[kChordCount][4] = {
    {0, 0, 0, 0},     // 0 none
    {0, 2, 7, 0},     // 1
    {0, 2, 8, 0},     // 2
    {0, 3, 6, 0},     // 3
    {0, 3, 7, 0},     // 4
    {0, 4, 7, 0},     // 5
    {0, 4, 8, 0},     // 6
    {0, 5, 7, 0},     // 7
    {0, 5, 10, 0},    // 8
    {0, 5, 12, 0},    // 9
    {0, 6, 7, 0},     // 10
    {0, 7, 12, 0},    // 11
    {0, 7, 14, 0},    // 12
    {0, 2, 7, 9},     // 13
    {0, 2, 7, 10},    // 14
    {0, 2, 7, 11},    // 15
    {0, 3, 6, 9},     // 16
    {0, 3, 6, 10},    // 17
    {0, 3, 7, 8},     // 18
    {0, 3, 7, 9},     // 19
    {0, 3, 7, 10},    // 20
    {0, 3, 7, 11},    // 21
    {0, 4, 7, 9},     // 22
    {0, 4, 7, 10},    // 23
    {0, 4, 7, 11},    // 24
    {0, 4, 8, 9},     // 25
    {0, 4, 8, 10},    // 26
    {0, 4, 8, 11},    // 27
    {0, 5, 7, 10},    // 28
    {0, 5, 7, 11},    // 29
};
const uint8_t kChordSize[kChordCount] = {0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
                                         4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};

uint16_t chordMask(uint8_t value) {
    if (value == 0 || value >= kChordCount) return 0;
    uint16_t m = 0;
    for (int i = 0; i < kChordSize[value]; ++i) m |= uint16_t(1u << (kChordIntervals[value][i] % 12));
    return m;
}

namespace {
// spec 03 §3.6 Range column, index = FX id.
struct Range { uint8_t lo, hi; };
const Range kRanges[kFxListCount] = {
    {0, 100},  // 0 None
    {0, 0},    // 1 Off
    {0, 100},  // 2 Micro-move
    {0, 53},   // 3 Roll (TL 47)
    {0, 100},  // 4 Chance
    {0, 100},  // 5 Random note
    {0, 47},   // 6 Random instrument (TL 100)
    {0, 100},  // 7 Random volume
    {0, 127}, {0, 127}, {0, 127}, {0, 127}, {0, 127},  // 8..12 CC A..E
    {1, 1},    // 13 Break
    {0, 29},   // 14 Chord (TL 15)
    {4, 200},  // 15 Tempo (5 under Random FX)
    {0, 255},  // 16 Random FX value
    {25, 75},  // 17 Swing
    {0, 100},  // 18 Volume
    {0, 100},  // 19 Glide
    {0, 100},  // 20 Gate
    {0, 33},   // 21 Arp
    {0, 100},  // 22 Position
    {0, 23},   // 23 Volume LFO (TL 24)
    {0, 28},   // 24 Pan LFO (TL 30)
    {0, 47},   // 25 Slice
    {0, 1},    // 26 Reverse
    {0, 100}, {0, 100}, {0, 100},  // 27..29 filters
    {0, 100},  // 30 Delay send
    {0, 100},  // 31 Panning
    {0, 100},  // 32 Reverb send
    {0, 28},   // 33 Finetune LFO (TL 30)
    {0, 198},  // 34 Micro-tune / pitch bend
    {0, 28},   // 35 Filter LFO (TL 30)
    {0, 28},   // 36 Position LFO (TL 30)
    {0, 127},  // 37 CC F
    {0, 100},  // 38 Overdrive
    {4, 16},   // 39 Bit depth (TL 1)
    {0, 48},   // 40 Tune
    {0, 255},  // 41 Slide up
    {0, 255},  // 42 Slide down
    {0, 8},    // 43 Synth preset (list only)
};
}  // namespace

uint8_t fxMin(uint8_t type) { return type < kFxListCount ? kRanges[type].lo : 0; }
uint8_t fxMax(uint8_t type) { return type < kFxListCount ? kRanges[type].hi : 255; }

}  // namespace seq
}  // namespace ot
