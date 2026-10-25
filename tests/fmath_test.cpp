// No double libm on the device (docs/OPTIMIZATION_2X.md OP-4): the fmath.h
// helpers against the libm (rounding exact, transcendentals within a few ulp),
// the recorder's note-ratio table against std::pow, and the Sample Editor
// effects that moved off the libm against the former libm formulas. Those
// effects now run their per-sample loops in float (the M4 has no double FPU),
// so they match within the LSB tolerances of tests/sample_fx_speed_test.cpp
// (EQ 3, Limiter 2, Overdrive 1) instead of on every sample.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "test.h"

#include "app/sample_fx.h"
#include "fmath.h"

using namespace ot;

namespace {

const double kVals[] = {0.0,  -0.0, 0.25, 0.5,  -0.5, 0.49999999999999994, -0.49999999999999994, 1.5, -1.5,
                        2.5,  -2.5, 1e-300, -1e-300, 123456.5, -123456.5, 4503599627370495.5, 1e15 + 0.75,
                        -7.999999999999999, 65535.49999, 32767.0 - 1e-4};

std::vector<int16_t> sine(uint32_t n, double amp, double hz) {
    std::vector<int16_t> s(n);
    for (uint32_t i = 0; i < n; ++i) s[i] = int16_t(amp * std::sin(2.0 * 3.14159265358979 * hz * double(i) / 44100.0));
    return s;
}

std::vector<int16_t> runFx(int fx, const float* p, const std::vector<int16_t>& in) {
    const uint32_t m = sfx::outputFrames(fx, p, uint32_t(in.size()));
    std::vector<int16_t> out(m);
    out.resize(sfx::process(fx, p, in.data(), uint32_t(in.size()), out.data(), m));
    return out;
}

int16_t sat(double v) {
    if (v >= 32767.0) return 32767;
    if (v <= -32768.0) return -32768;
    return int16_t(int32_t(v));
}

// The former all-double formulas (2.x before OP-4).
struct RefBiquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void peaking(double f, double lin, double q) {
        const double A = std::sqrt(lin);
        const double w0 = 2.0 * 3.14159265358979323846 * f / 44117.647;
        const double sn = std::sin(w0), cs = std::cos(w0);
        const double alpha = sn / (2.0 * q);
        const double a0 = 1.0 + alpha / A;
        b0 = (1.0 + alpha * A) / a0;
        b1 = -2.0 * cs / a0;
        b2 = (1.0 - alpha * A) / a0;
        a1 = -2.0 * cs / a0;
        a2 = (1.0 - alpha / A) / a0;
    }
    double run(double x) {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

std::vector<int16_t> refEq(const float* p, const std::vector<int16_t>& in) {
    RefBiquad b[3];
    bool on[3];
    for (int k = 0; k < 3; ++k) {
        const double lin = std::pow(10.0, double(p[3 * k]) / 20.0);
        const double f = p[3 * k + 1] < 20000.0f ? double(p[3 * k + 1]) : 20000.0;
        double bw = f / (double(p[3 * k + 2]) + 3.0 * f / 20000.0);
        if (bw > 20000.0) bw = 20000.0;
        on[k] = lin != 1.0 && bw > 0.0 && f > 0.0;
        if (on[k]) b[k].peaking(f, lin, f / bw);
    }
    std::vector<int16_t> out(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        double v = double(in[i]) / 32767.0;
        for (int k = 0; k < 3; ++k)
            if (on[k]) v = b[k].run(v);
        if (v > 1.0) v = 1.0;
        if (v < -1.0) v = -1.0;
        out[i] = sat(v * 32767.0);
    }
    return out;
}

double refCoef(float ms) { return ms <= 0.0f ? 1.0 : 1.0 - std::exp(-2.2 * 1000.0 / (double(ms) * 44100.0)); }

std::vector<int16_t> refLimiter(const float* p, const std::vector<int16_t>& in) {
    const double thr = std::pow(10.0, double(p[0]) / 20.0) * 32767.0;
    const double ca = refCoef(p[1]), cr = refCoef(p[2]);
    double e2 = 0.0, g = 1.0;
    std::vector<int16_t> out(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const double e1 = std::fabs(double(in[i]));
        const double ce = e1 > e2 ? ca : cr;
        e2 = ce * e1 + (1.0 - ce) * e2;
        const double r = e2 > thr ? thr / e2 : 1.0;
        const double cg = r < g ? ca : cr;
        g = cg * r + (1.0 - cg) * g;
        out[i] = sat(double(in[i]) * g);
    }
    return out;
}

std::vector<int16_t> refOverdriveExp(double g, const std::vector<int16_t>& in) {
    std::vector<int16_t> out(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const int x = in[i];
        const double ax = x < 0 ? -double(x) : double(x);
        const double y = (1.0 - std::exp(-4.0 * g * ax / 32767.0)) * 32767.0;
        out[i] = sat(x < 0 ? -y : y);
    }
    return out;
}

// Max |a - b| and the number of differing samples.
void diff(const std::vector<int16_t>& a, const std::vector<int16_t>& b, int& maxd, size_t& count) {
    maxd = 0;
    count = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        const int d = std::abs(int(a[i]) - int(b[i]));
        if (d) ++count;
        if (d > maxd) maxd = d;
    }
}

}  // namespace

TEST(fmath_rounding_matches_libm) {
    for (double v : kVals) {
        CHECK(fm::floorD(v) == std::floor(v));
        CHECK(fm::ceilD(v) == std::ceil(v));
        CHECK_EQ(fm::lroundD(v), std::lround(v));
    }
    // Dense sweep around halves and integers.
    for (int i = -4000; i <= 4000; ++i) {
        for (double e : {-1e-12, 0.0, 1e-12}) {
            const double v = double(i) * 0.25 + e;
            CHECK(fm::floorD(v) == std::floor(v));
            CHECK(fm::ceilD(v) == std::ceil(v));
            CHECK_EQ(fm::lroundD(v), std::lround(v));
        }
    }
}

// |a - b| in units of the last place of b.
double ulps(double a, double b) {
    if (a == b) return 0.0;
    const double u = std::nextafter(std::fabs(b), INFINITY) - std::fabs(b);
    return std::fabs(a - b) / u;
}

TEST(fmath_transcendentals_within_4_ulp) {
    double worst[5] = {};
    for (int i = -20000; i <= 20000; ++i) {
        const double x = double(i) * 0.000371 + 1e-9 * double(i % 7);
        const double e = ulps(fm::expD(x * 7.0), std::exp(x * 7.0));
        const double p = ulps(fm::pow10D(x * 0.4), std::pow(10.0, x * 0.4));   // +-3 (+-60 dB)
        const double sn = std::fabs(fm::sinD(x) - std::sin(x)) / 2.220446049250313e-16;   // absolute, near 0
        const double cs = std::fabs(fm::cosD(x) - std::cos(x)) / 2.220446049250313e-16;
        const double q = ulps(fm::sqrtD(std::fabs(x) * 3.3), std::sqrt(std::fabs(x) * 3.3));
        const double v[5] = {e, p, sn, cs, q};
        for (int k = 0; k < 5; ++k) worst[k] = v[k] > worst[k] ? v[k] : worst[k];
    }
    CHECK(worst[0] <= 4.0);
    CHECK(worst[1] <= 4.0);
    CHECK(worst[2] <= 4.0);
    CHECK(worst[3] <= 4.0);
    CHECK(worst[4] <= 1.0);
    CHECK(fm::sqrtD(0.0) == 0.0);
    CHECK(fm::pow10D(0.0) == 1.0);   // EQ bypass test (lin != 1.0)
}

TEST(fmath_note_ratio_table_is_pow) {
    for (int n = 0; n < 128; ++n) CHECK(ulps(fm::kNoteRatio[n], std::pow(2.0, double(n - 60) / 12.0)) <= 1.0);
}

TEST(sample_fx_within_tolerance_of_the_double_libm_results) {
    const std::vector<int16_t> in = sine(44100, 30000.0, 220.0);
    const std::vector<int16_t> mix = [] {
        std::vector<int16_t> a = sine(44100, 12000.0, 97.0), b = sine(44100, 12000.0, 3100.0);
        for (size_t i = 0; i < a.size(); ++i) a[i] = int16_t(a[i] + b[i]);
        return a;
    }();
    int maxd;
    size_t count;
    {
        float p[9];
        sfx::defaults(sfx::kEqualizer, p);
        p[0] = 6.0f;
        p[3] = -4.5f;
        p[6] = 3.0f;
        p[4] = 1200.0f;
        diff(runFx(sfx::kEqualizer, p, mix), refEq(p, mix), maxd, count);
        CHECK(maxd <= 3);
    }
    {
        float p[9];
        sfx::defaults(sfx::kLimiter, p);
        diff(runFx(sfx::kLimiter, p, in), refLimiter(p, in), maxd, count);
        CHECK(maxd <= 2);
        p[0] = -20.0f;
        p[1] = 0.3f;
        p[2] = 2000.0f;
        diff(runFx(sfx::kLimiter, p, in), refLimiter(p, in), maxd, count);
        CHECK(maxd <= 2);
    }
    for (float g : {0.01f, 1.0f, 2.37f, 5.0f}) {
        float p[9];
        sfx::defaults(sfx::kOverdrive, p);
        p[0] = g;
        p[1] = 1.0f;
        diff(runFx(sfx::kOverdrive, p, in), refOverdriveExp(double(g), in), maxd, count);
        CHECK(maxd <= 1);
    }
}
