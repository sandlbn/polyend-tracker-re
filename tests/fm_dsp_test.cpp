// Shared four-operator FM DSP (audio/fm_dsp.h, docs/PLAN_RETRO.md §2): the
// sine's error, every algorithm's carriers and its direct modulator routes
// (heard through the kernel, not only read from the table), a 1 > 2 pair's
// sidebands at the carrier +- k x the modulator (Bessel amplitudes), and the
// feedback bounded at its maximum. Runs in every profile (header-only).
#include "test.h"

#include <cmath>
#include <vector>

#include "audio/fm_dsp.h"

using namespace ot::audio;
using namespace ot::audio::fm;

namespace {

constexpr double kFs = 44117.647;
constexpr double kPi = 3.14159265358979;

struct Collect {
    std::vector<float>* buf;
    void operator()(uint32_t, float y) { buf->push_back(y); }
};

using Kern = void (*)(Ops&, OpRamp&, uint32_t, Collect&);
const Kern kK[8] = {kernel<0, Collect>, kernel<1, Collect>, kernel<2, Collect>, kernel<3, Collect>,
                    kernel<4, Collect>, kernel<5, Collect>, kernel<6, Collect>, kernel<7, Collect>};

// n frames of algorithm a, operators at hz[k] with amplitudes amp[k].
std::vector<float> run(int a, const double* hz, const float* amp, float fb, uint32_t n) {
    Ops V{};
    OpRamp r{};
    for (int k = 0; k < kOps; ++k) {
        r.inc[k] = float(hz[k] / kFs);
        r.amp[k] = amp[k];
    }
    r.fb = fb;
    std::vector<float> out;
    Collect c{&out};
    for (uint32_t done = 0; done < n; done += 128) kK[a](V, r, 128, c);
    return out;
}
double rms(const std::vector<float>& x) {
    double s = 0;
    for (float v : x) s += double(v) * v;
    return std::sqrt(s / double(x.size()));
}
// Goertzel magnitude (Hann window) at hz, as a sine amplitude.
double tone(const std::vector<float>& b, double hz) {
    const double w = 2.0 * kPi * hz / kFs, c = 2.0 * std::cos(w), wn = 2.0 * kPi / double(b.size());
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < b.size(); ++i) {
        const double s0 = b[i] * (0.5 - 0.5 * std::cos(wn * double(i))) + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return 4.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / double(b.size());
}

}  // namespace

TEST(fm_sine_error) {
    double worst = 0.0;
    for (int i = 0; i < 100000; ++i) {
        const double x = double(i) / 100000.0;
        worst = std::max(worst, std::fabs(double(sinCyc(float(x))) - std::sin(2.0 * kPi * x)));
    }
    std::printf("  sinCyc |error| <= %.5f\n", worst);
    CHECK(worst < 0.0012);   // about 0.11 % of full scale
    CHECK(std::fabs(fracf(-2.25f) - 0.75f) < 1e-6f);
    CHECK(std::fabs(fracf(3.5f) - 0.5f) < 1e-6f);
}

TEST(fm_algorithms_carriers_and_routes) {
    const int kCarriers[8] = {1, 1, 1, 1, 2, 3, 3, 4};
    for (int a = 0; a < 8; ++a) {
        CHECK_EQ(carrierCount(a), kCarriers[a]);
        for (int k = 0; k < kOps; ++k) {
            // A modulator only ever comes from a lower operator; operator 1 has none (its feedback).
            CHECK_EQ(int(kAlgs[a].mod[k] >> k), 0);
            // Heard alone: only a carrier sounds.
            const double hz[4] = {300, 300, 300, 300};
            float amp[4] = {0, 0, 0, 0};
            amp[k] = 1.0f;
            const double lv = rms(run(a, hz, amp, 0.0f, 4096));
            const bool carrier = (kAlgs[a].carriers >> k) & 1;
            CHECK(carrier ? lv > 0.1 : lv == 0.0);
        }
        // A direct route j > k into a carrier k changes it; a lower operator
        // not routed into k leaves it alone (the others at 0).
        for (int k = 0; k < kOps; ++k) {
            if (!((kAlgs[a].carriers >> k) & 1)) continue;
            for (int j = 0; j < k; ++j) {
                if ((kAlgs[a].carriers >> j) & 1) continue;   // a carrier j is heard anyway
                const double hz[4] = {700, 700, 700, 700};
                float amp0[4] = {0, 0, 0, 0}, amp1[4] = {0, 0, 0, 0};
                amp0[k] = amp1[k] = 1.0f;
                amp1[j] = 0.4f;
                const std::vector<float> x = run(a, hz, amp0, 0.0f, 4096), y = run(a, hz, amp1, 0.0f, 4096);
                double d = 0;
                for (size_t i = 0; i < x.size(); ++i) d += std::fabs(double(x[i] - y[i]));
                const bool routed = (kAlgs[a].mod[k] >> j) & 1;
                CHECK(routed ? d > 1.0 : d == 0.0);
            }
        }
    }
}

TEST(fm_pair_sidebands) {
    // Algorithm 5 (1 > 2, 3 > 4): carrier 2 at 1,000 Hz, modulator 1 at 200 Hz,
    // amplitude 0.3 cycles = index 1.885 rad: sidebands J_k(1.885) at 1000 +- 200 k.
    const double hz[4] = {200, 1000, 50, 50};
    const float amp[4] = {0.3f, 1.0f, 0.0f, 0.0f};
    const std::vector<float> y = run(4, hz, amp, 0.0f, 32768);
    // The carriers' mean (two carriers): halve the Bessel amplitudes.
    const double J[4] = {0.2906, 0.5809, 0.3203, 0.1130};   // J0..J3 of 1.885
    for (int k = 0; k < 4; ++k) {
        const double up = tone(y, 1000.0 + 200.0 * k), dn = tone(y, 1000.0 - 200.0 * k);
        std::printf("  sideband %d: %.4f / %.4f (want %.4f)\n", k, up, dn, 0.5 * J[k]);
        CHECK(std::fabs(up - 0.5 * J[k]) < 0.02);
        CHECK(std::fabs(dn - 0.5 * J[k]) < 0.02);
    }
    CHECK(tone(y, 1100.0) < 0.005);   // nothing between the sidebands
    CHECK(tone(y, 900.0) < 0.005);
}

TEST(fm_feedback_bounded) {
    const double hz[4] = {220, 0, 0, 0};
    const float amp[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    for (float fb : {0.0f, 0.125f, 0.25f, 0.4375f}) {
        const std::vector<float> y = run(7, hz, amp, fb, 44100);
        float pk = 0;
        bool fin = true;
        for (float v : y) {
            fin = fin && std::isfinite(v);
            pk = std::max(pk, std::fabs(v));
        }
        const double h2 = tone(y, 440.0), h1 = tone(y, 220.0);
        std::printf("  feedback %.3f: peak %.3f, 2nd harmonic %.4f of %.4f\n", fb, 4.0f * pk, h2, h1);
        CHECK(fin);
        CHECK(4.0f * pk <= 1.001f);   // one of four carriers: the mean is <= 1/4
        if (fb == 0.0f) CHECK(h2 < 0.001);
        else CHECK(h2 > 0.002);
    }
}
