// Per-track 4-band parametric EQ (see track_eq.h).
#include "audio/track_eq.h"

#include <cmath>

#include "fastrun.h"

namespace ot {
namespace audio {

namespace {
constexpr float kTwoPi = 6.28318530718f;
constexpr int kMaxDetents = 64;   // one encoder event is never more than this

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
int clampDetents(int d) { return clampi(d, -kMaxDetents, kMaxDetents); }
}  // namespace

void TrackEqSet::setDefault() {
    for (auto& t : track)
        for (int i = 0; i < kEqBands; ++i) t.band[i] = eqDefaultBand(i);
}

const char* eqTypeName(uint8_t type) {
    static const char* const kNames[kEqTypeCount] = {"Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch"};
    return type < kEqTypeCount ? kNames[type] : "?";
}

EqBand eqDefaultBand(int index) {
    EqBand b;
    switch (index) {
        case 0: b.type = kEqLowCut; b.freqHz = 80; b.q100 = 71; break;
        case 1: b.type = kEqPeak; b.freqHz = 500; b.q100 = 100; break;
        case 2: b.type = kEqPeak; b.freqHz = 2500; b.q100 = 100; break;
        default: b.type = kEqHighCut; b.freqHz = 12000; b.q100 = 71; break;
    }
    b.on = false;
    b.gainDb10 = 0;
    return b;
}

void eqSanitize(EqBand& b) {
    if (b.type >= kEqTypeCount) b.type = kEqPeak;
    b.freqHz = uint16_t(clampi(b.freqHz, kEqFreqMin, kEqFreqMax));
    b.gainDb10 = int16_t(clampi(b.gainDb10, -kEqGainMax, kEqGainMax));
    b.q100 = uint16_t(clampi(b.q100, kEqQMin, kEqQMax));
    if (b.type != kEqLowCut && b.type != kEqHighCut) b.slope = 0;
    else if (b.slope > 1) b.slope = 1;
}

uint16_t eqStepFreq(uint16_t hz, int d, bool coarse) {
    if (d == 0) return hz;
    d = clampDetents(d);
    const float octaves = float(d) * (coarse ? 1.0f / 6.0f : 1.0f / 24.0f);
    int v = int(std::lround(float(hz) * std::exp2(octaves)));
    if (d > 0 && v <= int(hz)) v = int(hz) + 1;
    if (d < 0 && v >= int(hz)) v = int(hz) - 1;
    return uint16_t(clampi(v, kEqFreqMin, kEqFreqMax));
}

int16_t eqStepGain(int16_t gainDb10, int d, bool coarse) {
    d = clampDetents(d);
    const int v = int(gainDb10) + d * (coarse ? 30 : 5);
    return int16_t(clampi(v, -kEqGainMax, kEqGainMax));
}

uint16_t eqStepQ(uint16_t q100, int d, bool coarse) {
    if (d == 0) return q100;
    d = clampDetents(d);
    const float f = std::pow(coarse ? 1.5f : 1.06f, float(d));
    int v = int(std::lround(float(q100) * f));
    if (d > 0 && v <= int(q100)) v = int(q100) + 1;
    if (d < 0 && v >= int(q100)) v = int(q100) - 1;
    return uint16_t(clampi(v, kEqQMin, kEqQMax));
}

uint8_t eqStepType(uint8_t type, int d) {
    int v = (int(type) + d) % int(kEqTypeCount);
    if (v < 0) v += int(kEqTypeCount);
    return uint8_t(v);
}

static void biquad(const EqBand& b, float q, float c[5]) {
    const float fs = kSampleRate;
    float f = float(b.freqHz);
    if (f > 0.45f * fs) f = 0.45f * fs;
    const float w0 = kTwoPi * f / fs;
    const float cw = std::cos(w0), sw = std::sin(w0);
    const float alpha = sw / (2.0f * q);
    const float A = std::pow(10.0f, float(b.gainDb10) * (1.0f / 400.0f));   // 10^(dB/40)
    float b0, b1, b2, a0, a1, a2;
    switch (b.type) {
        case kEqLowShelf: {
            const float sa = 2.0f * std::sqrt(A) * alpha;
            b0 = A * ((A + 1.0f) - (A - 1.0f) * cw + sa);
            b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw);
            b2 = A * ((A + 1.0f) - (A - 1.0f) * cw - sa);
            a0 = (A + 1.0f) + (A - 1.0f) * cw + sa;
            a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cw);
            a2 = (A + 1.0f) + (A - 1.0f) * cw - sa;
            break;
        }
        case kEqHighShelf: {
            const float sa = 2.0f * std::sqrt(A) * alpha;
            b0 = A * ((A + 1.0f) + (A - 1.0f) * cw + sa);
            b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw);
            b2 = A * ((A + 1.0f) + (A - 1.0f) * cw - sa);
            a0 = (A + 1.0f) - (A - 1.0f) * cw + sa;
            a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cw);
            a2 = (A + 1.0f) - (A - 1.0f) * cw - sa;
            break;
        }
        case kEqLowCut:   // high-pass
            b0 = (1.0f + cw) * 0.5f;
            b1 = -(1.0f + cw);
            b2 = b0;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cw;
            a2 = 1.0f - alpha;
            break;
        case kEqHighCut:   // low-pass
            b0 = (1.0f - cw) * 0.5f;
            b1 = 1.0f - cw;
            b2 = b0;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cw;
            a2 = 1.0f - alpha;
            break;
        case kEqNotch:
            b0 = 1.0f;
            b1 = -2.0f * cw;
            b2 = 1.0f;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cw;
            a2 = 1.0f - alpha;
            break;
        default:   // kEqPeak
            b0 = 1.0f + alpha * A;
            b1 = -2.0f * cw;
            b2 = 1.0f - alpha * A;
            a0 = 1.0f + alpha / A;
            a1 = -2.0f * cw;
            a2 = 1.0f - alpha / A;
            break;
    }
    const float inv = 1.0f / a0;
    c[0] = b0 * inv;
    c[1] = b1 * inv;
    c[2] = b2 * inv;
    c[3] = a1 * inv;
    c[4] = a2 * inv;
}

void eqCoefficients(const EqBand& in, float c[5]) {
    EqBand b = in;
    eqSanitize(b);
    biquad(b, float(b.q100) * 0.01f, c);
}

void eqStageCoefficients(const EqBand& in, int stage, float c[5]) {
    EqBand b = in;
    eqSanitize(b);
    if (eqStageCount(b) == 1) {
        biquad(b, float(b.q100) * 0.01f, c);
        return;
    }
    // Pole Q of an 8th-order Butterworth: 1 / (2 cos((2k - 1) pi / 16)), k = 1..4.
    static const float kButter8[4] = {0.50979558f, 0.60134489f, 0.89997622f, 2.56291545f};
    // Only the resonant stage (the last) follows the Q control, by the square root of
    // its ratio to Butterworth, so the peak stays comparable to a 12 dB cut's.
    const int k = stage < 0 ? 0 : stage > 3 ? 3 : stage;
    float q = kButter8[k];
    if (k == 3) q *= std::sqrt(float(b.q100) * 0.01f / 0.70710678f);
    biquad(b, q, c);
}

bool eqBandActive(const EqBand& b) {
    return !eqUsesGain(b.type) || b.gainDb10 != 0;
}

float eqResponseDb(const TrackEqParams& p, float hz) {
    const float w = kTwoPi * hz / kSampleRate;
    const float c1 = std::cos(w), s1 = std::sin(w);
    const float c2 = std::cos(2.0f * w), s2 = std::sin(2.0f * w);
    float db = 0.0f;
    for (int i = 0; i < kEqBands; ++i) {
        const EqBand& b = p.band[i];
        if (!b.on || !eqBandActive(b)) continue;
        EqBand nb = b;
        eqSanitize(nb);
        if (!eqSlopeAllowed(i, nb.type)) nb.slope = 0;
        for (int k = 0; k < eqStageCount(nb); ++k) {
            float c[5];
            eqStageCoefficients(nb, k, c);
            const float nr = c[0] + c[1] * c1 + c[2] * c2, ni = -(c[1] * s1 + c[2] * s2);
            const float dr = 1.0f + c[3] * c1 + c[4] * c2, di = -(c[3] * s1 + c[4] * s2);
            const float num = nr * nr + ni * ni, den = dr * dr + di * di;
            if (num <= 1e-20f || den <= 1e-20f) continue;
            db += 10.0f * std::log10(num / den);
        }
    }
    return db;
}

void TrackEq::set(const TrackEqParams& p) {
    // First stage and stage capacity of each band: the cuts of bands 1 and 4 may use four.
    static const uint8_t kFirst[kEqBands] = {0, 4, 5, 6};
    static const uint8_t kMax[kEqBands] = {4, 1, 1, 4};
    for (int i = 0; i < kEqBands; ++i) {
        EqBand nb = p.band[i];
        eqSanitize(nb);
        if (!eqSlopeAllowed(i, nb.type)) nb.slope = 0;
        const bool want = nb.on && eqBandActive(nb);
        const int ns = want ? eqStageCount(nb) : 0;
        for (int j = 0; j < kMax[i]; ++j) {
            Stage& s = st_[kFirst[i] + j];
            if (j >= ns) {
                if (s.live) {
                    s.live = false;
                    s.z1 = s.z2 = 0.0f;
                    --n_;
                }
                continue;
            }
            if (s.live && s.cfg == nb) continue;
            if (!s.live) {
                s.live = true;
                s.z1 = s.z2 = 0.0f;
                ++n_;
            }
            s.cfg = nb;
            eqStageCoefficients(nb, j, s.c);
        }
    }
}

void TrackEq::reset() {
    for (auto& s : st_) s.z1 = s.z2 = 0.0f;
}

OT_FASTRUN_FN void TrackEq::process(float* x, uint32_t n) {
    for (auto& s : st_) {
        if (!s.live) continue;
        const float b0 = s.c[0], b1 = s.c[1], b2 = s.c[2], a1 = s.c[3], a2 = s.c[4];
        float z1 = s.z1, z2 = s.z2;
        for (uint32_t i = 0; i < n; ++i) {
            const float in = x[i];
            const float out = b0 * in + z1;
            z1 = b1 * in - a1 * out + z2;
            z2 = b2 * in - a2 * out;
            x[i] = out;
        }
        // Keep decaying tails out of the denormal range.
        if (std::fabs(z1) < 1.0e-18f) z1 = 0.0f;
        if (std::fabs(z2) < 1.0e-18f) z2 = 0.0f;
        s.z1 = z1;
        s.z2 = z2;
    }
}

}  // namespace audio
}  // namespace ot
