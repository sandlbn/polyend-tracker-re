// VAP virtual analog poly synth engine (see vap.h) [OT].
//
// Units (spec 21 §8.4 stored values, our own scaling of the modulation):
//   - pitch destinations: amount 1 = +-1 octave per unit source;
//   - Filter Cutoff destination and Filter Env Amt: 1 = +-10 octaves;
//   - PWM: 1 = +-0.5 pulse width; Osc Mix / Shape / Noise / sustains: added;
//   - Volume destination: gain x max(0, 1 + amount x source);
//   - Filter Env Decay destination: decay time x 2^(4 x amount x source).
// Sources: envelopes 0..1, LFOs -1..1.
//
// Per block a voice: (1) takes the destinations at the block start (the last
// block's end, kept in the voice; evaluated from p.start[] after a note on),
// (2) steps glide, envelopes and LFOs by one block, (3) evaluates every
// destination from the new state and p.end[], (4) renders 128 frames ramping
// between the two, so every ramp is continuous.
//
// The sample loop is split into passes over a stack buffer, each a small
// kernel picked per block: oscillators (fused saw + saw for Init; saw /
// tri-saw / saw-pulse / general morph; osc 2 synced), noise (fused into a
// 24 dB type's first stage), filter stage(s), the last adding into L / R.
//
// Cost (tests/audio_bench.cpp, M4 instructions per voice per block incl.
// prepare): Init 9.0k, worst (sync, tri-saw + saw-pulse, noise, LP OB 24,
// six mod slots) 17.1k.
#include "audio/engines/vap.h"

#include <cstring>

#include "audio/dsp.h"

// Per-block (not per-sample) code is built for size: flash matters more than
// the few hundred instructions per block it runs.
#if defined(__GNUC__) && !defined(__clang__)
#define VAP_PER_BLOCK __attribute__((optimize("Os")))
#else
#define VAP_PER_BLOCK
#endif

namespace ot {
namespace audio {
namespace vap {

namespace {

constexpr float kOutTrim = 0.70710678f;   // -3 dB voice output (see Dest::amp)
constexpr float kBlockT = float(kBlock) / kSampleRate;   // seconds per block
constexpr float kInvFs = 1.0f / kSampleRate;
constexpr float kPi = 3.14159265f;
constexpr float kLog2e = 1.44269504f;
constexpr float kCutoffOct = 10.0f;    // Filter Env Amt / cutoff destination: 1 = 10 octaves
constexpr float kOneShotEnd = 0.99999f;

// ---------------------------------------------------------------------------
// Small math (no libm in the audio path).

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float absf(float x) { return __builtin_fabsf(x); }
inline int enumOf(float x, int max) {
    const int i = int(x + 0.5f);
    return i < 0 ? 0 : (i > max ? max : i);
}

// 2^x for |x| <= 60: integer part in the exponent, 2^f on f in [-0.5, 0.5]
// by its Taylor series to the 5th power (relative error < 3e-6).
inline float fastExp2(float x) {
    x = clampf(x, -60.0f, 60.0f);
    const int i = int(x + (x >= 0.0f ? 0.5f : -0.5f));
    const float f = x - float(i);
    const float p = 1.0f + f * (0.693147181f + f * (0.240226507f + f * (0.0555041087f + f * (0.00961812911f + f * 0.00133335581f))));
    const uint32_t b = uint32_t(i + 127) << 23;
    float s;
    std::memcpy(&s, &b, 4);
    return p * s;
}

// Per-block decay factor reaching about 1 % (-40 dB) after t seconds.
inline float blockCoef(float t) {
    if (t <= 0.0005f) return 0.0f;
    return fastExp2(-4.6f * kLog2e * kBlockT / t);
}

// tan(x) for 0 <= x <= 1.42 (rational approximation; 3 % low at the top end,
// i.e. a slightly low cutoff near Nyquist only).
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

// ---------------------------------------------------------------------------
// Engine state.

constexpr int kModSlots = 6;
constexpr int kEnvs = 3;   // 0 Amp, 1 Env 1 (filter), 2 Env 2 (wave)
enum EnvStage : uint8_t { kIdle = 0, kAttack, kDecay, kRelease };
enum LfoRetrig : int { kFree = 0, kOneShot, kNote };
enum LfoWave : int { kTri = 0, kSine, kRampUp, kRampDown, kSquare, kSampleHold };

// LFO Sync divisions (Ratio rounded, 0..15): quarter notes per LFO cycle.
constexpr float kSyncBeats[16] = {16.0f, 12.0f, 8.0f, 6.0f, 4.0f, 3.0f, 2.0f, 1.5f,
                                  1.0f, 0.75f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f, 1.0f / 12.0f};
float gTempo = 120.0f;

struct Mod {
    uint8_t src, dst;
    float a0, a1;   // amount at block start / end
};

// Slot scratch (per-block caches shared by the slot's voices).
struct SlotState {
    uint32_t serial;
    uint8_t valid;
    uint8_t hasLast;     // lastPitch is set (glide source of a fresh voice)
    uint8_t nmod;
    uint8_t ftype;       // Filter Type 0..14
    uint8_t lfoWave[2], lfoRetrig[2];
    uint8_t glideMode, oscSync, reset1, reset2;
    uint16_t dstMask;    // destinations some mod slot drives
    float envInc[kEnvs], envCd[kEnvs], envCr[kEnvs];   // attack step, decay / release factors (p.end)
    float glideCoef;
    float lfoInc[2];                  // phase per block
    float lfoPh0[2], lfoPh1[2];       // free-running phase at block start / end
    float lfoHold0[2], lfoHold1[2];   // free-running S&H value at block start / end
    uint32_t rng;
    float lastPitch;
    Mod mod[kModSlots];
};
static_assert(sizeof(SlotState) <= kSynthSlotScratchBytes, "VAP slot scratch");

// Destination values at one instant (block start or end).
struct Dest {
    float dt1, dt2;        // phase increments
    float A1, B1, C1;      // osc 1: saw, second-saw (pulse) and triangle weights (mix and filter input gain included)
    float A2, B2, C2;
    float w1, w2;          // pulse widths 0..1
    float noise;
    float g, k;            // filter: tan(pi fc / fs), damping of the resonant stage
    float amp;             // amp envelope x Volume destination
};

struct VoiceState {
    float ph1, ph2;        // oscillator phases 0..1
    float pitch, target;   // glided note pitch, its target (semitones)
    float f[4];            // filter states: stage A s1, s2; stage B s1, s2
    float env[kEnvs];
    float lfoPh[2];        // per-voice LFO phase (One Shot / Note)
    float lfoHold[2];      // per-voice S&H value
    uint32_t rng;
    Dest last;             // destinations at the end of the last block (= this block's start)
    uint8_t stage[kEnvs];
    uint8_t started;
    uint8_t lastValid;     // `last` holds the previous block's end (cleared by noteOn)
};
static_assert(sizeof(VoiceState) <= kSynthVoiceStateBytes, "VAP voice state");

// ---------------------------------------------------------------------------
// LFO waveforms (phase 0..1 -> -1..1).

__attribute__((noinline)) float lfoShape(int wave, float ph, float hold) {
    switch (wave) {
        case kTri: return ph < 0.25f ? 4.0f * ph : (ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f);
        case kSine: {
            const float u = ph < 0.5f ? 2.0f * ph : 2.0f * ph - 2.0f;   // -1..1
            const float y = 4.0f * u * (1.0f - absf(u));
            return y + 0.225f * (y * absf(y) - y);
        }
        case kRampUp: return 2.0f * ph - 1.0f;
        case kRampDown: return 1.0f - 2.0f * ph;
        case kSquare: return ph < 0.5f ? 1.0f : -1.0f;
        default: return hold;
    }
}

// ---------------------------------------------------------------------------
// Filter types (Mini manual p.158 order).

enum FMode : uint8_t { kLP = 0, kHP, kBP, kNotch };
enum FFlavour : uint8_t { kSVF = 0, kOB, kMG };
struct FType {
    uint8_t mode, flavour, poles24;
};
constexpr FType kFTypes[15] = {
    {kLP, kMG, 1},  {kLP, kOB, 1},  {kLP, kOB, 0},  {kLP, kSVF, 1}, {kLP, kSVF, 0},
    {kHP, kOB, 1},  {kHP, kOB, 0},  {kHP, kSVF, 1}, {kHP, kSVF, 0},
    {kBP, kOB, 1},  {kBP, kOB, 0},  {kBP, kSVF, 1}, {kBP, kSVF, 0},
    {kNotch, kSVF, 1}, {kNotch, kSVF, 0},
};
// Damping of the first stage of a 24 dB type: Butterworth-like for OB / SVF,
// two coincident real poles for MG (ladder-like response).
constexpr float kStage1K = 1.848f;
constexpr float kStage1KMG = 2.0f;

// ---------------------------------------------------------------------------
// Destinations.

inline void oscWeights(float shape, float gain, float& A, float& B, float& C) {
    // 0 triangle -> 0.5 saw -> 1 pulse
    if (shape < 0.5f) {
        C = (1.0f - 2.0f * shape) * gain;
        A = 2.0f * shape * gain;
        B = 0.0f;
    } else {
        const float q = (2.0f * shape - 1.0f) * gain;
        C = 0.0f;
        A = (2.0f - 2.0f * shape) * gain + q;   // saw part + pulse's first saw
        B = q;
    }
}

inline float pitchToDt(float pitch) {
    return clampf(440.0f * kInvFs * fastExp2((pitch - 69.0f) * (1.0f / 12.0f)), 1.0e-6f, 0.45f);
}

// src[]: Amp Env, Env 1, Env 2, LFO 1, LFO 2. atEnd selects the mod amount.
VAP_PER_BLOCK void evalDest(const float* P, const SlotState& S, const float* src, bool atEnd, float pitch, float cutOct, Dest& d) {
    float m[kDstCount];
    for (int j = 0; j < kDstCount; ++j) {
        OT_NO_LIBCALL();
        m[j] = 0.0f;
    }
    for (int j = 0; j < S.nmod; ++j) {
        const Mod& md = S.mod[j];
        m[md.dst] += (atEnd ? md.a1 : md.a0) * src[md.src];
    }
    const float env1 = src[kSrcEnv1], env2 = src[kSrcEnv2];
    // Oscillators.
    const float mix = clampf(P[kOscMix] + P[kMixEnvAmt] * env2 + m[kDstOscMix], -1.0f, 1.0f);
    oscWeights(clampf(P[kShape1] + m[kDstShape1], 0.0f, 1.0f), 0.5f * (1.0f - mix), d.A1, d.B1, d.C1);
    oscWeights(clampf(P[kShape2] + m[kDstShape2], 0.0f, 1.0f), 0.5f * (1.0f + mix), d.A2, d.B2, d.C2);
    d.w1 = clampf(0.5f + P[kPw1] + 0.5f * m[kDstPw1], 0.03f, 0.97f);
    d.w2 = clampf(0.5f + P[kPw2] + 0.5f * m[kDstPw2], 0.03f, 0.97f);
    const float fine = 0.01f * P[kFinetune], det = 0.005f * P[kDetune];
    d.dt1 = pitchToDt(pitch + P[kTune1] + fine - det + 12.0f * m[kDstPitch1]);
    d.dt2 = pitchToDt(pitch + P[kTune2] + fine + det + 12.0f * m[kDstPitch2]);
    d.noise = clampf(P[kNoise] + m[kDstNoise], 0.0f, 1.0f);
    // Filter.
    const float oct = kCutoffOct * (P[kFilterEnvAmt] * env1 + m[kDstCutoff]) + P[kNoteTrack] * (pitch - 60.0f) * (1.0f / 12.0f) +
                      cutOct;   // + Performance (#23)
    const float fc = clampf(P[kCutoff] * fastExp2(oct), 16.0f, 0.45f * kSampleRate);
    const float g = fastTan(kPi * fc * kInvFs);
    const float r = clampf(P[kResonance] * (1.0f / 1.5f), 0.0f, 1.0f);
    d.g = g;
    d.k = 2.0f * (1.0f - r) * (1.0f - r) + 0.02f;   // Q 0.5 .. 50
    // Filter input gain (MG: drive, both: resonance bass loss), folded into
    // the oscillator / noise levels.
    const FType ft = kFTypes[S.ftype];
    const float in = ft.flavour == kMG ? 1.2f - 0.6f * r : (ft.flavour == kSVF ? 1.0f - 0.4f * r : 1.0f);
    d.A1 *= in;
    d.B1 *= in;
    d.C1 *= in;
    d.A2 *= in;
    d.B2 *= in;
    d.C2 *= in;
    d.noise *= in;
    // Amplifier. kOutTrim: -3 dB output staging (USER_REPORTS_R2 item 3). With
    // free-running oscillators the Init patch's two equal saws sum in phase on
    // every note (until 2.0.3 a random relative phase lost 3 dB on average);
    // the trim keeps Init's average level and a 4-voice chord's headroom at
    // the default slot gain (tests/synth_levels_test.cpp).
    const float vol = 1.0f + m[kDstVolume];
    d.amp = src[kSrcAmpEnv] * (vol > 0.0f ? vol * kOutTrim : 0.0f);
}

// ---------------------------------------------------------------------------
// Oscillator kernels. kKind: 0 saw, 1 triangle + saw, 2 saw + pulse,
// 3 general. Weights / width / increment ramp linearly across the block.
//
// Morph: y = A saw(ph) - B (saw(ph - w) + 2w - 1) + C tri(ph), saw = 2ph - 1,
// tri = 1 - 4|ph - 0.5|; with A = saw part + B the B terms form a +-1 pulse.
// Band limiting is event based: each discontinuity (a phase accumulator
// wrapping) adds the second half of its polyBLEP / polyBLAMP residual to the
// current sample and the first half to the previous one (t = time since the
// event in samples, unit step: before t^2 / 2, after -(1 - t)^2 / 2; unit
// slope change: before t^3 / 6, after (1 - t)^3 / 6). The previous sample of
// the block's first frame belongs to the last block, which predicted the
// event and corrected its own last frame (predictEvents).

struct OscRamp {
    float dt, ddt, idt;
    float A, dA, B, dB, C, dC, w, dw;
};

inline OscRamp oscRamp(float dt0, float dt1, float A0, float A1, float B0, float B1, float C0, float C1, float w0, float w1) {
    constexpr float n = 1.0f / float(kBlock);
    OscRamp o;
    o.dt = dt0;
    o.ddt = (dt1 - dt0) * n;
    o.idt = 2.0f / (dt0 + dt1);
    o.A = A0;
    o.dA = (A1 - A0) * n;
    o.B = B0;
    o.dB = (B1 - B0) * n;
    o.C = C0;
    o.dC = (C1 - C0) * n;
    o.w = w0;
    o.dw = (w1 - w0) * n;
    return o;
}

constexpr float kTriBlamp = 8.0f / 6.0f;   // triangle slope change (8 dt per sample) / 6

inline float frac1(float x) {   // x in [-1, 2) -> [0, 1)
    if (x < 0.0f) x += 1.0f;
    else if (x >= 1.0f) x -= 1.0f;
    return x;
}

// Corrections for events of the next sample (end of a block): adds the
// first halves into the last frame. ph / q / h are the end states.
template <bool kSq, bool kTri>
inline float predictEvents(float ph, float q, float h, const OscRamp& o) {
    float fix = 0.0f;
    if (ph + o.dt >= 1.0f) {
        const float t = (ph + o.dt - 1.0f) * o.idt;
        fix -= o.A * t * t;
        if (kTri) fix += kTriBlamp * o.dt * o.C * t * t * t;
    }
    if (kSq && q + o.dt - o.dw >= 1.0f) {
        const float t = clampf((q + o.dt - o.dw - 1.0f) * o.idt, 0.0f, 1.0f);
        fix += o.B * t * t;
    }
    if (kTri && h + o.dt >= 1.0f) {
        const float t = (h + o.dt - 1.0f) * o.idt;
        fix -= kTriBlamp * o.dt * o.C * t * t * t;
    }
    return fix;
}

template <int kKind>
void oscKernel(float* buf, float& phase, OscRamp o) {   // adds into buf
    constexpr bool kSq = kKind == 2 || kKind == 3, kTri = kKind == 1 || kKind == 3;
    float ph = phase, q = 0.0f, h = 0.0f;
    if (kSq) q = frac1(ph - o.w);
    if (kTri) h = frac1(ph + 0.5f);
    float sink = 0.0f;
    float* prev = &sink;
    for (uint32_t i = 0; i < kBlock; ++i) {
        ph += o.dt;
        float y = 0.0f;
        if (ph >= 1.0f) {
            ph -= 1.0f;
            const float t = ph * o.idt, u = 1.0f - t;
            y = o.A * u * u;
            float pf = -o.A * t * t;
            if (kTri) {
                const float d = kTriBlamp * o.dt * o.C;
                y += d * u * u * u;
                pf += d * t * t * t;
            }
            *prev += pf;
        }
        y += o.A * (2.0f * ph - 1.0f);
        if (kSq) {
            q += o.dt - o.dw;
            if (q >= 1.0f) {
                q -= 1.0f;
                const float t = clampf(q * o.idt, 0.0f, 1.0f), u = 1.0f - t;
                y -= o.B * u * u;
                *prev += o.B * t * t;
            } else if (q < 0.0f) {   // width moving faster than the phase (very low notes)
                q += 1.0f;
            }
            y -= o.B * (2.0f * (q + o.w) - 2.0f);
        }
        if (kTri) {
            h += o.dt;
            if (h >= 1.0f) {
                h -= 1.0f;
                const float t = h * o.idt, u = 1.0f - t;
                const float d = kTriBlamp * o.dt * o.C;
                y -= d * u * u * u;
                *prev -= d * t * t * t;
            }
            y += o.C * (1.0f - 4.0f * absf(ph - 0.5f));
        }
        buf[i] += y;
        prev = &buf[i];
        o.dt += o.ddt;
        o.A += o.dA;
        if (kSq) {
            o.B += o.dB;
            o.w += o.dw;
        }
        if (kTri) o.C += o.dC;
    }
    buf[kBlock - 1] += predictEvents<kSq, kTri>(ph, q, h, o);
    phase = ph;
}

using OscFn = void (*)(float*, float&, OscRamp);
constexpr OscFn kOscFns[4] = {oscKernel<0>, oscKernel<1>, oscKernel<2>, oscKernel<3>};

// Both oscillators as saws (the Init patch): one fused loop.
OT_FASTRUN_FN void sawSawKernel(float* buf, float& phase1, float& phase2, OscRamp a, OscRamp b) {
    float p1 = phase1, p2 = phase2;
    float sink = 0.0f;
    float* prev = &sink;
    for (uint32_t i = 0; i < kBlock; ++i) {
        p1 += a.dt;
        p2 += b.dt;
        float y = 0.0f;
        if (p1 >= 1.0f) {
            p1 -= 1.0f;
            const float t = p1 * a.idt, u = 1.0f - t;
            y = a.A * u * u;
            *prev -= a.A * t * t;
        }
        if (p2 >= 1.0f) {
            p2 -= 1.0f;
            const float t = p2 * b.idt, u = 1.0f - t;
            y += b.A * u * u;
            *prev -= b.A * t * t;
        }
        y += 2.0f * (a.A * p1 + b.A * p2) - (a.A + b.A);
        buf[i] = y;
        prev = &buf[i];
        a.dt += a.ddt;
        b.dt += b.ddt;
        a.A += a.dA;
        b.A += b.dA;
    }
    buf[kBlock - 1] += predictEvents<false, false>(p1, 0.0f, 0.0f, a) + predictEvents<false, false>(p2, 0.0f, 0.0f, b);
    phase1 = p1;
    phase2 = p2;
}

// Naive morph value at phase ph (hard-sync jump height).
template <bool kSq, bool kTri>
inline float naiveOsc(float ph, const OscRamp& o) {
    float y = o.A * (2.0f * ph - 1.0f);
    if (kSq) y -= o.B * (2.0f * (frac1(ph - o.w) + o.w) - 2.0f);
    if (kTri) y += o.C * (1.0f - 4.0f * absf(ph - 0.5f));
    return y;
}

// Osc 2 hard-synced to osc 1, added into buf (osc 1 is already there). The
// loop re-runs osc 1's phase accumulator exactly as oscKernel did, so the
// resets land on its wraps: when osc 1 wrapped t samples ago osc 2 restarts
// at t dt2 and its output jumps by h = y2(0) - y2(phase at the reset), band
// limited like any other step.
template <int kKind>
__attribute__((noinline)) void syncKernel(float* buf, float ph1, const OscRamp& a, float& phase2, OscRamp o) {
    constexpr bool kSq = kKind == 2 || kKind == 3, kTri = kKind == 1 || kKind == 3;
    float p1 = ph1, dt1 = a.dt;
    const float ddt1 = a.ddt, idt1 = a.idt;
    float ph = phase2, q = 0.0f, h = 0.0f;
    if (kSq) q = frac1(ph - o.w);
    if (kTri) h = frac1(ph + 0.5f);
    float sink = 0.0f;
    float* prev = &sink;
    for (uint32_t i = 0; i < kBlock; ++i) {
        p1 += dt1;
        dt1 += ddt1;
        float y;
        if (p1 >= 1.0f) {
            p1 -= 1.0f;
            const float t = p1 * idt1, u = 1.0f - t;
            const float hgt = naiveOsc<kSq, kTri>(0.0f, o) - naiveOsc<kSq, kTri>(frac1(ph + o.dt * u), o);
            ph = t * o.dt;
            if (kSq) q = frac1(ph - o.w);
            if (kTri) h = frac1(ph + 0.5f);
            y = naiveOsc<kSq, kTri>(ph, o) - 0.5f * hgt * u * u;
            *prev += 0.5f * hgt * t * t;
        } else {
            ph += o.dt;
            y = 0.0f;
            if (ph >= 1.0f) {
                ph -= 1.0f;
                const float t = ph * o.idt, u = 1.0f - t;
                y = o.A * u * u;
                float pf = -o.A * t * t;
                if (kTri) {
                    const float d = kTriBlamp * o.dt * o.C;
                    y += d * u * u * u;
                    pf += d * t * t * t;
                }
                *prev += pf;
            }
            y += o.A * (2.0f * ph - 1.0f);
            if (kSq) {
                q += o.dt - o.dw;
                if (q >= 1.0f) {
                    q -= 1.0f;
                    const float t = clampf(q * o.idt, 0.0f, 1.0f), u = 1.0f - t;
                    y -= o.B * u * u;
                    *prev += o.B * t * t;
                } else if (q < 0.0f) {
                    q += 1.0f;
                }
                y -= o.B * (2.0f * (q + o.w) - 2.0f);
            }
            if (kTri) {
                h += o.dt;
                if (h >= 1.0f) {
                    h -= 1.0f;
                    const float t = h * o.idt, u = 1.0f - t;
                    const float d = kTriBlamp * o.dt * o.C;
                    y -= d * u * u * u;
                    *prev -= d * t * t * t;
                }
                y += o.C * (1.0f - 4.0f * absf(ph - 0.5f));
            }
        }
        buf[i] += y;
        prev = &buf[i];
        o.dt += o.ddt;
        o.A += o.dA;
        if (kSq) {
            o.B += o.dB;
            o.w += o.dw;
        }
        if (kTri) o.C += o.dC;
    }
    if (p1 + dt1 >= 1.0f) {   // the next frame resets osc 2
        const float t = (p1 + dt1 - 1.0f) * idt1;
        const float hgt = naiveOsc<kSq, kTri>(0.0f, o) - naiveOsc<kSq, kTri>(frac1(ph + o.dt * (1.0f - t)), o);
        buf[kBlock - 1] += 0.5f * hgt * t * t;
    } else {
        buf[kBlock - 1] += predictEvents<kSq, kTri>(ph, q, h, o);
    }
    phase2 = ph;
}

using SyncFn = void (*)(float*, float, const OscRamp&, float&, OscRamp);
constexpr SyncFn kSyncFns[4] = {syncKernel<0>, syncKernel<1>, syncKernel<2>, syncKernel<3>};

__attribute__((noinline)) void noiseKernel(float* buf, uint32_t& rng, float n0, float dn) {
    uint32_t r = rng;
    for (uint32_t i = 0; i < kBlock; ++i) {
        r = r * 1664525u + 1013904223u;
        buf[i] += n0 * float(int32_t(r)) * (1.0f / 2147483648.0f);
        n0 += dn;
    }
    rng = r;
}

// ---------------------------------------------------------------------------
// Filter kernels: one TPT SVF stage (Simper's form) over buf.
//   kSat: 0 none, 2 soft clip after (OB).
//   kOut: add y * gain into L / R instead of writing buf.

struct SvfRamp {
    float a1, da1, a2, da2, a3, da3, k, dk;
};

// Coefficients ramping from (g0, k0) to (g1, k1) across the block.
inline SvfRamp svfRamp(float g0, float k0, float g1, float k1) {
    constexpr float n = 1.0f / float(kBlock);
    const float a10 = 1.0f / (1.0f + g0 * (g0 + k0)), a11 = 1.0f / (1.0f + g1 * (g1 + k1));
    const float a20 = g0 * a10, a21 = g1 * a11;
    const float a30 = g0 * a20, a31 = g1 * a21;
    return SvfRamp{a10, (a11 - a10) * n, a20, (a21 - a20) * n, a30, (a31 - a30) * n, k0, (k1 - k0) * n};
}

// Noise added at the filter input (fused into a 24 dB type's first stage).
struct NoiseRamp {
    uint32_t rng;
    float n, dn;
};

template <int kMode, int kSat, bool kOut, bool kNoise = false>
OT_FASTRUN_FN void svfKernel(float* buf, float* s, SvfRamp c, float* L, float* R, float g, float dg,
                                         NoiseRamp* nz = nullptr) {
    float s1 = s[0], s2 = s[1];
    uint32_t rng = kNoise ? nz->rng : 0u;
    float nl = kNoise ? nz->n : 0.0f;
    for (uint32_t i = 0; i < kBlock; ++i) {
        float x = buf[i];
        if (kNoise) {
            rng = rng * 1664525u + 1013904223u;
            x += nl * float(int32_t(rng)) * (1.0f / 2147483648.0f);
            nl += nz->dn;
        }
        const float v3 = x - s2;
        const float v1 = c.a1 * s1 + c.a2 * v3;
        const float v2 = s2 + c.a2 * s1 + c.a3 * v3;
        s1 = 2.0f * v1 - s1;
        s2 = 2.0f * v2 - s2;
        float y;
        if (kMode == kLP) y = v2;
        else if (kMode == kHP) y = x - c.k * v1 - v2;
        else if (kMode == kBP) y = c.k * v1;
        else y = x - c.k * v1;
        if (kSat == 2) y = softClip2(y);
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
        if (kMode != kLP) c.k += c.dk;
    }
    s[0] = s1;
    s[1] = s2;
    if (kNoise) nz->rng = rng;
}

using SvfFn = void (*)(float*, float*, SvfRamp, float*, float*, float, float, NoiseRamp*);
// First stage of a 24 dB type [noise fused][mode].
constexpr SvfFn kSvfFirst[2][4] = {
    {svfKernel<kLP, 0, false>, svfKernel<kHP, 0, false>, svfKernel<kBP, 0, false>, svfKernel<kNotch, 0, false>},
    {svfKernel<kLP, 0, false, true>, svfKernel<kHP, 0, false, true>, svfKernel<kBP, 0, false, true>,
     svfKernel<kNotch, 0, false, true>},
};
// Last stage, into L / R [OB soft clip][mode] (there is no OB notch).
constexpr SvfFn kSvfLast[2][4] = {
    {svfKernel<kLP, 0, true>, svfKernel<kHP, 0, true>, svfKernel<kBP, 0, true>, svfKernel<kNotch, 0, true>},
    {svfKernel<kLP, 2, true>, svfKernel<kHP, 2, true>, svfKernel<kBP, 2, true>, svfKernel<kBP, 2, true>},
};

// ---------------------------------------------------------------------------

class VapEngine final : public ISynthEngine {
public:
    int32_t id() const override { return kSynthEngineVap; }
    const char* name() const override { return "VAP"; }

    // The framework passes the song tempo before prepare() every block.
    void setTempo(float bpm) override { vap::setTempo(bpm); }

    void slotReset(SynthSlotScratch& sc) override {
        std::memset(sc.bytes, 0, sizeof(sc.bytes));
        SlotState& S = sc.as<SlotState>();
        S.rng = 0x2545F491u;
    }

    VAP_PER_BLOCK void prepare(SynthSlotScratch& sc, const SynthParams& p) override {
        if (p.count < kParamCount) return;
        SlotState& S = sc.as<SlotState>();
        const float* E = p.end;
        if (!S.valid || S.serial != p.serial) {
            for (int e = 0; e < kEnvs; ++e) {
                const int base = e == 0 ? kAmpA : (e == 1 ? kEnv1A : kEnv2A);
                const float a = E[base];
                S.envInc[e] = a <= kBlockT ? 2.0f : kBlockT / a;
                S.envCd[e] = blockCoef(E[base + 1]);
                S.envCr[e] = blockCoef(E[base + 3]);
            }
            S.glideCoef = blockCoef(E[kGlideTime]);
            S.ftype = uint8_t(enumOf(E[kFilterType], 14));
            for (int l = 0; l < 2; ++l) {
                const int base = l == 0 ? kLfo1Ratio : kLfo2Ratio;
                S.lfoWave[l] = uint8_t(enumOf(E[base + 1], 5));
                S.lfoRetrig[l] = uint8_t(enumOf(E[base + 2], 2));
            }
            S.glideMode = uint8_t(enumOf(E[kGlideMode], 3));
            S.oscSync = E[kOscSync] >= 0.5f;
            S.reset1 = E[kReset1] >= 0.5f;
            S.reset2 = E[kReset2] >= 0.5f;
            S.serial = p.serial;
            S.valid = 1;
        }
        // LFO rates (Sync: tempo divisions; the tempo may change any block).
        for (int l = 0; l < 2; ++l) {
            const int base = l == 0 ? kLfo1Ratio : kLfo2Ratio;
            const float hz = E[base + 3] >= 0.5f ? gTempo * (1.0f / 60.0f) / kSyncBeats[enumOf(E[base], 15)]
                                                 : clampf(E[base], 0.0f, 100.0f);
            S.lfoInc[l] = hz * kBlockT;
            // Free-running phase (shared by the slot's voices).
            S.lfoPh0[l] = S.lfoPh1[l];
            S.lfoHold0[l] = S.lfoHold1[l];
            float ph = S.lfoPh1[l] + S.lfoInc[l];
            if (ph >= 1.0f) {
                ph -= float(int(ph));
                S.lfoHold1[l] = rnd(S.rng);
            }
            S.lfoPh1[l] = ph;
        }
        // Mod matrix (amounts ramp start -> end).
        int n = 0;
        uint16_t mask = 0;
        for (int k = 0; k < kModSlots; ++k) {
            const int b = kModBase + 3 * k;
            const float a0 = p.start[b + 2], a1 = E[b + 2];
            if (a0 == 0.0f && a1 == 0.0f) continue;
            Mod& md = S.mod[n++];
            md.src = uint8_t(enumOf(E[b], kSrcCount - 1));
            md.dst = uint8_t(enumOf(E[b + 1], kDstCount - 1));
            md.a0 = a0;
            md.a1 = a1;
            mask = uint16_t(mask | (1u << md.dst));
        }
        S.nmod = uint8_t(n);
        S.dstMask = mask;
    }

    VAP_PER_BLOCK void noteOn(SynthVoice& v, const SynthParams& p, const SynthNote& n) override {
        SlotState& S = v.slotScratch->as<SlotState>();
        VoiceState& V = v.as<VoiceState>();
        const float* E = p.end;
        if (!V.started) {
            V.rng = (lcg(S.rng) ^ 0x9E3779B9u) | 1u;
            V.started = 1;
        }
        V.lastValid = 0;
        const int gm = enumOf(E[kGlideMode], 3);
        const bool legato = n.overlap && (gm == 2 || gm == 3);   // Legato modes: no retrigger
        const bool glide = !n.fxGlide && E[kGlideTime] > 0.0005f && ((gm == 0 || gm == 2) ? (n.reused || S.hasLast) : n.overlap);
        const float from = n.reused ? V.pitch : S.lastPitch;
        V.target = n.pitch;
        V.pitch = glide ? from : n.pitch;
        S.lastPitch = n.pitch;
        S.hasLast = 1;
        if (!legato) {
            for (int e = 0; e < kEnvs; ++e) V.stage[e] = kAttack;
            for (int l = 0; l < 2; ++l) {
                const int base = l == 0 ? kLfo1Ratio : kLfo2Ratio;
                if (enumOf(E[base + 2], 2) != kFree) {
                    V.lfoPh[l] = 0.0f;
                    V.lfoHold[l] = rnd(V.rng);
                }
            }
            // Reset On: the wave restarts at phase 0 on note on. Reset Off: free
            // running (Mini manual "wave reset on note on, on or off"; 2x-06):
            // the phase continues from wherever the voice left it (0 on a fresh,
            // zeroed voice), so a repeated note renders the same every time.
            if (E[kReset1] >= 0.5f) V.ph1 = 0.0f;
            if (E[kReset2] >= 0.5f) V.ph2 = 0.0f;
        }
        v.level = V.env[0];
    }

    void noteOff(SynthVoice& v, const SynthParams& p) override {
        (void)p;
        VoiceState& V = v.as<VoiceState>();
        for (int e = 0; e < kEnvs; ++e)
            if (V.stage[e] != kIdle) V.stage[e] = kRelease;
    }

    VAP_PER_BLOCK OT_FASTRUN_FN bool render(SynthVoice& v, const SynthParams& p, float g0, float gInc, float* L, float* R) override {
        VoiceState& V = v.as<VoiceState>();
        if (!V.started || V.stage[0] == kIdle || p.count < kParamCount) {
            v.level = 0.0f;
            return false;
        }
        const SlotState& S = v.slotScratch->as<SlotState>();
        const float* P1 = p.end;

        // Sources at the block start.
        float src0[kSrcCount], src1[kSrcCount];
        src0[kSrcAmpEnv] = V.env[0];
        src0[kSrcEnv1] = V.env[1];
        src0[kSrcEnv2] = V.env[2];
        for (int l = 0; l < 2; ++l) {
            const int w = S.lfoWave[l];
            src0[kSrcLfo1 + l] = S.lfoRetrig[l] == kFree ? lfoShape(w, S.lfoPh0[l], S.lfoHold0[l])
                                                         : lfoShape(w, V.lfoPh[l], V.lfoHold[l]);
        }
        const float pitch0 = V.pitch;

        // Step glide, envelopes and LFOs by one block.
        if (V.pitch != V.target) {
            const float d = (V.pitch - V.target) * S.glideCoef;
            V.pitch = absf(d) < 0.001f ? V.target : V.target + d;
        }
        float susMod[kEnvs] = {0.0f, 0.0f, 0.0f};
        float decayMod = 0.0f;
        if (S.dstMask & ((1u << kDstAmpSus) | (1u << kDstEnv1Sus) | (1u << kDstEnv2Sus) | (1u << kDstEnv1Decay))) {
            for (int j = 0; j < S.nmod; ++j) {
                const Mod& md = S.mod[j];
                const float x = md.a1 * src0[md.src];
                if (md.dst == kDstAmpSus) susMod[0] += x;
                else if (md.dst == kDstEnv1Sus) susMod[1] += x;
                else if (md.dst == kDstEnv2Sus) susMod[2] += x;
                else if (md.dst == kDstEnv1Decay) decayMod += x;
            }
        }
        for (int e = 0; e < kEnvs; ++e) {
            float lv = V.env[e];
            const int sBase = e == 0 ? kAmpS : (e == 1 ? kEnv1S : kEnv2S);
            const float sus = clampf(P1[sBase] + susMod[e], 0.0f, 1.0f);
            switch (V.stage[e]) {
                case kAttack:
                    lv += S.envInc[e];
                    if (lv >= 1.0f) {
                        lv = 1.0f;
                        V.stage[e] = kDecay;
                    }
                    break;
                case kDecay: {
                    float cd = S.envCd[e];
                    if (e == 1 && decayMod != 0.0f) cd = blockCoef(P1[kEnv1D] * fastExp2(4.0f * decayMod));
                    lv = sus + (lv - sus) * cd;
                    if (sus <= 0.0f && lv < 1.0e-4f) {
                        lv = 0.0f;
                        V.stage[e] = kIdle;
                    }
                    break;
                }
                case kRelease:
                    lv *= S.envCr[e];
                    if (lv < 1.0e-4f) {
                        lv = 0.0f;
                        V.stage[e] = kIdle;
                    }
                    break;
                default: break;
            }
            V.env[e] = lv;
        }
        src1[kSrcAmpEnv] = V.env[0];
        src1[kSrcEnv1] = V.env[1];
        src1[kSrcEnv2] = V.env[2];
        for (int l = 0; l < 2; ++l) {
            const int w = S.lfoWave[l];
            if (S.lfoRetrig[l] == kFree) {
                src1[kSrcLfo1 + l] = lfoShape(w, S.lfoPh1[l], S.lfoHold1[l]);
                continue;
            }
            float ph = V.lfoPh[l] + S.lfoInc[l];
            if (ph >= 1.0f) {
                if (S.lfoRetrig[l] == kOneShot) {
                    ph = kOneShotEnd;
                } else {
                    ph -= float(int(ph));
                    V.lfoHold[l] = rnd(V.rng);
                }
            }
            V.lfoPh[l] = ph;
            src1[kSrcLfo1 + l] = lfoShape(w, ph, V.lfoHold[l]);
        }

        // Destinations: the block start is the last block's end.
        Dest d0, d1;
        if (V.lastValid) d0 = V.last;
        else evalDest(p.start, S, src0, false, pitch0 + v.bendStart, v.cutoffOct, d0);
        evalDest(P1, S, src1, true, V.pitch + v.bend, v.cutoffOct, d1);
        V.last = d1;
        V.lastValid = 1;

        // Oscillators and noise into buf.
        float buf[kBlock];
        const OscRamp o1 = oscRamp(d0.dt1, d1.dt1, d0.A1, d1.A1, d0.B1, d1.B1, d0.C1, d1.C1, d0.w1, d1.w1);
        const OscRamp o2 = oscRamp(d0.dt2, d1.dt2, d0.A2, d1.A2, d0.B2, d1.B2, d0.C2, d1.C2, d0.w2, d1.w2);
        const bool on1 = d0.A1 != 0.0f || d1.A1 != 0.0f || d0.C1 != 0.0f || d1.C1 != 0.0f;
        const bool on2 = d0.A2 != 0.0f || d1.A2 != 0.0f || d0.C2 != 0.0f || d1.C2 != 0.0f;
        const int k1 = kindOf(d0.B1, d1.B1, d0.C1, d1.C1), k2 = kindOf(d0.B2, d1.B2, d0.C2, d1.C2);
        if (on1 && on2 && k1 == 0 && k2 == 0 && !S.oscSync) {   // Init: two saws, one loop
            sawSawKernel(buf, V.ph1, V.ph2, o1, o2);
        } else {
            zeroWords(buf, sizeof(buf));
            const float ph1 = V.ph1;
            if (on1) kOscFns[k1](buf, V.ph1, o1);
            else advance(V.ph1, o1);
            if (!on2) advance(V.ph2, o2);
            else if (S.oscSync) kSyncFns[k2](buf, ph1, o1, V.ph2, o2);   // osc 2 restarts on osc 1's wraps
            else kOscFns[k2](buf, V.ph2, o2);
        }
        const bool noise = d0.noise > 0.0f || d1.noise > 0.0f;
        NoiseRamp nz{V.rng, d0.noise, (d1.noise - d0.noise) * (1.0f / float(kBlock))};

        // Filter and output.
        constexpr float n = 1.0f / float(kBlock);
        const FType ft = kFTypes[S.ftype];
        const float gEnd = g0 + gInc * float(kBlock);
        const float ga = d0.amp * g0, gb = d1.amp * gEnd;
        const float dg = (gb - ga) * n;
        if (ft.poles24) {
            const float k1 = ft.flavour == kMG ? kStage1KMG : kStage1K;
            stage1(ft, buf, V.f, svfRamp(d0.g, k1, d1.g, k1), noise ? &nz : nullptr);
        } else if (noise) {
            noiseKernel(buf, nz.rng, nz.n, nz.dn);
        }
        V.rng = nz.rng;
        stageOut(ft, buf, V.f + 2, svfRamp(d0.g, d0.k, d1.g, d1.k), L, R, ga, dg);

        v.level = V.env[0];
        if (V.stage[0] == kIdle) {
            V.env[0] = 0.0f;
            return false;
        }
        return true;
    }

private:
    static int kindOf(float B0, float B1, float C0, float C1) {
        const bool sq = B0 != 0.0f || B1 != 0.0f, tri = C0 != 0.0f || C1 != 0.0f;
        return sq ? (tri ? 3 : 2) : (tri ? 1 : 0);
    }
    // Phase of a silent oscillator keeps running (sync / mix changes stay continuous).
    static void advance(float& ph, const OscRamp& o) {
        ph += float(kBlock) * (o.dt + 0.5f * float(kBlock - 1) * o.ddt);
        ph -= float(int(ph));
    }
    static void stage1(const FType& ft, float* buf, float* s, const SvfRamp& r, NoiseRamp* nz) {
        kSvfFirst[nz ? 1 : 0][ft.mode](buf, s, r, nullptr, nullptr, 0.0f, 0.0f, nz);
    }
    static void stageOut(const FType& ft, float* buf, float* s, const SvfRamp& r, float* L, float* R, float g, float dg) {
        kSvfLast[ft.flavour == kOB ? 1 : 0][ft.mode](buf, s, r, L, R, g, dg, nullptr);
    }
};

VapEngine gVap;

}  // namespace

ISynthEngine* engine() { return &gVap; }

void setTempo(float bpm) {
    if (bpm > 0.0f) gTempo = bpm;
}

}  // namespace vap
}  // namespace audio
}  // namespace ot
