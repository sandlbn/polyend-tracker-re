// [I] Live resampling (Octatrack-style, not in stock): record the master mix,
// one sample track or the line input while the sequencer plays, starting on
// the next bar, for a fixed number of bars or until stopped (snapped to a
// bar), into the sample memory reserved at the end of the free bank
// (SampleBank::setReserved); App::serviceResample() then turns it into the
// next free instrument without moving any other sample.
//
// Audio context: onBlock() from App::preBlock after the sequencer advanced,
// tapBlock() from the engine after the mix (audio::IResampleTap). Main loop:
// arm / requestStop / reset and the state getters. No locking: single
// volatile state byte, the area is only touched by one side at a time.
#pragma once

#include <cstddef>
#include <cstdint>

#include "audio/engine.h"

namespace ot {

struct ResampleSettings {
    // 0 master, 1..8 sample track, 9 line input (AudioEngine::kTapLineIn).
    uint8_t source = 0;
    uint8_t length = 3;   // index into kResampleBars: Manual, 1, 2, 4, 8, 16 bars
    uint8_t place = 0;    // 0 off, 1..8: put the result on that track
    // 0 off; equal slices: 1 one per beat, 2 one per step; at hits: 3 on the
    // beat grid, 4 on the step grid (Beat Slice, App::resampleSliceSteps).
    uint8_t slices = 0;
    // 0 a new instrument (the next free slot); 1..48 instrument N: a take of
    // the same length (App::kResampleFitFrames) overwrites it in place, its
    // settings kept; else it goes to a new instrument (App::finishResample).
    uint8_t into = 0;
    static constexpr int kSources = 10;
    static constexpr int kLengths = 6;
    static constexpr int kPlaces = 9;
    static constexpr int kSlices = 5;
    static constexpr int kInto = 49;
};
constexpr uint8_t kResampleBars[ResampleSettings::kLengths] = {0, 1, 2, 4, 8, 16};

// Sliced takes at hits: of `cells` equal grid cells over a planar take of n
// frames (r may be null), the cells where a new hit starts, in order; cell 0
// always. A hit is a jump of the signal's first difference (transients over
// a ringing tail) in the first ~30 ms of the cell against the ~10 ms before
// it. Writes at most `max` cell indices to `out`; returns how many it found.
int findHitCells(const int16_t* l, const int16_t* r, uint32_t n, int cells, uint16_t* out, int max);

class Resampler final : public audio::IResampleTap {
public:
    enum State : uint8_t { kIdle, kArmed, kRecording, kStopping, kDone };
    enum End : uint8_t { kEndNone, kEndBars, kEndStopped, kEndTransport, kEndFull };

    // Per-take diagnostics (the "resample:" diag log line, src/app_main.cpp).
    // Written by tapBlock() while recording; read by the main loop once the
    // take is done (the audio side no longer writes then).
    struct TakeStats {
        uint32_t blocks = 0;          // tap blocks recorded
        uint32_t silentBlocks = 0;    // blocks of digital silence (peak 0)
        uint32_t longestSilent = 0;   // longest run of silent blocks
        uint16_t peak = 0;            // largest |sample|, both channels
    };

    // Main loop. `area` holds two planar halves of areaSamples / 2 frames
    // (left, right). bars = 0: until requestStop().
    void arm(int16_t* area, uint32_t areaSamples, uint8_t bars);
    // Recording: stop at the next bar (keeps what was recorded). Armed: no-op.
    void requestStop();
    void reset();   // back to idle (the caller releases the area)

    State state() const { return State(state_); }
    bool busy() const { return state_ != kIdle; }
    End end() const { return End(end_); }
    uint32_t frames() const { return frames_; }
    uint32_t barsDone() const { return barsDone_; }
    uint8_t bars() const { return bars_; }
    uint32_t capacityFrames() const { return half_; }
    const TakeStats& stats() const { return stats_; }
    const int16_t* left() const { return area_; }
    const int16_t* right() const { return area_ ? area_ + half_ : nullptr; }

    // Audio context: once per live block after the sequencer advanced.
    // barStart: a bar began in this block (its notes start in it).
    void onBlock(bool playing, bool barStart);
    void tapBlock(const float* l, const float* r, uint32_t n) override;

private:
    void finish(End e) {
        end_ = e;
        state_ = kDone;
    }
    int16_t* area_ = nullptr;
    uint32_t half_ = 0;
    volatile uint8_t state_ = kIdle;
    volatile uint8_t end_ = kEndNone;
    volatile uint32_t frames_ = 0;
    volatile uint32_t barsDone_ = 0;
    uint8_t bars_ = 0;
    TakeStats stats_;
    uint32_t silentRun_ = 0;
};

// The last finished (or cancelled) take, kept by App::finishResample() for
// the diag log: seq counts takes, so the device glue logs each one once.
struct ResampleReport {
    enum Outcome : uint8_t { kOk, kEmpty, kNoSlot, kFailed, kCancelled };
    uint32_t seq = 0;
    uint8_t source = 0;      // ResampleSettings::source
    uint8_t end = 0;         // Resampler::End
    uint8_t outcome = kOk;
    int8_t slot = -1;        // instrument index (kOk)
    uint32_t frames = 0;
    Resampler::TakeStats stats;
};

// "resample: take N source S end E -> R | F frames (T s) blocks B silent X
// longest Y peak P (Q%) | rx restarts +A stale +B". Returns the length.
size_t formatResampleReport(char* out, size_t cap, const ResampleReport& r, uint32_t rxRestarts,
                            uint32_t rxStale);

}  // namespace ot
