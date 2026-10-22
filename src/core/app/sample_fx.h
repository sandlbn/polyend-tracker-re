// Sample Editor offline effects (spec 06 §6.4 table: 17 effects, parameter
// ranges, steps and defaults; spec 05 §2.10). Pure functions on mono int16
// buffers; stereo samples are processed per planar channel by the caller.
//
// The processing follows spec 18 §2 (stock 1.9.2 effect objects): formulas,
// gain staging, truncation + int16 saturation and length changes (Delay tail,
// wavetable smoother, time-stretch). Items spec 18 §2.3 leaves open are marked
// `SPEC GAP:` in sample_fx.cpp (EQ coefficients, flanger tap, time-stretch
// target length and window sizes, fade endpoints, overdrive type 0 above
// 2/3 FS, limiter pre-smoothing).
#pragma once

#include <cstdint>

#include "audio/track_eq.h"

namespace ot {
namespace sfx {

enum Effect : uint8_t {
    kNormalizer = 0,
    kCrop,
    kReverse,
    kAmplifier,
    kOverdrive,
    kDelay,
    kBitcrusher,
    kChorus,
    kFlanger,
    kEqualizer,
    kEq4,           // OpenTracker: the 4-band track EQ, offline (after Equalizer)
    kLimiter,
    kCompressor,
    kWavetableSmoother,
    kStretchBeat,
    kStretchNote,
    kFadeIn,
    kFadeOut,
    kEffectCount = 18,
};

// 24 = the EQ4 effect: four bands of six values (kEq4Fields). Every other
// effect has at most 9.
constexpr int kMaxParams = 24;
constexpr int kEq4Fields = 6;   // per band: on, type, freq Hz, gain dB, slope, Q
constexpr uint32_t kMaxFrames = 0x1E0000;   // spec 06 §6.1/§6.3 length limit

// Display formats for parameter values.
enum Fmt : uint8_t {
    kFmtInt,        // "%d"
    kFmtFloat2,     // "%.2f"
    kFmtFloat1,     // "%.1f"
    kFmtPercent,    // value is percent: "%d%%"
    kFmtX100,       // shown x100 as percent
    kFmtMs,         // "%d ms"
    kFmtMsF,        // "%.1f ms"
    kFmtHz,         // "%d Hz"
    kFmtHzF,        // "%.1f Hz"
    kFmtDb,         // "%.1f dB" / "%d dB"
    kFmtSamplesMs,  // samples shown in ms (x 1000 / 44100)
    kFmtQuarter,    // shown x0.25
    kFmtSeconds,    // "%.2f s"
    kFmtBpm,        // "%d BPM"
    kFmtLabel,      // labels[]
};

struct ParamDesc {
    const char* name;
    float min, max, step, def;
    Fmt fmt;
    const char* const* labels;   // kFmtLabel only
};

struct EffectDesc {
    const char* name;
    int params;
    ParamDesc p[kMaxParams];
};

const EffectDesc& effect(int i);
// EQ4 parameter block: band b occupies p[6b .. 6b+5] = on (0/1), type
// (audio::EqType), freq (Hz), gain (dB), slope (0 = 12, 1 = 48 dB/oct), Q.
// Unpack clamps everything and drops a slope the band cannot have.
void eq4Pack(const audio::TrackEqParams& e, float* p);
void eq4Unpack(const float* p, audio::TrackEqParams* e);

// Default parameter table (spec 06 §6.1 "17 x 9 default table"; EQ4 adds 24).
void defaults(int fx, float* params);
float clampParam(int fx, int i, float v);
void formatParam(int fx, int i, float v, char* out, int cap);

// Frames produced for a selection of n frames (time-stretch and the
// wavetable smoother change the length; every other effect keeps it).
uint32_t outputFrames(int fx, const float* params, uint32_t n);
// Process n input frames into out (capacity cap); returns frames written
// (== outputFrames unless cap is smaller). in and out must not overlap.
// `progress` (optional) is called now and then with (ctx, frames done, frames
// total) while a long effect runs (EQ4); the other effects are quick.
using ProgressFn = void (*)(void* ctx, uint32_t done, uint32_t total);
uint32_t process(int fx, const float* params, const int16_t* in, uint32_t n, int16_t* out, uint32_t cap,
                 ProgressFn progress = nullptr, void* ctx = nullptr);

}  // namespace sfx
}  // namespace ot
