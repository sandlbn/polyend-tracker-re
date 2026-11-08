// In-memory pattern (spec 02 §4.2-§4.4).
#pragma once

#include <cstdint>

#include "model/constants.h"
#include "model/fx.h"

namespace ot {

struct StepFx {
    uint8_t type = 0;   // FxId value
    uint8_t value = 0;
    bool used() const { return type != 0 || isPresetCmd(type, value); }
    // Type 0, keeping the value like the stock clear, unless it would read as a Z.
    void clearType() {
        type = 0;
        if (isPresetValue(value)) value = 0;
    }
};

// One step. fx[0] is "FX slot 0", fx[1] is "FX slot 1". On disk the order is
// slot 1 then slot 0 (spec 02 §4.2); the codec handles the swap.
struct Step {
    int8_t note = kNoteEmpty;
    uint8_t instrument = 0;
    StepFx fx[kFxPerStep];

    // Firmware clear routine: note empty, instrument 0, FX types 0. FX
    // values are left untouched (spec 02 §4.2), but for one that would read
    // as a Z command.
    void clear() {
        note = kNoteEmpty;
        instrument = 0;
        for (auto& f : fx) f.clearType();
    }
    bool operator==(const Step& o) const {
        return note == o.note && instrument == o.instrument && fx[0].type == o.fx[0].type &&
               fx[0].value == o.fx[0].value && fx[1].type == o.fx[1].type && fx[1].value == o.fx[1].value;
    }
};

struct Track {
    uint8_t lastStep = kDefaultTrackLastStep;  // length - 1 (0..127), spec 02 §4.2
    Step steps[kStepCount];

    int length() const { return int(lastStep) + 1; }
    void clear(uint8_t last) {
        lastStep = last;
        for (auto& s : steps) s.clear();
    }

    // Track without data: every step empty (note, instrument and both FX
    // types 0; FX values are ignored like Step::clear() leaves them, a Z
    // command excepted).
    bool blank() const {
        for (const Step& s : steps)
            if (s.note != kNoteEmpty || s.instrument || s.fx[0].used() || s.fx[1].used()) return false;
        return true;
    }
};
static_assert(sizeof(Track) == 769, "track record is 769 bytes (spec 02 §4.2)");

// N tracks of a pattern. Layout = N packed track records (no padding).
template <int N>
struct PatternTracks {
    static constexpr int kTracks = N;
    Track tracks[N];

    // Default pattern (spec 02 §4.4): every track 32 steps, all steps empty.
    void setDefault() {
        for (auto& t : tracks) {
            t.lastStep = kDefaultTrackLastStep;
            for (auto& s : t.steps) s = Step{};
        }
    }
};

// Stock pattern: tracks 0..11. The resident pattern store
// (App::storedStock) and the pattern_NN.mtp image (spec 02 §4).
struct PatternStock : PatternTracks<kStockTrackCount> {};

// Working pattern: all 16 tracks (sequencer current / preload buffers, undo,
// file undo, clipboard, performance scratch). Tracks 12..15 are stored in
// the ext-track pool (model/pattern_store.h) when not empty.
struct Pattern : PatternTracks<kTrackCount> {
    void copyStockFrom(const PatternStock& s) {
        for (int t = 0; t < kStockTrackCount; ++t) tracks[t] = s.tracks[t];
    }
    void copyStockTo(PatternStock& s) const {
        for (int t = 0; t < kStockTrackCount; ++t) s.tracks[t] = tracks[t];
    }
};

static_assert(sizeof(PatternStock) == 769u * kStockTrackCount, "packed stock tracks");
static_assert(sizeof(Pattern) == 769u * kTrackCount, "packed tracks");

// An extension track that has no pool block reads as this: all steps empty,
// the pattern length of stock track 0 (as the legacy 8 -> 12 upgrade fills
// tracks 8..11, spec 02 §4.4).
inline void defaultExtTrack(Track& t, const PatternStock& s) { t.clear(s.tracks[0].lastStep); }
inline void defaultExtTrack(Track& t, const Pattern& p) { t.clear(p.tracks[0].lastStep); }
// True when ext track t (12..15) of p carries nothing a block must keep.
inline bool extTrackIsDefault(const Track& t, uint8_t baseLastStep) {
    return t.lastStep == baseLastStep && t.blank();
}

}  // namespace ot
