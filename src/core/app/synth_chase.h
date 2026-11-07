// [OT] Z chase: which synth preset each slot is on at a pattern position,
// read from the pattern data the way the sequencer plays it (sequencer.cpp
// stepStart). Used at Play from a later position (ISequencerHost::synthChase)
// and while stopped, so pads and MIDI play what Play would start on: the
// pattern alone up to the pattern cursor, or on the Song screen the song
// positions before its cursor.
//
// Play order: song mode, playlist positions 0..pos-1 whole (an entry 0 is
// skipped), then the pattern's steps; within a step tracks 0..15, each FX-B
// then FX-A. A Z on a step without a note uses the track's last instrument.
// Chance, random FX and mutes are not modelled: a pattern is read as written.
//
// Main loop only. Each stored pattern has a summary (what one pass from the
// own patches leaves behind) so a song prefix costs a few bytes per position;
// store writers drop it, and an idle pass rebuilds the stale ones.
#pragma once

#include <cstdint>

#include "model/constants.h"
#include "model/pattern.h"

namespace ot {
namespace chase {

constexpr uint8_t kNone = 0xFF;   // no Z (summary) / no note seen (last)

struct State {
    uint8_t preset[kSynthSlotCount];   // 0 = own patch, 1..8
    uint8_t last[kTrackCount];         // last note's instrument, kNone = none
};
inline void startState(State& s) {
    for (auto& p : s.preset) p = 0;
    for (auto& l : s.last) l = kNone;
}

// One pass of a pattern from the own patches and no notes. kUnresolved: a Z
// without a note before the first note of its track, so its slot depends on
// what came before (the pattern is then scanned with the real state).
struct Summary {
    uint8_t flags;
    uint8_t zOut[kSynthSlotCount];   // the slot's preset after the pass, kNone = no Z for it
    uint8_t last[kTrackCount];       // last note's instrument, kNone = no note
};
enum : uint8_t { kValid = 1, kHasZ = 2, kUnresolved = 4 };

// Steps from..toInclusive of `tracks` (nullptr = an empty track) applied to
// io. used: bit n set = preset n holds a patch (a Z naming an empty one does
// nothing). sum (optional, from a startState io): filled with the summary.
void scan(const Track* const tracks[kTrackCount], int from, int toInclusive, uint16_t used, State& io,
          Summary* sum = nullptr);
// A valid summary without kUnresolved applied to io (one pass of its pattern).
void apply(const Summary& sum, State& io);

// The chase memory (SDRAM on the device, zeroed POD).
struct StopKey {
    uint32_t patSerial, presetSerial, stopSerial, playlistHash, ticks;
    uint16_t used;
    int16_t cursor;
    uint8_t pattern, songMode, pos, valid;
};
struct Memory {
    Summary pat[kPatternCount];   // pattern n at [n - 1]
    uint32_t patSerial;           // bumped by every invalidation
    uint32_t presetSerial;        // store / clear / reset of a preset
    uint32_t stopSerial;          // playing -> stopped, or any other stop of the sequencer
    uint16_t used;                // the preset mask the summaries were made with
    uint8_t warmNext;             // next pattern index the idle warm-up looks at
    // Song prefix cache: the state at the start of position `pos`.
    uint8_t prefixValid, prefixPos;
    uint8_t seqStops;             // the sequencer's stop count last seen
    uint16_t prefixUsed;
    uint32_t prefixHash, prefixSerial;
    State prefix;
    StopKey stopped;              // what the stopped chase was last applied for
    uint8_t applied[kSynthSlotCount];   // and the preset it gave each slot
};
Memory& mem();
void resetAll();             // everything stale, serials and keys cleared
void invalidate(int num);    // pattern num's summary is stale
void invalidateAll();
// FNV-1a of playlist[0..n-1].
uint32_t playlistHash(const uint8_t* playlist, int n);

}  // namespace chase
}  // namespace ot
