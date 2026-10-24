// WTFM 2-operator wavetable FM synth engine (see wtfm.h, docs/specs/2x-08) [OT].
//
// Units (spec 21 §8.5 stored values, our own scaling of the modulation):
//   - FM: phase modulation, 1 rad of index per 100 % (stored 1);
//   - Feedback 1 / 2 / 2>1: pi rad at 100 % on the two-sample average;
//   - pitch destinations: 1 = +-1 octave per unit source;
//   - Filter Cutoff destination and Filter Env Amt: 1 = +-10 octaves;
//   - Shape / Feedback / sustains: added; FM: added in stored units;
//   - Volume destination: gain x max(0, 1 + amount x source);
//   - Filter Env Decay destination: decay time x 2^(4 x amount x source).
// Sources: envelopes 0..1, LFOs -1..1.
//
// Per block a voice takes the destinations at the block start (the last
// block's end), steps glide, envelopes and LFOs, evaluates the destinations
// at the block end and renders 128 frames ramping between the two (as VAP).
// Passes over a stack buffer: operator 2 into buf (skipped while nothing
// listens to it), operator 1 reading it and writing its output in place,
// then the filter stage(s), the last one adding into L / R.
#include "audio/engines/wtfm.h"

#include <cstring>

#include "audio/dsp.h"

#if defined(__GNUC__) && !defined(__clang__)
#define WTFM_PER_BLOCK __attribute__((optimize("Os")))
#else
#define WTFM_PER_BLOCK
#endif

namespace ot {
namespace audio {
namespace wtfm {

namespace {

constexpr float kOutTrim = 0.5f;   // voice output staging (tests/synth_levels_test.cpp)
constexpr float kBlockT = float(kBlock) / kSampleRate;
constexpr float kInvFs = 1.0f / kSampleRate;
constexpr float kPi = 3.14159265f;
constexpr float kLog2e = 1.44269504f;
constexpr float kCutoffOct = 10.0f;
constexpr float kOneShotEnd = 0.99999f;
constexpr float kFmCycles = 0.159154943f;   // 1 rad per stored unit (100 %), in cycles
constexpr float kFbCycles = 0.25f;          // 0.5 cycles at 100 %, times the 1/2 of the average
constexpr float kMaxInc = 0.49f;            // operator frequency limit (cycles per sample)

// ---------------------------------------------------------------------------
// Small math (no libm in the audio path).

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float absf(float x) { return __builtin_fabsf(x); }
inline float minf(float a, float b) { return a < b ? a : b; }
inline float maxf(float a, float b) { return a > b ? a : b; }
inline int enumOf(float x, int max) {
    const int i = int(x + 0.5f);
    return i < 0 ? 0 : (i > max ? max : i);
}

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

inline float blockCoef(float t) {
    if (t <= 0.0005f) return 0.0f;
    return fastExp2(-4.6f * kLog2e * kBlockT / t);
}

inline float fastTan(float x) {
    const float x2 = x * x;
    return x * (15.0f - x2) / (15.0f - 6.0f * x2);
}

inline uint32_t lcg(uint32_t& r) {
    r = r * 1664525u + 1013904223u;
    return r;
}
inline float rnd(uint32_t& r) { return float(int32_t(lcg(r))) * (1.0f / 2147483648.0f); }

inline float softClip2(float x) {
    x = 0.5f * (absf(x + 3.0f) - absf(x - 3.0f));
    return x - (1.0f / 27.0f) * x * x * x;
}

// x - floor(x) for x > -8 (phase-modulated phases stay above -4).
inline float fracf(float x) {
    x += 8.0f;
    return x - float(int(x));
}

// sin(pi t) for t in [-1, 1]: parabola, plain (-30 dB harmonics) or refined
// (error < 0.1 %, -60 dB). No branches.
inline float parab(float t) { return 4.0f * t * (1.0f - absf(t)); }
inline float parabR(float t) {
    const float y = parab(t);
    return y + 0.225f * (y * absf(y) - y);
}
// sin(2 pi x) for x in [0, 1) (= -sin(pi (2x - 1))).
inline float sinPlain(float x) { return -parab(2.0f * x - 1.0f); }
inline float sinCyc(float x) { return -parabR(2.0f * x - 1.0f); }
// clamp to [0, 1] without branches.
inline float sat1(float x) { return 0.5f * (absf(x) - absf(x - 1.0f) + 1.0f); }

// ---------------------------------------------------------------------------
// Engine state.

constexpr int kModSlots = 6;
constexpr int kEnvs = 3;   // 0 Amp, 1 Env 1 (filter), 2 Env 2 (aux)
enum EnvStage : uint8_t { kIdle = 0, kAttack, kDecay, kRelease };
enum LfoRetrig : int { kFree = 0, kOneShot, kNote };
enum LfoWave : int { kTri = 0, kSine, kRampUp, kRampDown, kSquare, kSampleHold };

constexpr float kSyncBeats[16] = {16.0f, 12.0f, 8.0f, 6.0f, 4.0f, 3.0f, 2.0f, 1.5f,
                                  1.0f, 0.75f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f, 1.0f / 12.0f};
float gTempo = 120.0f;

struct Mod {
    uint8_t src, dst;
    float a0, a1;
};

struct SlotState {
    uint32_t serial;
    uint8_t valid;
    uint8_t hasLast;
    uint8_t nmod;
    uint8_t ftype;
    uint8_t lfoWave[2], lfoRetrig[2];
    uint8_t glideMode, pad[3];
    uint16_t dstMask;
    float envInc[kEnvs], envCd[kEnvs], envCr[kEnvs];
    float glideCoef;
    float lfoInc[2];
    float lfoPh0[2], lfoPh1[2];
    float lfoHold0[2], lfoHold1[2];
    uint32_t rng;
    float lastPitch;
    Mod mod[kModSlots];
};
static_assert(sizeof(SlotState) <= kSynthSlotScratchBytes, "WTFM slot scratch");

// Destination values at one instant (block start or end).
struct Dest {
    float inc1, inc2;   // operator phase increments (cycles per sample)
    float s1, s2;       // Shape 0..1
    float fb1, fb2, fb21;   // feedback 0..1
    float fm;           // FM, stored units 0..10
    float g, k;         // filter: tan(pi fc / fs), damping of the resonant stage
    float amp;          // amp envelope x Volume destination x filter input gain x trim
};

struct VoiceState {
    float ph1, ph2;         // operator phases 0..1
    float y1a, y1b;         // carrier output, one and two samples back (sub-samples when oversampled)
    float y2a, y2b;         // modulator output, one and two samples back
    float pitch, target;
    float f[4];             // filter states: stage A s1, s2; stage B s1, s2
    float env[kEnvs];
    float lfoPh[2];
    float lfoHold[2];
    Dest last;
    uint8_t stage[kEnvs];
    uint8_t started;
    uint8_t lastValid;
};
static_assert(sizeof(VoiceState) <= kSynthVoiceStateBytes, "WTFM voice state");

// ---------------------------------------------------------------------------
// LFO waveforms (phase 0..1 -> -1..1).

__attribute__((noinline)) float lfoShape(int wave, float ph, float hold) {
    switch (wave) {
        case kTri: return ph < 0.25f ? 4.0f * ph : (ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f);
        case kSine: return sinCyc(ph);
        case kRampUp: return 2.0f * ph - 1.0f;
        case kRampDown: return 1.0f - 2.0f * ph;
        case kSquare: return ph < 0.5f ? 1.0f : -1.0f;
        default: return hold;
    }
}

// ---------------------------------------------------------------------------
// Filter types (VAP's list and laws).

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
constexpr float kStage1K = 1.848f;
constexpr float kStage1KMG = 2.0f;

struct SvfRamp {
    float a1, da1, a2, da2, a3, da3, k, dk;
};

inline SvfRamp svfRamp(float g0, float k0, float g1, float k1) {
    constexpr float n = 1.0f / float(kBlock);
    const float a10 = 1.0f / (1.0f + g0 * (g0 + k0)), a11 = 1.0f / (1.0f + g1 * (g1 + k1));
    const float a20 = g0 * a10, a21 = g1 * a11;
    const float a30 = g0 * a20, a31 = g1 * a21;
    return SvfRamp{a10, (a11 - a10) * n, a20, (a21 - a20) * n, a30, (a31 - a30) * n, k0, (k1 - k0) * n};
}

template <int kMode, int kSat, bool kOut>
void svfKernel(float* buf, float* s, SvfRamp c, float* L, float* R, float g, float dg) {
    float s1 = s[0], s2 = s[1];
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float x = buf[i];
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
    // Denormal guard: a silent tail must not leave tiny states behind.
    s[0] = absf(s1) < 1.0e-20f ? 0.0f : s1;
    s[1] = absf(s2) < 1.0e-20f ? 0.0f : s2;
}

using SvfFn = void (*)(float*, float*, SvfRamp, float*, float*, float, float);
constexpr SvfFn kSvfFirst[4] = {svfKernel<kLP, 0, false>, svfKernel<kHP, 0, false>, svfKernel<kBP, 0, false>,
                                svfKernel<kNotch, 0, false>};
constexpr SvfFn kSvfLast[2][4] = {
    {svfKernel<kLP, 0, true>, svfKernel<kHP, 0, true>, svfKernel<kBP, 0, true>, svfKernel<kNotch, 0, true>},
    {svfKernel<kLP, 2, true>, svfKernel<kHP, 2, true>, svfKernel<kBP, 2, true>, svfKernel<kBP, 2, true>},
};

// ---------------------------------------------------------------------------
// Operators: the Character rows (spec 2x-08 §3).

enum OpKind : int { kKSine = 0, kKMorph, kKMorphTri, kKAdd, kKinds };
constexpr int kAddN[6] = {1, 2, 3, 5, 7, 11};

// Row coefficients at one instant (ramped across the block).
//   morph: w = (sat(th a) + sat((th - b) c)) / 2, y = -cos(2 pi w), computed
//          as sin(pi (1/2 - |2w - 1|)); y += u (tri - y) (kKMorphTri); fold
//          (Wilder) per block.
//   add:   y = sin(2 pi (th + I sin(2 pi N th))).
struct Row {
    float a, b, c, u, I;
};

// Coefficients of row `ch` at Shape s for an operator running at `inc` cycles
// per sample (band limiting).
WTFM_PER_BLOCK Row rowAt(int ch, float s, float inc) {
    Row r{0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    s = clampf(s, 0.0f, 1.0f);
    if (ch <= kWilder) {
        const float drow = ch == kSmoother ? 0.16f : 0.04f;
        float d = 0.5f - (0.5f - drow) * minf(2.0f * s, 1.0f);
        d = minf(maxf(d, 2.0f * inc), 0.5f);
        const float t = clampf(2.0f * s - 1.0f, 0.0f, 1.0f);
        const float h1 = t * (0.5f - d);
        const float l3 = (1.0f - d) - t * (1.0f - 2.0f * d);
        r.a = 1.0f / d;
        r.b = d + h1;
        r.c = 1.0f / l3;
        r.u = ch == kSmoother ? 0.0f : maxf(0.0f, 1.0f - 2.0f * s);
    } else {
        const float n = float(kAddN[ch - kAdd1]);
        const float lim = maxf(0.0f, ((0.5f / maxf(inc, 1.0e-6f) - 1.0f) / n - 2.0f) * (1.0f / (2.0f * kPi)));
        r.I = minf(0.32f * s, lim);
    }
    return r;
}

struct OpRamp {
    float inc, dinc;
    Row r, dr;
    float off;   // sine kind: phase offset (0.75 = -cos, the morph rows' phase)
    float n;     // add kind: N
    bool fold;   // morph kinds: Wilder
};

template <int kKind>
inline float opWave(float th, const OpRamp& o) {
    if (kKind == kKSine) {
        float x = th + o.off;
        if (x >= 1.0f) x -= 1.0f;
        return sinCyc(x);
    } else if (kKind == kKMorph || kKind == kKMorphTri) {
        const float u = sat1(th * o.r.a) + sat1((th - o.r.b) * o.r.c) - 1.0f;
        float y = parabR(0.5f - absf(u));
        if (kKind == kKMorphTri) y += o.r.u * ((1.0f - 4.0f * absf(th - 0.5f)) - y);
        if (o.fold) y = 3.0f * y - 2.25f * y * absf(y);
        return y;
    } else {
        const float inner = sinPlain(fracf(o.n * th));
        return sinCyc(fracf(th + o.r.I * inner));
    }
}

template <int kKind>
inline void stepRow(OpRamp& o) {
    o.inc += o.dinc;
    if (kKind == kKMorph || kKind == kKMorphTri) {
        o.r.a += o.dr.a;
        o.r.b += o.dr.b;
        o.r.c += o.dr.c;
        if (kKind == kKMorphTri) o.r.u += o.dr.u;
    } else if (kKind == kKAdd) {
        o.r.I += o.dr.I;
    }
}

// Operator 2 into buf. kFb: its own feedback is on this block.
template <int kKind, bool kFb>
void op2Kernel(float* buf, float& phase, float& ya, float& yb, OpRamp o, float fb, float dfb) {
    float ph = phase, a = ya, b = yb;
    for (uint32_t i = 0; i < kBlock; ++i) {
        ph += o.inc;
        if (ph >= 1.0f) ph -= 1.0f;
        const float y = opWave<kKind>(kFb ? fracf(ph + fb * (a + b)) : ph, o);
        b = a;
        a = y;
        buf[i] = y;
        stepRow<kKind>(o);
        if (kFb) fb += dfb;
    }
    phase = ph;
    ya = a;
    yb = b;
}

using Op2Fn = void (*)(float*, float&, float&, float&, OpRamp, float, float);
constexpr Op2Fn kOp2Fns[2][kKinds] = {
    {op2Kernel<kKSine, false>, op2Kernel<kKMorph, false>, op2Kernel<kKMorphTri, false>, op2Kernel<kKAdd, false>},
    {op2Kernel<kKSine, true>, op2Kernel<kKMorph, true>, op2Kernel<kKMorphTri, true>, op2Kernel<kKAdd, true>},
};

struct CarrierMod {
    float fm, dfm;       // FM index (cycles per unit of op 2)
    float fb, dfb;       // own feedback
    float fb21, dfb21;   // op 2 into the feedback node
    float m2a, m2b;      // op 2 one and two samples before the block
};

// Operator 1 (carrier): reads op 2 from buf (kMod), writes its output to buf.
// kMod: any modulation input (FM, own feedback, Feedback 2>1) this block.
// kOs: two sub-frames per frame (op 2 interpolated), decimated by [1 2 1] / 4.
template <int kKind, bool kOs, bool kMod>
void op1Kernel(float* buf, float& phase, float& ya, float& yb, OpRamp o, CarrierMod c) {
    float ph = phase, a = ya, b = yb, p1 = c.m2a, p2 = c.m2b;
    for (uint32_t i = 0; i < kBlock; ++i) {
        float out;
        if (kMod) {
            const float y2 = buf[i];
            const float fbk = c.fb21 * (p1 + p2);
            if (kOs) {
                const float prev = a;
                const float inc = 0.5f * o.inc;
                ph += inc;
                if (ph >= 1.0f) ph -= 1.0f;
                const float ya1 = opWave<kKind>(fracf(ph + c.fm * 0.5f * (p1 + y2) + c.fb * (a + b) + fbk), o);
                b = a;
                a = ya1;
                ph += inc;
                if (ph >= 1.0f) ph -= 1.0f;
                const float ya2 = opWave<kKind>(fracf(ph + c.fm * y2 + c.fb * (a + b) + fbk), o);
                b = a;
                a = ya2;
                out = 0.25f * (prev + 2.0f * ya1 + ya2);
            } else {
                ph += o.inc;
                if (ph >= 1.0f) ph -= 1.0f;
                out = opWave<kKind>(fracf(ph + c.fm * y2 + c.fb * (a + b) + fbk), o);
                b = a;
                a = out;
            }
            p2 = p1;
            p1 = y2;
            c.fm += c.dfm;
            c.fb += c.dfb;
            c.fb21 += c.dfb21;
        } else if (kOs) {
            const float prev = a;
            const float inc = 0.5f * o.inc;
            ph += inc;
            if (ph >= 1.0f) ph -= 1.0f;
            const float ya1 = opWave<kKind>(ph, o);
            ph += inc;
            if (ph >= 1.0f) ph -= 1.0f;
            b = ya1;
            a = opWave<kKind>(ph, o);
            out = 0.25f * (prev + 2.0f * ya1 + a);
        } else {
            ph += o.inc;
            if (ph >= 1.0f) ph -= 1.0f;
            out = opWave<kKind>(ph, o);
            b = a;
            a = out;
        }
        buf[i] = out;
        stepRow<kKind>(o);
    }
    phase = ph;
    ya = a;
    yb = b;
}

using Op1Fn = void (*)(float*, float&, float&, float&, OpRamp, CarrierMod);
#define WTFM_OP1(os, mod) {op1Kernel<kKSine, os, mod>, op1Kernel<kKMorph, os, mod>, op1Kernel<kKMorphTri, os, mod>, op1Kernel<kKAdd, os, mod>}
constexpr Op1Fn kOp1Fns[2][2][kKinds] = {
    {WTFM_OP1(false, false), WTFM_OP1(false, true)},
    {WTFM_OP1(true, false), WTFM_OP1(true, true)},
};
#undef WTFM_OP1

// The kernel kind of row `ch` across a block with Shape s0 -> s1.
inline int kindOf(int ch, const Row& r0, const Row& r1, float s0, float s1) {
    if (ch <= kWilder) {
        if (ch == kSmoother && s0 <= 0.0f && s1 <= 0.0f) return kKSine;
        return (r0.u > 0.0f || r1.u > 0.0f) ? kKMorphTri : kKMorph;
    }
    return (r0.I <= 0.0f && r1.I <= 0.0f) ? kKSine : kKAdd;
}

WTFM_PER_BLOCK OpRamp opRamp(int ch, int kind, float inc0, float inc1, float s0, float s1, float rate) {
    constexpr float n = 1.0f / float(kBlock);
    OpRamp o;
    o.inc = inc0;
    o.dinc = (inc1 - inc0) * n;
    const Row r0 = rowAt(ch, s0, inc0 * rate), r1 = rowAt(ch, s1, inc1 * rate);
    o.r = r0;
    o.dr = Row{(r1.a - r0.a) * n, (r1.b - r0.b) * n, (r1.c - r0.c) * n, (r1.u - r0.u) * n, (r1.I - r0.I) * n};
    o.off = ch <= kWilder ? 0.75f : 0.0f;
    o.n = float(ch >= kAdd1 ? kAddN[ch - kAdd1] : 1);
    o.fold = ch == kWilder;
    (void)kind;
    return o;
}

// ---------------------------------------------------------------------------
// Destinations.

inline float pitchToInc(float pitch, float ratio, float cents) {
    return clampf(440.0f * kInvFs * ratio * fastExp2((pitch - 69.0f + 0.01f * cents) * (1.0f / 12.0f)), 1.0e-6f, kMaxInc);
}

WTFM_PER_BLOCK void evalDest(const float* P, const SlotState& S, const float* src, bool atEnd, float pitch, Dest& d) {
    float m[kDstCount];
    for (int j = 0; j < kDstCount; ++j) {
        OT_NO_LIBCALL();
        m[j] = 0.0f;
    }
    for (int j = 0; j < S.nmod; ++j) {
        const Mod& md = S.mod[j];
        m[md.dst] += (atEnd ? md.a1 : md.a0) * src[md.src];
    }
    d.inc1 = pitchToInc(pitch + 12.0f * m[kDstPitch1], ratioOf(enumOf(P[kRatio1], 47)), P[kFine1]);
    d.inc2 = pitchToInc(pitch + 12.0f * m[kDstPitch2], ratioOf(enumOf(P[kRatio2], 47)), P[kFine2]);
    d.s1 = clampf(P[kShape1] + m[kDstShape1], 0.0f, 1.0f);
    d.s2 = clampf(P[kShape2] + m[kDstShape2], 0.0f, 1.0f);
    d.fb1 = clampf(P[kFb1] + m[kDstFb1], 0.0f, 1.0f);
    d.fb2 = clampf(P[kFb2] + m[kDstFb2], 0.0f, 1.0f);
    d.fb21 = clampf(P[kFb21] + m[kDstFb21], 0.0f, 1.0f);
    d.fm = clampf(P[kFm] + m[kDstFm], 0.0f, 10.0f);
    // Filter.
    const float oct = kCutoffOct * (P[kFilterEnvAmt] * src[kSrcEnv1] + m[kDstCutoff]) + P[kNoteTrack] * (pitch - 60.0f) * (1.0f / 12.0f);
    const float fc = clampf(P[kCutoff] * fastExp2(oct), 16.0f, 0.45f * kSampleRate);
    const float r = clampf(P[kResonance] * (1.0f / 1.5f), 0.0f, 1.0f);
    d.g = fastTan(kPi * fc * kInvFs);
    d.k = 2.0f * (1.0f - r) * (1.0f - r) + 0.02f;
    // Filter input gain (MG drive, SVF resonance bass loss) folded into the
    // carrier's output level; the OB soft clip sees the same level as VAP's.
    const FType ft = kFTypes[S.ftype];
    const float in = ft.flavour == kMG ? 1.2f - 0.6f * r : (ft.flavour == kSVF ? 1.0f - 0.4f * r : 1.0f);
    const float vol = 1.0f + m[kDstVolume];
    d.amp = src[kSrcAmpEnv] * (vol > 0.0f ? vol * kOutTrim : 0.0f) * in;
}

// ---------------------------------------------------------------------------

class WtfmEngine final : public ISynthEngine {
public:
    int32_t id() const override { return kSynthEngineWtfm; }
    const char* name() const override { return "WTFM"; }

    void setTempo(float bpm) override { wtfm::setTempo(bpm); }

    void slotReset(SynthSlotScratch& sc) override {
        std::memset(sc.bytes, 0, sizeof(sc.bytes));
        SlotState& S = sc.as<SlotState>();
        S.rng = 0x6A09E667u;
    }

    WTFM_PER_BLOCK void prepare(SynthSlotScratch& sc, const SynthParams& p) override {
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
            S.serial = p.serial;
            S.valid = 1;
        }
        for (int l = 0; l < 2; ++l) {
            const int base = l == 0 ? kLfo1Ratio : kLfo2Ratio;
            const float hz = E[base + 3] >= 0.5f ? gTempo * (1.0f / 60.0f) / kSyncBeats[enumOf(E[base], 15)]
                                                 : clampf(E[base], 0.0f, 100.0f);
            S.lfoInc[l] = hz * kBlockT;
            S.lfoPh0[l] = S.lfoPh1[l];
            S.lfoHold0[l] = S.lfoHold1[l];
            float ph = S.lfoPh1[l] + S.lfoInc[l];
            if (ph >= 1.0f) {
                ph -= float(int(ph));
                S.lfoHold1[l] = rnd(S.rng);
            }
            S.lfoPh1[l] = ph;
        }
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

    WTFM_PER_BLOCK void noteOn(SynthVoice& v, const SynthParams& p, const SynthNote& n) override {
        SlotState& S = v.slotScratch->as<SlotState>();
        VoiceState& V = v.as<VoiceState>();
        const float* E = p.end;
        V.started = 1;
        V.lastValid = 0;
        const int gm = enumOf(E[kGlideMode], 3);
        const bool legato = n.overlap && (gm == 2 || gm == 3);
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
                    V.lfoHold[l] = rnd(S.rng);
                }
            }
            // Reset On: the operator restarts at phase 0 with a cleared
            // feedback memory; Off: free running across the voice's notes.
            if (E[kReset1] >= 0.5f) {
                V.ph1 = 0.0f;
                V.y1a = V.y1b = 0.0f;
            }
            if (E[kReset2] >= 0.5f) {
                V.ph2 = 0.0f;
                V.y2a = V.y2b = 0.0f;
            }
        }
        v.level = V.env[0];
    }

    void noteOff(SynthVoice& v, const SynthParams& p) override {
        (void)p;
        VoiceState& V = v.as<VoiceState>();
        for (int e = 0; e < kEnvs; ++e)
            if (V.stage[e] != kIdle) V.stage[e] = kRelease;
    }

    WTFM_PER_BLOCK bool render(SynthVoice& v, const SynthParams& p, float g0, float gInc, float* L, float* R) override {
        VoiceState& V = v.as<VoiceState>();
        if (!V.started || V.stage[0] == kIdle || p.count < kParamCount) {
            v.level = 0.0f;
            return false;
        }
        SlotState& S = v.slotScratch->as<SlotState>();
        const float* P1 = p.end;

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
                    V.lfoHold[l] = rnd(S.rng);
                }
            }
            V.lfoPh[l] = ph;
            src1[kSrcLfo1 + l] = lfoShape(w, ph, V.lfoHold[l]);
        }

        Dest d0, d1;
        if (V.lastValid) d0 = V.last;
        else evalDest(p.start, S, src0, false, pitch0 + v.bendStart, d0);
        evalDest(P1, S, src1, true, V.pitch + v.bend, d1);
        V.last = d1;
        V.lastValid = 1;

        // Operators.
        constexpr float n = 1.0f / float(kBlock);
        const bool os = P1[kOversample] >= 0.5f;
        const int ch1 = enumOf(P1[kChar1], kCharCount - 1), ch2 = enumOf(P1[kChar2], kCharCount - 1);
        float buf[kBlock];
        const float m2a = V.y2a, m2b = V.y2b;
        const bool op2On = d0.fm > 0.0f || d1.fm > 0.0f || d0.fb21 > 0.0f || d1.fb21 > 0.0f;
        if (op2On) {
            OpRamp o2 = opRamp(ch2, 0, d0.inc2, d1.inc2, d0.s2, d1.s2, 1.0f);
            const int k2 = kindOf(ch2, o2.r, rowAt(ch2, d1.s2, d1.inc2), d0.s2, d1.s2);
            const bool fb2 = d0.fb2 > 0.0f || d1.fb2 > 0.0f;
            kOp2Fns[fb2 ? 1 : 0][k2](buf, V.ph2, V.y2a, V.y2b, o2, kFbCycles * d0.fb2, kFbCycles * (d1.fb2 - d0.fb2) * n);
        } else {
            // The silent modulator keeps running (free-running phase).
            V.ph2 = fracf(V.ph2 + float(kBlock) * 0.5f * (d0.inc2 + d1.inc2));
            V.y2a = V.y2b = 0.0f;
        }
        const float rate = os ? 0.5f : 1.0f;
        OpRamp o1 = opRamp(ch1, 0, d0.inc1, d1.inc1, d0.s1, d1.s1, rate);
        const int k1 = kindOf(ch1, o1.r, rowAt(ch1, d1.s1, d1.inc1 * rate), d0.s1, d1.s1);
        const FType ft = kFTypes[S.ftype];
        CarrierMod cm;
        cm.fm = kFmCycles * d0.fm;
        cm.dfm = kFmCycles * (d1.fm - d0.fm) * n;
        cm.fb = kFbCycles * d0.fb1;
        cm.dfb = kFbCycles * (d1.fb1 - d0.fb1) * n;
        cm.fb21 = kFbCycles * d0.fb21;
        cm.dfb21 = kFbCycles * (d1.fb21 - d0.fb21) * n;
        cm.m2a = op2On ? m2a : 0.0f;
        cm.m2b = op2On ? m2b : 0.0f;
        const bool mod = op2On || d0.fb1 > 0.0f || d1.fb1 > 0.0f;
        kOp1Fns[os ? 1 : 0][mod ? 1 : 0][k1](buf, V.ph1, V.y1a, V.y1b, o1, cm);

        // Filter and output.
        const float gEnd = g0 + gInc * float(kBlock);
        const float ga = d0.amp * g0, gb = d1.amp * gEnd;
        const float dg = (gb - ga) * n;
        if (ft.poles24) {
            const float sk = ft.flavour == kMG ? kStage1KMG : kStage1K;
            kSvfFirst[ft.mode](buf, V.f, svfRamp(d0.g, sk, d1.g, sk), nullptr, nullptr, 0.0f, 0.0f);
        }
        kSvfLast[ft.flavour == kOB ? 1 : 0][ft.mode](buf, V.f + 2, svfRamp(d0.g, d0.k, d1.g, d1.k), L, R, ga, dg);

        v.level = V.env[0];
        if (V.stage[0] == kIdle) {
            V.env[0] = 0.0f;
            return false;
        }
        return true;
    }
};

WtfmEngine gWtfm;

}  // namespace

ISynthEngine* engine() { return &gWtfm; }

void setTempo(float bpm) {
    if (bpm > 0.0f) gTempo = bpm;
}

}  // namespace wtfm
}  // namespace audio
}  // namespace ot
