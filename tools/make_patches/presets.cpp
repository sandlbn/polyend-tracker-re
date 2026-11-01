// OpenTracker synth preset pack (presets.h, PRESETS.md) [OT].
//
// Every sound here is our own design, built on the engine's built-in Init
// patch (patchInit) with the parameter semantics of our engines
// (opentracker-synths modules/{acd,fat,vap,wtfm,perc,ju6,dtype,cstype,atype,ttype}). 3.x: a model's
// presets are in the pack when its module is in the build profile (OT_MOD_<id>). Macro amounts follow the macro law of
// audio/synth_params.h: amount x stored range per unit of the macro, so a
// bipolar macro at 63 (and a unipolar one at 0) plays the patch as saved.
//
// Levels: the slot gain is not part of a patch; every preset is voiced for
// the default slot gain (-9 dB, SynthSlotRecord::defaults) and checked by
// tests/patches_test.cpp: audible, and no sample at full scale (VAP and the
// FAT presets for chords: 4-voice chords; PERC: every drum and five drums at
// once, toms alone).
#include "presets.h"

#include <cstdio>
#include <cstring>

#if defined(OT_MOD_ACD1)
#include "acd.h"
#endif
#if defined(OT_MOD_FAT1)
#include "fat.h"
#endif
#if defined(OT_MOD_PRC1)
#include "perc.h"
#endif
#if defined(OT_MOD_VAP1)
#include "vap.h"
#endif
#if defined(OT_MOD_WTF1)
#include "wtfm.h"
#endif
#if defined(OT_MOD_JU6S)
#include "ju6.h"
#endif
#if defined(OT_MOD_DTY1)
#include "dtype.h"
#endif
#if defined(OT_MOD_CST1)
#include "cstype.h"
#endif
#if defined(OT_MOD_ATY1)
#include "atype.h"
#endif
#if defined(OT_MOD_TTY1)
#include "ttype.h"
#endif
#include "audio/synth_params.h"
#include "modules/synth_models.h"
#include "formats/patch_pb.h"

namespace presets {

using ot::SynthModel;

namespace {

// Cutoff macro amount for +-hz (VAP / ACD cutoff range 20..20000 Hz).
constexpr float cut(float hz) { return hz / 19980.0f; }

#if defined(OT_MOD_ACD1)
// ---------------------------------------------------------------------------
// ACD: one oscillator (saw + pulse + sub + noise), LP SVF 12 / SVF 24 / RD3.

std::vector<Preset> acdPresets() {
    using namespace ot::audio::acd;
    const SynthModel A = SynthModel::ACD;
    return {
        {A, "OT Rubber Acid", "Round saw acid line: RD3 ladder, fast filter envelope, legato glide.", 1,
         {{kSawMix, 1.0f}, {kFilterType, float(kRd3)}, {kCutoff, 380.0f}, {kResonance, 1.1f}, {kNoteTrack, 0.5f},
          {kFilterEnv, 0.4f}, {kModD, 0.3f}, {kModS, 0.0f}, {kAmpA, 0.002f}, {kAmpD, 0.5f}, {kAmpS, 0.8f},
          {kAmpR, 0.08f}, {kGlideMode, float(kLegato)}, {kGlideTime, 0.08f}},
         {{"Cutoff", true, {{kCutoff, cut(600)}}},
          {"Resonance", true, {{kResonance, 0.25f}}},
          {"Env Amt", true, {{kFilterEnv, 0.2f}}},
          {"Decay", true, {{kModD, 0.03f}}},
          {"Glide", false, {{kGlideTime, 0.1f}}},
          {"Sub", false, {{kSubMix, 0.6f}}}}},
        {A, "OT Squelch Box", "Square-wave squelch: high ladder resonance, snappy envelope.", 1,
         {{kVolume, 0.75f}, {kSawMix, 0.0f}, {kSquareMix, 1.0f}, {kFilterType, float(kRd3)}, {kCutoff, 260.0f}, {kResonance, 1.3f},
          {kNoteTrack, 0.5f}, {kFilterEnv, 0.55f}, {kModD, 0.18f}, {kAmpA, 0.002f}, {kAmpD, 1.0f}, {kAmpS, 0.9f},
          {kAmpR, 0.06f}, {kLfoHz, 3.0f}, {kLfoWave, float(kTri)}, {kGlideMode, float(kLegatoOverlap)},
          {kGlideTime, 0.06f}},
         {{"Cutoff", true, {{kCutoff, cut(500)}}},
          {"Resonance", true, {{kResonance, 0.15f}}},
          {"Env Amt", true, {{kFilterEnv, 0.2f}}},
          {"Decay", true, {{kModD, 0.02f}}},
          {"Wobble", false, {{kFilterLfo, 0.15f}}},
          {"Glide", false, {{kGlideTime, 0.1f}}}}},
        {A, "OT Deep Sub", "Clean sub bass: triangle sub with a little square on top.", 1,
         {{kSawMix, 0.0f}, {kSquareMix, 0.25f}, {kSubMix, 1.0f}, {kSubMode, 2.0f}, {kFilterType, float(kSvf24)},
          {kCutoff, 220.0f}, {kResonance, 0.2f}, {kNoteTrack, 0.6f}, {kFilterEnv, 0.08f}, {kModD, 0.2f},
          {kAmpA, 0.004f}, {kAmpD, 1.0f}, {kAmpS, 1.0f}, {kAmpR, 0.15f}},
         {{"Tone", true, {{kCutoff, cut(400)}}},
          {"Body", false, {{kSquareMix, 0.5f}}},
          {"Punch", true, {{kFilterEnv, 0.2f}}},
          {"Release", true, {{kAmpR, 0.02f}}},
          {"Glide", false, {{kGlideTime, 0.1f}}},
          {"Grit", false, {{kNoiseMix, 0.15f}}}}},
        {A, "OT Rubber Bass", "Pulse + sub bass with a short filter bounce.", 1,
         {{kSawMix, 0.3f}, {kSquareMix, 0.8f}, {kPw, 0.35f}, {kSubMix, 0.5f}, {kFilterType, float(kSvf24)},
          {kCutoff, 450.0f}, {kResonance, 0.6f}, {kNoteTrack, 0.5f}, {kFilterEnv, 0.3f}, {kModD, 0.15f},
          {kAmpA, 0.002f}, {kAmpD, 0.4f}, {kAmpS, 0.6f}, {kAmpR, 0.1f}, {kGlideMode, float(kLegato)},
          {kGlideTime, 0.05f}},
         {{"Cutoff", true, {{kCutoff, cut(700)}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Env Amt", true, {{kFilterEnv, 0.2f}}},
          {"Decay", true, {{kModD, 0.02f}}},
          {"Width", true, {{kPw, 0.3f}}},
          {"Sub", true, {{kSubMix, 0.4f}}}}},
        {A, "OT Night Pluck", "Saw + square pluck, dark and short; good with delay.", 1,
         {{kVolume, 1.5f}, {kSawMix, 1.0f}, {kSquareMix, 0.5f}, {kFilterType, float(kSvf12)}, {kCutoff, 500.0f}, {kResonance, 0.4f},
          {kFilterEnv, 0.45f}, {kModD, 0.18f}, {kAmpA, 0.001f}, {kAmpD, 0.5f}, {kAmpS, 0.0f}, {kAmpR, 0.4f},
          {kLfoHz, 5.0f}},
         {{"Cutoff", true, {{kCutoff, cut(800)}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Env Amt", true, {{kFilterEnv, 0.2f}}},
          {"Decay", true, {{kAmpD, 0.04f}, {kModD, 0.015f}}},
          {"Release", true, {{kAmpR, 0.03f}}},
          {"Vibrato", false, {{kPitchLfo, 0.003f}}}}},
        {A, "OT Glass Pluck", "Narrow pulse pluck with a pulse-width sweep; bright and glassy.", 1,
         {{kSawMix, 0.0f}, {kSquareMix, 1.0f}, {kPw, 0.2f}, {kPwEnv, 0.5f}, {kModD, 0.25f},
          {kFilterType, float(kSvf12)}, {kCutoff, 1500.0f}, {kResonance, 0.8f}, {kFilterEnv, 0.25f},
          {kAmpA, 0.001f}, {kAmpD, 0.7f}, {kAmpS, 0.0f}, {kAmpR, 0.5f}},
         {{"Cutoff", true, {{kCutoff, cut(1000)}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Width", true, {{kPw, 0.25f}}},
          {"PW Sweep", true, {{kPwEnv, 0.2f}}},
          {"Decay", true, {{kAmpD, 0.05f}, {kModD, 0.02f}}},
          {"Release", true, {{kAmpR, 0.04f}}}}},
        {A, "OT Acid Lead", "Singing mono lead: saw + pulse, slow filter swell, vibrato, glide.", 1,
         {{kVolume, 1.5f}, {kSawMix, 1.0f}, {kSquareMix, 0.6f}, {kPw, 0.4f}, {kFilterType, float(kSvf24)}, {kCutoff, 1300.0f},
          {kResonance, 0.7f}, {kFilterEnv, 0.3f}, {kModA, 0.01f}, {kModD, 0.6f}, {kModS, 0.4f}, {kAmpA, 0.004f},
          {kAmpD, 0.5f}, {kAmpS, 1.0f}, {kAmpR, 0.25f}, {kPitchLfo, 0.012f}, {kLfoHz, 5.5f},
          {kLfoWave, float(kSine)}, {kLfoRetrig, float(kNote)}, {kGlideMode, float(kOverlap)},
          {kGlideTime, 0.07f}},
         {{"Cutoff", true, {{kCutoff, cut(1200)}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Env Amt", true, {{kFilterEnv, 0.2f}}},
          {"Vibrato", true, {{kPitchLfo, 0.003f}}},
          {"Glide", false, {{kGlideTime, 0.1f}}},
          {"Release", true, {{kAmpR, 0.03f}}}}},
        {A, "OT Wobble Acid", "Ladder bass with a triangle LFO on the cutoff (wobble).", 1,
         {{kSawMix, 1.0f}, {kFilterType, float(kRd3)}, {kCutoff, 300.0f}, {kResonance, 1.15f}, {kNoteTrack, 0.5f},
          {kFilterEnv, 0.2f}, {kModD, 0.4f}, {kFilterLfo, 0.35f}, {kLfoHz, 3.0f}, {kLfoWave, float(kTri)},
          {kLfoRetrig, float(kNote)}, {kAmpA, 0.002f}, {kAmpD, 1.0f}, {kAmpS, 1.0f}, {kAmpR, 0.1f}},
         {{"Cutoff", true, {{kCutoff, cut(500)}}},
          {"Resonance", true, {{kResonance, 0.15f}}},
          {"Wobble", true, {{kFilterLfo, 0.15f}}},
          {"Rate", true, {{kLfoHz, 0.02f}}},
          {"Env Amt", true, {{kFilterEnv, 0.2f}}},
          {"Square", false, {{kSquareMix, 0.8f}}}}},
        {A, "OT Hollow Bass", "Square bass with a two-octave sub; hollow and woody.", 1,
         {{kVolume, 0.85f}, {kSawMix, 0.0f}, {kSquareMix, 1.0f}, {kSubMix, 0.7f}, {kSubMode, 3.0f}, {kFilterType, float(kSvf24)},
          {kCutoff, 700.0f}, {kResonance, 0.3f}, {kNoteTrack, 0.4f}, {kFilterEnv, 0.2f}, {kModD, 0.25f},
          {kAmpA, 0.002f}, {kAmpD, 0.3f}, {kAmpS, 0.7f}, {kAmpR, 0.1f}},
         {{"Cutoff", true, {{kCutoff, cut(800)}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Width", true, {{kPw, 0.3f}}},
          {"Sub", true, {{kSubMix, 0.3f}}},
          {"Env Amt", true, {{kFilterEnv, 0.2f}}},
          {"Release", true, {{kAmpR, 0.02f}}}}},
        {A, "OT Bent Acid", "Dirty acid: pulse sweep, a touch of noise, glide on every note.", 1,
         {{kSawMix, 0.7f}, {kSquareMix, 0.5f}, {kPw, 0.3f}, {kPwEnv, -0.3f}, {kNoiseMix, 0.06f},
          {kFilterType, float(kRd3)}, {kCutoff, 320.0f}, {kResonance, 1.25f}, {kNoteTrack, 0.5f},
          {kFilterEnv, 0.5f}, {kModD, 0.22f}, {kAmpA, 0.002f}, {kAmpD, 1.0f}, {kAmpS, 0.85f}, {kAmpR, 0.08f},
          {kGlideMode, float(kAlways)}, {kGlideTime, 0.12f}},
         {{"Cutoff", true, {{kCutoff, cut(500)}}},
          {"Resonance", true, {{kResonance, 0.15f}}},
          {"Env Amt", true, {{kFilterEnv, 0.2f}}},
          {"Decay", true, {{kModD, 0.02f}}},
          {"Noise", false, {{kNoiseMix, 0.1f}}},
          {"Glide", true, {{kGlideTime, 0.04f}}}}},
        {A, "OT Sub Pluck", "Short sub-heavy pluck bass for house and garage lines.", 1,
         {{kSawMix, 0.4f}, {kSubMix, 1.0f}, {kSubMode, 2.0f}, {kFilterType, float(kSvf24)}, {kCutoff, 260.0f},
          {kResonance, 0.3f}, {kNoteTrack, 0.5f}, {kFilterEnv, 0.35f}, {kModD, 0.08f}, {kAmpA, 0.001f},
          {kAmpD, 0.35f}, {kAmpS, 0.0f}, {kAmpR, 0.15f}},
         {{"Cutoff", true, {{kCutoff, cut(400)}}},
          {"Punch", true, {{kFilterEnv, 0.2f}}},
          {"Decay", true, {{kAmpD, 0.03f}}},
          {"Saw", true, {{kSawMix, 0.4f}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Release", true, {{kAmpR, 0.02f}}}}},
        {A, "OT Wah Lead", "Saw lead with a slow resonant filter wah.", 1,
         {{kVolume, 1.4f}, {kSawMix, 1.0f}, {kSquareMix, 0.3f}, {kFilterType, float(kSvf24)}, {kCutoff, 900.0f}, {kResonance, 1.0f},
          {kFilterLfo, 0.3f}, {kLfoHz, 0.7f}, {kLfoWave, float(kSine)}, {kLfoRetrig, float(kFree)},
          {kFilterEnv, 0.1f}, {kAmpA, 0.01f}, {kAmpD, 1.0f}, {kAmpS, 1.0f}, {kAmpR, 0.3f},
          {kGlideMode, float(kOverlap)}, {kGlideTime, 0.05f}},
         {{"Cutoff", true, {{kCutoff, cut(800)}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Wah Depth", true, {{kFilterLfo, 0.15f}}},
          {"Wah Rate", true, {{kLfoHz, 0.01f}}},
          {"Release", true, {{kAmpR, 0.03f}}},
          {"Glide", false, {{kGlideTime, 0.1f}}}}},
    };
}
#endif  // OT_MOD_ACD1

#if defined(OT_MOD_FAT1)
// ---------------------------------------------------------------------------
// FAT: a unison stack of up to 7 saws (Fatness), saw -> pulse (Timbre),
// Brightness and noise into LP MG 24 / LP OB 24 / LP OB 12, Amp Env, Filter
// Env, one LFO (vibrato, cutoff), glide.

std::vector<Preset> fatPresets() {
    using namespace ot::audio::fat;
    const SynthModel F = SynthModel::FAT;
    return {
        {F, "OT Super Saw", "The classic seven-saw stack: wide detune, open OB filter, a little vibrato.", 4,
         {{kVolume, 0.95f}, {kFatness, 0.85f}, {kBrightness, 0.8f}, {kOscLfo, 0.06f}, {kFilterType, float(kLpOb24)},
          {kCutoff, 6000.0f}, {kResonance, 0.2f}, {kFilterEnv, 0.1f}, {kEnvD, 0.8f}, {kEnvS, 0.4f}, {kAmpA, 0.01f},
          {kAmpD, 1.0f}, {kAmpS, 0.9f}, {kAmpR, 0.6f}, {kLfoHz, 5.0f}},
         {{"Cutoff", true, {{kCutoff, cut(5000)}}},
          {"Fatness", true, {{kFatness, 0.15f}}},
          {"Brightness", true, {{kBrightness, 0.4f}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Attack", true, {{kAmpA, 0.05f}}},
          {"Release", true, {{kAmpR, 0.08f}}}}},
        {F, "OT Wide Pad", "Slow, dark unison pad: soft attack, a filter swell and a slow free-running sweep.", 4,
         {{kVolume, 0.89f}, {kFatness, 0.7f}, {kBrightness, 0.3f}, {kOscLfo, 0.08f}, {kFilterType, float(kLpMg24)},
          {kCutoff, 1800.0f}, {kResonance, 0.3f}, {kFilterEnv, 0.15f}, {kFilterLfo, 0.05f}, {kEnvA, 1.0f},
          {kEnvD, 2.0f}, {kEnvS, 0.5f}, {kEnvR, 1.5f}, {kAmpA, 0.8f}, {kAmpD, 1.0f}, {kAmpS, 1.0f}, {kAmpR, 1.8f},
          {kLfoHz, 0.3f}, {kLfoWave, float(kSine)}, {kLfoRetrig, float(kFree)}},
         {{"Cutoff", true, {{kCutoff, cut(1500)}}},
          {"Fatness", true, {{kFatness, 0.2f}}},
          {"Sweep", false, {{kFilterLfo, 0.2f}}},
          {"Attack", true, {{kAmpA, 0.05f}, {kEnvA, 0.05f}}},
          {"Release", true, {{kAmpR, 0.1f}}},
          {"Brightness", true, {{kBrightness, 0.3f}}}}},
        {F, "OT Hoover Lead", "Mono hoover: full stack part pulse, deep vibrato, legato glide.", 1,
         {{kVolume, 1.16f}, {kFatness, 1.0f}, {kTimbre, 0.4f}, {kBrightness, 0.6f}, {kOscLfo, 0.15f},
          {kFilterType, float(kLpOb12)}, {kCutoff, 2500.0f}, {kResonance, 0.5f}, {kFilterEnv, 0.2f}, {kEnvD, 0.5f},
          {kEnvS, 0.3f}, {kAmpA, 0.005f}, {kAmpS, 1.0f}, {kAmpR, 0.3f}, {kLfoHz, 5.5f}, {kLfoWave, float(kSine)},
          {kGlideMode, float(kLegato)}, {kGlideTime, 0.12f}},
         {{"Cutoff", true, {{kCutoff, cut(2000)}}},
          {"Timbre", true, {{kTimbre, 0.4f}}},
          {"Vibrato", true, {{kOscLfo, 0.1f}}},
          {"Glide", true, {{kGlideTime, 0.05f}}},
          {"Resonance", true, {{kResonance, 0.25f}}},
          {"Fatness", true, {{kFatness, 0.3f}}}}},
        {F, "OT Trance Pluck", "Short unison pluck: resonant OB filter snapped open by its envelope.", 4,
         {{kVolume, 1.18f}, {kFatness, 0.6f}, {kFilterType, float(kLpOb24)}, {kCutoff, 300.0f}, {kResonance, 0.7f},
          {kFilterEnv, 0.45f}, {kEnvD, 0.25f}, {kEnvS, 0.0f}, {kAmpD, 0.6f}, {kAmpS, 0.0f}, {kAmpR, 0.3f}},
         {{"Cutoff", true, {{kCutoff, cut(600)}}},
          {"Env Amt", true, {{kFilterEnv, 0.15f}}},
          {"Decay", true, {{kEnvD, 0.02f}, {kAmpD, 0.03f}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Fatness", true, {{kFatness, 0.25f}}},
          {"Release", true, {{kAmpR, 0.03f}}}}},
        {F, "OT Thick Bass", "Mono unison bass: three saws, a touch of pulse, MG filter bounce, short glide.", 1,
         {{kVolume, 1.5f}, {kFatness, 0.35f}, {kTimbre, 0.2f}, {kBrightness, 0.5f}, {kNoise, 0.03f},
          {kFilterType, float(kLpMg24)}, {kCutoff, 220.0f}, {kResonance, 0.6f}, {kFilterEnv, 0.35f}, {kEnvD, 0.3f},
          {kEnvS, 0.1f}, {kAmpA, 0.002f}, {kAmpD, 0.8f}, {kAmpS, 0.8f}, {kAmpR, 0.1f}, {kOscLfo, 0.0f},
          {kGlideMode, float(kLegato)}, {kGlideTime, 0.05f}},
         {{"Cutoff", true, {{kCutoff, cut(400)}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Env Amt", true, {{kFilterEnv, 0.2f}}},
          {"Fatness", true, {{kFatness, 0.2f}}},
          {"Timbre", false, {{kTimbre, 0.6f}}},
          {"Glide", false, {{kGlideTime, 0.1f}}}}},
        {F, "OT Dark Strings", "String ensemble: a warm stack under a 12 dB filter, swell and vibrato.", 4,
         {{kVolume, 0.98f}, {kFatness, 0.75f}, {kBrightness, -0.2f}, {kOscLfo, 0.1f}, {kFilterType, float(kLpOb12)},
          {kCutoff, 1400.0f}, {kResonance, 0.15f}, {kFilterEnv, 0.1f}, {kEnvA, 0.6f}, {kEnvS, 0.7f}, {kEnvR, 1.0f},
          {kAmpA, 0.5f}, {kAmpD, 1.0f}, {kAmpS, 1.0f}, {kAmpR, 1.2f}, {kLfoHz, 5.0f}, {kLfoWave, float(kSine)}},
         {{"Cutoff", true, {{kCutoff, cut(1200)}}},
          {"Brightness", true, {{kBrightness, 0.4f}}},
          {"Vibrato", true, {{kOscLfo, 0.08f}}},
          {"Attack", true, {{kAmpA, 0.04f}, {kEnvA, 0.04f}}},
          {"Release", true, {{kAmpR, 0.08f}}},
          {"Fatness", true, {{kFatness, 0.2f}}}}},
        {F, "OT Square Stack", "Mono lead of detuned squares (Timbre 1), OB 24 filter, overlap glide.", 1,
         {{kVolume, 1.5f}, {kFatness, 0.5f}, {kTimbre, 1.0f}, {kBrightness, 0.7f}, {kFilterType, float(kLpOb24)},
          {kCutoff, 1600.0f}, {kResonance, 0.4f}, {kFilterEnv, 0.2f}, {kEnvD, 0.4f}, {kEnvS, 0.3f}, {kAmpS, 0.9f},
          {kAmpR, 0.2f}, {kGlideMode, float(kOverlap)}, {kGlideTime, 0.08f}},
         {{"Cutoff", true, {{kCutoff, cut(1500)}}},
          {"Timbre", true, {{kTimbre, 0.5f}}},
          {"Resonance", true, {{kResonance, 0.25f}}},
          {"Env Amt", true, {{kFilterEnv, 0.2f}}},
          {"Fatness", true, {{kFatness, 0.3f}}},
          {"Glide", false, {{kGlideTime, 0.1f}}}}},
        {F, "OT Detuned Keys", "Keys from a light stack: free-running phases, a decaying filter, a breath of noise.", 4,
         {{kVolume, 0.9f}, {kFatness, 0.3f}, {kTimbre, 0.15f}, {kBrightness, 0.4f}, {kNoise, 0.02f},
          {kReset, 0.0f}, {kFilterType, float(kLpMg24)}, {kCutoff, 900.0f}, {kResonance, 0.35f},
          {kFilterEnv, 0.25f}, {kEnvD, 0.6f}, {kEnvS, 0.2f}, {kAmpD, 1.5f}, {kAmpS, 0.4f}, {kAmpR, 0.4f}},
         {{"Cutoff", true, {{kCutoff, cut(800)}}},
          {"Env Amt", true, {{kFilterEnv, 0.15f}}},
          {"Decay", true, {{kAmpD, 0.1f}, {kEnvD, 0.04f}}},
          {"Fatness", true, {{kFatness, 0.2f}}},
          {"Timbre", true, {{kTimbre, 0.15f}}},
          {"Release", true, {{kAmpR, 0.04f}}}}},
    };
}
#endif  // OT_MOD_FAT1

#if defined(OT_MOD_WTF1)
// ---------------------------------------------------------------------------
// WTFM: two operators (operator 2 modulates the carrier), Character rows
// along Shape, Feedback 1 / 2 / 2>1, Ratio 0.25..12, 15 filter types; the
// FM depth envelopes come from Env 2 (or Env 1) through the mod matrix.
// Classic two-operator FM sounds of our own design.

namespace wt {
using namespace ot::audio::wtfm;
// Ratio value -> stored index ((i + 1) / 4).
constexpr float ratio(float r) { return r * 4.0f - 1.0f; }
// Mod slot k: source -> destination, amount.
std::vector<Edit> route(int k, int src, int dst, float amt) {
    return {{kModBase + 3 * k, float(src)}, {kModBase + 3 * k + 1, float(dst)}, {kModBase + 3 * k + 2, amt}};
}
std::vector<Edit> join(std::vector<Edit> a, const std::vector<Edit>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}
// An open filter (LP SVF 12 at 14 kHz, no envelope, no key tracking).
const std::vector<Edit> kOpen = {{kFilterType, 4.0f}, {kCutoff, 14000.0f}, {kResonance, 0.0f}, {kFilterEnvAmt, 0.0f},
                                 {kNoteTrack, 0.0f}};
std::vector<Macro> keyMacros() {
    return {{"Brightness", true, {{kFm, 0.15f}}},
            {"Feedback", false, {{kFb1, 0.5f}}},
            {"Decay", true, {{kAmpD, 0.1f}, {kEnv2D, 0.1f}}},
            {"Release", true, {{kAmpR, 0.08f}}},
            {"Tone", true, {{kCutoff, cut(8000)}}},
            {"Ratio 2", true, {{kRatio2, 0.1f}}}};
}
std::vector<Macro> bassMacros() {
    return {{"Brightness", true, {{kFm, 0.15f}}},
            {"Cutoff", true, {{kCutoff, cut(1500)}}},
            {"Feedback", false, {{kFb2, 0.6f}}},
            {"Decay", true, {{kEnv2D, 0.05f}, {kAmpD, 0.05f}}},
            {"Glide", false, {{kGlideTime, 0.1f}}},
            {"Grit", false, {{kFb1, 0.4f}}}};
}
std::vector<Macro> padMacros() {
    return {{"Brightness", true, {{kFm, 0.1f}}},
            {"Cutoff", true, {{kCutoff, cut(5000)}}},
            {"Shape", true, {{kShape1, 0.4f}}},
            {"Attack", true, {{kAmpA, 0.05f}}},
            {"Release", true, {{kAmpR, 0.08f}}},
            {"Motion", false, {{kModBase + 2, 0.1f}}}};
}
std::vector<Macro> leadMacros() {
    return {{"Brightness", true, {{kFm, 0.15f}}},
            {"Feedback", true, {{kFb1, 0.4f}}},
            {"Cutoff", true, {{kCutoff, cut(6000)}}},
            {"Vibrato", false, {{kModBase + 2, 0.02f}}},
            {"Glide", false, {{kGlideTime, 0.1f}}},
            {"Release", true, {{kAmpR, 0.08f}}}};
}
}  // namespace wt

std::vector<Preset> wtfmPresets() {
    using namespace ot::audio::wtfm;
    using wt::join;
    using wt::kOpen;
    using wt::ratio;
    using wt::route;
    const SynthModel W = SynthModel::WTFM;
    // Classic FM keys: the modulator's depth decays with Env 2.
    auto keys = [&](float r2, float fm, float envFm, float envD, float ampD, float ampS, float ampR) {
        return join(join(kOpen, {{kRatio2, ratio(r2)}, {kFm, fm}, {kEnv2A, 0.0f}, {kEnv2D, envD}, {kEnv2S, 0.0f},
                                 {kEnv2R, 0.4f}, {kAmpA, 0.002f}, {kAmpD, ampD}, {kAmpS, ampS}, {kAmpR, ampR}}),
                    route(0, kSrcEnv2, kDstFm, envFm));
    };
    return {
        // --- keys ---
        {W, "OT FM E-Piano", "Classic FM electric piano: 1:1 operators, bright attack that mellows.", 4,
         join(keys(1.0f, 0.35f, 1.6f, 1.2f, 2.5f, 0.25f, 0.6f), {{kVolume, 0.9f}, {kFine2, 4.0f}}), wt::keyMacros()},
        {W, "OT Tine Piano", "Bell-like tine on top of a soft body: the modulator at 12x, short decay.", 4,
         join(keys(12.0f, 0.15f, 1.2f, 0.35f, 2.8f, 0.2f, 0.7f), {{kVolume, 0.9f}}), wt::keyMacros()},
        {W, "OT Mellow Tines", "Darker, rounder FM piano for chords.", 4,
         join(keys(1.0f, 0.2f, 0.9f, 1.8f, 3.0f, 0.3f, 0.8f), {{kCutoff, 5000.0f}, {kVolume, 0.95f}}), wt::keyMacros()},
        {W, "OT Bright Keys", "Glassy 2:1 keys with a sharp attack.", 4,
         join(keys(2.0f, 0.6f, 1.0f, 0.6f, 1.8f, 0.2f, 0.5f), {{kVolume, 0.85f}}), wt::keyMacros()},
        {W, "OT FM Clav", "Funky clavinet: saw-like carrier, 3:1 bite and a quick filter.", 1,
         join(keys(3.0f, 0.8f, 1.5f, 0.15f, 0.6f, 0.0f, 0.1f),
              {{kChar1, float(kSharper)}, {kShape1, 0.55f}, {kFb21, 0.25f}, {kFilterType, 2.0f}, {kCutoff, 2500.0f},
               {kResonance, 0.5f}, {kFilterEnvAmt, 0.25f}, {kEnv1D, 0.2f}, {kVolume, 1.7f}}),
         wt::keyMacros()},
        {W, "OT FM Harpsi", "Plucked harpsichord: 4:1 modulator, no sustain.", 4,
         join(keys(4.0f, 1.0f, 1.0f, 0.5f, 1.4f, 0.0f, 0.4f), {{kFb1, 0.15f}, {kVolume, 0.85f}}), wt::keyMacros()},
        {W, "OT FM Organ", "Drawbar-style organ: hollow odd row, 2:1 FM, gentle vibrato.", 4,
         join(join(kOpen, {{kChar1, float(kAdd2)}, {kShape1, 0.35f}, {kRatio2, ratio(2.0f)}, {kFm, 0.7f},
                           {kAmpA, 0.004f}, {kAmpD, 1.0f}, {kAmpS, 1.0f}, {kAmpR, 0.08f}, {kLfo1Ratio, 6.0f},
                           {kLfo1Wave, 1.0f}, {kLfo1Retrig, 0.0f}, {kVolume, 0.8f}}),
              route(0, kSrcLfo1, kDstPitch1, 0.008f)),
         wt::keyMacros()},
        {W, "OT Perc Organ", "Organ with a percussive click from a fast FM envelope.", 4,
         join(keys(3.0f, 0.4f, 2.0f, 0.12f, 1.0f, 0.8f, 0.1f), {{kChar1, float(kAdd1)}, {kShape1, 0.25f}, {kVolume, 0.8f}}),
         wt::keyMacros()},
        // --- bells and mallets ---
        {W, "OT Glass Bell", "Clear bell: 3.5:1 inharmonic partials that ring out.", 4,
         join(keys(3.5f, 1.5f, 1.5f, 2.0f, 3.5f, 0.0f, 2.0f), {{kVolume, 0.8f}}), wt::keyMacros()},
        {W, "OT Tubular Bell", "Deep church bell: 2.75:1 with a long, darkening tail.", 4,
         join(keys(2.75f, 2.0f, 1.5f, 3.0f, 5.0f, 0.0f, 3.0f), {{kCutoff, 9000.0f}, {kVolume, 0.75f}}), wt::keyMacros()},
        {W, "OT FM Marimba", "Wooden mallet: 4:1 modulator, very short FM envelope.", 4,
         join(keys(4.0f, 0.2f, 2.5f, 0.08f, 0.6f, 0.0f, 0.3f), {{kVolume, 0.95f}}), wt::keyMacros()},
        {W, "OT FM Vibes", "Vibraphone: soft 4:1 strike and a motor tremolo.", 4,
         join(join(keys(4.0f, 0.3f, 1.2f, 0.2f, 3.0f, 0.0f, 1.0f),
                   {{kLfo2Ratio, 5.0f}, {kLfo2Wave, 1.0f}, {kLfo2Retrig, 0.0f}, {kVolume, 0.9f}}),
              route(1, kSrcLfo2, kDstVolume, 0.25f)),
         wt::keyMacros()},
        {W, "OT Kalimba", "Thumb piano: 5:1 ping on a sine.", 4,
         join(keys(5.0f, 0.4f, 2.0f, 0.06f, 0.9f, 0.0f, 0.4f), {{kVolume, 0.95f}}), wt::keyMacros()},
        {W, "OT Music Box", "Tiny music box: an octave up, 6:1 sparkle.", 4,
         join(keys(6.0f, 0.5f, 1.0f, 0.3f, 1.6f, 0.0f, 1.0f), {{kRatio1, ratio(2.0f)}, {kVolume, 0.85f}}),
         wt::keyMacros()},
        // --- basses ---
        {W, "OT FM Bass", "Punchy 1:1 FM bass with a decaying growl.", 1,
         join(join({{kFilterType, 0.0f}, {kCutoff, 2200.0f}, {kResonance, 0.3f}, {kFilterEnvAmt, 0.1f}, {kNoteTrack, 0.5f},
                    {kRatio2, ratio(1.0f)}, {kFm, 1.0f}, {kFb2, 0.3f}, {kEnv2D, 0.3f}, {kEnv2S, 0.0f}, {kAmpA, 0.002f},
                    {kAmpD, 0.8f}, {kAmpS, 0.7f}, {kAmpR, 0.1f}, {kGlideMode, 2.0f}, {kGlideTime, 0.05f}},
                   route(0, kSrcEnv2, kDstFm, 2.0f)),
              {{kVolume, 1.9f}}),
         wt::bassMacros()},
        {W, "OT Solid Bass", "Round, solid 80s FM bass: modulator feedback, quick bite.", 1,
         join({{kFilterType, 0.0f}, {kCutoff, 3000.0f}, {kResonance, 0.2f}, {kFilterEnvAmt, 0.0f}, {kNoteTrack, 0.5f},
               {kRatio2, ratio(1.0f)}, {kFm, 1.5f}, {kFb2, 0.45f}, {kEnv2D, 0.18f}, {kEnv2S, 0.0f}, {kAmpA, 0.001f},
               {kAmpD, 1.2f}, {kAmpS, 0.6f}, {kAmpR, 0.08f}, {kVolume, 1.9f}},
              route(0, kSrcEnv2, kDstFm, 1.5f)),
         wt::bassMacros()},
        {W, "OT Slap Bass", "Slapped bass: 3:1 snap on attack, then a plain body.", 1,
         join({{kFilterType, 1.0f}, {kCutoff, 4000.0f}, {kResonance, 0.2f}, {kFilterEnvAmt, 0.0f}, {kNoteTrack, 0.4f},
               {kRatio2, ratio(3.0f)}, {kFm, 0.3f}, {kFb1, 0.2f}, {kEnv2D, 0.07f}, {kEnv2S, 0.0f}, {kAmpA, 0.001f},
               {kAmpD, 0.9f}, {kAmpS, 0.4f}, {kAmpR, 0.1f}, {kVolume, 2.0f}},
              route(0, kSrcEnv2, kDstFm, 3.0f)),
         wt::bassMacros()},
        {W, "OT Wood Bass", "Upright-style bass: soft 1:1 pluck, short sustain.", 1,
         join({{kVolume, 1.8f}, {kFilterType, 3.0f}, {kCutoff, 1600.0f}, {kResonance, 0.1f}, {kFilterEnvAmt, 0.1f}, {kNoteTrack, 0.6f},
               {kRatio2, ratio(1.0f)}, {kFm, 0.3f}, {kEnv2D, 0.12f}, {kEnv2S, 0.0f}, {kAmpA, 0.004f}, {kAmpD, 0.9f},
               {kAmpS, 0.15f}, {kAmpR, 0.2f}},
              route(0, kSrcEnv2, kDstFm, 1.0f)),
         wt::bassMacros()},
        {W, "OT Sub Thump", "Deep sine sub with a short FM thump.", 1,
         join({{kVolume, 1.6f}, {kFilterType, 0.0f}, {kCutoff, 600.0f}, {kResonance, 0.1f}, {kFilterEnvAmt, 0.1f}, {kNoteTrack, 0.5f},
               {kRatio2, ratio(2.0f)}, {kEnv2D, 0.06f}, {kEnv2S, 0.0f}, {kAmpA, 0.002f}, {kAmpD, 1.0f}, {kAmpS, 0.9f},
               {kAmpR, 0.12f}},
              route(0, kSrcEnv2, kDstFm, 1.2f)),
         wt::bassMacros()},
        {W, "OT Growl Bass", "Wild, moving bass: folded carrier, LFO on the FM depth.", 1,
         join(join({{kChar1, float(kWilder)}, {kShape1, 0.5f}, {kFilterType, 1.0f}, {kCutoff, 1200.0f}, {kResonance, 0.6f},
                    {kFilterEnvAmt, 0.2f}, {kEnv1D, 0.3f}, {kNoteTrack, 0.5f}, {kRatio2, ratio(0.5f)}, {kFm, 0.8f},
                    {kLfo1Ratio, 4.0f}, {kLfo1Wave, 0.0f}, {kAmpA, 0.002f}, {kAmpS, 1.0f}, {kAmpR, 0.1f},
                    {kGlideMode, 2.0f}, {kGlideTime, 0.06f}, {kVolume, 1.7f}},
                   route(0, kSrcLfo1, kDstFm, 0.6f)),
              {}),
         wt::bassMacros()},
        // --- leads and brass ---
        {W, "OT FM Brass", "Brass section: the FM depth swells in with the attack.", 4,
         join(join({{kFilterType, 4.0f}, {kCutoff, 8000.0f}, {kResonance, 0.0f}, {kFilterEnvAmt, 0.0f}, {kNoteTrack, 0.0f},
                    {kRatio2, ratio(1.0f)}, {kFm, 0.3f}, {kFb2, 0.3f}, {kEnv2A, 0.08f}, {kEnv2D, 0.5f}, {kEnv2S, 0.5f},
                    {kEnv2R, 0.3f}, {kAmpA, 0.05f}, {kAmpD, 0.5f}, {kAmpS, 0.85f}, {kAmpR, 0.25f}, {kLfo1Ratio, 5.5f},
                    {kLfo1Wave, 1.0f}, {kVolume, 0.85f}},
                   route(0, kSrcEnv2, kDstFm, 1.5f)),
              route(1, kSrcLfo1, kDstPitch1, 0.004f)),
         wt::leadMacros()},
        {W, "OT Square Lead", "Hollow square lead with feedback edge and vibrato.", 1,
         join(join({{kChar1, float(kSharper)}, {kShape1, 0.85f}, {kFb1, 0.3f}, {kRatio2, ratio(2.0f)}, {kFm, 0.3f},
                    {kFilterType, 1.0f}, {kCutoff, 5000.0f}, {kResonance, 0.3f}, {kFilterEnvAmt, 0.0f}, {kNoteTrack, 0.5f},
                    {kAmpA, 0.005f}, {kAmpS, 1.0f}, {kAmpR, 0.15f}, {kLfo1Ratio, 5.0f}, {kLfo1Wave, 1.0f},
                    {kGlideMode, 2.0f}, {kGlideTime, 0.08f}, {kVolume, 1.6f}},
                   route(0, kSrcLfo1, kDstPitch1, 0.006f)),
              {}),
         wt::leadMacros()},
        {W, "OT Reed Lead", "Clarinet-like reed: the odd-harmonic row, soft vibrato.", 1,
         join(join(kOpen, {{kChar1, float(kAdd2)}, {kShape1, 0.8f}, {kRatio2, ratio(2.0f)}, {kFm, 0.2f}, {kAmpA, 0.03f},
                           {kAmpS, 1.0f}, {kAmpR, 0.2f}, {kLfo1Ratio, 5.0f}, {kLfo1Wave, 1.0f}, {kCutoff, 6000.0f},
                           {kVolume, 1.8f}}),
              route(0, kSrcLfo1, kDstPitch1, 0.005f)),
         wt::leadMacros()},
        {W, "OT Metal Lead", "Screaming lead: folded row, heavy feedback, oversampled.", 1,
         join(join({{kChar1, float(kWilder)}, {kShape1, 0.6f}, {kFb1, 0.6f}, {kRatio2, ratio(1.0f)}, {kFm, 1.0f},
                    {kOversample, 1.0f}, {kFilterType, 1.0f}, {kCutoff, 4500.0f}, {kResonance, 0.4f}, {kFilterEnvAmt, 0.0f},
                    {kNoteTrack, 0.5f}, {kAmpA, 0.004f}, {kAmpS, 1.0f}, {kAmpR, 0.2f}, {kLfo1Ratio, 5.5f},
                    {kLfo1Wave, 1.0f}, {kGlideMode, 2.0f}, {kGlideTime, 0.06f}, {kVolume, 1.6f}},
                   route(0, kSrcLfo1, kDstPitch1, 0.006f)),
              {}),
         wt::leadMacros()},
        // --- pads ---
        {W, "OT FM Strings", "Soft string ensemble: saw-like carrier, slow attack, slow shape motion.", 4,
         join(join({{kChar1, float(kSmoother)}, {kShape1, 0.5f}, {kRatio2, ratio(1.0f)}, {kFine2, 7.0f}, {kFm, 0.5f},
                    {kFilterType, 3.0f}, {kCutoff, 4000.0f}, {kResonance, 0.1f}, {kFilterEnvAmt, 0.0f}, {kNoteTrack, 0.3f},
                    {kAmpA, 0.6f}, {kAmpD, 1.0f}, {kAmpS, 1.0f}, {kAmpR, 1.2f}, {kLfo1Ratio, 0.3f}, {kLfo1Wave, 1.0f},
                    {kLfo1Retrig, 0.0f}, {kVolume, 0.85f}},
                   route(0, kSrcLfo1, kDstShape1, 0.1f)),
              {}),
         wt::padMacros()},
        {W, "OT Crystal Pad", "Shimmering 3:1 pad, the FM depth drifting slowly.", 4,
         join(join(kOpen, {{kRatio2, ratio(3.0f)}, {kFm, 0.8f}, {kAmpA, 1.2f}, {kAmpD, 1.0f}, {kAmpS, 1.0f}, {kAmpR, 2.0f},
                           {kLfo1Ratio, 0.2f}, {kLfo1Wave, 1.0f}, {kLfo1Retrig, 0.0f}, {kCutoff, 7000.0f},
                           {kVolume, 0.8f}}),
              route(0, kSrcLfo1, kDstFm, 0.4f)),
         wt::padMacros()},
        {W, "OT Choir Pad", "Airy choir: the Add 5 row through a low-pass, slow swell.", 4,
         join(join({{kChar1, float(kAdd5)}, {kShape1, 0.5f}, {kFilterType, 3.0f}, {kCutoff, 2500.0f}, {kResonance, 0.2f},
                    {kFilterEnvAmt, 0.0f}, {kNoteTrack, 0.3f}, {kAmpA, 0.9f}, {kAmpD, 1.0f}, {kAmpS, 1.0f}, {kAmpR, 1.5f},
                    {kLfo1Ratio, 0.4f}, {kLfo1Wave, 1.0f}, {kLfo1Retrig, 0.0f}, {kVolume, 0.9f}},
                   route(0, kSrcLfo1, kDstShape1, 0.15f)),
              {}),
         wt::padMacros()},
        {W, "OT Hollow Pad", "Hollow odd-harmonic pad that slowly opens.", 4,
         join(join({{kChar1, float(kAdd2)}, {kShape1, 0.3f}, {kRatio2, ratio(2.0f)}, {kFm, 0.3f}, {kFilterType, 0.0f},
                    {kCutoff, 1200.0f}, {kResonance, 0.3f}, {kFilterEnvAmt, 0.2f}, {kEnv1A, 1.5f}, {kEnv1D, 2.0f},
                    {kEnv1S, 0.6f}, {kNoteTrack, 0.4f}, {kAmpA, 0.8f}, {kAmpS, 1.0f}, {kAmpR, 1.5f}, {kLfo1Ratio, 0.25f},
                    {kLfo1Wave, 1.0f}, {kLfo1Retrig, 0.0f}, {kVolume, 0.9f}},
                   route(0, kSrcLfo1, kDstShape1, 0.2f)),
              {}),
         wt::padMacros()},
        // --- effects ---
        {W, "OT Noise Hit", "Metallic noise burst: Add 11 row, full feedback, short.", 1,
         join(join(kOpen, {{kChar1, float(kAdd11)}, {kShape1, 1.0f}, {kFb1, 1.0f}, {kRatio2, ratio(7.0f)}, {kFm, 4.0f},
                           {kAmpA, 0.001f}, {kAmpD, 0.25f}, {kAmpS, 0.0f}, {kAmpR, 0.2f}, {kEnv2D, 0.2f}, {kEnv2S, 0.0f},
                           {kVolume, 1.4f}}),
              route(0, kSrcEnv2, kDstFm, 2.0f)),
         wt::leadMacros()},
        {W, "OT Laser Zap", "Falling FM zap: the pitch and FM depth drop together.", 1,
         join(join(join(kOpen, {{kRatio2, ratio(1.5f)}, {kFm, 1.0f}, {kAmpA, 0.001f}, {kAmpD, 0.4f}, {kAmpS, 0.0f},
                                {kAmpR, 0.2f}, {kEnv2D, 0.25f}, {kEnv2S, 0.0f}, {kVolume, 1.6f}}),
                   route(0, kSrcEnv2, kDstPitch1, 2.0f)),
              join(route(1, kSrcEnv2, kDstPitch2, 2.0f), route(2, kSrcEnv2, kDstFm, 3.0f))),
         wt::leadMacros()},
    };
}
#endif  // OT_MOD_WTF1

#if defined(OT_MOD_VAP1)
// ---------------------------------------------------------------------------
// VAP: two morphing oscillators, 15 filter types, 3 envelopes, 2 LFOs and a
// 6-slot mod matrix. Filter types: 0 LP MG 24, 1 LP OB 24, 2 LP OB 12,
// 3 LP SVF 24, 4 LP SVF 12, ... 11 BP SVF 24. Shape: 0 tri, 0.5 saw, 1 pulse.

using namespace ot::audio::vap;
constexpr int modSrc(int k) { return kModBase + 3 * k; }
constexpr int modDst(int k) { return kModBase + 3 * k + 1; }
constexpr int modAmt(int k) { return kModBase + 3 * k + 2; }
#define MOD(k, src, dst, amt) {modSrc(k), float(src)}, {modDst(k), float(dst)}, {modAmt(k), amt}

enum : int { kLpMg24 = 0, kLpOb24 = 1, kLpOb12 = 2, kLpSvf24 = 3, kLpSvf12 = 4, kBpSvf24 = 11 };
enum : int { kWTri = 0, kWSine = 1 };
enum : int { kRFree = 0, kRNote = 2 };

std::vector<Preset> vapPresets() {
    const SynthModel V = SynthModel::VAP;
    return {
        {V, "OT Velvet Pad", "Warm detuned-saw pad, slow attack, a gentle tempo-synced filter drift.", 4,
         {{kVolume, 0.82f}, {kShape1, 0.5f}, {kShape2, 0.5f}, {kDetune, 20.0f}, {kFilterType, float(kLpOb24)}, {kCutoff, 1400.0f},
          {kResonance, 0.3f}, {kFilterEnvAmt, 0.15f}, {kEnv1A, 0.8f}, {kEnv1D, 2.0f}, {kEnv1S, 0.5f},
          {kEnv1R, 1.5f}, {kAmpA, 0.7f}, {kAmpD, 1.0f}, {kAmpS, 1.0f}, {kAmpR, 1.6f}, {kLfo1Sync, 1.0f},
          {kLfo1Ratio, 4.0f}, {kLfo1Wave, float(kWTri)}, {kLfo1Retrig, float(kRFree)}, {kLfo2Ratio, 0.3f},
          {kLfo2Wave, float(kWTri)}, {kLfo2Retrig, float(kRFree)}, MOD(0, kSrcLfo1, kDstCutoff, 0.04f),
          MOD(1, kSrcLfo2, kDstPitch2, 0.003f)},
         {{"Cutoff", true, {{kCutoff, cut(1000)}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Attack", true, {{kAmpA, 0.05f}, {kEnv1A, 0.05f}}},
          {"Release", true, {{kAmpR, 0.08f}}},
          {"Motion", false, {{modAmt(0), 0.02f}}},
          {"Detune", true, {{kDetune, 0.15f}}}}},
        {V, "OT Glass Pad", "Airy triangle + pulse pad an octave apart, slow pulse-width shimmer.", 4,
         {{kVolume, 0.8f}, {kShape1, 0.0f}, {kShape2, 0.8f}, {kTune2, 12.0f}, {kOscMix, -0.2f}, {kDetune, 8.0f},
          {kFilterType, float(kLpSvf12)}, {kCutoff, 3000.0f}, {kResonance, 0.25f}, {kFilterEnvAmt, 0.0f},
          {kAmpA, 1.0f}, {kAmpD, 1.0f}, {kAmpS, 1.0f}, {kAmpR, 2.2f}, {kLfo1Sync, 1.0f}, {kLfo1Ratio, 2.0f},
          {kLfo1Wave, float(kWTri)}, {kLfo1Retrig, float(kRFree)}, {kLfo2Ratio, 0.25f},
          {kLfo2Wave, float(kWTri)}, {kLfo2Retrig, float(kRFree)}, MOD(0, kSrcLfo2, kDstPw2, 0.5f),
          MOD(1, kSrcLfo1, kDstCutoff, 0.05f)},
         {{"Cutoff", true, {{kCutoff, cut(2000)}}},
          {"Shimmer", true, {{kOscMix, 0.3f}}},
          {"Attack", true, {{kAmpA, 0.06f}}},
          {"Release", true, {{kAmpR, 0.1f}}},
          {"PWM Depth", false, {{modAmt(0), 0.04f}}},
          {"Detune", true, {{kDetune, 0.1f}}}}},
        {V, "OT Warm Strings", "String ensemble: detuned saws, soft swell, gentle vibrato.", 4,
         {{kVolume, 0.83f}, {kShape1, 0.5f}, {kShape2, 0.5f}, {kDetune, 14.0f}, {kFilterType, float(kLpOb12)}, {kCutoff, 2400.0f},
          {kResonance, 0.15f}, {kFilterEnvAmt, 0.1f}, {kEnv1A, 0.4f}, {kEnv1D, 1.0f}, {kEnv1S, 0.7f},
          {kEnv1R, 0.8f}, {kAmpA, 0.35f}, {kAmpD, 1.0f}, {kAmpS, 1.0f}, {kAmpR, 0.9f}, {kLfo1Ratio, 5.2f},
          {kLfo1Wave, float(kWSine)}, {kLfo1Retrig, float(kRFree)}, MOD(0, kSrcLfo1, kDstPitch1, 0.004f),
          MOD(1, kSrcLfo1, kDstPitch2, 0.004f)},
         {{"Cutoff", true, {{kCutoff, cut(1500)}}},
          {"Attack", true, {{kAmpA, 0.04f}}},
          {"Release", true, {{kAmpR, 0.06f}}},
          {"Vibrato", true, {{modAmt(0), 0.001f}, {modAmt(1), 0.001f}}},
          {"Detune", true, {{kDetune, 0.15f}}},
          {"Env Amt", true, {{kFilterEnvAmt, 0.1f}}}}},
        {V, "OT Brass Stab", "Punchy synth brass: ladder filter with a quick envelope bite.", 4,
         {{kVolume, 0.72f}, {kShape1, 0.5f}, {kShape2, 0.5f}, {kDetune, 10.0f}, {kFilterType, float(kLpMg24)}, {kCutoff, 500.0f},
          {kResonance, 0.25f}, {kFilterEnvAmt, 0.3f}, {kEnv1A, 0.03f}, {kEnv1D, 0.35f}, {kEnv1S, 0.35f},
          {kEnv1R, 0.25f}, {kAmpA, 0.015f}, {kAmpD, 0.4f}, {kAmpS, 0.85f}, {kAmpR, 0.22f}},
         {{"Cutoff", true, {{kCutoff, cut(800)}}},
          {"Env Amt", true, {{kFilterEnvAmt, 0.15f}}},
          {"Attack", true, {{kEnv1A, 0.01f}, {kAmpA, 0.01f}}},
          {"Release", true, {{kAmpR, 0.03f}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Detune", true, {{kDetune, 0.1f}}}}},
        {V, "OT Sync Lead", "Hard-sync lead: an envelope sweeps the synced oscillator; glide and vibrato.", 1,
         {{kVolume, 0.87f}, {kShape1, 0.5f}, {kShape2, 0.5f}, {kOscSync, 1.0f}, {kOscMix, 0.6f}, {kFilterType, float(kLpSvf24)},
          {kCutoff, 4000.0f}, {kResonance, 0.2f}, {kFilterEnvAmt, 0.0f}, {kAmpA, 0.003f}, {kAmpD, 0.6f},
          {kAmpS, 0.9f}, {kAmpR, 0.25f}, {kEnv2A, 0.0f}, {kEnv2D, 0.5f}, {kEnv2S, 0.25f}, {kEnv2R, 0.3f},
          {kLfo1Ratio, 5.5f}, {kLfo1Wave, float(kWSine)}, {kGlideMode, 1.0f}, {kGlideTime, 0.08f},
          MOD(0, kSrcEnv2, kDstPitch2, 1.5f), MOD(1, kSrcLfo1, kDstPitch1, 0.004f),
          MOD(2, kSrcLfo1, kDstPitch2, 0.004f)},
         {{"Cutoff", true, {{kCutoff, cut(3000)}}},
          {"Sync Sweep", true, {{modAmt(0), 0.1f}}},
          {"Sweep Time", true, {{kEnv2D, 0.03f}}},
          {"Vibrato", false, {{modAmt(1), 0.002f}, {modAmt(2), 0.002f}}},
          {"Glide", false, {{kGlideTime, 0.05f}}},
          {"Release", true, {{kAmpR, 0.03f}}}}},
        {V, "OT PWM Lead", "Two pulse waves with independent width LFOs; thick and moving.", 1,
         {{kVolume, 0.67f}, {kShape1, 1.0f}, {kShape2, 1.0f}, {kPw2, 0.1f}, {kDetune, 6.0f}, {kFilterType, float(kLpSvf24)},
          {kCutoff, 2600.0f}, {kResonance, 0.3f}, {kFilterEnvAmt, 0.12f}, {kEnv1D, 0.4f}, {kEnv1S, 0.3f},
          {kAmpA, 0.005f}, {kAmpS, 1.0f}, {kAmpR, 0.2f}, {kLfo1Ratio, 4.5f}, {kLfo1Wave, float(kWTri)},
          {kLfo1Retrig, float(kRFree)}, {kLfo2Ratio, 3.1f}, {kLfo2Wave, float(kWTri)},
          {kLfo2Retrig, float(kRFree)}, {kGlideMode, 1.0f}, {kGlideTime, 0.06f}, MOD(0, kSrcLfo1, kDstPw1, 0.6f),
          MOD(1, kSrcLfo2, kDstPw2, 0.5f)},
         {{"Cutoff", true, {{kCutoff, cut(2000)}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"PWM Depth", true, {{modAmt(0), 0.05f}, {modAmt(1), 0.05f}}},
          {"PWM Rate", true, {{kLfo1Ratio, 0.02f}, {kLfo2Ratio, 0.015f}}},
          {"Glide", false, {{kGlideTime, 0.05f}}},
          {"Release", true, {{kAmpR, 0.03f}}}}},
        {V, "OT Soft Pluck", "Mellow triangle + saw pluck with an octave on top.", 4,
         {{kVolume, 0.81f}, {kShape1, 0.0f}, {kShape2, 0.5f}, {kTune2, 12.0f}, {kOscMix, -0.3f}, {kFilterType, float(kLpOb12)},
          {kCutoff, 450.0f}, {kResonance, 0.3f}, {kFilterEnvAmt, 0.4f}, {kEnv1A, 0.0f}, {kEnv1D, 0.3f},
          {kEnv1S, 0.0f}, {kEnv1R, 0.3f}, {kAmpA, 0.002f}, {kAmpD, 0.9f}, {kAmpS, 0.0f}, {kAmpR, 0.5f}},
         {{"Cutoff", true, {{kCutoff, cut(600)}}},
          {"Env Amt", true, {{kFilterEnvAmt, 0.15f}}},
          {"Decay", true, {{kAmpD, 0.06f}, {kEnv1D, 0.02f}}},
          {"Release", true, {{kAmpR, 0.04f}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Tone", true, {{kOscMix, 0.3f}}}}},
        {V, "OT Harp Pluck", "Bright pulse + triangle pluck with a long ring; phase-reset attack.", 4,
         {{kVolume, 0.84f}, {kShape1, 1.0f}, {kPw1, -0.2f}, {kShape2, 0.0f}, {kTune2, 12.0f}, {kOscMix, -0.2f}, {kReset1, 1.0f},
          {kReset2, 1.0f}, {kFilterType, float(kLpSvf24)}, {kCutoff, 700.0f}, {kResonance, 0.35f},
          {kFilterEnvAmt, 0.38f}, {kEnv1A, 0.0f}, {kEnv1D, 0.45f}, {kEnv1S, 0.0f}, {kEnv1R, 0.5f}, {kAmpA, 0.0f},
          {kAmpD, 1.6f}, {kAmpS, 0.0f}, {kAmpR, 1.2f}},
         {{"Cutoff", true, {{kCutoff, cut(800)}}},
          {"Env Amt", true, {{kFilterEnvAmt, 0.15f}}},
          {"Decay", true, {{kAmpD, 0.08f}, {kEnv1D, 0.03f}}},
          {"Width", true, {{kPw1, 0.3f}}},
          {"Release", true, {{kAmpR, 0.06f}}},
          {"Resonance", true, {{kResonance, 0.2f}}}}},
        {V, "OT Fat Bass", "Saw + square an octave down through the ladder filter; mono, legato glide.", 1,
         {{kVolume, 0.79f}, {kShape1, 0.5f}, {kShape2, 1.0f}, {kTune2, -12.0f}, {kDetune, 6.0f}, {kFilterType, float(kLpMg24)},
          {kCutoff, 230.0f}, {kResonance, 0.45f}, {kFilterEnvAmt, 0.28f}, {kNoteTrack, 0.5f}, {kEnv1D, 0.28f},
          {kEnv1S, 0.1f}, {kEnv1R, 0.1f}, {kAmpA, 0.002f}, {kAmpD, 0.5f}, {kAmpS, 0.8f}, {kAmpR, 0.09f},
          {kReset1, 1.0f}, {kReset2, 1.0f}, {kGlideMode, 2.0f}, {kGlideTime, 0.05f}},
         {{"Cutoff", true, {{kCutoff, cut(500)}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Env Amt", true, {{kFilterEnvAmt, 0.15f}}},
          {"Decay", true, {{kEnv1D, 0.03f}}},
          {"Sub", true, {{kOscMix, 0.4f}}},
          {"Glide", false, {{kGlideTime, 0.05f}}}}},
        {V, "OT Bell Keys", "Struck bell: two triangles at an inharmonic interval, bright strike, long ring.", 4,
         {{kVolume, 0.92f}, {kShape1, 0.0f}, {kShape2, 0.0f}, {kTune2, 27.0f}, {kOscMix, -0.7f}, {kMixEnvAmt, 1.2f}, {kEnv2A, 0.0f},
          {kEnv2D, 0.35f}, {kEnv2S, 0.0f}, {kEnv2R, 0.3f}, {kReset1, 1.0f}, {kReset2, 1.0f},
          {kFilterType, float(kLpSvf12)}, {kCutoff, 6000.0f}, {kResonance, 0.1f}, {kFilterEnvAmt, 0.0f},
          {kAmpA, 0.0f}, {kAmpD, 2.5f}, {kAmpS, 0.0f}, {kAmpR, 1.8f}},
         {{"Tone", true, {{kCutoff, cut(3000)}}},
          {"Strike", true, {{kMixEnvAmt, 0.15f}}},
          {"Decay", true, {{kAmpD, 0.1f}}},
          {"Release", true, {{kAmpR, 0.1f}}},
          {"Ratio", true, {{kTune2, 0.05f}}},
          {"Chorus", false, {{kDetune, 0.15f}}}}},
        {V, "OT Mellow Keys", "Soft electric-piano-like keys with a gentle tremolo.", 4,
         {{kVolume, 0.77f}, {kShape1, 0.15f}, {kShape2, 0.0f}, {kTune2, 12.0f}, {kOscMix, -0.4f}, {kFilterType, float(kLpOb12)},
          {kCutoff, 1500.0f}, {kResonance, 0.1f}, {kFilterEnvAmt, 0.18f}, {kEnv1D, 0.7f}, {kEnv1S, 0.2f},
          {kEnv1R, 0.4f}, {kAmpA, 0.003f}, {kAmpD, 1.8f}, {kAmpS, 0.35f}, {kAmpR, 0.45f}, {kLfo2Ratio, 5.0f},
          {kLfo2Wave, float(kWSine)}, {kLfo2Retrig, float(kRFree)}, MOD(0, kSrcLfo2, kDstVolume, 0.12f)},
         {{"Cutoff", true, {{kCutoff, cut(1200)}}},
          {"Env Amt", true, {{kFilterEnvAmt, 0.12f}}},
          {"Decay", true, {{kAmpD, 0.1f}}},
          {"Release", true, {{kAmpR, 0.04f}}},
          {"Tremolo", false, {{modAmt(0), 0.03f}}},
          {"Tone", true, {{kShape1, 0.3f}}}}},
        {V, "OT Wind Sweep", "Resonant band-passed noise, slowly swept over four bars.", 4,
         {{kVolume, 1.2f}, {kNoise, 1.0f}, {kShape1, 0.0f}, {kOscMix, -1.0f}, {kFilterType, float(kBpSvf24)}, {kCutoff, 900.0f},
          {kResonance, 1.0f}, {kFilterEnvAmt, 0.0f}, {kNoteTrack, 0.5f}, {kLfo1Sync, 1.0f}, {kLfo1Ratio, 0.0f},
          {kLfo1Wave, float(kWTri)}, {kLfo1Retrig, float(kRFree)}, {kAmpA, 1.2f}, {kAmpD, 1.0f}, {kAmpS, 1.0f},
          {kAmpR, 2.5f}, MOD(0, kSrcLfo1, kDstCutoff, 0.3f)},
         {{"Cutoff", true, {{kCutoff, cut(800)}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Sweep Depth", true, {{modAmt(0), 0.03f}}},
          {"Air", true, {{kNoise, 0.5f}}},
          {"Attack", true, {{kAmpA, 0.08f}}},
          {"Release", true, {{kAmpR, 0.1f}}}}},
        {V, "OT Arp Blip", "Short square + saw blip with phase reset; made for arpeggios and fast lines.", 4,
         {{kVolume, 0.86f}, {kShape1, 1.0f}, {kShape2, 0.5f}, {kTune2, 12.0f}, {kOscMix, -0.3f}, {kReset1, 1.0f}, {kReset2, 1.0f},
          {kFilterType, float(kLpSvf12)}, {kCutoff, 1800.0f}, {kResonance, 0.45f}, {kFilterEnvAmt, 0.25f},
          {kEnv1A, 0.0f}, {kEnv1D, 0.12f}, {kEnv1S, 0.0f}, {kEnv1R, 0.1f}, {kAmpA, 0.0f}, {kAmpD, 0.22f},
          {kAmpS, 0.0f}, {kAmpR, 0.12f}},
         {{"Cutoff", true, {{kCutoff, cut(1500)}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Env Amt", true, {{kFilterEnvAmt, 0.15f}}},
          {"Length", true, {{kAmpD, 0.015f}, {kEnv1D, 0.01f}}},
          {"Octave", true, {{kOscMix, 0.4f}}},
          {"Release", true, {{kAmpR, 0.02f}}}}},
        {V, "OT Unison Lead", "Wide detuned-saw mono lead with glide and vibrato.", 1,
         {{kVolume, 0.76f}, {kShape1, 0.5f}, {kShape2, 0.5f}, {kDetune, 32.0f}, {kFilterType, float(kLpOb24)}, {kCutoff, 3200.0f},
          {kResonance, 0.2f}, {kFilterEnvAmt, 0.12f}, {kEnv1D, 0.5f}, {kEnv1S, 0.5f}, {kAmpA, 0.004f},
          {kAmpD, 0.5f}, {kAmpS, 1.0f}, {kAmpR, 0.3f}, {kGlideMode, 1.0f}, {kGlideTime, 0.07f},
          {kLfo1Ratio, 5.5f}, {kLfo1Wave, float(kWSine)}, {kLfo1Retrig, float(kRNote)},
          MOD(0, kSrcLfo1, kDstPitch1, 0.003f), MOD(1, kSrcLfo1, kDstPitch2, 0.003f)},
         {{"Cutoff", true, {{kCutoff, cut(2500)}}},
          {"Detune", true, {{kDetune, 0.25f}}},
          {"Vibrato", false, {{modAmt(0), 0.002f}, {modAmt(1), 0.002f}}},
          {"Glide", false, {{kGlideTime, 0.05f}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Release", true, {{kAmpR, 0.03f}}}}},
        {V, "OT Dark Drone", "Low resonant drone, very slow filter and pitch drift; hold a note.", 4,
         {{kVolume, 0.92f}, {kShape1, 0.5f}, {kShape2, 0.6f}, {kTune2, -12.0f}, {kDetune, 12.0f}, {kFilterType, float(kLpMg24)},
          {kCutoff, 280.0f}, {kResonance, 0.9f}, {kFilterEnvAmt, 0.0f}, {kLfo1Ratio, 0.07f},
          {kLfo1Wave, float(kWTri)}, {kLfo1Retrig, float(kRFree)}, {kLfo2Ratio, 0.2f}, {kLfo2Wave, float(kWSine)},
          {kLfo2Retrig, float(kRFree)}, {kAmpA, 1.5f}, {kAmpD, 1.0f}, {kAmpS, 1.0f}, {kAmpR, 3.0f},
          MOD(0, kSrcLfo1, kDstCutoff, 0.12f), MOD(1, kSrcLfo2, kDstPitch2, 0.004f)},
         {{"Cutoff", true, {{kCutoff, cut(500)}}},
          {"Resonance", true, {{kResonance, 0.2f}}},
          {"Motion", false, {{modAmt(0), 0.03f}}},
          {"Attack", true, {{kAmpA, 0.1f}}},
          {"Release", true, {{kAmpR, 0.2f}}},
          {"Detune", true, {{kDetune, 0.12f}}}}},
        {V, "OT Organ Keys", "Square + triangle an octave up, instant on/off, light vibrato.", 4,
         {{kVolume, 0.7f}, {kShape1, 1.0f}, {kShape2, 0.0f}, {kTune2, 12.0f}, {kOscMix, 0.2f}, {kFilterType, float(kLpSvf12)},
          {kCutoff, 5000.0f}, {kResonance, 0.0f}, {kFilterEnvAmt, 0.0f}, {kAmpA, 0.004f}, {kAmpD, 1.0f},
          {kAmpS, 1.0f}, {kAmpR, 0.06f}, {kLfo1Ratio, 6.0f}, {kLfo1Wave, float(kWSine)},
          {kLfo1Retrig, float(kRFree)}, MOD(0, kSrcLfo1, kDstPitch1, 0.002f), MOD(1, kSrcLfo1, kDstPitch2, 0.002f)},
         {{"Tone", true, {{kCutoff, cut(2000)}}},
          {"Drawbar", true, {{kOscMix, 0.4f}}},
          {"Vibrato", false, {{modAmt(0), 0.002f}, {modAmt(1), 0.002f}}},
          {"Shape", true, {{kShape1, 0.3f}}},
          {"Release", true, {{kAmpR, 0.03f}}},
          {"Detune", true, {{kDetune, 0.1f}}}}},
    };
}
#undef MOD
#endif  // OT_MOD_VAP1

#if defined(OT_MOD_PRC1)
// ---------------------------------------------------------------------------
// PERC kits. Notes: kick 60, snare 62, hats 64..67 (closed .. open), cymbal
// 69, perc 71, every other note the tom at that pitch. Body Wave index =
// family x 11 + position (families: 0 Sat-Sin, 1 Triangle, 2 Square,
// 3 Square FM, 4-7 FM 1/3/7/11, 8-11 tzFM, 12-15 Glitch 1-4). Kick models
// 0 Classic8, 1 Synced8, 2 Classic9; snare 0 Classic8 .. 4 Modern+; hat and
// cymbal 0 Classic, 1 FM; perc 0 Clap, 1 Clap+, 2 Maracas, 3 Shaker,
// 4 Cowbell; waveshaper 0 Saturate, 1 Overdrive, 2 Overdrive+, 3 Bitcrush,
// 4 Bitcrush+.

namespace pc = ot::audio::perc;
constexpr int lvl(int drum, int field) { return pc::kMixer + 5 * drum + field; }   // 0 level 1 pan 2 delay 3 reverb
constexpr float wave(int family, int pos) { return float(family * 11 + pos); }

// The same six macros on every kit (all bipolar: 63 = the kit as saved).
std::vector<Macro> kitMacros() {
    using namespace pc;
    return {
        {"Tone", true,
         {{kSnareTone, 0.4f}, {kHatLpf, 0.25f}, {kCymBodyF, 0.2f}, {kPercTimbre, 0.3f}, {kKickTrTimbre, 0.3f},
          {kSnareSnapTimbre, 0.3f}}},
        {"Decay", true,
         {{kKickDecay, 0.04f}, {kSnareDecay, 0.02f}, {kHatOpen, 0.05f}, {kCymBodyDecay, 0.1f}, {kTomDecay, 0.04f},
          {kPercDecay, 0.15f}}},
        {"Pitch", true, {{kKickPitch, 0.06f}, {kSnarePitch, 0.06f}, {kHatPitch, 0.2f}, {kCymPitch, 0.2f}}},
        {"Snap", true, {{kSnareMix, 0.4f}, {kSnareSnapDecay, 0.02f}, {kKickTrLevel, 0.4f}, {kTomTrLevel, 0.4f}}},
        {"Drive", true, {{kWsLevel, 0.4f}}},
        {"Space", true,
         {{lvl(kSnare, 3), 0.3f}, {lvl(kHat, 2), 0.3f}, {lvl(kCymbal, 3), 0.3f}, {lvl(kPerc, 2), 0.3f},
          {lvl(kTom, 3), 0.3f}}},
    };
}

std::vector<Preset> synthwaveKits();

std::vector<Preset> percPresets() {
    using namespace pc;
    const SynthModel P = SynthModel::PERC;
    std::vector<Preset> v = {
        {P, "OT Kit Boom", "Analog-style boom kit: long sine kick with a pitch drop, soft snare, clap.", 1,
         {{lvl(kKick, 0), 0.85f}, {lvl(kTom, 0), 0.68f}, {lvl(kSnare, 0), 0.85f},
          {lvl(kHat, 0), 0.85f}, {lvl(kCymbal, 0), 0.85f}, {lvl(kPerc, 0), 0.85f},
          {kKickWave, wave(0, 0)}, {kKickPitch, 29.0f}, {kKickAttack, 1.0f}, {kKickDecay, 1.2f}, {kKickModel, 2.0f},
          {kKickModAmount, 0.15f}, {kKickModDecay, 0.05f}, {kKickTrLevel, 0.25f}, {kKickTrTimbre, 0.2f},
          {kTomWave, wave(0, 0)}, {kTomModel, 0.0f}, {kTomDecay, 0.5f}, {kTomPitchMod, 1.0f}, {kTomTrLevel, 0.3f},
          {kSnareModel, 1.0f}, {kSnarePitch, 52.0f}, {kSnareTone, 0.5f}, {kSnareDecay, 0.18f},
          {kSnareSnapDecay, 0.2f}, {kSnareMix, -0.1f}, {kSnareSnapTimbre, 0.35f}, {kHatModel, 0.0f},
          {kHatTimbre, 12.0f}, {kHatPitch, 320.0f}, {kHatClosed, 0.07f}, {kHatOpen, 0.55f}, {kHatHpf, 6500.0f},
          {kHatLpf, 12000.0f}, {kHatMix, -0.3f}, {kCymModel, 0.0f}, {kCymTimbre, 12.0f}, {kCymBodyDecay, 2.5f},
          {kPercModel, 0.0f}, {kPercDecay, 0.3f}, {kPercVariation, 0.3f}, {kPercTimbre, 0.55f},
          {lvl(kPerc, 3), 0.15f}, {lvl(kSnare, 2), 0.0f}, {lvl(kHat, 2), 0.0f}, {lvl(kCymbal, 2), 0.0f},
          {lvl(kPerc, 2), 0.0f}, {kEqLow, 2.0f}},
         kitMacros()},
        {P, "OT Kit Punch", "Punchy club kit: clicky kick, bright snare, noisy hats, saturated bus.", 1,
         {{kKickWave, wave(1, 3)}, {kKickPitch, 33.0f}, {kKickAttack, 0.5f}, {kKickDecay, 0.38f},
          {kKickModel, 0.0f}, {kKickModAmount, 0.5f}, {kKickModDecay, 0.04f}, {kKickTrTimbre, -0.5f},
          {kKickTrLevel, 0.7f}, {kSnareModel, 3.0f}, {kSnarePitch, 56.0f}, {kSnareTone, 0.6f},
          {kSnareDecay, 0.14f}, {kSnareSnapDecay, 0.22f}, {kSnareMix, 0.25f}, {kSnareSnapTimbre, 0.6f},
          {kHatModel, 0.0f}, {kHatTimbre, 14.0f}, {kHatClosed, 0.05f}, {kHatOpen, 0.45f}, {kHatHpf, 7500.0f},
          {kHatLpf, 14000.0f}, {kHatMix, -0.6f}, {kCymModel, 0.0f}, {kCymTimbre, 16.0f}, {kCymBodyDecay, 1.8f},
          {kCymImpactDecay, 0.3f}, {kPercModel, 1.0f}, {kPercDecay, 0.25f}, {lvl(kSnare, 2), 0.0f},
          {lvl(kHat, 2), 0.0f}, {lvl(kCymbal, 2), 0.0f}, {lvl(kPerc, 2), 0.0f}, {lvl(kPerc, 3), 0.1f},
          {kWsType, 0.0f}, {kWsLevel, 0.25f}},
         kitMacros()},
        {P, "OT Kit Dusty", "Lo-fi kit: 6-bit crush, rolled-off highs, warm low end.", 1,
         {{kKickWave, wave(0, 0)}, {kKickPitch, 30.0f}, {kKickDecay, 0.45f}, {kKickModel, 2.0f},
          {kSnareModel, 0.0f}, {kSnarePitch, 50.0f}, {kSnareDecay, 0.2f}, {kSnareTone, 0.4f}, {kSnareMix, -0.2f},
          {kSnareSnapTimbre, 0.25f}, {kHatTimbre, 8.0f}, {kHatClosed, 0.06f}, {kHatOpen, 0.35f},
          {kHatLpf, 7000.0f}, {kHatHpf, 3000.0f}, {kCymBodyF, 2000.0f}, {kCymBodyDecay, 1.2f},
          {kPercModel, 2.0f}, {kPercDecay, 0.12f}, {lvl(kSnare, 2), 0.0f}, {lvl(kSnare, 3), 0.2f},
          {lvl(kHat, 2), 0.0f}, {lvl(kCymbal, 2), 0.0f}, {lvl(kPerc, 2), 0.0f}, {kWsType, 3.0f},
          {kWsLevel, 0.5f}, {kEqLow, 3.0f}, {kEqMid, 2.0f}, {kEqHigh, -6.0f}},
         kitMacros()},
        {P, "OT Kit Foundry", "Industrial kit: folded kick, resonant toms, hard overdrive, metal hats.", 1,
         {{kKickWave, wave(14, 6)}, {kKickPitch, 27.0f}, {kKickDecay, 0.5f}, {kKickModel, 1.0f},
          {kKickModAmount, 0.6f}, {kKickModDecay, 0.1f}, {kKickTrTimbre, -0.8f}, {kKickTrLevel, 0.8f},
          {kTomWave, wave(5, 6)}, {kTomModel, 2.0f}, {kTomDecay, 0.6f}, {kTomPitchMod, -3.0f},
          {kSnareModel, 4.0f}, {kSnarePitch, 45.0f}, {kSnareDecay, 0.3f}, {kSnareSnapDecay, 0.35f},
          {kSnareMix, 0.3f}, {kSnareSnapTimbre, 0.8f}, {kHatModel, 1.0f}, {kHatTimbre, 19.0f},
          {kHatClosed, 0.08f}, {kHatOpen, 0.7f}, {kHatHpf, 4000.0f}, {kHatLpf, 16000.0f}, {kCymModel, 1.0f},
          {kCymTimbre, 21.0f}, {kCymBodyDecay, 3.0f}, {kPercModel, 4.0f}, {kPercTimbre, 0.3f},
          {kPercDecay, 0.35f}, {lvl(kSnare, 2), 0.0f}, {lvl(kSnare, 3), 0.25f}, {lvl(kHat, 2), 0.0f},
          {lvl(kCymbal, 2), 0.0f}, {lvl(kCymbal, 3), 0.3f}, {lvl(kPerc, 2), 0.0f}, {kWsType, 2.0f},
          {kWsLevel, 0.55f}, {kEqMid, 3.0f}, {kEqLow, -1.0f}},
         kitMacros()},
        {P, "OT Kit FM Metal", "FM kit: phase-modulated kick, toms and snare, FM hats and cowbell.", 1,
         {{lvl(kKick, 0), 0.70f}, {lvl(kTom, 0), 0.56f}, {lvl(kSnare, 0), 0.70f},
          {lvl(kHat, 0), 0.70f}, {lvl(kCymbal, 0), 0.70f}, {lvl(kPerc, 0), 0.70f},
          {kKickWave, wave(4, 5)}, {kKickPitch, 31.0f}, {kKickDecay, 0.35f}, {kKickModel, 0.0f},
          {kKickModAmount, 0.7f}, {kKickModDecay, 0.08f}, {kTomWave, wave(6, 5)}, {kTomModel, 0.0f},
          {kTomDecay, 0.4f}, {kTomPitchMod, 2.0f}, {kTomWaveMod, 0.7f}, {kSnareWave, wave(5, 7)},
          {kSnareModel, 3.0f}, {kSnarePitch, 58.0f}, {kSnareTone, 0.7f}, {kSnareDecay, 0.15f},
          {kSnareMix, -0.3f}, {kHatModel, 1.0f}, {kHatTimbre, 5.0f}, {kHatClosed, 0.06f}, {kHatOpen, 0.5f},
          {kCymModel, 1.0f}, {kCymTimbre, 10.0f}, {kCymPitch, 330.0f}, {kCymBodyDecay, 2.0f},
          {kPercModel, 4.0f}, {kPercTimbre, 0.7f}, {kPercDecay, 0.2f}, {lvl(kSnare, 2), 0.0f},
          {lvl(kHat, 2), 0.25f}, {lvl(kCymbal, 2), 0.0f}, {lvl(kPerc, 2), 0.4f}},
         kitMacros()},
        {P, "OT Kit Minimal", "Minimal kit: tight kick, rimshot-like snare, short hats, shaker on delay.", 1,
         {{kKickWave, wave(0, 0)}, {kKickPitch, 32.0f}, {kKickDecay, 0.28f}, {kKickModel, 2.0f},
          {kKickModAmount, 0.1f}, {kKickTrLevel, 0.3f}, {kKickTrTimbre, 0.5f}, {kTomModel, 0.0f},
          {kTomDecay, 0.25f}, {kSnareModel, 3.0f}, {kSnarePitch, 70.0f}, {kSnareTone, 0.8f},
          {kSnareDecay, 0.05f}, {kSnareSnapDecay, 0.06f}, {kSnareMix, -0.6f}, {kSnareSnapTimbre, 0.7f},
          {kHatShape, 0.3f}, {kHatTimbre, 12.0f}, {kHatClosed, 0.03f}, {kHatOpen, 0.25f}, {kHatHpf, 9000.0f},
          {kHatLpf, 15000.0f}, {kCymBodyDecay, 0.8f}, {lvl(kCymbal, 0), 0.7f}, {kPercModel, 3.0f},
          {kPercDecay, 0.1f}, {kPercTimbre, 0.7f}, {lvl(kSnare, 2), 0.4f}, {lvl(kHat, 2), 0.2f},
          {lvl(kCymbal, 2), 0.0f}, {lvl(kPerc, 2), 0.5f}, {kEqLow, 1.0f}},
         kitMacros()},
        {P, "OT Kit Studio", "Tight acoustic-like kit: beater kick, ringing toms, crisp snare, washy cymbal.", 1,
         {{lvl(kKick, 0), 0.70f}, {lvl(kTom, 0), 0.56f}, {lvl(kSnare, 0), 0.70f},
          {lvl(kHat, 0), 0.70f}, {lvl(kCymbal, 0), 0.70f}, {lvl(kPerc, 0), 0.70f},
          {kKickWave, wave(0, 3)}, {kKickPitch, 35.0f}, {kKickDecay, 0.3f}, {kKickModel, 2.0f},
          {kKickModAmount, 0.12f}, {kKickModDecay, 0.03f}, {kKickTrTimbre, -0.3f}, {kKickTrLevel, 0.55f},
          {kTomModel, 1.0f}, {kTomDecay, 0.55f}, {kTomPitchMod, 1.2f}, {kTomTrLevel, 0.8f}, {kSnareModel, 2.0f},
          {kSnarePitch, 54.0f}, {kSnareTone, 0.45f}, {kSnareDecay, 0.2f}, {kSnareSnapAttack, 0.5f},
          {kSnareSnapDecay, 0.3f}, {kSnareMix, 0.35f}, {kSnareSnapTimbre, 0.55f}, {kHatModel, 0.0f},
          {kHatShape, 0.2f}, {kHatTimbre, 20.0f}, {kHatPitch, 420.0f}, {kHatClosed, 0.09f}, {kHatOpen, 0.9f},
          {kHatHpf, 6000.0f}, {kHatLpf, 13000.0f}, {kHatMix, -0.5f}, {kCymModel, 0.0f}, {kCymTimbre, 18.0f},
          {kCymPitch, 450.0f}, {kCymBodyF, 5000.0f}, {kCymImpactF, 8000.0f}, {kCymBodyDecay, 3.5f},
          {kCymMix, -0.6f}, {kPercModel, 1.0f}, {kPercDecay, 0.2f}, {lvl(kKick, 3), 0.05f},
          {lvl(kTom, 3), 0.15f}, {lvl(kSnare, 2), 0.0f}, {lvl(kSnare, 3), 0.2f}, {lvl(kHat, 2), 0.0f},
          {lvl(kCymbal, 2), 0.0f}, {lvl(kCymbal, 3), 0.2f}, {lvl(kPerc, 2), 0.0f}, {kEqHigh, 2.0f}},
         kitMacros()},
        {P, "OT Kit Glitch", "Glitch kit: bit-twisted bodies, crushed bus, delays on hats and shaker.", 1,
         {{kKickWave, wave(15, 5)}, {kKickPitch, 30.0f}, {kKickDecay, 0.3f}, {kKickModel, 1.0f},
          {kKickModAmount, -0.6f}, {kKickModDecay, 0.05f}, {kTomWave, wave(12, 8)}, {kTomModel, 0.0f},
          {kTomDecay, 0.3f}, {kTomPitchMod, 6.0f}, {kTomWaveMod, -0.8f}, {kSnareWave, wave(13, 7)},
          {kSnareModel, 4.0f}, {kSnarePitch, 60.0f}, {kSnareDecay, 0.12f}, {kSnareMix, 0.0f},
          {kSnareSnapTimbre, 1.0f}, {kHatModel, 1.0f}, {kHatTimbre, 24.0f}, {kHatClosed, 0.04f},
          {kHatOpen, 0.3f}, {kHatHpf, 2000.0f}, {kCymModel, 1.0f}, {kCymTimbre, 23.0f}, {kCymBodyDecay, 1.0f},
          {kPercModel, 3.0f}, {kPercVariation, 0.9f}, {kPercDecay, 0.06f}, {lvl(kSnare, 2), 0.3f},
          {lvl(kHat, 2), 0.5f}, {lvl(kCymbal, 2), 0.0f}, {lvl(kPerc, 2), 0.6f}, {kWsType, 4.0f},
          {kWsLevel, 0.35f}},
         kitMacros()},
    };
    for (Preset& p : synthwaveKits()) v.push_back(p);
    return v;
}

// ---------------------------------------------------------------------------
// Synthwave / retrowave kits ("OT Kit SW ..."). The 80s gated snare is a
// long, snap-heavy noise tail (Snap Decay 0.3-0.45 s) cut by the body's
// short decay, plus a reverb send; the toms sweep down several semitones
// (Pitch Mod) for the electronic-tom "pew". Every drum sets all four mixer
// fields; the per-drum sends only sound when the instrument's own Delay /
// Reverb send is up (send = patch send x instrument send). Each kit has its
// own six macros (all bipolar: 63 = the kit as saved).

// Macro helpers shared by the synthwave kits.
Macro swKickDecay(float a = 0.04f) {
    using namespace pc;
    return {"Kick Decay", true, {{kKickDecay, a}, {kKickModDecay, a * 0.15f}}};
}
Macro swSnareTone() {
    using namespace pc;
    return {"Snare Tone", true, {{kSnareTone, 0.35f}, {kSnareSnapTimbre, 0.3f}, {kSnarePitch, 0.03f}}};
}
Macro swGate(float a = 0.025f) {
    using namespace pc;
    return {"Snare Gate", true, {{kSnareSnapDecay, a}, {kSnareDecay, a * 0.5f}, {kPercDecay, a * 4.0f}}};
}
Macro swHatDecay() {
    using namespace pc;
    return {"Hat Decay", true, {{kHatOpen, 0.04f}, {kHatClosed, 0.004f}, {kCymBodyDecay, 0.08f}}};
}
Macro swDrive(float a = 0.35f) {
    using namespace pc;
    return {"Drive", true, {{kWsLevel, a}}};
}
Macro swRoom(float a = 0.35f) {
    using namespace pc;
    return {"Room", true,
            {{lvl(kSnare, 3), a}, {lvl(kTom, 3), a}, {lvl(kPerc, 3), a}, {lvl(kCymbal, 3), a * 0.7f},
             {lvl(kHat, 3), a * 0.4f}, {lvl(kKick, 3), a * 0.15f}}};
}
Macro swPitch() {
    using namespace pc;
    return {"Pitch", true,
            {{kKickPitch, 0.04f}, {kSnarePitch, 0.06f}, {kHatPitch, 0.12f}, {kCymPitch, 0.12f}, {kPercTimbre, 0.15f}}};
}
Macro swTomSweep() {
    using namespace pc;
    return {"Tom Sweep", true, {{kTomPitchMod, 0.12f}, {kTomModDecay, 0.012f}, {kTomDecay, 0.03f}}};
}
Macro swEcho(float a = 0.35f) {
    using namespace pc;
    return {"Echo", true, {{lvl(kHat, 2), a}, {lvl(kPerc, 2), a}, {lvl(kSnare, 2), a * 0.5f}}};
}

std::vector<Preset> synthwaveKits() {
    using namespace pc;
    const SynthModel P = SynthModel::PERC;
    return {
        {P, "OT Kit SW Gated", "80s gated kit: big snap-heavy snare with a hard-cut room tail, sweeping toms, Clap+.", 1,
         {{lvl(kKick, 0), 1.05f}, {lvl(kTom, 0), 0.62f}, {lvl(kSnare, 0), 0.70f},
          {lvl(kHat, 0), 0.62f}, {lvl(kCymbal, 0), 0.55f}, {lvl(kPerc, 0), 0.70f},
          {lvl(kKick, 1), 0.0f}, {lvl(kTom, 1), -0.15f}, {lvl(kSnare, 1), 0.0f}, {lvl(kHat, 1), 0.3f},
          {lvl(kCymbal, 1), -0.35f}, {lvl(kPerc, 1), 0.1f},
          {lvl(kKick, 2), 0.0f}, {lvl(kTom, 2), 0.0f}, {lvl(kSnare, 2), 0.0f}, {lvl(kHat, 2), 0.0f},
          {lvl(kCymbal, 2), 0.0f}, {lvl(kPerc, 2), 0.0f},
          {lvl(kKick, 3), 0.05f}, {lvl(kTom, 3), 0.4f}, {lvl(kSnare, 3), 0.65f}, {lvl(kHat, 3), 0.1f},
          {lvl(kCymbal, 3), 0.3f}, {lvl(kPerc, 3), 0.55f},
          {kKickWave, wave(0, 1)}, {kKickModel, 2.0f}, {kKickPitch, 32.0f}, {kKickAttack, 0.8f}, {kKickDecay, 0.38f},
          {kKickModAmount, 0.25f}, {kKickModDecay, 0.035f}, {kKickTrTimbre, 0.4f}, {kKickTrLevel, 0.5f},
          {kTomWave, wave(1, 2)}, {kTomModel, 1.0f}, {kTomAttack, 0.5f}, {kTomDecay, 0.6f}, {kTomTrLevel, 0.45f},
          {kTomTrTimbre, 1.2f}, {kTomPitchMod, 2.5f}, {kTomModDecay, 0.15f}, {kTomWaveMod, 0.2f},
          {kSnareModel, 2.0f}, {kSnarePitch, 55.0f}, {kSnareTone, 0.45f}, {kSnareDecay, 0.2f},
          {kSnareSnapAttack, 0.5f}, {kSnareSnapDecay, 0.42f}, {kSnareMix, 0.3f}, {kSnareSnapTimbre, 0.6f},
          {kHatModel, 0.0f}, {kHatTimbre, 12.0f}, {kHatPitch, 330.0f}, {kHatShape, 0.25f}, {kHatClosed, 0.05f},
          {kHatOpen, 0.5f}, {kHatHpf, 7000.0f}, {kHatLpf, 15000.0f}, {kHatMix, -0.4f},
          {kCymModel, 0.0f}, {kCymTimbre, 14.0f}, {kCymBodyDecay, 2.5f}, {kCymMix, -0.4f},
          {kPercModel, 1.0f}, {kPercVariation, 0.4f}, {kPercTimbre, 0.5f}, {kPercDecay, 0.35f},
          {kWsType, 0.0f}, {kWsLevel, 0.2f}, {kEqHigh, 2.0f}},
         {swKickDecay(), swGate(), swSnareTone(), swHatDecay(), swDrive(), swRoom()}},

        {P, "OT Kit SW Outrun", "Driving outrun kit: tight clicky kick, bright gated snare, open hats on delay, cowbell.", 1,
         {{lvl(kKick, 0), 1.05f}, {lvl(kTom, 0), 0.60f}, {lvl(kSnare, 0), 0.75f},
          {lvl(kHat, 0), 0.65f}, {lvl(kCymbal, 0), 0.55f}, {lvl(kPerc, 0), 0.45f},
          {lvl(kKick, 1), 0.0f}, {lvl(kTom, 1), 0.2f}, {lvl(kSnare, 1), -0.05f}, {lvl(kHat, 1), 0.35f},
          {lvl(kCymbal, 1), -0.3f}, {lvl(kPerc, 1), -0.4f},
          {lvl(kKick, 2), 0.0f}, {lvl(kTom, 2), 0.0f}, {lvl(kSnare, 2), 0.0f}, {lvl(kHat, 2), 0.3f},
          {lvl(kCymbal, 2), 0.0f}, {lvl(kPerc, 2), 0.35f},
          {lvl(kKick, 3), 0.0f}, {lvl(kTom, 3), 0.35f}, {lvl(kSnare, 3), 0.5f}, {lvl(kHat, 3), 0.1f},
          {lvl(kCymbal, 3), 0.25f}, {lvl(kPerc, 3), 0.2f},
          {kKickWave, wave(1, 2)}, {kKickModel, 0.0f}, {kKickPitch, 33.0f}, {kKickAttack, 0.5f}, {kKickDecay, 0.3f},
          {kKickModAmount, 0.45f}, {kKickModDecay, 0.03f}, {kKickTrTimbre, 0.6f}, {kKickTrLevel, 0.6f},
          {kTomWave, wave(1, 3)}, {kTomModel, 2.0f}, {kTomDecay, 0.5f}, {kTomTrLevel, 0.4f}, {kTomTrTimbre, 1.0f},
          {kTomPitchMod, 2.0f}, {kTomModDecay, 0.12f},
          {kSnareModel, 4.0f}, {kSnarePitch, 57.0f}, {kSnareTone, 0.4f}, {kSnareDecay, 0.16f},
          {kSnareSnapAttack, 0.3f}, {kSnareSnapDecay, 0.32f}, {kSnareMix, 0.35f}, {kSnareSnapTimbre, 0.72f},
          {kHatModel, 0.0f}, {kHatTimbre, 16.0f}, {kHatPitch, 380.0f}, {kHatShape, 0.15f}, {kHatClosed, 0.045f},
          {kHatOpen, 0.45f}, {kHatHpf, 8000.0f}, {kHatLpf, 16000.0f}, {kHatMix, -0.5f},
          {kCymModel, 0.0f}, {kCymTimbre, 16.0f}, {kCymBodyDecay, 2.2f}, {kCymImpactDecay, 0.3f},
          {kPercModel, 4.0f}, {kPercVariation, 0.35f}, {kPercTimbre, 0.55f}, {kPercDecay, 0.2f},
          {kWsType, 1.0f}, {kWsLevel, 0.15f}, {kEqLow, 1.5f}, {kEqHigh, 2.5f}},
         {swKickDecay(0.03f), swSnareTone(), swHatDecay(), swEcho(), swDrive(), swRoom()}},

        {P, "OT Kit SW Darksynth", "Dark heavy kit: saturated low kick, huge snare, resonant toms, hard overdrive.", 1,
         {{lvl(kKick, 0), 0.85f}, {lvl(kTom, 0), 0.55f}, {lvl(kSnare, 0), 0.70f},
          {lvl(kHat, 0), 0.55f}, {lvl(kCymbal, 0), 0.50f}, {lvl(kPerc, 0), 0.60f},
          {lvl(kKick, 1), 0.0f}, {lvl(kTom, 1), -0.2f}, {lvl(kSnare, 1), 0.0f}, {lvl(kHat, 1), 0.25f},
          {lvl(kCymbal, 1), 0.3f}, {lvl(kPerc, 1), -0.15f},
          {lvl(kKick, 2), 0.0f}, {lvl(kTom, 2), 0.0f}, {lvl(kSnare, 2), 0.0f}, {lvl(kHat, 2), 0.0f},
          {lvl(kCymbal, 2), 0.0f}, {lvl(kPerc, 2), 0.0f},
          {lvl(kKick, 3), 0.0f}, {lvl(kTom, 3), 0.45f}, {lvl(kSnare, 3), 0.7f}, {lvl(kHat, 3), 0.15f},
          {lvl(kCymbal, 3), 0.4f}, {lvl(kPerc, 3), 0.6f},
          {kKickWave, wave(0, 6)}, {kKickModel, 2.0f}, {kKickPitch, 28.0f}, {kKickAttack, 1.0f}, {kKickDecay, 0.55f},
          {kKickModAmount, 0.3f}, {kKickModDecay, 0.045f}, {kKickTrTimbre, -0.6f}, {kKickTrLevel, 0.55f},
          {kTomWave, wave(0, 5)}, {kTomModel, 2.0f}, {kTomDecay, 0.75f}, {kTomTrLevel, 0.5f}, {kTomTrTimbre, 0.8f},
          {kTomPitchMod, 3.0f}, {kTomModDecay, 0.2f},
          {kSnareModel, 4.0f}, {kSnarePitch, 50.0f}, {kSnareTone, 0.35f}, {kSnareDecay, 0.25f},
          {kSnareSnapAttack, 0.8f}, {kSnareSnapDecay, 0.45f}, {kSnareMix, 0.2f}, {kSnareSnapTimbre, 0.45f},
          {kHatModel, 1.0f}, {kHatTimbre, 7.0f}, {kHatPitch, 280.0f}, {kHatShape, 0.2f}, {kHatClosed, 0.06f},
          {kHatOpen, 0.6f}, {kHatHpf, 5500.0f}, {kHatLpf, 11000.0f}, {kHatMix, -0.3f},
          {kCymModel, 1.0f}, {kCymTimbre, 9.0f}, {kCymBodyDecay, 3.0f}, {kCymBodyF, 2500.0f},
          {kPercModel, 1.0f}, {kPercVariation, 0.6f}, {kPercTimbre, 0.35f}, {kPercDecay, 0.45f},
          {kWsType, 2.0f}, {kWsLevel, 0.35f}, {kEqLow, 3.0f}, {kEqMid, -2.0f}, {kEqHigh, 1.0f}},
         {swKickDecay(0.05f), swGate(0.03f), swTomSweep(), swHatDecay(), swDrive(0.3f), swRoom(0.3f)}},

        {P, "OT Kit SW Vapor", "Vaporwave lo-fi kit: soft round kick, washed-out snare, dull hats, 6-bit crush.", 1,
         {{lvl(kKick, 0), 0.95f}, {lvl(kTom, 0), 0.62f}, {lvl(kSnare, 0), 0.80f},
          {lvl(kHat, 0), 0.75f}, {lvl(kCymbal, 0), 0.80f}, {lvl(kPerc, 0), 0.75f},
          {lvl(kKick, 1), 0.0f}, {lvl(kTom, 1), 0.15f}, {lvl(kSnare, 1), 0.0f}, {lvl(kHat, 1), -0.25f},
          {lvl(kCymbal, 1), 0.3f}, {lvl(kPerc, 1), 0.25f},
          {lvl(kKick, 2), 0.0f}, {lvl(kTom, 2), 0.0f}, {lvl(kSnare, 2), 0.25f}, {lvl(kHat, 2), 0.2f},
          {lvl(kCymbal, 2), 0.0f}, {lvl(kPerc, 2), 0.45f},
          {lvl(kKick, 3), 0.1f}, {lvl(kTom, 3), 0.5f}, {lvl(kSnare, 3), 0.8f}, {lvl(kHat, 3), 0.3f},
          {lvl(kCymbal, 3), 0.5f}, {lvl(kPerc, 3), 0.6f},
          {kKickWave, wave(0, 0)}, {kKickModel, 2.0f}, {kKickPitch, 30.0f}, {kKickAttack, 2.5f}, {kKickDecay, 0.5f},
          {kKickModAmount, 0.12f}, {kKickModDecay, 0.05f}, {kKickTrTimbre, 0.15f}, {kKickTrLevel, 0.2f},
          {kTomWave, wave(0, 0)}, {kTomModel, 0.0f}, {kTomDecay, 0.55f}, {kTomPitchMod, 1.2f}, {kTomModDecay, 0.1f},
          {kSnareModel, 0.0f}, {kSnarePitch, 52.0f}, {kSnareTone, 0.5f}, {kSnareDecay, 0.22f},
          {kSnareSnapAttack, 1.5f}, {kSnareSnapDecay, 0.35f}, {kSnareMix, 0.1f}, {kSnareSnapTimbre, 0.3f},
          {kHatModel, 0.0f}, {kHatTimbre, 9.0f}, {kHatPitch, 260.0f}, {kHatShape, 0.4f}, {kHatClosed, 0.07f},
          {kHatOpen, 0.6f}, {kHatHpf, 3500.0f}, {kHatLpf, 7500.0f}, {kHatMix, -0.2f},
          {kCymModel, 0.0f}, {kCymTimbre, 10.0f}, {kCymBodyF, 2200.0f}, {kCymImpactF, 4000.0f},
          {kCymBodyDecay, 3.0f},
          {kPercModel, 0.0f}, {kPercVariation, 0.5f}, {kPercTimbre, 0.4f}, {kPercDecay, 0.4f},
          {kWsType, 3.0f}, {kWsLevel, 0.4f}, {kEqLow, 2.0f}, {kEqMid, -1.0f}, {kEqHigh, -6.0f}},
         {swKickDecay(), swSnareTone(), swHatDecay(), {"Crush", true, {{kWsLevel, 0.4f}}}, swEcho(0.3f),
          swRoom(0.2f)}},

        {P, "OT Kit SW Hex Toms", "Electronic tom kit: singing pitch-sweep toms with stick noise, ping snare, clap.", 1,
         {{lvl(kKick, 0), 1.05f}, {lvl(kTom, 0), 0.70f}, {lvl(kSnare, 0), 0.75f},
          {lvl(kHat, 0), 0.60f}, {lvl(kCymbal, 0), 0.55f}, {lvl(kPerc, 0), 0.65f},
          {lvl(kKick, 1), 0.0f}, {lvl(kTom, 1), 0.25f}, {lvl(kSnare, 1), 0.0f}, {lvl(kHat, 1), 0.35f},
          {lvl(kCymbal, 1), -0.35f}, {lvl(kPerc, 1), -0.1f},
          {lvl(kKick, 2), 0.0f}, {lvl(kTom, 2), 0.15f}, {lvl(kSnare, 2), 0.0f}, {lvl(kHat, 2), 0.0f},
          {lvl(kCymbal, 2), 0.0f}, {lvl(kPerc, 2), 0.0f},
          {lvl(kKick, 3), 0.0f}, {lvl(kTom, 3), 0.5f}, {lvl(kSnare, 3), 0.55f}, {lvl(kHat, 3), 0.1f},
          {lvl(kCymbal, 3), 0.3f}, {lvl(kPerc, 3), 0.45f},
          {kKickWave, wave(0, 0)}, {kKickModel, 2.0f}, {kKickPitch, 31.0f}, {kKickAttack, 0.5f}, {kKickDecay, 0.4f},
          {kKickModAmount, 0.35f}, {kKickModDecay, 0.03f}, {kKickTrTimbre, 0.5f}, {kKickTrLevel, 0.45f},
          {kTomWave, wave(1, 3)}, {kTomModel, 1.0f}, {kTomAttack, 0.3f}, {kTomDecay, 0.85f}, {kTomTrLevel, 0.7f},
          {kTomTrTimbre, 1.5f}, {kTomPitchMod, 4.0f}, {kTomModDecay, 0.28f}, {kTomWaveMod, 0.3f},
          {kSnareModel, 3.0f}, {kSnarePitch, 60.0f}, {kSnareTone, 0.4f}, {kSnareDecay, 0.2f},
          {kSnareSnapAttack, 0.3f}, {kSnareSnapDecay, 0.3f}, {kSnareMix, 0.15f}, {kSnareSnapTimbre, 0.55f},
          {kHatModel, 0.0f}, {kHatTimbre, 12.0f}, {kHatPitch, 340.0f}, {kHatClosed, 0.05f}, {kHatOpen, 0.45f},
          {kHatHpf, 7500.0f}, {kHatLpf, 14000.0f}, {kHatMix, -0.4f},
          {kCymModel, 0.0f}, {kCymTimbre, 14.0f}, {kCymBodyDecay, 2.2f},
          {kPercModel, 0.0f}, {kPercVariation, 0.35f}, {kPercTimbre, 0.5f}, {kPercDecay, 0.3f},
          {kWsType, 0.0f}, {kWsLevel, 0.2f}, {kEqHigh, 1.5f}},
         {swTomSweep(), {"Tom Decay", true, {{kTomDecay, 0.05f}}}, {"Tom Stick", true, {{kTomTrLevel, 0.4f},
          {kTomTrTimbre, 0.3f}}}, swSnareTone(), swDrive(), swRoom()}},

        {P, "OT Kit SW Electro", "Electro kit in the classic analog drum machine style: long boomy kick, snappy snare, ticking hats, roomy clap.", 1,
         {{lvl(kKick, 0), 1.00f}, {lvl(kTom, 0), 0.62f}, {lvl(kSnare, 0), 0.72f},
          {lvl(kHat, 0), 0.70f}, {lvl(kCymbal, 0), 0.75f}, {lvl(kPerc, 0), 0.90f},
          {lvl(kKick, 1), 0.0f}, {lvl(kTom, 1), -0.2f}, {lvl(kSnare, 1), 0.05f}, {lvl(kHat, 1), 0.3f},
          {lvl(kCymbal, 1), -0.3f}, {lvl(kPerc, 1), -0.05f},
          {lvl(kKick, 2), 0.0f}, {lvl(kTom, 2), 0.0f}, {lvl(kSnare, 2), 0.0f}, {lvl(kHat, 2), 0.15f},
          {lvl(kCymbal, 2), 0.0f}, {lvl(kPerc, 2), 0.2f},
          {lvl(kKick, 3), 0.0f}, {lvl(kTom, 3), 0.3f}, {lvl(kSnare, 3), 0.35f}, {lvl(kHat, 3), 0.05f},
          {lvl(kCymbal, 3), 0.2f}, {lvl(kPerc, 3), 0.5f},
          {kKickWave, wave(0, 0)}, {kKickModel, 2.0f}, {kKickPitch, 29.0f}, {kKickAttack, 1.0f}, {kKickDecay, 0.9f},
          {kKickModAmount, 0.2f}, {kKickModDecay, 0.05f}, {kKickTrTimbre, 0.3f}, {kKickTrLevel, 0.35f},
          {kTomWave, wave(0, 0)}, {kTomModel, 0.0f}, {kTomDecay, 0.45f}, {kTomPitchMod, 1.5f}, {kTomModDecay, 0.06f},
          {kSnareModel, 0.0f}, {kSnarePitch, 58.0f}, {kSnareTone, 0.6f}, {kSnareDecay, 0.15f},
          {kSnareSnapDecay, 0.18f}, {kSnareMix, 0.15f}, {kSnareSnapTimbre, 0.7f},
          {kHatModel, 0.0f}, {kHatTimbre, 12.0f}, {kHatPitch, 320.0f}, {kHatClosed, 0.035f}, {kHatOpen, 0.4f},
          {kHatHpf, 8000.0f}, {kHatLpf, 15000.0f}, {kHatMix, -0.2f},
          {kCymModel, 0.0f}, {kCymTimbre, 12.0f}, {kCymBodyDecay, 2.0f},
          {kPercModel, 0.0f}, {kPercVariation, 0.3f}, {kPercTimbre, 0.55f}, {kPercDecay, 0.28f},
          {kEqLow, 1.0f}},
         {swKickDecay(0.06f), swSnareTone(), swHatDecay(), swPitch(), swDrive(0.4f), swRoom(0.3f)}},

        {P, "OT Kit SW Drive", "Midtempo driving kit: overdriven punchy kick and snare, shaker on delay, wide toms.", 1,
         {{lvl(kKick, 0), 0.90f}, {lvl(kTom, 0), 0.55f}, {lvl(kSnare, 0), 0.68f},
          {lvl(kHat, 0), 0.60f}, {lvl(kCymbal, 0), 0.50f}, {lvl(kPerc, 0), 0.60f},
          {lvl(kKick, 1), 0.0f}, {lvl(kTom, 1), 0.3f}, {lvl(kSnare, 1), 0.0f}, {lvl(kHat, 1), -0.3f},
          {lvl(kCymbal, 1), 0.35f}, {lvl(kPerc, 1), 0.4f},
          {lvl(kKick, 2), 0.0f}, {lvl(kTom, 2), 0.0f}, {lvl(kSnare, 2), 0.0f}, {lvl(kHat, 2), 0.1f},
          {lvl(kCymbal, 2), 0.0f}, {lvl(kPerc, 2), 0.45f},
          {lvl(kKick, 3), 0.0f}, {lvl(kTom, 3), 0.35f}, {lvl(kSnare, 3), 0.5f}, {lvl(kHat, 3), 0.05f},
          {lvl(kCymbal, 3), 0.25f}, {lvl(kPerc, 3), 0.2f},
          {kKickWave, wave(2, 2)}, {kKickModel, 1.0f}, {kKickPitch, 32.0f}, {kKickAttack, 0.5f}, {kKickDecay, 0.35f},
          {kKickModAmount, 0.4f}, {kKickModDecay, 0.04f}, {kKickTrTimbre, -0.4f}, {kKickTrLevel, 0.6f},
          {kTomWave, wave(1, 4)}, {kTomModel, 1.0f}, {kTomDecay, 0.55f}, {kTomTrLevel, 0.5f}, {kTomTrTimbre, 1.1f},
          {kTomPitchMod, 2.2f}, {kTomModDecay, 0.14f},
          {kSnareModel, 4.0f}, {kSnarePitch, 54.0f}, {kSnareTone, 0.45f}, {kSnareDecay, 0.18f},
          {kSnareSnapAttack, 0.4f}, {kSnareSnapDecay, 0.3f}, {kSnareMix, 0.25f}, {kSnareSnapTimbre, 0.6f},
          {kHatModel, 0.0f}, {kHatTimbre, 14.0f}, {kHatPitch, 360.0f}, {kHatShape, 0.3f}, {kHatClosed, 0.05f},
          {kHatOpen, 0.4f}, {kHatHpf, 7000.0f}, {kHatLpf, 14000.0f}, {kHatMix, -0.5f},
          {kCymModel, 0.0f}, {kCymTimbre, 15.0f}, {kCymBodyDecay, 2.0f},
          {kPercModel, 3.0f}, {kPercVariation, 0.4f}, {kPercTimbre, 0.65f}, {kPercDecay, 0.12f},
          {kWsType, 2.0f}, {kWsLevel, 0.3f}, {kEqLow, 1.5f}, {kEqMid, 1.0f}},
         {swKickDecay(), swSnareTone(), swGate(), swEcho(), swDrive(0.3f), swRoom()}},

        {P, "OT Kit SW Studio 80", "80s studio drum-machine kit: short beater kick, crisp snare, airy hats, clap, crash.", 1,
         {{lvl(kKick, 0), 1.10f}, {lvl(kTom, 0), 0.62f}, {lvl(kSnare, 0), 0.78f},
          {lvl(kHat, 0), 0.80f}, {lvl(kCymbal, 0), 0.55f}, {lvl(kPerc, 0), 0.70f},
          {lvl(kKick, 1), 0.0f}, {lvl(kTom, 1), -0.2f}, {lvl(kSnare, 1), 0.05f}, {lvl(kHat, 1), 0.3f},
          {lvl(kCymbal, 1), -0.35f}, {lvl(kPerc, 1), -0.1f},
          {lvl(kKick, 2), 0.0f}, {lvl(kTom, 2), 0.0f}, {lvl(kSnare, 2), 0.0f}, {lvl(kHat, 2), 0.0f},
          {lvl(kCymbal, 2), 0.0f}, {lvl(kPerc, 2), 0.0f},
          {lvl(kKick, 3), 0.05f}, {lvl(kTom, 3), 0.3f}, {lvl(kSnare, 3), 0.35f}, {lvl(kHat, 3), 0.05f},
          {lvl(kCymbal, 3), 0.25f}, {lvl(kPerc, 3), 0.35f},
          {kKickWave, wave(0, 3)}, {kKickModel, 2.0f}, {kKickPitch, 34.0f}, {kKickAttack, 0.5f}, {kKickDecay, 0.26f},
          {kKickModAmount, 0.15f}, {kKickModDecay, 0.025f}, {kKickTrTimbre, -0.25f}, {kKickTrLevel, 0.6f},
          {kTomWave, wave(0, 2)}, {kTomModel, 1.0f}, {kTomDecay, 0.5f}, {kTomTrLevel, 0.6f}, {kTomTrTimbre, 1.0f},
          {kTomPitchMod, 1.0f}, {kTomModDecay, 0.08f},
          {kSnareModel, 1.0f}, {kSnarePitch, 54.0f}, {kSnareTone, 0.45f}, {kSnareDecay, 0.16f},
          {kSnareSnapAttack, 0.5f}, {kSnareSnapDecay, 0.24f}, {kSnareMix, 0.3f}, {kSnareSnapTimbre, 0.65f},
          {kHatModel, 0.0f}, {kHatTimbre, 20.0f}, {kHatPitch, 420.0f}, {kHatShape, 0.2f}, {kHatClosed, 0.06f},
          {kHatOpen, 0.7f}, {kHatHpf, 8000.0f}, {kHatLpf, 16000.0f}, {kHatMix, -0.6f},
          {kCymModel, 0.0f}, {kCymTimbre, 18.0f}, {kCymPitch, 450.0f}, {kCymBodyF, 5000.0f}, {kCymImpactF, 8000.0f},
          {kCymBodyDecay, 3.0f}, {kCymMix, -0.6f},
          {kPercModel, 0.0f}, {kPercVariation, 0.25f}, {kPercTimbre, 0.6f}, {kPercDecay, 0.25f},
          {kWsType, 0.0f}, {kWsLevel, 0.15f}, {kEqHigh, 2.0f}},
         {swKickDecay(0.03f), swSnareTone(), swHatDecay(), swPitch(), swDrive(), swRoom()}},
    };
}
#endif  // OT_MOD_PRC1

#if defined(OT_MOD_JU6S)
// ---------------------------------------------------------------------------
// JU6 (module JU6S, engine 0x81): one DCO (saw, pulse with manual or LFO
// width, sub square, noise), HPF 0-3 (0 = bass boost, 1 = flat), resonant
// 24 dB VCF with Env (bipolar), LFO, Key follow and Velocity, one ADSR for
// VCF and VCA (VCA Env or Gate), one triangle LFO with Delay, the built-in
// chorus (Off / I / II) the instrument always had: pads and strings on II,
// brass, keys, plucks and leads on I, the bass dry.

namespace ju = ot::audio::ju6;
static_assert(kJu6Engine == ju::kEngineId, "presets.h kJu6Engine");

// Macro helpers (amount x stored range per unit: Cutoff 20..20000 Hz,
// times 0..10 s, Env -1..1, levels 0..1).
Macro juCutoff(float hz) { return {"Cutoff", true, {{ju::kCutoff, cut(hz)}}}; }
Macro juReso(float a = 0.15f) { return {"Resonance", true, {{ju::kResonance, a}}}; }
Macro juEnv(float a = 0.1f) { return {"Env Amt", true, {{ju::kEnvAmt, a}}}; }
Macro juAttack(float s) { return {"Attack", true, {{ju::kAttack, s / 10.0f}}}; }
Macro juDecay(float s) { return {"Decay", true, {{ju::kDecay, s / 10.0f}}}; }
Macro juRelease(float s) { return {"Release", true, {{ju::kRelease, s / 10.0f}}}; }
Macro juPwm(float a = 0.3f) { return {"PWM", true, {{ju::kPw, a}}}; }
Macro juSub(float a = 0.4f) { return {"Sub", false, {{ju::kSub, a}}}; }
Macro juVibrato(float a = 0.15f) { return {"Vibrato", false, {{ju::kVibrato, a}}}; }
Macro juLfoFilter(float a = 0.2f) { return {"LFO > VCF", false, {{ju::kVcfLfo, a}}}; }
Macro juLfoRate(float hz, bool bipolar = true) { return {"LFO Rate", bipolar, {{ju::kLfoRate, hz / 19.9f}}}; }
Macro juPw(float a) { return {"PW", false, {{ju::kPw, a}}}; }
// Chorus 0..2: a unipolar macro up one mode per 0.5 (Off -> I -> II).
Macro juChorus(float modes = 1.0f) { return {"Chorus", false, {{ju::kChorus, 0.5f * modes}}}; }
constexpr float kChoI = float(ju::kChorusI), kChoII = float(ju::kChorusII);

std::vector<Preset> ju6Presets() {
    const SynthModel F = SynthModel::VAP;   // the Mini fallback model of the slot record
    const int32_t J = kJu6Engine;
    return {
        {F, "OT Ensemble Strings", "Saw + slow PWM ensemble pad, gentle attack and long release; Chorus II.", 4,
         {{ju::kChorus, kChoII}, {ju::kPulse, 1.0f}, {ju::kPw, 0.5f}, {ju::kPwMode, float(ju::kPwLfo)}, {ju::kSub, 0.0f}, {ju::kCutoff, 3200.0f}, {ju::kResonance, 0.1f},
          {ju::kEnvAmt, 0.12f}, {ju::kKeyFollow, 0.5f}, {ju::kAttack, 0.35f}, {ju::kDecay, 2.0f}, {ju::kSustain, 0.85f}, {ju::kRelease, 1.2f},
          {ju::kLfoRate, 0.7f}, {ju::kLfoDelay, 0.0f}, {ju::kVolume, 0.8f}},
         {juCutoff(2500), juAttack(0.3f), juRelease(0.8f), juPwm(), juEnv(0.15f), juVibrato(0.2f)}, J},
        {F, "OT Poly Brass", "Saw brass: the filter envelope opens on every chord, velocity to the VCF; Chorus I.", 4,
         {{ju::kChorus, kChoI}, {ju::kSub, 0.2f}, {ju::kCutoff, 600.0f}, {ju::kResonance, 0.2f}, {ju::kEnvAmt, 0.55f}, {ju::kKeyFollow, 0.6f}, {ju::kVelVcf, 0.5f},
          {ju::kAttack, 0.08f}, {ju::kDecay, 0.6f}, {ju::kSustain, 0.55f}, {ju::kRelease, 0.3f}},
         {juCutoff(500), juEnv(0.15f), juAttack(0.1f), juDecay(0.4f), juReso(), juChorus()}, J},
        {F, "OT PWM Pad", "Pulse only, deep slow PWM, sub underneath: the classic moving pad; Chorus II.", 4,
         {{ju::kChorus, kChoII}, {ju::kSaw, 0.0f}, {ju::kPulse, 1.0f}, {ju::kPw, 0.75f}, {ju::kPwMode, float(ju::kPwLfo)}, {ju::kSub, 0.3f}, {ju::kCutoff, 2200.0f},
          {ju::kResonance, 0.1f}, {ju::kEnvAmt, 0.1f}, {ju::kAttack, 0.6f}, {ju::kDecay, 3.0f}, {ju::kSustain, 0.9f}, {ju::kRelease, 1.5f},
          {ju::kVolume, 0.7f}, {ju::kLfoRate, 0.5f}, {ju::kLfoDelay, 0.0f}},
         {juCutoff(2000), juPwm(0.25f), juLfoRate(3.0f, false), juAttack(0.5f), juRelease(1.0f), juReso()}, J},
        {F, "OT Sub Bass", "Square + full sub through the bass-boost HPF, short filter thump; dry (Chorus Off).", 1,
         {{ju::kSaw, 0.0f}, {ju::kPulse, 1.0f}, {ju::kPw, 0.0f}, {ju::kPwMode, float(ju::kPwManual)}, {ju::kSub, 0.8f}, {ju::kHpf, 0.0f},
          {ju::kCutoff, 320.0f}, {ju::kResonance, 0.25f}, {ju::kEnvAmt, 0.25f}, {ju::kKeyFollow, 0.3f}, {ju::kAttack, 0.0f},
          {ju::kVolume, 0.7f}, {ju::kDecay, 0.35f}, {ju::kSustain, 0.6f}, {ju::kRelease, 0.12f}},
         {juCutoff(250), juReso(), juEnv(0.15f), juDecay(0.3f), juRelease(0.2f), juChorus(2.0f)}, J},
        {F, "OT Square Pluck", "Saw + square pluck: fast filter decay, no sustain; Chorus I.", 4,
         {{ju::kChorus, kChoI}, {ju::kPulse, 1.0f}, {ju::kPw, 0.2f}, {ju::kPwMode, float(ju::kPwManual)}, {ju::kSub, 0.0f}, {ju::kCutoff, 400.0f},
          {ju::kResonance, 0.35f}, {ju::kEnvAmt, 0.6f}, {ju::kKeyFollow, 0.7f}, {ju::kVelVcf, 0.3f}, {ju::kAttack, 0.0f},
          {ju::kDecay, 0.35f}, {ju::kSustain, 0.0f}, {ju::kRelease, 0.3f}},
         {juCutoff(350), juReso(), juEnv(0.15f), juDecay(0.3f), juRelease(0.3f), juPwm(0.2f)}, J},
        {F, "OT Arp Seq", "Short resonant saw blip for arpeggios and sequences; HPF 2 keeps it out of the bass; Chorus I.", 4,
         {{ju::kChorus, kChoI}, {ju::kSub, 0.3f}, {ju::kHpf, 2.0f}, {ju::kCutoff, 700.0f}, {ju::kResonance, 0.55f}, {ju::kEnvAmt, 0.5f}, {ju::kKeyFollow, 0.5f},
          {ju::kVelVcf, 0.5f}, {ju::kAttack, 0.0f}, {ju::kDecay, 0.15f}, {ju::kSustain, 0.0f}, {ju::kRelease, 0.12f}},
         {juCutoff(800), juReso(0.2f), juEnv(0.2f), juDecay(0.2f), juRelease(0.15f), juSub()}, J},
        {F, "OT Gate Organ", "Square + sub on the Gate VCA, open filter, delayed vibrato: organ-ish keys; Chorus I.", 4,
         {{ju::kChorus, kChoI}, {ju::kSaw, 0.0f}, {ju::kPulse, 1.0f}, {ju::kPw, 0.0f}, {ju::kPwMode, float(ju::kPwManual)}, {ju::kSub, 0.6f}, {ju::kHpf, 2.0f},
          {ju::kCutoff, 3500.0f}, {ju::kResonance, 0.0f}, {ju::kEnvAmt, 0.0f}, {ju::kKeyFollow, 0.5f}, {ju::kVcaMode, float(ju::kVcaGate)},
          {ju::kVolume, 0.7f}, {ju::kSustain, 1.0f}, {ju::kVibrato, 0.08f}, {ju::kLfoRate, 6.0f}, {ju::kLfoDelay, 0.3f}},
         {juCutoff(2500), juSub(0.4f), juPw(0.4f), juVibrato(0.15f), juLfoRate(3.0f), juReso(0.2f)}, J},
        {F, "OT Hoover Whoop", "Saw + deep fast PWM + sub with a slow resonant filter whoop; Chorus II.", 4,
         {{ju::kChorus, kChoII}, {ju::kPulse, 1.0f}, {ju::kPw, 0.9f}, {ju::kPwMode, float(ju::kPwLfo)}, {ju::kSub, 0.6f}, {ju::kVibrato, 0.06f}, {ju::kCutoff, 250.0f},
          {ju::kResonance, 0.6f}, {ju::kEnvAmt, 0.7f}, {ju::kKeyFollow, 0.5f}, {ju::kAttack, 0.25f}, {ju::kDecay, 0.8f},
          {ju::kSustain, 0.3f}, {ju::kRelease, 0.4f}, {ju::kLfoRate, 4.5f}, {ju::kLfoDelay, 0.0f}, {ju::kVolume, 0.8f}},
         {juCutoff(240), juEnv(0.15f), juReso(0.15f), juAttack(0.2f), juLfoRate(3.0f), juVibrato(0.2f)}, J},
        {F, "OT Poly Lead", "Saw + narrow pulse lead with delayed vibrato and key-tracked filter; Chorus I.", 4,
         {{ju::kChorus, kChoI}, {ju::kPulse, 1.0f}, {ju::kPw, 0.3f}, {ju::kPwMode, float(ju::kPwManual)}, {ju::kSub, 0.2f}, {ju::kVibrato, 0.12f},
          {ju::kCutoff, 1800.0f}, {ju::kResonance, 0.3f}, {ju::kEnvAmt, 0.35f}, {ju::kKeyFollow, 0.7f}, {ju::kVelVcf, 0.3f},
          {ju::kAttack, 0.01f}, {ju::kDecay, 0.4f}, {ju::kSustain, 0.7f}, {ju::kRelease, 0.25f}, {ju::kLfoRate, 5.5f},
          {ju::kLfoDelay, 0.6f}, {ju::kVolume, 0.85f}},
         {juCutoff(1500), juReso(), juEnv(0.15f), juVibrato(0.2f), juRelease(0.3f), juPwm(0.3f)}, J},
        {F, "OT Glass Ring", "Narrow pulse through a ringing, fully key-tracked filter: bell and glass; Chorus II.", 4,
         {{ju::kChorus, kChoII}, {ju::kSaw, 0.0f}, {ju::kPulse, 1.0f}, {ju::kPw, 0.85f}, {ju::kPwMode, float(ju::kPwManual)}, {ju::kSub, 0.0f}, {ju::kHpf, 3.0f},
          {ju::kCutoff, 1200.0f}, {ju::kResonance, 0.85f}, {ju::kEnvAmt, 0.45f}, {ju::kKeyFollow, 1.0f}, {ju::kAttack, 0.0f},
          {ju::kDecay, 1.6f}, {ju::kSustain, 0.0f}, {ju::kRelease, 1.4f}, {ju::kVolume, 0.8f}},
         {juCutoff(1000), juReso(0.1f), juEnv(0.15f), juDecay(1.0f), juRelease(1.0f), juPwm(0.1f)}, J},
        {F, "OT Noise Sweep", "Noise through a slow resonant filter sweep with LFO wobble: wind and risers; Chorus II.", 4,
         {{ju::kChorus, kChoII}, {ju::kSaw, 0.0f}, {ju::kSub, 0.0f}, {ju::kNoise, 1.0f}, {ju::kHpf, 2.0f}, {ju::kCutoff, 150.0f}, {ju::kResonance, 0.8f},
          {ju::kEnvAmt, 0.8f}, {ju::kVcfLfo, 0.15f}, {ju::kKeyFollow, 0.0f}, {ju::kAttack, 1.5f}, {ju::kDecay, 2.5f}, {ju::kSustain, 0.2f},
          {ju::kRelease, 2.0f}, {ju::kLfoRate, 0.3f}, {ju::kLfoDelay, 0.0f}},
         {juCutoff(150), juReso(0.1f), juEnv(0.15f), juAttack(1.0f), juLfoFilter(), juLfoRate(4.0f, false)}, J},
        {F, "OT Warm Keys", "Saw + sub e-piano-like keys: velocity to the VCF, decaying tone, bass boost; Chorus I.", 4,
         {{ju::kChorus, kChoI}, {ju::kSub, 0.4f}, {ju::kHpf, 0.0f}, {ju::kCutoff, 900.0f}, {ju::kResonance, 0.1f}, {ju::kEnvAmt, 0.3f}, {ju::kKeyFollow, 0.6f},
          {ju::kVelVcf, 0.6f}, {ju::kVolume, 0.85f}, {ju::kAttack, 0.005f}, {ju::kDecay, 1.8f}, {ju::kSustain, 0.25f}, {ju::kRelease, 0.5f}},
         {juCutoff(800), juEnv(0.15f), juDecay(1.0f), juRelease(0.4f), juSub(), juChorus()}, J},
    };
}
#endif  // OT_MOD_JU6S

#if defined(OT_MOD_DTY1)
// ---------------------------------------------------------------------------
// D-Type (module DTY1, engine 0x82, Patches/DTYPE/): three VCOs (6 waves,
// Lo..2', VCO 3 off the keyboard as an LFO), white / pink noise, mixer with
// feedback, the 4-pole ladder (Cutoff, Emphasis to self-oscillation,
// Contour, Key Track), two ADS contours with the Decay switch, glide, Mod
// Mix (VCO 3 .. noise) into the VCOs and the filter through the Wheel. Mono:
// every patch plays Poly 1. M1-M4 are the same on every patch (Cutoff,
// Emphasis, Contour, Decay of both contours); M5 glide or wheel, M6 the
// patch's own; leads and brass add Velocity -> Contour to M7.

namespace dt = ot::audio::dtype;
static_assert(kDtypeEngine == dt::kEngineId, "presets.h kDtypeEngine");

// Macro helpers (amount x stored range per unit: Cutoff 20..20000 Hz,
// times 0..10 s, Glide Time 0..3 s, Tune -24..24 st, Fine -100..100 ct, levels 0..1).
Macro dtCutoff(float hz) { return {"Cutoff", true, {{dt::kCutoff, cut(hz)}}}; }
Macro dtEmph(float a = 0.2f) { return {"Emphasis", true, {{dt::kEmphasis, a}}}; }
Macro dtContour(float a = 0.25f) { return {"Contour", true, {{dt::kContour, a}}}; }
Macro dtDecay(float s) { return {"Decay", true, {{dt::kFDecay, s / 10.0f}, {dt::kLDecay, s / 10.0f}}}; }
Macro dtGlide(float s) { return {"Glide", false, {{dt::kGlideTime, s / 3.0f}}}; }
Macro dtWheel(float a, bool bipolar = false) { return {"Wheel", bipolar, {{dt::kWheel, a}}}; }
Macro dtVibrato(float a) { return {"Vibrato", false, {{dt::kWheel, a}}}; }
Macro dtNoise(float a) { return {"Noise", false, {{dt::kNoiseLevel, a}}}; }
Macro dtDetune(float ct) { return {"Detune", true, {{dt::kVco2Fine, ct / 200.0f}}}; }
// Velocity (M7) -> Contour: the contour scales with the velocity (amount a).
const std::vector<Target> kDtVelContour = {{dt::kContour, 0.5f}};
constexpr float k32 = float(dt::k32), k16 = float(dt::k16), k8 = float(dt::k8), k4 = float(dt::k4);

std::vector<Preset> dtypePresets() {
    const SynthModel F = SynthModel::ACD;   // the Mini fallback model of the slot record (mono)
    const int32_t D = kDtypeEngine;
    return {
        {F, "OT Model Bass", "Two saws at 16', half-open ladder with a short contour, legato glide 50 ms; M6 adds a 32' square.", 1,
         {{dt::kVco1Range, k16}, {dt::kVco2Range, k16}, {dt::kVco2Fine, 5.0f}, {dt::kVco3Range, k32}, {dt::kVco3Wave, float(dt::kSquare)},
          {dt::kVco3Kb, 1.0f}, {dt::kCutoff, 500.0f}, {dt::kEmphasis, 0.35f}, {dt::kContour, 0.45f}, {dt::kFDecay, 0.35f},
          {dt::kFSustain, 0.2f}, {dt::kLDecay, 0.4f}, {dt::kLSustain, 0.85f}, {dt::kOscMod, 0.0f}, {dt::kGlideTime, 0.05f}, {dt::kVolume, 1.3f}},
         {dtCutoff(400), dtEmph(), dtContour(), dtDecay(0.3f), dtGlide(0.3f), {"Sub", false, {{dt::kVco3Level, 0.6f}}}}, D},
        {F, "OT Fat Lead", "Three saws at 8', detuned, cutoff 2.5 kHz, legato (tied notes glide 100 ms, no re-attack); velocity to the contour.", 1,
         {{dt::kVco2Fine, 8.0f}, {dt::kVco3Range, k8}, {dt::kVco3Wave, float(dt::kSaw)}, {dt::kVco3Kb, 1.0f}, {dt::kVco3Fine, -9.0f},
          {dt::kVco3Level, 0.5f}, {dt::kVco1Level, 0.7f}, {dt::kVco2Level, 0.6f}, {dt::kCutoff, 2500.0f}, {dt::kEmphasis, 0.25f},
          {dt::kContour, 0.3f}, {dt::kFDecay, 0.8f}, {dt::kFSustain, 0.5f}, {dt::kLDecay, 1.0f}, {dt::kLSustain, 0.9f},
          {dt::kOscMod, 0.0f}, {dt::kGlideMode, float(dt::kLegato)}, {dt::kGlideTime, 0.1f}, {dt::kVolume, 1.2f}},
         {dtCutoff(2000), dtEmph(), dtContour(), dtDecay(0.6f), dtGlide(0.4f), dtDetune(30.0f)}, D, kDtVelContour},
        {F, "OT Brass Section", "Saw at 8' + square at 16', a slow filter attack (80 ms), vibrato from VCO 3 on the wheel (M5).", 1,
         {{dt::kVco2Range, k16}, {dt::kVco2Wave, float(dt::kSquare)}, {dt::kVco2Level, 0.5f}, {dt::kVco2Fine, 0.0f},
          {dt::kVco3Tune, 16.0f}, {dt::kCutoff, 700.0f}, {dt::kEmphasis, 0.15f}, {dt::kContour, 0.5f}, {dt::kFAttack, 0.08f},
          {dt::kFDecay, 0.6f}, {dt::kFSustain, 0.5f}, {dt::kLAttack, 0.03f}, {dt::kLDecay, 0.8f}, {dt::kLSustain, 0.85f},
          {dt::kOscMod, 0.2f}, {dt::kVolume, 1.35f}},
         {dtCutoff(600), dtEmph(), dtContour(), dtDecay(0.5f), dtVibrato(0.8f), {"Attack", true, {{dt::kFAttack, 0.01f}, {dt::kLAttack, 0.005f}}}},
         D, kDtVelContour},
        {F, "OT Sync Bite", "Saw lead with VCO 3 on the keyboard as audio-rate FM and high emphasis: a hard, bright edge; M5 more FM.", 1,
         {{dt::kVco2Level, 0.3f}, {dt::kVco2Fine, 4.0f}, {dt::kVco3Range, k8}, {dt::kVco3Wave, float(dt::kTri)}, {dt::kVco3Kb, 1.0f},
          {dt::kOscMod, 0.9f}, {dt::kWheel, 0.8f}, {dt::kCutoff, 900.0f}, {dt::kEmphasis, 0.65f}, {dt::kContour, 0.5f},
          {dt::kFDecay, 0.25f}, {dt::kFSustain, 0.15f}, {dt::kLDecay, 0.5f}, {dt::kLSustain, 0.8f}, {dt::kVolume, 1.1f}},
         {dtCutoff(700), dtEmph(0.15f), dtContour(), dtDecay(0.3f), dtWheel(0.2f, true), {"FM Tune", true, {{dt::kVco3Tune, 0.25f}}}},
         D, kDtVelContour},
        {F, "OT Pluck Perc", "Square + saw pluck: Decay switch Off (short release), no sustain, a fast contour.", 1,
         {{dt::kVco1Wave, float(dt::kSquare)}, {dt::kVco2Range, k4}, {dt::kVco2Level, 0.3f}, {dt::kCutoff, 400.0f},
          {dt::kEmphasis, 0.4f}, {dt::kContour, 0.6f}, {dt::kFAttack, 0.0f}, {dt::kFDecay, 0.12f}, {dt::kFSustain, 0.0f},
          {dt::kLAttack, 0.0f}, {dt::kLDecay, 0.25f}, {dt::kLSustain, 0.0f}, {dt::kDecaySw, 0.0f}, {dt::kOscMod, 0.0f}, {dt::kVolume, 1.4f}},
         {dtCutoff(400), dtEmph(), dtContour(), dtDecay(0.2f), dtGlide(0.2f), dtNoise(0.4f)}, D},
        {F, "OT Tom Thump", "Triangle at 32' + noise into a ladder near self-oscillation, Key Track Full: a pitched drum.", 1,
         {{dt::kVco1Range, k32}, {dt::kVco1Wave, float(dt::kTri)}, {dt::kVco2Level, 0.0f}, {dt::kNoiseLevel, 0.15f},
          {dt::kCutoff, 150.0f}, {dt::kEmphasis, 0.72f}, {dt::kContour, 0.6f}, {dt::kKeyTrack, float(dt::kKeyFull)},
          {dt::kFAttack, 0.0f}, {dt::kFDecay, 0.18f}, {dt::kFSustain, 0.0f}, {dt::kLAttack, 0.0f}, {dt::kLDecay, 0.35f},
          {dt::kLSustain, 0.0f}, {dt::kOscMod, 0.0f}, {dt::kVolume, 1.8f}},
         {dtCutoff(120), dtEmph(0.15f), dtContour(), dtDecay(0.3f), dtGlide(0.2f), dtNoise(0.4f)}, D},
        {F, "OT Whistle Sine", "No VCO: Emphasis 100 % makes the ladder the oscillator, in tune from C4 (Key Track Full).", 1,
         {{dt::kVco1Level, 0.0f}, {dt::kVco2Level, 0.0f}, {dt::kCutoff, 261.63f}, {dt::kEmphasis, 1.0f}, {dt::kContour, 0.0f},
          {dt::kKeyTrack, float(dt::kKeyFull)}, {dt::kVco3Tune, 16.0f}, {dt::kOscMod, 0.0f}, {dt::kFilterMod, 0.1f},
          {dt::kLAttack, 0.02f}, {dt::kLDecay, 0.3f}, {dt::kLSustain, 1.0f}, {dt::kGlideMode, float(dt::kLegato)}, {dt::kGlideTime, 0.08f}, {dt::kVolume, 1.2f}},
         {dtCutoff(60), dtEmph(0.15f), dtContour(0.1f), dtDecay(0.5f), dtVibrato(0.7f), {"Breath", false, {{dt::kNoiseLevel, 0.3f}}}},
         D},
        {F, "OT Hollow Square", "Squares at 8' and 16', Key Track 2/3, a gentle contour: a hollow, woody voice.", 1,
         {{dt::kVco1Wave, float(dt::kSquare)}, {dt::kVco1Level, 0.6f}, {dt::kVco2Range, k16}, {dt::kVco2Wave, float(dt::kSquare)},
          {dt::kVco2Level, 0.5f}, {dt::kVco2Fine, 4.0f}, {dt::kCutoff, 1500.0f}, {dt::kEmphasis, 0.2f}, {dt::kContour, 0.25f},
          {dt::kFDecay, 0.7f}, {dt::kFSustain, 0.6f}, {dt::kLDecay, 0.8f}, {dt::kLSustain, 0.9f}, {dt::kOscMod, 0.0f},
          {dt::kGlideTime, 0.04f}, {dt::kVolume, 1.4f}},
         {dtCutoff(1200), dtEmph(), dtContour(), dtDecay(0.5f), dtGlide(0.3f), dtDetune(20.0f)}, D},
        {F, "OT Growl Bass", "Saws at 32' and 16' with the output fed back into the mixer (Feedback 80 %), cutoff 300 Hz.", 1,
         {{dt::kVco1Range, k32}, {dt::kVco2Range, k16}, {dt::kVco2Fine, 3.0f}, {dt::kFeedback, 0.8f}, {dt::kCutoff, 300.0f},
          {dt::kEmphasis, 0.25f}, {dt::kContour, 0.35f}, {dt::kFDecay, 0.3f}, {dt::kFSustain, 0.3f}, {dt::kLDecay, 0.4f},
          {dt::kLSustain, 0.85f}, {dt::kOscMod, 0.0f}, {dt::kVolume, 1.0f}},
         {dtCutoff(250), dtEmph(), dtContour(), dtDecay(0.3f), dtGlide(0.3f), {"Feedback", true, {{dt::kFeedback, 0.2f}}}}, D},
        {F, "OT Noise Hit", "Noise alone through a resonant ladder with a full, fast contour: snares, claps, zaps.", 1,
         {{dt::kVco1Level, 0.0f}, {dt::kVco2Level, 0.0f}, {dt::kNoiseLevel, 0.8f}, {dt::kCutoff, 300.0f}, {dt::kEmphasis, 0.7f},
          {dt::kContour, 1.0f}, {dt::kKeyTrack, float(dt::kKeyFull)}, {dt::kFAttack, 0.0f}, {dt::kFDecay, 0.15f},
          {dt::kFSustain, 0.0f}, {dt::kLAttack, 0.0f}, {dt::kLDecay, 0.3f}, {dt::kLSustain, 0.0f}, {dt::kOscMod, 0.0f}, {dt::kVolume, 1.3f}},
         {dtCutoff(250), dtEmph(0.15f), dtContour(0.2f), dtDecay(0.2f), dtGlide(0.2f), {"Pink", false, {{dt::kNoiseColour, 1.0f}}}}, D},
        {F, "OT Wind Drone", "Pink noise + triangle at 32', slow contours, a slow VCO 3 moving the filter on the wheel.", 1,
         {{dt::kVco1Range, k32}, {dt::kVco1Wave, float(dt::kTri)}, {dt::kVco1Level, 0.4f}, {dt::kVco2Level, 0.0f},
          {dt::kNoiseLevel, 0.7f}, {dt::kNoiseColour, float(dt::kPink)}, {dt::kVco3Tune, -24.0f}, {dt::kCutoff, 600.0f},
          {dt::kEmphasis, 0.6f}, {dt::kContour, 0.3f}, {dt::kFAttack, 1.5f}, {dt::kFDecay, 3.0f}, {dt::kFSustain, 0.6f},
          {dt::kLAttack, 1.2f}, {dt::kLDecay, 3.0f}, {dt::kLSustain, 1.0f}, {dt::kOscMod, 0.0f}, {dt::kFilterMod, 0.35f},
          {dt::kWheel, 0.6f}, {dt::kVolume, 1.3f}},
         {dtCutoff(500), dtEmph(), dtContour(), dtDecay(2.0f), dtWheel(0.4f, true), {"Rate", true, {{dt::kVco3Tune, 0.25f}}}}, D},
        {F, "OT Lab Sweep", "Saw + wide pulse, VCO 3 at 16' as audio-rate FM, Emphasis 90 %, a 2 s filter contour sweep.", 1,
         {{dt::kVco2Wave, float(dt::kWide)}, {dt::kVco2Level, 0.4f}, {dt::kVco3Range, k16}, {dt::kVco3Wave, float(dt::kTri)},
          {dt::kVco3Kb, 1.0f}, {dt::kVco3Level, 0.2f}, {dt::kOscMod, 0.8f}, {dt::kWheel, 0.8f}, {dt::kCutoff, 200.0f},
          {dt::kEmphasis, 0.9f}, {dt::kContour, 0.8f}, {dt::kFAttack, 2.0f}, {dt::kFDecay, 2.0f}, {dt::kFSustain, 0.2f},
          {dt::kLAttack, 0.01f}, {dt::kLDecay, 2.0f}, {dt::kLSustain, 0.9f}, {dt::kVolume, 0.95f}},
         {dtCutoff(200), dtEmph(0.1f), dtContour(0.2f), dtDecay(1.5f), dtWheel(0.2f, true), {"Mod Mix", false, {{dt::kModMix, 1.0f}}}},
         D},
    };
}
#endif  // OT_MOD_DTY1

#if defined(OT_MOD_CST1)
// ---------------------------------------------------------------------------
// CS-Type (module CST1, engine 0x83, Patches/CSTYPE/): two layers per note
// (I at 0.., II at 22..), each a saw / pulse / sine / noise VCO into a
// resonant high-pass and low-pass (IL / AL contour) and its own ADSR VCA;
// the ring modulator, the sub-osc LFO, Touch and the Ribbon. Poly 4 (chords)
// except the Poly 1 lead. M1 Brilliance, M2 Resonance, M3 Touch, M4 Release,
// M5 Ribbon on every patch; M6 the patch's own.

namespace cs = ot::audio::cstype;
static_assert(kCstypeEngine == cs::kEngineId, "presets.h kCstypeEngine");
constexpr int kII = cs::kLayer2;

// Macro helpers (Brilliance / Resonance / Ribbon -1..1, times 0..10 s, levels 0..1).
Macro csBril(float a = 0.25f) { return {"Brilliance", true, {{cs::kBrilliance, a}}}; }
Macro csRes(float a = 0.15f) { return {"Resonance", true, {{cs::kResonance, a}}}; }
Macro csTouch() { return {"Touch", false, {{cs::kTouch, 1.0f}}}; }
Macro csRelease(float s) { return {"Release", true, {{cs::kRelease, s / 10.0f}, {kII + cs::kRelease, s / 10.0f}}}; }
Macro csRibbon(float a = 0.5f) { return {"Ribbon", true, {{cs::kRibbon, a}}}; }
// One layer's edit (layer 0 = I, 1 = II).
Edit L(int layer, int param, float v) { return {(layer ? kII : 0) + param, v}; }

std::vector<Preset> cstypePresets() {
    const SynthModel F = SynthModel::VAP;   // the Mini fallback model of the slot record (poly)
    const int32_t C = kCstypeEngine;
    const float k16 = float(cs::k16), k4 = float(cs::k4);
    return {
        {F, "OT Two Layer Brass", "Layer I a saw, layer II saw + pulse at 16', both low-passes opened by their contours; velocity to the attack levels, Touch opens them.", 4,
         {L(0, cs::kLpfCutoff, 1300), L(0, cs::kLpfRes, 0.25f), L(0, cs::kAttackLevel, 0.6f), L(0, cs::kFAttack, 0.08f), L(0, cs::kFDecay, 0.7f),
          L(1, cs::kFeet, k16), L(1, cs::kPulse, 1.0f), L(1, cs::kPw, 0.2f), L(1, cs::kLpfCutoff, 900), L(1, cs::kAttackLevel, 0.5f),
          L(1, cs::kFAttack, 0.12f), L(1, cs::kAttack, 0.05f), L(1, cs::kLevel, 0.5f), {cs::kInitBril, 0.5f}, {cs::kAfterVcf, 0.4f}, {cs::kVolume, 0.6f}},
         {csBril(), csRes(), csTouch(), csRelease(0.6f), csRibbon(), {"Layer II", true, {{kII + cs::kLevel, 0.4f}}}}, C},
        {F, "OT Glass Strings", "Two pulse layers (8' and 4') swept by their own slow PWM, a high-pass taking the body out, slow attack and long release.", 4,
         {L(0, cs::kSaw, 0.0f), L(0, cs::kPulse, 1.0f), L(0, cs::kPw, 0.3f), L(0, cs::kPwm, 0.5f), L(0, cs::kPwmSpeed, 0.6f),
          L(0, cs::kHpfCutoff, 250), L(0, cs::kLpfCutoff, 3500), L(0, cs::kAttackLevel, 0.1f), L(0, cs::kAttack, 0.4f), L(0, cs::kRelease, 1.2f),
          L(1, cs::kFeet, k4), L(1, cs::kSaw, 0.0f), L(1, cs::kPulse, 1.0f), L(1, cs::kPw, 0.4f), L(1, cs::kPwm, 0.6f), L(1, cs::kPwmSpeed, 0.9f),
          L(1, cs::kHpfCutoff, 400), L(1, cs::kLpfCutoff, 4000), L(1, cs::kAttackLevel, 0.1f), L(1, cs::kAttack, 0.6f), L(1, cs::kRelease, 1.4f),
          L(1, cs::kLevel, 0.45f), {cs::kVolume, 0.7f}},
         {csBril(), csRes(), csTouch(), csRelease(1.0f), csRibbon(), {"PWM", true, {{cs::kPwm, 0.3f}, {kII + cs::kPwm, 0.3f}}}}, C},
        {F, "OT Ring Mod Bells", "Two sines an octave apart through the ring modulator, its rate swept down at the start, a decaying bell envelope.", 4,
         {L(0, cs::kSaw, 0.0f), L(0, cs::kSine, 1.0f), L(0, cs::kLpfCutoff, 8000), L(0, cs::kAttackLevel, 0.0f), L(0, cs::kDecay, 1.6f),
          L(0, cs::kSustain, 0.0f), L(0, cs::kRelease, 1.4f), L(1, cs::kFeet, k4), L(1, cs::kSaw, 0.0f), L(1, cs::kSine, 1.0f),
          L(1, cs::kLpfCutoff, 8000), L(1, cs::kAttackLevel, 0.0f), L(1, cs::kAttack, 0.0f), L(1, cs::kDecay, 0.9f), L(1, cs::kSustain, 0.0f),
          L(1, cs::kRelease, 0.9f), L(1, cs::kLevel, 0.5f), {cs::kRmDepth, 0.75f}, {cs::kRmSpeed, 270}, {cs::kRmSweep, 0.4f},
          {cs::kRmDecay, 0.6f}, {cs::kInitBril, 0.0f}, {cs::kVolume, 0.95f}},
         {csBril(), csRes(), csTouch(), csRelease(1.0f), csRibbon(), {"RM Depth", true, {{cs::kRmDepth, 0.25f}}}}, C},
        {F, "OT Pressure Lead", "Poly 1 lead: saw + pulse and a detuned saw; Touch (M3, FX CC c) brings in vibrato, opens the filter and pushes the level.", 1,
         {L(0, cs::kPulse, 1.0f), L(0, cs::kPw, 0.25f), L(0, cs::kLpfCutoff, 1200), L(0, cs::kLpfRes, 0.45f), L(0, cs::kAttackLevel, 0.4f),
          L(0, cs::kFDecay, 0.8f), L(0, cs::kSustain, 0.9f), L(0, cs::kRelease, 0.3f), L(1, cs::kLpfCutoff, 1500), L(1, cs::kLpfRes, 0.3f),
          L(1, cs::kAttack, 0.02f), L(1, cs::kRelease, 0.3f), L(1, cs::kLevel, 0.5f), {cs::kDetune, 12.0f}, {cs::kAfterVco, 0.6f},
          {cs::kAfterVcf, 0.5f}, {cs::kAfterVca, 0.3f}, {cs::kGlideTime, 0.05f}, {cs::kVolume, 1.0f}},
         {csBril(), csRes(), csTouch(), csRelease(0.3f), csRibbon(), {"Glide", false, {{cs::kGlideTime, 0.1f}}}}, C},
        {F, "OT Noise Wash", "Noise in both layers through resonant filters, layer II high-passed, the cutoffs moved by a slow sub-osc sine: surf and wind.", 4,
         {L(0, cs::kSaw, 0.0f), L(0, cs::kNoise, 1.0f), L(0, cs::kLpfCutoff, 900), L(0, cs::kLpfRes, 0.6f), L(0, cs::kAttackLevel, 0.2f),
          L(0, cs::kAttack, 1.5f), L(0, cs::kRelease, 2.0f), L(1, cs::kSaw, 0.0f), L(1, cs::kNoise, 1.0f), L(1, cs::kHpfCutoff, 1500),
          L(1, cs::kHpfRes, 0.4f), L(1, cs::kLpfCutoff, 6000), L(1, cs::kAttack, 2.0f), L(1, cs::kRelease, 2.5f), L(1, cs::kLevel, 0.4f),
          {cs::kSpeed, 0.5f}, {cs::kToVcf, 0.5f}, {cs::kInitBril, 0.0f}, {cs::kVolume, 1.2f}},
         {csBril(), csRes(0.1f), csTouch(), csRelease(1.5f), csRibbon(), {"Wave Speed", true, {{cs::kSpeed, 0.02f}}}}, C},
        {F, "OT Soft Keys", "A dark saw + sine layer with a piano-like decay and a 4' sine layer for the attack; velocity to brightness and level.", 4,
         {L(0, cs::kSine, 0.5f), L(0, cs::kLpfCutoff, 1600), L(0, cs::kLpfRes, 0.1f), L(0, cs::kAttackLevel, 0.4f), L(0, cs::kFDecay, 0.9f),
          L(0, cs::kDecay, 1.4f), L(0, cs::kSustain, 0.3f), L(0, cs::kRelease, 0.5f), L(1, cs::kFeet, k4), L(1, cs::kSaw, 0.0f),
          L(1, cs::kSine, 1.0f), L(1, cs::kAttack, 0.0f), L(1, cs::kDecay, 0.5f), L(1, cs::kSustain, 0.0f), L(1, cs::kRelease, 0.3f),
          L(1, cs::kLevel, 0.35f), {cs::kInitBril, 0.6f}, {cs::kInitLevelVel, 0.4f}, {cs::kVolume, 0.75f}},
         {csBril(), csRes(), csTouch(), csRelease(0.5f), csRibbon(), {"Tine", true, {{kII + cs::kLevel, 0.3f}}}}, C},
        {F, "OT Film Pad", "A saw at 8' and a PWM pulse at 16', slow attacks, a long release, a gentle sub-osc drift on the pitch.", 4,
         {L(0, cs::kLpfCutoff, 1600), L(0, cs::kAttackLevel, 0.3f), L(0, cs::kFAttack, 1.0f), L(0, cs::kAttack, 0.8f), L(0, cs::kRelease, 2.0f),
          L(1, cs::kFeet, k16), L(1, cs::kSaw, 0.0f), L(1, cs::kPulse, 1.0f), L(1, cs::kPw, 0.3f), L(1, cs::kPwm, 0.4f), L(1, cs::kLpfCutoff, 1000),
          L(1, cs::kAttack, 1.2f), L(1, cs::kRelease, 2.4f), L(1, cs::kLevel, 0.5f), {cs::kSpeed, 0.2f}, {cs::kToVco, 0.15f},
          {cs::kVolume, 0.7f}},
         {csBril(), csRes(), csTouch(), csRelease(1.5f), csRibbon(), {"Drift", false, {{cs::kToVco, 0.3f}}}}, C},
        {F, "OT Reso Sweep", "One saw layer into a low-pass at 85 % resonance swept up by a slow contour from below (IL negative).", 4,
         {L(0, cs::kLpfCutoff, 350), L(0, cs::kLpfRes, 0.85f), L(0, cs::kInitLevel, -0.2f), L(0, cs::kAttackLevel, 0.7f),
          L(0, cs::kFAttack, 1.2f), L(0, cs::kFDecay, 1.5f), L(0, cs::kRelease, 0.6f), L(1, cs::kLevel, 0.0f), {cs::kInitBril, 0.0f},
          {cs::kVolume, 0.65f}},
         {csBril(), csRes(0.08f), csTouch(), csRelease(0.6f), csRibbon(), {"Sweep", true, {{cs::kFAttack, 0.08f}, {cs::kFDecay, 0.08f}}}}, C},
        {F, "OT Hard Touch", "A pulse and a 16' saw: the velocity sets the brightness and the level strongly; play it with Volume FX.", 4,
         {L(0, cs::kSaw, 0.0f), L(0, cs::kPulse, 1.0f), L(0, cs::kPw, 0.15f), L(0, cs::kLpfCutoff, 700), L(0, cs::kLpfRes, 0.3f),
          L(0, cs::kAttackLevel, 0.7f), L(0, cs::kFDecay, 0.4f), L(1, cs::kFeet, k16), L(1, cs::kLpfCutoff, 900), L(1, cs::kAttackLevel, 0.5f),
          L(1, cs::kLevel, 0.4f), {cs::kInitBril, 1.0f}, {cs::kInitLevelVel, 0.5f}, {cs::kVolume, 0.75f}},
         {csBril(), csRes(), csTouch(), csRelease(0.4f), csRibbon(), {"Width", false, {{cs::kPw, 0.6f}}}}, C},
        {F, "OT Whistle Duet", "No waves: both low-passes sing (resonance at the top), layer II a fifth above, following the keys by half.", 4,
         {L(0, cs::kSaw, 0.0f), L(0, cs::kLpfCutoff, 880), L(0, cs::kLpfRes, 1.0f), L(0, cs::kAttackLevel, 0.0f), L(0, cs::kAttack, 0.08f),
          L(0, cs::kRelease, 0.5f), L(0, cs::kLevel, 0.4f), L(1, cs::kSaw, 0.0f), L(1, cs::kLpfCutoff, 1318), L(1, cs::kLpfRes, 1.0f),
          L(1, cs::kAttackLevel, 0.0f), L(1, cs::kAttack, 0.12f), L(1, cs::kRelease, 0.5f), L(1, cs::kLevel, 0.3f), {cs::kInitBril, 0.0f},
          {cs::kVolume, 0.7f}},
         {csBril(0.1f), csRes(0.05f), csTouch(), csRelease(0.5f), csRibbon(), {"Interval", true, {{kII + cs::kLpfCutoff, 0.02f}}}}, C},
        {F, "OT Dual Filter Growl", "A 16' saw between a resonant high-pass and low-pass, a 16' pulse layer, the low-passes wobbled by the sub-osc.", 4,
         {L(0, cs::kFeet, k16), L(0, cs::kHpfCutoff, 180), L(0, cs::kHpfRes, 0.7f), L(0, cs::kLpfCutoff, 550), L(0, cs::kLpfRes, 0.7f),
          L(0, cs::kAttackLevel, 0.3f), L(1, cs::kFeet, k16), L(1, cs::kSaw, 0.0f), L(1, cs::kPulse, 1.0f), L(1, cs::kLpfCutoff, 700),
          L(1, cs::kLevel, 0.4f), {cs::kSpeed, 5.0f}, {cs::kToVcf, 0.25f}, {cs::kVolume, 0.55f}},
         {csBril(), csRes(0.1f), csTouch(), csRelease(0.3f), csRibbon(), {"Growl", false, {{cs::kToVcf, 0.5f}}}}, C},
        {F, "OT Choir Vox", "A breathy pulse and a saw each between resonant band-limiting filters: vowel-like formants; a slow vibrato from the sub-osc.", 4,
         {L(0, cs::kSaw, 0.0f), L(0, cs::kPulse, 1.0f), L(0, cs::kPw, 0.6f), L(0, cs::kPwm, 0.3f), L(0, cs::kNoise, 0.05f),
          L(0, cs::kHpfCutoff, 350), L(0, cs::kHpfRes, 0.5f), L(0, cs::kLpfCutoff, 1800), L(0, cs::kLpfRes, 0.5f), L(0, cs::kAttack, 0.3f),
          L(0, cs::kRelease, 1.0f), L(1, cs::kHpfCutoff, 700), L(1, cs::kHpfRes, 0.6f), L(1, cs::kLpfCutoff, 2500), L(1, cs::kLpfRes, 0.4f),
          L(1, cs::kAttack, 0.4f), L(1, cs::kRelease, 1.2f), L(1, cs::kLevel, 0.5f), {cs::kSpeed, 5.0f}, {cs::kToVco, 0.05f},
          {cs::kVolume, 0.65f}},
         {csBril(), csRes(), csTouch(), csRelease(0.8f), csRibbon(), {"Vowel", true, {{cs::kLpfCutoff, 0.03f}, {kII + cs::kLpfCutoff, 0.03f}}}}, C},
    };
}
#endif  // OT_MOD_CST1

#if defined(OT_MOD_ATY1)
// ---------------------------------------------------------------------------
// A-Type (module ATY1, engine 0x84, Patches/ATYPE/): two VCOs (saw / pulse
// with PWM, 32'..4', VCO 2 tune and hard sync), noise or the ring modulator,
// the sample & hold, HPF, 4-pole or 2-pole low-pass, ADSR and AR, one LFO,
// Keyboard Mono / Duo, Priority Last / Low / High. Every patch plays Poly 1
// (Duo needs it: VCO 1 takes the lowest held note, VCO 2 the highest; two
// tracks on the slot or two MIDI keys). M1-M4 are the same on every patch
// (Cutoff, Resonance, Env Amt, Decay + Release); M5 the vibrato or the S&H
// rate, M6 the patch's own; leads add Velocity -> Env Amt to M7.

namespace at = ot::audio::atype;
static_assert(kAtypeEngine == at::kEngineId, "presets.h kAtypeEngine");

// Macro helpers (amount x stored range per unit: Cutoff 20..20000 Hz,
// Env Amt -1..1, times 0..10 s, S&H Rate 0.1..50 Hz, Tune -24..24 st, levels 0..1).
Macro atCutoff(float hz) { return {"Cutoff", true, {{at::kCutoff, cut(hz)}}}; }
Macro atRes(float a = 0.2f) { return {"Resonance", true, {{at::kResonance, a}}}; }
Macro atEnv(float a = 0.15f) { return {"Env Amt", true, {{at::kEnvAmt, a}}}; }
Macro atDecay(float s) { return {"Decay", true, {{at::kDecay, s / 10.0f}, {at::kRelease, s / 10.0f}, {at::kArRelease, s / 10.0f}}}; }
Macro atVibrato(float a) { return {"Vibrato", false, {{at::kVco1LfoMod, a}, {at::kVco2Mod, a}}}; }
Macro atShRate(float hz) { return {"S&H Rate", true, {{at::kShRate, hz / 49.9f}}}; }
Macro atTune(const char* name, float st) { return {name, true, {{at::kVco2Tune, st / 48.0f}}}; }
// Velocity (M7) -> Env Amt.
const std::vector<Target> kAtVelEnv = {{at::kEnvAmt, 0.5f}};
constexpr float kA32 = float(at::k32), kA16 = float(at::k16);
constexpr float kPulseW = float(at::kPulse), kDuoK = float(at::kDuo), kArE = float(at::kAr), k2P = float(at::k2Pole);

std::vector<Preset> atypePresets() {
    const SynthModel F = SynthModel::ACD;   // the Mini fallback model of the slot record (mono)
    const int32_t A = kAtypeEngine;
    return {
        {F, "OT Duo Fifths", "Duo: two notes held (two tracks or two keys) split, VCO 1 the low one, VCO 2 the high one; two saws, a bright 4-pole.", 1,
         {{at::kKeyboard, kDuoK}, {at::kVco2Fine, 3.0f}, {at::kVco1Level, 0.7f}, {at::kVco2Level, 0.7f}, {at::kCutoff, 1800.0f},
          {at::kResonance, 0.25f}, {at::kEnvAmt, 0.3f}, {at::kDecay, 0.5f}, {at::kSustain, 0.7f}, {at::kRelease, 0.25f}, {at::kVolume, 1.1f}},
         {atCutoff(1500), atRes(), atEnv(), atDecay(0.4f), atVibrato(0.3f), {"Detune", true, {{at::kVco2Fine, 0.15f}}}}, A},
        {F, "OT Sync Tear", "VCO 2 synced to VCO 1, swept by the ADSR: the classic tearing lead; M6 moves the sync tune.", 1,
         {{at::kSync, 1.0f}, {at::kVco2Tune, 7.0f}, {at::kVco2Fine, 0.0f}, {at::kVco1Level, 0.3f}, {at::kVco2Level, 0.8f},
          {at::kVco2Mod, 0.55f}, {at::kVco2ModSrc, float(at::kModAdsr)}, {at::kCutoff, 3000.0f}, {at::kResonance, 0.3f},
          {at::kEnvAmt, 0.25f}, {at::kDecay, 0.6f}, {at::kSustain, 0.6f}, {at::kRelease, 0.3f}, {at::kGlideTime, 0.03f}, {at::kVolume, 0.9f}},
         {atCutoff(2500), atRes(), atEnv(), atDecay(0.5f), {"Vibrato", false, {{at::kVco1LfoMod, 0.3f}}}, atTune("Sync Tune", 12.0f)},
         A, kAtVelEnv},
        {F, "OT Sample Hold Zap", "A pulse through a resonant 4-pole stepped by the S&H on noise at 10 Hz; M5 the rate, M6 the lag.", 1,
         {{at::kVco1Wave, kPulseW}, {at::kVco1Pw, 0.3f}, {at::kVco2Level, 0.4f}, {at::kCutoff, 900.0f}, {at::kResonance, 0.7f},
          {at::kEnvAmt, 0.2f}, {at::kShAmt, 0.55f}, {at::kShRate, 10.0f}, {at::kSustain, 0.8f}, {at::kRelease, 0.4f}, {at::kVolume, 1.0f}},
         {atCutoff(700), atRes(0.15f), atEnv(), atDecay(0.4f), atShRate(8.0f), {"Lag", false, {{at::kShLag, 0.15f}}}}, A},
        {F, "OT Odd Bass", "A 32' saw and a 16' pulse a fifth up through a dark 4-pole with a short envelope; M6 the pulse width.", 1,
         {{at::kVco1Range, kA32}, {at::kVco2Range, kA16}, {at::kVco2Wave, kPulseW}, {at::kVco2Pw, 0.6f}, {at::kVco2Tune, -5.0f},
          {at::kVco2Fine, 0.0f}, {at::kVco1Level, 0.8f}, {at::kVco2Level, 0.5f}, {at::kCutoff, 350.0f}, {at::kResonance, 0.45f},
          {at::kEnvAmt, 0.45f}, {at::kDecay, 0.25f}, {at::kSustain, 0.25f}, {at::kRelease, 0.15f}, {at::kKeyTrack, 0.5f}, {at::kVolume, 1.4f}},
         {atCutoff(300), atRes(), atEnv(), atDecay(0.2f), atVibrato(0.2f), {"PW", true, {{at::kVco2Pw, 0.35f}}}}, A},
        {F, "OT Ring Bell", "The ring modulator of two VCOs a tritone apart, an AR with a long release: a metallic bell; M6 the ring's tune.", 1,
         {{at::kVco2Tune, 6.0f}, {at::kVco2Fine, 13.0f}, {at::kVco1Level, 0.1f}, {at::kVco2Level, 0.0f}, {at::kSource, float(at::kRing)},
          {at::kSourceLevel, 0.9f}, {at::kCutoff, 6000.0f}, {at::kResonance, 0.1f}, {at::kEnvAmt, 0.0f}, {at::kVcaEnv, kArE},
          {at::kArAttack, 0.0f}, {at::kArRelease, 1.6f}, {at::kKeyTrack, 1.0f}, {at::kVolume, 1.3f}},
         {atCutoff(4000), atRes(), atEnv(), atDecay(1.0f), atVibrato(0.2f), atTune("Ring Tune", 7.0f)}, A},
        {F, "OT PWM Strings", "Duo: two pulse waves swept by a slow LFO into the 2-pole, slow attack and release; play two tracks for two voices.", 1,
         {{at::kKeyboard, kDuoK}, {at::kVco1Wave, kPulseW}, {at::kVco2Wave, kPulseW}, {at::kVco1Pw, 0.2f}, {at::kVco2Pw, 0.3f},
          {at::kVco1Pwm, 0.6f}, {at::kVco2Pwm, 0.5f}, {at::kLfoRate, 0.7f}, {at::kLfoShape, float(at::kTri)}, {at::kFilterMode, k2P},
          {at::kCutoff, 2400.0f}, {at::kResonance, 0.15f}, {at::kEnvAmt, 0.1f}, {at::kAttack, 0.35f}, {at::kDecay, 1.0f},
          {at::kSustain, 0.9f}, {at::kRelease, 0.9f}, {at::kVolume, 1.2f}},
         {atCutoff(2000), atRes(), atEnv(), atDecay(0.8f), atVibrato(0.25f), {"PWM", true, {{at::kVco1Pwm, 0.3f}, {at::kVco2Pwm, 0.3f}}}}, A},
        {F, "OT Random Arp", "The S&H on noise steps VCO 2 at 8 Hz over a held note: random melodies; M6 the step range.", 1,
         {{at::kVco1Level, 0.25f}, {at::kVco2Level, 0.7f}, {at::kVco2Wave, kPulseW}, {at::kVco2Pw, 0.4f}, {at::kVco2Mod, 0.6f},
          {at::kVco2ModSrc, float(at::kModSh)}, {at::kShRate, 8.0f}, {at::kCutoff, 1600.0f}, {at::kResonance, 0.35f},
          {at::kEnvAmt, 0.15f}, {at::kShAmt, 0.15f}, {at::kSustain, 0.8f}, {at::kRelease, 0.5f}, {at::kVolume, 1.0f}},
         {atCutoff(1200), atRes(), atEnv(), atDecay(0.4f), atShRate(8.0f), {"Steps", true, {{at::kVco2Mod, 0.3f}}}}, A},
        {F, "OT Noise Drum", "Noise through the resonant 2-pole with a fast decay; Priority Low: on a slot two tracks share, the lower held note wins; M6 the snap.", 1,
         {{at::kVco1Level, 0.0f}, {at::kVco2Level, 0.0f}, {at::kSourceLevel, 1.0f}, {at::kFilterMode, k2P}, {at::kCutoff, 1200.0f},
          {at::kResonance, 0.6f}, {at::kKeyTrack, 1.0f}, {at::kEnvAmt, 0.35f}, {at::kHpf, 120.0f}, {at::kAttack, 0.0f},
          {at::kDecay, 0.18f}, {at::kSustain, 0.0f}, {at::kRelease, 0.12f}, {at::kPriority, float(at::kLow)}, {at::kVolume, 1.5f}},
         {atCutoff(1000), atRes(), atEnv(), atDecay(0.15f), atVibrato(0.2f), {"Snap", true, {{at::kEnvAmt, 0.2f}, {at::kDecay, -0.008f}}}}, A},
        {F, "OT Lead 2-Osc", "Saw and pulse slightly apart, a singing 4-pole, legato glide 60 ms; velocity to the envelope; M5 vibrato.", 1,
         {{at::kVco2Wave, kPulseW}, {at::kVco2Pw, 0.2f}, {at::kVco2Fine, 9.0f}, {at::kVco1Level, 0.6f}, {at::kVco2Level, 0.5f},
          {at::kCutoff, 1400.0f}, {at::kResonance, 0.4f}, {at::kEnvAmt, 0.35f}, {at::kDecay, 0.7f}, {at::kSustain, 0.6f},
          {at::kRelease, 0.3f}, {at::kGlideMode, float(at::kLegato)}, {at::kGlideTime, 0.06f}, {at::kVolume, 1.1f}},
         {atCutoff(1200), atRes(), atEnv(), atDecay(0.5f), atVibrato(0.35f), {"Detune", true, {{at::kVco2Fine, 0.2f}}}}, A, kAtVelEnv},
        {F, "OT Noise Wind", "Noise into the 2-pole near self-oscillation, the cutoff swept by a slow LFO, slow attack: wind; M6 the LFO rate.", 1,
         {{at::kVco1Level, 0.0f}, {at::kVco2Level, 0.0f}, {at::kSourceLevel, 0.8f}, {at::kFilterMode, k2P}, {at::kCutoff, 700.0f},
          {at::kResonance, 0.85f}, {at::kKeyTrack, 0.3f}, {at::kEnvAmt, 0.1f}, {at::kLfoAmt, 0.45f}, {at::kLfoRate, 0.2f},
          {at::kAttack, 1.0f}, {at::kDecay, 2.0f}, {at::kSustain, 0.9f}, {at::kRelease, 1.5f}, {at::kVolume, 1.2f}},
         {atCutoff(500), atRes(0.04f), atEnv(), atDecay(1.5f), atVibrato(0.2f), {"LFO Rate", true, {{at::kLfoRate, 0.02f}}}}, A},
        {F, "OT Sync Scream", "Synced VCO 2 an octave up, its pitch wobbled by the LFO, a resonant 4-pole above a high-pass: a screaming lead.", 1,
         {{at::kSync, 1.0f}, {at::kVco2Tune, 12.0f}, {at::kVco2Fine, 0.0f}, {at::kVco1Level, 0.2f}, {at::kVco2Level, 0.8f},
          {at::kVco2Mod, 0.45f}, {at::kLfoRate, 5.5f}, {at::kCutoff, 2500.0f}, {at::kResonance, 0.6f}, {at::kHpf, 300.0f},
          {at::kEnvAmt, 0.3f}, {at::kDecay, 0.8f}, {at::kSustain, 0.7f}, {at::kRelease, 0.3f}, {at::kVolume, 1.0f}},
         {atCutoff(2000), atRes(0.15f), atEnv(), atDecay(0.5f), atVibrato(0.3f), atTune("Scream", 12.0f)}, A, kAtVelEnv},
        {F, "OT Duo Organ", "Duo: two square waves (16' and 8') on an AR, the 2-pole open: a two-voice organ from two tracks; M6 VCO 2's level.", 1,
         {{at::kKeyboard, kDuoK}, {at::kVco1Wave, kPulseW}, {at::kVco2Wave, kPulseW}, {at::kVco1Range, kA16}, {at::kVco2Fine, 2.0f},
          {at::kVco1Level, 0.6f}, {at::kVco2Level, 0.6f}, {at::kFilterMode, k2P}, {at::kCutoff, 3500.0f}, {at::kResonance, 0.0f},
          {at::kEnvAmt, 0.0f}, {at::kVcaEnv, kArE}, {at::kArAttack, 0.005f}, {at::kArRelease, 0.1f}, {at::kVolume, 1.0f}},
         {atCutoff(2500), atRes(), atEnv(), atDecay(0.3f), atVibrato(0.2f), {"VCO2 Level", true, {{at::kVco2Level, 0.4f}}}}, A},
    };
}
#endif  // OT_MOD_ATY1

#if defined(OT_MOD_TTY1)
// ---------------------------------------------------------------------------
// T-Type (module TTY1, engine 0x85, Patches/TTYPE/): one patch is one kit of
// ten drums (kick 60, rim 61, snare 62, clap 63 / 71, hats 64-67, cowbell 68,
// crash 69, ride 70, toms 48-59), each with a Character, per-drum sends,
// Accent, Drive and Width. Every kit has the same macros: M1 Kick Decay, M2
// Hat Decay, M3 Tune (kick, snare, toms), M4 Drive, M5 Snappy (snare noise and
// clap tail), M6 Accent. Poly 1: one voice owns the kit, the drums overlap.

namespace tt = ot::audio::ttype;
static_assert(kTtypeEngine == tt::kEngineId, "presets.h kTtypeEngine");

// Macro amounts x stored range: Kick Decay 0.05..2.5 s, Hat Decays 0.01..0.5 /
// 0.1..3 s, Tunes 30..120 / 120..400 / 40..200 / 60..300 / 80..400 Hz, levels 0..1.
std::vector<Macro> ttMacros() {
    return {{"Kick Decay", true, {{tt::kKickDecay, 0.15f}}},
            {"Hat Decay", true, {{tt::kHatOpenDecay, 0.12f}, {tt::kHatClosedDecay, 0.08f}}},
            {"Tune", true,
             {{tt::kKickTune, 0.1f}, {tt::kSnareTune, 0.08f}, {tt::kTomLoTune, 0.08f}, {tt::kTomMidTune, 0.08f},
              {tt::kTomHiTune, 0.08f}}},
            {"Drive", false, {{tt::kDrive, 0.6f}, {tt::kKickDrive, 0.4f}}},
            {"Snappy", true, {{tt::kSnareSnappy, 0.35f}, {tt::kClapTail, 0.35f}}},
            {"Accent", true, {{tt::kAccent, 0.5f}}}};
}
constexpr int ttSend(int sound, bool reverb) { return tt::sendParam(sound, reverb); }

std::vector<Preset> ttypePresets() {
    const SynthModel F = SynthModel::PERC;   // the Mini fallback model of the slot record (a kit)
    const int32_t T = kTtypeEngine;
    const std::vector<Macro> M = ttMacros();
    return {
        {F, "OT Kit Hybrid Punch", "Punchy hybrid kick with a click, a cracking snare, noisy hats, a wide clap: the all-round techno kit.", 1,
         {{tt::kKickDecay, 0.45f}, {tt::kKickDrive, 0.3f}, {tt::kKickAttack, 0.5f}, {tt::kSnareTone, 0.6f},
          {tt::kHatClosedDecay, 0.05f}, {tt::kHatOpenDecay, 0.4f}},
         M, T},
        {F, "OT Kit Deep Boom", "A long, deep, pure kick (Boom), soft snare, round toms, metal hats and a crash: deep house and dub techno.", 1,
         {{tt::kKickChar, 1.0f}, {tt::kKickTune, 46.0f}, {tt::kKickDecay, 1.1f}, {tt::kKickSweep, 0.4f}, {tt::kKickAttack, 0.2f},
          {tt::kSnareChar, 1.0f}, {tt::kSnareDecay, 0.3f}, {tt::kTomChar, 1.0f}, {tt::kHatChar, 1.0f}, {tt::kAccent, 0.8f},
          {ttSend(tt::kSSnare, true), 0.4f}},
         M, T},
        {F, "OT Kit Tight", "Small and tight: a knocking kick, a thin snare, short toms and hats, a two-burst clap, a low cowbell.", 1,
         {{tt::kKickChar, 2.0f}, {tt::kKickTune, 62.0f}, {tt::kKickDecay, 0.3f}, {tt::kSnareChar, 2.0f}, {tt::kSnareDecay, 0.12f},
          {tt::kClapChar, 2.0f}, {tt::kTomChar, 2.0f}, {tt::kBellChar, 2.0f}, {tt::kHatChar, 2.0f}, {tt::kHatClosedDecay, 0.04f},
          {tt::kHatOpenDecay, 0.3f}, {tt::kWidth, 0.4f}},
         M, T},
        {F, "OT Kit Industrial", "Overdriven: the kick's drive and the kit Drive up, a bright cracking snare, metal hats, a thin cymbal, popping rim.", 1,
         {{tt::kKickDrive, 0.8f}, {tt::kKickAttack, 0.7f}, {tt::kKickDecay, 0.5f}, {tt::kDrive, 0.45f}, {tt::kSnareTone, 0.85f},
          {tt::kSnareSnappy, 0.75f}, {tt::kHatChar, 1.0f}, {tt::kHatTone, 0.8f}, {tt::kCymChar, 2.0f}, {tt::kRimChar, 1.0f},
          {ttSend(tt::kSSnare, true), 0.45f}, {ttSend(tt::kSClap, true), 0.45f}},
         M, T},
        {F, "OT Kit Minimal Tech", "Minimal: a short punchy kick, a loud stick rim, thin closed hats with an echo, a clanging cowbell, a dry clap.", 1,
         {{tt::kKickTune, 52.0f}, {tt::kKickDecay, 0.3f}, {tt::kRimLevel, 0.9f}, {tt::kHatChar, 2.0f}, {tt::kHatClosedDecay, 0.05f},
          {tt::kBellChar, 0.0f}, {tt::kClapChar, 1.0f}, {tt::kWidth, 0.45f}, {ttSend(tt::kSHat, false), 0.3f},
          {ttSend(tt::kSRim, false), 0.25f}},
         M, T},
        {F, "OT Kit Acid House", "A booming kick, a wide clap with a long tail, a soft snare, noisy open hats, a bright cowbell, a ride.", 1,
         {{tt::kKickChar, 1.0f}, {tt::kKickDecay, 0.5f}, {tt::kKickAttack, 0.3f}, {tt::kClapTail, 0.75f}, {tt::kClapDecay, 0.4f},
          {tt::kSnareChar, 1.0f}, {tt::kHatOpenDecay, 0.45f}, {tt::kBellChar, 1.0f}, {tt::kCymChar, 1.0f},
          {ttSend(tt::kSClap, true), 0.4f}},
         M, T},
        {F, "OT Kit Dub", "Dub techno: deep kick, a soft snare and a popping rim into the delay and the reverb, metal hats with an echo.", 1,
         {{tt::kKickChar, 1.0f}, {tt::kKickDecay, 0.7f}, {tt::kKickTune, 50.0f}, {tt::kSnareChar, 1.0f}, {tt::kSnareDecay, 0.35f},
          {tt::kRimChar, 1.0f}, {tt::kHatChar, 1.0f}, {ttSend(tt::kSSnare, true), 0.6f}, {ttSend(tt::kSSnare, false), 0.4f},
          {ttSend(tt::kSRim, false), 0.5f}, {ttSend(tt::kSHat, false), 0.4f}, {ttSend(tt::kSClap, true), 0.5f}},
         M, T},
        {F, "OT Kit Electro", "Electro: a knocking kick with a long sweep, thin snare, a tight clap, sweeping toms, a bright cowbell.", 1,
         {{tt::kKickChar, 2.0f}, {tt::kKickSweep, 0.9f}, {tt::kKickDecay, 0.6f}, {tt::kSnareChar, 2.0f}, {tt::kClapChar, 2.0f},
          {tt::kTomChar, 0.0f}, {tt::kTomLoDecay, 0.6f}, {tt::kTomMidDecay, 0.5f}, {tt::kTomHiDecay, 0.45f}, {tt::kBellChar, 1.0f},
          {tt::kHatChar, 2.0f}},
         M, T},
        {F, "OT Kit Hard", "Hard techno: a driven punchy kick with a loud click, kit Drive, a snappy snare, a dry clap, bright noisy hats.", 1,
         {{tt::kKickDrive, 1.0f}, {tt::kKickAttack, 0.8f}, {tt::kKickDecay, 0.4f}, {tt::kKickTune, 58.0f}, {tt::kDrive, 0.35f},
          {tt::kSnareSnappy, 0.7f}, {tt::kClapChar, 1.0f}, {tt::kHatTone, 0.7f}, {tt::kHatOpenDecay, 0.35f}},
         M, T},
        {F, "OT Kit Dusty Tape", "Lo-fi: a dull booming kick, a dark soft snare, dark metal hats, a ringing cymbal, narrow, light accents.", 1,
         {{tt::kKickChar, 1.0f}, {tt::kKickTune, 50.0f}, {tt::kKickAttack, 0.1f}, {tt::kSnareChar, 1.0f}, {tt::kSnareTone, 0.3f},
          {tt::kHatChar, 1.0f}, {tt::kHatTone, 0.2f}, {tt::kCymChar, 1.0f}, {tt::kCymTone, 0.2f}, {tt::kWidth, 0.3f},
          {tt::kAccent, 0.6f}},
         M, T},
        {F, "OT Kit Broken", "Broken beat: a knocking kick, a cracking snare, short toms, a ticking rim, a low cowbell, a tight clap.", 1,
         {{tt::kKickChar, 2.0f}, {tt::kKickDecay, 0.35f}, {tt::kSnareDecay, 0.3f}, {tt::kTomChar, 2.0f}, {tt::kRimChar, 2.0f},
          {tt::kBellChar, 2.0f}, {tt::kClapChar, 2.0f}, {tt::kHatChar, 2.0f}, {tt::kHatClosedDecay, 0.05f}},
         M, T},
        {F, "OT Kit Rave", "Rave: a long punchy kick with drive, a wide clap into the reverb, a cracking snare, a ringing ride, open hats.", 1,
         {{tt::kKickSweep, 0.8f}, {tt::kKickDrive, 0.5f}, {tt::kKickDecay, 0.55f}, {tt::kClapTail, 0.8f}, {tt::kClapDecay, 0.45f},
          {tt::kCymChar, 1.0f}, {tt::kHatOpenDecay, 0.6f}, {tt::kDrive, 0.25f}, {ttSend(tt::kSClap, true), 0.5f},
          {ttSend(tt::kSRide, true), 0.3f}},
         M, T},
    };
}
#endif  // OT_MOD_TTY1

void fill(ot::PatchMacro& m, const Macro& d) {
    std::snprintf(m.name, sizeof(m.name), "%s", d.name);
    m.nameLen = uint8_t(std::strlen(m.name));
    m.bipolar = d.bipolar;
    m.count = 0;
    for (const Target& t : d.targets) {
        if (m.count >= ot::kPatchMacroTargets) break;
        m.targets[m.count].param = t.param;
        m.targets[m.count].amountBits = ot::patchFloatToBits(t.amount);
        ++m.count;
    }
}

// The section heading of an engine: its screen name (D-Type; the folder is DTYPE).
const char* modelName(int32_t engine) { return ot::synthEngineDisplayName(engine); }

}  // namespace

const std::vector<Preset>& all() {
    static const std::vector<Preset> v = [] {
        std::vector<Preset> out;
        auto add = [&out](std::vector<Preset> ps) {
            for (Preset& p : ps) out.push_back(p);
        };
#if defined(OT_MOD_ACD1)
        add(acdPresets());
#endif
#if defined(OT_MOD_FAT1)
        add(fatPresets());
#endif
#if defined(OT_MOD_VAP1)
        add(vapPresets());
#endif
#if defined(OT_MOD_WTF1)
        add(wtfmPresets());
#endif
#if defined(OT_MOD_PRC1)
        add(percPresets());
#endif
#if defined(OT_MOD_JU6S)
        add(ju6Presets());
#endif
#if defined(OT_MOD_DTY1)
        add(dtypePresets());
#endif
#if defined(OT_MOD_CST1)
        add(cstypePresets());
#endif
#if defined(OT_MOD_ATY1)
        add(atypePresets());
#endif
#if defined(OT_MOD_TTY1)
        add(ttypePresets());
#endif
        (void)add;
        return out;
    }();
    return v;
}

int32_t engineOf(const Preset& p) { return p.engine ? p.engine : ot::synthEngineId(p.model); }

const char* folderOf(const Preset& p) { return ot::synthEngineFolder(engineOf(p)); }

bool build(ot::App& a, int s, const Preset& pr, std::string& err) {
    a.setSynthModel(s, int(pr.model));
    if (pr.engine) a.setSynthEngine(s, pr.engine);   // a module engine: the record keeps pr.model
    a.initSynthPatch(s);   // the built-in Init, never a library patch from the card
    ot::SynthPatch& p = a.synthPatch(s);
    const int32_t engine = engineOf(pr);
    if (a.slotEngineId(s) != engine || p.engine != engine) {
        err = std::string(pr.name) + ": engine " + std::to_string(engine) + " not in this build";
        return false;
    }
    for (const Edit& e : pr.edits) {
        if (!ot::patchParamInRange(engine, e.index, e.value)) {
            err = std::string(pr.name) + ": parameter " + std::to_string(e.index) + " out of range";
            return false;
        }
        if (!ot::audio::synthParamSet(p, e.index, e.value)) {
            err = std::string(pr.name) + ": cannot set parameter " + std::to_string(e.index);
            return false;
        }
    }
    if (pr.macros.size() > size_t(ot::audio::kSynthSeqMacros)) {
        err = std::string(pr.name) + ": more than 6 macros";
        return false;
    }
    for (size_t k = 0; k < pr.macros.size(); ++k) fill(p.macros[k], pr.macros[k]);
    ot::PatchMacro& m7 = p.macros[ot::kPatchMacroVelocity];
    for (const Target& t : pr.velocity) {
        if (m7.count >= ot::kPatchMacroTargets) {
            err = std::string(pr.name) + ": too many Velocity targets";
            return false;
        }
        m7.targets[m7.count].param = t.param;
        m7.targets[m7.count].amountBits = ot::patchFloatToBits(t.amount);
        ++m7.count;
    }
    ot::PatchIssue issues[4];
    if (const int n = ot::validatePatch(p, issues, 4)) {
        err = std::string(pr.name) + ": " + std::to_string(n) + " validation issue(s), first kind " +
              std::to_string(int(issues[0].kind)) + " index " + std::to_string(issues[0].index);
        return false;
    }
    a.markSynthPatchDirty(s);
    ot::SynthSlotRecord r = a.synthSlot(s);
    std::snprintf(r.patchName, sizeof(r.patchName), "%s", pr.name);
    for (int k = 0; k < int(ot::mt::kSynthMacroCount); ++k)
        r.macros[k] = ot::audio::synthMacroDefault(p.macros[k].bipolar);
    a.setSynthSlotRecord(s, r);
    return true;
}

bool buildAndSave(ot::App& a, int s, const Preset& pr, std::string& err) {
    if (!build(a, s, pr, err)) return false;
    if (!a.saveSynthLibraryPatch(s, pr.name)) {
        err = std::string(pr.name) + ": Save As failed";
        return false;
    }
    return true;
}

std::string markdown() {
    std::string o;
    o += "# OpenTracker preset pack\n\n";
    o += "Our own synth patches for the ACD, FAT, VAP, WTFM and PERC engines of OpenTracker 2.x and the JU6,\n";
    o += "D-Type, CS-Type, A-Type and T-Type module engines of OpenTracker 3.x, written by\n";
    o += "`reimpl/tools/make_patches` (`make -C reimpl patches`) through the firmware's own Save As path.\n";
    o += "Every patch starts from the engine's built-in Init patch; no factory patch is used.\n\n";
    o += "Files: `Patches/<MODEL>/<name>.patch`. Copy the `Patches` folder to the root of the SD card,\n";
    o += "then use Load Patch on a synth page.\n\n";
    o += "Macros: M1-M6 (FX CC A-F). A bipolar macro (+-) at 63 and a unipolar macro at 0 play the\n";
    o += "patch as saved. M7 is Velocity (Volume FX), as in the Init patch. Levels are voiced for the\n";
    o += "default slot gain (-9 dB); `tests/patches_test.cpp` checks that every patch is audible, stays\n";
    o += "at least 1 dB below full scale (VAP, JU6, CS-Type, FAT and WTFM Poly 4: 4-voice chords; A-Type: also a\n";
    o += "Duo pair from two tracks; PERC: five drums at once, toms alone; T-Type: every drum alone, then eight\n";
    o += "at once, toms\n";
    o += "alone) and does not clip with all macros at either extreme.\n\n";
    o += "Poly: the suggested slot polyphony (1 = mono, 4 = chords). It is a slot setting on the synth\n";
    o += "page, not part of the patch.\n";
    int32_t cur = -1;
    int count = 0;
    for (const Preset& p : all()) {
        if (engineOf(p) != cur) {
            cur = engineOf(p);
            int n = 0;
            for (const Preset& q : all()) n += engineOf(q) == cur;
            o += std::string("\n## ") + modelName(cur) + " (" + std::to_string(n) + ")\n\n";
            if (cur == kJu6Engine)
                o += "JU6 is a module engine (3.x, module JU6S): pick it after PERC in the Synth page model list.\n"
                     "Its stereo chorus is built in, as on the instrument it is modelled on: the patch parameter\n"
                     "Chorus (Off / I / II, Edit Patch section CHO) runs once per synth slot on the slot's voices.\n"
                     "Pads and strings use II, brass, keys, plucks and leads I, the bass is dry; the Chorus macro\n"
                     "(where there is one) steps it up a mode. A JU6 patch saved before the chorus loads with\n"
                     "Chorus Off. Every JU6 patch is checked with 4-voice chords, chorus on.\n\n";
            if (cur == kDtypeEngine)
                o += "D-Type is a module engine (3.x, module DTY1, folder `Patches/DTYPE/`): pick it after JU6 in\n"
                     "the Synth page model list. It is a mono synth (three oscillators, noise, a 4-pole ladder):\n"
                     "set the slot's Poly to 1 for last-note priority, legato and glide. M1-M4 are the same on\n"
                     "every patch (Cutoff, Emphasis, Contour, Decay of both contours); Wheel (M5 where it is the\n"
                     "Wheel or Vibrato macro) brings in the modulation of VCO 3 or the noise. Leads and brass\n"
                     "(Fat Lead, Brass Section, Sync Bite) also scale the Contour with the velocity (M7).\n\n";
            if (cur == kCstypeEngine)
                o += "CS-Type is a module engine (3.x, module CST1, folder `Patches/CSTYPE/`): pick it after D-Type\n"
                     "in the Synth page model list. Every note plays two layers (I and II), each a VCO into a\n"
                     "resonant high-pass and low-pass and its own VCA; Poly is the slot setting (4 for chords).\n"
                     "M1 Brilliance, M2 Resonance, M3 Touch (after-touch: FX CC c brings it in), M4 Release,\n"
                     "M5 Ribbon (pitch, +-), M6 the patch's own. Velocity (M7) also scales each layer's filter\n"
                     "attack level (Init -> Bril).\n\n";
            if (cur == kAtypeEngine)
                o += "A-Type is a module engine (3.x, module ATY1, folder `Patches/ATYPE/`): pick it after CS-Type\n"
                     "in the Synth page model list. Two VCOs with sync, ring modulator, sample & hold. Play it at\n"
                     "Poly 1: Keyboard Duo splits two held notes (from two tracks on the slot, or two MIDI keys),\n"
                     "VCO 1 the lowest, VCO 2 the highest; Priority Last / Low / High picks the note in Mono, and\n"
                     "releasing a key returns to a note still held. M1-M4 are the same on every patch (Cutoff,\n"
                     "Resonance, Env Amt, Decay + Release); leads (Sync Tear, Lead 2-Osc, Sync Scream) also scale\n"
                     "the envelope amount with the velocity (M7).\n\n";
            if (cur == kTtypeEngine)
                o += "T-Type is a module engine (3.x, module TTY1, folder `Patches/TTYPE/`): a drum synth in the\n"
                     "classic analog drum machine style, pick it after D-Type in the Synth page model list. One\n"
                     "patch is one kit; play it at Poly 1 (the drums overlap in the one voice). Notes: kick 60, rim\n"
                     "61, snare 62, clap 63 and 71, hats 64-67 (closed to open; a closed hat cuts an open one),\n"
                     "cowbell 68, crash 69, ride 70 (one cymbal), low / mid / high tom 48-51 / 52-55 / 56-59 (a\n"
                     "semitone up per note); notes from 72 up repeat 60-71, notes below 48 play the low tom. Every\n"
                     "drum has a Character (punchy hybrid / deep analog / small tight). The Volume FX is the hit's\n"
                     "velocity through Accent. Every kit has the same macros: M1 Kick Decay, M2 Hat Decay, M3 Tune\n"
                     "(kick, snare, toms), M4 Drive, M5 Snappy (snare noise, clap tail), M6 Accent. The per-drum\n"
                     "sends sound when the instrument's own Delay / Reverb send is up.\n\n";
            if (cur == ot::synthEngineId(SynthModel::PERC))
                o += "Notes (MIDI numbers): kick 60, snare 62, hi-hats 64-67 (closed to open), cymbal 69, perc 71; any other\n"
                     "note plays the tom at that pitch. The `OT Kit SW` kits are synthwave / retrowave kits\n"
                     "(gated snares, sweeping toms); their per-drum Delay / Reverb sends sound only when the\n"
                     "instrument's own Delay / Reverb send is up (drum send = patch send x instrument send).\n\n";
            o += "| Patch | Description | Poly | Macros M1-M6 |\n|---|---|---|---|\n";
        }
        std::string macros;
        for (size_t k = 0; k < p.macros.size(); ++k) {
            if (k) macros += ", ";
            macros += p.macros[k].name;
            if (p.macros[k].bipolar) macros += " (+-)";
        }
        o += std::string("| ") + p.name + " | " + p.desc + " | " + std::to_string(p.playPoly) + " | " + macros +
             " |\n";
        ++count;
    }
    o += "\nTotal: " + std::to_string(count) + " patches.\n";
    return o;
}

}  // namespace presets
