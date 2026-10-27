// Synth parameters by index and the macro law (synth_params.h).
#include "audio/synth_params.h"

#include <cmath>

#include "formats/patch_pb.h"

namespace ot {
namespace audio {

int synthParamCount(int32_t engine) {
    int n = 0;
    patchEngineParams(engine, &n);
    return n;
}

int synthParamTotal(int32_t engine) {
    int n = 0, ne = 0;
    patchEngineParams(engine, &n);
    patchEngineExtParams(engine, &ne);
    return n + ne;
}

int synthWidthParam(int32_t engine) {
    int ne = 0;
    patchEngineExtParams(engine, &ne);
    return ne > 0 ? synthParamCount(engine) : -1;
}

int synthFatStereoParam() { return synthParamCount(kSynthEngineFat) + 1; }

const PatchParamRange* synthParamRange(int32_t engine, int index) {
    int n = 0, ne = 0;
    const PatchParamRange* t = patchEngineParams(engine, &n);
    const PatchParamRange* x = patchEngineExtParams(engine, &ne);
    if (!t || index < 0) return nullptr;
    if (index < n) return &t[index];
    return index < n + ne ? &x[index - n] : nullptr;
}

float synthParamClamp(int32_t engine, int index, float v) {
    const PatchParamRange* r = synthParamRange(engine, index);
    if (!r || (r->flags & kParamFlagNoRange)) return v;
    return v < r->min ? r->min : (v > r->max ? r->max : v);
}

float synthParamGet(const SynthPatch& p, int index) {
    const PatchParamRange* r = synthParamRange(p.engine, index);
    if (!r) return 0.0f;
    const int n = synthParamCount(p.engine);
    const PatchParam* e = nullptr;
    if (index < n) {
        if (index < p.paramCount) e = &p.params[index];
    } else if (index - n < p.extCount) {
        e = &p.ext[index - n];
    }
    if (e && e->present) {
        const float v = patchBitsToFloat(e->bits);
        if (patchParamInRange(p.engine, index, v)) return v;
    }
    return r->def;
}

bool synthParamSet(SynthPatch& p, int index, float v) {
    const PatchParamRange* r = synthParamRange(p.engine, index);
    if (!r || !std::isfinite(v)) return false;
    v = synthParamClamp(p.engine, index, v);
    const int mini = synthParamCount(p.engine);
    if (index >= mini) {
        // [OT] extension: grow up to it; entries in front stay absent.
        const int k = index - mini;
        for (int i = p.extCount; i <= k; ++i) p.ext[i] = PatchParam{0u, false};
        if (k >= p.extCount) p.extCount = uint8_t(k + 1);
        p.ext[k].bits = patchFloatToBits(v);
        p.ext[k].present = true;
        return true;
    }
    if (index >= p.paramCount) {
        // Grow to the engine count; new entries keep the default (absent).
        const int n = synthParamCount(p.engine);
        for (int i = p.paramCount; i < n; ++i) p.params[i] = PatchParam{0u, false};
        p.paramCount = uint16_t(n);
    }
    p.params[index].bits = patchFloatToBits(v);
    p.params[index].present = true;
    return true;
}

int synthMacroTargets(const PatchMacro& m) {
    return m.count < 0 ? 0 : (m.count > kPatchMacroTargets ? kPatchMacroTargets : int(m.count));
}

int synthEffectiveParams(const SynthPatch& p, const uint8_t values[kSynthSeqMacros], float velocity, float* out) {
    int n = 0;
    const PatchParamRange* t = patchEngineParams(p.engine, &n);
    if (!t) return 0;
    for (int i = 0; i < n; ++i) out[i] = synthParamGet(p, i);
    // M1..M6: additive, scaled by the parameter range (U8).
    float add[kPatchMaxParams] = {};
    bool hit[kPatchMaxParams] = {};
    for (int k = 0; k < kSynthSeqMacros; ++k) {
        const PatchMacro& m = p.macros[k];
        const float mv = synthMacroM(values[k], m.bipolar);
        for (int j = 0, c = synthMacroTargets(m); j < c; ++j) {
            const int i = m.targets[j].param;
            if (i < 0 || i >= n) continue;   // empty target (-1) inside count
            const float a = patchBitsToFloat(m.targets[j].amountBits);
            if (!std::isfinite(a)) continue;
            add[i] += a * mv * (t[i].max - t[i].min);
            hit[i] = true;
        }
    }
    for (int i = 0; i < n; ++i)
        if (hit[i]) out[i] = synthParamClamp(p.engine, i, out[i] + add[i]);
    // M7: relative to full velocity (synth_params.h).
    const PatchMacro& m7 = p.macros[kPatchMacroVelocity];
    float mul[kPatchMaxParams];
    for (int i = 0; i < n; ++i) mul[i] = 1.0f;
    bool any = false;
    for (int j = 0, c = synthMacroTargets(m7); j < c; ++j) {
        const int i = m7.targets[j].param;
        if (i < 0 || i >= n) continue;
        const float a = patchBitsToFloat(m7.targets[j].amountBits);
        if (!std::isfinite(a)) continue;
        mul[i] += a * (velocity - 1.0f);
        any = true;
    }
    if (any)
        for (int i = 0; i < n; ++i)
            if (mul[i] != 1.0f) out[i] = synthParamClamp(p.engine, i, out[i] * mul[i]);
    return n;
}

}  // namespace audio
}  // namespace ot
