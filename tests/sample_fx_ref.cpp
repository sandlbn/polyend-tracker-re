// The Sample Editor effects as they were before the speed work (upstream
// c6128eb src/core/app/sample_fx.cpp, namespace sfx -> sfx_ref): the reference
// the faster sample_fx.cpp is compared against (sample_fx_speed_test.cpp,
// tools/sfx_render). Test-only; not linked into the firmware.

#include "sample_fx_ref.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "fmath.h"

namespace ot {
namespace sfx_ref {

// The enums and descriptor types are sfx's (app/sample_fx.h).
using namespace ::ot::sfx;

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
// sub-sampled to at most 256 points per candidate.
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
        int64_t best = nominal;
        double bestC = -1e300;
        const int64_t lo = nominal - S < 0 ? 0 : nominal - S;
        const int64_t hi = nominal + S > last ? last : nominal + S;
        for (int64_t s = lo; s <= hi; ++s) {
            double c = 0.0;
            for (uint32_t i = 0; i < O; i += stride) {
                const double wt = double(O - i) * double(i);
                c += wt * double(at(in, n, nat + i)) * double(in[s + i]);
            }
            if (c > bestC) {
                bestC = c;
                best = s;
            }
        }
        for (uint32_t i = 0; i < N; ++i) {
            const uint64_t o = o0 + i;
            if (o >= w) break;
            double v = in[best + i];
            if (i < O) {
                const double a = double(i) / double(O);
                v = double(at(in, n, nat + i)) * (1.0 - a) + v * a;
            }
            out[o] = sat(v);
        }
        prev = best;
    }
    return w;
}

// Peaking biquad (direct form I, normalised samples).
// SPEC GAP: the stock coefficient formula (lg:0x791c0) is not decoded; this
// is the RBJ peaking EQ with Q = f / bw.
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
            const double g = peak ? double(p[0]) * 32767.0 / 100.0 / double(peak) : 1.0;
            for (uint32_t i = 0; i < w; ++i) out[i] = sat(double(in[i]) * g);
            break;
        }
        case kCrop: std::memcpy(out, in, w * sizeof(int16_t)); break;
        case kReverse:
            for (uint32_t i = 0; i < w; ++i) out[i] = in[n - 1 - i];
            break;
        case kAmplifier:
            for (uint32_t i = 0; i < w; ++i) out[i] = sat(double(in[i]) * double(p[0]));
            break;
        case kOverdrive: {
            const int type = clampi(int(p[1]), 0, 2);
            const double g = p[0];
            for (uint32_t i = 0; i < w; ++i) {
                const int x = in[i];
                const double ax = x < 0 ? -double(x) : double(x);
                double y;
                if (type == 0) {
                    // Piecewise soft clip on s = |x g| / FS.
                    // SPEC GAP: above 2/3 FS the stock behaviour is taken as a
                    // hard clip to FS (spec 18 §2.3 item 5).
                    const double s = ax * g / 32767.0;
                    if (s < 1.0 / 3.0) y = 2.0 * s;
                    else if (s < 2.0 / 3.0) y = (3.0 - (2.0 - 3.0 * s) * (2.0 - 3.0 * s)) / 3.0;
                    else y = 1.0;
                    y *= 32767.0;
                } else if (type == 1) {
                    const double k = 4.0 * g;
                    y = (1.0 - fm::expD(-k * ax / 32767.0)) * 32767.0;
                } else {
                    // The integer truncation is the binary's (spec 18 §2.2).
                    const double k = 10.0 * g;
                    const double t = double(int64_t(k * ax / 32767.0));   // >= 0, < 2^63
                    y = (1.0 - 1.0 / (1.0 + t)) * 32767.0;
                }
                out[i] = sat(x < 0 ? -y : y);
            }
            break;
        }
        case kDelay: {
            // Multi-tap echo: y[i] = sat(sum_{k=0..K} f^k x[i - kD]).
            uint32_t D;
            int K;
            delayTaps(p, &D, &K);
            float gains[99];
            gains[0] = 1.0f;
            for (int k = 1; k <= K; ++k) gains[k] = float(double(gains[k - 1]) * double(p[0]));
            for (uint32_t i = 0; i < w; ++i) {
                // Taps with 0 <= i - kD < n.
                const uint32_t kHi = i / D < uint32_t(K) ? i / D : uint32_t(K);
                const uint32_t kLo = i < n ? 0u : (i - n) / D + 1;
                float acc = 0.0f;
                for (uint32_t k = kLo; k <= kHi; ++k) acc += gains[k] * float(in[i - k * D]);
                out[i] = sat(acc);
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
            const double depth = double(int(p[2]));
            const uint32_t incr = uint32_t(double(p[3]) * 2147483648.0 / kRateExact);
            uint32_t phase = 0;
            for (uint32_t i = 0; i < w; ++i) {
                double d = double(off) + depth * double(std::sin(float(2.0 * kPi / 2147483648.0 * double(phase))));
                if (d < 0.0) d = 0.0;
                if (d > double(L - 1)) d = double(L - 1);
                const double pos = double(i) - d;
                const double fl = fm::floorD(pos);
                const double fr = pos - fl;
                const int64_t i0 = int64_t(fl);
                const double tap = double(at(in, n, i0)) * (1.0 - fr) + double(at(in, n, i0 + 1)) * fr;
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
            const double thr = fm::pow10D(double(p[0]) / 20.0) * 32767.0;
            const double ca = dynCoef(p[1]), cr = dynCoef(p[2]);
            // SPEC GAP: c0 (obj+0x74, set at ctor) is not decoded; 1.0 makes
            // e1 the instantaneous |x|.
            const double c0 = 1.0;
            double e1 = 0.0, e2 = 0.0, g = 1.0;
            for (uint32_t i = 0; i < w; ++i) {
                const double ax = std::fabs(double(in[i]));
                e1 = c0 * ax + (1.0 - c0) * e1;
                const double ce = e1 > e2 ? ca : cr;
                e2 = ce * e1 + (1.0 - ce) * e2;
                const double r = e2 > thr ? thr / e2 : 1.0;
                const double cg = r < g ? ca : cr;
                g = cg * r + (1.0 - cg) * g;
                out[i] = sat(double(in[i]) * g);
            }
            break;
        }
        case kCompressor: {
            const double thr = p[0];
            const double ratio = p[1] < 1.0f ? 1.0 : double(p[1]);
            const double ca = dynCoef(p[2]), cr = dynCoef(p[3]);
            double ms = 0.0, G = 1.0;
            for (uint32_t i = 0; i < w; ++i) {
                const double x = double(in[i]) / 32768.0;
                ms = 0.002 * x * x + 0.998 * ms;
                double target = 1.0;
                if (ms > 0.0) {
                    // Single-precision transcendental math (Cortex-M4F FPU).
                    const float L = 10.0f * std::log10(float(ms));
                    if (double(L) > thr)
                        target = double(std::pow(10.0f, float((thr - double(L)) * (1.0 - 1.0 / ratio)) / 20.0f));
                }
                const double c = target < G ? ca : cr;
                G = c * target + (1.0 - c) * G;
                out[i] = sat(double(in[i]) * G);
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
                const double wa = 1.0 - double(k - f0) / mcount;
                for (int t = 0; t < W; ++t) {
                    const uint32_t o = uint32_t(k) * uint32_t(W) + uint32_t(t);
                    if (o >= w) break;
                    const double v = wa * double(at(in, n, int64_t(a) * W + t)) +
                                     (1.0 - wa) * double(at(in, n, int64_t(a + 1) * W + t));
                    out[o] = sat(v);
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
            for (uint32_t i = 0; i < w; ++i) out[i] = sat(double(in[i]) * double(i) / double(n));
            break;
        case kFadeOut:
            // SPEC GAP: ramp 1 -> 0 assumed (lg:0x79748); a = 1 - i / n.
            for (uint32_t i = 0; i < w; ++i) out[i] = sat(double(in[i]) * (1.0 - double(i) / double(n)));
            break;
        default: std::memcpy(out, in, w * sizeof(int16_t)); break;
    }
    return w;
}

}  // namespace sfx_ref
}  // namespace ot
