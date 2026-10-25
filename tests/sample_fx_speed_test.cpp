// The faster Sample Editor effects (app/sample_fx.cpp) against the version
// before the speed work (sample_fx_ref.cpp): every effect, at its defaults and
// at settings that do work, on a kick, a tonal mix with noise and a drum loop.
// The output must be identical sample for sample unless a case says otherwise
// (the tolerance is printed per case either way).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
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

// The per-sample effects run in float now (the reference: double), so the
// truncated int16 result may land one step away where the exact value is
// within float rounding of an integer. These bounds hold host to host: on the
// M4 the float effects differ slightly (FMA contraction, newlib's libm).
int allowed(int fx) {
    switch (fx) {
        case sfx::kNormalizer: case sfx::kAmplifier: case sfx::kOverdrive: case sfx::kDelay:
        case sfx::kFlanger: case sfx::kLimiter: case sfx::kCompressor:
        case sfx::kWavetableSmoother: case sfx::kFadeIn: case sfx::kFadeOut: return 1;
        default: return 0;
    }
}

void allEffects(const char* sig, const std::vector<int16_t>& in) {
    for (int fx = 0; fx < sfx::kEffectCount; ++fx) {
        float p[sfx::kMaxParams];
        sfx::defaults(fx, p);
        check(sig, fx, p, in, allowed(fx));
    }
}

// Settings that do work (the defaults of several leave the sample unchanged).
// allow: the LSB error accepted for this case (-1: the effect's default).
struct Setting {
    int fx;
    std::vector<std::pair<int, float>> set;
    int allow = -1;
};
const Setting kWork[] = {
    {sfx::kNormalizer, {{0, 50}}},
    {sfx::kAmplifier, {{0, 1.5f}}},
    {sfx::kAmplifier, {{0, 4.99f}}},
    {sfx::kOverdrive, {{0, 2.5f}, {1, 0}}},
    {sfx::kOverdrive, {{0, 2.5f}, {1, 1}}},
    {sfx::kOverdrive, {{0, 5}, {1, 2}}},
    {sfx::kDelay, {{0, 0.95f}, {1, 120}}},
    {sfx::kDelay, {{0, 0.5f}, {1, 7}}},
    {sfx::kChorus, {{1, 4}}},
    {sfx::kFlanger, {{0, 4096}, {1, 30}, {2, 8}, {3, 5}}},
    {sfx::kFlanger, {{0, 1024}, {1, 90}, {2, 3}, {3, 0.3f}}},
    // The EQ stays in double: identical to the reference. Widths: 0.5 is the
    // widest band, 5 the narrowest.
    {sfx::kEqualizer, {{0, 6}, {3, -3}, {6, 4}}},
    {sfx::kEqualizer, {{0, 12}, {1, 10}, {2, 0.5f}}},                     // 10 Hz, widest, +12 dB
    {sfx::kEqualizer, {{0, 12}, {1, 10}, {2, 5}}},                        // 10 Hz, narrowest, +12 dB
    {sfx::kEqualizer, {{0, -12}, {1, 200}, {3, 12}, {4, 100}, {6, -12}, {7, 20000}}},
    {sfx::kEqualizer, {{0, 12}, {1, 60}, {2, 1}}},                        // +12 dB at 60 Hz
    {sfx::kLimiter, {{0, -20}}},
    {sfx::kLimiter, {{0, -40}, {1, 0.3f}, {2, 2000}}},
    {sfx::kCompressor, {{0, -30}, {1, 8}}},
    {sfx::kCompressor, {{0, -60}, {1, 50}, {2, 0.3f}, {3, 2000}}},
    {sfx::kCompressor, {{0, -30}, {1, 1.5f}, {3, 2000}}},                 // gentle, slow release
    {sfx::kWavetableSmoother, {{0, 64}, {1, 512}}},
    {sfx::kBitcrusher, {{0, 3}, {1, 4000}}},
};

void workSettings(const char* sig, const std::vector<int16_t>& in) {
    for (const Setting& w : kWork) {
        float p[sfx::kMaxParams];
        sfx::defaults(w.fx, p);
        for (const auto& kv : w.set) p[kv.first] = kv.second;
        check(sig, w.fx, p, in, w.allow >= 0 ? w.allow : allowed(w.fx));
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

TEST(per_sample_effects_within_one_lsb) {
    workSettings("kick", kick());
    workSettings("mix", mix(3 * 44100));
    workSettings("loop", loop());
}

TEST(time_stretch_matches_the_reference) {
    stretches("kick", kick());
    stretches("mix", mix(3 * 44100));
    stretches("loop", loop());
}

