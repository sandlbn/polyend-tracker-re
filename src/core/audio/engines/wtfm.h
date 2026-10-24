// WTFM: 2-operator wavetable FM synth engine (Tracker Mini engine id 5; spec 21
// §8.5, docs/specs/2x-08-wtfm-engine.md) [OT].
//
// Clean-room DSP of our own design that follows the WTFM parameter set
// (budget: <= 12k M4 instructions per voice per block for the Init patch,
// <= 22k worst case, block-rate modulation, no stored tables):
//
//  - 2 operators; operator 2 phase-modulates operator 1 (the carrier). Each
//    operator also modulates itself (Feedback 1 / 2) and operator 2 feeds the
//    carrier's feedback node (Feedback 2>1); the feedback paths use the
//    two-sample average of the operator's output.
//  - Each operator reads a "Character" row: Smoother / Sharper / Wilder morph
//    triangle -> saw -> square (a cosine through a phase-distortion warp,
//    Wilder folded); Add 1 / 2 / 3 / 5 / 7 / 11 add the stride-N partial
//    series to a sine (self phase modulation at N times the frequency).
//    Shape moves along the row. Rows are closed forms, band limited per block
//    from the operator's pitch.
//  - Oversampling runs the carrier at 2 fs.
//  - Filter (15 types), Amp Env, Env 1 (filter), Env 2 (aux), 2 LFOs, the
//    6-slot mod matrix and glide follow VAP (vap.cpp).
//
// Engine contract (audio/synth_engine.h): Volume (index 42) is never applied
// here; output is mono (L = R).
#pragma once

#include "audio/synth_engine.h"

namespace ot {
namespace audio {
namespace wtfm {

// Parameter indices (spec 21 §8.5).
enum Param : int {
    kRatio1 = 0, kRatio2, kFine1, kFine2, kShape1, kShape2,
    kOversample = 8, kChar1, kChar2, kFm, kFb1, kFb2, kFb21,
    kFilterType = 15, kCutoff, kResonance, kFilterEnvAmt, kNoteTrack,
    kAmpA = 20, kAmpD, kAmpS, kAmpR,
    kEnv1A = 24, kEnv1D, kEnv1S, kEnv1R,
    kEnv2A = 28, kEnv2D, kEnv2S, kEnv2R,
    kLfo1Ratio = 32, kLfo1Wave, kLfo1Retrig, kLfo1Sync,
    kLfo2Ratio = 36, kLfo2Wave, kLfo2Retrig, kLfo2Sync,
    kGlideMode = 40, kGlideTime, kVolume,
    kModBase = 43,   // slot k: Src 43+3k, Dest 44+3k, Amt 45+3k
    kReset1 = 61, kReset2,
    kParamCount = 63,
};

// Character rows (enum 9).
enum Character : int { kSmoother = 0, kSharper, kWilder, kAdd1, kAdd2, kAdd3, kAdd5, kAdd7, kAdd11, kCharCount };

// Mod matrix sources / destinations (ui/screen_synth_tables.cpp kWtfmDest).
enum ModSrc : int { kSrcAmpEnv = 0, kSrcEnv1, kSrcEnv2, kSrcLfo1, kSrcLfo2, kSrcCount };
enum ModDest : int {
    kDstPitch1 = 0, kDstPitch2, kDstShape1, kDstShape2, kDstFb1, kDstFb2, kDstFb21, kDstFm, kDstCutoff,
    kDstAmpSus, kDstEnv1Sus, kDstEnv2Sus, kDstVolume, kDstEnv1Decay, kDstCount
};

// Ratio enum index 0..47 -> frequency ratio 0.25 .. 12.00 (spec 2x-08 §1).
inline float ratioOf(int index) { return 0.25f * float((index < 0 ? 0 : (index > 47 ? 47 : index)) + 1); }

ISynthEngine* engine();

// Tempo for LFO Sync (as vap::setTempo).
void setTempo(float bpm);

}  // namespace wtfm
}  // namespace audio
}  // namespace ot
