// BBD-style stereo chorus (header-only DSP, module-facing): the classic two-mode
// BBD chorus of the BBDC master insert (opentracker-fx modules/bbd) and of the
// JU6 synth's built-in chorus (opentracker-synths modules/ju6, once per slot).
// One copy of the DSP for both repositories; each caller owns its State and
// its kLine-frame line (static storage of the module, declared in its
// ModuleInfo) and passes its targets per block.
//
// After the published analyses of the classic two-mode BBD chorus: the mono sum passes
// the BBD's low-pass filters (three one-pole stages at Tone, 6..12 kHz) and a
// gentle soft clip (standing in for the compander) into one delay line (int16,
// like a bucket brigade), read by two taps moved by one triangle LFO: the
// left tap at centre + depth * tri, the right one at centre - depth * tri
// (opposite phase). The original filters before and after its BBD; both are
// linear and the delay moves slowly, so here all stages run before the line
// (the same response, and a silent line reads exact zeros). Each tap is mixed
// over the dry signal. The mode fixes rate and depth as the original's buttons
// do (I: 0.5 Hz, 2.0..5.0 ms; II: 0.8 Hz, 1.5..5.5 ms; I+II: a fast, shallow
// wobble, 9.75 Hz, 3.1..3.9 ms, as the Juno-106's I+II). No hiss: silence
// stays silence. Our own design; textbook technique.
#pragma once

#include <cstdint>

#include "audio/audio_tables.h"

namespace ot {
namespace audio {
namespace bbd {

constexpr int kLine = 256;   // frames (5.8 ms), int16 (512 B)

// What a chorus mode button fixes: LFO rate and the tap delay swing.
struct Mode {
    float hz;       // triangle LFO rate
    float centre;   // centre delay, frames
    float depth;    // swing either side of the centre, frames
};
constexpr float kMs = 0.001f * kSampleRate;   // frames per millisecond
constexpr int kModeCount = 3;                 // mode 1..kModeCount (0 = Off)
constexpr Mode kModes[kModeCount] = {
    {0.5f, 3.5f * kMs, 1.5f * kMs},    // Chorus I: 2.0 .. 5.0 ms
    {0.8f, 3.5f * kMs, 2.0f * kMs},    // Chorus II: 1.5 .. 5.5 ms
    {9.75f, 3.5f * kMs, 0.4f * kMs},   // Chorus I+II: 3.1 .. 3.9 ms
};
static_assert(kModes[1].centre + kModes[1].depth + 2.0f < float(kLine), "bbd line too short");
static_assert(kModes[0].centre + kModes[0].depth + 2.0f < float(kLine), "bbd line too short");
static_assert(kModes[2].centre + kModes[2].depth + 2.0f < float(kLine), "bbd line too short");

// The BBD filters' cutoff for Tone t (0..100), Hz.
inline float toneHz(uint8_t t) { return 6000.0f * __builtin_exp2f(0.01f * float(t)); }

// One chorus. All zero is a fresh state: no Tone coefficient yet, Width
// jumps to its first target, the taps start at the mode's delays.
struct State {
    uint32_t w;            // line write index
    float ph;              // LFO phase, cycles
    float dl, dr;          // tap delays the last block ended on (0: start at the mode's)
    float wet;             // the wet amount the last block ended on (ramps)
    float width;           // the width the last block ended on
    float g;               // one-pole coefficient of the BBD filters
    float lp[3];           // the BBD filters' states
    uint8_t mode;          // the mode the last block ran (the fade after Off keeps it)
    uint8_t tone;          // the Tone g was computed for
    uint8_t toneSet;       // g is valid for tone
    uint8_t widthSet;      // width is valid (0: jump to the next target)
};

namespace detail {
inline float absf(float x) { return __builtin_fabsf(x); }
// Triangle, x in [0, 1): 1 at 0, -1 at 1/2.
inline float tri(float x) { return 4.0f * absf(x - 0.5f) - 1.0f; }
// Soft clip: unity slope at 0, flat at +-1.5 -> +-1.
inline float clip(float x) {
    x = 0.5f * (absf(x + 1.5f) - absf(x - 1.5f));
    return x - (4.0f / 27.0f) * x * x * x;
}
inline int16_t toQ15(float x) {
    x = x > 0.99997f ? 0.99997f : (x < -1.0f ? -1.0f : x);
    return int16_t(x * 32768.0f);
}
inline float flush(float x) { return absf(x) < 1.0e-20f ? 0.0f : x; }
}  // namespace detail

// Silence: an empty line, the LFO and taps back to their start (Tone kept).
inline void clear(State& s, int16_t* line) {
    for (int i = 0; i < kLine; ++i) line[i] = 0;
    s.w = 0;
    s.ph = s.dl = s.dr = s.wet = 0.0f;
    s.mode = 1;
    s.widthSet = 0;
    s.lp[0] = s.lp[1] = s.lp[2] = 0.0f;
}

// Frames of silent input after which the line holds exact zeros again (the
// filters fall below the int16 step within a few frames, then one line
// length): from then on a silent input gives a silent output.
constexpr uint32_t kTailFrames = uint32_t(kLine) + uint32_t(kBlock);

// One block in place. mode 0 (Off) .. kModeCount; wetEnd 0..1 (1: dry and
// wet equal, as on the original) and widthEnd 0..1 (0: both taps on both sides,
// 1: one tap per side) are reached at the block end; tone 0..100. Returns
// false when Off and faded out: l / r untouched (and the line cleared).
// Out of line: inlined with a constant n, the compiler unrolls it (+0.7 KB
// flash for no measured gain).
__attribute__((noinline)) inline bool process(State& s, int16_t* line, uint8_t mode, float wetEnd, float widthEnd, uint8_t tone, float* l, float* r,
                    uint32_t n) {
    using namespace detail;
    if (mode > kModeCount) mode = kModeCount;
    if (!mode) wetEnd = 0.0f;
    if (wetEnd <= 0.0f && s.wet <= 0.0f) return false;   // Off and faded out: untouched
    if (!s.toneSet || tone != s.tone) {
        s.tone = tone;
        s.toneSet = 1;
        s.g = 1.0f - __builtin_expf(-6.2831853f * toneHz(tone) / kSampleRate);
    }
    if (!s.widthSet) {
        s.width = widthEnd;
        s.widthSet = 1;
    }
    // While fading out after Off the taps keep the last mode's motion.
    if (mode) s.mode = mode;
    const Mode& md = kModes[s.mode - 1];
    // The triangle is a straight line between its corners: the tap delays at
    // the block ends, ramped from where the last block ended (a corner inside
    // a block rounds it off; a mode switch glides over one block).
    float ph1 = s.ph + md.hz * float(n) / kSampleRate;
    ph1 -= float(int(ph1));
    const float t1 = tri(ph1);
    if (s.dl <= 0.0f) {
        s.dl = md.centre + md.depth * tri(s.ph);
        s.dr = md.centre - md.depth * tri(s.ph);
    }
    const float inv = 1.0f / float(n);
    float dl = s.dl, dr = s.dr;
    const float dl1 = md.centre + md.depth * t1, dr1 = md.centre - md.depth * t1;
    const float ddl = (dl1 - dl) * inv, ddr = (dr1 - dr) * inv;
    // Gains at the block ends, ramped: dry ducks as the wet comes in (at wet
    // 1 both 0.7); the taps' sum on both sides, their difference scaled by
    // Width. kG: 0.7 / 2 with the int16 line's 1 / 32768 folded in.
    constexpr float kG = 0.35f / 32768.0f;
    const float wet = s.wet;
    float dry = 1.0f - 0.3f * wet, mid = kG * wet, side = kG * wet * s.width;
    const float ddry = -0.3f * (wetEnd - wet) * inv, dmid = kG * (wetEnd - wet) * inv;
    const float dside = (kG * wetEnd * widthEnd - side) * inv;
    const float g = s.g;
    const float gh = 0.5f * g;   // the mono sum's 1/2 folded in
    float a0 = s.lp[0], a1 = s.lp[1], a2 = s.lp[2];
    constexpr uint32_t kMask = uint32_t(kLine - 1);
    uint32_t w = s.w;
    for (uint32_t i = 0; i < n; ++i) {
        a0 += gh * (l[i] + r[i]) - g * a0;
        a1 += g * (a0 - a1);
        a2 += g * (a1 - a2);
        line[w] = toQ15(clip(a2));
        // A tap d = k + f frames back: between w - k and w - k - 1.
        const uint32_t kl = uint32_t(dl), kr = uint32_t(dr);
        const float fl = dl - float(kl), fr = dr - float(kr);
        const float x0 = float(line[(w - kl) & kMask]), x1 = float(line[(w - kl - 1u) & kMask]);
        const float y0 = float(line[(w - kr) & kMask]), y1 = float(line[(w - kr - 1u) & kMask]);
        const float tl = x0 + fl * (x1 - x0), tr = y0 + fr * (y1 - y0);
        const float m = mid * (tl + tr), sd = side * (tl - tr);
        l[i] = dry * l[i] + (m + sd);
        r[i] = dry * r[i] + (m - sd);
        w = (w + 1u) & kMask;
        dl += ddl;
        dr += ddr;
        dry += ddry;
        mid += dmid;
        side += dside;
    }
    s.w = w;
    s.ph = ph1;
    s.dl = dl1;
    s.dr = dr1;
    s.wet = wetEnd;
    s.width = widthEnd;
    // No denormals as the filters decay in silence (the line truncates them).
    s.lp[0] = flush(a0);
    s.lp[1] = flush(a1);
    s.lp[2] = flush(a2);
    if (wetEnd <= 0.0f) {
        clear(s, line);
        return false;
    }
    return true;
}

}  // namespace bbd
}  // namespace audio
}  // namespace ot
