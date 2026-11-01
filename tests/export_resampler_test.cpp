// ExportResampler tests: 32-tap least-squares fractional-delay conversion of the WAV export
// from the device rate (44100 * 2500 / 2499 Hz) to 44100 Hz (spec 04 §6.1).
// The reference math (least-squares fractional-delay design, SNR and flutter
// measurements) follows the author's offline design notes; the double precision
// design here is independent of the generated table in the firmware.
#include "test.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "audio/export_resampler.h"
#include "formats/wav.h"

using namespace ot;
using namespace ot::audio;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDev = 44100.0 * 2500.0 / 2499.0;   // device rate, Hz
constexpr double kOutRate = 44100.0;
constexpr int kTaps = ExportResampler::kTaps;        // 32, taps x[n-15 .. n+16]
constexpr int kCentre = kTaps / 2 - 1;               // 15
constexpr uint32_t kPhases = ExportResampler::kOutputPerInput;   // 2499

// --- deterministic noise (no rand()) ----------------------------------------
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int range(int lo, int hi) { return lo + int(next() % uint32_t(hi - lo + 1)); }   // inclusive
};

// --- double precision design (lab: lsFdExact, band 0.82, eps 0, 32 taps) -----
double cosInt(double a, double b, double d) {   // integral of cos(w d) over [a, b]
    return std::fabs(d) < 1e-12 ? b - a : (std::sin(b * d) - std::sin(a * d)) / d;
}

// Normal equations Q w = p with Q_kl = int_0^wp cos(w (k - l)) dw (+ 1e-13 on
// the diagonal) and p_k = int_0^wp cos(w (k - D)) dw, D = 15 + mu. Q does not
// depend on mu, so its Cholesky factor is computed once; the arithmetic per
// mu is the lab's.
struct LsDesign {
    static constexpr double kBand = 0.82;
    double l[kTaps * kTaps] = {};

    LsDesign() {
        const double wp = kBand * kPi;
        for (int k = 0; k < kTaps; ++k)
            for (int c = 0; c < kTaps; ++c) l[k * kTaps + c] = cosInt(0.0, wp, k - c) + (k == c ? 1e-13 : 0.0);
        for (int j = 0; j < kTaps; ++j) {
            double sum = l[j * kTaps + j];
            for (int k = 0; k < j; ++k) sum -= l[j * kTaps + k] * l[j * kTaps + k];
            l[j * kTaps + j] = std::sqrt(sum);
            for (int i = j + 1; i < kTaps; ++i) {
                double t = l[i * kTaps + j];
                for (int k = 0; k < j; ++k) t -= l[i * kTaps + k] * l[j * kTaps + k];
                l[i * kTaps + j] = t / l[j * kTaps + j];
            }
        }
    }

    void weights(double mu, double* w) const {
        const double wp = kBand * kPi, d = kCentre + mu;
        double x[kTaps];
        for (int k = 0; k < kTaps; ++k) x[k] = cosInt(0.0, wp, k - d);
        for (int i = 0; i < kTaps; ++i) {
            double t = x[i];
            for (int k = 0; k < i; ++k) t -= l[i * kTaps + k] * x[k];
            x[i] = t / l[i * kTaps + i];
        }
        for (int i = kTaps - 1; i >= 0; --i) {
            double t = x[i];
            for (int k = i + 1; k < kTaps; ++k) t -= l[k * kTaps + i] * x[k];
            x[i] = t / l[i * kTaps + i];
        }
        for (int k = 0; k < kTaps; ++k) w[k] = x[k];
    }
};

const LsDesign& design() {
    static const LsDesign d;
    return d;
}

// --- naive reference: one output frame at a time -----------------------------
int32_t roundSum(float acc) { return int32_t(acc + (acc >= 0.0f ? 0.5f : -0.5f)); }   // half away from zero
int16_t clamp16(int32_t v) { return int16_t(std::min<int32_t>(32767, std::max<int32_t>(-32768, v))); }
uint32_t expectedFrames(uint64_t inFrames) { return uint32_t(inFrames * 2499 / 2500); }

// in = interleaved stereo. out = clamped int16; raw (optional) = the rounded
// sums before the clamp.
void naiveConvert(const std::vector<int16_t>& in, std::vector<int16_t>& out, std::vector<int32_t>* raw = nullptr) {
    const int64_t frames = int64_t(in.size() / 2);
    const uint32_t outN = expectedFrames(uint64_t(frames));
    out.clear();
    if (raw) raw->clear();
    for (uint32_t m = 0; m < outN; ++m) {
        const int64_t n = int64_t(m) + int64_t(m / kPhases);
        float w[kTaps];
        exportSincWeights(m % kPhases, w);
        for (int ch = 0; ch < 2; ++ch) {
            float acc = 0.0f;
            for (int k = 0; k < kTaps; ++k) {
                const int64_t i = n - kCentre + k;
                const float x = (i < 0 || i >= frames) ? 0.0f : float(in[size_t(2 * i + ch)]);
                acc += w[k] * x;
            }
            const int32_t r = roundSum(acc);
            out.push_back(clamp16(r));
            if (raw) raw->push_back(r);
        }
    }
}

// --- the class under test, driven through its public API --------------------
struct Collect {
    std::vector<std::vector<int16_t>> out;   // per stem, interleaved stereo
    int calls = 0;
    bool badStem = false;
    int failAtCall = -1;                     // sink returns false from this call on
};

bool collectSink(void* ctx, int stem, const int16_t* p, uint32_t frames) {
    Collect* c = static_cast<Collect*>(ctx);
    if (stem < 0 || size_t(stem) >= c->out.size()) {
        c->badStem = true;
        return false;
    }
    if (c->failAtCall >= 0 && c->calls >= c->failAtCall) return false;
    ++c->calls;
    c->out[size_t(stem)].insert(c->out[size_t(stem)].end(), p, p + 2 * size_t(frames));
    return true;
}

// Pushes all stems (equal length, interleaved stereo) in blocks that cycle
// through sizes[0..nsizes), then finish(). The history is pre-filled with
// garbage: begin() must zero it.
bool runClass(ExportResampler& rs, const std::vector<std::vector<int16_t>>& stems, const uint32_t* sizes, int nsizes,
              Collect& c) {
    const int n = int(stems.size());
    ExportResampler::History hist[ExportResampler::kMaxStems];
    for (auto& h : hist)
        for (auto& v : h) v = 0x5555;
    c.out.assign(size_t(n), std::vector<int16_t>());
    rs.begin(n, hist);
    std::vector<const int16_t*> ptr(static_cast<size_t>(n));
    const uint32_t total = uint32_t(stems[0].size() / 2);
    uint32_t pos = 0;
    int si = 0;
    while (pos < total) {
        const uint32_t f = std::min(sizes[si++ % nsizes], total - pos);
        for (int s = 0; s < n; ++s) ptr[size_t(s)] = stems[size_t(s)].data() + 2 * size_t(pos);
        if (!rs.pushBlock(ptr.data(), f, collectSink, &c)) return false;
        pos += f;
    }
    static const int16_t zeroBlock[kBlock * 2] = {};
    return rs.finish(zeroBlock, collectSink, &c);
}

const uint32_t kBlocks128[] = {128};
const uint32_t kBlocksUneven[] = {1, 3, 31, 100, 128};

// Mono signal -> converted left channel (L = R = signal, one stem).
std::vector<int16_t> convertMono(const std::vector<int16_t>& x) {
    std::vector<std::vector<int16_t>> stems(1);
    stems[0].resize(x.size() * 2);
    for (size_t i = 0; i < x.size(); ++i) stems[0][2 * i] = stems[0][2 * i + 1] = x[i];
    ExportResampler rs;
    Collect c;
    runClass(rs, stems, kBlocks128, 1, c);
    std::vector<int16_t> left(c.out[0].size() / 2);
    for (size_t i = 0; i < left.size(); ++i) left[i] = c.out[0][2 * i];
    return left;
}

// --- measurement helpers (lab: sineIn, steady, snrSine, flutter) -------------
std::vector<int16_t> sineIn(double f, double amp, uint32_t frames, double ph) {
    std::vector<int16_t> v(frames);
    for (uint32_t i = 0; i < frames; ++i)
        v[i] = clamp16(int32_t(std::floor(amp * std::sin(2.0 * kPi * f * i / kDev + ph) + 0.5)));
    return v;
}
double dbfs(double db) { return 32767.0 * std::pow(10.0, db / 20.0); }

// Steady region: skip one phase cycle plus the filter at both ends.
struct Region {
    uint64_t a, b;
};
Region steady(uint64_t frames) {
    return {kPhases + 64, frames > 2 * (kPhases + 64) ? frames - kPhases - 64 : frames};
}

double snrOf(const std::vector<int16_t>& out, double f, double amp, double ph) {
    const Region r = steady(out.size());
    double se = 0, ss = 0;
    for (uint64_t m = r.a; m < r.b; ++m) {
        const double ref = amp * std::sin(2.0 * kPi * f * double(m) / kOutRate + ph);
        ss += ref * ref;
        se += (out[m] - ref) * (out[m] - ref);
    }
    return 10.0 * std::log10(ss / std::max(se, 1e-30));
}

// The old 4-point cubic of the export (same position rule, taps x[n-1..n+2]).
std::vector<int16_t> cubicConvert(const std::vector<int16_t>& x) {
    const int64_t len = int64_t(x.size());
    const uint32_t outN = expectedFrames(uint64_t(len));
    auto at = [&](int64_t i) { return (i < 0 || i >= len) ? 0.0f : float(x[size_t(i)]); };
    std::vector<int16_t> out(outN);
    for (uint32_t m = 0; m < outN; ++m) {
        const int64_t n = int64_t(m) + int64_t(m / kPhases);
        const float mu = float(m % kPhases) / float(kPhases);
        out[m] = cubicS16(at(n - 1), at(n), at(n + 1), at(n + 2), mu);
    }
    return out;
}

const uint32_t kSnrFrames = kPhases * 12;
const double kSnrPhase = 0.3;

// Amplitude and position per phase bin (least-squares fit of a sin + b cos):
// flutter = max/min amplitude in dB, wobble = max - min position in samples.
void flutter(const std::vector<int16_t>& out, double f, double& flutterDb, double& wobble) {
    const int bins = 50;
    double ss[bins] = {}, cc[bins] = {}, sc[bins] = {}, ys[bins] = {}, yc[bins] = {};
    const Region r = steady(out.size());
    for (uint64_t m = r.a; m < r.b; ++m) {
        const int b = int((m % kPhases) * bins / kPhases);
        const double th = 2.0 * kPi * f * double(m) / kOutRate + kSnrPhase;
        const double s = std::sin(th), co = std::cos(th);
        ss[b] += s * s;
        cc[b] += co * co;
        sc[b] += s * co;
        ys[b] += out[m] * s;
        yc[b] += out[m] * co;
    }
    double amin = 1e30, amax = 0, pmin = 1e30, pmax = -1e30;
    for (int b = 0; b < bins; ++b) {
        const double det = ss[b] * cc[b] - sc[b] * sc[b];
        if (det <= 0) continue;
        const double a = (ys[b] * cc[b] - yc[b] * sc[b]) / det, bb = (yc[b] * ss[b] - ys[b] * sc[b]) / det;
        const double amp = std::hypot(a, bb), p = std::atan2(bb, a);   // out ~ amp sin(th + p)
        amin = std::min(amin, amp);
        amax = std::max(amax, amp);
        pmin = std::min(pmin, p);
        pmax = std::max(pmax, p);
    }
    flutterDb = 20.0 * std::log10(amax / amin);
    wobble = (pmax - pmin) / (2.0 * kPi * f) * kOutRate;   // samples
}

}  // namespace

// --- 1. table vs design -------------------------------------------------------
TEST(export_resampler_table_matches_the_exact_design) {
    const LsDesign& d = design();
    float w[kTaps];

    // Phase 0 is the identity: weight 1 on x[n], 0 elsewhere.
    exportSincWeights(0, w);
    for (int k = 0; k < kTaps; ++k) CHECK_EQ(w[k], k == kCentre ? 1.0f : 0.0f);

    // All 2499 phases against the double design at mu = r / 2499. (No phase
    // other than 0 lands exactly on a table row: 2499 and 32 are coprime.)
    double maxDiff = 0, worstSum = 0, worstSumVsDesign = 0;
    uint32_t worstPhase = 0, worstSumPhase = 0;
    for (uint32_t r = 0; r < kPhases; ++r) {
        double ex[kTaps];
        d.weights(double(r) / double(kPhases), ex);
        exportSincWeights(r, w);
        double sum = 0;
        for (int k = 0; k < kTaps; ++k) {
            sum += w[k];
            const double diff = std::fabs(double(w[k]) - ex[k]);
            if (diff > maxDiff) {
                maxDiff = diff;
                worstPhase = r;
            }
        }
        double dsum = 0;
        for (int k = 0; k < kTaps; ++k) dsum += ex[k];
        worstSumVsDesign = std::max(worstSumVsDesign, std::fabs(sum - dsum));
        if (std::fabs(sum - 1.0) > worstSum) {
            worstSum = std::fabs(sum - 1.0);
            worstSumPhase = r;
        }
    }
    std::printf("  table vs exact design: max |dw| %.3g (phase %u), worst |sum - 1| %.3g (phase %u), worst |sum - design sum| %.3g\n",
                maxDiff, unsigned(worstPhase), worstSum, unsigned(worstSumPhase), worstSumVsDesign);
    CHECK(maxDiff < 1e-4);
    CHECK(worstSumVsDesign < 1e-6);
    CHECK(worstSum < 1.5e-5);   // the least-squares design itself sums to 1 - 1.1e-5 at mu ~ 0.5

    // The design itself sums to 1 (sanity check of the port).
    double ex[kTaps];
    d.weights(0.0, ex);
    double s0 = 0;
    for (int k = 0; k < kTaps; ++k) s0 += ex[k];
    CHECK_NEAR(s0, 1.0, 1e-7);
    CHECK_NEAR(ex[kCentre], 1.0, 1e-7);   // 1e-13 diagonal jitter
}

TEST(export_resampler_table_is_mirror_symmetric) {
    float a[kTaps], b[kTaps];
    int bad = 0;
    for (uint32_t r = 1; r < kPhases; ++r) {
        exportSincWeights(r, a);
        exportSincWeights(kPhases - r, b);
        for (int k = 0; k < kTaps; ++k) bad += (a[k] != b[kTaps - 1 - k]);
    }
    CHECK_EQ(bad, 0);
}

// --- 2. bit-identical to the naive per-frame reference ------------------------
TEST(export_resampler_one_stem_equals_the_naive_reference) {
    const uint32_t frames = 50 * 128;
    Rng rng(12345);
    std::vector<std::vector<int16_t>> stems(1, std::vector<int16_t>(size_t(frames) * 2));
    for (auto& v : stems[0]) v = int16_t(rng.range(-20000, 20000));
    std::vector<int16_t> want;
    naiveConvert(stems[0], want);
    CHECK_EQ(want.size(), size_t(2 * 6397));

    ExportResampler rs;   // one object, begin() restarts it
    for (int mode = 0; mode < 2; ++mode) {
        Collect c;
        CHECK(runClass(rs, stems, mode ? kBlocksUneven : kBlocks128, mode ? 5 : 1, c));
        CHECK(!c.badStem);
        CHECK_EQ(c.out.size(), size_t(1));
        CHECK(c.out[0] == want);
    }
}

TEST(export_resampler_14_stems_equal_the_naive_reference) {
    const int nstems = ExportResampler::kMaxStems;
    const uint32_t frames = 50 * 128;
    Rng rng(777);
    std::vector<std::vector<int16_t>> stems(size_t(nstems), std::vector<int16_t>(size_t(frames) * 2));
    for (int s = 0; s < nstems; ++s) {
        std::vector<int16_t>& v = stems[size_t(s)];
        // Stems 7..11 are traps for the two exact skips (silent channel, L == R):
        // channels that differ only in a bit the cheap tests could overlook.
        switch (s) {
        case 0:   // silent
            break;
        case 1:   // L == R, moderate
            for (uint32_t i = 0; i < frames; ++i) v[2 * i] = v[2 * i + 1] = int16_t(rng.range(-9000, 9000));
            break;
        case 2:   // distinct channels, full-scale noise (clamps)
            for (auto& x : v) x = int16_t(rng.range(-32768, 32767));
            break;
        case 3:   // distinct channels, tiny values (rounding)
            for (auto& x : v) x = int16_t(rng.range(-3, 3));
            break;
        case 4:   // one channel silent, one noisy
            for (uint32_t i = 0; i < frames; ++i) v[2 * i + 1] = int16_t(rng.range(-30000, 30000));
            break;
        case 5:   // L == R, full scale
            for (uint32_t i = 0; i < frames; ++i) v[2 * i] = v[2 * i + 1] = int16_t(rng.range(-32768, 32767));
            break;
        case 6:   // distinct channels
            for (auto& x : v) x = int16_t(rng.range(-15000, 15000));
            break;
        case 7:   // R = L ^ 1: 1 LSB apart, equal high byte
            for (uint32_t i = 0; i < frames; ++i) {
                v[2 * i] = int16_t(rng.range(-20000, 20000));
                v[2 * i + 1] = int16_t(v[2 * i] ^ 1);
            }
            break;
        case 8:   // R = L + 256: equal low byte, different high byte
            for (uint32_t i = 0; i < frames; ++i) {
                v[2 * i] = int16_t(rng.range(-20000, 20000));
                v[2 * i + 1] = int16_t(v[2 * i] + 256);
            }
            break;
        case 9:   // L = k * 256 (low byte zero, not silent), R noise
            for (uint32_t i = 0; i < frames; ++i) {
                v[2 * i] = int16_t(rng.range(-100, 100) * 256);
                v[2 * i + 1] = int16_t(rng.range(-15000, 15000));
            }
            break;
        case 10:   // R = L ^ 0x100
            for (uint32_t i = 0; i < frames; ++i) {
                v[2 * i] = int16_t(rng.range(-20000, 20000));
                v[2 * i + 1] = int16_t(v[2 * i] ^ 0x100);
            }
            break;
        case 11:   // L == R except one frame (R ^ 0x100), inside a single chunk
            for (uint32_t i = 0; i < frames; ++i) v[2 * i] = v[2 * i + 1] = int16_t(rng.range(-9000, 9000));
            v[2 * 3000 + 1] = int16_t(v[2 * 3000 + 1] ^ 0x100);
            break;
        case 12:   // L == R except one frame (R ^ 1)
            for (uint32_t i = 0; i < frames; ++i) v[2 * i] = v[2 * i + 1] = int16_t(rng.range(-9000, 9000));
            v[2 * 4100 + 1] = int16_t(v[2 * 4100 + 1] ^ 1);
            break;
        default:   // L noisy, R silent
            for (uint32_t i = 0; i < frames; ++i) v[2 * i] = int16_t(rng.range(-30000, 30000));
            break;
        }
    }
    std::vector<std::vector<int16_t>> want(static_cast<size_t>(nstems));
    for (int s = 0; s < nstems; ++s) naiveConvert(stems[size_t(s)], want[size_t(s)]);

    ExportResampler rs;
    for (int mode = 0; mode < 2; ++mode) {
        Collect c;
        CHECK(runClass(rs, stems, mode ? kBlocksUneven : kBlocks128, mode ? 5 : 1, c));
        CHECK(!c.badStem);
        CHECK_EQ(c.out.size(), size_t(nstems));
        int differing = 0;
        for (int s = 0; s < nstems; ++s) {
            CHECK_EQ(c.out[size_t(s)].size(), size_t(2 * 6397));   // floor(6400 * 2499 / 2500)
            differing += (c.out[size_t(s)] != want[size_t(s)]);
        }
        CHECK_EQ(differing, 0);
        // A silent stem stays silent.
        bool allZero = true;
        for (int16_t v : c.out[0]) allZero = allZero && v == 0;
        CHECK(allZero);
    }
}

TEST(export_resampler_stops_when_the_sink_fails) {
    std::vector<std::vector<int16_t>> stems(2, std::vector<int16_t>(size_t(4 * 128) * 2, 1000));
    ExportResampler rs;
    Collect c;
    c.failAtCall = 3;
    CHECK(!runClass(rs, stems, kBlocks128, 1, c));
    CHECK(!c.badStem);
}

// --- 3. lengths and alignment -------------------------------------------------
TEST(export_resampler_output_lengths) {
    const uint32_t lens[] = {0, 1, 3, 31, 128, 2500, 50 * 128};
    const uint32_t want[] = {0, 0, 2, 30, 127, 2499, 6397};
    Rng rng(99);
    for (int i = 0; i < 7; ++i) {
        CHECK_EQ(ExportResampler::outputFrames(lens[i]), want[i]);
        CHECK_EQ(expectedFrames(lens[i]), want[i]);
        std::vector<std::vector<int16_t>> stems(1, std::vector<int16_t>(size_t(lens[i]) * 2));
        for (auto& v : stems[0]) v = int16_t(rng.range(-1000, 1000));
        ExportResampler rs;
        Collect c;
        CHECK(runClass(rs, stems, kBlocks128, 1, c));
        CHECK_EQ(c.out[0].size(), size_t(2) * want[i]);
        CHECK_EQ(rs.emittedFrames(), want[i]);
    }
    // The input count is tracked while pushing.
    ExportResampler rs;
    ExportResampler::History hist[1];
    rs.begin(1, hist);
    CHECK_EQ(rs.inputFrames(), 0u);
    std::vector<int16_t> blk(size_t(kBlock) * 2, 5);
    const int16_t* p = blk.data();
    Collect c;
    c.out.assign(1, std::vector<int16_t>());
    CHECK(rs.pushBlock(&p, kBlock, collectSink, &c));
    CHECK_EQ(rs.inputFrames(), kBlock);
}

TEST(export_resampler_impulse_at_frame_0_gives_frame_0) {
    std::vector<int16_t> x(600, 0);
    x[0] = 32767;
    const std::vector<int16_t> y = convertMono(x);
    CHECK_EQ(y.size(), size_t(expectedFrames(600)));
    CHECK_EQ(y[0], int16_t(32767));
    // Phase 0 is the identity, so the impulse is also alone at m = 0; later
    // frames carry the sinc tail, which stays within the impulse response.
    for (size_t m = 1; m < y.size(); ++m) CHECK(std::abs(int(y[m])) <= 64);
}

TEST(export_resampler_ramp_has_no_position_error) {
    // x[i] = slope * i - offset. With g = sum of the weights of a phase (1 up to
    // ~1e-5 for the least-squares design), output m is
    //   g * (slope * n - offset) + slope * (mu + p),   n = m + m / 2499,
    // where p is the position error in samples. The output is int16, so the
    // residual is p * slope plus a rounding error of at most 0.5.
    const double slope = 16.0, offset = 30000.0;
    const uint32_t frames = 3800;
    std::vector<int16_t> x(frames);
    for (uint32_t i = 0; i < frames; ++i) x[i] = int16_t(slope * i - offset);   // exact integers
    const std::vector<int16_t> y = convertMono(x);
    CHECK(y.size() > kPhases + 128);   // every phase occurs
    double worst = 0;
    for (uint64_t m = 64; m + 64 < y.size(); ++m) {   // away from the zero-padded ends
        const uint64_t r = m % kPhases;
        float w[kTaps];
        exportSincWeights(uint32_t(r), w);
        double g = 0;
        for (int k = 0; k < kTaps; ++k) g += double(w[k]);
        const double n = double(m + m / kPhases), mu = double(r) / double(kPhases);
        const double want = g * (slope * n - offset) + slope * mu;
        worst = std::max(worst, std::fabs(double(y[m]) - want));
    }
    const double posErr = std::max(0.0, worst - 0.5) / slope;
    std::printf("  ramp: max residual %.4f LSB, position error <= %.4f samples (cubic: 0.0967)\n", worst, posErr);
    CHECK(posErr < 0.0003);
}

// --- 4. quality -----------------------------------------------------------------
TEST(export_resampler_sine_snr) {
    struct Case {
        double f, minDb;
    };
    // Lab (L32, round, int16 in and out, -1 dBFS): 93.6 / 93.7 / 92.3 / 90.8 dB.
    const Case cases[] = {{1000, 90}, {10000, 90}, {15000, 89}, {18000, 87}, {20000, -1}};
    const double amp = dbfs(-1);
    for (const Case& c : cases) {
        const std::vector<int16_t> x = sineIn(c.f, amp, kSnrFrames, kSnrPhase);
        const double snr = snrOf(convertMono(x), c.f, amp, kSnrPhase);
        const double before = snrOf(cubicConvert(x), c.f, amp, kSnrPhase);
        std::printf("  %5.0f Hz at -1 dBFS: SNR %5.1f dB (old cubic %5.1f dB)\n", c.f, snr, before);
        if (c.minDb > 0) CHECK(snr >= c.minDb);
    }
}

TEST(export_resampler_flutter_and_position_wobble_at_10khz) {
    const std::vector<int16_t> x = sineIn(10000, dbfs(-1), kSnrFrames, kSnrPhase);
    const std::vector<int16_t> y = convertMono(x);
    double fl = 0, wob = 0;
    flutter(y, 10000, fl, wob);
    double flOld = 0, wobOld = 0;
    flutter(cubicConvert(x), 10000, flOld, wobOld);
    std::printf("  10 kHz: flutter %.4f dB, wobble %.4f samples (old cubic %.3f dB, %.4f samples)\n", fl, wob,
                flOld, wobOld);
    CHECK(fl < 0.005);
    CHECK(wob < 0.001);
}

// --- 5. extremes ----------------------------------------------------------------
TEST(export_resampler_silence_is_exact) {
    const std::vector<int16_t> y = convertMono(std::vector<int16_t>(5000, 0));
    CHECK_EQ(y.size(), size_t(expectedFrames(5000)));
    int bad = 0;
    for (int16_t v : y) bad += (v != 0);
    CHECK_EQ(bad, 0);
}

TEST(export_resampler_dc_is_exact_in_the_steady_part) {
    const int16_t levels[] = {32767, -32768, 1, -1};
    for (int16_t level : levels) {
        const std::vector<int16_t> y = convertMono(std::vector<int16_t>(5000, level));
        int bad = 0;
        // The filter is 32 frames long: skip 32 frames at both ends (zero padding).
        for (size_t m = 32; m + 32 < y.size(); ++m) bad += (y[m] != level);
        CHECK_EQ(bad, 0);
    }
}

TEST(export_resampler_full_scale_sine_clips_only_a_few_samples) {
    const std::vector<int16_t> x = sineIn(10000, 32767.0, kPhases * 4, 0.3);
    std::vector<int16_t> in2(x.size() * 2), ref;
    for (size_t i = 0; i < x.size(); ++i) in2[2 * i] = in2[2 * i + 1] = x[i];
    std::vector<int32_t> raw;
    naiveConvert(in2, ref, &raw);
    int beyond = 0;
    for (int32_t v : raw) beyond += (v > 32767 || v < -32768);
    const std::vector<int16_t> y = convertMono(x);
    CHECK_EQ(y.size() * 2, raw.size());
    int differing = 0;
    for (size_t m = 0; m < y.size(); ++m) differing += (y[m] != clamp16(raw[2 * m]));
    CHECK_EQ(differing, 0);
    // Old cubic: 1668 clipped samples of 9992 (lab).
    std::printf("  0 dBFS 10 kHz sine: %d of %zu samples beyond int16 (clamped)\n", beyond, y.size());
    CHECK(beyond <= 4);
}

TEST(export_resampler_square_wave_clamps_without_wrap) {
    // Full-scale square, 50 frames per half period.
    const uint32_t frames = kPhases * 4;
    std::vector<int16_t> x(frames);
    for (uint32_t i = 0; i < frames; ++i) x[i] = (i / 50) % 2 ? int16_t(-32768) : int16_t(32767);
    std::vector<int16_t> in2(x.size() * 2), ref;
    for (size_t i = 0; i < x.size(); ++i) in2[2 * i] = in2[2 * i + 1] = x[i];
    std::vector<int32_t> raw;
    naiveConvert(in2, ref, &raw);
    const std::vector<int16_t> y = convertMono(x);
    int beyond = 0, differing = 0, wrong = 0, clamped = 0;
    int32_t peak = 0;
    for (size_t m = 0; m < y.size(); ++m) {
        const int32_t r = raw[2 * m];
        beyond += (r > 32767 || r < -32768);
        peak = std::max(peak, std::abs(r));
        differing += (y[m] != clamp16(r));
        clamped += (y[m] == 32767 || y[m] == -32768);
    }
    // Away from the edges (and from both ends) the output keeps the sign and
    // most of the size of the plateau; a wrapped sample would flip the sign.
    for (size_t m = 64; m + 64 < y.size(); ++m) {
        const double t = double(m) * 2500.0 / 2499.0;
        // Edges lie at t = 50 k - 0.5; dist = frames to the nearest one.
        const double dist = 25.0 - std::fabs(std::fmod(t + 0.5, 50.0) - 25.0);
        if (dist < 3.0) continue;
        const bool plus = (int64_t(std::floor(t)) / 50) % 2 == 0;
        wrong += plus ? !(y[m] > 20000) : !(y[m] < -20000);
    }
    std::printf("  full-scale square: peak %d (%.2f dB over full scale), %d samples beyond int16, %d at the limits\n",
                int(peak), 20.0 * std::log10(double(peak) / 32767.0), beyond, clamped);
    CHECK_EQ(differing, 0);
    CHECK_EQ(wrong, 0);
    CHECK(beyond > 0);   // the filter overshoots, the clamp is exercised
}

// --- 6. rounding ----------------------------------------------------------------
TEST(export_resampler_rounds_to_nearest_not_truncation) {
    // Isolated impulses of height v, 100 frames apart: every output near one is
    // the single product w * v, a non-integer almost everywhere. The class must
    // round it half away from zero, so a negative train gives exactly the
    // negated output. Covers more than one phase cycle.
    const uint32_t frames = 6000;
    const int amps[] = {1000, 7};
    int truncDiffers = 0, nonzero = 0;
    for (int v : amps) {
        std::vector<int16_t> pos(frames, 0), neg(frames, 0);
        for (uint32_t i = 100; i < frames - 100; i += 100) {
            pos[i] = int16_t(v);
            neg[i] = int16_t(-v);
        }
        const std::vector<int16_t> yp = convertMono(pos), yn = convertMono(neg);
        CHECK_EQ(yp.size(), yn.size());
        int asym = 0, wrong = 0;
        for (size_t m = 0; m < yp.size(); ++m) {
            const int64_t n = int64_t(m) + int64_t(m / kPhases);
            float w[kTaps];
            exportSincWeights(uint32_t(m % kPhases), w);
            float acc = 0.0f;
            for (int k = 0; k < kTaps; ++k) {
                const int64_t i = n - kCentre + k;
                if (i >= 100 && i < int64_t(frames) - 100 && i % 100 == 0) acc = w[k] * float(v);
            }
            wrong += (yp[m] != clamp16(roundSum(acc)));
            asym += (yn[m] != -yp[m]);
            truncDiffers += (int(acc) != roundSum(acc));
            nonzero += (yp[m] != 0);
        }
        CHECK_EQ(wrong, 0);
        CHECK_EQ(asym, 0);
    }
    CHECK(nonzero > 1000);
    CHECK(truncDiffers > 500);   // truncation would be noticed
}
