// The project's synth presets (see synth_presets.h); their files are handled
// by the project jobs in project_io.cpp.
#include "app/synth_presets.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "audio/synth_engine.h"
#include "audio/synth_params.h"
#include "sdram_bss.h"

namespace ot {

namespace {
OT_SDRAM_BSS SynthPatch g_presets[kSynthPresetCount];
OT_SDRAM_BSS uint8_t g_presetDirty;
bool validPreset(int n) { return n >= 1 && n <= kSynthPresetCount; }
// The audio interrupt reads an entry whose engine is not 0 (a Z in the pattern):
// the engine goes first when an entry is written, and last when it is filled.
void setEngine(SynthPatch& p, int32_t engine) {
    std::atomic_signal_fence(std::memory_order_seq_cst);
    *const_cast<volatile int32_t*>(&p.engine) = engine;
    std::atomic_signal_fence(std::memory_order_seq_cst);
}
// A mix value as the slot record holds it (out of range: held at its limit;
// not a number: `def`).
float held(float v, float lo, float hi, float def) {
    if (!std::isfinite(v)) return def;
    return v < lo ? lo : (v > hi ? hi : v);
}
OT_SDRAM_BSS uint32_t g_presetResets;
}  // namespace

namespace presets {
SynthPatch* table() { return g_presets; }
uint8_t& dirty() { return g_presetDirty; }
void copyBehindEngine(SynthPatch& to, const SynthPatch& from) {
    static_assert(offsetof(SynthPatch, engine) == 0, "the engine leads the patch");
    std::memcpy(reinterpret_cast<uint8_t*>(&to) + sizeof(to.engine),
                reinterpret_cast<const uint8_t*>(&from) + sizeof(from.engine), sizeof(SynthPatch) - sizeof(to.engine));
}
int modelOfEngine(int32_t e) {
    for (int m = 0; m < kSynthModelCount; ++m)
        if (synthEngineId(SynthModel(m)) == e) return m;
    return -1;
}
}  // namespace presets
using presets::copyBehindEngine;
static_assert(kPatchPresetNameMax == int(mt::kSynthPatchNameSize), "a preset carries a record's patch name");

const SynthPatch& App::synthPreset(int n) const { return g_presets[validPreset(n) ? n - 1 : 0]; }
bool App::synthPresetUsed(int n) const { return validPreset(n) && g_presets[n - 1].engine != 0; }
bool App::synthPresetDirty() const { return g_presetDirty != 0; }
int App::synthPresetActive(int s) const { return engine.synth().presetOf(s); }

// A slot on preset n goes back to its patch before the entry changes.
void App::leaveSynthPreset(int n) {
    for (int s = 0; s < kSynthSlotCount; ++s)
        if (engine.synth().presetOf(s) == n) engine.synth().patchChanged(s);
}

// [OT] Edit what you hear (see app.h).
int App::synthEditTarget(int s) const {
    const int n = engine.synth().presetOf(s);
    return synthPresetUsed(n) ? n : 0;
}
const SynthPatch& App::synthTargetPatch(int s) const {
    const int n = synthEditTarget(s);
    return n ? g_presets[n - 1] : synthPatch(s);
}
SynthPatch& App::synthTargetPatch(int s) {
    const int n = synthEditTarget(s);
    return n ? g_presets[n - 1] : synthPatch(s);
}

SynthPatch& App::synthTargetPatch(int s, int n) {
    return synthPresetUsed(n) ? g_presets[n - 1] : synthPatch(s);
}

SynthSlotRecord App::synthTargetRecord(int s, int n) const {
    SynthSlotRecord r = project.synthSlot(s);
    if (!synthPresetUsed(n)) return r;
    const SynthPatch& p = g_presets[n - 1];
    const int m = presets::modelOfEngine(p.engine);
    r.model = m < 0 ? uint32_t(kSynthModelCount) : uint32_t(m);
    std::snprintf(r.patchName, sizeof(r.patchName), "%s", p.presetName);
    if (patchHasPoly(p)) r.polyphony = p.poly;
    if (patchHasMix(p)) {
        r.gainDb = held(patchMix(p, kPatchMixGain), -96.0f, 12.0f, -9.0f);
        r.pan = held(patchMix(p, kPatchMixPan), -100.0f, 100.0f, 0.0f);
        r.reverbSend = held(patchMix(p, kPatchMixReverb), 0.0f, 100.0f, 0.0f);
        r.delaySend = held(patchMix(p, kPatchMixDelay), 0.0f, 100.0f, 0.0f);
    }
    return r;
}

void App::setSynthTargetRecord(int s, int n, const SynthSlotRecord& r) {
    if (s < 0 || s >= kSynthSlotCount) return;
    SynthSlotRecord own = project.synthSlot(s);
    if (!n) {
        // Only what the pages edit: the model and the name stay the record's.
        own.gainDb = r.gainDb;
        own.pan = r.pan;
        own.reverbSend = r.reverbSend;
        own.delaySend = r.delaySend;
        own.polyphony = r.polyphony;
        std::memcpy(own.macros, r.macros, sizeof(own.macros));
        setSynthSlotRecord(s, own);
        return;
    }
    if (!synthPresetUsed(n)) return;   // emptied since the read
    const SynthSlotRecord t = synthTargetRecord(s, n);
    SynthPatch& p = g_presets[n - 1];
    bool changed = false;
    if (r.gainDb != t.gainDb || r.pan != t.pan || r.reverbSend != t.reverbSend || r.delaySend != t.delaySend) {
        patchSetMix(p, r.gainDb, r.pan, r.reverbSend, r.delaySend);
        changed = true;
    }
    if (r.polyphony != t.polyphony) {
        patchSetPoly(p, r.polyphony);
        changed = true;
    }
    if (changed) markSynthPresetDirty(n);
    // The macro page values stay the slot's.
    if (std::memcmp(own.macros, r.macros, sizeof(own.macros)) != 0) {
        std::memcpy(own.macros, r.macros, sizeof(own.macros));
        setSynthSlotRecord(s, own);
    }
}

void App::markSynthTargetDirty(int s, int n) {
    if (!n) markSynthPatchDirty(s);
    else if (synthPresetUsed(n)) markSynthPresetDirty(n);
}

uint32_t App::synthPresetResets() const { return g_presetResets; }

void App::markSynthPresetDirty(int n) {
    if (!validPreset(n)) return;
    g_presetDirty = uint8_t(g_presetDirty | (1u << (n - 1)));
    unsaved_ = true;
    engine.synth().presetChanged(uint8_t(n));   // heard at the next block; the slots stay on it
}

void App::setSynthPresetPatch(int n, const SynthPatch& src, const char* name) {
    if (!synthPresetUsed(n) || src.engine == 0) return;
    if (previewPreset_ == n) endSynthPreview();
    SynthPatch& e = g_presets[n - 1];
    // What the preset carries besides its sound stays.
    const uint8_t mixCount = e.mixCount;
    PatchParam mix[kPatchMixCount];
    std::memcpy(mix, e.mix, sizeof(mix));
    const bool polyPresent = e.polyPresent;
    const uint8_t poly = e.poly;
    // A new engine: no slot reads a half-written entry. While the engine is 0
    // a slot that binds (an audio block between here and the next setEngine)
    // plays its own patch for that block; presetChanged below binds it to the
    // new entry at the next one, and the slot's preset number never changes.
    setEngine(e, 0);
    copyBehindEngine(e, src);
    e.mixCount = mixCount;
    std::memcpy(e.mix, mix, sizeof(mix));
    e.polyPresent = polyPresent;
    e.poly = poly;
    patchSetPresetName(e, name);
    setEngine(e, src.engine);
    for (int s = 0; s < kSynthSlotCount; ++s) {
        if (engine.synth().presetOf(s) != n) continue;
        SynthSlotRecord r = project.synthSlot(s);
        for (int k = 0; k < int(mt::kSynthMacroCount); ++k) r.macros[k] = audio::synthMacroDefault(e.macros[k].bipolar);
        setSynthSlotRecord(s, r);
    }
    markSynthPresetDirty(n);
}

void App::storeSynthPreset(int n, int s) {
    if (!validPreset(n) || s < 0 || s >= kSynthSlotCount) return;
    // [OT] What the slot plays is stored: a slot on preset n stores nothing.
    const int from = synthEditTarget(s);
    if (from == n) return;
    if (previewSlot_ == s || previewPreset_ == n) endSynthPreview();
    const SynthSlotRecord r = synthTargetRecord(s);
    const SynthPatch& src = from ? g_presets[from - 1] : patches_[s];
    setEngine(g_presets[n - 1], 0);   // no Z can select it from here on
    leaveSynthPreset(n);
    copyBehindEngine(g_presets[n - 1], src);
    // [OT] The slot's mix, polyphony and patch name go with the preset (the
    // slot keeps its own).
    patchSetMix(g_presets[n - 1], r.gainDb, r.pan, r.reverbSend, r.delaySend);
    patchSetPoly(g_presets[n - 1], r.polyphony);
    patchSetPresetName(g_presets[n - 1], r.patchName);
    setEngine(g_presets[n - 1], src.engine);
    g_presetDirty = uint8_t(g_presetDirty | (1u << (n - 1)));
    ++chase::mem().presetSerial;   // [OT] the stopped Z chase applies again
    unsaved_ = true;
}

void App::clearSynthPreset(int n) {
    if (!synthPresetUsed(n)) return;
    if (previewPreset_ == n) endSynthPreview();
    setEngine(g_presets[n - 1], 0);   // no Z can select it from here on
    leaveSynthPreset(n);
    patchStripPreset(g_presets[n - 1]);
    g_presetDirty = uint8_t(g_presetDirty | (1u << (n - 1)));
    ++chase::mem().presetSerial;
    unsaved_ = true;
}

bool App::loadSynthPreset(int s, int n) {
    if (s < 0 || s >= kSynthSlotCount || !synthPresetUsed(n)) return false;
    if (previewSlot_ == s) endSynthPreview();
    const SynthPatch& p = g_presets[n - 1];
    if (!audio::synthEngineFor(p.engine)) return false;   // an engine this build does not have
    if (p.engine != patches_[s].engine)
        for (int m = 0; m < kSynthModelCount; ++m)
            if (synthEngineId(SynthModel(m)) == p.engine) setSynthModel(s, m);
    if (project.synthSlot(s).model >= uint32_t(kSynthModelCount) ||
        synthEngineId(SynthModel(project.synthSlot(s).model)) != p.engine)
        return false;
    std::memcpy(&patches_[s], &p, sizeof(SynthPatch));
    patchStripPreset(patches_[s]);   // [OT] the slot's own patch never carries a mix, name or polyphony
    SynthSlotRecord r = project.synthSlot(s);
    // [OT] The preset's stored name, else "Preset n".
    if (p.presetName[0] && std::strcmp(p.presetName, "Init") != 0)
        std::snprintf(r.patchName, sizeof(r.patchName), "%s", p.presetName);
    else
        std::snprintf(r.patchName, sizeof(r.patchName), "Preset %d", n);
    // [OT] The preset's mix and polyphony become the slot's; a value the
    // record cannot hold is held at its limit.
    if (patchHasMix(p)) {
        r.gainDb = held(patchMix(p, kPatchMixGain), -96.0f, 12.0f, -9.0f);
        r.pan = held(patchMix(p, kPatchMixPan), -100.0f, 100.0f, 0.0f);
        r.reverbSend = held(patchMix(p, kPatchMixReverb), 0.0f, 100.0f, 0.0f);
        r.delaySend = held(patchMix(p, kPatchMixDelay), 0.0f, 100.0f, 0.0f);
    }
    if (patchHasPoly(p)) r.polyphony = p.poly;
    for (int k = 0; k < int(mt::kSynthMacroCount); ++k) r.macros[k] = audio::synthMacroDefault(p.macros[k].bipolar);
    setSynthSlotRecord(s, r);
    markSynthPatchDirty(s);   // also ends a running Z on this slot
    return true;
}

void App::resetSynthPresets() {
    // [OT] A Load Patch preview of the old project is forgotten (in a preset
    // or a slot): ending it later would put the old sound into the new one.
    previewSlot_ = -1;
    previewPreset_ = 0;
    for (SynthPatch& p : g_presets) setEngine(p, 0);
    for (int s = 0; s < kSynthSlotCount; ++s)
        if (engine.synth().presetOf(s)) engine.synth().patchChanged(s);
    std::memset(g_presets, 0, sizeof(g_presets));
    g_presetDirty = 0;
    ++g_presetResets;
    ++chase::mem().presetSerial;
}

}  // namespace ot
