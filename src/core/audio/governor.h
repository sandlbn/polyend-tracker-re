// Synth voice governor (docs/specs/2x-02 §3; 2.x M1 plan §5) [OT].
//
// Lowers a global synth voice cap C (start: the budget B, 4 in 2.0.0) when
// the render load gets close to the block budget, and steals synth voices
// until the sounding count fits. It never touches sample tracks, effects or
// MIDI: it only sees synth voices through IVoicePool (implemented by the M3
// synth engine). Evaluated once per live block after render, in the audio
// context, from the CpuMeter values (integer compares only).
//
// Control law (§3.2 as revised after the 2.0.0 hardware round,
// docs/OPTIMIZATION_2X.md "Governor"), 1 block = 2.90 ms. The cut follows the
// overrun risk, not one loud block: 2.0.0 cut on any block >= 90 % and the
// device's synth song (average 55-63 %, single peaks 83-97 %, no overrun)
// lost three of four voices again and again.
//   overrun   L >= 1000 permille (an audible gap):   C := max(0, sounding - 1)
//   risk      L >= 980 permille, or a second block >= 950 within 1 s:
//                                                    C := max(F, min(C, sounding) - 1)
//   average   A >= 800 permille:                     C := max(F, C - 1)
//   deep      A >= 880 permille:                     C := max(0, C - 1)
//             with the floor F = B - 1: peaks and a high average without an
//             overrun take one voice at most; every cut starts a hold-off of
//             kHoldOffBlocks (250 ms) in which no further cut happens;
//   restore   A < 650 permille and every block < 900 permille for
//             kRestoreBlocks (2 s) in a row, and C < B: C := C + 1, the 2 s
//             count restarts (hysteresis: cut >= 800 / >= 950, restore < 650
//             / < 900).
// With no synth voice sounding nothing is cut (0 voices: no-op). Off during
// offline render (setEnabled(false)); reset() (C := B) on Stop and on project
// load.
#pragma once

#include <cstdint>

namespace ot {
namespace audio {

class CpuMeter;

// Synth voices as the governor sees them (M3 synth engine).
class IVoicePool {
public:
    virtual ~IVoicePool() = default;
    // Synth voices currently sounding (all slots; stolen ones excluded).
    virtual int sounding() const = 0;
    // Steal one voice by the §3.3 rule (release phase quietest first, then
    // quietest, then oldest); it fades out over 64 frames. False: none left.
    virtual bool stealOne() = 0;
};

class Governor {
public:
    static constexpr int kDefaultBudget = 4;            // 2x-05 §3 (2.0.0)
    static constexpr int kMaxBudget = 16;
    static constexpr uint32_t kOverrunPermille = 1000;
    static constexpr uint32_t kRiskPermille = 950;      // a near-overrun block
    static constexpr uint32_t kRiskNowPermille = 980;   // one such block is enough
    static constexpr uint32_t kRiskWindowBlocks = 345;  // two near-overrun blocks within 1 s
    static constexpr uint32_t kCutPermille = 800;       // average
    static constexpr uint32_t kDeepPermille = 880;      // average: below the floor
    static constexpr uint32_t kRestorePermille = 650;   // average
    static constexpr uint32_t kCalmPeakPermille = 900;  // every block below it to restore
    static constexpr uint32_t kHoldOffBlocks = 86;      // 250 ms
    static constexpr uint32_t kRestoreBlocks = 690;     // 2 s
    static constexpr uint32_t kRecentBlocks = 690;      // "reduced voices in the last 2 s"

    void setPool(IVoicePool* pool) { pool_ = pool; }
    // Synth voices sounding now (0 without a pool; telemetry).
    int sounding() const { return pool_ ? pool_->sounding() : 0; }
    // Budget B (clamped 0..kMaxBudget); the cap returns to it.
    void setBudget(int b);
    int budget() const { return budget_; }
    int cap() const { return cap_; }
    bool limiting() const { return cap_ < budget_; }
    // The lowest cap a cut without an overrun (or a deep average) reaches.
    int floor() const { return budget_ > 0 ? budget_ - 1 : 0; }
    // false during offline render: evaluate() does nothing.
    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }
    // C := B, timers cleared (Stop, project load). Counters are kept.
    void reset();

    // Once per live block after render. l / a in permille, overrun: L >= 100 %.
    void evaluate(uint32_t l, uint32_t a, bool overrun);
    void evaluate(const CpuMeter& m);

    // Reductions since boot, and whether one happened in the last ~2 s.
    uint32_t cuts() const { return cuts_; }
    bool cutRecent() const { return cuts_ && sinceCut_ < kRecentBlocks; }
    // Voices stolen since boot.
    uint32_t steals() const { return steals_; }

private:
    void cutTo(int c);

    IVoicePool* pool_ = nullptr;
    int budget_ = kDefaultBudget;
    int cap_ = kDefaultBudget;
    bool enabled_ = true;
    uint32_t holdOff_ = 0;      // blocks left in the hold-off
    uint32_t calm_ = 0;         // consecutive calm blocks (A < kRestorePermille, L < kCalmPeakPermille)
    uint32_t sinceRisk_ = 0xFFFFFFFFu;   // blocks since the last block >= kRiskPermille
    uint32_t sinceCut_ = 0xFFFFFFFFu;
    uint32_t cuts_ = 0;
    uint32_t steals_ = 0;
};

}  // namespace audio
}  // namespace ot
