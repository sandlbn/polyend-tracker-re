// Synth parameters by index and the macro law (2.x M3, docs/MINI_PLAN.md §3,
// spec 21 §7, §8 with the MINI_PLAN corrections) [OT].
//
// Pure functions over SynthPatch; used by the framework (audio context) and
// by the synth UI (WP-G, main loop). Ranges come from the patch codec tables
// (formats/patch_pb.h: patchEngineParams).
//
// Macro law (U8, owner decision): for every target parameter i of macros
// M1..M6 with patch value p0 and stored range [min, max]
//     p = clamp(p0 + sum(amount * m * (max - min)), min, max)
//     m = v / 127            (unipolar macro, v = 0..127)
//     m = (v - 63) / 64      (bipolar macro, 63 = centre)
// Targets are macros[k].targets[0 .. count) (count clamped 0..6); a target
// whose param is -1 (or not a parameter of the engine) is skipped.
//
// Macro 7 ("Velocity", driven by Volume / Random Volume FX) [OT extension of
// U8]: the additive law cannot make a note quieter than the patch (every
// factory patch has M7 -> Volume +1 with Volume = 1), so M7 is relative to
// full velocity: with m7 = velocity (0..1, 1 = no Volume FX)
//     p = clamp(p0 * (1 + sum(amount * (m7 - 1))), min, max)
// i.e. Volume +1 scales the level linearly with the velocity and a note
// without a Volume FX plays the patch exactly as saved. For an engine with
// no Volume parameter (PERC) the velocity scales the voice gain directly.
// Macro 8 ("Pressure") is stored only.
#pragma once

#include <cstdint>

#include "model/synth_patch.h"

namespace ot {
namespace audio {

constexpr int kSynthSeqMacros = 6;   // M1..M6: FX CC a..f and the project page values

// Parameter table access (nullptr / 0 for an unknown engine or index).
// synthParamCount is the Mini's parameter count (macro targets, the
// engine's own table); indexes count .. synthParamTotal - 1 are the [OT]
// extension parameters (model/synth_patch.h kPatchExtField), which every
// function below also takes.
int synthParamCount(int32_t engine);
int synthParamTotal(int32_t engine);
// [OT] Width (0..1, 0 = mono) of an engine, -1 for PERC / unknown.
int synthWidthParam(int32_t engine);
// [OT] FAT Stereo (0..1, 0 = mono): the stack split between L and R.
int synthFatStereoParam();
const PatchParamRange* synthParamRange(int32_t engine, int index);
// Stored value of parameter `index` (the default when absent or out of range).
float synthParamGet(const SynthPatch& p, int index);
// Set parameter `index` (clamped to its range, marked present). False for a
// bad index or a non-finite value. Call SynthBank::patchChanged() afterwards.
bool synthParamSet(SynthPatch& p, int index, float v);
// Clamp v into the range of parameter `index` (unchanged for "no range").
float synthParamClamp(int32_t engine, int index, float v);

// Macro value -> m (see above).
inline float synthMacroM(uint8_t v, bool bipolar) {
    if (v > 127) v = 127;
    return bipolar ? (float(int(v) - 63) * (1.0f / 64.0f)) : float(v) * (1.0f / 127.0f);
}
// Page / default value of a macro: 63 when bipolar, else 0 (spec 21 §3.3).
inline uint8_t synthMacroDefault(bool bipolar) { return bipolar ? 63 : 0; }
// Number of live targets of macro k (count clamped 0..6).
int synthMacroTargets(const PatchMacro& m);

// Every effective parameter of the patch: out[0..count) = patch values with
// macros M1..M6 at `values[0..5]` and M7 at `velocity` (law above). Returns
// the parameter count (0 for an unknown engine).
int synthEffectiveParams(const SynthPatch& p, const uint8_t values[kSynthSeqMacros], float velocity, float* out);

}  // namespace audio
}  // namespace ot
