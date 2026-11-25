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
#include "modules/module.h"
#include "modules/synth_descriptor.h"
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

// [I] The open import file (openPatchImport; main loop only): its name, the
// engine whose folder and format it is, its patch count and length. Its
// bytes live in the scratch, which jobs share: every preview / load reads
// the file again. Main-loop data: SDRAM on the device.
OT_SDRAM_BSS char g_importFile[64];
OT_SDRAM_BSS int32_t g_importEngine;
OT_SDRAM_BSS int g_importCount;
OT_SDRAM_BSS uint32_t g_importLen;

const mod::PatchImport* importOf(int32_t engine) { return mod::ModuleHost::instance().patchImport(engine); }
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

bool App::previewSynthLibraryPatch(int s, const char* name, uint8_t t) {
    if (s < 0 || s >= kSynthSlotCount) return false;
    const SynthPatch* p = readSynthLibraryPatch(synthTargetEngine(s, synthTargetOf(s, t)), name);
    return p && previewSynthPatch(s, *p, t);
}

bool App::previewSynthPatch(int s, const SynthPatch& p, uint8_t t) {
    if (s < 0 || s >= kSynthSlotCount) return false;
    const uint8_t n = synthTargetOf(s, t);
    if (previewSlot_ != s || g_previewPreset != n) {
        endSynthPreview();
        std::memcpy(&g_previewSaved, &synthTargetPatch(s, n), sizeof(SynthPatch));
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
    std::memcpy(&synthTargetPatch(s, n), &p, sizeof(SynthPatch));
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

void App::dropSynthPreview() {
    previewSlot_ = -1;
    g_previewPreset = 0;
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
    loadSynthPatch(s, *tmp, name);
    return true;
}

void App::loadSynthPatch(int s, const SynthPatch& p, const char* name) {
    SynthSlotRecord r = project.synthSlot(s);
    patches_[s] = p;
    patchSrc_[s] = PatchSource::Library;
    setName(r, name);
    resetMacros(r, patches_[s]);
    setSynthSlotRecord(s, r);
    markSynthPatchDirty(s);
}

int App::openPatchImport(int s, const char* file, uint8_t t) {
    std::snprintf(g_importFile, sizeof(g_importFile), "%s", file ? file : "");
    g_importEngine = s >= 0 && s < kSynthSlotCount ? synthTargetEngine(s, synthTargetOf(s, t)) : 0;
    return readPatchImport();
}

// g_importFile into the scratch (kFileOff); its patch count.
int App::readPatchImport() {
    g_importCount = 0;
    g_importLen = 0;
    const char* file = g_importFile;
    const mod::PatchImport* im = importOf(g_importEngine);
    if (!im) return 0;   // none, or incomplete (ModuleHost::patchImport)
    if (!mem_.scratch || mem_.scratchBytes < kPatchTmpOff + sizeof(SynthPatch)) return 0;
    if (!file[0] || std::strchr(file, '/')) return 0;
    if (jobBusy()) finishJob();   // the job shares the scratch
    char path[96];
    std::snprintf(path, sizeof(path), "Patches/%s/%s", synthEngineFolder(g_importEngine), file);
    uint32_t got = 0;
    if (readWholeFile(fs(), path, mem_.scratch + kFileOff, im->maxBytes + 1, &got) != FmtStatus::Ok ||
        got > im->maxBytes)
        return 0;
    const int n = im->count(mem_.scratch + kFileOff, got);
    g_importCount = n > 0 ? n : 0;
    g_importLen = got;
    return g_importCount;
}

void App::importPatchName(int k, char* out, int cap) const {
    if (cap <= 0) return;
    out[0] = 0;
    const mod::PatchImport* im = importOf(g_importEngine);
    if (!im || k < 0 || k >= g_importCount || !mem_.scratch) return;
    im->name(mem_.scratch + kFileOff, g_importLen, k, out, cap);
    out[cap - 1] = 0;
    int n = 0;
    for (; out[n]; ++n) {
        const char c = out[n];
        // Printable ASCII that a FAT file name can hold (Save names the file after it).
        if (c < 0x20 || c > 0x7E || std::strchr("/\\*?<>|:\"", c)) out[n] = '_';
    }
    if (!n) std::snprintf(out, size_t(cap), "Voice %d", k + 1);
}

const SynthPatch* App::importPatch(int32_t eng, int k) {
    if (k < 0) return nullptr;
    const mod::PatchImport* im = importOf(eng);
    if (!im || eng != g_importEngine) return nullptr;
    if (k >= readPatchImport()) return nullptr;   // the scratch may have been used since the file opened
    auto* tmp = reinterpret_cast<SynthPatch*>(mem_.scratch + kPatchTmpOff);
    if (!patchInit(eng, tmp) || !im->toPatch(mem_.scratch + kFileOff, g_importLen, k, *tmp)) return nullptr;
    return tmp;
}

bool App::previewImportPatch(int s, int k, uint8_t t) {
    if (s < 0 || s >= kSynthSlotCount) return false;
    const SynthPatch* p = importPatch(synthTargetEngine(s, synthTargetOf(s, t)), k);
    return p && previewSynthPatch(s, *p, t);
}

bool App::loadImportPatch(int s, int k, uint8_t t) {
    if (s < 0 || s >= kSynthSlotCount) return false;
    const uint8_t n = synthTargetOf(s, t);
    if (previewSlot_ >= 0) endSynthPreview();
    const SynthPatch* p = importPatch(synthTargetEngine(s, n), k);
    if (!p) return false;
    char name[40];
    importPatchName(k, name, sizeof(name));   // before the load: it reads the scratch
    if (n) setSynthPresetPatch(n, *p, name);   // [3.x] the preset's entry, as loadSynthTargetPatch
    else loadSynthPatch(s, *p, name);
    return true;
}

// [3.x] The edit target's patch operations (screen_synth.cpp): on a preset
// they replace the preset's entry, the slot's own otherwise.
void App::setSynthTargetEngine(int s, int32_t engineId, uint8_t t) {
    const uint8_t n = synthTargetOf(s, t);
    if (!n) {
        setSynthEngine(s, engineId);
        return;
    }
    if (previewSlot_ >= 0) endSynthPreview();
    const SynthPatch* p = readSynthLibraryPatch(engineId, "Init");
    if (!p) {
        SynthPatch* init = scratchPatch();
        if (!init || !patchInit(engineId, init)) return;
        p = init;
    }
    setSynthPresetPatch(n, *p, "Init");
}

bool App::loadSynthTargetPatch(int s, const char* name, uint8_t t) {
    const uint8_t n = synthTargetOf(s, t);
    if (!n) return loadSynthLibraryPatch(s, name);
    if (previewSlot_ >= 0) endSynthPreview();
    const SynthPatch* p = readSynthLibraryPatch(synthTargetEngine(s, n), name);
    if (!p) return false;
    setSynthPresetPatch(n, *p, name);
    return true;
}

void App::initSynthTargetPatch(int s, uint8_t t) {
    const uint8_t n = synthTargetOf(s, t);
    if (!n) {
        initSynthPatch(s);
        return;
    }
    if (previewSlot_ >= 0) endSynthPreview();
    SynthPatch* init = scratchPatch();
    if (init && patchInit(synthTargetEngine(s, n), init)) setSynthPresetPatch(n, *init, "Init");
}

bool App::synthUnavailableNotice(char* out, size_t cap) const {
    for (int s = 0; s < kSynthSlotCount; ++s) {
        const int32_t eng = slotEngineId(s);
        if (eng && audio::synthEngineFor(eng)) continue;
        if (const char* why = cardSlotState(s))
            std::snprintf(out, cap, "Synth %d (%s): %s", s + 1, synthEngineDisplayName(eng), why);
        else
            std::snprintf(out, cap, "Synth %d (%s) not available", s + 1, synthEngineDisplayName(eng));
        return true;
    }
    return false;
}

// ---- [I] card synths ------------------------------------------------------

const char* App::cardSlotState(int s) const {
    const int32_t eng = slotEngineId(s);
    const mod::CardEngine* e = mod::cardEngine(eng);
    if (!e || mod::cardEngineLoaded(eng)) return nullptr;
    if (seq.isPlaying() || seq.isArmed()) return "Loads at Stop";
    int32_t engines[kCardWanted];
    cardWantedEngines(engines);
    if (std::memcmp(cardTried_, engines, sizeof(cardTried_)) != 0) return "Loading";
    const mod::OtmLoadReport& r = moduleLoad_;
    if (r.noRoom) return "No card memory";
    const mod::OtmLoadReport::Item& it = r.items[e->file];
    if (it.refused) return "Module refused";
    if (it.error == mod::OtmError::TooBig) return "Card memory full";
    return mod::otmErrorText(it.error);
}

void App::cardWantedEngines(int32_t* out) const {
    for (int s = 0; s < kSynthSlotCount; ++s) out[s] = slotEngineId(s);
    // A Z or a preset instrument plays a preset on a slot: its synth is
    // loaded while the preset names it, as a slot's is.
    for (int n = 1; n <= kSynthPresetCount; ++n)
        out[kSynthSlotCount + n - 1] = synthPresetUsed(n) ? synthPreset(n).engine : 0;
}

void App::serviceCardSynths(uint32_t now) {
    int32_t engines[kCardWanted];
    cardWantedEngines(engines);
    const bool synthsTried = std::memcmp(cardTried_, engines, sizeof(cardTried_)) == 0;
    const bool fxWaiting = cardFxWaiting(now);   // [I] the card effects that are on changed
    if (synthsTried && !fxWaiting) {
        cardSince_ = 0;
        return;
    }
    if (!synthsTried && (!cardSince_ || std::memcmp(cardWaiting_, engines, sizeof(cardWaiting_)) != 0)) {
        std::memcpy(cardWaiting_, engines, sizeof(cardWaiting_));
        cardSince_ = now ? now : 1;
        return;
    }
    if (!cardSince_) {
        cardSince_ = now ? now : 1;
        return;
    }
    if (now - cardSince_ < kCardSettleMs || seq.isPlaying() || seq.isArmed() || jobBusy() || sampleTailBusy()) return;
    cardSince_ = 0;
    syncCardSynths(true);
}

void App::holdSynthEngines() {
    // No card engine runs from here on: the next block drops the synth
    // voices (a block is 2.9 ms); when no block runs on its own, this does.
    // The 50 ms fallback is safe with the hot rows in the fast block too: the
    // main loop never runs inside a block (the audio ISR), and after
    // dropEngines nothing in a block reaches an engine: render and noteOn
    // check hold_, noteOff and applyFx find no voice, stopAll no engine.
    audio::SynthBank& bank = engine.synth();
    bank.holdEngines(true);
    const uint32_t t0 = millis();
    while (!bank.enginesHeld())
        if (!hal_.audio || !hal_.audio->blocksOnTheirOwn() || millis() - t0 > 50) {
            bank.dropEngines();
            break;
        }
}

void App::rescanCardSynths() {
    if (!mem_.scratch) return;
    if (previewSlot_ >= 0) endSynthPreview();
    holdSynthEngines();
    engine.setMasterInserts(nullptr, 0);   // [I] card effects leave too
    mod::rescanCardSynths(fs(), mem_.scratch, mem_.scratchBytes);
    engine.synth().holdEngines(false);
    bank.setModuleHold(0);
    bindMasterInserts();
    // Nothing tried yet: the project load (or a model change) loads anew.
    std::memset(cardTried_, 0, sizeof(cardTried_));
    for (int s = 0; s < kSynthSlotCount; ++s) engine.synth().patchChanged(s);
}

bool App::syncCardSynths(bool initPatches) {
    // The slots' and the presets' engines: card synths load, card and
    // built-in synths share the fast block (modules/module_files.h).
    int32_t engines[kCardWanted];
    cardWantedEngines(engines);
    std::memcpy(cardTried_, engines, sizeof(cardTried_));
    // [I] With the card effects that are on, waiting to load or have project
    // data (app/master_fx.cpp).
    int fx[mod::kMaxCardFx];
    int nFx = wantedCardFx(fx);
    markCardFxTried(fx, nFx);
    if (!mod::cardSynthsDiffer(engines, kCardWanted, fx, nFx)) {
        settleCardFx();
        return true;
    }
    // The scratch is free: serviceCardSynths waits for the job to end, the
    // project load calls this between its units.
    if (!mem_.scratch) return false;
    if (previewSlot_ >= 0) endSynthPreview();
    audio::SynthBank& bank = engine.synth();
    // A slot on a preset (a Z) stays on it: the reload is not an edit.
    uint8_t onPreset[kSynthSlotCount];
    for (int s = 0; s < kSynthSlotCount; ++s) onPreset[s] = uint8_t(bank.presetOf(s));
    holdSynthEngines();
    beginCardFx(fx, &nFx);
    const bool ok =
        mod::loadCardSynths(fs(), engines, kCardWanted, mem_.scratch, mem_.scratchBytes, fx, nFx);
    bank.holdEngines(false);
    const bool again = endCardFx(fx, nFx);
    for (int s = 0; s < kSynthSlotCount; ++s) {
        const int32_t eng = slotEngineId(s);
        // A slot that took its card engine before it loaded: its Init patch now.
        if (initPatches && patches_[s].engine != eng && audio::synthEngineFor(eng)) {
            SynthSlotRecord r = project.synthSlot(s);
            setName(r, "Init");
            project.setSynthSlot(s, r);
            if (!loadSynthLibraryPatch(s, "Init")) initSynthPatch(s);
        }
        engine.synth().patchChanged(s);
        if (onPreset[s]) bank.presetSelect(s, onPreset[s]);   // false: the preset is empty now
    }
    if (!ok && initPatches) {
        char t[64];
        if (synthUnavailableNotice(t, sizeof(t))) ui.notify(t, 4000);
    }
    ui.dirty();
    // [I] Card effects Open loaded for their data that are off leave again.
    if (again) syncCardSynths(initPatches);
    return ok;
}

bool App::saveSynthLibraryPatch(int s, const char* name, uint8_t t) {
    if (s < 0 || s >= kSynthSlotCount || !mem_.scratch || mem_.scratchBytes < kPatchMaxFileSize) return false;
    if (jobBusy()) finishJob();
    SynthSlotRecord r = project.synthSlot(s);
    const uint8_t tg = synthTargetOf(s, t);
    const int32_t eng = synthTargetEngine(s, tg);   // [3.x] the edit target's
    char path[96], dir[48], tmpPath[104];
    if (!libraryPath(path, sizeof(path), eng, name)) return false;
    uint8_t* out = mem_.scratch + kFileOff;
    size_t n = 0;
    if (encodePatch(synthTargetPatch(s, tg), out, kPatchMaxFileSize, &n) != PatchStatus::kOk) return false;
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
    if (tg) {
        setSynthPresetName(tg, name);
        return true;
    }
    setName(r, name);
    setSynthSlotRecord(s, r);
    return true;
}

}  // namespace ot
