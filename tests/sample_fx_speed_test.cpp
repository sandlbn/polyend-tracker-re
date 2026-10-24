// The faster Sample Editor effects (app/sample_fx.cpp) against the version
// before the speed work (sample_fx_ref.cpp): every effect, at its defaults and
// at settings that do work, on a kick, a tonal mix with noise and a drum loop.
// The output must be identical sample for sample unless a case says otherwise
// (the tolerance is printed per case either way).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "test.h"

#include "app/sample_fx.h"
#include "sample_fx_ref.h"

using namespace ot;

namespace {

std::vector<int16_t> kick() {
    std::vector<int16_t> s(19845);
    double ph = 0.0;
    for (size_t i = 0; i < s.size(); ++i) {
        const double t = double(i) / 44100.0;
        ph += 2.0 * 3.14159265358979 * (45.0 + 120.0 * std::exp(-t * 30.0)) / 44100.0;
        s[i] = int16_t(28000.0 * std::exp(-t * 7.0) * std::sin(ph));
    }
    return s;
}

uint32_t g_rng = 1;
int noise() {
    g_rng = g_rng * 1664525u + 1013904223u;
    return int(g_rng >> 16) - 32768;
}

std::vector<int16_t> mix(uint32_t n) {
    std::vector<int16_t> s(n);
    for (uint32_t i = 0; i < n; ++i) {
        const double t = double(i) / 44100.0;
        const double f = (int(t * 2.0) % 2) ? 220.0 : 164.81;
        s[i] = int16_t(9000.0 * std::sin(2.0 * 3.14159265358979 * f * t) +
                       6000.0 * std::sin(2.0 * 3.14159265358979 * f * 1.5 * t) + 0.08 * noise());
    }
    return s;
}

// Four bars of noise-burst "drums" at 120 BPM: kick on 1 and 3, snare on 2 and 4, hats.
std::vector<int16_t> loop() {
    std::vector<int16_t> s(4 * 88200, 0);
    const uint32_t beat = 22050;
    for (uint32_t b = 0; b < 16; ++b)
        for (uint32_t i = 0; i < beat; ++i) {
            const double t = double(i) / 44100.0;
            double v = 0.15 * noise() * std::exp(-t * 60.0);   // hat on every beat
            if (b % 2 == 0) v += 26000.0 * std::exp(-t * 12.0) * std::sin(2.0 * 3.14159265358979 * 55.0 * t);
            else v += 0.6 * noise() * std::exp(-t * 18.0);       // snare
            const double y = double(s[b * beat + i]) + v;
            s[b * beat + i] = int16_t(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
        }
    return s;
}

struct Diff {
    uint32_t lenNew = 0, lenRef = 0, differing = 0;
    int maxAbs = 0;
};

Diff compare(int fx, const float* p, const std::vector<int16_t>& in) {
    const uint32_t m = sfx::outputFrames(fx, p, uint32_t(in.size()));
    std::vector<int16_t> a(m + 16, 0x5a5a), b(m + 16, 0x5a5a);
    Diff d;
    d.lenNew = sfx::process(fx, p, in.data(), uint32_t(in.size()), a.data(), m);
    d.lenRef = sfx_ref::process(fx, p, in.data(), uint32_t(in.size()), b.data(), m);
    for (uint32_t i = 0; i < d.lenNew && i < d.lenRef; ++i) {
        const int e = std::abs(int(a[i]) - int(b[i]));
        if (e) ++d.differing;
        if (e > d.maxAbs) d.maxAbs = e;
    }
    return d;
}

// One case: prints the difference, checks it against the allowed LSB error.
void check(const char* sig, int fx, const float* p, const std::vector<int16_t>& in, int allowLsb = 0) {
    const Diff d = compare(fx, p, in);
    std::printf("  %-6s %-17s p0=%-7g p1=%-7g p3=%-4g len %7u  differing %6u  max %5d LSB\n", sig,
                sfx::effect(fx).name, double(p[0]), double(p[1]), double(p[3]), d.lenNew, d.differing, d.maxAbs);
    CHECK_EQ(d.lenNew, d.lenRef);
    CHECK(d.maxAbs <= allowLsb);
}

void allEffects(const char* sig, const std::vector<int16_t>& in) {
    for (int fx = 0; fx < sfx::kEffectCount; ++fx) {
        float p[sfx::kMaxParams];
        sfx::defaults(fx, p);
        check(sig, fx, p, in);
    }
}

void stretches(const char* sig, const std::vector<int16_t>& in) {
    float p[sfx::kMaxParams];
    // Timestretch Beat: tempo, steps, micro-step, granule (quarters).
    for (const float g : {1.0f, 4.0f, 10.0f})
        for (const float steps : {4.0f, 16.0f, 64.0f}) {
            sfx::defaults(sfx::kStretchBeat, p);
            p[1] = steps;
            p[3] = g;
            check(sig, sfx::kStretchBeat, p, in);
        }
    // Timestretch Note: length (s), granule (ms).
    for (const float len : {0.2f, 1.0f, 3.0f})
        for (const float gr : {10.0f, 100.0f, 1000.0f}) {
            sfx::defaults(sfx::kStretchNote, p);
            p[0] = len;
            p[1] = gr;
            check(sig, sfx::kStretchNote, p, in);
        }
}

}  // namespace

TEST(all_effects_at_defaults_match_the_reference) {
    allEffects("kick", kick());
    allEffects("mix", mix(3 * 44100));
}

TEST(time_stretch_matches_the_reference) {
    stretches("kick", kick());
    stretches("mix", mix(3 * 44100));
    stretches("loop", loop());
}

