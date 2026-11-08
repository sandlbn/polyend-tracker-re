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
}  // namespace

// Patches/<model>/<name>.patch decoded into the scratch decode slot; nullptr
// when missing, damaged or of another engine.
const SynthPatch* App::readSynthLibraryPatch(int s, const char* name) {
    if (s < 0 || s >= kSynthSlotCount || !mem_.scratch || mem_.scratchBytes < kPatchTmpOff + sizeof(SynthPatch))
        return nullptr;
    if (jobBusy()) finishJob();   // the job shares the scratch (spec 05 appendix E3)
    const int32_t eng = slotEngineId(s);
    char path[96];
    if (!libraryPath(path, sizeof(path), eng, name)) return nullptr;
    uint8_t* buf = mem_.scratch + kFileOff;
    auto* tmp = reinterpret_cast<SynthPatch*>(mem_.scratch + kPatchTmpOff);
    uint32_t got = 0;
    if (readWholeFile(fs(), path, buf, kPatchMaxFileSize + 1, &got) != FmtStatus::Ok || got > kPatchMaxFileSize)
        return nullptr;
    if (decodePatch(buf, got, tmp) != PatchStatus::kOk || tmp->engine != eng) return nullptr;
    return tmp;
}

bool App::previewSynthLibraryPatch(int s, const char* name) {
    const SynthPatch* p = readSynthLibraryPatch(s, name);
    if (!p) return false;
    if (previewSlot_ != s) {
        endSynthPreview();
        std::memcpy(&g_previewSaved, &patches_[s], sizeof(SynthPatch));
        previewSrc_ = patchSrc_[s];
        previewDirty_ = patchDirty_[s];
        patchDirty_[s] = false;   // the autosave never writes a previewed patch
        previewSlot_ = int8_t(s);
    }
    std::memcpy(&patches_[s], p, sizeof(SynthPatch));
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
    std::memcpy(&patches_[s], &g_previewSaved, sizeof(SynthPatch));
    patchSrc_[s] = previewSrc_;
    patchDirty_[s] = previewDirty_;
    engine.synth().setSlotRecord(s, project.synthSlot(s));
    engine.synth().patchChanged(s);
    chaseReapply(s);   // [3.x] stopped: back on the preset the chase has it on (a load after this is heard)
}

void App::setSynthSlotRecord(int s, const SynthSlotRecord& r) {
    if (s < 0 || s >= kSynthSlotCount) return;
    const SynthSlotRecord old = project.synthSlot(s);
    project.setSynthSlot(s, r);
    const SynthSlotRecord now = project.synthSlot(s);
    // [3.x] An edit of the slot's own mix is heard at once: a slot on a
    // preset that carries a mix keeps the preset's patch and insert and plays
    // with its own mix until the preset changes (SynthBank::ownMixWins).
    const int z = engine.synth().presetOf(s);
    if (z && (presets::mixTable()[z - 1].flags & audio::kPresetMixHas) &&
        (std::memcmp(&old.gainDb, &now.gainDb, sizeof now.gainDb) != 0 ||
         std::memcmp(&old.pan, &now.pan, sizeof now.pan) != 0 ||
         std::memcmp(&old.reverbSend, &now.reverbSend, sizeof now.reverbSend) != 0 ||
         std::memcmp(&old.delaySend, &now.delaySend, sizeof now.delaySend) != 0))
        engine.synth().ownMixWins(s);
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
    if (previewSlot_ == s) endSynthPreview();
    const SynthPatch* tmp = readSynthLibraryPatch(s, name);
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
    const int32_t eng = slotEngineId(s);
    char path[96], dir[48], tmpPath[104];
    if (!libraryPath(path, sizeof(path), eng, name)) return false;
    uint8_t* out = mem_.scratch + kFileOff;
    size_t n = 0;
    if (encodePatch(patches_[s], out, kPatchMaxFileSize, &n) != PatchStatus::kOk) return false;
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
    setName(r, name);
    setSynthSlotRecord(s, r);
    return true;
}

}  // namespace ot
