// [3.x] The shared virtual-analog DSP header audio/va_dsp.h (docs/PLAN_CLASSIC.md
// §2, used by A-Type and CS-Type): the helpers, the one-frame-late polyBLEP /
// polyBLAMP oscillator frame and its hard-sync step residual, the parabolic
// sine, the TPT state-variable filter (cutoff, 12 dB slope, the level-
// dependent damping: bounded at zero damping, singing at a negative one), the 4-pole ladder step (the clipped variant equal to D-Type's
// kernel sample for sample) and the envelopes (ADSR timing, the IL / AL
// contour).
#include "test.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "audio/va_dsp.h"

using namespace ot::audio;
namespace va = ot::audio::va;

namespace {

constexpr double kFs = ot::audio::kSampleRate;
constexpr double kPi = 3.14159265358979;
using Buf = std::vector<float>;

// Goertzel magnitude (Hann window) at hz.
double tone(const Buf& b, double hz) {
    const double w = 2.0 * kPi * hz / kFs, c = 2.0 * std::cos(w);
    const double wn = 2.0 * kPi / double(b.size());
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < b.size(); ++i) {
        const double s0 = b[i] * (0.5 - 0.5 * std::cos(wn * double(i))) + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / double(b.size());
}
double db(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }
// Power below maxHz off the harmonics of f0 over the harmonic power (DFT, Hann).
double aliasRatio(const Buf& x, double f0, double maxHz) {
    const size_t n = x.size();
    const double binHz = kFs / double(n);
    double harm = 0, alias = 0;
    for (size_t k = 1; k <= n / 2 && double(k) * binHz < maxHz; ++k) {
        double re = 0, im = 0;
        const double dw = 2.0 * kPi * double(k) / double(n);
        for (size_t i = 0; i < n; ++i) {
            const double w = x[i] * (0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(n)));
            re += w * std::cos(dw * double(i));
            im -= w * std::sin(dw * double(i));
        }
        const double pw = re * re + im * im, f = double(k) * binHz, h = std::round(f / f0);
        if (h >= 1 && std::fabs(f - h * f0) <= 4 * binHz) harm += pw;
        else alias += pw;
    }
    return alias / harm;
}
// The SVF low-pass / high-pass response at hz (sine in, steady-state rms ratio).
double svfGain(float cutoffHz, float R, double hz, bool high, double amp = 1.0) {
    va::Svf f{};
    const float g = va::svfG(cutoffHz, 0.0f);
    const va::SvfCoef c = va::svfCoef(g, R);
    double in = 0, out = 0;
    const int n = int(kFs);
    for (int i = 0; i < n; ++i) {
        const float x = float(amp * std::sin(2.0 * kPi * hz * double(i) / kFs));
        float lp, bp, hp;
        va::svfStep(f, c, x, lp, bp, hp);
        if (i > n / 2) {
            in += double(x) * x;
            out += double(high ? hp : lp) * (high ? hp : lp);
        }
    }
    return std::sqrt(out / in);
}

}  // namespace

TEST(va_helpers) {
    CHECK_EQ(va::clampf(3.0f, 0.0f, 1.0f), 1.0f);
    CHECK_EQ(va::enumOf(2.4f, 3), 2);
    CHECK_EQ(va::enumOf(7.0f, 3), 3);
    CHECK_EQ(va::enumOf(-1.0f, 3), 0);
    for (float x : {-7.3f, -1.0f, 0.0f, 0.37f, 4.5f, 11.9f}) CHECK(std::fabs(va::fastExp2(x) / std::exp2(x) - 1.0) < 2e-5);
    CHECK_EQ(va::blockCoef(0.0f), 0.0f);
    for (float x : {0.1f, 0.7f, 1.3f}) CHECK(std::fabs(va::tanPade(x) / std::tan(x) - 1.0) < 1e-3);
    CHECK_EQ(va::tanhR(0.0f), 0.0f);
    CHECK(std::fabs(va::tanhR(3.0f) - 1.0f) < 1e-6);
    CHECK_EQ(va::tanhR(10.0f), va::tanhR(3.0f));
    CHECK_EQ(va::flush(1e-25f), 0.0f);
    CHECK_EQ(va::flush(1e-3f), 1e-3f);
    // A440 increment.
    CHECK(std::fabs(va::incOf(69.0f, 0.0f, 0.0f, 0.45f) - 440.0f / float(kFs)) < 1e-7);
    CHECK(std::fabs(va::incOf(81.0f, 0.0f, 0.0f, 0.45f) - 880.0f / float(kFs)) < 2e-7);
    CHECK_EQ(va::incOf(200.0f, 0.0f, 0.0f, 0.45f), 0.45f);
    // Parabolic sine: within 0.2 % of full scale, the right sign and phase.
    double err = 0;
    for (int i = 0; i < 1000; ++i) {
        const float p = float(i) / 1000.0f;
        err = std::max(err, std::fabs(double(va::sinePar(p)) - std::sin(2.0 * kPi * p)));
    }
    CHECK(err < 2e-3);
}

TEST(va_polyblep_saw_and_pulse) {
    // Note 96 (2093 Hz): the band-limited saw aliases far less than a naive one.
    const float d = va::incOf(96.0f, 0.0f, 0.0f, va::kMaxDt);
    Buf a, b;
    float p = 0.13f, z = 0.0f, q = 0.13f;
    bool hi = va::vcoHi<va::kShSaw>(p, 0.5f);
    for (int i = 0; i < 8192; ++i) {
        a.push_back(va::vcoFrame<va::kShSaw>(p, z, hi, d, 0.5f));
        q += d;
        if (q >= 1.0f) q -= 1.0f;
        b.push_back(q + q - 1.0f);
    }
    const double ra = aliasRatio(a, 2093.0, 18000.0), rb = aliasRatio(b, 2093.0, 18000.0);
    std::printf("  saw alias: polyBLEP %.2e, naive %.2e\n", ra, rb);
    CHECK(ra < 0.25 * rb);
    // A pulse at 10 % keeps its mean at 0 (DC removed) and both edges corrected.
    Buf c;
    p = 0.0f;
    z = 0.0f;
    hi = va::vcoHi<va::kShPulse>(p, 0.1f);
    double mean = 0;
    for (int i = 0; i < 44100; ++i) {
        c.push_back(va::vcoFrame<va::kShPulse>(p, z, hi, 0.01f, 0.1f));
        mean += c.back();
    }
    CHECK(std::fabs(mean / 44100.0) < 0.01);
    // A phase step past both the duty and the wrap in one frame still sees both edges.
    p = 0.95f;
    z = 0.0f;
    hi = false;
    const float out0 = va::vcoFrame<va::kShPulse>(p, z, hi, 0.4f, 0.2f);   // wraps and passes 0.2 (p = 0.35)
    (void)out0;
    CHECK(!hi);
    CHECK(std::fabs(z) < 2.0f);
}

TEST(va_saw_and_pulse_off_one_phase) {
    // vcoSawPulse = gs x saw + gp x a pulse high for the last 30 % of the
    // cycle (vcoFrame's 30 % pulse 0.3 of a cycle on), band-limited like
    // vcoFrame (the same samples), also at a high increment past both edges.
    for (float d : {0.0131f, 0.21f, 0.44f}) {
        float p = 0.6f, z = 0.0f, ps = 0.6f, zs = 0.0f, pp = 0.9f, zp = 0.0f;
        bool high = p >= 0.7f;
        bool hs = va::vcoHi<va::kShSaw>(ps, 0.5f), hp = va::vcoHi<va::kShPulse>(pp, 0.3f);
        double worst = 0;
        for (int i = 0; i < 4000; ++i) {
            const float both = va::vcoSawPulse(p, z, high, d, 0.3f, 0.7f, 0.4f);
            const float ref = 0.7f * va::vcoFrame<va::kShSaw>(ps, zs, hs, d, 0.5f) + 0.4f * va::vcoFrame<va::kShPulse>(pp, zp, hp, d, 0.3f);
            worst = std::max(worst, double(std::fabs(both - ref)));
        }
        CHECK(worst < 2e-4);   // the two phases round apart (their edge times / d)
    }
}

TEST(va_saw_pulse_width_moving_across_the_phase) {
    // A width swept fast and deep: the held output never jumps by a full
    // unresidualised step (the naive pulse's 2 x gp) except at its edges'
    // band-limited transitions: the largest first difference stays below
    // what a missed edge gives (2 gp = 1).
    float p = 0.1f, z = 0.0f;
    bool high = p >= 0.5f;
    const float d = va::incOf(60.0f, 0.0f, 0.0f, va::kMaxDt);
    float last = 0.0f, worst = 0.0f;
    for (int i = 0; i < 44100; ++i) {
        const float w = 0.05f + 0.45f * (0.5f + 0.5f * float(std::sin(2.0 * kPi * 20.0 * double(i) / kFs)));
        const float y = va::vcoSawPulse(p, z, high, d, w, 0.0f, 0.5f);
        if (i > 2) worst = std::max(worst, std::fabs(y - last));
        last = y;
    }
    std::printf("  saw+pulse, width at 20 Hz full depth: largest step %.3f\n", worst);
    CHECK(worst < 0.8f);
}

TEST(va_step_residual_hard_sync) {
    // A hard-sync reset (a step of h at x frames past the event) with the
    // residual is smoother than the bare step: the first difference shrinks.
    float out = 0.0f, cur = 0.0f;
    va::stepResidual(-1.6f, 0.3f, out, cur);
    CHECK(std::fabs(out - (-1.6f * 0.3f * 0.3f * 0.5f)) < 1e-6);
    CHECK(std::fabs(cur - (1.6f * 0.7f * 0.7f * 0.5f)) < 1e-6);
    // A saw and a pulse at note 84 synced to a master at note 60: the
    // band-limited resets alias less than the naive ones; without a reset
    // vcoSync is the plain oscillator.
    const float dm = va::incOf(60.0f, 0.0f, 0.0f, va::kMaxDt), ds = va::incOf(84.3f, 0.0f, 0.0f, va::kMaxDt);
    for (int shape : {int(va::kShSaw), int(va::kShPulse)}) {
        Buf a, b;
        float pm = 0.0f, zm = 0.0f, ps = 0.0f, zs = 0.0f, qm = 0.0f, qs = 0.0f;
        bool hm = false;
        for (int i = 0; i < 8192; ++i) {
            const float before = pm;
            va::vcoFrame<va::kShSaw>(pm, zm, hm, dm, 0.5f);
            const float x = pm < before ? pm / dm : -1.0f;   // the master's wrap: frames since it
            a.push_back(shape == va::kShSaw ? va::vcoSync<va::kShSaw>(ps, zs, ds, 0.3f, x)
                                            : va::vcoSync<va::kShPulse>(ps, zs, ds, 0.3f, x));
            qm += dm;
            qs += ds;
            if (qs >= 1.0f) qs -= 1.0f;
            if (qm >= 1.0f) {
                qm -= 1.0f;
                qs = qm / dm * ds;
            }
            b.push_back(shape == va::kShSaw ? qs + qs - 1.0f : (qs < 0.3f ? 1.0f : -1.0f) + 0.4f);
        }
        const Buf a1(a.begin() + 1, a.end()), b1(b.begin(), b.end() - 1);   // vcoSync is one frame late
        const double ra = aliasRatio(a1, 261.63, 18000.0), rb = aliasRatio(b1, 261.63, 18000.0);
        std::printf("  synced %s alias: band-limited %.2e, naive %.2e\n", shape == va::kShSaw ? "saw" : "pulse", ra, rb);
        CHECK(ra < 0.5 * rb);
    }
    float p1 = 0.2f, z1 = 0.0f, p2 = 0.2f, z2 = 0.0f;
    bool h1 = va::vcoHi<va::kShPulse>(p1, 0.3f);
    int same = 0;
    for (int i = 0; i < 2000; ++i) {
        const float u = va::vcoFrame<va::kShPulse>(p1, z1, h1, 0.013f, 0.3f), v = va::vcoSync<va::kShPulse>(p2, z2, 0.013f, 0.3f, -1.0f);
        same += std::fabs(u - v) < 1e-5f;
    }
    CHECK_EQ(same, 2000);
}

TEST(va_svf_cutoff_slope_and_bounds) {
    // -3 dB at the cutoff (R = 1/sqrt 2), 12 dB per octave above for the
    // low-pass and below for the high-pass.
    CHECK(std::fabs(db(svfGain(1000.0f, 0.70710678f, 1000.0, false)) + 3.01) < 0.3);
    CHECK(std::fabs(db(svfGain(1000.0f, 0.70710678f, 1000.0, true)) + 3.01) < 0.3);
    const double s = db(svfGain(500.0f, 0.70710678f, 4000.0, false)) - db(svfGain(500.0f, 0.70710678f, 8000.0, false));
    std::printf("  SVF low-pass slope 4k -> 8k at 500 Hz: %.1f dB\n", s);
    CHECK(s > 11.0 && s < 14.5);
    const double h = db(svfGain(4000.0f, 0.70710678f, 500.0, true)) - db(svfGain(4000.0f, 0.70710678f, 250.0, true));
    CHECK(h > 11.0 && h < 12.8);
    // Low damping: a peak at the cutoff (1 / (2 x damping)).
    CHECK(db(svfGain(1000.0f, 0.1f, 1000.0, false, 0.1)) > 13.0);
    // Zero damping with a kick, the level-dependent damping: it rings at the
    // cutoff and dies down, bounded.
    auto ringLevel = [](float k, float kick, float cutoff, int n) {
        va::Svf f{};
        const float g = va::svfG(cutoff, 0.0f);
        Buf y;
        const float kmin = va::svfKMin(g);
        float h = va::svfH(g, k);
        for (int i = 0; i < n; ++i) {
            const float d = va::svfLevelK(k, kmin, f);
            h = va::svfHTrack(h, g, d);
            float lp, bp, hp;
            va::svfStep(f, va::SvfCoef{g, d, h}, i < 4 ? kick : 0.0f, lp, bp, hp);
            y.push_back(lp);
        }
        return y;
    };
    const Buf y = ringLevel(0.0f, 4.0f, 800.0f, 44100);
    float pk = 0.0f;
    for (float v : y) {
        pk = std::max(pk, std::fabs(v));
        CHECK(std::isfinite(v));
    }
    CHECK(pk < 6.0f);
    const Buf tail(y.end() - 8192, y.end());
    CHECK(tone(tail, 800.0) > 4.0 * tone(tail, 1200.0));
    // Negative damping: a tiny seed grows into a steady, bounded tone at the
    // cutoff, about sqrt(-k / kSvfAgc) loud, at low and high cutoffs.
    for (float fc : {200.0f, 800.0f, 6000.0f, 19000.0f}) {
        const Buf w = ringLevel(-0.2f, 1e-3f, fc, 2 * 44100);
        const Buf r(w.end() - 8192, w.end());
        float rp = 0.0f;
        bool fin = true;
        for (float v : w) rp = std::max(rp, std::fabs(v)), fin &= std::isfinite(v);
        float tp = 0.0f;
        for (float v : r) tp = std::max(tp, std::fabs(v));
        std::printf("  SVF self-oscillation at %.0f Hz: peak %.2f (all %.2f)\n", fc, tp, rp);
        CHECK(fin);
        CHECK(tp > (fc < 10000.0f ? 0.5f : 0.1f) && tp < 2.0f);
        CHECK(rp < 3.0f);
        if (fc < 10000.0f) CHECK(tone(r, fc) > 4.0 * tone(r, 1.5 * fc));
    }
    // Loud input at a high cutoff with zero damping: stable (no clip in the loop).
    va::Svf hs{};
    const float gh = va::svfG(18000.0f, 0.0f);
    bool fin = true;
    for (int i = 0; i < 44100; ++i) {
        float lp, bp, hp;
        const float d = va::svfLevelK(0.0f, va::svfKMin(gh), hs);
        va::svfStep(hs, va::SvfCoef{gh, d, va::svfH(gh, d)}, (i & 1) ? 2.0f : -2.0f, lp, bp, hp);
        fin &= std::isfinite(lp) && std::fabs(lp) < 100.0f;
    }
    CHECK(fin);
}

TEST(va_svf_level_damping_with_input_at_the_cutoff) {
    // A sine at the cutoff (0.6 through the rational tanh) into the SVF with
    // the per-sample level damping (svfLevelK, h by svfHTrack): the level is
    // steady (a damping set once per block from the level feeds back and
    // jumps from block to block).
    for (float fc : {2000.0f, 6000.0f})
        for (float kw : {0.02f, -0.2f}) {
            va::Svf f{};
            const float g = va::svfG(fc, 0.0f), kmin = va::svfKMin(g);
            float h = va::svfH(g, kw);
            const double per = kFs / fc;
            std::vector<double> env;
            double acc = 0;
            int n = 0;
            for (int i = 0; i < 3 * 44100; ++i) {
                const float x = va::tanhR(0.6f * float(std::sin(2.0 * kPi * fc * double(i) / kFs)));
                const float k = va::svfLevelK(kw, kmin, f);
                h = va::svfHTrack(h, g, k);
                float lp, bp, hp;
                va::svfStep(f, va::SvfCoef{g, k, h}, x, lp, bp, hp);
                acc += double(lp) * lp;
                if (++n >= int(2.0 * per)) {
                    env.push_back(std::sqrt(acc / n));
                    acc = 0;
                    n = 0;
                }
            }
            double lo = 1e9, hi = 0;
            for (size_t i = env.size() / 2; i < env.size(); ++i) lo = std::min(lo, env[i]), hi = std::max(hi, env[i]);
            std::printf("  level damping, sine at %.0f Hz, k %.2f: envelope %.3f .. %.3f\n", fc, kw, lo, hi);
            CHECK(std::isfinite(hi));
            CHECK(hi < 1.1 * lo + 1e-6);
            CHECK(hi < 3.0);
        }
    // Without input, k < 0 still sings, bounded, at low and high cutoffs.
    for (float fc : {200.0f, 800.0f, 6000.0f}) {
        va::Svf f{};
        f.s1 = 1e-4f;
        const float g = va::svfG(fc, 0.0f), kmin = va::svfKMin(g);
        float h = va::svfH(g, -0.2f), pk = 0.0f;
        for (int i = 0; i < 2 * 44100; ++i) {
            const float k = va::svfLevelK(-0.2f, kmin, f);
            h = va::svfHTrack(h, g, k);
            float lp, bp, hp;
            va::svfStep(f, va::SvfCoef{g, k, h}, 0.0f, lp, bp, hp);
            if (i > 44100) pk = std::max(pk, std::fabs(lp));
        }
        std::printf("  level damping, no input, %.0f Hz: sings at %.2f\n", fc, pk);
        CHECK(pk > 0.3f && pk < 2.0f);
    }
}

TEST(va_svf_h_tracks_a_ramp) {
    // g from 20 Hz to 0.45 fs and k from 2 to -0.2 within one block: the
    // Newton-tracked h stays on 1 / (1 + g (g + k)).
    const float g0 = va::svfG(20.0f, 0.0f), g1 = va::svfG(19800.0f, 0.0f);
    float h = va::svfH(g0, 2.0f), worst = 0.0f;
    for (int i = 1; i <= 128; ++i) {
        const float g = g0 + (g1 - g0) * float(i) / 128.0f, k = 2.0f - 2.2f * float(i) / 128.0f;
        h = va::svfHNext(h, g, k);
        worst = std::max(worst, std::fabs(h / va::svfH(g, k) - 1.0f));
    }
    std::printf("  svfHNext: worst relative error %.2e\n", worst);
    CHECK(worst < 2e-2f);   // the whole range in one block: within 2 %
}

TEST(va_ladder_matches_the_dtype_kernel) {
    // The clipped variant reproduces D-Type's ladder sample (its kernel's
    // operations in order), the unclipped one is the plain ZDF ladder.
    auto ref = [](float x, float s[4], float G, float k, float r) {
        auto clip3 = [](float v) { return v - v * v * v * (1.0f / 27.0f); };
        const float sig = (1.0f - G) * (((s[0] * G + s[1]) * G + s[2]) * G + s[3]);
        const float u = va::tanhR((x - k * sig) * r);
        float v = G * (clip3(u) - s[0]);
        const float y0 = v + s[0];
        s[0] = y0 + v;
        v = G * (clip3(y0) - s[1]);
        const float y1 = v + s[1];
        s[1] = y1 + v;
        v = G * (clip3(y1) - s[2]);
        const float y2 = v + s[2];
        s[2] = y2 + v;
        v = G * (clip3(y2) - s[3]);
        const float y = v + s[3];
        s[3] = y + v;
        return y;
    };
    float a[4] = {}, b[4] = {};
    const float G = va::stageGain(1200.0f, 0.0f), k = 4.0f, r = 1.0f / (1.0f + k * G * G * G * G);
    uint32_t n = 7;
    int same = 0;
    for (int i = 0; i < 20000; ++i) {
        const float x = 0.8f * va::lcg(n);
        same += ref(x, a, G, k, r) == va::ladderStep<true>(x, b, G, k, r);
    }
    CHECK_EQ(same, 20000);
    // Unclipped, k = 0: the 24 dB slope (one octave: ~24 dB far above the cutoff).
    auto gainAt = [](double hz) {
        float s[4] = {};
        const float G = va::stageGain(400.0f, 0.0f);
        double in = 0, out = 0;
        for (int i = 0; i < 44100; ++i) {
            const float x = 0.1f * float(std::sin(2.0 * kPi * hz * double(i) / kFs));
            const float y = va::ladderStep<false>(x, s, G, 0.0f, 1.0f);
            if (i > 22050) {
                in += double(x) * x;
                out += double(y) * y;
            }
        }
        return db(std::sqrt(out / in));
    };
    const double slope = gainAt(3200.0) - gainAt(6400.0);
    std::printf("  ladder slope 3.2k -> 6.4k at 400 Hz: %.1f dB\n", slope);
    CHECK(slope > 21.0 && slope < 28.0);
    CHECK(std::fabs(gainAt(50.0)) < 0.5);
}

TEST(va_envelopes) {
    constexpr float kBlockT = float(kBlock) / float(kSampleRate);
    // ADSR: the attack reaches 1 after Attack seconds (+- one block), the
    // decay reaches Sustain, the release ends below 1e-4.
    va::Contour c;
    va::setContour(c, 0.2f, 0.3f, 0.5f, 0.4f);
    uint8_t st = va::kAtt;
    float e = 0.0f;
    int blocks = 0;
    while (st == va::kAtt && blocks < 10000) {
        e = va::step(st, e, c);
        ++blocks;
    }
    CHECK(std::fabs(float(blocks) * kBlockT - 0.2f) < 2.0f * kBlockT);
    for (int i = 0; i < int(1.5f / kBlockT); ++i) e = va::step(st, e, c);
    CHECK_EQ(e, 0.5f);
    st = va::kRel;
    blocks = 0;
    while (st != va::kIdle && blocks < 10000) {
        e = va::step(st, e, c);
        ++blocks;
    }
    CHECK_EQ(e, 0.0f);
    CHECK(float(blocks) * kBlockT < 1.2f);
    // Attack 0: full at once.
    va::setContour(c, 0.0f, 0.3f, 0.5f, 0.4f);
    st = va::kAtt;
    CHECK_EQ(va::step(st, 0.0f, c), 1.0f);
    // IL / AL: from the initial level to the attack level, back to the
    // initial level in the decay, held, to 0 in the release.
    va::IlAl f;
    va::setIlAl(f, 0.05f, 0.1f, 0.2f);
    const float il = -0.3f, al = 0.8f;
    float v = il;
    st = va::kAtt;
    float top = -1.0f;
    for (int i = 0; i < int(0.05f / kBlockT) + 2; ++i) {
        v = va::stepIlAl(st, v, f, il, al);
        top = std::max(top, v);
    }
    CHECK(std::fabs(top - al) < 0.02f);
    for (int i = 0; i < int(1.0f / kBlockT); ++i) v = va::stepIlAl(st, v, f, il, al);
    CHECK(std::fabs(v - il) < 1e-3f);
    st = va::kRel;
    for (int i = 0; i < int(2.0f / kBlockT); ++i) v = va::stepIlAl(st, v, f, il, al);
    CHECK_EQ(v, 0.0f);
}
