// The project's synth presets (see synth_presets.h); their files are handled
// by the project jobs in project_io.cpp.
#include "app/synth_presets.h"

#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "audio/synth_params.h"
#include "sdram_bss.h"

namespace ot {

namespace {
OT_SDRAM_BSS SynthPatch g_presets[kSynthPresetCount];
OT_SDRAM_BSS uint8_t g_presetDirty;
bool validPreset(int n) { return n >= 1 && n <= kSynthPresetCount; }
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
    leaveSynthPreset(n);
    std::memcpy(&g_presets[n - 1], &patches_[s], sizeof(SynthPatch));
    g_presetDirty = uint8_t(g_presetDirty | (1u << (n - 1)));
    unsaved_ = true;
}

void App::clearSynthPreset(int n) {
    if (!synthPresetUsed(n)) return;
    leaveSynthPreset(n);
    g_presets[n - 1].engine = 0;
    g_presetDirty = uint8_t(g_presetDirty | (1u << (n - 1)));
    unsaved_ = true;
}

bool App::loadSynthPreset(int s, int n) {
    if (s < 0 || s >= kSynthSlotCount || !synthPresetUsed(n)) return false;
    if (previewSlot_ == s) endSynthPreview();
    const SynthPatch& p = g_presets[n - 1];
    if (p.engine != slotEngineId(s)) setSynthEngine(s, p.engine);
    if (slotEngineId(s) != p.engine) return false;   // an engine this build does not have
    std::memcpy(&patches_[s], &p, sizeof(SynthPatch));
    SynthSlotRecord r = project.synthSlot(s);
    std::snprintf(r.patchName, sizeof(r.patchName), "Preset %d", n);
    for (int k = 0; k < int(mt::kSynthMacroCount); ++k) r.macros[k] = audio::synthMacroDefault(p.macros[k].bipolar);
    setSynthSlotRecord(s, r);
    markSynthPatchDirty(s);   // also ends a running Z on this slot
    return true;
}

void App::resetSynthPresets() {
    for (int s = 0; s < kSynthSlotCount; ++s)
        if (engine.synth().presetOf(s)) engine.synth().patchChanged(s);
    std::memset(g_presets, 0, sizeof(g_presets));
    g_presetDirty = 0;
}

}  // namespace ot
