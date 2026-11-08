// Small DSP building blocks for the audio engine (spec 04 §4).
//
// VolumeEnvelope is a float port of an algorithm from the PJRC Teensy Audio
// Library (effect_envelope.cpp):
//
//   Audio Library for Teensy 3.X
//   Copyright (c) 2014-2017, Paul Stoffregen, paul@pjrc.com
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy
//   of this software and associated documentation files (the "Software"), to deal
//   in the Software without restriction, including without limitation the rights
//   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//   copies of the Software, and to permit persons to whom the Software is
//   furnished to do so, subject to the following conditions:
//
//   The above copyright notice, development funding notice, and this permission
//   notice shall be included in all copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//   THE SOFTWARE.
//
// (Development of the PJRC library was funded by PJRC.COM, LLC by sales of
// Teensy and Audio Adaptor boards.)
#pragma once

#include <cstdint>

#include "audio/audio_tables.h"
#include "fastrun.h"

namespace ot {
namespace audio {

inline float clampUnit(float x) { return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x); }
// clampUnit() (same result, NaN passes through) with one compare on the
// in-range path: cheaper where the compiler keeps the rare branch out of
// line (reverb output).
inline float clampUnitFast(float x) {
    if (__builtin_expect(__builtin_fabsf(x) > 1.0f, 0)) return x < 0.0f ? -1.0f : 1.0f;
    return x;
}
// float (+-1 full scale) -> int16: truncation toward zero, saturating, NaN -> 0.
inline int16_t toS16(float x) {
#if defined(__arm__) && defined(__ARM_FP) && defined(__ARM_FEATURE_SAT)
    // Cortex-M4F: VCVT to signed 16-bit fixed point with 15 fraction bits
    // = trunc(x * 32768) saturated to int16, NaN -> 0: the same result as
    // the portable path, one instruction.
    float v = x;
    int32_t i;
    __asm__("vcvt.s16.f32 %0, %0, #15" : "+t"(v));
    __asm__("vmov %0, %1" : "=r"(i) : "t"(v));
    return int16_t(i);
#else
    float v = x * 32768.0f;
    if (!(v == v)) return 0;   // NaN guard (float->int of NaN is undefined)
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    return int16_t(v);
#endif
}
// Copy / zero loops in the audio path carry an empty asm statement so GCC
// does not turn them into memset()/memcpy() calls: the device links
// newlib-nano, whose versions move one byte at a time (and a bytewise copy
// to SDRAM would multiply the slow accesses).
#define OT_NO_LIBCALL() __asm__ volatile("")

// Zero `bytes` (a multiple of 16, 4-byte aligned) with word stores.
inline void zeroWords(void* p, uint32_t bytes) {
    uint32_t* w = static_cast<uint32_t*>(p);
    for (uint32_t n = bytes / 16u; n; --n) {
        OT_NO_LIBCALL();
        w[0] = 0;
        w[1] = 0;
        w[2] = 0;
        w[3] = 0;
        w += 4;
    }
}
inline void zeroFloats(float* p, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        OT_NO_LIBCALL();
        p[i] = 0.0f;
    }
}

// int16 sample -> float (+-1 full scale): float(v) / 32768, exact.
inline float s16ToF(int32_t v) {
#if defined(__arm__) && defined(__ARM_FP)
    float f;
    __asm__("vmov %0, %1" : "=t"(f) : "r"(v));
    __asm__("vcvt.f32.s32 %0, %0, #15" : "+t"(f));
    return f;
#else
    return float(v) * (1.0f / 32768.0f);
#endif
}
// s16ToF(toS16(x)): x rounded to the int16 grid (truncation, saturation,
// NaN -> 0), staying in a float register on the M4 (two in-place VCVTs).
inline float s16Round(float x) {
#if defined(__arm__) && defined(__ARM_FP) && defined(__ARM_FEATURE_SAT)
    float v = x;
    __asm__("vcvt.s16.f32 %0, %0, #15\n\tvcvt.f32.s16 %0, %0, #15" : "+t"(v));
    return v;
#else
    return s16ToF(toS16(x));
#endif
}
// Saturate an int32 to the int16 range (SSAT on the M4).
inline int32_t sat16(int32_t v) {
#if defined(__arm__) && defined(__ARM_FEATURE_SAT)
    __asm__("ssat %0, #16, %1" : "=r"(v) : "r"(v));
    return v;
#else
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
#endif
}

// Gain with a per-block linear ramp from the previous to the new target
// (spec 04 §4.2: increment = (new - old) / 128).
struct Ramp {
    float cur = 0.0f;
    float target = 0.0f;
    void set(float t) { target = t; }
    void jump(float t) { cur = target = t; }
    // Returns the per-sample increment for this block and the start value; the
    // ramp ends at target.
    float begin(float& start) {
        start = cur;
        const float inc = (target - cur) * (1.0f / float(kBlock));
        cur = target;
        return inc;
    }
};

// PJRC AudioEffectEnvelope (spec 04 §4.7) with the firmware extensions:
// no-attack, bypass, loop/gate (volume LFO), FORCED retrigger with pending note.
class VolumeEnvelope {
public:
    enum State : uint8_t { kIdle = 0, kDelay, kAttack, kHold, kDecay, kSustain, kRelease, kForced };
    // group() result flags
    static constexpr int kEvForcedDone = 1;  // retrigger fade finished: start the pending note now
    static constexpr int kEvIdle = 2;        // release finished: stop the player

    VolumeEnvelope() {
        // Constructor defaults (spec 04 §4.7).
        setTimes(0.0f, 10.5f, 2.5f, 35.0f, 0.5f, 300.0f);
        forcedCount_ = envMsToCount(1.0f);   // releaseNoteOn = 1 ms
    }

    void setTimes(float delayMs, float attackMs, float holdMs, float decayMs, float sustain, float releaseMs) {
        delayCount_ = envMsToCount(delayMs);
        attackCount_ = nz(envMsToCount(attackMs));
        holdCount_ = envMsToCount(holdMs);
        decayCount_ = nz(envMsToCount(decayMs));
        sustain_ = sustain < 0.0f ? 0.0f : (sustain > 1.0f ? 1.0f : sustain);
        releaseCount_ = nz(envMsToCount(releaseMs));
    }
    void setNoAttack(bool b) { noAttack_ = b; }
    void setBypass(bool b) { bypass_ = b; }
    void setLoop(bool b, bool randomLevel = false) {
        loop_ = b;
        randomLevel_ = randomLevel;
    }
    void setRandomSource(uint32_t* seed) { seed_ = seed; }

    // Returns true when the envelope entered FORCED (retrigger while sounding):
    // the caller must start the new note when group() reports kEvForcedDone.
    bool noteOn() {
        gate_ = true;
        if (state_ == kIdle || state_ == kDelay || forcedCount_ == 0) {
            start();
            return false;
        }
        if (state_ != kForced) {
            state_ = kForced;
            count_ = forcedCount_;
            inc_ = -level_ / float(count_);
        }
        return true;
    }
    void noteOff() { releaseWith(releaseCount_); }
    void releaseWith(uint16_t count) {
        gate_ = false;
        if (state_ != kRelease && state_ != kIdle && state_ != kForced) {
            state_ = kRelease;
            count_ = nz(count);
            inc_ = -level_ / float(count_);
        }
    }
    void stop() {
        state_ = kIdle;
        level_ = 0.0f;
        inc_ = 0.0f;
        gate_ = false;
    }
    bool active() const { return state_ != kIdle; }
    State state() const { return State(state_); }
    float level() const { return level_ * scale_; }

    // One 8-sample group: writes the per-sample gains into g[0..7].
    OT_FASTRUN_INLINE(envGroup) int group(float* g) {
        int ev = 0;
        while (count_ == 0) {
            if (state_ == kAttack) {
                if (holdCount_ > 0) {
                    enter(kHold, holdCount_, 1.0f, 0.0f);
                } else {
                    enterDecay();
                }
            } else if (state_ == kHold) {
                enterDecay();
            } else if (state_ == kDecay) {
                level_ = sustain_;
                if (loop_ && gate_) {
                    enter(kRelease, releaseCount_, sustain_, -sustain_ / float(releaseCount_));
                } else {
                    enter(kSustain, 0xFFFF, sustain_, 0.0f);
                }
            } else if (state_ == kSustain) {
                count_ = 0xFFFF;
            } else if (state_ == kRelease) {
                if (loop_ && gate_) {
                    level_ = 0.0f;
                    start();
                    continue;
                }
                state_ = kIdle;
                level_ = 0.0f;
                zeroFloats(g, 8);
                return ev | kEvIdle;
            } else if (state_ == kForced) {
                level_ = 0.0f;
                start();
                ev |= kEvForcedDone;
                continue;
            } else if (state_ == kDelay) {
                enterAttack();
            } else {  // idle
                zeroFloats(g, 8);
                return ev;
            }
        }
        // Linear per-sample interpolation inside the group (PJRC: mult += inc/8 first).
        const float step = inc_ * 0.125f;
        float m = level_;
        for (int i = 0; i < 8; ++i) {
            m += step;
            g[i] = m * scale_;
        }
        level_ += inc_;
        --count_;
        return ev;
    }

private:
    static uint16_t nz(uint16_t c) { return c == 0 ? 1 : c; }
    void enter(uint8_t s, uint16_t c, float lvl, float inc) {
        state_ = s;
        count_ = c;
        level_ = lvl;
        inc_ = inc;
    }
    void enterAttack() {
        if (noAttack_) {
            // Polyend "no attack": jump to HOLD at full level (spec 04 §4.7).
            level_ = 1.0f;
            if (holdCount_ > 0) enter(kHold, holdCount_, 1.0f, 0.0f);
            else enterDecay();
            return;
        }
        enter(kAttack, attackCount_, level_, 1.0f / float(attackCount_));
    }
    void enterDecay() { enter(kDecay, decayCount_, 1.0f, (sustain_ - 1.0f) / float(decayCount_)); }
    void start() {
        if (randomLevel_ && seed_) {
            // Random LFO shape: new level per cycle (spec 04 §4.8 shape 4, I).
            *seed_ = *seed_ * 1664525u + 1013904223u;
            scale_ = float(*seed_ >> 8) * (1.0f / 16777216.0f);
        } else {
            scale_ = 1.0f;
        }
        if (bypass_) {
            enter(kSustain, 0xFFFF, sustain_, 0.0f);
            return;
        }
        level_ = 0.0f;
        if (delayCount_ > 0) enter(kDelay, delayCount_, 0.0f, 0.0f);
        else enterAttack();
    }

    uint8_t state_ = kIdle;
    uint16_t count_ = 0;
    float level_ = 0.0f;
    float inc_ = 0.0f;
    float scale_ = 1.0f;
    uint16_t delayCount_ = 0, attackCount_ = 1, holdCount_ = 0, decayCount_ = 1, releaseCount_ = 1;
    uint16_t forcedCount_ = 6;
    float sustain_ = 0.5f;
    bool noAttack_ = false, bypass_ = false, loop_ = false, randomLevel_ = false, gate_ = false;
    uint32_t* seed_ = nullptr;
};

// Output stage of the sample filter: unchanged up to |y| = 0.9, then a smooth
// knee towards (never reaching) full scale, 1 - 0.01 / (|y| - 0.8): continuous
// in value and slope at 0.9. A loud resonant peak rounds off instead of being
// flat-topped by a clamp; full-scale material without resonance loses < 0.5 dB.
inline float sampleFilterSat(float y) {
    const float a = y < 0.0f ? -y : y;
    if (a <= 0.9f) return y;
    const float v = 1.0f - 0.01f / (a - 0.8f);
    return y < 0.0f ? -v : v;
}

// One sample of the sample-voice filter: a zero-delay-feedback (trapezoidal)
// state-variable filter. Exact cutoff tuning (prewarped), flat passbands up
// to the top, stable at any cutoff; 12 dB/octave. kType 0 LP, 1 HP, 2 BP
// (BP gain Q at the centre, like the earlier filter). The fused voice chains
// call this too, so every path does the same operations.
template <int kType>
inline float sampleFilterStep(float in, float& s1, float& s2, float a1, float a2, float a3, float k) {
    const float v3 = in - s2;
    const float v1 = a1 * s1 + a2 * v3;
    const float v2 = s2 + a2 * s1 + a3 * v3;
    s1 = 2.0f * v1 - s1;
    s2 = 2.0f * v2 - s2;
    const float y = kType == 0 ? v2 : (kType == 1 ? in - k * v1 - v2 : v1);
    return sampleFilterSat(y);
}

// Keeps the state bounded (only reached by a runaway input).
inline float sampleFilterBound(float s) { return s > 16.0f ? 16.0f : (s < -16.0f ? -16.0f : s); }

// The coefficient ramp across one block, from c to t (shared by every path).
struct FiltRamp {
    float a1, a2, a3, k, d1, d2, d3, dk;
    FiltRamp(const FiltCoef& c, const FiltCoef& t, uint32_t n) {
        const float inv = 1.0f / float(n);
        a1 = c.a1;
        a2 = c.a2;
        a3 = c.a3;
        k = c.k;
        d1 = (t.a1 - c.a1) * inv;
        d2 = (t.a2 - c.a2) * inv;
        d3 = (t.a3 - c.a3) * inv;
        dk = (t.k - c.k) * inv;
    }
    void step() {
        a1 += d1;
        a2 += d2;
        a3 += d3;
        k += dk;
    }
};

// The sample-voice filter (2.x). The coefficients ramp linearly across the
// block from c (end of the last block) to the target.
struct SampleFilter {
    float s1 = 0.0f, s2 = 0.0f;
    FiltCoef c;
    void reset() { s1 = s2 = 0.0f; }
    bool sameCoef(const SampleFilter& o) const {
        return c.a1 == o.c.a1 && c.a2 == o.c.a2 && c.a3 == o.c.a3 && c.k == o.c.k;
    }

    // type 0 LP, 1 HP, 2 BP. In-place on x[0..n).
    void process(float* x, uint32_t n, int type, const FiltCoef& t) {
        if (type == 0) run<0>(x, n, t);
        else if (type == 1) run<1>(x, n, t);
        else run<2>(x, n, t);
    }

private:
    template <int kType>
    void run(float* __restrict x, uint32_t n, const FiltCoef& t) {
        FiltRamp r(c, t, n);
        float z1 = s1, z2 = s2;
        for (uint32_t i = 0; i < n; ++i) {
            r.step();
            x[i] = sampleFilterStep<kType>(x[i], z1, z2, r.a1, r.a2, r.a3, r.k);
        }
        s1 = sampleFilterBound(z1);
        s2 = sampleFilterBound(z2);
        c = t;
    }
};

}  // namespace audio
}  // namespace ot
