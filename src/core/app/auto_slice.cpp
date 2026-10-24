// Auto-slice transient detector (spec 13 §F, exact algorithm).
#include "app/auto_slice.h"

#include <cmath>
#include <cstring>

#include "app/sample_fx_ot.h"
#include "fmath.h"

namespace ot {

namespace {
double bitsToDouble(uint64_t b) {
    double d;
    std::memcpy(&d, &b, sizeof(d));
    return d;
}

// Sum of squares of x[0..n) as u64.
uint64_t sumSquares(const int16_t* x, uint32_t n) {
    uint64_t s = 0;
    for (uint32_t i = 0; i < n; ++i) s += uint64_t(int32_t(x[i]) * int32_t(x[i]));
    return s;
}
}  // namespace

float AutoSlicer::threshold(const uint32_t* e, int n) {
    // spec 13 §F.4: 64-bit sums, low 32 bits of mean and variance kept.
    uint64_t sum = 0;
    for (int i = 0; i < n; ++i) sum += e[i];
    const uint32_t mean = uint32_t(sum / uint64_t(n));
    uint64_t vs = 0;
    for (int i = 0; i < n; ++i) {
        const int64_t d = int64_t(e[i]) - int64_t(mean);
        vs += uint64_t(d * d);
    }
    const uint32_t var = uint32_t(vs / uint64_t(n));
    static const double kA = bitsToDouble(0x3FFA127913E81451ull);   // 1.62951...
    static const double kB = bitsToDouble(0xBDE5798EE2308C3Aull);   // -1.5625e-10
    double c = kA + kB * double(var);
    if (c < 1.2) c = 1.2;
    return float(mean) * float(c);
}

namespace {
// Spectral flux scratch (main loop only). N = 256, hop 128 (2.9 ms).
constexpr int kFluxN = 256, kFluxHop = 128, kFluxBins = kFluxN / 2 + 1;
constexpr int kMaxCand = 96;
constexpr uint32_t kFluxSpacing = 2048;   // 46 ms
struct FluxScratch {
    float win[kFluxN];
    bool winReady;
    float re[kFluxN], im[kFluxN];
    float prev[kFluxBins];
    uint32_t candPos[kMaxCand];
    float candVal[kMaxCand];
};
FluxScratch gFlux;
}  // namespace

bool AutoSlicer::start(const int16_t* data, uint32_t len, uint16_t* out, uint8_t* outCount) {
    if (!data || !out) return false;
    if (len < (mode_ == kFlux ? 4096u : kMinLength)) return false;   // §F.2 (Breaks: 93 ms)
    data_ = data;
    len_ = len;
    out_ = out;
    outCount_ = outCount;
    active_ = true;
    pos_ = 0;
    last_ = 0;
    count_ = 0;
    idx_ = 0;
    final_ = false;
    for (uint32_t& r : ring_) r = 0;
    fluxN_ = 0;
    fluxPeak_ = 0;
    lastCand_ = 0;
    nCand_ = 0;
    for (float& h : fluxHist_) h = 0;
    for (float& p : gFlux.prev) p = 0;
    return true;
}

// One flux frame per hop; returns busy. Up to 64 frames (8192 samples) per call.
bool AutoSlicer::fluxStep() {
    for (int batch = 0; batch < 64; ++batch) {
        const uint32_t f0 = fluxN_ * kFluxHop;
        if (f0 + kFluxN > len_) {
            fluxFinish();
            return false;
        }
        if (!gFlux.winReady) {   // Hann, once (double maths is slow on the M4F)
            for (int i = 0; i < kFluxN; ++i)
                gFlux.win[i] = 0.5f - 0.5f * float(fm::cosD(2.0 * 3.14159265358979 * (i + 0.5) / kFluxN));
            gFlux.winReady = true;
        }
        for (int i = 0; i < kFluxN; ++i) {
            gFlux.re[i] = float(data_[f0 + uint32_t(i)]) * gFlux.win[i];
            gFlux.im[i] = 0;
        }
        sfx::fftInPlace(gFlux.re, gFlux.im, kFluxN, false);
        // Half-wave rectified difference of compressed magnitudes; bins from
        // ~170 Hz up, high bins weighted more (hats and snare cracks).
        float flux = 0;
        for (int k = 0; k < kFluxBins; ++k) {
            const float m = std::sqrt(std::sqrt(gFlux.re[k] * gFlux.re[k] + gFlux.im[k] * gFlux.im[k]));   // |X|^0.5
            const float d = m - gFlux.prev[k];
            gFlux.prev[k] = m;
            if (k >= 1 && d > 0) flux += d * (1.0f + float(k) / float(kFluxBins));
        }
        // Peak picking on frame t-2 with the two frames each side.
        for (int i = 0; i < 15; ++i) fluxHist_[i] = fluxHist_[i + 1];
        fluxHist_[15] = flux;
        if (flux > fluxPeak_) fluxPeak_ = flux;
        ++fluxN_;
        if (fluxN_ >= 5) {
            const float c = fluxHist_[13];
            bool peak = c > 0;
            for (int i = 11; i <= 15; ++i)
                if (i != 13 && fluxHist_[i] >= c) peak = false;
            float mean = 0;
            for (int i = 0; i < 16; ++i) mean += fluxHist_[i];
            mean /= 16.0f;
            if (peak && c > 1.5f * mean + 0.04f * fluxPeak_) {
                const uint32_t pos = fluxRefine((fluxN_ - 3) * kFluxHop);
                if (nCand_ > 0 && pos < lastCand_ + kFluxSpacing) {
                    if (c > gFlux.candVal[nCand_ - 1]) {   // keep the stronger of two close onsets
                        gFlux.candPos[nCand_ - 1] = pos;
                        gFlux.candVal[nCand_ - 1] = c;
                        lastCand_ = pos;
                    }
                } else if (nCand_ < kMaxCand) {
                    gFlux.candPos[nCand_] = pos;
                    gFlux.candVal[nCand_] = c;
                    ++nCand_;
                    lastCand_ = pos;
                }
            }
        }
    }
    return true;
}

// The onset inside the frame: the first 32-frame block whose energy jumps
// over twice the block before it, a block early (pre-roll).
uint32_t AutoSlicer::fluxRefine(uint32_t f0) const {
    constexpr uint32_t kB = 32;
    uint64_t prevE = 0;
    for (uint32_t b = 0; b * kB < kFluxN && f0 + (b + 1) * kB <= len_; ++b) {
        uint64_t e = 1;
        for (uint32_t i = 0; i < kB; ++i) {
            const int32_t x = data_[f0 + b * kB + i];
            e += uint64_t(x * x);
        }
        if (b > 0 && e > 2 * prevE && e > 2000 * kB) return f0 + (b > 1 ? (b - 1) : 0) * kB;
        prevE = e;
    }
    return f0 + kFluxN / 2;
}

void AutoSlicer::fluxFinish() {
    // Slice 0 at the sample start, then the strongest onsets up to 48.
    count_ = 0;
    out_[count_++] = 0;
    int idx[kMaxCand];
    int n = 0;
    for (int i = 0; i < nCand_; ++i)
        if (gFlux.candPos[i] >= kFluxSpacing) idx[n++] = i;   // the start is slice 0 already
    // Strongest first (insertion sort; n <= 96).
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && gFlux.candVal[idx[j]] > gFlux.candVal[idx[j - 1]]; --j) {
            const int t = idx[j];
            idx[j] = idx[j - 1];
            idx[j - 1] = t;
        }
    if (n > kMaxSlices - 1) n = kMaxSlices - 1;
    // Back into time order.
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && gFlux.candPos[idx[j]] < gFlux.candPos[idx[j - 1]]; --j) {
            const int t = idx[j];
            idx[j] = idx[j - 1];
            idx[j - 1] = t;
        }
    for (int i = 0; i < n; ++i) append(gFlux.candPos[idx[i]]);
    active_ = false;
    if (outCount_) *outCount_ = count_;
}

void AutoSlicer::append(uint32_t p) {
    // §F.5: u16((65535 / len) * p), float, truncated.
    const float f = (65535.0f / float(len_)) * float(p);
    out_[count_++] = uint16_t(f >= 65535.0f ? 65535.0f : f);
}

uint32_t AutoSlicer::refine(uint32_t p) const {
    // spec 13 §F.6. The window is clamped to the sample end (the stock
    // window may run up to ~6144 frames past it; §F.6 recommends clamping).
    uint32_t s, m;
    if (p < kHalfWindow) {
        s = 0;
        m = ((2 * kHalfWindow - p) >> 7) & 0xFFFF;
    } else if (p + kHalfWindow > len_) {
        s = p - kHalfWindow;
        m = ((p - len_ + 2 * kHalfWindow) >> 7) & 0xFFFF;
    } else {
        s = p - kHalfWindow;
        m = 96;
    }
    if (s < len_ && s + m * kSub > len_) m = (len_ - s) / kSub;
    if (m <= 5) return s;
    uint32_t e[96];
    if (m > 96) m = 96;
    for (uint32_t k = 0; k < m; ++k) e[k] = uint32_t(sumSquares(data_ + s + k * kSub, kSub) >> 7);
    const float thr = threshold(e, int(m));
    for (uint32_t k = 5; k < m; ++k)
        if (float(e[k]) > thr && (count_ == 0 || s + kSub * k > last_ + kSpacing)) return s + kSub * (k - 5);
    return s;
}

bool AutoSlicer::step() {
    if (!active_) return false;
    if (mode_ == kFlux) return fluxStep();
    // §F.3 block energy.
    uint32_t n = kBlock;
    if (len_ - pos_ < kBlock) {
        n = len_ - pos_;
        final_ = true;
    }
    // The stock sum covers all 2048 scratch entries (stale squares in a short
    // final block); that block is never tested, so only full blocks matter.
    const uint32_t energy = uint32_t(sumSquares(data_ + pos_, n) >> 11);
    // §F.5 detection, before the energy enters the ring.
    if (pos_ == kRing * kBlock) {
        const float thr = threshold(ring_, kRing);
        for (int i = 0; i < kRing; ++i) {
            const uint32_t bp = uint32_t(i) * kBlock;
            if (float(ring_[i]) > thr && (i == 0 || bp > last_ + kSpacing) && count_ < kMaxSlices) {
                last_ = bp;
                append(refine(bp));
            }
        }
        if (count_ == 0) append(0);
    } else if (pos_ > kRing * kBlock && !final_) {
        const float thr = threshold(ring_, kRing);
        if (float(energy) > thr && pos_ > last_ + kSpacing && count_ < kMaxSlices) {
            const uint32_t p = refine(pos_);
            last_ = p;
            append(p);
        }
    }
    ring_[idx_] = energy;
    idx_ = uint8_t((idx_ + 1) % kRing);
    pos_ += n;
    if (pos_ >= len_ || count_ >= kMaxSlices) {
        active_ = false;
        if (outCount_) *outCount_ = count_;
    }
    return active_;
}

}  // namespace ot
