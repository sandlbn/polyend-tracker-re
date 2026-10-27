// FAT: unison "super saw" synth engine (Tracker Mini engine id 2; spec 21
// §8.3 parameter table; docs/specs/2x-07-fat-engine.md) [OT].
//
// Clean-room DSP of our own design that follows the FAT parameter set
// (budget: <= 12k M4 instructions per voice per block for the Init patch,
// <= 22k worst case, no oversampling, block-rate modulation, no tables):
//
//  - Oscillator stack: up to 7 polyBLEP saws, a centre one and 3 detuned
//    pairs (Fatness: detune spread and pair levels, power-normalised); every
//    saw morphs towards a pulse (Timbre: saw(ph) - T saw(ph + 1/2)). A
//    one-pole low pass (Brightness) and white noise (Noise) feed the filter.
//  - Filter: LP MG 24 / LP OB 24 / LP OB 12, the TPT state-variable stages
//    of VAP's filter types 0..2; cutoff from Cutoff x Filter Env x LFO.
//  - Amp Env, Filter Env (Env 1), one LFO (stack pitch, cutoff) and patch
//    glide run at block rate; destinations ramp linearly across the block.
//
// Engine contract (audio/synth_engine.h): Volume (index 24) is never applied
// here; output is mono (L = R).
#pragma once

#include "audio/synth_engine.h"

namespace ot {
namespace audio {
namespace fat {

// Parameter indices (spec 21 §8.3).
enum Param : int {
    kFatness = 0,
    kBrightness,
    kOscLfo,       // Osc LFO Amt (vibrato)
    kTimbre,
    kNoise,
    kReset,
    kFilterType,
    kCutoff,
    kResonance,
    kFilterLfo,    // Filter LFO Amt
    kFilterEnv,    // Filter Env Amt
    kAmpA,
    kAmpD,
    kAmpS,
    kAmpR,
    kEnvA,         // Env 1 ("Filter Env")
    kEnvD,
    kEnvS,
    kEnvR,
    kLfoHz,
    kLfoWave,
    kLfoRetrig,
    kGlideMode,
    kGlideTime,
    kVolume,
    kParamCount   // 25
};
// [OT] Extension parameters after the Mini's (synth_params.h, patch field 16):
// Width (slot level, SynthBank) and Stereo (here: the stack's pairs split
// between L and R, 0 = mono). Read only when SynthParams::count covers them.
constexpr int kWidthParam = kParamCount;
constexpr int kStereoParam = kParamCount + 1;

enum FilterType : int { kLpMg24 = 0, kLpOb24 = 1, kLpOb12 = 2 };
enum LfoWave : int { kTri = 0, kSine, kRampUp, kRampDown, kSquare, kSampleHold };
enum LfoRetrig : int { kFree = 0, kOneShot = 1, kNote = 2 };
enum GlideMode : int { kAlways = 0, kOverlap = 1, kLegato = 2, kLegatoOverlap = 3 };

ISynthEngine* engine();

}  // namespace fat
}  // namespace audio
}  // namespace ot
