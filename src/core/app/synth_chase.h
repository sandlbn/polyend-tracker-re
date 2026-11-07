// [3.x] Z chase (app/synth_chase.cpp): the synth preset each slot plays at a
// position of the song, read from the patterns as written -- the last Z
// before it in play order (mutes, Chance and random FX not followed). Used at
// a play start (ISequencerHost::synthChase) and, while stopped, to play pads
// and MIDI on the preset the Play of the screen would start with (the Pattern
// screen's cursor row, the Song screen's position).
//
// A summary per stored pattern keeps a whole pass cheap: what a full pass
// leaves each slot on and which synth each track played last. A no-note Z
// before the first note of its track depends on what came before: such a
// pattern is read again in full where it is used. Summaries are main-loop
// data, made stale by every write to the pattern store and by any change of
// the presets, and rebuilt on use or a few at a time while stopped.
#pragma once

#include <cstdint>

#include "model/constants.h"

namespace ot {
namespace chase {

// A play state: preset per slot (0 = own patch) and the instrument each track
// played last (0xFF = not known; kNonSynth = some instrument that is not a synth).
struct State {
    uint8_t preset[kSynthSlotCount];
    uint8_t last[kTrackCount];
};
constexpr uint8_t kUnknown = 0xFF;
constexpr uint8_t kNonSynth = 0;

// One pattern, 8 bytes. zf[0] low nibble: flags; then one nibble per slot (zf[0]
// high, zf[1] low, zf[1] high): the preset a full pass leaves it on, kNoZ = no
// Z for that slot. last: 3 bits per track, 0 no note, 1 not a synth, 2 + s Synth s.
struct Summary {
    uint8_t zf[2];
    uint8_t last[6];
};
static_assert(sizeof(Summary) == 8, "8 bytes per pattern");
enum : uint8_t { kValid = 1, kHasZ = 2, kUnresolved = 4 };
constexpr uint8_t kNoZ = 0x0F;

struct Memory {
    Summary pat[kPatternCount];   // pattern n at [n - 1]
    uint32_t gen;                 // bumped by anything the stopped chase reads
    // Stopped auditioning: the inputs last applied and what each slot got
    // (0xFF = not known: the slots are set again).
    uint32_t keyGen;
    int16_t keyStep;
    uint8_t keyPattern, keyPos, keySong, keyValid;
    uint8_t applied[kSynthSlotCount];
    // Song prefix of the stopped chase (positions before keyPos), by gen.
    uint8_t prefixValid, prefixPos;
    uint32_t prefixGen;
    State prefix;
    uint8_t warmNext;             // next pattern index the warm-up looks at
    uint8_t stale;                // a summary may be stale (the warm-up has work)
    uint64_t ticks;               // the sequencer's tick count at the last pass
};

Memory& mem();

}  // namespace chase
}  // namespace ot
