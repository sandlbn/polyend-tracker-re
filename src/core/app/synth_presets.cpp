// The project's synth presets (see synth_presets.h); their files are handled
// by the project jobs in project_io.cpp.
#include "app/synth_presets.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "audio/synth_engine.h"
#include "audio/synth_params.h"
#include "sdram_bss.h"

namespace ot {

namespace {
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
SynthPatch g_hostPresets[kSynthPresetCount];
OT_SDRAM_BSS SynthPatch* g_table = g_hostPresets;
#else
OT_SDRAM_BSS SynthPatch* g_table;
#endif
OT_SDRAM_BSS uint8_t g_presetDirty;
#if OT_SLOT_INSERTS
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
audio::PresetInsert g_hostInserts[kSynthPresetCount];
OT_SDRAM_BSS audio::PresetInsert* g_ins = g_hostInserts;
#else
OT_SDRAM_BSS audio::PresetInsert* g_ins;
#endif
#endif
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
void bind(SynthPatch* storage) { g_table = storage; }
SynthPatch* table() { return g_table; }
uint8_t& dirty() { return g_presetDirty; }
#if OT_SLOT_INSERTS
void bindInserts(audio::PresetInsert* storage) { g_ins = storage; }
audio::PresetInsert* insertTable() { return g_ins; }
#endif
}  // namespace presets

const SynthPatch& App::synthPreset(int n) const { return g_table[validPreset(n) ? n - 1 : 0]; }
bool App::synthPresetUsed(int n) const { return validPreset(n) && g_table[n - 1].engine != 0; }
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
    setEngine(g_table[n - 1], 0);   // no Z can select it from here on
    leaveSynthPreset(n);
#if OT_SLOT_INSERTS
    // [3.x] The slot's insert goes with the patch (none: the preset removes it).
    audio::PresetInsert& pi = g_ins[n - 1];
    pi = audio::PresetInsert{};
    if (const char* id = slotInsertId(s)) {
        std::memcpy(pi.id, id, 4);
        pi.fx = slotInsertEffect(s);
        pi.set = slotInsert_[s].set;
        if (slotInsert_[s].off) pi.flags = audio::kPresetInsertOff;
    }
    pi.flags = uint8_t(pi.flags | audio::kPresetInsertHas);
    sidecarDirty_ = true;
#endif
    copyBehindEngine(g_table[n - 1], patches_[s]);
    setEngine(g_table[n - 1], patches_[s].engine);
    g_presetDirty = uint8_t(g_presetDirty | (1u << (n - 1)));
    unsaved_ = true;
    chaseInvalidateAll();   // a Z of an empty preset did nothing; now it selects
    chaseForget();          // and a slot on preset n went to its own patch
}

void App::clearSynthPreset(int n) {
    if (!synthPresetUsed(n)) return;
    setEngine(g_table[n - 1], 0);   // no Z can select it from here on
    leaveSynthPreset(n);
#if OT_SLOT_INSERTS
    g_ins[n - 1] = audio::PresetInsert{};
    sidecarDirty_ = true;
#endif
    g_presetDirty = uint8_t(g_presetDirty | (1u << (n - 1)));
    unsaved_ = true;
    chaseInvalidateAll();
    chaseForget();
}

bool App::loadSynthPreset(int s, int n) {
    if (s < 0 || s >= kSynthSlotCount || !synthPresetUsed(n)) return false;
    if (previewSlot_ == s) endSynthPreview();
    const SynthPatch& p = g_table[n - 1];
    if (!audio::synthEngineFor(p.engine)) return false;   // an engine this build does not have
    if (p.engine != slotEngineId(s)) setSynthEngine(s, p.engine);
    if (slotEngineId(s) != p.engine) return false;
    std::memcpy(&patches_[s], &p, sizeof(SynthPatch));
    SynthSlotRecord r = project.synthSlot(s);
    std::snprintf(r.patchName, sizeof(r.patchName), "Preset %d", n);
    for (int k = 0; k < int(mt::kSynthMacroCount); ++k) r.macros[k] = audio::synthMacroDefault(p.macros[k].bipolar);
    setSynthSlotRecord(s, r);
    markSynthPatchDirty(s);   // also ends a running Z on this slot
#if OT_SLOT_INSERTS
    // [3.x] The preset's insert becomes the slot's (one saved before presets
    // had inserts leaves it as it is).
    const audio::PresetInsert& pi = g_ins[n - 1];
    if (pi.flags & audio::kPresetInsertHas) setSlotInsert(s, pi.id, pi.set, (pi.flags & audio::kPresetInsertOff) != 0);
#endif
    return true;
}

void App::resetSynthPresets() {
    for (int n = 0; n < kSynthPresetCount; ++n) setEngine(g_table[n], 0);
    for (int s = 0; s < kSynthSlotCount; ++s)
        if (engine.synth().presetOf(s)) engine.synth().patchChanged(s);
    std::memset(g_table, 0, sizeof(SynthPatch) * kSynthPresetCount);
#if OT_SLOT_INSERTS
    std::memset(static_cast<void*>(g_ins), 0, sizeof(audio::PresetInsert) * kSynthPresetCount);
#endif
    g_presetDirty = 0;
    chaseInvalidateAll();
    chaseForget();
}

}  // namespace ot
