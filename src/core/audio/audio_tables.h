// Audio engine constants and every UI-value -> DSP-value mapping (spec 04 §7).
//
// All mappings live here so the voice/effect code never hard-codes a law.
// Tables are built once by audio::initTables() (idempotent, not thread-safe;
// call it before the audio callback starts -- AudioEngine::init does).
// Single precision only (Cortex-M4F FPU): no double arithmetic.
#pragma once

#include <cstdint>

namespace ot {
namespace audio {

// --- clocking (spec 04 §1.1) ------------------------------------------------
constexpr float kSampleRate = 44117.647f;       // MCLK/256
constexpr uint32_t kBlock = 128;                // frames per block
constexpr int kVoices = 8;                      // one mono voice per track (§1.2)
// Sequencer advance per live block: 128 / 44122.65 s (spec 04 §1.1 step 1).
constexpr float kLiveBlockSeconds = 128.0f / 44122.6484f;
// Offline render: sequencer step and virtual µs clock per block (spec 04 §6.1).
constexpr float kRenderBlockSeconds = 0.0029013333f;
constexpr float kRenderBlockUs = 2901.333f;
constexpr float kLiveBlockUs = 128.0f * 1.0e6f / kSampleRate;

// --- pitch (spec 04 §2.4) ---------------------------------------------------
constexpr int kPitchTableSize = 120;
constexpr int kUnityNote = 60;
constexpr int kWavetableSizes = 7;              // W = 32 << k, k = 0..6
float pitchRate(int idx);                       // 2^((i-60)/12) * 0.9996, idx clamped 0..119
const float* pitchTable();                      // 120 entries
const float* wavetablePitchTable(uint32_t windowSize);  // per-W table (§2.4)
int wavetableSizeIndex(uint32_t windowSize);    // 0..6 (nearest lower power of two, clamped)
// Glide time in samples: uint(g * 44.1) (spec 04 §2.4).
uint32_t glideSamples(uint32_t ms);
// Step FX Glide (id 19) value 0..100 -> ms: 0..49 -> 0..1 s, 50..100 -> 1..15 s (spec 03 §3.6 help text).
uint32_t glideFxMs(uint8_t v);

// --- level laws (spec 04 §4.2, §5.5) ----------------------------------------
float dbToGain(float db);
float volGain(int v);       // VOL[]: 0 -> 0, 1 -> -24 dB, i -> (i-50)*0.48 dB
float trkGain(int v);       // TRK[]: mixer fader law, 86 = 0 dB, 100 = +6 dB
float sendGain(int v);      // SEND[]: 0 -> 0, i -> -40 + 0.4 i dB
// Linear balance pan law (spec 04 §4.3); p clamped 0..100.
void panGains(int p, float& l, float& r);

// --- filter (spec 04 §4.4) --------------------------------------------------
constexpr int kCutoffSteps = 478;               // k = 0..477
float cutoffHz(float c);                        // 20 * 2^(floor(c*477)/50), clamped 20..17647.06
float svfDamp(float resonance);                 // 1/q, q = r + 0.7 clamped 0.7..5
// Sample-voice filter coefficients (zero-delay-feedback SVF, Voice): g =
// tan(pi f / fs), k = svfDamp(r); a1 = 1 / (1 + g (g + k)), a2 = g a1,
// a3 = g a2. f clamped to 20 Hz .. 0.45 fs.
struct FiltCoef {
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, k = 1.0f / 0.7f;
};
FiltCoef sampleFilterCoef(float hz, float resonance);

// --- overdrive / bit depth (spec 04 §4.5, §4.6) ------------------------------
float overdrivePreGain(int od);                 // 0 -> 0 (off); (od+16)*8/256
uint16_t bitMask(int bits);                     // 0xFFFF << (16 - b), b clamped 4..16

// --- envelopes / LFO rates (spec 04 §4.7, §4.8) -----------------------------
constexpr int kRateCount = 29;                  // RATE (slots 1..5), steps per cycle
constexpr int kRateVCount = 24;                 // RATE_V (volume LFO)
float lfoRateSteps(int idx);
float volLfoRateSteps(int idx);
constexpr uint32_t kLfoUnitsPerStep = 576;      // sequencer phase units per step (§4.8)
// PJRC envelope counts: (ms * 44.1176 + 7) >> 3, saturated to 65535 (§4.7).
uint16_t envMsToCount(float ms);

// --- granular windows (spec 04 §3.1) ----------------------------------------
constexpr int kGrainWindowLen = 1000;
const uint16_t* grainWindow(int shape);         // 0 square, 1 triangle, 2 half-sine

// --- delay (spec 04 §5.1) ---------------------------------------------------
constexpr uint32_t kDelayLineLen = 262144;      // samples per line, mask 0x3FFFF
constexpr uint32_t kDelayMaxMs = 3500;
constexpr int kDelaySyncCount = 19;
uint32_t delayTimeSamples(float ms);            // int(min(ms,3500) * 44.1), >= 1
float delaySyncMs(float bpm, int idx);          // 15000/BPM * DSYNC[idx], clamped 3500
uint8_t delayFeedbackK(int ui);                 // int(min(ui/100, 0.99) * 255)
// One-pole low-pass coefficients in Q10 (spec 07 Q9): b = b0 = b1, c = stored
// feedback coefficient; y = (x*b + x1*b - y1*c) >> 10.
void delayFilterCoefs(float hz, int32_t& b, int32_t& c);

// --- reverb (spec 04 §5.2) --------------------------------------------------
constexpr uint32_t kReverbRingLen = 16384;      // two rings, mask 0x3FFF
constexpr float kReverbDefaultAmount = 0.8f;
constexpr float kReverbDefaultGain = 0.1f;

// --- limiter (spec 04 §5.3) -------------------------------------------------
float limiterThreshold(int v);                  // project value 0..32767 -> LIMTHR / 32767
float limiterCoef(float ms, int mode);          // mode 0: 1-exp(-2.2*1000/(t*44100)); 1: t/1000

// --- master saturation (spec 04 §5.4) ---------------------------------------
// Output shaper, result clamped to [-1, 1]. Inline so the per-sample master
// loop folds the mode switch (engine.cpp instantiates one loop per mode).
inline float saturate(float u, int mode) {
    const float a = u < 0.0f ? -u : u;
    const float s = u < 0.0f ? -1.0f : 1.0f;
    switch (mode) {
        case 1: {
            // u - 0.1488 u^3; the cubic peaks at |u| = 1.497 (0.998) -- clamp the
            // argument there so large sums do not fold back (OpenTracker choice).
            const float c = u < -1.497f ? -1.497f : (u > 1.497f ? 1.497f : u);
            u = c - 0.1488f * c * c * c;
            break;
        }
        case 2:
            u = a < 1.0f ? 1.5f * u - 0.5f * u * u * u : s;
            break;
        case 3:
            if (a < 1.0f / 3.0f) u = 2.0f * u;
            else if (a <= 2.0f / 3.0f) {
                const float t = 2.0f - 3.0f * a;
                u = s * (3.0f - t * t) / 3.0f;
            } else u = s;
            break;
        case 4:
            if (a < 0.25f) u = 3.0f * u;
            else if (a <= 0.5f) {
                const float t = 2.0f - 4.0f * a;
                u = s * (1.0f - t * t * t / 4.0f);
            } else u = s;
            break;
        default:
            break;
    }
    // Clamp to [-1, 1]; the in-range test on the bit pattern (|u| <= 1 <=>
    // (bits & 0x7FFFFFFF) <= 0x3F800000) saves two FPU compares per call.
    uint32_t b;
    __builtin_memcpy(&b, &u, 4);
    if ((b & 0x7FFFFFFFu) <= 0x3F800000u) return u;
    return u < -1.0f ? -1.0f : (u > 1.0f ? 1.0f : u);
}

// Builds the tables (pitch, wavetable pitch, cutoff, level laws, windows).
void initTables();

// The wavetable pitch tables (§2.4) and grain windows (§3.1) are constant,
// but their values depend on the libm (exp2f / sinf differ in the last bit
// between newlib and glibc). ARM builds use const copies in flash computed
// with the device toolchain's newlib (audio_tables_arm.inc, regenerated by
// make -C tests audio-tables; checked against computeConstTables() by the
// M4 bench); other builds compute them in initTables().
void computeConstTables(float wtPitch[kWavetableSizes][kPitchTableSize], uint16_t window[3][kGrainWindowLen]);
// True when the tables in use equal computeConstTables() bit for bit.
bool constTablesMatch();

}  // namespace audio
}  // namespace ot
