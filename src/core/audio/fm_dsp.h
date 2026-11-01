// Shared four-operator FM DSP (header-only, inline only, module-facing): the
// operator kernel of the Arc-Type synth voice (opentracker-synths
// modules/arc; docs/PLAN_RETRO.md §2). Lifted from the reviewed FM4 engine
// (modules/fm4/src/fm4.cpp, ours) and reorganised; FM4 keeps its own copy in
// 3.0.0 (its golden hashes pin it). Nothing here is compiled into an image
// unless a module uses it.
//
//  - sinCyc: sin(2 pi x) for a phase in cycles (a refined parabola, error
//    ~0.11 %), fracf: the phase wrap;
//  - Alg / kAlgs: the eight usual four-operator topologies (for each
//    operator the operators that modulate it, and the carriers);
//  - kernel<A>: n frames of four operators in algorithm A: the phases run
//    1..4, each modulated by the outputs of the operators routed into it (this
//    sample's values), operator 1 by its own last two outputs (Feedback). A
//    modulator at amplitude 1 moves its target's phase by one cycle. The
//    carriers' mean goes to the engine's per-sample hook out(i, y) (Arc-Type
//    quantises it there). Increments and amplitudes ramp linearly per frame
//    (in scalars, so the hook's stores cannot alias them; written back at the
//    end: a next chunk continues the ramps).
#pragma once

#include <cstdint>

namespace ot {
namespace audio {
namespace fm {

constexpr int kOps = 4;

// x - floor(x) for x > -8.
inline float fracf(float x) {
    x += 8.0f;
    return x - float(int(x));
}
// sin(2 pi x), x in [0, 1): refined parabola (error ~0.11 % of full scale).
inline float sinCyc(float x) {
    const float t = 2.0f * x - 1.0f;
    const float y = 4.0f * t * (1.0f - __builtin_fabsf(t));
    return -(y + 0.225f * (y * __builtin_fabsf(y) - y));
}

// For each operator the operators modulating it (bit j = operator j + 1), and
// the carriers (bit k = operator k + 1).
struct Alg {
    uint8_t mod[kOps];
    uint8_t carriers;
};
inline constexpr Alg kAlgs[8] = {
    {{0, 1, 2, 4}, 8},    // 1 > 2 > 3 > 4
    {{0, 0, 3, 4}, 8},    // (1 + 2) > 3 > 4
    {{0, 0, 2, 5}, 8},    // (1 + (2 > 3)) > 4
    {{0, 1, 0, 6}, 8},    // ((1 > 2) + 3) > 4
    {{0, 1, 0, 4}, 10},   // 1 > 2, 3 > 4
    {{0, 1, 1, 1}, 14},   // 1 > (2, 3, 4)
    {{0, 1, 0, 0}, 14},   // 1 > 2, 3, 4
    {{0, 0, 0, 0}, 15},   // 1, 2, 3, 4
};
constexpr int carrierCount(int a) {
    return ((kAlgs[a].carriers >> 0) & 1) + ((kAlgs[a].carriers >> 1) & 1) + ((kAlgs[a].carriers >> 2) & 1) +
           ((kAlgs[a].carriers >> 3) & 1);
}

// The operators of one voice: phases (cycles) and operator 1's last two outputs.
struct Ops {
    float ph[kOps];
    float fbA, fbB;
};
// Per-frame values at the chunk start and their per-frame increments.
struct OpRamp {
    float inc[kOps], dinc[kOps];   // phase increments (cycles per frame)
    float amp[kOps], damp[kOps];   // operator output amplitudes
    float fb;                      // feedback depth (cycles per unit of the two-sample mean x 2)
};

template <int kA, class Out>
inline void kernel(Ops& V, OpRamp& ramp, uint32_t n, Out& out) {
    constexpr Alg a = kAlgs[kA];
    constexpr float kOut = 1.0f / float(carrierCount(kA));
    // Scalars (not the ramp's arrays): they stay in registers, and the hook's
    // stores cannot alias them.
    float i0 = ramp.inc[0], i1 = ramp.inc[1], i2 = ramp.inc[2], i3 = ramp.inc[3];
    const float di0 = ramp.dinc[0], di1 = ramp.dinc[1], di2 = ramp.dinc[2], di3 = ramp.dinc[3];
    float a0 = ramp.amp[0], a1 = ramp.amp[1], a2 = ramp.amp[2], a3 = ramp.amp[3];
    const float da0 = ramp.damp[0], da1 = ramp.damp[1], da2 = ramp.damp[2], da3 = ramp.damp[3];
    const float fbk = ramp.fb;
    float p0 = V.ph[0], p1 = V.ph[1], p2 = V.ph[2], p3 = V.ph[3];
    float fa = V.fbA, fb = V.fbB;
    for (uint32_t i = 0; i < n; ++i) {
        p0 += i0;
        p1 += i1;
        p2 += i2;
        p3 += i3;
        if (p0 >= 1.0f) p0 -= 1.0f;
        if (p1 >= 1.0f) p1 -= 1.0f;
        if (p2 >= 1.0f) p2 -= 1.0f;
        if (p3 >= 1.0f) p3 -= 1.0f;
        const float y0 = a0 * sinCyc(fracf(p0 + fbk * (fa + fb)));
        fb = fa;
        fa = y0;
        float in1 = 0.0f, in2 = 0.0f, in3 = 0.0f;
        if (a.mod[1] & 1) in1 += y0;
        const float y1 = a1 * sinCyc(fracf(p1 + in1));
        if (a.mod[2] & 1) in2 += y0;
        if (a.mod[2] & 2) in2 += y1;
        const float y2 = a2 * sinCyc(fracf(p2 + in2));
        if (a.mod[3] & 1) in3 += y0;
        if (a.mod[3] & 2) in3 += y1;
        if (a.mod[3] & 4) in3 += y2;
        const float y3 = a3 * sinCyc(fracf(p3 + in3));
        float y = 0.0f;
        if (a.carriers & 1) y += y0;
        if (a.carriers & 2) y += y1;
        if (a.carriers & 4) y += y2;
        if (a.carriers & 8) y += y3;
        out(i, y * kOut);
        i0 += di0;
        i1 += di1;
        i2 += di2;
        i3 += di3;
        a0 += da0;
        a1 += da1;
        a2 += da2;
        a3 += da3;
    }
    ramp.inc[0] = i0;
    ramp.inc[1] = i1;
    ramp.inc[2] = i2;
    ramp.inc[3] = i3;
    ramp.amp[0] = a0;
    ramp.amp[1] = a1;
    ramp.amp[2] = a2;
    ramp.amp[3] = a3;
    V.ph[0] = p0;
    V.ph[1] = p1;
    V.ph[2] = p2;
    V.ph[3] = p3;
    V.fbA = __builtin_fabsf(fa) < 1.0e-20f ? 0.0f : fa;
    V.fbB = __builtin_fabsf(fb) < 1.0e-20f ? 0.0f : fb;
}

}  // namespace fm
}  // namespace audio
}  // namespace ot
