// The shared BBD chorus (audio/bbd_chorus.h): I+II runs I's and II's tap
// pairs at once, silence stays silence, and a switch into or out of I+II
// crossfades (no step in the output).
#include "test.h"

#include <cmath>
#include <vector>

#include "audio/bbd_chorus.h"

using namespace ot;
using namespace ot::audio;

namespace {

struct Run {
    bbd::State s = {};
    int16_t line[bbd::kLine] = {};
    std::vector<float> l, r;
    double t = 0.0;
    // One block of a 220 Hz sine (0.5) through mode `mode`, appended to l / r.
    void block(uint8_t mode, bool silent = false) {
        float bl[kBlock], br[kBlock];
        for (uint32_t i = 0; i < kBlock; ++i, t += 1.0 / kSampleRate)
            bl[i] = br[i] = silent ? 0.0f : 0.5f * float(std::sin(6.283185307 * 220.0 * t));
        bbd::process(s, line, mode, 1.0f, 1.0f, 50, bl, br, kBlock);
        l.insert(l.end(), bl, bl + kBlock);
        r.insert(r.end(), br, br + kBlock);
    }
};

// Largest sample-to-sample step of x in [from, to).
float maxStep(const std::vector<float>& x, size_t from, size_t to) {
    float m = 0.0f;
    for (size_t i = from + 1; i < to; ++i) m = std::fmax(m, std::fabs(x[i] - x[i - 1]));
    return m;
}

}  // namespace

TEST(bbd_chorus_both_modes_differ_from_each_mode) {
    Run a, b, c;
    for (int k = 0; k < 400; ++k) {
        a.block(1);
        b.block(2);
        c.block(bbd::kModeBoth);
    }
    double dab = 0, dac = 0, dbc = 0;
    bool finite = true;
    for (size_t i = 0; i < c.l.size(); ++i) {
        finite = finite && std::isfinite(c.l[i]) && std::isfinite(c.r[i]);
        dac += std::fabs(a.l[i] - c.l[i]);
        dbc += std::fabs(b.l[i] - c.l[i]);
        dab += std::fabs(a.l[i] - b.l[i]);
    }
    CHECK(finite);
    CHECK(dac > 0.05 * dab);
    CHECK(dbc > 0.05 * dab);
}

TEST(bbd_chorus_both_modes_silence_stays_silence) {
    Run c;
    for (int k = 0; k < 50; ++k) c.block(bbd::kModeBoth);
    const uint32_t blocks = (bbd::kTailFrames + kBlock - 1) / kBlock + 1;
    for (uint32_t k = 0; k < blocks; ++k) c.block(bbd::kModeBoth, true);
    const size_t from = c.l.size();
    for (int k = 0; k < 20; ++k) c.block(bbd::kModeBoth, true);
    bool zero = true;
    for (size_t i = from; i < c.l.size(); ++i) zero = zero && c.l[i] == 0.0f && c.r[i] == 0.0f;
    CHECK(zero);
}

TEST(bbd_chorus_switch_into_and_out_of_both_crossfades) {
    // I -> I+II -> II -> I+II -> I on a sustained tone: no switch block steps
    // more than the steady signal does (a 220 Hz sine at 0.5 moves at most
    // ~0.016 per sample; the wet adds a little).
    Run c;
    const uint8_t seq[] = {1, bbd::kModeBoth, 2, bbd::kModeBoth, 1};
    float steady = 0.0f, worst = 0.0f;
    for (uint8_t m : seq) {
        const size_t at = c.l.size();
        for (int k = 0; k < 60; ++k) c.block(m);
        steady = std::fmax(steady, std::fmax(maxStep(c.l, at + 20 * kBlock, c.l.size()),
                                             maxStep(c.r, at + 20 * kBlock, c.r.size())));
        if (at) worst = std::fmax(worst, std::fmax(maxStep(c.l, at - 1, at + 2 * kBlock),
                                                   maxStep(c.r, at - 1, at + 2 * kBlock)));
    }
    std::printf("bbd switch: steady step %.4f, worst step at a switch %.4f\n", double(steady), double(worst));
    CHECK(steady > 0.0f);
    CHECK(worst < 1.15f * steady);
}
