#include "app/sample_fx.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "fmath.h"

namespace ot {
namespace sfx {

namespace {

constexpr float kRate = 44100.0f;
// AUDIO_SAMPLE_RATE_EXACT: the stock conversions for the bitcrusher hold, the
// flanger LFO and the note time-stretch use it (spec 18 §2.2).
constexpr double kRateExact = 44117.647;
constexpr double kPi = 3.14159265358979323846;

// SPEC GAP: the stock type labels are not in the specs; the curves are stock
// (spec 18 §2.2 Overdrive: piecewise soft clip, exponential, stepped).
const char* const kOdTypes[3] = {"Soft", "Exp", "Step"};

// spec 06 §6.4 table (names are OpenTracker's wording).
const EffectDesc kEffects[kEffectCount] = {
    {"Normalizer", 1, {{"Level", 1, 100, 1, 100, kFmtPercent, nullptr}}},   // SPEC GAP: "runtime min" -> 1
    {"Crop", 0, {}},
    {"Reverse", 0, {}},
    {"Amplifier", 1, {{"Gain", 0.01f, 5, 0.01f, 1, kFmtFloat2, nullptr}}},
    {"Overdrive", 2, {{"Gain", 0.01f, 5, 0.01f, 1, kFmtFloat2, nullptr}, {"Type", 0, 2, 1, 0, kFmtLabel, kOdTypes}}},
    {"Delay", 2, {{"Feedback", 0.01f, 0.95f, 0.01f, 0.95f, kFmtX100, nullptr}, {"Time", 1, 5000, 5, 50, kFmtMs, nullptr}}},
    {"Bitcrusher", 2, {{"Bits", 1, 16, 1, 4, kFmtInt, nullptr}, {"Rate", 500, 44100, 100, 44100, kFmtHz, nullptr}}},
    {"Chorus", 2, {{"Length", 128, 4096, 128, 128, kFmtSamplesMs, nullptr}, {"Voices", 1, 8, 1, 1, kFmtInt, nullptr}}},
    {"Flanger",
     4,
     {{"Length", 128, 4096, 128, 128, kFmtSamplesMs, nullptr},
      {"Offset", 1, 100, 1, 50, kFmtPercent, nullptr},
      {"Depth", 1, 8, 1, 1, kFmtInt, nullptr},
      {"Rate", 0.1f, 5, 0.1f, 1, kFmtHzF, nullptr}}},
    {"Equalizer",
     9,
     {{"Low gain", -12, 12, 0.05f, 0, kFmtDb2, nullptr},
      {"Low freq", 10, 200, 1, 80, kFmtHz, nullptr},
      {"Low width", 0.5f, 5, 0.05f, 1, kFmtFloat2, nullptr},
      {"Mid gain", -12, 12, 0.05f, 0, kFmtDb2, nullptr},
      {"Mid freq", 100, 8000, 10, 800, kFmtHz, nullptr},
      {"Mid width", 0.5f, 5, 0.05f, 1, kFmtFloat2, nullptr},
      {"High gain", -12, 12, 0.05f, 0, kFmtDb2, nullptr},
      {"High freq", 8000, 20000, 100, 12000, kFmtHz, nullptr},
      {"High width", 0.5f, 5, 0.05f, 1, kFmtFloat2, nullptr}}},
    {"Limiter",
     3,
     {{"Threshold", -60, 0, 1, -6, kFmtDb, nullptr},
      {"Attack", 0.3f, 10, 0.1f, 3, kFmtMsF, nullptr},
      {"Release", 1, 2000, 1, 300, kFmtMs, nullptr}}},
    {"Compressor",
     4,
     {{"Threshold", -60, 0, 1, -12, kFmtDb, nullptr},
      {"Ratio", 1, 50, 0.5f, 4, kFmtFloat1, nullptr},
      {"Attack", 0.3f, 100, 0.1f, 10, kFmtMsF, nullptr},
      {"Release", 1, 2000, 1, 50, kFmtMs, nullptr}}},
    {"WT Smoother",
     2,
     {{"Window Num", 1, 256, 1, 256, kFmtInt, nullptr}, {"Window Len", 1, 2048, 1, 2048, kFmtInt, nullptr}}},
    // SPEC GAP: no default tempo in the table; 120 BPM (the screen sets the project tempo on entry).
    {"Timestretch Beat",
     4,
     {{"Tempo", 40, 800, 1, 120, kFmtBpm, nullptr},
      {"Steps", 1, 64, 1, 16, kFmtInt, nullptr},
      {"Micro-step", 0, 1, 0.01f, 0, kFmtFloat2, nullptr},
      {"Granule", 1, 10, 1, 4, kFmtQuarter, nullptr}}},
    // SPEC GAP: no default length; 1.00 s.
    {"Timestretch Note", 2, {{"Length", 0.1f, 8, 0.01f, 1, kFmtSeconds, nullptr}, {"Granule", 10, 1000, 1, 100, kFmtMs, nullptr}}},
    {"Fade In", 0, {}},
    {"Fade Out", 0, {}},
};

// y = sat(trunc(v)): the stock results truncate toward zero and saturate to
// int16 (spec 18 §2.1).
inline int16_t sat(double v) {
    if (!(v == v)) return 0;
    if (v >= 32767.0) return 32767;
    if (v <= -32768.0) return -32768;
    return int16_t(int32_t(v));
}

// The same in single precision for the per-sample loops: the Cortex-M4F has a
// single-precision FPU only, so a double per sample is a soft-float call.
inline int16_t satf(float v) {
    if (!(v == v)) return 0;
    if (v >= 32767.0f) return 32767;
    if (v <= -32768.0f) return -32768;
    return int16_t(int32_t(v));
}

// trunc(x * a / d) for d > 0, exactly (toward zero, as the stock results),
// without the M4's software 64-bit division: est is a float estimate of the
// quotient (within a few units), corrected with the exact remainder. Every
// product is 32 x 32 -> 64 bits (one M4 instruction).
inline int32_t truncMulDiv(int32_t x, int32_t a, int32_t d, float est) {
    const int64_t q = int64_t(x) * a;
    int32_t t = int32_t(est);
    int64_t r = q - int64_t(t) * d;
    if (q >= 0) {
        while (r < 0) --t, r += d;
        while (r >= d) ++t, r -= d;
    } else {
        while (r > 0) ++t, r -= d;
        while (r <= -d) --t, r += d;
    }
    return t;
}

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline float at(const int16_t* in, uint32_t n, int64_t i) { return i >= 0 && i < int64_t(n) ? float(in[i]) : 0.0f; }

// Delay (spec 18 §2.2): tap spacing D = trunc(T * 44.1); K = the smallest k
// with f^k <= 0.01, at most 98.
void delayTaps(const float* p, uint32_t* D, int* K) {
    *D = uint32_t(double(p[1]) * 44.1);
    if (*D < 1) *D = 1;
    const double f = p[0];
    double g = 1.0;
    int k = 0;
    while (g > 0.01 && k < 98) {
        g *= f;
        ++k;
    }
    *K = k;
}

// Wavetable smoother: effective source window count n (spec 18 §2.2).
int smootherSource(const float* p, uint32_t len) {
    const int C = int(p[0]), W = int(p[1]) < 1 ? 1 : int(p[1]);
    int n = int(len / uint32_t(W)) - 1;
    if (C - 1 < n) n = C - 1;
    return n < 1 ? 1 : n;
}

// WSOLA engine shared by both time-stretch effects (spec 18 §2.2): input
// segments of N samples are copied to the output; each next segment's start
// is the candidate around its nominal position whose overlap best matches the
// natural continuation of the previous segment, by cross-correlation weighted
// with the parabola (O - i) * i. The output is zero-filled once the source is
// used up.
// SPEC GAP: overlap O = N/4 and seek range +-min(N/4, 512) are OpenTracker's
// (the stock sizes, lg:0x7a650, are not decoded); the correlation is
// sub-sampled to at most 256 points per candidate (at most 511 when O < 512).
//
// The weight and the continuation do not depend on the candidate, so their
// product is built once per segment (an int32 kernel) and each candidate is
// one integer multiply-add per point, summed exactly in int64: the M4 has no
// double FPU, and the old double sum per candidate took ~50 s on a 0.45 s
// sample. The kernel is exact while (O/2)^2 * 32768 fits an int32 (O < 512);
// above that it is scaled down by the fewest bits that fit, which can only
// change which candidate wins on a near-tie.
constexpr uint32_t kWsolaPoints = 512;   // > the most sampled points (511, at O = 511)

uint32_t wsola(const int16_t* in, uint32_t n, uint32_t m, uint32_t N, int16_t* out, uint32_t cap) {
    const uint32_t w = m < cap ? m : cap;
    for (uint32_t i = 0; i < w; ++i) out[i] = 0;
    if (N > n) N = n;
    if (N < 16) {
        // Too short to segment: plain copy of what fits.
        for (uint32_t i = 0; i < w && i < n; ++i) out[i] = in[i];
        return w;
    }
    const uint32_t O = N / 4;
    const uint32_t Hs = N - O;
    const int64_t S = int64_t(O < 512 ? O : 512);
    const uint32_t stride = O > 256 ? O / 256 : 1;
    const uint32_t P = (O + stride - 1) / stride;   // sampled points, i = 0, stride, ...
    // Kernel scale: |w(i) * x| <= (O/2)^2 * 32768 must fit an int32.
    int shift = 0;
    while ((uint64_t(O) * O / 4) * 32768u >> shift > 0x7FFFFFFFu) ++shift;
    int32_t kern[kWsolaPoints];
    const double alpha = double(n) / double(m);
    const int64_t last = int64_t(n) - int64_t(N);
    // First segment: straight copy.
    for (uint32_t i = 0; i < N && i < w; ++i) out[i] = in[i];
    int64_t prev = 0;
    for (uint64_t k = 1;; ++k) {
        const uint64_t o0 = k * Hs;
        if (o0 >= w) break;
        const int64_t nominal = int64_t(double(o0) * alpha);
        if (nominal > last) break;   // source used up: the rest stays zero
        const int64_t nat = prev + int64_t(Hs);   // natural continuation
        for (uint32_t j = 0; j < P; ++j) {
            const uint32_t i = j * stride;
            const int64_t wt = int64_t(O - i) * int64_t(i);
            const int64_t x = nat + i < int64_t(n) ? in[nat + i] : 0;
            kern[j] = int32_t((wt * x) >> shift);
        }
        int64_t best = nominal;
        int64_t bestC = INT64_MIN;
        const int64_t lo = nominal - S < 0 ? 0 : nominal - S;
        const int64_t hi = nominal + S > last ? last : nominal + S;
        for (int64_t s = lo; s <= hi; ++s) {
            const int16_t* y = in + s;
            int64_t c = 0;
            if (stride == 1) {
                // Every point (O < 512, the common case): four at a time.
                uint32_t j = 0;
                int64_t c1 = 0, c2 = 0, c3 = 0;
                for (; j + 4 <= P; j += 4) {
                    c += int64_t(kern[j]) * y[j];
                    c1 += int64_t(kern[j + 1]) * y[j + 1];
                    c2 += int64_t(kern[j + 2]) * y[j + 2];
                    c3 += int64_t(kern[j + 3]) * y[j + 3];
                }
                for (; j < P; ++j) c += int64_t(kern[j]) * y[j];
                c += c1 + c2 + c3;   // exact: the order of an integer sum does not matter
            } else {
                for (uint32_t j = 0; j < P; ++j) c += int64_t(kern[j]) * y[j * stride];
            }
            if (c > bestC) {
                bestC = c;
                best = s;
            }
        }
        for (uint32_t i = 0; i < N; ++i) {
            const uint64_t o = o0 + i;
            if (o >= w) break;
            if (i < O) {
                const double a = double(i) / double(O);
                out[o] = sat(double(at(in, n, nat + i)) * (1.0 - a) + double(in[best + i]) * a);
            } else {
                out[o] = in[best + i];   // sat(double(x)) == x for an int16
            }
        }
        prev = best;
    }
    return w;
}

// Peaking biquad (direct form I, normalised samples).
// SPEC GAP: the stock coefficient formula (lg:0x791c0) is not decoded; this
// is the RBJ peaking EQ with Q = f / bw.
// Kept in double: a float form loses low-frequency gain (DC at 10 Hz +12 dB:
// 32767 -> 32283), even around the second differences.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void peaking(double f, double lin, double q) {
        // fmath.h double helpers: no double libm on the device
        // (docs/OPTIMIZATION_2X.md OP-4).
        const double A = fm::sqrtD(lin);
        const double w0 = 2.0 * kPi * f / kRateExact;
        const double sn = fm::sinD(w0), cs = fm::cosD(w0);
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

// Limiter / compressor smoothing coefficient (spec 18 §2.2):
// c = 1 - exp(-2.2 * 1000 / (t_ms * 44100)).
double dynCoef(float ms) {
    return ms <= 0.0f ? 1.0 : 1.0 - fm::expD(-2.2 * 1000.0 / (double(ms) * 44100.0));
}

}  // namespace

const EffectDesc& effect(int i) { return kEffects[i < 0 || i >= kEffectCount ? 0 : i]; }

void defaults(int fx, float* p) {
    const EffectDesc& e = effect(fx);
    for (int i = 0; i < kMaxParams; ++i) p[i] = i < e.params ? e.p[i].def : 0.0f;
}

float clampParam(int fx, int i, float v) {
    const EffectDesc& e = effect(fx);
    if (i < 0 || i >= e.params) return v;
    const ParamDesc& d = e.p[i];
    // Snap to the step grid from min (avoids float drift on repeated steps).
    float k = std::round((v - d.min) / d.step);
    v = d.min + k * d.step;
    if (v < d.min) v = d.min;
    if (v > d.max) v = d.max;
    return v;
}

void formatParam(int fx, int i, float v, char* out, int cap) {
    const EffectDesc& e = effect(fx);
    const size_t c = size_t(cap);
    if (i < 0 || i >= e.params) {
        if (cap) out[0] = 0;
        return;
    }
    const ParamDesc& d = e.p[i];
    switch (d.fmt) {
        case kFmtInt: std::snprintf(out, c, "%d", int(std::lround(v))); break;
        case kFmtFloat2: std::snprintf(out, c, "%.2f", double(v)); break;
        case kFmtFloat1: std::snprintf(out, c, "%.1f", double(v)); break;
        case kFmtPercent: std::snprintf(out, c, "%d%%", int(std::lround(v))); break;
        case kFmtX100: std::snprintf(out, c, "%d%%", int(std::lround(v * 100.0f))); break;
        case kFmtMs: std::snprintf(out, c, "%d ms", int(std::lround(v))); break;
        case kFmtMsF: std::snprintf(out, c, "%.1f ms", double(v)); break;
        case kFmtHz: std::snprintf(out, c, "%d Hz", int(std::lround(v))); break;
        case kFmtHzF: std::snprintf(out, c, "%.1f Hz", double(v)); break;
        // EQ gains step 0.05 dB: two decimals; thresholds step 1 dB (manual
        // p.213, MANUAL_AUDIT_2026-10-23 #15).
        case kFmtDb: std::snprintf(out, c, "%d dB", int(std::lround(v))); break;
        case kFmtDb2: std::snprintf(out, c, "%.2f dB", double(v)); break;
        case kFmtSamplesMs: std::snprintf(out, c, "%.1f ms", double(v * 1000.0f / kRate)); break;
        case kFmtQuarter: std::snprintf(out, c, "%.2f", double(v * 0.25f)); break;
        case kFmtSeconds: std::snprintf(out, c, "%.2f s", double(v)); break;
        case kFmtBpm: std::snprintf(out, c, "%d BPM", int(std::lround(v))); break;
        case kFmtLabel: {
            const int k = int(std::lround(v));
            std::snprintf(out, c, "%s", d.labels && k >= 0 && k <= int(d.max) ? d.labels[k] : "?");
            break;
        }
    }
}

uint32_t outputFrames(int fx, const float* p, uint32_t n) {
    switch (fx) {
        case kDelay: {
            // The echo runs until the last tap has passed the selection end.
            uint32_t D;
            int K;
            delayTaps(p, &D, &K);
            const uint64_t m = uint64_t(n) + uint64_t(K) * D;
            return m > 0xFFFFFFFFu ? 0xFFFFFFFFu : uint32_t(m);
        }
        case kWavetableSmoother: {
            const uint32_t C = uint32_t(p[0]), W = uint32_t(p[1]);
            if (smootherSource(p, n) == int(C)) return n;   // n == C: unchanged copy
            return C * W;
        }
        case kStretchBeat: {
            // SPEC GAP: the stock target-length formula is open; the target is
            // (steps + micro step) sixteenth notes at the given tempo.
            const float secs = (p[1] + p[2]) * 15.0f / (p[0] > 0.0f ? p[0] : 120.0f);
            return uint32_t(secs * kRate + 0.5f);
        }
        case kStretchNote:
            // A target of 0 means the current length.
            return p[0] <= 0.0f ? n : uint32_t(p[0] * kRate + 0.5f);
        default: return n;
    }
}

uint32_t process(int fx, const float* p, const int16_t* in, uint32_t n, int16_t* out, uint32_t cap) {
    const uint32_t m = outputFrames(fx, p, n);
    const uint32_t w = m < cap ? m : cap;
    if (n == 0) {
        for (uint32_t i = 0; i < w; ++i) out[i] = 0;
        return w;
    }
    switch (fx) {
        case kNormalizer: {
            // Peak P = max|x|; gain = (level * 32767 / 100) / P. The stock
            // code does not guard P = 0; a silent selection is copied here.
            int peak = 0;
            for (uint32_t i = 0; i < n; ++i) {
                const int a = in[i] < 0 ? -int(in[i]) : in[i];
                if (a > peak) peak = a;
            }
            // Exactly: y = trunc(x * level * 32767 / (100 * P)), so the peak
            // lands on level * 32767 / 100 (32767 at 100 %). Level is whole
            // percent (step 1).
            if (!peak) {
                std::memcpy(out, in, w * sizeof(int16_t));
                break;
            }
            const int32_t num = int32_t(std::lround(p[0])) * 32767, den = 100 * peak;
            const float g = float(num) / float(den);
            for (uint32_t i = 0; i < w; ++i)
                out[i] = int16_t(clampi(truncMulDiv(in[i], num, den, float(in[i]) * g), -32768, 32767));
            break;
        }
        case kCrop: std::memcpy(out, in, w * sizeof(int16_t)); break;
        case kReverse:
            for (uint32_t i = 0; i < w; ++i) out[i] = in[n - 1 - i];
            break;
        case kAmplifier:
            for (uint32_t i = 0; i < w; ++i) out[i] = satf(float(in[i]) * p[0]);
            break;
        case kOverdrive: {
            const int type = clampi(int(p[1]), 0, 2);
            const float g = p[0];
            for (uint32_t i = 0; i < w; ++i) {
                const int x = in[i];
                const float ax = x < 0 ? -float(x) : float(x);
                float y;
                if (type == 0) {
                    // Piecewise soft clip on s = |x g| / FS.
                    // SPEC GAP: above 2/3 FS the stock behaviour is taken as a
                    // hard clip to FS (spec 18 §2.3 item 5).
                    const float s = ax * g / 32767.0f;
                    if (s < 1.0f / 3.0f) y = 2.0f * s;
                    else if (s < 2.0f / 3.0f) y = (3.0f - (2.0f - 3.0f * s) * (2.0f - 3.0f * s)) / 3.0f;
                    else y = 1.0f;
                    y *= 32767.0f;
                } else if (type == 1) {
                    const float k = 4.0f * g;
                    y = (1.0f - std::exp(-k * ax / 32767.0f)) * 32767.0f;
                } else {
                    // The integer truncation is the binary's (spec 18 §2.2).
                    const float k = 10.0f * g;
                    const float t = float(int32_t(k * ax / 32767.0f));   // >= 0, <= 50
                    y = (1.0f - 1.0f / (1.0f + t)) * 32767.0f;
                }
                out[i] = satf(x < 0 ? -y : y);
            }
            break;
        }
        case kDelay: {
            // Multi-tap echo: y[i] = sat(sum_{k=0..K} f^k x[i - kD]).
            uint32_t D;
            int K;
            delayTaps(p, &D, &K);
            // Computed as the recurrence y[i] = x[i] + f y[i-D] - f^(K+1) x[i-(K+1)D]
            // (x = 0 outside the selection): three operations per sample
            // instead of up to K + 1 = 99. Samples D apart form independent
            // chains, so they run chain by chain, kChains at a time, each with
            // its running sum y in a small array: no D-sample history (up to
            // 220,500 at 5 s) and sequential memory access within a block.
            constexpr uint32_t kChains = 256;
            const float f = p[0];
            double gK = 1.0;
            for (int k = 0; k <= K; ++k) gK *= double(p[0]);   // f^(K+1)
            const float fK1 = float(gK);
            const uint64_t lag = uint64_t(K + 1) * D;
            float y[kChains];
            for (uint32_t r0 = 0; r0 < D && r0 < w; r0 += kChains) {
                const uint32_t nb = D - r0 < kChains ? D - r0 : kChains;
                for (uint32_t b = 0; b < nb; ++b) y[b] = 0.0f;
                for (uint64_t base = r0; base < w; base += D) {
                    for (uint32_t b = 0; b < nb; ++b) {
                        const uint64_t i = base + b;
                        if (i >= w) break;
                        const float x = i < n ? float(in[i]) : 0.0f;
                        const float xo = i >= lag && i - lag < n ? float(in[i - lag]) : 0.0f;
                        y[b] = x + f * y[b] - fK1 * xo;
                        out[i] = satf(y[b]);
                    }
                }
            }
            break;
        }
        case kBitcrusher: {
            // Hold H = clamp(round(44117.647 / R), 1, 64) samples; keep the
            // top B bits (B = 16: hold only). Rounded to the nearest step
            // (stock truncates toward -inf, which adds hiss and DC to quiet
            // material) -- OpenTracker deviation, deliberate.
            const int B = clampi(int(p[0]), 1, 16);
            const int H = clampi(int(fm::lroundD(kRateExact / double(p[1] > 0.0f ? p[1] : 1.0f))), 1, 64);
            const int shift = 16 - B;
            const int32_t half = B == 16 ? 0 : 1 << (shift - 1);
            int32_t held = 0;
            for (uint32_t i = 0; i < w; ++i) {
                if (i % uint32_t(H) == 0) held = in[i];
                out[i] = int16_t(B == 16 ? held : clampi(((held + half) >> shift) * (1 << shift), -32768, 32767));
            }
            break;
        }
        case kChorus: {
            // PJRC AudioEffectChorus structure: line L = length/2 (zeroed at
            // start); tap k sits k * (L/(n-1) - 1) samples back; integer mean.
            // The taps never wrap past L, so the line is read from the input.
            const int voices = int(p[1]);
            const int L = int(p[0]) / 2;
            if (voices < 2 || L < 1) {
                std::memcpy(out, in, w * sizeof(int16_t));
                break;
            }
            const int step = L / (voices - 1) - 1;
            for (uint32_t i = 0; i < w; ++i) {
                int32_t sum = 0;
                int d = 0;
                for (int k = 0; k < voices; ++k) {
                    const int64_t j = int64_t(i) - d;
                    sum += j >= 0 ? in[j] : 0;
                    d += step;
                    if (d >= L) d -= L;
                }
                out[i] = int16_t(sum / voices);
            }
            break;
        }
        case kFlanger: {
            // PJRC AudioEffectFlange-style: line L = length/2; offset =
            // trunc(L * offset% / 100) <= L-1; depth in samples; LFO phase
            // increment = rate * 2^31 / 44117.647 (31-bit phase).
            // SPEC GAP: the per-sample tap (0x79980) is not decoded: the tap is
            // offset + depth * sin(phase) samples back, read with linear
            // interpolation, and mixed (x + tap) / 2.
            const int L = int(p[0]) / 2;
            int off = int(double(L) * double(p[1]) / 100.0);
            if (off > L - 1) off = L - 1;
            if (off < 0) off = 0;
            const float depth = float(int(p[2]));
            const uint32_t incr = uint32_t(double(p[3]) * 2147483648.0 / kRateExact);
            constexpr float kPhaseToRad = float(2.0 * kPi / 2147483648.0);
            uint32_t phase = 0;
            for (uint32_t i = 0; i < w; ++i) {
                float d = float(off) + depth * std::sin(kPhaseToRad * float(phase));
                if (d < 0.0f) d = 0.0f;
                if (d > float(L - 1)) d = float(L - 1);
                // pos = i - d, split so the index stays exact on long samples:
                // floor(i - d) = i - ceil(d), and the fraction is ceil(d) - d.
                const int32_t cd = int32_t(d) + (float(int32_t(d)) < d ? 1 : 0);
                const float fr = float(cd) - d;
                const int64_t i0 = int64_t(i) - cd;
                const float tap = at(in, n, i0) * (1.0f - fr) + at(in, n, i0 + 1) * fr;
                out[i] = int16_t((int32_t(in[i]) + int32_t(tap)) / 2);
                phase = (phase + incr) & 0x7FFFFFFFu;
            }
            break;
        }
        case kEqualizer: {
            // Three cascaded peaking bands; bw = f'/(w + 3 f'/20000) with
            // f' = min(f, 20000), clamped 0..20000; a band whose (linear) gain
            // is exactly 1.0 is bypassed. y = sat(trunc(clip(v) * 32767)).
            // SPEC GAP: UI dB -> linear gain 10^(dB/20) (the stock conversion
            // is open).
            Biquad b[3];
            bool on[3];
            for (int k = 0; k < 3; ++k) {
                const double lin = fm::pow10D(double(p[3 * k]) / 20.0);
                const double f = p[3 * k + 1] < 20000.0f ? double(p[3 * k + 1]) : 20000.0;
                double bw = f / (double(p[3 * k + 2]) + 3.0 * f / 20000.0);
                if (bw > 20000.0) bw = 20000.0;
                on[k] = lin != 1.0 && bw > 0.0 && f > 0.0;
                if (on[k]) b[k].peaking(f, lin, f / bw);
            }
            for (uint32_t i = 0; i < w; ++i) {
                double v = double(in[i]) / 32767.0;
                for (int k = 0; k < 3; ++k)
                    if (on[k]) v = b[k].run(v);
                if (v > 1.0) v = 1.0;
                if (v < -1.0) v = -1.0;
                out[i] = sat(v * 32767.0);
            }
            break;
        }
        case kLimiter: {
            const float thr = float(fm::pow10D(double(p[0]) / 20.0) * 32767.0);
            const float ca = float(dynCoef(p[1])), cr = float(dynCoef(p[2]));
            // SPEC GAP: c0 (obj+0x74, set at ctor) is not decoded; 1.0 makes
            // e1 the instantaneous |x|.
            const float c0 = 1.0f;
            float e1 = 0.0f, e2 = 0.0f, g = 1.0f;
            for (uint32_t i = 0; i < w; ++i) {
                const float ax = std::fabs(float(in[i]));
                e1 = c0 * ax + (1.0f - c0) * e1;
                const float ce = e1 > e2 ? ca : cr;
                e2 += ce * (e1 - e2);
                const float r = e2 > thr ? thr / e2 : 1.0f;
                const float cg = r < g ? ca : cr;
                g += cg * (r - g);
                out[i] = satf(float(in[i]) * g);
            }
            break;
        }
        case kCompressor: {
            const float thr = p[0];
            const float slope = float(1.0 - 1.0 / (p[1] < 1.0f ? 1.0 : double(p[1])));
            const float ca = float(dynCoef(p[2])), cr = float(dynCoef(p[3]));
            float ms = 0.0f, G = 1.0f;
            for (uint32_t i = 0; i < w; ++i) {
                const float x = float(in[i]) / 32768.0f;
                ms += 0.002f * (x * x - ms);
                float target = 1.0f;
                if (ms > 0.0f) {
                    const float L = 10.0f * std::log10(ms);
                    if (L > thr) target = std::pow(10.0f, (thr - L) * slope / 20.0f);
                }
                const float c = target < G ? ca : cr;
                G += c * (target - G);
                out[i] = satf(float(in[i]) * G);
            }
            break;
        }
        case kWavetableSmoother: {
            // Output window k (of C, W samples each) crossfades source windows
            // a = floor(k n / C) and a + 1; the weight of a falls from 1 by 1/m
            // per output window, m = the output windows mapped onto a.
            const int C = int(p[0]), W = int(p[1]) < 1 ? 1 : int(p[1]);
            const int ns = smootherSource(p, n);
            if (ns == C) {
                std::memcpy(out, in, w * sizeof(int16_t));
                break;
            }
            auto firstOf = [&](int a) { return (int64_t(a) * C + ns - 1) / ns; };
            for (int k = 0; k < C; ++k) {
                const int a = int(int64_t(k) * ns / C);
                const int64_t f0 = firstOf(a), f1 = firstOf(a + 1);
                const double mcount = double(f1 - f0 > 0 ? f1 - f0 : 1);
                const float wa = float(1.0 - double(k - f0) / mcount);
                for (int t = 0; t < W; ++t) {
                    const uint32_t o = uint32_t(k) * uint32_t(W) + uint32_t(t);
                    if (o >= w) break;
                    const float v = wa * at(in, n, int64_t(a) * W + t) + (1.0f - wa) * at(in, n, int64_t(a + 1) * W + t);
                    out[o] = satf(v);
                }
            }
            break;
        }
        case kStretchBeat: {
            // Segment length = len * g * 0.25 / (S + mu) samples (spec 18 §2.2).
            const double steps = double(p[1]) + double(p[2]);
            const double seg = double(n) * double(p[3]) * 0.25 / (steps > 0.0 ? steps : 1.0);
            return wsola(in, n, m, uint32_t(seg), out, cap);
        }
        case kStretchNote:
            // SPEC GAP: granule samples = granule ms * 44.1 (the stock
            // constants at bin:0x7a5a8/0x7a5ac are not given in the spec).
            return wsola(in, n, m, uint32_t(double(p[1]) * 44.1), out, cap);
        case kFadeIn:
            // Linear ramp a = 0 -> 1, y = trunc(x a).
            // SPEC GAP: start/step (lg:0x79608) not decoded; a = i / n.
            {
                const float inv = 1.0f / float(n);
                for (uint32_t i = 0; i < w; ++i)
                    out[i] = int16_t(truncMulDiv(in[i], int32_t(i), int32_t(n), float(in[i]) * (float(i) * inv)));
            }
            break;
        case kFadeOut:
            // SPEC GAP: ramp 1 -> 0 assumed (lg:0x79748); a = 1 - i / n.
            // y = trunc(x (n - i) / n) exactly (the last sample of a 1000-frame
            // fade of 10000 is 10, not 9.99...).
            {
                const float inv = 1.0f / float(n);
                for (uint32_t i = 0; i < w; ++i) {
                    const int32_t left = int32_t(n - i);
                    out[i] = int16_t(truncMulDiv(in[i], left, int32_t(n), float(in[i]) * (float(left) * inv)));
                }
            }
            break;
        default: std::memcpy(out, in, w * sizeof(int16_t)); break;
    }
    return w;
}

}  // namespace sfx
}  // namespace ot
