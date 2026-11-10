// Audio mappings and tables (spec 04 §7 summary; individual sections cited).
#include "audio/audio_tables.h"

#include <cmath>
#include <cstring>

// Const wavetable pitch / grain window tables on ARM (see audio_tables.h);
// OT_TABLES_RUNTIME (the table generator) computes them instead.
#if defined(__arm__) && !defined(OT_TABLES_RUNTIME)
#define OT_CONST_AUDIO_TABLES 1
#else
#define OT_CONST_AUDIO_TABLES 0
#endif

namespace ot {
namespace audio {

namespace {

constexpr float kPi = 3.14159265f;

bool gBuilt = false;
float gPitch[kPitchTableSize];
#if OT_CONST_AUDIO_TABLES
#include "audio/audio_tables_arm.inc"
const float (&gWtPitch)[kWavetableSizes][kPitchTableSize] = kWtPitchArm;
const uint16_t (&gWindow)[3][kGrainWindowLen] = kWindowArm;
#else
float gWtPitch[kWavetableSizes][kPitchTableSize];
uint16_t gWindow[3][kGrainWindowLen];
#endif
float gCutoff[kCutoffSteps];
float gVol[101];
float gTrk[101];
float gSend[101];

// RATE (spec 04 §4.8), steps per LFO cycle, 29 entries.
const float kRate[kRateCount] = {128.0f, 96.0f, 64.0f, 48.0f, 32.0f, 24.0f, 16.0f, 12.0f, 8.0f, 6.0f,
                                 4.0f, 3.0f, 2.0f, 1.5f, 1.0f, 0.75f, 0.5f, 0.375f, 1.0f / 3.0f, 0.25f,
                                 0.1875f, 1.0f / 6.0f, 0.125f, 1.0f / 12.0f, 0.0625f, 1.0f / 24.0f,
                                 1.0f / 32.0f, 1.0f / 48.0f, 1.0f / 64.0f};
// RATE_V (spec 04 §4.8), volume LFO, 24 entries.
const float kRateV[kRateVCount] = {24.0f, 16.0f, 12.0f, 8.0f, 6.0f, 4.0f, 3.0f, 2.0f, 1.5f, 1.0f,
                                   0.75f, 0.5f, 0.375f, 1.0f / 3.0f, 0.25f, 0.1875f, 1.0f / 6.0f,
                                   0.125f, 1.0f / 12.0f, 0.0625f, 1.0f / 24.0f, 1.0f / 32.0f,
                                   1.0f / 48.0f, 1.0f / 64.0f};
// DSYNC (spec 04 §5.1), delay sync rates in steps.
const float kDSync[kDelaySyncCount] = {8.0f, 6.0f, 4.0f, 3.0f, 2.0f, 1.5f, 1.0f, 0.75f, 0.5f, 0.375f,
                                       1.0f / 3.0f, 0.25f, 0.1875f, 1.0f / 6.0f, 0.125f, 1.0f / 12.0f,
                                       0.0625f, 1.0f / 24.0f, 1.0f / 32.0f};
// LIMTHR (spec 04 §5.3), 101 entries.
const uint16_t kLimThr[101] = {
    0,     7,     8,     9,     10,    11,    12,    13,    14,    15,    16,    18,    19,    21,
    23,    25,    27,    29,    32,    35,    38,    41,    45,    49,    53,    58,    63,    69,
    75,    81,    88,    96,    105,   114,   124,   135,   147,   160,   174,   189,   206,   224,
    244,   265,   288,   314,   342,   372,   404,   440,   479,   521,   567,   617,   672,   731,
    795,   865,   942,   1025,  1115,  1213,  1320,  1437,  1564,  1701,  1851,  2015,  2192,  2386,
    2596,  2825,  3074,  3345,  3640,  3961,  4311,  4691,  5105,  5555,  6045,  6578,  7158,  7789,
    8476,  9223,  10037, 10922, 11885, 12933, 14074, 15315, 16422, 18135, 19734, 21474, 23368, 25429,
    27671, 30112, 32767};

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }

void computeConstTables(float wtPitch[kWavetableSizes][kPitchTableSize], uint16_t window[3][kGrainWindowLen]) {
    // spec 04 §2.4: wt_rate_W[i] = rate[i] * (2^(1/4) * 440 * W / 44100), rate as gPitch.
    float pitch[kPitchTableSize];
    for (int i = 0; i < kPitchTableSize; ++i) pitch[i] = std::exp2(float(i - kUnityNote) / 12.0f) * 0.9996f;
    for (int k = 0; k < kWavetableSizes; ++k) {
        const float w = float(32u << k);
        const float scale = std::exp2(0.25f) * 440.0f * w / 44100.0f;
        for (int i = 0; i < kPitchTableSize; ++i) wtPitch[k][i] = pitch[i] * scale;
    }
    // spec 04 §3.1 grain windows.
    for (int i = 0; i < kGrainWindowLen; ++i) {
        window[0][i] = 65535;
        const int tri = i < 500 ? i : (kGrainWindowLen - i);
        window[1][i] = uint16_t(clampi(int(float(tri) / 500.0f * 65535.0f), 0, 65535));
        window[2][i] = uint16_t(clampi(int(std::sin(kPi * float(i) / 1000.0f) * 65535.0f), 0, 65535));
    }
}

bool constTablesMatch() {
    static float wt[kWavetableSizes][kPitchTableSize];
    static uint16_t win[3][kGrainWindowLen];
    computeConstTables(wt, win);
    return std::memcmp(wt, gWtPitch, sizeof(wt)) == 0 && std::memcmp(win, gWindow, sizeof(win)) == 0;
}

void initTables() {
    if (gBuilt) return;
    // spec 04 §2.4: rate[i] = 2^((i-60)/12) * 0.9996.
    for (int i = 0; i < kPitchTableSize; ++i) gPitch[i] = std::exp2(float(i - kUnityNote) / 12.0f) * 0.9996f;
#if !OT_CONST_AUDIO_TABLES
    computeConstTables(gWtPitch, gWindow);
#endif
    // spec 04 §4.4: CUT[k] = 20 * 2^(k/50).
    for (int k = 0; k < kCutoffSteps; ++k) gCutoff[k] = 20.0f * std::exp2(float(k) / 50.0f);
    // spec 04 §4.2 VOL[], TRK[]; §5.5 SEND[].
    gVol[0] = 0.0f;
    gVol[1] = dbToGain(-24.0f);
    for (int i = 2; i <= 100; ++i) gVol[i] = dbToGain(float(i - 50) * 0.48f);
    gTrk[0] = 0.0f;
    gTrk[1] = dbToGain(-36.0f);
    for (int i = 2; i <= 85; ++i) gTrk[i] = dbToGain(-36.0f + 0.42f * float(i));
    gTrk[86] = 1.0f;
    for (int i = 87; i <= 100; ++i) gTrk[i] = dbToGain(0.12f + 0.42f * float(i - 86));
    gSend[0] = 0.0f;
    for (int i = 1; i <= 100; ++i) gSend[i] = dbToGain(-40.0f + 0.4f * float(i));
    gBuilt = true;
}

float pitchRate(int idx) { return gPitch[clampi(idx, 0, kPitchTableSize - 1)]; }
const float* pitchTable() { return gPitch; }

int wavetableSizeIndex(uint32_t w) {
    int k = 0;
    while (k < kWavetableSizes - 1 && (32u << (k + 1)) <= w) ++k;
    return k;
}
const float* wavetablePitchTable(uint32_t w) { return gWtPitch[wavetableSizeIndex(w)]; }

uint32_t glideSamples(uint32_t ms) { return uint32_t(float(ms) * 44.1f); }

uint32_t glideFxMs(uint8_t v) {
    if (v > 100) v = 100;
    if (v < 50) return uint32_t(v) * 1000u / 49u;
    return 1000u + uint32_t(v - 50) * 14000u / 50u;
}

float volGain(int v) { return gVol[clampi(v, 0, 100)]; }
float trkGain(int v) { return gTrk[clampi(v, 0, 100)]; }
float sendGain(int v) { return gSend[clampi(v, 0, 100)]; }

void panGains(int p, float& l, float& r) {
    p = clampi(p, 0, 100);
    if (p < 50) {
        l = 1.0f;
        r = float(p) / 50.0f;
    } else if (p == 50) {
        l = r = 1.0f;
    } else {
        l = float(100 - p) / 50.0f;
        r = 1.0f;
    }
}

float cutoffHz(float c) {
    c = clampf(c, 0.0f, 1.0f);
    const int k = clampi(int(c * 477.0f), 0, kCutoffSteps - 1);
    return clampf(gCutoff[k], 20.0f, 17647.06f);
}

float svfDamp(float resonance) {
    const float q = clampf(resonance + 0.7f, 0.7f, 5.0f);
    return 1.0f / q;
}

FiltCoef sampleFilterCoef(float hz, float resonance) {
    hz = clampf(hz, 20.0f, 0.45f * kSampleRate);
    const float g = std::tan(hz * kPi / kSampleRate);
    FiltCoef c;
    c.k = svfDamp(resonance);
    c.a1 = 1.0f / (1.0f + g * (g + c.k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
}

float overdrivePreGain(int od) {
    if (od <= 0) return 0.0f;
    od = clampi(od, 0, 100);
    return float((od + 16) * 8) / 256.0f;
}

uint16_t bitMask(int bits) {
    bits = clampi(bits, 4, 16);
    return uint16_t(0xFFFFu << (16 - bits));
}

float lfoRateSteps(int idx) { return kRate[clampi(idx, 0, kRateCount - 1)]; }
float volLfoRateSteps(int idx) { return kRateV[clampi(idx, 0, kRateVCount - 1)]; }

uint16_t envMsToCount(float ms) {
    if (!(ms > 0.0f)) ms = 0.0f;
    const float c = ms * 44.1176f;
    if (c > 600000.0f) return 65535;
    uint32_t n = (uint32_t(c) + 7u) >> 3;
    return n > 65535u ? 65535u : uint16_t(n);
}

const uint16_t* grainWindow(int shape) { return gWindow[clampi(shape, 0, 2)]; }

uint32_t delayTimeSamples(float ms) {
    ms = clampf(ms, 0.0f, float(kDelayMaxMs));
    const uint32_t t = uint32_t(ms * 44.1f);
    return t < 1 ? 1 : t;
}

float delaySyncMs(float bpm, int idx) {
    if (!(bpm > 0.0f)) bpm = 130.0f;
    const float ms = 15000.0f / bpm * kDSync[clampi(idx, 0, kDelaySyncCount - 1)];
    return ms > float(kDelayMaxMs) ? float(kDelayMaxMs) : ms;
}

uint8_t delayFeedbackK(int ui) {
    float fb = clampf(float(ui) / 100.0f, 0.0f, 0.99f);
    return uint8_t(fb * 255.0f);
}

// Delay one-pole low-pass (spec 07 Q9, correcting spec 04 §5.1's swapped
// sin/cos): w = 2 pi f / 44100, K = cos(w) / (1 + sin(w)),
// b = (int)((1 - K) * 0.5 * 1023) (b0 = b1), c = (int)(-K * 1023); the filter
// is y[n] = (x[n]*b + x[n-1]*b - y[n-1]*c) >> 10. Stable for 0..22050 Hz.
void delayFilterCoefs(float hz, int32_t& b, int32_t& c) {
    hz = clampf(hz, 100.0f, 20000.0f);
    const float w = 2.0f * kPi * hz / 44100.0f;
    const float k = std::cos(w) / (1.0f + std::sin(w));
    b = int32_t((1.0f - k) * 0.5f * 1023.0f);   // (int) truncates toward zero
    c = int32_t(-k * 1023.0f);
}

float limiterThreshold(int v) {
    v = clampi(v, 0, 32767);
    return float(kLimThr[(v * 100) / 32767]) / 32767.0f;
}

float limiterCoef(float ms, int mode) {
    if (mode == 1) return clampf(ms / 1000.0f, 0.0f, 1.0f);
    if (!(ms > 0.0f)) return 1.0f;
    return 1.0f - std::exp(-2.2f * 1000.0f / (ms * 44100.0f));
}

}  // namespace audio
}  // namespace ot
