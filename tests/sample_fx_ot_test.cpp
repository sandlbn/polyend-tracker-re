// OT fork Sample Editor effects: Transcode (G.711 / G.726) and DCT Crush.
#include "test.h"

#include <vector>

#include "app/sample_fx.h"
#include "app/sample_fx_ot.h"

using namespace ot;

namespace {

std::vector<int16_t> sine(uint32_t n, double hz, double amp = 12000) {
    std::vector<int16_t> v(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = int16_t(amp * std::sin(2 * M_PI * hz * double(i) / 44100.0));
    return v;
}

// SNR in dB of y against x, skipping a settling head and allowing a lag.
double snr(const std::vector<int16_t>& x, const std::vector<int16_t>& y, uint32_t skip, int maxLag = 16) {
    double best = -100;
    for (int lag = 0; lag <= maxLag; ++lag) {
        double s = 0, e = 0;
        for (uint32_t i = skip; i + uint32_t(lag) < x.size(); ++i) {
            const double a = x[i], b = y[i + uint32_t(lag)];
            s += a * a;
            e += (a - b) * (a - b);
        }
        const double d = 10 * std::log10(s / (e > 1 ? e : 1));
        if (d > best) best = d;
    }
    return best;
}

// A busy, less predictable signal: three tones and noise.
std::vector<int16_t> busy(uint32_t n) {
    std::vector<int16_t> v(n);
    uint32_t r = 1;
    for (uint32_t i = 0; i < n; ++i) {
        r = r * 1664525u + 1013904223u;
        const double t = double(i) / 44100.0;
        v[i] = int16_t(5000 * std::sin(2 * M_PI * 310 * t) + 3000 * std::sin(2 * M_PI * 1270 * t) +
                       2000 * std::sin(2 * M_PI * 2900 * t) + double(int32_t(r >> 20) - 2048) * 0.8);
    }
    return v;
}

// Reference: the same chain with the codec bypassed (Mix 0 is dry, so use the
// highest-quality setting as the reference instead: 40k, one generation).
std::vector<int16_t> run(int fx, const float* p, const std::vector<int16_t>& in) {
    std::vector<int16_t> out(in.size());
    sfx::process(fx, p, in.data(), uint32_t(in.size()), out.data(), uint32_t(out.size()));
    return out;
}

}  // namespace

TEST(table_entries) {
    CHECK_EQ(sfx::kEffectCount, 19);
    CHECK(std::strcmp(sfx::effect(sfx::kTranscode).name, "Transcode") == 0);
    CHECK(std::strcmp(sfx::effect(sfx::kDctCrush).name, "DCT Crush") == 0);
    float p[sfx::kMaxParams];
    sfx::defaults(sfx::kTranscode, p);
    char t[24];
    sfx::formatParam(sfx::kTranscode, 0, p[0], t, sizeof(t));
    CHECK(std::strcmp(t, "G726 32k") == 0);
}

TEST(g711_round_trip) {
    int worstU = 0, worstA = 0;
    for (int x = -32000; x <= 32000; x += 7) {
        const int du = std::abs(sfx::ulawRoundTrip(int16_t(x)) - x);
        const int da = std::abs(sfx::alawRoundTrip(int16_t(x)) - x);
        // Logarithmic: error relative to the level, plus a small floor.
        CHECK(du <= std::abs(x) / 16 + 40);
        CHECK(da <= std::abs(x) / 16 + 40);
        worstU = du > worstU ? du : worstU;
        worstA = da > worstA ? da : worstA;
    }
    CHECK(worstU > 0);   // it is lossy
    CHECK_EQ(int(sfx::ulawRoundTrip(0)), 0);
}

TEST(g726_rates_quality_order) {
    // The codec alone on a busy signal: more bits, more SNR; tandem passes lose.
    const auto x = busy(44100);
    std::vector<int16_t> y(x.size());
    double q[6];
    for (int c = 0; c < 6; ++c) {
        sfx::codecDirect(c, 1, x.data(), uint32_t(x.size()), y.data());
        q[c] = snr(x, y, 2000, 0);
    }
    std::printf("  SNR u/A/40/32/24/16k: %.1f %.1f %.1f %.1f %.1f %.1f dB\n", q[0], q[1], q[2], q[3], q[4], q[5]);
    CHECK(q[0] > 25 && q[1] > 25);   // G.711 ~ 8 bits log
    CHECK(q[2] > q[3] && q[3] > q[4] && q[4] > q[5]);
    CHECK(q[3] > 12);
    CHECK(q[5] > 1);
    sfx::codecDirect(4, 8, x.data(), uint32_t(x.size()), y.data());
    CHECK(snr(x, y, 2000, 0) < q[4]);   // 8 generations of 24k
}

TEST(transcode_errors_stay_bounded) {
    const auto x = busy(44100);
    float p[sfx::kMaxParams];
    sfx::defaults(sfx::kTranscode, p);
    p[0] = 4;   // 24k
    p[1] = 3;
    const auto clean = run(sfx::kTranscode, p, x);
    p[3] = 100;   // 1 % bit errors
    const auto err = run(sfx::kTranscode, p, x);
    double d = 0;
    for (size_t i = 0; i < x.size(); ++i) d += std::fabs(double(err[i]) - double(clean[i]));
    CHECK(d > 0);   // the errors change the result
    p[2] = 16;      // 16 generations with errors: still finite output
    const auto worst = run(sfx::kTranscode, p, x);
    CHECK(worst.size() == x.size());
}

TEST(transcode_packet_loss) {
    const auto x = sine(44100, 500);
    float p[sfx::kMaxParams];
    sfx::defaults(sfx::kTranscode, p);
    p[0] = 0;   // u-law
    p[4] = 50;
    p[5] = 0;   // silence
    const auto y = run(sfx::kTranscode, p, x);
    // About half of the 10 ms packets are silent.
    int silent = 0, packets = 0;
    for (uint32_t s = 0; s + 441 <= y.size(); s += 441, ++packets) {
        int peak = 0;
        for (uint32_t i = s + 20; i < s + 420; ++i) peak = std::abs(y[i]) > peak ? std::abs(y[i]) : peak;
        silent += peak < 200;
    }
    CHECK(silent > packets / 5);
    CHECK(silent < packets * 4 / 5);
}

TEST(dct_crush) {
    std::vector<int16_t> x(44100);
    for (uint32_t i = 0; i < x.size(); ++i)
        x[i] = int16_t(6000 * std::sin(2 * M_PI * 220 * i / 44100.0) + 3000 * std::sin(2 * M_PI * 3100 * i / 44100.0) +
                       ((i * 7919u) % 2000) - 1000);
    float p[sfx::kMaxParams];
    sfx::defaults(sfx::kDctCrush, p);
    p[1] = 100;   // keep every bin
    p[2] = 12;    // fine quantiser
    const double clean = snr(x, run(sfx::kDctCrush, p, x), 1024, 0);
    std::printf("  DCT crush keep 100 %% / 12 bits: %.1f dB\n", clean);
    CHECK(clean > 25);
    p[1] = 5;
    p[2] = 2;
    const double crushed = snr(x, run(sfx::kDctCrush, p, x), 1024, 0);
    CHECK(crushed < clean - 6);
    p[3] = 100;   // freeze every frame: the first frame's spectrum repeats
    const auto fr = run(sfx::kDctCrush, p, x);
    CHECK(fr.size() == x.size());
}
