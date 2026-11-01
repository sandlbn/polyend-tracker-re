// Shared virtual-analog DSP (header-only, inline only, module-facing): the
// building blocks of the A-Type and CS-Type synth voices (opentracker-synths
// modules/atype, modules/cstype; docs/PLAN_CLASSIC.md §2). Lifted from the
// reviewed D-Type and JU6 code and reorganised; D-Type and JU6 keep their own
// copies in 3.0.0 (their golden hashes pin them). Nothing here is compiled
// into an image unless a module uses it.
//
//  - helpers: clamp, enum index, fast 2^x, per-block coefficients, tan,
//    rational tanh, cubic clip, denormal flush, white-noise LCG, the
//    block-rate linear ramp (Lin), the oscillator increment;
//  - oscillators: the one-frame-late polyBLEP / polyBLAMP frame (vcoFrame:
//    saw, pulse, triangle, tri-saw), the same with a hard-sync reset
//    (vcoSync: its jump gets the step residual too), a saw and a pulse off
//    one phase (vcoSawPulse), a parabolic sine;
//  - filters: the TPT state-variable filter (low / band / high-pass at once,
//    linear: stable at any positive damping; self-oscillation from a negative
//    damping that the state's level pulls back per sample, svfLevelK),
//    the 4-pole ZDF ladder sample (input tanh, optional cubic clip
//    per stage: the clipped one is D-Type's kernel);
//  - envelopes: ADSR with an RC attack aiming past the top (Contour), and the
//    IL / AL contour (initial level -> attack level -> initial level -> 0).
//
// Every block-rate value an engine ramps across a block starts from the end
// value of its previous block (no step when a macro slams a parameter).
#pragma once

#include <cstdint>
#include <cstring>

#include "audio/audio_tables.h"

namespace ot {
namespace audio {
namespace va {

constexpr float kBlockT = float(kBlock) / kSampleRate;
constexpr float kInvFs = 1.0f / kSampleRate;
constexpr float kPi = 3.14159265f;
constexpr float kLog2e = 1.44269504f;
constexpr float kAttOver = 1.3f;   // the attack aims past the top (an RC charging curve)
constexpr float kMaxDt = 0.45f;    // phase increment ceiling (polyBLEP needs < 0.5)

// --- helpers -------------------------------------------------------------------

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
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
// Per-block factor reaching ~1 % after t seconds (0 = instant).
inline float blockCoef(float t) { return t <= 0.0005f ? 0.0f : fastExp2(-4.6f * kLog2e * kBlockT / t); }
// One-pole low-pass coefficient for hz (per sample).
inline float onePole(float hz) { return 1.0f - fastExp2(-2.0f * kPi * hz * kInvFs * kLog2e); }
// tan(x) for 0 <= x < pi / 2 (Pade 7/6; < 0.1 % up to 1.4).
inline float tanPade(float x) {
    const float x2 = x * x;
    return x * (135135.0f - x2 * (17325.0f - x2 * (378.0f - x2))) /
           (135135.0f - x2 * (62370.0f - x2 * (3150.0f - 28.0f * x2)));
}
// Rational tanh, exact at 0 and +-3 (where it reaches +-1), clamped beyond.
inline float tanhR(float x) {
    x = clampf(x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
// Cubic soft clip (a ladder stage input): near linear below ~1.
inline float clip3(float x) { return x - x * x * x * (1.0f / 27.0f); }
inline float flush(float x) { return __builtin_fabsf(x) < 1.0e-20f ? 0.0f : x; }
// White noise -1..1 (LCG; one per sample).
inline float lcg(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return float(int32_t(s)) * (1.0f / 2147483648.0f);
}

// A block-rate value ramped across the block: its value now and the
// per-frame increment.
struct Lin {
    float v, d;
};
inline Lin lin(float from, float to) { return Lin{from, (to - from) * (1.0f / float(kBlock))}; }

// Phase increment of an oscillator at note + offset semitones + drift cents.
inline float incOf(float note, float semi, float driftCents, float maxDt) {
    return clampf(440.0f * kInvFs * fastExp2((note + semi + 0.01f * driftCents - 69.0f) * (1.0f / 12.0f)), 1.0e-7f, maxDt);
}
// Cutoff x 2^oct, clamped to 8 Hz .. 0.45 fs.
inline float cutoffHz(float cutoff, float oct) { return clampf(cutoff * fastExp2(oct), 8.0f, 0.45f * kSampleRate); }
// Ladder stage gain G = g / (1 + g), g = tan(pi f / fs).
inline float stageGain(float cutoff, float oct) {
    const float t = tanPade(kPi * cutoffHz(cutoff, oct) * kInvFs);
    return t / (1.0f + t);
}
// SVF integrator gain g = tan(pi f / fs).
inline float svfG(float cutoff, float oct) { return tanPade(kPi * cutoffHz(cutoff, oct) * kInvFs); }

// --- oscillators ------------------------------------------------------------------

enum Shape : uint8_t { kShTri = 0, kShTriSaw, kShSaw, kShPulse, kShOff };

// Residuals of a step h at the fraction x of a frame past the edge: the frame
// before it gets h x^2 / 2, the frame now -h (1 - x)^2 / 2 (the two-frame
// polyBLEP, applied one frame late).
inline void stepResidual(float h, float x, float& before, float& now) {
    const float y = 1.0f - x;
    before += 0.5f * h * x * x;
    now -= 0.5f * h * y * y;
}

// One oscillator frame, band-limited: the steps (saw wrap, pulse edges) get
// the polyBLEP residual, the triangle's corners the polyBLAMP one. An edge is
// found when it is crossed (the wrap, or a change of the hi / lo state carried
// in `hi`), so the frame before it is still held in z: it gets the residual's
// first half, the frame now the second. Returns the held frame (the
// oscillator sounds one frame late) and holds the new one. A phase step past
// both the duty and the wrap in one frame corrects both edges.
// Residuals of a slope change c per frame: before c x^3 / 6, after c (1 - x)^3 / 6.
template <int kSh>
inline float vcoFrame(float& p, float& z, bool& hi, float d, float duty) {
    constexpr float kSaw = kSh == kShSaw ? 1.0f : (kSh == kShTriSaw ? 0.5f : 0.0f);
    constexpr float kTri = kSh == kShTri ? 1.0f : (kSh == kShTriSaw ? 0.5f : 0.0f);
    p += d;
    const bool wrap = p >= 1.0f;
    if (wrap) p -= 1.0f;
    float out = z, cur = 0.0f;
    if (kSaw > 0.0f) {   // 2p - 1: a step of -2 at the wrap
        cur = kSaw * (p + p - 1.0f);
        if (wrap) {
            const float x = p / d, y = 1.0f - x;
            cur += kSaw * y * y;
            out -= kSaw * x * x;
        }
    }
    if (kTri > 0.0f) {   // 4 |p - 1/2| - 1: corners of -8 d per frame at the wrap, +8 d at one half
        cur += kTri * (4.0f * __builtin_fabsf(p - 0.5f) - 1.0f);
        const float k = kTri * (8.0f / 6.0f) * d;
        if (wrap) {
            const float x = p / d, y = 1.0f - x;
            cur -= k * y * y * y;
            out -= k * x * x * x;
        }
        const bool h = p < 0.5f;
        if (!h && hi) {
            const float x = (p - 0.5f) / d, y = 1.0f - x;
            cur += k * y * y * y;
            out += k * x * x * x;
        }
        hi = h;
    }
    if (kSh == kShPulse) {   // +1 below the duty, -1 above, less its mean: +2 at the wrap, -2 at the duty
        const bool h = p < duty;
        cur = (h ? 1.0f : -1.0f) - (duty + duty - 1.0f);
        if (wrap) {
            const float x = p / d, y = 1.0f - x;
            cur -= y * y;
            out += x * x;
        }
        if (!h && (hi || wrap)) {
            const float x = (p - duty) / d, y = 1.0f - x;
            cur += y * y;
            out -= x * x;
        }
        hi = h;
    }
    z = cur;
    return out;
}
// The hi / lo state of a phase for vcoFrame (the triangle's half, the pulse's duty).
template <int kSh>
inline bool vcoHi(float p, float duty) {
    return p < (kSh == kShPulse ? duty : 0.5f);
}

namespace detail {
// The naive saw / pulse (DC removed) at phase p.
template <int kSh>
inline float naive(float p, float duty) {
    return kSh == kShSaw ? p + p - 1.0f : (p < duty ? 1.0f : -1.0f) - (duty + duty - 1.0f);
}
// A run of the phase from p over dp (<= one frame, at most one wrap) that
// ends `age` frames before now: each edge crossed adds its residual. Returns
// the end phase.
template <int kSh>
inline float run(float p, float dp, float d, float age, float duty, float& before, float& now) {
    float e = p + dp;
    const bool wrap = e >= 1.0f;
    if (kSh == kShSaw) {
        if (wrap) {
            e -= 1.0f;
            stepResidual(-2.0f, age + e / d, before, now);
        }
    } else {
        if (p < duty && e >= duty) stepResidual(-2.0f, age + (e - duty) / d, before, now);
        if (wrap) {
            e -= 1.0f;
            stepResidual(2.0f, age + e / d, before, now);
            if (e >= duty) stepResidual(-2.0f, age + (e - duty) / d, before, now);
        }
    }
    return e;
}
}  // namespace detail

// vcoFrame for a saw or pulse with a hard-sync reset: x >= 0 = the master
// wrapped x frames ago (0 <= x < 1), so the phase restarts at 0 then; the
// jump from where it was gets the step residual like any edge. x < 0: no
// reset (the same output as vcoFrame).
template <int kSh>
inline float vcoSync(float& p, float& z, float d, float duty, float x) {
    static_assert(kSh == kShSaw || kSh == kShPulse, "vcoSync: saw or pulse");
    float out = z, cur = 0.0f;
    if (x < 0.0f) {
        p = detail::run<kSh>(p, d, d, 0.0f, duty, out, cur);
    } else {
        const float q = detail::run<kSh>(p, (1.0f - x) * d, d, x, duty, out, cur);
        stepResidual(detail::naive<kSh>(0.0f, duty) - detail::naive<kSh>(q, duty), x, out, cur);
        p = detail::run<kSh>(0.0f, x * d, d, 0.0f, duty, out, cur);
    }
    z = cur + detail::naive<kSh>(p, duty);
    return out;
}

// A saw (level gs) and a pulse (level gp, DC removed) off one phase, both
// band-limited one frame late as vcoFrame does them (one phase, one held
// sample). The pulse is high for the last `width` of the cycle, so its
// fundamental adds to the saw's (aligned the other way the two wrap steps
// cancel into a saw an octave up): it rises at 1 - width (+2 gp) and falls
// with the saw at the wrap (-2 gs - 2 gp). A step past both edges in one
// frame corrects both. `high` carries the pulse's state from frame to frame
// (start it at p >= 1 - width).
inline float vcoSawPulse(float& p, float& z, bool& high, float d, float width, float gs, float gp) {
    const float on = 1.0f - width, e = p + d;
    float out = z, cur = 0.0f;
    // One compare in the common case: the next edge is the rise (low) or the
    // wrap (high). high is carried, so a width moving across the phase steps
    // with its residual (the edge time clamped into the frame).
    if (e >= (high ? 1.0f : on)) {
        if (!high) stepResidual(2.0f * gp, clampf((e - on) / d, 0.0f, 1.0f), out, cur);
        high = true;
        if (e >= 1.0f) {
            p = e - 1.0f;
            stepResidual(-2.0f * (gs + gp), p / d, out, cur);
            high = p >= on;
            if (high) stepResidual(2.0f * gp, clampf((p - on) / d, 0.0f, 1.0f), out, cur);
        } else {
            p = e;
        }
    } else {
        p = e;
    }
    z = cur + gs * (p + p - 1.0f) + gp * ((high ? 1.0f : -1.0f) - (width + width - 1.0f));
    return out;
}

// Sine of the phase 0..1 (a parabola refined once: within 0.1 % of full scale).
inline float sinePar(float p) {
    const float u = p < 0.5f ? p : p - 1.0f;   // -0.5 .. 0.5
    const float y = 8.0f * u * (1.0f - 2.0f * __builtin_fabsf(u));
    return y + 0.225f * (y * __builtin_fabsf(y) - y);
}

// --- filters ------------------------------------------------------------------------

// TPT state-variable filter (two trapezoidal integrators, the loop solved per
// sample, linear). k = 2 x damping (sqrt 2: Butterworth; 0: undamped; < 0:
// the tone grows); h = 1 / (1 + g (g + k)).
struct Svf {
    float s1, s2;
};
struct SvfCoef {
    float g, k, h;
};
inline float svfH(float g, float k) { return 1.0f / (1.0f + g * (g + k)); }
// h for the next sample while g and k move: one Newton step of 1 / D from the
// last h (exact at the block start; the error squares each step). A linear
// ramp of h is far off mid-block when g moves a lot and makes the loop
// unstable. svfHTrack: the same, falling back to the division when the step
// would overshoot (D jumped).
inline float svfHNext(float h, float g, float k) { return h * (2.0f - (1.0f + g * (g + k)) * h); }
inline float svfHTrack(float h, float g, float k) {
    const float D = 1.0f + g * (g + k), e = D * h;
    return e < 1.5f ? h * (2.0f - e) : 1.0f / D;
}
inline SvfCoef svfCoef(float g, float damping) {
    const float k = damping + damping;
    return SvfCoef{g, k, svfH(g, k)};
}
inline void svfStep(Svf& f, const SvfCoef& c, float x, float& lp, float& bp, float& hp) {
    hp = (x - (c.k + c.g) * f.s1 - f.s2) * c.h;
    const float v1 = c.g * hp;
    bp = v1 + f.s1;
    f.s1 = bp + v1;
    const float v2 = c.g * bp;
    lp = v2 + f.s2;
    f.s2 = lp + v2;
}
// Self-oscillation: the sample's k (2 x damping) is the wanted k plus kSvfAgc
// x the state's level before the solve (known, so the zero-delay solution
// stays exact), so a negative k grows the tone until it sings at an
// amplitude of about sqrt(-k / kSvfAgc) (1 at k = -0.2), and loud resonant
// input is damped a little. Per sample: a level term set once per block
// feeds back through the block and jumps from block to block. The growth
// stays bounded at high cutoffs: k >= svfKMin(g) = -kSvfGrow / g (once per
// block, for the block's largest g). h follows k with svfHTrack.
constexpr float kSvfAgc = 0.2f;
constexpr float kSvfGrow = 0.004f;
inline float svfKMin(float g) { return -kSvfGrow / g; }
inline float svfLevelK(float k, float kmin, const Svf& f) {
    const float d = k + kSvfAgc * (f.s1 * f.s1 + f.s2 * f.s2);
    return d < kmin ? kmin : d;
}

// One sample of the 4-pole ZDF ladder: x into four one-pole (trapezoidal)
// stages with the feedback loop solved for this sample (r = 1 / (1 + k G^4)),
// a rational tanh on the loop input, kClip: a cubic soft clip on each stage
// input. y4 = G^4 u + (1 - G) (((s0 G + s1) G + s2) G + s3), u = tanh(x - k y4).
template <bool kClip>
inline float ladderStep(float x, float* s, float G, float k, float r) {
    const float sig = (1.0f - G) * (((s[0] * G + s[1]) * G + s[2]) * G + s[3]);
    const float u = tanhR((x - k * sig) * r);
    float v = G * ((kClip ? clip3(u) : u) - s[0]);
    const float y0 = v + s[0];
    s[0] = y0 + v;
    v = G * ((kClip ? clip3(y0) : y0) - s[1]);
    const float y1 = v + s[1];
    s[1] = y1 + v;
    v = G * ((kClip ? clip3(y1) : y1) - s[2]);
    const float y2 = v + s[2];
    s[2] = y2 + v;
    v = G * ((kClip ? clip3(y2) : y2) - s[3]);
    const float y = v + s[3];
    s[3] = y + v;
    return y;
}

// --- envelopes ------------------------------------------------------------------

enum Stage : uint8_t { kIdle = 0, kAtt, kDec, kRel };

// ADSR, one step per block: the attack an RC curve towards 1.3 that reaches 1
// after Attack seconds; decay and release reach ~1 % after their times.
struct Contour {
    float attC, decC, relC, sustain;
    uint8_t attInstant;
};
inline void setContour(Contour& c, float att, float dec, float sus, float rel) {
    c.attInstant = att <= kBlockT;
    c.attC = c.attInstant ? 0.0f : fastExp2(-1.466f * kLog2e * kBlockT / att);
    c.decC = blockCoef(dec);
    c.relC = blockCoef(rel);
    c.sustain = clampf(sus, 0.0f, 1.0f);
}
inline float step(uint8_t& st, float e, const Contour& c) {
    switch (st) {
        case kAtt:
            e = c.attInstant ? 1.0f : kAttOver - (kAttOver - e) * c.attC;
            if (e >= 1.0f) {
                e = 1.0f;
                st = kDec;
            }
            return e;
        case kDec:   // and holds at Sustain (snapped: no endless approach, no subnormals)
            e = c.sustain + (e - c.sustain) * c.decC;
            if (__builtin_fabsf(e - c.sustain) < 1.0e-5f) e = c.sustain;
            return e;
        case kRel:
            e *= c.relC;
            if (e < 1.0e-4f) {
                e = 0.0f;
                st = kIdle;
            }
            return e;
        default: return 0.0f;
    }
}

// IL / AL contour (a bipolar filter envelope): from the initial level to the
// attack level in Attack (the same RC curve, aiming 30 % past it), back to
// the initial level in Decay and held there, to 0 in Release.
struct IlAl {
    float attC, decC, relC;
    uint8_t attInstant;
};
inline void setIlAl(IlAl& f, float att, float dec, float rel) {
    f.attInstant = att <= kBlockT;
    f.attC = f.attInstant ? 0.0f : fastExp2(-1.466f * kLog2e * kBlockT / att);
    f.decC = blockCoef(dec);
    f.relC = blockCoef(rel);
}
inline float stepIlAl(uint8_t& st, float e, const IlAl& f, float il, float al) {
    switch (st) {
        case kAtt: {
            const float tgt = al + (kAttOver - 1.0f) * (al - il);
            e = f.attInstant ? al : tgt - (tgt - e) * f.attC;
            if (al >= il ? e >= al : e <= al) {
                e = al;
                st = kDec;
            }
            return e;
        }
        case kDec:
            e = il + (e - il) * f.decC;
            if (__builtin_fabsf(e - il) < 1.0e-5f) e = il;
            return e;
        case kRel:
            e *= f.relC;
            if (__builtin_fabsf(e) < 1.0e-4f) {
                e = 0.0f;
                st = kIdle;
            }
            return e;
        default: return 0.0f;
    }
}

}  // namespace va
}  // namespace audio
}  // namespace ot
