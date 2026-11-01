// Effect slots for modules (docs/PLAN_3X.md §2.2, docs/PLAN_3X_M2.md M-C).
// Module-facing: only <cstdint>. Audio context, like ISynthEngine: no
// allocation, no I/O, no locks; parameters are read once per block (a module
// publishes them from the main loop) and ramped across the block.
#pragma once

#include <cstdint>

namespace ot {
namespace audio {

// After the master sum (dry + delay / reverb returns + line in, saturation
// applied), before the limiter. Up to AudioEngine::kMaxMasterInserts per
// image, run in profile order (a chain).
class IMasterInsert {
public:
    // Process the stereo master bus in place (n = kBlock frames). Called while
    // the bus carries sound or the previous call returned true; return true
    // while the insert still has a tail to play out (a delay line).
    virtual bool process(float* l, float* r, uint32_t n) = 0;
    // [3.x] E0: the song tempo in BPM (the sequencer's smoothed tempo), before
    // process() in the block it changes, and once when the chain is bound;
    // also while the bus is silent (tempo-synced delays). Optional.
    virtual void setTempo(float bpm) { (void)bpm; }
};

}  // namespace audio
}  // namespace ot
