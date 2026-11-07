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
void copyBehindEngine(SynthPatch& to, const SynthPatch& from) {
    static_assert(offsetof(SynthPatch, engine) == 0, "the engine leads the patch");
    std::memcpy(reinterpret_cast<uint8_t*>(&to) + sizeof(to.engine),
                reinterpret_cast<const uint8_t*>(&from) + sizeof(from.engine), sizeof(SynthPatch) - sizeof(to.engine));
}
}  // namespace

namespace presets {
SynthPatch* table() { return g_presets; }
uint8_t& dirty() { return g_presetDirty; }
}  // namespace presets

const SynthPatch& App::synthPreset(int n) const { return g_presets[validPreset(n) ? n - 1 : 0]; }
bool App::synthPresetUsed(int n) const { return validPreset(n) && g_presets[n - 1].engine != 0; }
bool App::synthPresetDirty() const { return g_presetDirty != 0; }
int App::synthPresetActive(int s) const { return engine.synth().presetOf(s); }

// A slot on preset n goes back to its patch before the entry changes.
void App::leaveSynthPreset(int n) {
    for (int s = 0; s < kSynthSlotCount; ++s)
        if (engine.synth().presetOf(s) == n) engine.synth().patchChanged(s);
}

void App::storeSynthPreset(int n, int s) {
    if (!validPreset(n) || s < 0 || s >= kSynthSlotCount) return;
    if (previewSlot_ == s) endSynthPreview();
    setEngine(g_presets[n - 1], 0);   // no Z can select it from here on
    leaveSynthPreset(n);
    copyBehindEngine(g_presets[n - 1], patches_[s]);
    // [OT] The slot's mix goes with the preset (the slot keeps its own).
    const SynthSlotRecord r = project.synthSlot(s);
    patchSetMix(g_presets[n - 1], r.gainDb, r.pan, r.reverbSend, r.delaySend);
    setEngine(g_presets[n - 1], patches_[s].engine);
    g_presetDirty = uint8_t(g_presetDirty | (1u << (n - 1)));
    ++chase::mem().presetSerial;   // [OT] the stopped Z chase applies again
    unsaved_ = true;
}

void App::clearSynthPreset(int n) {
    if (!synthPresetUsed(n)) return;
    setEngine(g_presets[n - 1], 0);   // no Z can select it from here on
    leaveSynthPreset(n);
    g_presets[n - 1].mixCount = 0;
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
    patches_[s].mixCount = 0;   // [OT] the slot's own patch never carries a mix
    SynthSlotRecord r = project.synthSlot(s);
    std::snprintf(r.patchName, sizeof(r.patchName), "Preset %d", n);
    if (patchHasMix(p)) {
        // [OT] The preset's mix becomes the slot's; a value the record cannot
        // hold is held at its limit, one that is not a number keeps the slot's.
        auto take = [](float v, float lo, float hi, float& out) {
            if (std::isfinite(v)) out = v < lo ? lo : (v > hi ? hi : v);
        };
        take(patchMix(p, kPatchMixGain), -96.0f, 12.0f, r.gainDb);
        take(patchMix(p, kPatchMixPan), -100.0f, 100.0f, r.pan);
        take(patchMix(p, kPatchMixReverb), 0.0f, 100.0f, r.reverbSend);
        take(patchMix(p, kPatchMixDelay), 0.0f, 100.0f, r.delaySend);
    }
    for (int k = 0; k < int(mt::kSynthMacroCount); ++k) r.macros[k] = audio::synthMacroDefault(p.macros[k].bipolar);
    setSynthSlotRecord(s, r);
    markSynthPatchDirty(s);   // also ends a running Z on this slot
    return true;
}

void App::resetSynthPresets() {
    for (SynthPatch& p : g_presets) setEngine(p, 0);
    for (int s = 0; s < kSynthSlotCount; ++s)
        if (engine.synth().presetOf(s)) engine.synth().patchChanged(s);
    std::memset(g_presets, 0, sizeof(g_presets));
    g_presetDirty = 0;
    ++chase::mem().presetSerial;
}

}  // namespace ot
