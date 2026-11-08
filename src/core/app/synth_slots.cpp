// Synth slot operations for the synth screens (2.x WP-G, docs/MINI_PLAN.md
// §3 UI; spec 21 §2 library layout, §3.3 records, §7 patches) [OT].
//
// The library is Patches/<MODEL>/<name>.patch on the card (spec 21 §2). The
// slot's patch lives in App::patches_; every change marks it dirty (the
// autosave rewrites Workspace/N.patch) and reaches the audio side through
// SynthBank::patchChanged (markSynthPatchDirty). Records go through the
// project bytes and the engine sync (markProjectDirty -> setSlotRecord).
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "app/synth_presets.h"
#include "modules/synth_models.h"
#include "audio/synth_params.h"
#include "formats/file_io.h"
#include "formats/patch_pb.h"
#include "sdram_bss.h"

namespace ot {

namespace {
// Scratch layout (same split as the project job's patch unit): the file at
// 0, the decoded patch at 0x8000.
constexpr uint32_t kFileOff = 0;
constexpr uint32_t kPatchTmpOff = 0x8000;
static_assert(kPatchMaxFileSize + 1 <= kPatchTmpOff, "patch file fits below the decode slot");

void setName(SynthSlotRecord& r, const char* name) {
    std::snprintf(r.patchName, sizeof(r.patchName), "%s", name ? name : "");
}

// Macro page values M1..M6 -> the patch defaults (63 bipolar, else 0).
void resetMacros(SynthSlotRecord& r, const SynthPatch& p) {
    for (int k = 0; k < int(mt::kSynthMacroCount); ++k) r.macros[k] = audio::synthMacroDefault(p.macros[k].bipolar);
}

bool libraryPath(char* out, size_t cap, int32_t engineId, const char* name) {
    if (!name || !name[0] || std::strchr(name, '/')) return false;
    std::snprintf(out, cap, "Patches/%s/%s.patch", synthEngineFolder(engineId), name);
    return true;
}

// The slot's patch while a Load Patch preview plays another one (one slot
// at a time; main loop only).
OT_SDRAM_BSS SynthPatch g_previewSaved;
OT_SDRAM_BSS uint8_t g_previewPreset;   // [3.x] 1..8: the preview plays in that preset's entry
}  // namespace

SynthPatch* App::scratchPatch() {
    if (!mem_.scratch || mem_.scratchBytes < kPatchTmpOff + sizeof(SynthPatch)) return nullptr;
    if (jobBusy()) finishJob();   // the job shares the scratch (spec 05 appendix E3)
    return reinterpret_cast<SynthPatch*>(mem_.scratch + kPatchTmpOff);
}

// Patches/<engine>/<name>.patch decoded into the scratch decode slot; nullptr
// when missing, damaged or of another engine.
const SynthPatch* App::readSynthLibraryPatch(int32_t eng, const char* name) {
    SynthPatch* tmp = scratchPatch();
    char path[96];
    if (!tmp || !libraryPath(path, sizeof(path), eng, name)) return nullptr;
    uint8_t* buf = mem_.scratch + kFileOff;
    uint32_t got = 0;
    if (readWholeFile(fs(), path, buf, kPatchMaxFileSize + 1, &got) != FmtStatus::Ok || got > kPatchMaxFileSize)
        return nullptr;
    if (decodePatch(buf, got, tmp) != PatchStatus::kOk || tmp->engine != eng) return nullptr;
    return tmp;
}

bool App::previewSynthLibraryPatch(int s, const char* name) {
    if (s < 0 || s >= kSynthSlotCount) return false;
    const uint8_t n = synthEditTarget(s);
    const SynthPatch* p = readSynthLibraryPatch(synthTargetEngine(s), name);
    if (!p) return false;
    if (previewSlot_ != s || g_previewPreset != n) {
        endSynthPreview();
        std::memcpy(&g_previewSaved, &synthTargetPatch(s), sizeof(SynthPatch));
        if (n) {   // [3.x] the preset's entry: its dirty bit is kept aside the same way
            uint8_t& d = presets::dirty();
            previewDirty_ = ((d >> (n - 1)) & 1u) != 0;
            d = uint8_t(d & ~(1u << (n - 1)));
        } else {
            previewSrc_ = patchSrc_[s];
            previewDirty_ = patchDirty_[s];
            patchDirty_[s] = false;   // the autosave never writes a previewed patch
        }
        previewSlot_ = int8_t(s);
        g_previewPreset = n;
    }
    // The same engine (readSynthLibraryPatch): the entry's engine word does not change.
    std::memcpy(&synthTargetPatch(s), p, sizeof(SynthPatch));
    if (n) {
        engine.synth().presetChanged(n);   // the slot stays on the preset
        return true;
    }
    SynthSlotRecord r = project.synthSlot(s);
    resetMacros(r, patches_[s]);
    engine.synth().setSlotRecord(s, r);   // audio side only; the project record is untouched
    engine.synth().patchChanged(s);
    return true;
}

void App::endSynthPreview() {
    const int s = previewSlot_;
    if (s < 0 || s >= kSynthSlotCount) return;
    previewSlot_ = -1;
    if (const uint8_t n = g_previewPreset) {   // [3.x] the preset's entry, byte for byte
        g_previewPreset = 0;
        std::memcpy(&presets::table()[n - 1], &g_previewSaved, sizeof(SynthPatch));
        if (previewDirty_) presets::dirty() = uint8_t(presets::dirty() | (1u << (n - 1)));
        engine.synth().presetChanged(n);
        return;
    }
    std::memcpy(&patches_[s], &g_previewSaved, sizeof(SynthPatch));
    patchSrc_[s] = previewSrc_;
    patchDirty_[s] = previewDirty_;
    engine.synth().setSlotRecord(s, project.synthSlot(s));
    engine.synth().patchChanged(s);
    chaseReapply(s);   // [3.x] stopped: back on the preset the chase has it on (a load after this is heard)
}

void App::setSynthSlotRecord(int s, const SynthSlotRecord& r) {
    if (s < 0 || s >= kSynthSlotCount) return;
    project.setSynthSlot(s, r);
    const SynthSlotRecord now = project.synthSlot(s);
    engine.synth().setSlotRecord(s, now);   // main-loop setter: applies at the next block
    markProjectDirty();
}

void App::setSynthEngine(int s, int32_t engineId) {
    if (s < 0 || s >= kSynthSlotCount) return;
    if (engineId < 0x80) {
        for (int m = 0; m < kSynthModelCount; ++m)
            if (synthEngineId(SynthModel(m)) == engineId) setSynthModel(s, m);
        return;
    }
    if (previewSlot_ == s) endSynthPreview();
    // A module engine: the record keeps its Mini model (the fallback a Mini
    // or an older OpenTracker plays); the sidecar names the engine.
    setSlotEngine(s, engineId);
    SynthSlotRecord r = project.synthSlot(s);
    setName(r, "Init");
    project.setSynthSlot(s, r);
    if (!loadSynthLibraryPatch(s, "Init")) initSynthPatch(s);
}

void App::setSynthModel(int s, int model) {
    if (s < 0 || s >= kSynthSlotCount || model < 0 || model >= kSynthModelCount) return;
    if (previewSlot_ == s) endSynthPreview();
    setSlotEngine(s, 0);
    SynthSlotRecord r = project.synthSlot(s);
    r.model = uint32_t(model);
    setName(r, "Init");
    project.setSynthSlot(s, r);
    if (!loadSynthLibraryPatch(s, "Init")) initSynthPatch(s);
}

void App::initSynthPatch(int s) {
    if (s < 0 || s >= kSynthSlotCount) return;
    if (previewSlot_ == s) endSynthPreview();
    SynthSlotRecord r = project.synthSlot(s);
    patchInit(slotEngineId(s), &patches_[s]);
    patchSrc_[s] = PatchSource::Init;
    setName(r, "Init");
    resetMacros(r, patches_[s]);
    setSynthSlotRecord(s, r);
    markSynthPatchDirty(s);
}

bool App::loadSynthLibraryPatch(int s, const char* name) {
    if (s < 0 || s >= kSynthSlotCount) return false;
    if (previewSlot_ == s) endSynthPreview();
    const SynthPatch* tmp = readSynthLibraryPatch(slotEngineId(s), name);
    if (!tmp) return false;
    SynthSlotRecord r = project.synthSlot(s);
    patches_[s] = *tmp;
    patchSrc_[s] = PatchSource::Library;
    setName(r, name);
    resetMacros(r, patches_[s]);
    setSynthSlotRecord(s, r);
    markSynthPatchDirty(s);
    return true;
}

// [3.x] The edit target's patch operations (screen_synth.cpp): on a preset
// they replace the preset's entry, the slot's own otherwise.
void App::setSynthTargetEngine(int s, int32_t engineId) {
    const uint8_t n = synthEditTarget(s);
    if (!n) {
        setSynthEngine(s, engineId);
        return;
    }
    if (previewSlot_ >= 0) endSynthPreview();
    const SynthPatch* p = readSynthLibraryPatch(engineId, "Init");
    if (!p) {
        SynthPatch* t = scratchPatch();
        if (!t || !patchInit(engineId, t)) return;
        p = t;
    }
    setSynthPresetPatch(n, *p, "Init");
}

bool App::loadSynthTargetPatch(int s, const char* name) {
    const uint8_t n = synthEditTarget(s);
    if (!n) return loadSynthLibraryPatch(s, name);
    if (previewSlot_ >= 0) endSynthPreview();
    const SynthPatch* p = readSynthLibraryPatch(synthTargetEngine(s), name);
    if (!p) return false;
    setSynthPresetPatch(n, *p, name);
    return true;
}

void App::initSynthTargetPatch(int s) {
    const uint8_t n = synthEditTarget(s);
    if (!n) {
        initSynthPatch(s);
        return;
    }
    if (previewSlot_ >= 0) endSynthPreview();
    SynthPatch* t = scratchPatch();
    if (t && patchInit(synthTargetEngine(s), t)) setSynthPresetPatch(n, *t, "Init");
}

bool App::synthUnavailableNotice(char* out, size_t cap) const {
    for (int s = 0; s < kSynthSlotCount; ++s) {
        const int32_t eng = slotEngineId(s);
        if (eng && audio::synthEngineFor(eng)) continue;
        std::snprintf(out, cap, "Synth %d (%s) not available", s + 1, synthEngineDisplayName(eng));
        return true;
    }
    return false;
}

bool App::saveSynthLibraryPatch(int s, const char* name) {
    if (s < 0 || s >= kSynthSlotCount || !mem_.scratch || mem_.scratchBytes < kPatchMaxFileSize) return false;
    if (jobBusy()) finishJob();
    SynthSlotRecord r = project.synthSlot(s);
    const int32_t eng = synthTargetEngine(s);   // [3.x] the edit target's
    char path[96], dir[48], tmpPath[104];
    if (!libraryPath(path, sizeof(path), eng, name)) return false;
    uint8_t* out = mem_.scratch + kFileOff;
    size_t n = 0;
    if (encodePatch(synthTargetPatch(s), out, kPatchMaxFileSize, &n) != PatchStatus::kOk) return false;
    IStorage& f = fs();
    std::snprintf(dir, sizeof(dir), "Patches/%s", synthEngineFolder(eng));
    if (!f.mkdir("Patches") || !f.mkdir(dir)) return false;
    std::snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
    if (writeWholeFile(f, tmpPath, out, uint32_t(n)) != FmtStatus::Ok) {
        f.remove(tmpPath);
        return false;
    }
    if (f.exists(path) && !f.remove(path)) return false;
    if (!f.rename(tmpPath, path)) return false;
    if (const uint8_t p = synthEditTarget(s)) {
        setSynthPresetName(p, name);
        return true;
    }
    setName(r, name);
    setSynthSlotRecord(s, r);
    return true;
}

}  // namespace ot
