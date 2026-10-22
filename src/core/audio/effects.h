// Send effects and master dynamics (spec 04 §5.1-§5.3).
//
// Delay and reverb keep their memory in caller-provided int16 buffers
// (SDRAM on the device), like the original (spec 04 §5.1, §5.2).
#pragma once

#include <cstdint>

#include "audio/audio_tables.h"

namespace ot {
namespace audio {

// Stereo / ping-pong delay with three int16 lines A, B, C (spec 04 §5.1).
class Delay {
public:
    static constexpr uint32_t kMemorySamples = 3 * kDelayLineLen;
    void init(int16_t* mem);          // kMemorySamples int16
    void clear();
    void setTimeSamples(uint32_t t) { time_ = t < 1 ? 1 : (t >= kDelayLineLen ? kDelayLineLen - 1 : t); }
    void setFeedbackK(uint8_t k) { k_ = k; }
    // Every change also clears the filter state of all taps (spec 07 Q9).
    void setFilter(float hz) {
        delayFilterCoefs(hz, b_coef_, c_coef_);
        xa_ = xb_ = xc_ = ya_ = yb_ = yc_ = 0;
    }
    void setPingPong(bool p) { pingpong_ = p; }
    void setDisabled(bool d) { disabled_ = d; }
    uint32_t timeSamples() const { return time_; }
    // Mono in, stereo out, kBlock samples, normalised floats. Returns false
    // when the outputs are all zero because the delay is bypassed (see below).
    //
    // CPU (OpenTracker, not in the spec): the lines live in SDRAM, so each
    // block reads/writes them as contiguous spans with 32-bit accesses
    // (ring_io). All three lines are read and written every sample, as on
    // stock, and the mode only picks the output (A/B for ping-pong, C for
    // mono), so switching ping-pong continues the echoes. When the input is digital
    // silence and every line read/write of the last delay time stayed
    // within +-kQuiet LSB (-80 dBFS), the delay is bypassed: zero output,
    // the write pointer keeps moving and zeros are written behind it until
    // the whole ring is clear.
    bool process(const float* in, float* outL, float* outR);
    bool bypassed() const { return bypass_; }
    static constexpr int32_t kQuiet = 3;   // LSB; 2^n - 1 (OR-accumulated)

private:
    int16_t* a_ = nullptr;
    int16_t* b_ = nullptr;
    int16_t* c_ = nullptr;
    uint32_t w_ = 0;
    uint32_t time_ = 22050;
    uint8_t k_ = 0;
    int32_t b_coef_ = 0, c_coef_ = 0;
    int32_t xa_ = 0, xb_ = 0, xc_ = 0;   // previous line reads
    int32_t ya_ = 0, yb_ = 0, yc_ = 0;   // previous filter outputs
    bool pingpong_ = false;
    bool disabled_ = false;
    bool bypass_ = false;
    uint32_t quiet_ = 0;                 // consecutive quiet samples (reads and writes)
    uint32_t zeroed_ = 0;                // bypassed samples zeroed behind w (stops at the ring length)
};

// Clouds-style FxEngine reverb adapted per spec 04 §5.2 (two int16 rings).
class Reverb {
public:
    static constexpr uint32_t kMemorySamples = 2 * kReverbRingLen;
    void init(int16_t* mem);          // kMemorySamples int16
    void clear();
    void setSize(float v) { krt_ = clamp01(v); }             // reverb_time
    void setDamping(float v) { klp_ = 1.0f - clamp01(v); }   // lp = 1 - damping
    void setDiffusion(float v) { kap_ = clamp01(v); }
    void setPredelay(float v) { predelay_ = int32_t(clamp01(v) * 5000.0f); }
    void setAmount(float v) { amount_ = v; }
    void setInputGain(float v) { gain_ = v; }
    // Mono send in (duplicated to L and R), stereo out. Returns false when the
    // outputs are all zero because the reverb is bypassed: input digital
    // silence and every ring write of the last full ring period (16384
    // samples, 0.37 s) within +-kQuiet LSB (OpenTracker CPU saving; the
    // ring pointer and LFO keep running). Ring accesses are done as
    // contiguous spans per block (ring_io), the taps being at fixed offsets
    // from the moving pointer.
    bool process(const float* in, float* outL, float* outR);
    float level() const { return level_; }
    bool bypassed() const { return bypass_; }
    static constexpr int32_t kQuiet = 3;

private:
    static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
    int16_t* a_ = nullptr;
    int16_t* b_ = nullptr;
    uint32_t p_ = 0;
    float krt_ = 0.7f, kap_ = 0.625f, klp_ = 0.8f;
    float amount_ = kReverbDefaultAmount, gain_ = kReverbDefaultGain;
    int32_t predelay_ = 0;
    float lp1_ = 0.0f, lp2_ = 0.0f, lp3_ = 0.0f;
    float lfoS_ = 0.0f, lfoPrev_ = 0.0f, lfoC_ = 0.0f;
    float level_ = 0.0f;
    uint32_t quiet_ = 0;   // consecutive samples whose ring writes were all quiet
    bool bypass_ = false;
    int32_t prevPre_ = 0;  // pre-delay of the previous block (0: region not written)
    // The four input all-passes (ring A offsets 0..1212) are independent
    // short delay lines: they live in this SRAM ring (same offsets, mask
    // 2047; 16384 is a multiple of 2048) instead of SDRAM.
    static constexpr uint32_t kApLen = 2048;
    int16_t ap_[kApLen] = {};
    void stepLfo(uint32_t p);
};

// Look-ahead limiter with optional side-chain (spec 04 §5.3), one channel.
class Limiter {
public:
    void setThreshold(float t) { thr_ = t; }        // normalised (LIMTHR / 32767)
    void setCoefs(float attack, float release) { ca_ = attack; cr_ = release; }
    void setDisabled(bool d) { disabled_ = d; }
    void reset();
    // sc == nullptr: normal mode; else side-chain mode (detector = sc, sc mixed back un-ducked).
    // Returns false when the output block is all zero because the limiter
    // sat at rest (silent input, empty look-ahead, envelope 0, gain 1).
    bool process(const float* x, const float* sc, float* out);
    float lastGain() const { return lastG_; }
    float meanGain() const { return meanG_; }

private:
    float thr_ = 1.0f;
    float ca_ = 1.0f, cr_ = 1.0f;
    float env_ = 0.0f, gs_ = 1.0f;
    float hist_[4] = {0, 0, 0, 0};
    uint32_t hp_ = 0;
    float lastG_ = 1.0f, meanG_ = 1.0f;
    bool disabled_ = false;
};

// Line-input dynamics DYN0/DYN1 (spec 04 §5.6), one channel: gate +
// soft-knee compressor + auto make-up + limit, on the line-in -> delay/reverb
// send path only (the direct MSUM path and the side-chain stay dry).
//
// Parameters are the ones the spec gives for the stock configuration of the
// "Audio Dynamics" library (AudioEffectDynamics, Marc Paquette): gate open at
// -32 dBFS and closed below -38 dBFS (hysteresis in between), compression
// threshold -40 dB, ratio 35, knee 6 dB, limit -3 dB, auto make-up with 6 dB
// headroom, smoothing coefficients exp(-0.0923) (attack) and exp(-0.00923)
// (release). The algorithm is written here from that published design (a
// level detector feeding dB-domain gain computers whose gains are smoothed
// one-pole per sample), not copied from the library source:
//   env  = max(|x|, env * cRel)                       peak level detector
//   gate: open if env >= -32 dB, closed if env < -38 dB, else unchanged;
//         target 0 dB (open) / -110 dB (closed), smoothed (opening: cAtt)
//   comp: target = curve(L) (soft knee, see staticGainDb), smoothed
//         (gain falling: cAtt, rising: cRel)
//   lim : min(0, -3 - (L + comp + makeup)), instantaneous
//   y    = x * 10^((gate + comp + makeup + lim) / 20)
// Fast log2/exp2 polynomials keep it to ~40 FPU ops per sample on the M4.
//
// CPU (OpenTracker): the engine calls process() only while the line input
// has a non-silent block and a non-zero send gain; otherwise idle() advances
// the state in closed form (one block of zero input) until it has settled
// (gate closed, gains at rest), after which idle() costs a single test.
class LineDynamics {
public:
    static constexpr float kGateOpenDb = -32.0f;
    static constexpr float kGateCloseDb = -38.0f;
    static constexpr float kCompThresholdDb = -40.0f;
    static constexpr float kCompRatio = 35.0f;
    static constexpr float kKneeDb = 6.0f;
    static constexpr float kLimitDb = -3.0f;
    static constexpr float kHeadroomDb = 6.0f;
    static constexpr float kFloorDb = -110.0f;   // closed gate
    static constexpr float kAttack = 0.911831559f;    // exp(-0.0923)
    static constexpr float kRelease = 0.990812466f;   // exp(-0.00923)
    // Compressor gain (dB, <= 0) for a detector level in dBFS, without make-up.
    static constexpr float staticGainDb(float levelDb) {
        constexpr float lo = kCompThresholdDb - 0.5f * kKneeDb, hi = kCompThresholdDb + 0.5f * kKneeDb;
        constexpr float s = 1.0f / kCompRatio - 1.0f;
        return levelDb <= lo ? 0.0f
                             : (levelDb >= hi ? s * (levelDb - kCompThresholdDb)
                                              : s * (levelDb - lo) * (levelDb - lo) * (0.5f / kKneeDb));
    }
    // Auto make-up: a 0 dBFS input (above the knee) comes out at -headroom.
    static constexpr float kMakeupDb = (1.0f - 1.0f / kCompRatio) * (0.0f - kCompThresholdDb) - kHeadroomDb;

    void reset() {
        env_ = 0.0f;
        gateDb_ = kFloorDb;
        compDb_ = 0.0f;
        open_ = false;
        settled_ = true;
    }
    // kBlock samples, x -> y (may not alias).
    void process(const float* x, float* y);
    // One block of zero input (no output needed: it would be zero).
    void idle() {
        if (!settled_) decay();
    }
    bool settled() const { return settled_; }
    bool gateOpen() const { return open_; }
    float gateDb() const { return gateDb_; }
    float compDb() const { return compDb_; }

private:
    void decay();
    float env_ = 0.0f;
    float gateDb_ = kFloorDb;
    float compDb_ = 0.0f;
    bool open_ = false;
    bool settled_ = true;
};

}  // namespace audio
}  // namespace ot
