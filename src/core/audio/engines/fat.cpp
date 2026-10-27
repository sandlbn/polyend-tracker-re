// FAT unison synth engine (see fat.h) [OT].
//
// Units: oscillator phases run in [-1, 1) (the naive saw is the phase
// itself) with increments of 2 f / fs; envelopes 0..1, the LFO -1..1.
//
// Parameter laws (the manual gives only names and ranges; the laws are ours,
// docs/specs/2x-07-fat-engine.md):
//   - Fatness F: the centre saw plus pairs k = 1..3 at +-k d cents,
//     d = 4 + 16 F; pair k's level w_k = 0.8 sqrt(clamp(3F - (k - 1), 0, 1))
//     (pair 1 sounds for F > 0, pair 2 above 1/3, pair 3 above 2/3, each
//     fading in from 0), the stack scaled by 1 / sqrt(1 + 2 sum w_k^2) (equal
//     power for uncorrelated phases).
//   - Timbre T: every saw becomes saw(ph) - T saw(ph + 1/2) (T = 1: a 50 %
//     pulse, odd harmonics only), scaled by 1 / sqrt(1 + T + T^2) (equal
//     power).
//   - Osc LFO Amt A: the LFO moves the stack's pitch by 12 A^2 semitones.
//   - Reset On: a non-legato note restarts the stack at the fixed spread
//     ph_k = k / 7; Off: the phases run free (a fresh voice starts at the
//     spread too, so the first note of a voice always sounds the same).
//   - Brightness B: one-pole low pass at 20 kHz x 2^(-2.25 (1 - B)) (1: off,
//     0: 4.2 kHz, -1: 880 Hz); Noise N: white noise at 0.35 N; both feed the
//     filter.
//   - Filter Type: VAP's types 0..2. LP MG 24 = two TPT state-variable
//     stages, the first with two coincident poles (damping 2) and a driven
//     input (1.2 - 0.6 r); LP OB 24 = a Butterworth-like first stage (damping
//     1.848) and a soft clip after the last; LP OB 12 = one stage + soft clip.
//     Cutoff / Resonance: g = tan(pi fc / fs), r = Resonance / 1.5, damping of
//     the resonant stage k = 2 (1 - r)^2 + 0.02 (Q 0.5 .. 50).
//   - Filter Env Amt / Filter LFO Amt: 10 x sign(a) x sqrt(|a|) octaves at
//     full envelope / LFO (ACD's law). No key tracking.
//   - Envelopes: linear attack, exponential decay / release reaching 1 % of
//     the distance in the stage time (ACD's law). The voice ends when the
//     amplitude envelope falls below -80 dB in its release (or decays to a 0
//     sustain).
//   - LFO: bipolar shapes; Free Running shares one phase per slot, One Shot
//     runs one cycle from the note start and holds its end value, Note
//     restarts with every (non-legato) note.
//   - Glide: constant time, linear in semitones. Always / Legato glide from
//     the slot's previous note; Overlap / Legato Overlap only from a held
//     note. The Legato modes do not retrigger envelopes / LFO on overlap.
//
// Per block a voice: (1) steps envelopes, LFO and glide by one block,
// (2) evaluates its destinations (stack pitch, cutoff, amplitude) at the
// block end, (3) renders 128 frames ramping each destination from the last
// block's end (kept in the voice) to the new one, so every ramp is
// continuous. Destinations that depend on the patch only (stack levels,
// Timbre, Brightness, Noise, resonance) are evaluated once per block per
// slot in prepare(), at p.start and p.end.
//
// The sample loop is split into passes over a stack buffer: the oscillator
// stack (a kernel per number of sounding pairs), the filter's first stage
// with Brightness and Noise fused (24 dB types), and the last stage adding
// y x amplitude into L / R (fused with Brightness / Noise for LP OB 12).
//
// Cost (tests/audio_bench.cpp, M4 instructions per voice per block incl.
// prepare): Init 9.2k, worst (seven saws with Timbre, Brightness, noise, LP OB
// 24, S&H LFO, glide; at pitch ~108) 18.7k.
#include "audio/engines/fat.h"

#include <cstring>

#include "audio/dsp.h"

// Per-block (not per-sample) code is built for size: flash matters more than
// the few hundred instructions per block it runs.
#if defined(__GNUC__) && !defined(__clang__)
#define FAT_PER_BLOCK __attribute__((optimize("Os")))
#else
#define FAT_PER_BLOCK
#endif

namespace ot {
namespace audio {
namespace fat {

namespace {

constexpr float kBlockT = float(kBlock) / kSampleRate;   // seconds per block
constexpr float kInvFs = 1.0f / kSampleRate;
constexpr float kInvBlock = 1.0f / float(kBlock);
constexpr float kPi = 3.14159265f;
constexpr int kOscs = 7;               // centre + 3 pairs
constexpr float kMaxDt = 0.86f;        // centre phase increment ceiling (one wrap per frame, pairs included)
constexpr float kDoneLevel = 1.0e-4f;  // -80 dB: the voice is finished
constexpr float kOutTrim = 0.39f;      // voice output level (Init ~ ACD's Init, tests/synth_levels_test.cpp)
constexpr float kPairLevel = 0.8f;     // pair level at full Fatness (relative to the centre)
constexpr float kNoiseLevel = 0.35f;   // noise at Noise = 1
constexpr float kStage1K = 1.848f;     // first-stage damping: OB 24 (Butterworth-like)
constexpr float kStage1KMG = 2.0f;     // ... MG 24 (two coincident poles)

// ---------------------------------------------------------------------------
// Small math (no libm in the audio path).

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float absf(float x) { return __builtin_fabsf(x); }
inline float sqrtf_(float x) { return __builtin_sqrtf(x); }   // VSQRT on the M4
inline int enumOf(float x, int max) {   // macro-pushed enums: round, clamp
    const int i = int(x + 0.5f);
    return i < 0 ? 0 : (i > max ? max : i);
}

// 2^x for |x| <= 60: integer part in the exponent, 2^f on f in [-0.5, 0.5]
// by its Taylor series to the 5th power (relative error < 3e-6).
FAT_PER_BLOCK float fastExp2(float x) {
    x = clampf(x, -60.0f, 60.0f);
    const int i = int(x + (x >= 0.0f ? 0.5f : -0.5f));
    const float f = x - float(i);
    const float p = 1.0f + f * (0.693147181f + f * (0.240226507f + f * (0.0555041087f + f * (0.00961812911f + f * 0.00133335581f))));
    const uint32_t b = uint32_t(i + 127) << 23;
    float s;
    std::memcpy(&s, &b, 4);
    return p * s;
}

// tan(x) for 0 <= x <= 1.42 (rational approximation; 3 % low at the top end,
// i.e. a slightly low cutoff near Nyquist only). VAP's law.
inline float fastTan(float x) {
    const float x2 = x * x;
    return x * (15.0f - x2) / (15.0f - 6.0f * x2);
}

inline uint32_t lcg(uint32_t& r) {
    r = r * 1664525u + 1013904223u;
    return r;
}
inline float rnd(uint32_t& r) { return float(int32_t(lcg(r))) * (1.0f / 2147483648.0f); }   // -1..1

// Soft clip: unity slope at 0, flat at +-3 -> +-2.
inline float softClip2(float x) {
    x = 0.5f * (absf(x + 3.0f) - absf(x - 3.0f));   // clamp to +-3 without branches
    return x - (1.0f / 27.0f) * x * x * x;
}

// Bipolar LFO shape at phase ph (0..1; 1 = the end of a One Shot cycle).
FAT_PER_BLOCK float lfoShape(int wave, float ph, float sh) {
    switch (wave) {
        case kTri: return ph < 0.25f ? 4.0f * ph : (ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f);
        case kSine: {   // parabola with one refinement step (< 0.1 %)
            const float u = ph < 0.5f ? 2.0f * ph : 2.0f * ph - 2.0f;   // -1..1
            const float y = 4.0f * u * (1.0f - absf(u));
            return y + 0.225f * (y * absf(y) - y);
        }
        case kRampUp: return ph + ph - 1.0f;
        case kRampDown: return 1.0f - ph - ph;
        case kSquare: return ph < 0.5f ? 1.0f : -1.0f;
        default: return sh;
    }
}

// ---------------------------------------------------------------------------
// Envelopes (per block).

struct EnvCoef {
    float aInc;   // attack increment per block
    float dK;     // decay factor per block
    float sus;    // sustain level
    float rK;     // release factor per block
};

// Exponential factor per block reaching 1 % of the distance in t seconds.
inline float stageK(float t) {
    if (t < 1.0e-4f) return 0.0f;
    return fastExp2(-6.6438562f * kBlockT / t);   // 4.6 / ln 2
}

FAT_PER_BLOCK EnvCoef envCoef(const float* e, int base) {
    EnvCoef c;
    const float a = e[base];
    c.aInc = a <= kBlockT ? 1.0f : kBlockT / a;
    c.dK = stageK(e[base + 1]);
    c.sus = clampf(e[base + 2], 0.0f, 1.0f);
    c.rK = stageK(e[base + 3]);
    return c;
}

enum EnvStage : uint8_t { kIdle = 0, kAttack, kDecay, kRelease };

FAT_PER_BLOCK void envStep(float& lv, uint8_t& st, const EnvCoef& e) {
    switch (st) {
        case kAttack:
            lv += e.aInc;
            if (lv >= 1.0f) {
                lv = 1.0f;
                st = kDecay;
            }
            break;
        case kDecay: lv = e.sus + (lv - e.sus) * e.dK; break;
        case kRelease: lv *= e.rK; break;
        default: break;
    }
}

// Modulation depth law: 10 x sign(a) x sqrt(|a|) octaves.
inline float octaves(float a) {
    a = clampf(a, -1.0f, 1.0f);
    return (a < 0.0f ? -10.0f : 10.0f) * sqrtf_(absf(a));
}

// ---------------------------------------------------------------------------
// Engine state.

// Patch-only destinations at one instant (block start or end).
struct OscDest {
    float w[4];    // centre / pair 1..3 levels (normalisation, filter drive included)
    float tb;      // Timbre
    float tone;    // Brightness one-pole coefficient G (1 = off)
    float noise;   // noise level (filter drive included)
    float k;       // damping of the resonant filter stage
    float st;      // [OT] Stereo 0..1 (0: the mono engine)
};

// Slot scratch (per-block caches shared by the slot's voices).
struct SlotState {
    uint32_t serial;
    uint8_t valid;
    uint8_t hasLast;   // lastPitch is set (glide source of a fresh voice)
    uint8_t pairs;     // pairs sounding this block (0..3)
    uint8_t tone;      // Brightness < 1 at the block start or end
    uint8_t noise;     // Noise > 0 at the block start or end
    uint8_t ftype, wave, retrig, glideMode, reset, pad[2];
    uint32_t rng;
    EnvCoef amp, fenv;
    float lfoInc;          // LFO cycles per block
    float freePh, freeVal, freeSh;
    float envOct, lfoOct;  // filter modulation depths (octaves at full scale)
    float vib;             // stack pitch at full LFO (semitones)
    float cutoff;          // Hz
    float glideBlocks;     // glide time in blocks (0 = off)
    float lastPitch;       // the slot's previous note (glide source)
    float ratio[kOscs], invRatio[kOscs];   // increment ratio per stack oscillator (p.end)
    OscDest d0, d1;                        // at p.start / p.end
};
static_assert(sizeof(SlotState) <= kSynthSlotScratchBytes, "FAT slot scratch");

enum VoiceFlag : uint8_t { kLastValid = 1, kStarted = 2, kLfoDone = 4 };

struct VoiceState {
    float ph[kOscs];   // stack phases (-1..1): centre, then pairs (+, -)
    float pitch;       // glided note pitch (semitones)
    float target;      // glide target
    float glide;       // semitones per block (0 = arrived)
    float amp, fenv;   // envelope levels
    float lfoPh, lfoSh;
    float lastDt;      // centre increment at the end of the last block
    float lastG;       // filter tan(pi fc / fs) ...
    float lastAmp;     // output amplitude ...
    float tz;          // Brightness filter state
    float f[4];        // filter states: first stage s1, s2; last stage s1, s2
    float tzR, fR[4];  // [OT] Stereo: the R side's (f / tz are the L side's); copies while mono
    uint32_t rng;
    uint8_t ampSt, fenvSt, flags, pad;
};
static_assert(sizeof(VoiceState) <= kSynthVoiceStateBytes, "FAT voice state");

inline float pitchToDt(float pitch) {
    return clampf(2.0f * 440.0f * kInvFs * fastExp2((pitch - 69.0f) * (1.0f / 12.0f)), 1.0e-6f, kMaxDt);
}

inline int pairsOf(float F) { return F > 2.0f / 3.0f ? 3 : (F > 1.0f / 3.0f ? 2 : (F > 0.0f ? 1 : 0)); }

// Patch-only destinations from parameter values P (count entries).
FAT_PER_BLOCK void oscDest(const float* P, int count, OscDest& d) {
    d.st = count > kStereoParam ? clampf(P[kStereoParam], 0.0f, 1.0f) : 0.0f;
    const float F = clampf(P[kFatness], 0.0f, 1.0f);
    float sum = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float w2 = clampf(3.0f * F - float(k), 0.0f, 1.0f) * (kPairLevel * kPairLevel);
        d.w[k + 1] = sqrtf_(w2);
        sum += w2;
    }
    const float T = clampf(P[kTimbre], 0.0f, 1.0f);
    // Filter: resonance and the MG type's input drive (folded into the levels).
    const float r = clampf(P[kResonance] * (1.0f / 1.5f), 0.0f, 1.0f);
    d.k = 2.0f * (1.0f - r) * (1.0f - r) + 0.02f;
    const float in = enumOf(P[kFilterType], 2) == kLpMg24 ? 1.2f - 0.6f * r : 1.0f;
    const float norm = in / sqrtf_((1.0f + 2.0f * sum) * (1.0f + T + T * T));
    d.w[0] = norm;
    for (int k = 1; k < 4; ++k) d.w[k] *= norm;
    d.tb = T;
    const float B = clampf(P[kBrightness], -1.0f, 1.0f);
    if (B >= 1.0f) {
        d.tone = 1.0f;
    } else {
        const float g = fastTan(kPi * kInvFs * 20000.0f * fastExp2(-2.25f * (1.0f - B)));
        d.tone = g / (1.0f + g);
    }
    d.noise = kNoiseLevel * in * clampf(P[kNoise], 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Oscillator stack, written into buf.
//
// Each oscillator k runs at the centre increment x ratio[k]. Its phase p in
// [-1, 1) is kept as a half phase h in [0, 1) and a parity bit (p = h - 1
// for p < 0, p = h for p >= 0), so one compare per frame catches both of
// its events: the wrap (p: 1 -> -1, parity 1 -> 0) and the half-cycle
// point (p crosses 0, parity 0 -> 1). With Timbre T the oscillator is
//     saw(p) - T saw(p +- 1) = (1 - T) p + T sign(p)
//                            = (1 - T) h + [(1 + T) parity - 1]
// i.e. the saw's (1 - T) part follows h, and the bracket only changes at
// the events: the stack sums sum_g w'_g H_g (H_g = the half phases of level
// group g, w' = (1 - T) w) plus a constant K = sum w ((1 + T) parity - 1)
// that each event updates. A wrap steps the output by -2 w, a half-cycle
// point by +2 T w.
//
// Band limiting is event based (as VAP's oscillators): an event adds the
// second half of its polyBLEP residual to the current frame and the first
// half to the previous one (t = time since the event in frames; before the
// step t^2 / 2, after -(1 - t)^2 / 2, x the step). The previous frame of
// the block's first one belongs to the last block, which predicted the event
// and corrected its own last frame.
//
// Levels and Timbre step in 8 segments of 16 frames (1/8 of the block's
// change each, where K is rebuilt); the increment ramps per frame.

struct StackRamp {
    float dt, ddt, idt;   // centre increment, its per-frame step, 1 / mean centre increment
    float w0[4], w1[4];   // centre / pair levels at the block start / end
    float tb0, tb1;       // Timbre at the block start / end
    const float* ratio;
    const float* invRatio;
};

constexpr int kSegs = 8;
constexpr uint32_t kSegLen = kBlock / kSegs;

template <int kPairs>
OT_FASTRUN_FN void stackKernel(float* buf, float* phase, const StackRamp& o) {
    constexpr int kN = 1 + 2 * kPairs;
    float h[kN], r[kN], ir[kN];
    uint32_t par = 0;
    for (int k = 0; k < kN; ++k) {
        const float p = phase[k];
        if (p >= 0.0f) {
            h[k] = p;
            par |= 1u << k;
        } else {
            h[k] = p + 1.0f;
        }
        r[k] = o.ratio[k];
        ir[k] = o.invRatio[k] * o.idt;
    }
    float dt = o.dt;
    const float ddt = o.ddt;
    float sink = 0.0f;
    float* prev = &sink;
    float* out = buf;
    float w[4], tb = o.tb0;
    for (int seg = 0; seg < kSegs; ++seg) {
        // Segment values at the segment's middle.
        const float f = (float(seg) + 0.5f) * (1.0f / float(kSegs));
        for (int g = 0; g <= kPairs; ++g) w[g] = o.w0[g] + (o.w1[g] - o.w0[g]) * f;
        tb = o.tb0 + (o.tb1 - o.tb0) * f;
        const float up = 1.0f + tb, saw = 1.0f - tb;
        float K = 0.0f;
        for (int k = 0; k < kN; ++k) K += w[(k + 1) >> 1] * ((par >> k) & 1u ? tb : -1.0f);
        const float v0 = saw * w[0], v1 = kPairs > 0 ? saw * w[1] : 0.0f, v2 = kPairs > 1 ? saw * w[2] : 0.0f,
                    v3 = kPairs > 2 ? saw * w[3] : 0.0f;
        for (uint32_t i = 0; i < kSegLen; ++i) {
            float y = 0.0f;
#pragma GCC unroll 7
            for (int k = 0; k < kN; ++k) {
                h[k] += dt * r[k];
                if (h[k] >= 1.0f) {
                    h[k] -= 1.0f;
                    const float t = h[k] * ir[k], u = 1.0f - t, a = w[(k + 1) >> 1];
                    par ^= 1u << k;
                    if ((par >> k) & 1u) {   // half-cycle point: +2 T w
                        const float b = tb * a;
                        y -= b * u * u;
                        *prev += b * t * t;
                        K += up * a;
                    } else {                 // wrap: -2 w
                        y += a * u * u;
                        *prev -= a * t * t;
                        K -= up * a;
                    }
                }
            }
            y += K + v0 * h[0];
            if (kPairs > 0) y += v1 * (h[1] + h[2]);
            if (kPairs > 1) y += v2 * (h[3] + h[4]);
            if (kPairs > 2) y += v3 * (h[5] + h[6]);
            *out = y;
            prev = out++;
            dt += ddt;
        }
    }
    // Events of the next frame (the next block's first): their first halves.
    float fix = 0.0f;
    for (int k = 0; k < kN; ++k) {
        const float a = w[(k + 1) >> 1], d = dt * r[k];
        if (h[k] + d >= 1.0f) {
            const float t = (h[k] + d - 1.0f) * ir[k];
            fix += ((par >> k) & 1u ? -a : tb * a) * t * t;
        }
        phase[k] = (par >> k) & 1u ? h[k] : h[k] - 1.0f;
    }
    buf[kBlock - 1] += fix;
}

// Two kernels (SRAM_L room, docs/OPTIMIZATION_2X.md item 6): no or one pair
// (the Init patch), two or three pairs; a silent pair runs at level 0.
using StackFn = void (*)(float*, float*, const StackRamp&);
constexpr StackFn kStackFns[4] = {stackKernel<1>, stackKernel<1>, stackKernel<3>, stackKernel<3>};

// [OT] Stereo: stackKernel's mono sum M into buf (the same arithmetic) and
// the pair difference D = sum (up - down) into bufD: every oscillator's
// terms again with sign s_k (centre 0, up +1, down -1). In flash, not
// SRAM_L: it runs only while Stereo > 0.
template <int kPairs>
void stackKernelSt(float* buf, float* bufD, float* phase, const StackRamp& o) {
    constexpr int kN = 1 + 2 * kPairs;
    float h[kN], r[kN], ir[kN], sg[kN];
    uint32_t par = 0;
    for (int k = 0; k < kN; ++k) {
        const float p = phase[k];
        if (p >= 0.0f) {
            h[k] = p;
            par |= 1u << k;
        } else {
            h[k] = p + 1.0f;
        }
        r[k] = o.ratio[k];
        ir[k] = o.invRatio[k] * o.idt;
        sg[k] = k == 0 ? 0.0f : ((k & 1) ? 1.0f : -1.0f);
    }
    float dt = o.dt;
    const float ddt = o.ddt;
    float sink = 0.0f, sinkD = 0.0f;
    float* prev = &sink;
    float* prevD = &sinkD;
    float* out = buf;
    float* outD = bufD;
    float w[4], tb = o.tb0;
    for (int seg = 0; seg < kSegs; ++seg) {
        const float f = (float(seg) + 0.5f) * (1.0f / float(kSegs));
        for (int g = 0; g <= kPairs; ++g) w[g] = o.w0[g] + (o.w1[g] - o.w0[g]) * f;
        tb = o.tb0 + (o.tb1 - o.tb0) * f;
        const float up = 1.0f + tb, saw = 1.0f - tb;
        float K = 0.0f, Kd = 0.0f;
        for (int k = 0; k < kN; ++k) {
            const float e = w[(k + 1) >> 1] * ((par >> k) & 1u ? tb : -1.0f);
            K += e;
            Kd += sg[k] * e;
        }
        const float v0 = saw * w[0], v1 = kPairs > 0 ? saw * w[1] : 0.0f, v2 = kPairs > 1 ? saw * w[2] : 0.0f,
                    v3 = kPairs > 2 ? saw * w[3] : 0.0f;
        for (uint32_t i = 0; i < kSegLen; ++i) {
            float y = 0.0f, yd = 0.0f;
#pragma GCC unroll 7
            for (int k = 0; k < kN; ++k) {
                h[k] += dt * r[k];
                if (h[k] >= 1.0f) {
                    h[k] -= 1.0f;
                    const float t = h[k] * ir[k], u = 1.0f - t, a = w[(k + 1) >> 1];
                    par ^= 1u << k;
                    // Event terms: e into M; +e (up, odd k) / -e (down) into D.
                    float ey, ep, eK;
                    if ((par >> k) & 1u) {   // half-cycle point: +2 T w
                        const float b = tb * a;
                        ey = -b * u * u;
                        ep = b * t * t;
                        eK = up * a;
                    } else {                 // wrap: -2 w
                        ey = a * u * u;
                        ep = -a * t * t;
                        eK = -up * a;
                    }
                    y += ey;
                    *prev += ep;
                    K += eK;
                    if (k != 0) {
                        if (k & 1) {
                            yd += ey;
                            *prevD += ep;
                            Kd += eK;
                        } else {
                            yd -= ey;
                            *prevD -= ep;
                            Kd -= eK;
                        }
                    }
                }
            }
            y += K + v0 * h[0];
            yd += Kd;
            if (kPairs > 0) {
                y += v1 * (h[1] + h[2]);
                yd += v1 * (h[1] - h[2]);
            }
            if (kPairs > 1) {
                y += v2 * (h[3] + h[4]);
                yd += v2 * (h[3] - h[4]);
            }
            if (kPairs > 2) {
                y += v3 * (h[5] + h[6]);
                yd += v3 * (h[5] - h[6]);
            }
            *out = y;
            *outD = yd;
            prev = out++;
            prevD = outD++;
            dt += ddt;
        }
    }
    float fix = 0.0f, fixD = 0.0f;
    for (int k = 0; k < kN; ++k) {
        const float a = w[(k + 1) >> 1], d = dt * r[k];
        if (h[k] + d >= 1.0f) {
            const float t = (h[k] + d - 1.0f) * ir[k];
            const float c = ((par >> k) & 1u ? -a : tb * a) * t * t;
            fix += c;
            fixD += sg[k] * c;
        }
        phase[k] = (par >> k) & 1u ? h[k] : h[k] - 1.0f;
    }
    buf[kBlock - 1] += fix;
    bufD[kBlock - 1] += fixD;
}

using StackStFn = void (*)(float*, float*, float*, const StackRamp&);
constexpr StackStFn kStackStFns[4] = {stackKernelSt<1>, stackKernelSt<1>, stackKernelSt<3>, stackKernelSt<3>};

// ---------------------------------------------------------------------------
// Filter kernels: TPT state-variable stages (Simper's form, VAP's code) over
// buf, low-pass output.
//   kTone / kNoise: Brightness one-pole (TPT, G = g / (1 + g)) and noise at
//                   the stage's input (the filter's first pass).
//   kClip: soft clip after the stage (OB).
//   kOut: add y x gain into L / R instead of writing buf.

struct SvfRamp {
    float a1, da1, a2, da2, a3, da3;
};

// Coefficients ramping from (g0, k0) to (g1, k1) across the block.
inline SvfRamp svfRamp(float g0, float k0, float g1, float k1) {
    const float a10 = 1.0f / (1.0f + g0 * (g0 + k0)), a11 = 1.0f / (1.0f + g1 * (g1 + k1));
    const float a20 = g0 * a10, a21 = g1 * a11;
    const float a30 = g0 * a20, a31 = g1 * a21;
    return SvfRamp{a10, (a11 - a10) * kInvBlock, a20, (a21 - a20) * kInvBlock, a30, (a31 - a30) * kInvBlock};
}

// Filter input: Brightness coefficient and noise level (ramped), their state.
struct InRamp {
    float tone, dtone;
    float noise, dnoise;
    float tz;
    uint32_t rng;
};

template <bool kTone, bool kNoise, bool kClip, bool kOut>
OT_FASTRUN_FN void svfKernel(float* buf, float* s, SvfRamp c, InRamp* in, float* L, float* R, float g, float dg) {
    float s1 = s[0], s2 = s[1];
    float tz = kTone ? in->tz : 0.0f, tone = kTone ? in->tone : 0.0f, nl = kNoise ? in->noise : 0.0f;
    uint32_t rng = kNoise ? in->rng : 0u;
    for (uint32_t i = 0; i < kBlock; ++i) {
        float x = buf[i];
        if (kTone) {
            const float v = (x - tz) * tone;
            x = v + tz;
            tz = x + v;
            tone += in->dtone;
        }
        if (kNoise) {
            rng = rng * 1664525u + 1013904223u;
            x += nl * float(int32_t(rng)) * (1.0f / 2147483648.0f);
            nl += in->dnoise;
        }
        const float v3 = x - s2;
        const float v1 = c.a1 * s1 + c.a2 * v3;
        const float v2 = s2 + c.a2 * s1 + c.a3 * v3;
        s1 = 2.0f * v1 - s1;
        s2 = 2.0f * v2 - s2;
        float y = v2;
        if (kClip) y = softClip2(y);
        if (kOut) {
            const float o = y * g;
            L[i] += o;
            R[i] += o;
            g += dg;
        } else {
            buf[i] = y;
        }
        c.a1 += c.da1;
        c.a2 += c.da2;
        c.a3 += c.da3;
    }
    s[0] = s1;
    s[1] = s2;
    if (kTone) in->tz = tz;
    if (kNoise) in->rng = rng;
}

using SvfFn = void (*)(float*, float*, SvfRamp, InRamp*, float*, float*, float, float);
// First stage of a 24 dB type [Brightness][noise].
constexpr SvfFn kSvfFirst[2][2] = {
    {svfKernel<false, false, false, false>, svfKernel<false, true, false, false>},
    {svfKernel<true, false, false, false>, svfKernel<true, true, false, false>},
};
// Last stage of a 24 dB type, into L / R [OB soft clip].
constexpr SvfFn kSvfLast[2] = {svfKernel<false, false, false, true>, svfKernel<false, false, true, true>};
// LP OB 12: the only stage, into L / R [Brightness][noise].
constexpr SvfFn kSvfOnly[2][2] = {
    {svfKernel<false, false, true, true>, svfKernel<false, true, true, true>},
    {svfKernel<true, false, true, true>, svfKernel<true, true, true, true>},
};

// ---------------------------------------------------------------------------

class FatEngine final : public ISynthEngine {
public:
    int32_t id() const override { return kSynthEngineFat; }
    const char* name() const override { return "FAT"; }

    void slotReset(SynthSlotScratch& sc) override {
        std::memset(sc.bytes, 0, sizeof(sc.bytes));
        SlotState& S = sc.as<SlotState>();
        S.rng = 0x2545F491u;
    }

    FAT_PER_BLOCK void prepare(SynthSlotScratch& sc, const SynthParams& p) override {
        if (p.count < kParamCount) return;
        SlotState& S = sc.as<SlotState>();
        if (!S.valid || S.serial != p.serial) refresh(S, p);
        // Patch-only destinations: the block end is cached by refresh(); the
        // start differs only while a value moves.
        if (p.moving) oscDest(p.start, p.count, S.d0);
        else S.d0 = S.d1;
        const int pairs0 = pairsOf(p.start[kFatness]), pairs1 = pairsOf(p.end[kFatness]);
        S.pairs = uint8_t(pairs0 > pairs1 ? pairs0 : pairs1);
        S.tone = S.d0.tone < 1.0f || S.d1.tone < 1.0f;
        S.noise = S.d0.noise > 0.0f || S.d1.noise > 0.0f;
        // Free-running LFO: one phase per slot.
        S.freePh += S.lfoInc;
        if (S.freePh >= 1.0f) {
            S.freePh -= float(int(S.freePh));
            S.freeSh = rnd(S.rng);
        }
        S.freeVal = lfoShape(S.wave, S.freePh, S.freeSh);
    }

    FAT_PER_BLOCK void noteOn(SynthVoice& v, const SynthParams& p, const SynthNote& n) override {
        if (p.count < kParamCount) return;
        SlotState& S = v.slotScratch->as<SlotState>();
        VoiceState& V = v.as<VoiceState>();
        if (!S.valid || S.serial != p.serial) refresh(S, p);
        const bool fresh = !(V.flags & kStarted);
        if (fresh) {
            V.rng = (lcg(S.rng) ^ 0x9E3779B9u) | 1u;
            V.flags |= kStarted;
        }
        // Glide (Always / Legato: from the slot's previous note; Overlap
        // modes: only from a held note). A Glide FX owns the start pitch.
        const int mode = S.glideMode;
        bool hasFrom = false;
        float from = n.pitch;
        if (n.reused) {
            hasFrom = true;
            from = V.pitch;
        } else if (S.hasLast) {
            hasFrom = true;
            from = S.lastPitch;
        }
        V.target = n.pitch;
        const bool always = mode == kAlways || mode == kLegato;
        if (S.glideBlocks > 0.0f && hasFrom && !n.fxGlide && (always || n.overlap) && from != n.pitch) {
            V.pitch = from;
            V.glide = absf(n.pitch - from) / S.glideBlocks;
        } else {
            V.pitch = n.pitch;
            V.glide = 0.0f;
        }
        const bool legato = (mode == kLegato || mode == kLegatoOverlap) && n.overlap;
        if (!legato || V.ampSt == kIdle) {
            V.ampSt = kAttack;
            V.fenvSt = kAttack;
            if (S.retrig != kFree) {
                V.lfoPh = 0.0f;
                V.flags &= uint8_t(~kLfoDone);
                V.lfoSh = rnd(V.rng);
            }
            // Reset On (or a fresh voice): the stack restarts at the fixed spread.
            if (fresh || S.reset)
                for (int k = 0; k < kOscs; ++k) V.ph[k] = float(2 * k) * (1.0f / float(kOscs)) - 1.0f;
        }
        S.lastPitch = n.pitch;
        S.hasLast = 1;
        v.level = V.amp;
    }

    void noteOff(SynthVoice& v, const SynthParams& p) override {
        (void)p;
        VoiceState& V = v.as<VoiceState>();
        if (V.ampSt != kIdle) V.ampSt = kRelease;
        if (V.fenvSt != kIdle) V.fenvSt = kRelease;
    }

    bool render(SynthVoice& v, const SynthParams& p, float g0, float gInc, float* L, float* R) override;

private:
    static FAT_PER_BLOCK void refresh(SlotState& S, const SynthParams& p) {
        const float* e = p.end;
        S.serial = p.serial;
        S.valid = 1;
        S.amp = envCoef(e, kAmpA);
        S.fenv = envCoef(e, kEnvA);
        S.reset = e[kReset] >= 0.5f;
        S.ftype = uint8_t(enumOf(e[kFilterType], 2));
        S.wave = uint8_t(enumOf(e[kLfoWave], 5));
        S.retrig = uint8_t(enumOf(e[kLfoRetrig], 2));
        S.glideMode = uint8_t(enumOf(e[kGlideMode], 3));
        S.lfoInc = clampf(e[kLfoHz], 0.0f, 100.0f) * kBlockT;
        S.envOct = octaves(e[kFilterEnv]);
        S.lfoOct = octaves(e[kFilterLfo]);
        const float a = clampf(e[kOscLfo], 0.0f, 1.0f);
        S.vib = 12.0f * a * a;
        S.cutoff = clampf(e[kCutoff], 20.0f, 20000.0f);
        const float gt = clampf(e[kGlideTime], 0.0f, 3.0f);
        S.glideBlocks = gt < 0.0005f ? 0.0f : (gt < kBlockT ? 1.0f : gt / kBlockT);
        // Detune: pair k at +-k d cents, d = 4 + 16 F.
        const float d = (4.0f + 16.0f * clampf(e[kFatness], 0.0f, 1.0f)) * (1.0f / 1200.0f);
        S.ratio[0] = S.invRatio[0] = 1.0f;
        for (int k = 1; k <= 3; ++k) {
            const float up = fastExp2(float(k) * d), dn = 1.0f / up;
            S.ratio[2 * k - 1] = S.invRatio[2 * k] = up;
            S.ratio[2 * k] = S.invRatio[2 * k - 1] = dn;
        }
        oscDest(e, p.count, S.d1);
    }
};

FAT_PER_BLOCK bool FatEngine::render(SynthVoice& v, const SynthParams& p, float g0, float gInc, float* L, float* R) {
    VoiceState& V = v.as<VoiceState>();
    if (V.ampSt == kIdle || p.count < kParamCount) {
        v.level = 0.0f;
        return false;
    }
    const SlotState& S = v.slotScratch->as<SlotState>();

    // 1. control: envelopes, LFO, glide -----------------------------------------------
    const float amp0 = V.amp;
    envStep(V.amp, V.ampSt, S.amp);
    envStep(V.fenv, V.fenvSt, S.fenv);
    float lfo;
    if (S.retrig == kFree) {
        lfo = S.freeVal;
    } else {
        if (!(V.flags & kLfoDone)) {
            V.lfoPh += S.lfoInc;
            if (V.lfoPh >= 1.0f) {
                if (S.retrig == kOneShot) {
                    V.lfoPh = 1.0f;
                    V.flags |= kLfoDone;
                } else {
                    V.lfoPh -= float(int(V.lfoPh));
                    V.lfoSh = rnd(V.rng);
                }
            }
        }
        lfo = lfoShape(S.wave, V.lfoPh, V.lfoSh);
    }
    if (V.glide > 0.0f) {
        const float d = V.target - V.pitch;
        if (absf(d) <= V.glide) {
            V.pitch = V.target;
            V.glide = 0.0f;
        } else {
            V.pitch += d > 0.0f ? V.glide : -V.glide;
        }
    }

    // 2. destinations at the block end ---------------------------------------------------
    const float dt1 = pitchToDt(clampf(V.pitch + v.bend + S.vib * lfo, -48.0f, 160.0f));
    const float oct = clampf(S.envOct * V.fenv + S.lfoOct * lfo, -20.0f, 20.0f);
    const float fc = clampf(S.cutoff * fastExp2(oct), 16.0f, 0.45f * kSampleRate);
    const float g1 = fastTan(kPi * fc * kInvFs);
    const float amp1 = V.amp * kOutTrim;
    if (!(V.flags & kLastValid)) {
        V.lastDt = dt1;
        V.lastG = g1;
        V.lastAmp = amp0 * kOutTrim;
        V.flags |= kLastValid;
    }

    // 3. oscillator stack ------------------------------------------------------------------
    float buf[kBlock];
    StackRamp o;
    o.dt = V.lastDt;
    o.ddt = (dt1 - V.lastDt) * kInvBlock;
    o.idt = 2.0f / (V.lastDt + dt1);
    for (int k = 0; k < 4; ++k) {
        o.w0[k] = S.d0.w[k];
        o.w1[k] = S.d1.w[k];
    }
    o.tb0 = S.d0.tb;
    o.tb1 = S.d1.tb;
    o.ratio = S.ratio;
    o.invRatio = S.invRatio;
    // [OT] Stereo: only with pairs sounding (the centre alone has no spread).
    const bool stereo = S.pairs > 0 && (S.d0.st > 0.0f || S.d1.st > 0.0f);
    float bufR[kBlock];
    if (stereo) {
        kStackStFns[S.pairs](buf, bufR, V.ph, o);
        float s = S.d0.st;
        const float ds = (S.d1.st - S.d0.st) * kInvBlock;
        for (uint32_t i = 0; i < kBlock; ++i) {
            s += ds;
            const float m = buf[i], d = s * bufR[i];
            buf[i] = m + d;    // L side
            bufR[i] = m - d;   // R side
        }
    } else {
        kStackFns[S.pairs](buf, V.ph, o);
    }

    // 4. filter (Brightness / noise in its first pass) and output -------------------------
    InRamp in{S.d0.tone, (S.d1.tone - S.d0.tone) * kInvBlock, S.d0.noise, (S.d1.noise - S.d0.noise) * kInvBlock,
              V.tz, V.rng};
    const float ga = V.lastAmp * g0, gb = amp1 * (g0 + gInc * float(kBlock));
    const float dg = (gb - ga) * kInvBlock;
    const SvfRamp res = svfRamp(V.lastG, S.d0.k, g1, S.d1.k);
    const float k1 = S.ftype == kLpMg24 ? kStage1KMG : kStage1K;
    if (!stereo) {
        if (S.ftype == kLpOb12) {
            kSvfOnly[S.tone][S.noise](buf, V.f + 2, res, &in, L, R, ga, dg);
        } else {
            kSvfFirst[S.tone][S.noise](buf, V.f, svfRamp(V.lastG, k1, g1, k1), &in, nullptr, nullptr, 0.0f, 0.0f);
            kSvfLast[S.ftype == kLpOb24](buf, V.f + 2, res, &in, L, R, ga, dg);
        }
        // The R side's state follows the L side's: Stereo starts without a step.
        V.tzR = in.tz;
        for (int k = 0; k < 4; ++k) V.fR[k] = V.f[k];
    } else {
        // Each side through its own filter (OB soft clip: not linear); the
        // kernels add into both of their outputs, so a side passes its own
        // channel twice at half gain. The noise is the same on both sides.
        InRamp inR = in;
        inR.tz = V.tzR;
        float* sides[2] = {buf, bufR};
        float* outs[2] = {L, R};
        float* st[2] = {V.f, V.fR};
        InRamp* ins[2] = {&in, &inR};
        for (int c = 0; c < 2; ++c) {
            if (S.ftype == kLpOb12) {
                kSvfOnly[S.tone][S.noise](sides[c], st[c] + 2, res, ins[c], outs[c], outs[c], 0.5f * ga, 0.5f * dg);
            } else {
                kSvfFirst[S.tone][S.noise](sides[c], st[c], svfRamp(V.lastG, k1, g1, k1), ins[c], nullptr, nullptr,
                                           0.0f, 0.0f);
                kSvfLast[S.ftype == kLpOb24](sides[c], st[c] + 2, res, ins[c], outs[c], outs[c], 0.5f * ga, 0.5f * dg);
            }
        }
        V.tzR = inR.tz;
        for (float& z : V.fR)
            if (absf(z) < 1.0e-20f) z = 0.0f;
    }
    V.tz = in.tz;
    V.rng = in.rng;
    for (float& z : V.f)
        if (absf(z) < 1.0e-20f) z = 0.0f;   // no denormals in silence
    V.lastDt = dt1;
    V.lastG = g1;
    V.lastAmp = amp1;

    v.level = V.amp;
    if (V.amp < kDoneLevel && (V.ampSt == kRelease || (V.ampSt == kDecay && S.amp.sus <= 0.0f))) {
        V.ampSt = kIdle;
        return false;
    }
    return true;
}

FatEngine gFat;

}  // namespace

ISynthEngine* engine() { return &gFat; }

}  // namespace fat
}  // namespace audio
}  // namespace ot
