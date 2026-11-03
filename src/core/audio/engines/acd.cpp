// ACD synth engine (see acd.h) [OT clean-room DSP].
//
// Signal path per voice and block (all passes over one 128-frame buffer):
//   1. control (block rate): Amp / Mod envelopes, LFO, glide, then the
//      block-end targets of phase increment, pulse duty and filter
//      coefficients; each is ramped linearly from the previous block's value
//   2. oscillator pass (template on pulse / sub kind): PolyBLEP saw, pulse as
//      the difference of two BLEP saws, sub locked to the main phase by an
//      octave counter (pulse via two saws, triangle naive)
//   3. filter pass (one loop per type, in place; the pre-filter noise of
//      Noise Mix > 0 is added at its input)
//   4. output pass: optional post-filter noise (Noise Mix < 0), amplitude
//      envelope x framework gain, added into L and R
//
// Parameter semantics chosen here (the manual gives only names and ranges):
//  - PW: high-time fraction of the pulse, 0.5 = square, clamped to 3..97 %;
//    PW LFO / PW Env add LFO x amount and Mod Env x amount.
//  - Sub Mode: -1 / -2 octave square, 25 % pulse, triangle.
//  - Noise Mix: |value| is the level; > 0 feeds the filter, < 0 is added
//    after it.
//  - Pitch LFO: octaves per unit (±3 = ±3 octaves of vibrato / siren).
//  - Filter Env Amt / Filter LFO Amt: 10 x sign(a) x sqrt(|a|) octaves at
//    full envelope / LFO (fine control of the small amounts patches use).
//  - Note Track: octaves of cutoff per octave of pitch, around C4 (60).
//  - Resonance 0..1.5: SVF Q from 0.707 to ~28 with an input attenuation
//    that keeps the peak level bounded; RD3 feedback 0..4.35 (self-oscillates
//    above ~1.38, bounded by its tanh).
//  - Envelopes: linear attack, exponential decay / release reaching 1 % of
//    the distance in the stage time. The voice ends when the amplitude
//    envelope falls below -80 dB in release (or decays to a 0 sustain).
//  - LFO: bipolar shapes; Free Running shares one phase per slot, One Shot
//    runs one cycle from the note start and holds its end value, Note
//    restarts with every (non-legato) note.
//  - Glide: constant time, linear in semitones. Always / Legato glide from
//    the slot's previous note; Overlap / Legato Overlap only from a held
//    note. The Legato modes do not retrigger envelopes / LFO on overlap.
#include "audio/engines/acd.h"

#include <cmath>
#include <cstring>

#include "fastrun.h"

namespace ot {
namespace audio {

namespace {

using namespace acd;

constexpr float kBlockSec = float(kBlock) / kSampleRate;
constexpr float kPiF = 3.14159265f;
constexpr float kOscGain = 0.5f;       // oscillator sum headroom
constexpr float kPostNoise = 0.35f;    // post-filter noise level at |Noise Mix| = 1
constexpr float kMaxInc = 0.45f;       // phase increment ceiling (PolyBLEP needs < 0.5)
constexpr float kDoneLevel = 1.0e-4f;  // -80 dB: the voice is finished

// Block-rate helpers are kept out of line (flash): they run a few times per block.
inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// 2^x for |x| <= 60: Taylor series of 2^f on [0, 1) and the exponent bits
// (relative error < 1.6e-4, 0.3 cent).
__attribute__((noinline)) float exp2Fast(float x) {
    x = clampf(x, -60.0f, 60.0f);
    int i = int(x);
    if (float(i) > x) --i;
    const float f = x - float(i);
    const float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    const uint32_t b = uint32_t(i + 127) << 23;
    float s;
    std::memcpy(&s, &b, 4);
    return p * s;
}

// tan(x) for 0 <= x <= 1.45: Padé [5/4] (relative error < 5e-5).
inline float tanPade(float x) {
    const float x2 = x * x;
    return x * (945.0f - 105.0f * x2 + x2 * x2) / (945.0f - 420.0f * x2 + 15.0f * x2 * x2);
}

// sin(2 pi ph), ph in [0, 1]: parabola with one refinement step (< 0.1 %).
inline float sin2pi(float ph) {
    const float x = ph + ph - 1.0f;   // sin(2 pi ph) = -sin(pi x)
    float y = 4.0f * x * (1.0f - std::fabs(x));
    y += 0.225f * (y * std::fabs(y) - y);
    return -y;
}

inline uint32_t lcg(uint32_t r) { return r * 1664525u + 1013904223u; }
inline float lcgFloat(uint32_t r) { return float(int32_t(r)) * 4.6566129e-10f; }   // -1..1

// Bipolar LFO shape at phase ph (0..1; 1 = the end of a One Shot cycle).
__attribute__((noinline)) float lfoShape(int wave, float ph, float sh) {
    switch (wave) {
        case kTri: return ph < 0.25f ? 4.0f * ph : (ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f);
        case kSine: return sin2pi(ph);
        case kRampUp: return ph + ph - 1.0f;
        case kRampDown: return 1.0f - ph - ph;
        case kSquare: return ph < 0.5f ? 1.0f : -1.0f;
        default: return sh;
    }
}

// Envelope coefficients per block.
struct EnvCoef {
    float aInc;   // attack increment per block
    float dK;     // decay factor per block
    float sus;    // sustain level
    float rK;     // release factor per block
};

// Exponential factor per block reaching 1 % of the distance in t seconds.
inline float stageK(float t) {
    if (t < 1.0e-4f) return 0.0f;
    return exp2Fast(-6.6438562f * kBlockSec / t);   // 4.6 / ln 2
}

__attribute__((noinline)) EnvCoef envCoef(const SynthParams& p, int base) {
    EnvCoef e;
    const float a = p.end[base];
    e.aInc = a <= kBlockSec ? 1.0f : kBlockSec / a;
    e.dK = stageK(p.end[base + 1]);
    e.sus = clampf(p.end[base + 2], 0.0f, 1.0f);
    e.rK = stageK(p.end[base + 3]);
    return e;
}

enum EnvStage : uint8_t { kIdle = 0, kAttack, kDecay, kRelease };

__attribute__((noinline)) void envStep(float& lv, uint8_t& st, const EnvCoef& e) {
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

// Filter laws from the resonance (0..1.5).
inline float svfInGain(float res) { return exp2Fast(-1.9f * (res * (1.0f / 1.5f))); }
inline float ladderK(float res) { return 2.9f * res; }

// Per-slot state (SynthSlotScratch, 384 B).
struct Slot {
    uint32_t serial;
    uint8_t valid, hasLast, wave, retrig;
    uint8_t ftype, subMode, glideMode, pad;
    EnvCoef amp, mod;
    float lfoInc;          // LFO cycles per block
    float freePh, freeVal, freeSh;
    uint32_t rng;
    float kRes;            // SVF damping (last stage) / ladder feedback
    float envOct, lfoOct;  // filter modulation depths (octaves at full scale)
    float pitchLfo;        // semitones at full LFO
    float noteTrack;       // octaves of cutoff per semitone
    float cutoff;          // Hz
    float glideBlocks;     // glide time in blocks (0 = off)
    float lastPitch;       // the slot's previous note (glide source)
};
static_assert(sizeof(Slot) <= kSynthSlotScratchBytes, "ACD slot state too large");

// Per-voice state (SynthVoice::state, 128 B).
enum VoiceFlag : uint8_t { kPrimed = 1, kLfoDone = 2 };
struct Voice {
    float phase;    // main oscillator phase 0..1
    float pitch;    // glided note pitch (semitones)
    float target;   // glide target
    float glide;    // semitones per block (0 = arrived)
    float inc;      // phase increment at the end of the last block
    float duty;     // pulse high fraction at the end of the last block
    float s[4];     // filter states
    float c[6];     // filter coefficients at the end of the last block
    float amp, mod; // envelope levels
    float lfoPh, lfoSh;
    uint32_t rng;
    uint8_t ampSt, modSt, ft, flags;
    uint32_t sub;   // octave counter of the sub oscillator
};
static_assert(sizeof(Voice) <= kSynthVoiceStateBytes, "ACD voice state too large");

// --- oscillator ------------------------------------------------------------------

// PolyBLEP residual at phase t for a step of increment dt (idt = 1 / dt).
inline float blep(float t, float dt, float idt) {
    if (t < dt) {
        const float x = t * idt;
        return x + x - x * x - 1.0f;
    }
    if (t > 1.0f - dt) {
        const float x = (t - 1.0f) * idt;
        return x * x + x + x + 1.0f;
    }
    return 0.0f;
}

struct Osc {
    float ph, inc, dInc, idt;
    float duty, dDuty;
    float wSaw, dSaw, wSq, dSq, wSub, dSub;   // ramped mix weights
    uint32_t cnt, mask;
    float invN;
    float lvl[4];    // sub level per octave-counter value (DC-free)
    float step[4];   // half the sub jump when the counter enters that value
};

// Sub kinds: 0 none; 1 square / pulse whose edges all fall on main-oscillator
// wraps (-1 / -2 oct square, -2 oct pulse): level table plus the main saw's
// BLEP residual scaled by the jump; 2 triangle; 3 -1 oct 25 % pulse (wrap
// edge + a second BLEP for the falling edge at half the main cycle).
template <bool kPulse, int kSub>
void oscLoop(Osc& o, float* out) {
    float ph = o.ph, inc = o.inc, duty = o.duty, wSaw = o.wSaw, wSq = o.wSq, wSub = o.wSub;
    const float dInc = o.dInc, dDuty = o.dDuty, dSaw = o.dSaw, dSq = o.dSq, dSub = o.dSub, idt = o.idt;
    uint32_t cnt = o.cnt;
    const uint32_t mask = o.mask;
    const float invN = o.invN;
    for (uint32_t i = 0; i < kBlock; ++i) {
        ph += inc;
        if (ph >= 1.0f) {
            ph -= 1.0f;
            ++cnt;
        }
        const float bm = blep(ph, inc, idt);
        float y = wSaw * (ph + ph - 1.0f - bm);
        if (kPulse) {
            float t2 = ph + 1.0f - duty;
            if (t2 >= 1.0f) t2 -= 1.0f;
            y += wSq * (t2 + t2 - 1.0f - blep(t2, inc, idt));
            duty += dDuty;
            wSq += dSq;
        }
        if (kSub == 1) {
            const uint32_t c = cnt & mask;
            float sv = o.lvl[c];
            if (bm != 0.0f) sv += o.step[ph < 0.5f ? c : ((c + 1) & mask)] * bm;
            y += wSub * sv;
            wSub += dSub;
        } else if (kSub == 2) {
            const float sp = (float(cnt & mask) + ph) * invN;
            y += wSub * (1.0f - 4.0f * std::fabs(sp - 0.5f));
            wSub += dSub;
        } else if (kSub == 3) {
            const bool first = (cnt & 1u) == 0;
            float sv = (first && ph < 0.5f) ? 1.5f : -0.5f;
            if (first) {
                float t3 = ph + 0.5f;
                if (t3 >= 1.0f) t3 -= 1.0f;
                sv -= blep(t3, inc, idt);
                if (bm != 0.0f && ph < 0.5f) sv += bm;   // rising edge just passed
            } else if (bm != 0.0f && ph >= 0.5f) {
                sv += bm;                                // rising edge coming
            }
            y += wSub * sv;
            wSub += dSub;
        }
        out[i] = y;
        inc += dInc;
        wSaw += dSaw;
    }
    o.ph = ph;
    o.cnt = cnt;
}

using OscFn = void (*)(Osc&, float*);
const OscFn kOscFns[2][4] = {
    {oscLoop<false, 0>, oscLoop<false, 1>, oscLoop<false, 2>, oscLoop<false, 3>},
    {oscLoop<true, 0>, oscLoop<true, 1>, oscLoop<true, 2>, oscLoop<true, 3>},
};

// Sub level tables (DC-free) per mode: high counts of N, N.
inline void subTables(int mode, Osc& o) {
    const bool twoOct = mode >= 3;
    const int n = twoOct ? 4 : 2;
    const int high = (mode % 3) == 0 ? n / 2 : (twoOct ? 1 : 0);   // -1 oct pulse: kind 3
    o.mask = uint32_t(n - 1);
    o.invN = 1.0f / float(n);
    const float mean = high > 0 ? (2.0f * float(high) / float(n) - 1.0f) : 0.0f;
    for (int c = 0; c < 4; ++c) o.lvl[c] = (c < high ? 1.0f : -1.0f) - mean;
    for (int c = 0; c < 4; ++c) o.step[c] = 0.5f * (o.lvl[c & (n - 1)] - o.lvl[(c + n - 1) & (n - 1)]);
}

// --- filters (in place, coefficients ramped c -> c + 128 dc) -------------------------
// kN: add the pre-filter noise (ramped weight) to the input.

struct Noise {
    float w, dw;
    uint32_t rng;
    template <bool kN>
    float add(float x) {
        if (!kN) return x;
        rng = lcg(rng);
        x += w * lcgFloat(rng);
        w += dw;
        return x;
    }
};

// TPT SVF coefficients (g = tan(pi fc / fs), k = damping).
inline void svfCoef(float g, float k, float* c) {
    const float a1 = 1.0f / (1.0f + g * (g + k));
    c[0] = a1;
    c[1] = g * a1;
    c[2] = g * g * a1;
}

template <bool kN>
void svf12(float* buf, float* s, const float* c, const float* dc, Noise& nz) {
    float z1 = s[0], z2 = s[1], a1 = c[0], a2 = c[1], a3 = c[2];
    const float d1 = dc[0], d2 = dc[1], d3 = dc[2];
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float v3 = nz.add<kN>(buf[i]) - z2;
        const float v1 = a1 * z1 + a2 * v3;
        const float v2 = z2 + a2 * z1 + a3 * v3;
        z1 = v1 + v1 - z1;
        z2 = v2 + v2 - z2;
        buf[i] = v2;
        a1 += d1;
        a2 += d2;
        a3 += d3;
    }
    s[0] = z1;
    s[1] = z2;
}

template <bool kN>
void svf24(float* buf, float* s, const float* c, const float* dc, Noise& nz) {
    float z1 = s[0], z2 = s[1], z3 = s[2], z4 = s[3];
    float a1 = c[0], a2 = c[1], a3 = c[2], b1 = c[3], b2 = c[4], b3 = c[5];
    const float da1 = dc[0], da2 = dc[1], da3 = dc[2], db1 = dc[3], db2 = dc[4], db3 = dc[5];
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float u3 = nz.add<kN>(buf[i]) - z2;
        const float u1 = a1 * z1 + a2 * u3;
        const float u2 = z2 + a2 * z1 + a3 * u3;
        z1 = u1 + u1 - z1;
        z2 = u2 + u2 - z2;
        const float v3 = u2 - z4;
        const float v1 = b1 * z3 + b2 * v3;
        const float v2 = z4 + b2 * z3 + b3 * v3;
        z3 = v1 + v1 - z3;
        z4 = v2 + v2 - z4;
        buf[i] = v2;
        a1 += da1;
        a2 += da2;
        a3 += da3;
        b1 += db1;
        b2 += db2;
        b3 += db3;
    }
    s[0] = z1;
    s[1] = z2;
    s[2] = z3;
    s[3] = z4;
}

// RD3 ladder: four TPT one-poles (G = g / (1 + g)); the feedback is solved
// linearly (u = (x - k S) / (1 + k G^4), S = the state part of the 4th
// output), then one tanh-shaped saturator on u. c = {G, 1 / (1 + k G^4), k};
// the output make-up gain (1 + k / 2) rides on the amplitude ramp.
template <bool kN>
OT_FASTRUN_FN void ladder(float* buf, float* s, const float* c, const float* dc, Noise& nz) {
    float s0 = s[0], s1 = s[1], s2 = s[2], s3 = s[3];
    float G = c[0], inv = c[1], k = c[2];
    const float dG = dc[0], dInv = dc[1], dK = dc[2];
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float B = 1.0f - G;
        const float S = B * (s3 + G * (s2 + G * (s1 + G * s0)));
        float u = (nz.add<kN>(buf[i]) - k * S) * inv;
        u = clampf(u, -3.0f, 3.0f);
        const float u2 = u * u;
        u = u * (27.0f + u2) / (27.0f + 9.0f * u2);   // tanh
        float v = (u - s0) * G;
        float y = v + s0;
        s0 = y + v;
        v = (y - s1) * G;
        y = v + s1;
        s1 = y + v;
        v = (y - s2) * G;
        y = v + s2;
        s2 = y + v;
        v = (y - s3) * G;
        y = v + s3;
        s3 = y + v;
        buf[i] = y;
        G += dG;
        inv += dInv;
        k += dK;
    }
    s[0] = s0;
    s[1] = s1;
    s[2] = s2;
    s[3] = s3;
}

// --- engine ------------------------------------------------------------------------

class AcdEngine final : public ISynthEngine {
public:
    int32_t id() const override { return kSynthEngineAcd; }
    const char* name() const override { return "ACD"; }

    void slotReset(SynthSlotScratch& scratch) override {
        Slot& s = scratch.as<Slot>();
        std::memset(&s, 0, sizeof(s));
        s.rng = 0x2545F491u;
    }

    void prepare(SynthSlotScratch& scratch, const SynthParams& p) override {
        Slot& s = scratch.as<Slot>();
        if (p.count < kParamCount) return;
        if (!s.valid || s.serial != p.serial) refresh(s, p);
        // Free-running LFO: one phase per slot.
        s.freePh += s.lfoInc;
        if (s.freePh >= 1.0f) {
            s.freePh -= float(int(s.freePh));
            s.rng = lcg(s.rng);
            s.freeSh = lcgFloat(s.rng);
        }
        s.freeVal = lfoShape(s.wave, s.freePh, s.freeSh);
    }

    void noteOn(SynthVoice& v, const SynthParams& p, const SynthNote& n) override {
        Voice& a = v.as<Voice>();
        Slot& s = v.slotScratch->as<Slot>();
        if (p.count < kParamCount) return;
        if (!s.valid || s.serial != p.serial) refresh(s, p);
        const int mode = s.glideMode;
        const bool alwaysGlide = mode == kAlways || mode == kLegato;
        bool hasFrom = false;
        float from = n.pitch;
        if (n.reused) {
            hasFrom = true;
            from = a.pitch;
        } else if (s.hasLast) {
            hasFrom = true;
            from = s.lastPitch;
        }
        a.target = n.pitch;
        if (s.glideBlocks > 0.0f && hasFrom && !n.fxGlide && (alwaysGlide || n.overlap) && from != n.pitch) {
            a.pitch = from;
            a.glide = std::fabs(n.pitch - from) / s.glideBlocks;
        } else {
            a.pitch = n.pitch;
            a.glide = 0.0f;
        }
        const bool legato = (mode == kLegato || mode == kLegatoOverlap) && n.overlap;
        if (!n.reused) {
            s.rng = lcg(s.rng ^ (uint32_t(n.note) * 0x9E3779B9u));
            a.rng = s.rng | 1u;
        }
        if (!legato || a.ampSt == kIdle) {
            a.ampSt = kAttack;
            a.modSt = kAttack;
            if (s.retrig != kFree) {
                a.lfoPh = 0.0f;
                a.flags &= uint8_t(~kLfoDone);
                a.rng = lcg(a.rng);
                a.lfoSh = lcgFloat(a.rng);
            }
        }
        s.lastPitch = n.pitch;
        s.hasLast = 1;
        v.level = a.amp;
    }

    void noteOff(SynthVoice& v, const SynthParams& p) override {
        (void)p;
        Voice& a = v.as<Voice>();
        if (a.ampSt != kIdle) a.ampSt = kRelease;
        if (a.modSt != kIdle) a.modSt = kRelease;
    }

    bool render(SynthVoice& v, const SynthParams& p, float g0, float gInc, float* L, float* R) override;

private:
    static void refresh(Slot& s, const SynthParams& p) {
        const float* e = p.end;
        s.serial = p.serial;
        s.valid = 1;
        s.amp = envCoef(p, kAmpA);
        s.mod = envCoef(p, kModA);
        s.wave = uint8_t(clampf(e[kLfoWave], 0.0f, 5.0f));
        s.retrig = uint8_t(clampf(e[kLfoRetrig], 0.0f, 2.0f));
        s.ftype = uint8_t(clampf(e[kFilterType], 0.0f, 2.0f));
        s.subMode = uint8_t(clampf(e[kSubMode], 0.0f, 5.0f));
        s.glideMode = uint8_t(clampf(e[kGlideMode], 0.0f, 3.0f));
        s.lfoInc = clampf(e[kLfoHz], 0.0f, 100.0f) * kBlockSec;
        const float res = clampf(e[kResonance], 0.0f, 1.5f), r = res * (1.0f / 1.5f);
        if (s.ftype == kRd3)
            s.kRes = ladderK(res);
        else if (s.ftype == kSvf24)
            s.kRes = 0.7654f * exp2Fast(-4.6f * r);   // 4th-order Butterworth high-Q section at 0
        else
            s.kRes = 1.4142f * exp2Fast(-5.3f * r);
        const float fe = clampf(e[kFilterEnv], -1.0f, 1.0f), fl = clampf(e[kFilterLfo], -1.0f, 1.0f);
        s.envOct = (fe < 0.0f ? -10.0f : 10.0f) * std::sqrt(std::fabs(fe));
        s.lfoOct = (fl < 0.0f ? -10.0f : 10.0f) * std::sqrt(std::fabs(fl));
        s.pitchLfo = 12.0f * clampf(e[kPitchLfo], -3.0f, 3.0f);
        s.noteTrack = clampf(e[kNoteTrack], -2.0f, 2.0f) * (1.0f / 12.0f);
        s.cutoff = clampf(e[kCutoff], 20.0f, 20000.0f);
        const float gt = clampf(e[kGlideTime], 0.0f, 3.0f);
        s.glideBlocks = gt < 0.0005f ? 0.0f : (gt < kBlockSec ? 1.0f : gt / kBlockSec);
    }
};

OT_FASTRUN_FN bool AcdEngine::render(SynthVoice& v, const SynthParams& p, float g0, float gInc, float* L, float* R) {
    Voice& a = v.as<Voice>();
    if (a.ampSt == kIdle || p.count < kParamCount) return false;
    Slot& s = v.slotScratch->as<Slot>();
    const float* e0 = p.start;
    const float* e1 = p.end;

    // 1. control: envelopes, LFO, glide ------------------------------------------
    const float amp0 = a.amp;
    envStep(a.amp, a.ampSt, s.amp);
    envStep(a.mod, a.modSt, s.mod);
    float lfo;
    if (s.retrig == kFree) {
        lfo = s.freeVal;
    } else {
        if (!(a.flags & kLfoDone)) {
            a.lfoPh += s.lfoInc;
            if (a.lfoPh >= 1.0f) {
                if (s.retrig == kOneShot) {
                    a.lfoPh = 1.0f;
                    a.flags |= kLfoDone;
                } else {
                    a.lfoPh -= float(int(a.lfoPh));
                    a.rng = lcg(a.rng);
                    a.lfoSh = lcgFloat(a.rng);
                }
            }
        }
        lfo = lfoShape(s.wave, a.lfoPh, a.lfoSh);
    }
    if (a.glide > 0.0f) {
        const float d = a.target - a.pitch;
        if (std::fabs(d) <= a.glide) {
            a.pitch = a.target;
            a.glide = 0.0f;
        } else {
            a.pitch += d > 0.0f ? a.glide : -a.glide;
        }
    }
    const float pitch = a.pitch + v.bend;
    const float semis = clampf(pitch + s.pitchLfo * lfo, -48.0f, 160.0f);
    const float inc1 = clampf(exp2Fast((semis - 69.0f) * (1.0f / 12.0f)) * (440.0f / kSampleRate), 1.0e-7f, kMaxInc);
    const float duty1 = clampf(e1[kPw] + e1[kPwLfo] * lfo + e1[kPwEnv] * a.mod, 0.03f, 0.97f);
    // Filter coefficients at the block end.
    const float oct = s.noteTrack * (pitch - 60.0f) + s.envOct * a.mod + s.lfoOct * lfo + v.cutoffOct;   // + Performance (#23)
    const float fc = clampf(s.cutoff * exp2Fast(clampf(oct, -20.0f, 20.0f)), 16.0f, 0.45f * kSampleRate);
    const float g = tanPade(kPiF * fc * (1.0f / kSampleRate));
    float c1[6];
    const int ft = s.ftype;
    if (ft == kRd3) {
        const float G = g / (1.0f + g), G2 = G * G;
        c1[0] = G;
        c1[1] = 1.0f / (1.0f + s.kRes * G2 * G2);
        c1[2] = s.kRes;
        c1[3] = c1[4] = c1[5] = 0.0f;
    } else if (ft == kSvf24) {
        svfCoef(g, 1.8478f, c1);
        svfCoef(g, s.kRes, c1 + 3);
    } else {
        svfCoef(g, s.kRes, c1);
        c1[3] = c1[4] = c1[5] = 0.0f;
    }
    if (!(a.flags & kPrimed) || a.ft != ft) {
        if ((a.flags & kPrimed) && a.ft != ft) a.s[0] = a.s[1] = a.s[2] = a.s[3] = 0.0f;
        a.inc = inc1;
        a.duty = duty1;
        std::memcpy(a.c, c1, sizeof(c1));
        a.ft = uint8_t(ft);
        a.flags |= kPrimed;
    }
    constexpr float kInv = 1.0f / float(kBlock);

    // 2. oscillators ---------------------------------------------------------------
    float buf[kBlock];
    const bool ladderType = ft == kRd3;
    const float in0 = ladderType ? kOscGain : kOscGain * svfInGain(e0[kResonance]);
    const float in1 = ladderType ? kOscGain : kOscGain * svfInGain(e1[kResonance]);
    Osc o;
    o.ph = a.phase;
    o.inc = a.inc;
    o.dInc = (inc1 - a.inc) * kInv;
    o.idt = 2.0f / (a.inc + inc1);
    o.duty = a.duty;
    o.dDuty = (duty1 - a.duty) * kInv;
    const float sq0 = e0[kSquareMix] * in0, sq1 = e1[kSquareMix] * in1;
    const float saw0 = e0[kSawMix] * in0 - sq0, saw1 = e1[kSawMix] * in1 - sq1;
    const float sub0 = e0[kSubMix] * in0, sub1 = e1[kSubMix] * in1;
    o.wSaw = saw0;
    o.dSaw = (saw1 - saw0) * kInv;
    o.wSq = sq0;
    o.dSq = (sq1 - sq0) * kInv;
    o.wSub = sub0;
    o.dSub = (sub1 - sub0) * kInv;
    o.cnt = a.sub;
    const int subMode = s.subMode;
    const bool pulse = sq0 != 0.0f || sq1 != 0.0f;
    const int subKind = (sub0 != 0.0f || sub1 != 0.0f) ? ((subMode % 3) == 2 ? 2 : (subMode == 1 ? 3 : 1)) : 0;
    if (subKind) subTables(subMode, o);
    kOscFns[pulse ? 1 : 0][subKind](o, buf);
    a.phase = o.ph;
    a.sub = o.cnt & 3u;
    a.inc = inc1;
    a.duty = duty1;

    // 3. filter, with the pre-filter noise ----------------------------------------------
    const float nz0 = e0[kNoiseMix], nz1 = e1[kNoiseMix];
    Noise nz;
    nz.rng = a.rng;
    nz.w = (nz0 > 0.0f ? nz0 : 0.0f) * in0;
    nz.dw = ((nz1 > 0.0f ? nz1 : 0.0f) * in1 - nz.w) * kInv;
    const bool pre = nz0 > 0.0f || nz1 > 0.0f;
    float dc[6];
    for (int i = 0; i < 6; ++i) dc[i] = (c1[i] - a.c[i]) * kInv;
    const float mk0 = ladderType ? 1.0f + 0.5f * a.c[2] : 1.0f, mk1 = ladderType ? 1.0f + 0.5f * c1[2] : 1.0f;
    if (ft == kRd3)
        (pre ? ladder<true> : ladder<false>)(buf, a.s, a.c, dc, nz);
    else if (ft == kSvf24)
        (pre ? svf24<true> : svf24<false>)(buf, a.s, a.c, dc, nz);
    else
        (pre ? svf12<true> : svf12<false>)(buf, a.s, a.c, dc, nz);
    uint32_t rng = nz.rng;
    std::memcpy(a.c, c1, sizeof(c1));
    for (float& z : a.s)
        if (std::fabs(z) < 1.0e-20f) z = 0.0f;   // no denormals in silence

    // 4. post-filter noise, amplitude, output ----------------------------------------------
    const float amp1 = a.amp;
    float env = amp0 * g0 * mk0;
    // env x gain (x ladder make-up) is a product of ramps; ramp it linearly
    // between the exact end points (the error is < 1 % of a block step).
    const float envEnd = amp1 * (g0 + gInc * float(kBlock)) * mk1;
    const float dEnv = (envEnd - env) * kInv;
    if (nz0 < 0.0f || nz1 < 0.0f) {
        float w = (nz0 < 0.0f ? -nz0 : 0.0f) * kPostNoise / mk0;
        const float dw = ((nz1 < 0.0f ? -nz1 : 0.0f) * kPostNoise / mk1 - w) * kInv;
        for (uint32_t i = 0; i < kBlock; ++i) {
            rng = lcg(rng);
            env += dEnv;
            const float y = (buf[i] + w * lcgFloat(rng)) * env;
            w += dw;
            L[i] += y;
            R[i] += y;
        }
    } else {
        for (uint32_t i = 0; i < kBlock; ++i) {
            env += dEnv;
            const float y = buf[i] * env;
            L[i] += y;
            R[i] += y;
        }
    }
    a.rng = rng;
    v.level = amp1;
    if (amp1 < kDoneLevel && (a.ampSt == kRelease || (a.ampSt == kDecay && s.amp.sus <= 0.0f))) {
        a.ampSt = kIdle;
        return false;
    }
    return true;
}

AcdEngine gAcd;

}  // namespace

ISynthEngine* synthAcdEngine() { return &gAcd; }

}  // namespace audio
}  // namespace ot
