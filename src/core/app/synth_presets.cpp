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
OT_SDRAM_BSS uint32_t g_presetEpoch;   // [3.x] App::synthPresetsEpoch
#if OT_SLOT_INSERTS
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
audio::PresetInsert g_hostInserts[kSynthPresetCount];
OT_SDRAM_BSS audio::PresetInsert* g_ins = g_hostInserts;
#else
OT_SDRAM_BSS audio::PresetInsert* g_ins;
#endif
#endif
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
audio::PresetMix g_hostMix[kSynthPresetCount];
OT_SDRAM_BSS audio::PresetMix* g_mix = g_hostMix;
#else
OT_SDRAM_BSS audio::PresetMix* g_mix;
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
void bindMix(audio::PresetMix* storage) { g_mix = storage; }
audio::PresetMix* mixTable() { return g_mix; }
}  // namespace presets

const SynthPatch& App::synthPreset(int n) const { return g_table[validPreset(n) ? n - 1 : 0]; }
#if OT_SLOT_INSERTS
const audio::PresetInsert& App::synthPresetInsert(int n) const { return g_ins[validPreset(n) ? n - 1 : 0]; }
#endif
const audio::PresetMix& App::synthPresetMix(int n) const { return g_mix[validPreset(n) ? n - 1 : 0]; }
bool App::synthPresetUsed(int n) const { return validPreset(n) && g_table[n - 1].engine != 0; }
bool App::synthPresetDirty() const { return g_presetDirty != 0; }
int App::synthPresetActive(int s) const { return engine.synth().presetOf(s); }

// --- [3.x] what the Synth pages edit ------------------------------------------------

uint8_t App::synthEditTarget(int s) const {
    if (s < 0 || s >= kSynthSlotCount) return 0;
    const uint8_t n = engine.synth().presetOf(s);
    return synthPresetUsed(n) ? n : 0;
}

SynthPatch& App::synthTargetPatch(int s, uint8_t t) {
    const uint8_t n = synthTargetOf(s, t);
    return n ? g_table[n - 1] : synthPatch(s);
}

const SynthPatch& App::synthTargetPatch(int s, uint8_t t) const {
    const uint8_t n = synthTargetOf(s, t);
    return n ? g_table[n - 1] : synthPatch(s);
}

int32_t App::synthTargetEngine(int s, uint8_t t) const {
    const uint8_t n = synthTargetOf(s, t);
    return n ? g_table[n - 1].engine : slotEngineId(s);
}

namespace {
bool sameMix(const SynthSlotRecord& a, const SynthSlotRecord& b) {
    return std::memcmp(&a.gainDb, &b.gainDb, sizeof a.gainDb) == 0 && std::memcmp(&a.pan, &b.pan, sizeof a.pan) == 0 &&
           std::memcmp(&a.reverbSend, &b.reverbSend, sizeof a.reverbSend) == 0 &&
           std::memcmp(&a.delaySend, &b.delaySend, sizeof a.delaySend) == 0 && a.polyphony == b.polyphony;
}
// The record's mix fields, polyphony and macro values from `from` (nothing
// else: the model and name of a record shaped by a preset stay the slot's).
void copyMix(SynthSlotRecord& to, const SynthSlotRecord& from) {
    to.gainDb = from.gainDb;
    to.pan = from.pan;
    to.reverbSend = from.reverbSend;
    to.delaySend = from.delaySend;
    to.polyphony = from.polyphony;
}
// A preset mix with the record's ranges (spec 21 §3.3); NaN never reaches a page.
bool usableMix(const audio::PresetMix& pm) {
    SynthSlotRecord chk;
    chk.gainDb = pm.gainDb;
    chk.pan = pm.pan;
    chk.reverbSend = pm.reverbSend;
    chk.delaySend = pm.delaySend;
    return chk.valid();
}
}  // namespace

SynthSlotRecord App::synthTargetRecord(int s, uint8_t t) const {
    SynthSlotRecord r = project.synthSlot(s);
    if (const uint8_t n = synthTargetOf(s, t)) {
        const audio::PresetMix& pm = g_mix[n - 1];
        if ((pm.flags & audio::kPresetMixHas) && usableMix(pm)) {
            r.gainDb = pm.gainDb;
            r.pan = pm.pan;
            r.reverbSend = pm.reverbSend;
            r.delaySend = pm.delaySend;
        }
        if (pm.flags & audio::kPresetMixPoly) r.polyphony = pm.polyphony > 8 ? 8u : pm.polyphony;
        std::snprintf(r.patchName, sizeof(r.patchName), "%s", pm.name);
        for (int k = 0; k < int(mt::kSynthMacroCount) && k < kPatchMacros; ++k)
            r.macros[k] = audio::synthMacroOnPreset(r.macros[k], patches_[s].macros[k].bipolar, g_table[n - 1].macros[k].bipolar);
    }
    return r;
}

void App::setSynthTargetRecord(int s, const SynthSlotRecord& r, uint8_t t) {
    if (s < 0 || s >= kSynthSlotCount) return;
    const uint8_t n = synthTargetOf(s, t);
    const SynthSlotRecord own = project.synthSlot(s);
    if (n && !sameMix(r, synthTargetRecord(s, n))) {
        // The values before the flags: the audio block may read the entry any time.
        audio::PresetMix& pm = g_mix[n - 1];
        pm.gainDb = r.gainDb;
        pm.pan = r.pan;
        pm.reverbSend = r.reverbSend;
        pm.delaySend = r.delaySend;
        pm.polyphony = uint8_t(r.polyphony > 8 ? 8 : r.polyphony);
        std::atomic_signal_fence(std::memory_order_seq_cst);
        pm.flags = uint8_t(pm.flags | audio::kPresetMixHas | audio::kPresetMixPoly);
        sidecarDirty_ = true;
        unsaved_ = true;
        engine.synth().presetChanged(n);
    }
    // The record: the macro values always; the mix and polyphony when it is the target.
    SynthSlotRecord w = own;
    if (n) {
        // Only what moved from the values shown; a step onto the value that
        // means "neutral" on this preset goes one further, so it can pass.
        const SynthSlotRecord shown = synthTargetRecord(s, n);
        for (int k = 0; k < int(mt::kSynthMacroCount) && k < kPatchMacros; ++k) {
            if (r.macros[k] == shown.macros[k]) continue;
            int v = r.macros[k];
            const int ownNeutral = audio::synthMacroDefault(patches_[s].macros[k].bipolar);
            if (v == ownNeutral && ownNeutral != audio::synthMacroDefault(g_table[n - 1].macros[k].bipolar)) {
                v += v > int(shown.macros[k]) ? 1 : -1;
                if (v < 0 || v > 127) continue;
            }
            w.macros[k] = uint8_t(v);
        }
    } else {
        std::memcpy(w.macros, r.macros, sizeof w.macros);
        copyMix(w, r);
    }
    if (std::memcmp(&w, &own, sizeof w) != 0) setSynthSlotRecord(s, w);
}

void App::markSynthPresetDirty(int n) {
    if (!validPreset(n)) return;
    g_presetDirty = uint8_t(g_presetDirty | (1u << (n - 1)));
    unsaved_ = true;
    engine.synth().presetChanged(uint8_t(n));
}

void App::markSynthTargetDirty(int s, uint8_t t) {
    if (const uint8_t n = synthTargetOf(s, t)) markSynthPresetDirty(n);
    else markSynthPatchDirty(s);
}

uint32_t App::synthPresetsEpoch() const { return g_presetEpoch; }

const char* App::synthPresetName(int n) const { return validPreset(n) ? g_mix[n - 1].name : ""; }

void App::setSynthPresetName(int n, const char* name) {
    if (!validPreset(n)) return;
    char* to = g_mix[n - 1].name;
    char t[sizeof(audio::PresetMix::name)];
    std::snprintf(t, sizeof t, "%s", name ? name : "");
    if (std::strcmp(t, to) == 0) return;
    std::memcpy(to, t, sizeof t);
    sidecarDirty_ = true;
    unsaved_ = true;
}

void App::setSynthPresetPatch(int n, const SynthPatch& src, const char* name) {
    if (!validPreset(n) || &src == &g_table[n - 1]) return;
    if (previewSlot_ >= 0) endSynthPreview();
    SynthPatch& e = g_table[n - 1];
    // Engine 0 while the entry is written: an audio block that binds a slot
    // on preset n in this window (a Z, a preset instrument's note) plays
    // the slot's own patch for that one block, and a preset instrument's
    // note is silent; presetChanged below binds the new patch at the next.
    setEngine(e, 0);
    copyBehindEngine(e, src);
    setEngine(e, src.engine);
    setSynthPresetName(n, name);
    for (int s = 0; s < kSynthSlotCount; ++s) {
        if (engine.synth().presetOf(s) != n) continue;
        SynthSlotRecord r = project.synthSlot(s);
        for (int k = 0; k < int(mt::kSynthMacroCount); ++k) r.macros[k] = audio::synthMacroDefault(e.macros[k].bipolar);
        setSynthSlotRecord(s, r);
    }
    markSynthPresetDirty(n);
}

// A slot on preset n goes back to its patch before the entry changes.
void App::leaveSynthPreset(int n) {
    for (int s = 0; s < kSynthSlotCount; ++s)
        if (engine.synth().presetOf(s) == n) engine.synth().patchChanged(s);
}

void App::storeSynthPreset(int n, int s, uint8_t t) {
    if (!validPreset(n) || s < 0 || s >= kSynthSlotCount) return;
    // [3.x] What the slot plays: its own sound, or the preset it is on.
    const uint8_t from = synthTargetOf(s, t);
    if (from == n) return;   // already there
    if (previewSlot_ >= 0) endSynthPreview();
    const SynthPatch& src = from ? g_table[from - 1] : patches_[s];
    const SynthSlotRecord tr = synthTargetRecord(s, from);
#if OT_SLOT_INSERTS
    // The insert goes with the patch (none: the preset removes it), read
    // before the entry changes.
    audio::PresetInsert pi{};
    if (const char* id = synthTargetInsertId(s, from)) {
        std::memcpy(pi.id, id, 4);
        pi.fx = synthTargetInsertEffect(s, from);
        pi.set = synthTargetInsertSettings(s, from);
        if (synthTargetInsertOff(s, from)) pi.flags = audio::kPresetInsertOff;
    }
    pi.flags = uint8_t(pi.flags | audio::kPresetInsertHas);
#endif
    setEngine(g_table[n - 1], 0);   // no Z can select it from here on
    leaveSynthPreset(n);
#if OT_SLOT_INSERTS
    g_ins[n - 1] = pi;
#endif
    // [3.x] And the mix (gain, pan, sends), the polyphony and the name.
    audio::PresetMix& pm = g_mix[n - 1];
    pm = audio::PresetMix{};
    pm.gainDb = tr.gainDb;
    pm.pan = tr.pan;
    pm.reverbSend = tr.reverbSend;
    pm.delaySend = tr.delaySend;
    pm.polyphony = uint8_t(tr.polyphony > 8 ? 8 : tr.polyphony);
    std::snprintf(pm.name, sizeof pm.name, "%s", tr.patchName);
    pm.flags = audio::kPresetMixHas | audio::kPresetMixPoly;
    sidecarDirty_ = true;
    copyBehindEngine(g_table[n - 1], src);
    setEngine(g_table[n - 1], src.engine);
    g_presetDirty = uint8_t(g_presetDirty | (1u << (n - 1)));
    unsaved_ = true;
    chaseInvalidateAll();   // a Z of an empty preset did nothing; now it selects
    chaseForget();          // and a slot on preset n went to its own patch
}

void App::clearSynthPreset(int n) {
    if (!synthPresetUsed(n)) return;
    if (previewSlot_ >= 0) endSynthPreview();
    setEngine(g_table[n - 1], 0);   // no Z can select it from here on
    leaveSynthPreset(n);
#if OT_SLOT_INSERTS
    g_ins[n - 1] = audio::PresetInsert{};
#endif
    g_mix[n - 1] = audio::PresetMix{};
    sidecarDirty_ = true;
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
    // [3.x] The preset's stored name, else "Preset n".
    const char* pn = g_mix[n - 1].name;
    if (pn[0] && std::strcmp(pn, "Init") != 0) std::snprintf(r.patchName, sizeof(r.patchName), "%s", pn);
    else std::snprintf(r.patchName, sizeof(r.patchName), "Preset %d", n);
    for (int k = 0; k < int(mt::kSynthMacroCount); ++k) r.macros[k] = audio::synthMacroDefault(p.macros[k].bipolar);
    // [3.x] The preset's mix becomes the slot's (one saved before presets
    // had a mix leaves it as it is), its polyphony too.
    const audio::PresetMix& pm = g_mix[n - 1];
    if ((pm.flags & audio::kPresetMixHas) && usableMix(pm)) {
        r.gainDb = pm.gainDb;
        r.pan = pm.pan;
        r.reverbSend = pm.reverbSend;
        r.delaySend = pm.delaySend;
    }
    if (pm.flags & audio::kPresetMixPoly) r.polyphony = pm.polyphony > 8 ? 8u : pm.polyphony;   // [3.x] and its polyphony
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
    dropSynthPreview();   // [3.x] a preview of the old project has nothing left to put back
    for (int n = 0; n < kSynthPresetCount; ++n) setEngine(g_table[n], 0);
    for (int s = 0; s < kSynthSlotCount; ++s)
        if (engine.synth().presetOf(s)) engine.synth().patchChanged(s);
    std::memset(g_table, 0, sizeof(SynthPatch) * kSynthPresetCount);
#if OT_SLOT_INSERTS
    std::memset(static_cast<void*>(g_ins), 0, sizeof(audio::PresetInsert) * kSynthPresetCount);
#endif
    std::memset(static_cast<void*>(g_mix), 0, sizeof(audio::PresetMix) * kSynthPresetCount);
    g_presetDirty = 0;
    ++g_presetEpoch;
    chaseInvalidateAll();
    chaseForget();
}

}  // namespace ot
