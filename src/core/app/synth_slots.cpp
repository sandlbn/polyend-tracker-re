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

SynthModel modelOf(const SynthSlotRecord& r) {
    return SynthModel(r.model < uint32_t(kSynthModelCount) ? r.model : 0);
}

void setName(SynthSlotRecord& r, const char* name) {
    std::snprintf(r.patchName, sizeof(r.patchName), "%s", name ? name : "");
}

// Macro page values M1..M6 -> the patch defaults (63 bipolar, else 0).
void resetMacros(SynthSlotRecord& r, const SynthPatch& p) {
    for (int k = 0; k < int(mt::kSynthMacroCount); ++k) r.macros[k] = audio::synthMacroDefault(p.macros[k].bipolar);
}

bool libraryPath(char* out, size_t cap, SynthModel m, const char* name) {
    if (!name || !name[0] || std::strchr(name, '/')) return false;
    std::snprintf(out, cap, "Patches/%s/%s.patch", synthModelName(m), name);
    return true;
}

// The slot's patch while a Load Patch preview plays another one (one slot
// at a time; main loop only).
OT_SDRAM_BSS SynthPatch g_previewSaved;
}  // namespace

SynthPatch* App::scratchSynthPatch() {
    if (!mem_.scratch || mem_.scratchBytes < kPatchTmpOff + sizeof(SynthPatch)) return nullptr;
    if (jobBusy()) finishJob();   // the job shares the scratch (spec 05 appendix E3)
    return reinterpret_cast<SynthPatch*>(mem_.scratch + kPatchTmpOff);
}

// Patches/<model>/<name>.patch decoded into the scratch decode slot; nullptr
// when missing, damaged or of another engine.
SynthPatch* App::readSynthLibraryPatch(SynthModel m, const char* name) {
    SynthPatch* tmp = scratchSynthPatch();
    if (!tmp) return nullptr;
    char path[96];
    if (!libraryPath(path, sizeof(path), m, name)) return nullptr;
    uint8_t* buf = mem_.scratch + kFileOff;
    uint32_t got = 0;
    if (readWholeFile(fs(), path, buf, kPatchMaxFileSize + 1, &got) != FmtStatus::Ok || got > kPatchMaxFileSize)
        return nullptr;
    if (decodePatch(buf, got, tmp) != PatchStatus::kOk || tmp->engine != synthEngineId(m)) return nullptr;
    patchStripPreset(*tmp);   // [OT] a preset's mix, name, polyphony (a copied preset file) are not the slot's
    return tmp;
}

bool App::previewSynthLibraryPatch(int s, const char* name) {
    if (s < 0 || s >= kSynthSlotCount) return false;
    // [OT] On the edit target: a preset's entry while the slot is on one.
    const int n = synthEditTarget(s);
    const SynthSlotRecord t = synthTargetRecord(s);
    if (t.model >= uint32_t(kSynthModelCount)) return false;
    SynthPatch* p = readSynthLibraryPatch(SynthModel(t.model), name);
    if (!p) return false;
    if (previewSlot_ != s || previewPreset_ != n) {
        endSynthPreview();
        if (n) {
            std::memcpy(&g_previewSaved, &presets::table()[n - 1], sizeof(SynthPatch));
            uint8_t& d = presets::dirty();
            previewDirty_ = (d >> (n - 1)) & 1;
            d = uint8_t(d & ~(1u << (n - 1)));   // the autosave never writes a previewed patch
        } else {
            std::memcpy(&g_previewSaved, &patches_[s], sizeof(SynthPatch));
            previewSrc_ = patchSrc_[s];
            previewDirty_ = patchDirty_[s];
            patchDirty_[s] = false;   // the autosave never writes a previewed patch
        }
        previewSlot_ = int8_t(s);
        previewPreset_ = uint8_t(n);
    }
    SynthSlotRecord r = project.synthSlot(s);
    if (n) {
        // [OT] The preset's mix, name and polyphony play on; the engine is the
        // entry's (the browser lists its model).
        SynthPatch& e = presets::table()[n - 1];
        p->mixCount = e.mixCount;
        std::memcpy(p->mix, e.mix, sizeof(p->mix));
        p->presetNameLen = e.presetNameLen;
        std::memcpy(p->presetName, e.presetName, sizeof(p->presetName));
        p->polyPresent = e.polyPresent;
        p->poly = e.poly;
        presets::copyBehindEngine(e, *p);
        resetMacros(r, e);
        engine.synth().setSlotRecord(s, r);   // audio side only; the project record is untouched
        engine.synth().presetChanged(uint8_t(n));
        return true;
    }
    std::memcpy(&patches_[s], p, sizeof(SynthPatch));
    resetMacros(r, patches_[s]);
    engine.synth().setSlotRecord(s, r);   // audio side only; the project record is untouched
    engine.synth().patchChanged(s);
    return true;
}

void App::endSynthPreview() {
    const int s = previewSlot_;
    if (s < 0 || s >= kSynthSlotCount) return;
    previewSlot_ = -1;
    const int n = previewPreset_;
    previewPreset_ = 0;
    if (n >= 1 && n <= kSynthPresetCount) {
        // [OT] The preset's entry back byte for byte (its engine never changed).
        SynthPatch& e = presets::table()[n - 1];
        if (e.engine == g_previewSaved.engine) presets::copyBehindEngine(e, g_previewSaved);
        if (previewDirty_) presets::dirty() = uint8_t(presets::dirty() | (1u << (n - 1)));
        engine.synth().setSlotRecord(s, project.synthSlot(s));
        engine.synth().presetChanged(uint8_t(n));
        return;
    }
    std::memcpy(&patches_[s], &g_previewSaved, sizeof(SynthPatch));
    patchSrc_[s] = previewSrc_;
    patchDirty_[s] = previewDirty_;
    engine.synth().setSlotRecord(s, project.synthSlot(s));
    engine.synth().patchChanged(s);
    chaseReapply(s);   // [OT] stopped: back on the preset the chase has it on
}

void App::setSynthSlotRecord(int s, const SynthSlotRecord& r) {
    if (s < 0 || s >= kSynthSlotCount) return;
    project.setSynthSlot(s, r);
    engine.synth().setSlotRecord(s, project.synthSlot(s));   // main-loop setter: applies at the next block
    markProjectDirty();
}

void App::setSynthModel(int s, int model) {
    if (s < 0 || s >= kSynthSlotCount || model < 0 || model >= kSynthModelCount) return;
    if (previewSlot_ == s) endSynthPreview();
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
    patchInit(synthEngineId(modelOf(r)), &patches_[s]);
    patchSrc_[s] = PatchSource::Init;
    setName(r, "Init");
    resetMacros(r, patches_[s]);
    setSynthSlotRecord(s, r);
    markSynthPatchDirty(s);
}

bool App::loadSynthLibraryPatch(int s, const char* name) {
    if (previewSlot_ == s) endSynthPreview();
    if (s < 0 || s >= kSynthSlotCount) return false;
    const SynthPatch* tmp = readSynthLibraryPatch(modelOf(project.synthSlot(s)), name);
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
        const SynthSlotRecord r = project.synthSlot(s);
        const SynthModel m = modelOf(r);
        if (audio::synthEngineFor(synthEngineId(m))) continue;
        std::snprintf(out, cap, "Synth %d (%s) not available", s + 1, synthModelName(m));
        return true;
    }
    return false;
}

bool App::writeSynthLibraryPatch(SynthModel m, const SynthPatch& p, const char* name) {
    if (!mem_.scratch || mem_.scratchBytes < kPatchMaxFileSize) return false;
    if (jobBusy()) finishJob();
    char path[96], dir[48], tmpPath[104];
    if (!libraryPath(path, sizeof(path), m, name)) return false;
    uint8_t* out = mem_.scratch + kFileOff;
    size_t n = 0;
    if (encodePatch(p, out, kPatchMaxFileSize, &n) != PatchStatus::kOk) return false;
    IStorage& f = fs();
    std::snprintf(dir, sizeof(dir), "Patches/%s", synthModelName(m));
    if (!f.mkdir("Patches") || !f.mkdir(dir)) return false;
    std::snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
    if (writeWholeFile(f, tmpPath, out, uint32_t(n)) != FmtStatus::Ok) {
        f.remove(tmpPath);
        return false;
    }
    if (f.exists(path) && !f.remove(path)) return false;
    return f.rename(tmpPath, path);
}

bool App::saveSynthLibraryPatch(int s, const char* name) {
    if (s < 0 || s >= kSynthSlotCount) return false;
    SynthSlotRecord r = project.synthSlot(s);
    if (!writeSynthLibraryPatch(modelOf(r), patches_[s], name)) return false;
    setName(r, name);
    setSynthSlotRecord(s, r);
    return true;
}

// --- [OT] the patch operations of the synth pages on the edit target --------

void App::setSynthTargetModel(int s, int model, int n) {
    if (n < 0) n = synthEditTarget(s);
    if (n && !synthPresetUsed(n)) return;
    if (!n) {
        setSynthModel(s, model);
        return;
    }
    if (model < 0 || model >= kSynthModelCount) return;
    const SynthModel m = SynthModel(model);
    const SynthPatch* p = readSynthLibraryPatch(m, "Init");
    if (!p) {
        SynthPatch* tmp = scratchSynthPatch();
        if (!tmp || !patchInit(synthEngineId(m), tmp)) return;
        p = tmp;
    }
    setSynthPresetPatch(n, *p, "Init");
}

void App::initSynthTargetPatch(int s, int n) {
    if (n < 0) n = synthEditTarget(s);
    if (n && !synthPresetUsed(n)) return;
    if (!n) {
        initSynthPatch(s);
        return;
    }
    SynthPatch* tmp = scratchSynthPatch();
    if (!tmp || !patchInit(presets::table()[n - 1].engine, tmp)) return;
    setSynthPresetPatch(n, *tmp, "Init");
}

bool App::loadSynthTargetLibraryPatch(int s, const char* name, int n) {
    if (previewSlot_ == s) endSynthPreview();
    if (n < 0) n = synthEditTarget(s);
    if (!n) return loadSynthLibraryPatch(s, name);
    if (!synthPresetUsed(n)) return false;
    const SynthSlotRecord t = synthTargetRecord(s, n);
    if (t.model >= uint32_t(kSynthModelCount)) return false;
    const SynthPatch* p = readSynthLibraryPatch(SynthModel(t.model), name);
    if (!p) return false;
    setSynthPresetPatch(n, *p, name);
    return true;
}

bool App::saveSynthTargetLibraryPatch(int s, const char* name, int n) {
    if (n < 0) n = synthEditTarget(s);
    if (!n) return saveSynthLibraryPatch(s, name);
    if (!synthPresetUsed(n)) return false;
    const SynthSlotRecord t = synthTargetRecord(s, n);
    SynthPatch* tmp = scratchSynthPatch();
    if (t.model >= uint32_t(kSynthModelCount) || !tmp) return false;
    // A library patch never carries what only a preset does.
    std::memcpy(tmp, &presets::table()[n - 1], sizeof(SynthPatch));
    patchStripPreset(*tmp);
    if (!writeSynthLibraryPatch(SynthModel(t.model), *tmp, name)) return false;
    patchSetPresetName(presets::table()[n - 1], name);
    markSynthPresetDirty(n);
    return true;
}

}  // namespace ot
