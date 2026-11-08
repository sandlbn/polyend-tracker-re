// [3.x] Edit what you hear: while a synth slot plays a preset (Z, the
// stopped chase, a P1..P8 note), the Synth pages and the Mixer show that
// preset and edits change the preset itself, heard at once and saved; the
// slot stays on it. Z0, Stop and a note of the slot's own instrument show
// the slot's own sound again. A preset carries the slot's polyphony too.
#include "test.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "app/synth_presets.h"
#include "audio/audio_tables.h"
#include "audio/synth_bank.h"
#include "audio/synth_params.h"
#include "formats/ot3.h"
#include "formats/patch_pb.h"
#include "model/fx.h"
#include "modules/module.h"
#include "modules/synth_models.h"
#include "ui/screens.h"
#include "ui_fakes.h"
#if __has_include("ot_profile.h")
#include "ot_profile.h"
#endif

using namespace ot;
using namespace ot::audio;

namespace {

#if defined(OT_MOD_ACD1) && OT_MOD_ACD1 && defined(OT_MOD_FAT1) && OT_MOD_FAT1
// --- the bank ------------------------------------------------------------------

struct Rig {
    SynthPatch own;
    SynthPatch others[2];
    SynthPatch presets[kSynthPresetCount] = {};
    PresetMix mix[kSynthPresetCount] = {};
    Governor gov;
    SynthBank bank;
    SynthMixInputs in;
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    Rig() {
        initTables();
        gov.setBudget(8);
        bank.setGovernor(&gov);
        bank.setStartsPerBlock(0);
        patchInit(synthEngineId(SynthModel::ACD), &own);
        for (SynthPatch& p : presets) p = own;
        presets[kSynthPresetCount - 1].engine = 0;
        bank.bindPatch(0, &own);
        for (int s = 1; s < kSynthSlots; ++s) {
            others[s - 1] = own;
            bank.bindPatch(s, &others[s - 1]);
        }
        bank.bindPresets(presets);
        bank.bindPresetMix(mix);
        for (int s = 0; s < kSynthSlots; ++s) bank.setSlotRecord(s, record(4));
        block();
    }
    static SynthSlotRecord record(int poly) {
        SynthSlotRecord r = SynthSlotRecord::defaults(0);
        r.model = uint32_t(SynthModel::ACD);
        r.polyphony = uint32_t(poly);
        return r;
    }
    void block() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        bank.render(in, L, R, D, V, nullptr);
    }
    void blocks(int n) {
        for (int b = 0; b < n; ++b) block();
    }
    void on(int track, int note, int slot = 0) {
        SynthNoteOn n;
        n.slot = uint8_t(slot);
        n.note = uint8_t(note);
        n.volume = -1;
        bank.noteOn(track, n);
    }
    void pOn(int track, int k, int note) {
        SynthNoteOn n;
        n.slot = 0;
        n.preset = uint8_t(k);
        n.note = uint8_t(note);
        n.volume = -1;
        bank.noteOn(track, n);
    }
};

// A parameter of the engine with a range, and a value inside it that is not v.
int rangedParam(int32_t engine, float v, float* other) {
    int n = 0;
    const PatchParamRange* t = patchEngineParams(engine, &n);
    for (int i = 0; i < n; ++i) {
        if (t[i].flags & kParamFlagNoRange || t[i].max <= t[i].min) continue;
        const float mid = 0.5f * (t[i].min + t[i].max);
        *other = std::fabs(mid - v) > 1e-3f ? mid : t[i].min;
        return i;
    }
    return -1;
}

TEST(a_preset_edit_ramps_a_parameter_and_the_slot_stays_on_the_preset) {
    Rig r;
    r.on(0, 48);
    CHECK(r.bank.presetSelect(0, 1));
    r.blocks(2);
    const int32_t acd = synthEngineId(SynthModel::ACD);
    float nv = 0.0f;
    const int i = rangedParam(acd, 0.0f, &nv);
    CHECK(i >= 0);
    nv = 0.5f * (synthParamGet(r.presets[0], i) + nv);
    synthParamSet(r.presets[0], i, nv);
    r.bank.presetChanged(1);
    r.block();
    CHECK_EQ(int(r.bank.presetOf(0)), 1);
    CHECK_NEAR(r.bank.params(0).end[i], nv, 1e-6);
    CHECK(r.bank.voicesOf(0) == 1);   // the note plays on (a ramp, not a restart)
    // The own patch was not touched; an edit of another preset changes nothing here.
    CHECK(std::fabs(synthParamGet(r.own, i) - nv) > 1e-6f);
    synthParamSet(r.presets[1], i, synthParamGet(r.own, i));
    r.bank.presetChanged(2);
    r.block();
    CHECK_NEAR(r.bank.params(0).end[i], nv, 1e-6);
    r.bank.presetSelect(0, 0);
    r.block();
    CHECK_NEAR(r.bank.params(0).end[i], synthParamGet(r.own, i), 1e-6);
}

TEST(a_new_engine_on_the_preset_restarts_the_slot_on_it) {
    Rig r;
    r.on(0, 48);
    r.bank.presetSelect(0, 1);
    r.blocks(2);
    CHECK_EQ(int(r.bank.voicesOf(0)), 1);
    const int32_t fat = synthEngineId(SynthModel::FAT);
    patchInit(fat, &r.presets[0]);
    r.bank.presetChanged(1);
    r.block();
    CHECK_EQ(int(r.bank.presetOf(0)), 1);
    CHECK_EQ(int(r.bank.params(0).engine), int(fat));
    CHECK_EQ(int(r.bank.voicesOf(0)), 0);   // the old engine's note faded
    r.on(1, 50);
    r.block();
    CHECK_EQ(int(r.bank.voicesOf(0)), 1);
}

TEST(a_preset_carries_its_polyphony) {
    Rig r;
    r.mix[0].polyphony = 1;
    r.mix[0].flags = kPresetMixPoly;   // no mix: only the voices
    r.mix[1].flags = 0;                // saved without polyphony: the slot's 4
    r.bank.presetSelect(0, 1);
    r.block();
    CHECK_EQ(r.bank.slotView(0).polyphony, 4);   // the record stays as it is
    r.on(0, 48);
    r.on(1, 52);
    r.on(2, 55);
    r.blocks(2);
    CHECK_EQ(r.bank.voicesOf(0), 1);   // mono
    r.bank.stopAll();
    r.bank.presetSelect(0, 2);
    r.block();
    r.on(3, 48);
    r.on(4, 52);
    r.on(5, 55);
    r.blocks(2);
    CHECK_EQ(r.bank.voicesOf(0), 3);
    // An edit of the preset's polyphony applies to the next notes (as a record change).
    r.bank.stopAll();
    r.bank.presetSelect(0, 1);
    r.blocks(2);
    r.mix[0].polyphony = 2;
    r.bank.presetChanged(1);
    r.block();
    for (int t = 0; t < 4; ++t) r.on(6 + t, 60 + 2 * t);
    r.blocks(2);
    CHECK_EQ(r.bank.voicesOf(0), 2);
    // Z0: the record's 4 again.
    r.bank.stopAll();
    r.bank.presetSelect(0, 0);
    r.block();
    for (int t = 0; t < 4; ++t) r.on(10 + t, 40 + 2 * t);
    r.blocks(2);
    CHECK_EQ(r.bank.voicesOf(0), 4);
}

TEST(a_preset_instrument_plays_with_the_presets_polyphony) {
    Rig r;
    r.mix[2].polyphony = 1;
    r.mix[2].flags = kPresetMixPoly;
    r.pOn(0, 3, 48);
    r.pOn(1, 3, 52);
    r.blocks(2);
    const int s = r.bank.slotOnPreset(3);
    CHECK(s >= 0);
    CHECK_EQ(int(r.bank.presetOf(s)), 3);
    CHECK_EQ(r.bank.slotOnPreset(2), -1);
    CHECK_EQ(r.bank.voicesOf(s), 1);
    r.bank.transportStop();   // Stop: the record's polyphony again
    r.bank.stopAll();
    r.block();
    for (int t = 0; t < 3; ++t) r.on(4 + t, 60 + 2 * t, s);
    r.blocks(2);
    CHECK_EQ(r.bank.voicesOf(s), 3);
}

TEST(a_preset_mix_edit_ramps_the_gain) {
    Rig r;
    r.mix[0].gainDb = -9.0f;
    r.mix[0].flags = kPresetMixHas;
    r.on(0, 48);
    r.bank.presetSelect(0, 1);
    r.blocks(2);
    CHECK_NEAR(r.bank.slotGain(0), dbToGain(-9.0f), 1e-6);
    r.mix[0].gainDb = 0.0f;
    r.bank.presetChanged(1);
    r.block();
    CHECK_NEAR(r.bank.slotGain(0), 1.0f, 1e-6);
    CHECK_EQ(int(r.bank.presetOf(0)), 1);
}

// --- the App ---------------------------------------------------------------------

// A note on slot s: the bank takes a Z at its next block with sound.
void sound(Harness& h, int s) {
    h.app->engine.synth().auditionOn(s, 60);
    h.audio.pump(8);
    h.app->engine.synth().auditionOff();
    h.audio.pump(1);
}
float gainOf(Harness& h, int s) { return h.app->engine.synth().slotGain(s); }
void setMix(App& a, int s, float gainDb, int poly = 4) {
    SynthSlotRecord r = a.synthSlot(s);
    r.gainDb = gainDb;
    r.polyphony = uint32_t(poly);
    a.setSynthSlotRecord(s, r);
}
// Slot 0 ACD at -12 dB, slot 1 FAT at -3 dB stored as P1 (named "Bells", poly 2).
void twoSounds(Harness& h) {
    App& a = *h.app;
    if (a.synthSlot(0).model != uint32_t(SynthModel::ACD)) a.setSynthModel(0, int(SynthModel::ACD));
    a.setSynthModel(1, int(SynthModel::FAT));
    setMix(a, 0, -12.0f, 4);
    setMix(a, 1, -3.0f, 2);
    SynthSlotRecord r = a.synthSlot(1);
    std::snprintf(r.patchName, sizeof r.patchName, "Bells");
    a.setSynthSlotRecord(1, r);
    a.storeSynthPreset(1, 1);
}
std::string wsPreset(int n) { return "Workspace/presets/" + std::to_string(n) + ".patch"; }
void cursorTo(Harness& h, int step) { static_cast<PatternScreen*>(h.app->ui.screen(kModPattern))->step = step; }
void putZ(Step& s, int n) { s.fx[1] = StepFx{0, uint8_t(kFxPresetCmdBase + n)}; }   // FX-A

TEST(the_target_is_the_preset_the_slot_plays) {
    Harness h;
    App& a = *h.app;
    twoSounds(h);
    const int32_t acd = synthEngineId(SynthModel::ACD), fat = synthEngineId(SynthModel::FAT);
    CHECK_EQ(int(a.synthEditTarget(0)), 0);
    CHECK_EQ(int(a.synthTargetEngine(0)), int(acd));
    a.engine.synth().presetSelect(0, 1);
    sound(h, 0);
    CHECK_EQ(int(a.synthEditTarget(0)), 1);
    CHECK_EQ(int(a.synthTargetEngine(0)), int(fat));
    CHECK(&a.synthTargetPatch(0) == &presets::table()[0]);
    const SynthSlotRecord t = a.synthTargetRecord(0);
    CHECK_NEAR(t.gainDb, -3.0f, 1e-6);
    CHECK_EQ(int(t.polyphony), 2);
    CHECK(std::strcmp(t.patchName, "Bells") == 0);
    CHECK_NEAR(a.synthSlot(0).gainDb, -12.0f, 1e-6);   // the record stays the slot's
    a.engine.synth().presetSelect(0, 0);
    sound(h, 0);
    CHECK_EQ(int(a.synthEditTarget(0)), 0);
    CHECK_NEAR(a.synthTargetRecord(0).gainDb, -12.0f, 1e-6);
    CHECK_EQ(int(a.synthTargetRecord(0).polyphony), 4);
    // Stop: back to the own sound.
    a.engine.synth().presetSelect(0, 1);
    a.engine.synth().transportStop();
    CHECK_EQ(int(a.synthEditTarget(0)), 0);
}

TEST(a_mix_edit_on_the_target_changes_the_preset_and_is_heard) {
    Harness h;
    App& a = *h.app;
    twoSounds(h);
    a.engine.synth().presetSelect(0, 1);
    sound(h, 0);
    CHECK_NEAR(gainOf(h, 0), dbToGain(-3.0f), 1e-6);
    CHECK(a.saveProjectAs("Tgt"));
    SynthSlotRecord r = a.synthTargetRecord(0);
    r.gainDb = -6.0f;
    r.polyphony = 3;
    r.macros[0] = 99;   // a macro value: the slot's record
    a.setSynthTargetRecord(0, r);
    sound(h, 0);
    CHECK_EQ(int(a.synthEditTarget(0)), 1);
    CHECK_NEAR(gainOf(h, 0), dbToGain(-6.0f), 1e-6);
    CHECK_NEAR(a.synthPresetMix(1).gainDb, -6.0f, 1e-6);
    CHECK_EQ(int(a.synthPresetMix(1).polyphony), 3);
    CHECK_NEAR(a.synthSlot(0).gainDb, -12.0f, 1e-6);
    CHECK_EQ(int(a.synthSlot(0).polyphony), 4);
    CHECK_EQ(int(a.synthSlot(0).macros[0]), 99);
    CHECK(std::strcmp(a.synthSlot(0).patchName, "Bells") != 0);
    CHECK(a.sidecarDirty());
    CHECK(a.unsavedChanges());
    a.engine.synth().presetSelect(0, 0);
    sound(h, 0);
    CHECK_NEAR(gainOf(h, 0), dbToGain(-12.0f), 1e-6);
}

TEST(a_patch_edit_on_the_target_changes_the_preset_and_autosaves_it) {
    Harness h;
    App& a = *h.app;
    twoSounds(h);
    a.autosave();
    CHECK(!a.synthPresetDirty());
    const std::vector<uint8_t> before = h.fs.files[wsPreset(1)];
    a.engine.synth().presetSelect(0, 1);
    sound(h, 0);
    SynthPatch& p = a.synthTargetPatch(0);
    const int32_t fat = synthEngineId(SynthModel::FAT);
    float nv = 0.0f;
    const int i = rangedParam(fat, synthParamGet(p, 0), &nv);
    CHECK(i >= 0);
    synthParamSet(p, i, nv);
    a.markSynthTargetDirty(0);
    sound(h, 0);
    CHECK_EQ(int(a.synthEditTarget(0)), 1);   // still on the preset
    CHECK_NEAR(a.engine.synth().params(0).end[i], nv, 1e-6);
    CHECK(!a.synthPatchDirty(0));
    CHECK(a.synthPresetDirty());
    a.autosave();
    CHECK(!a.synthPresetDirty());
    CHECK(h.fs.files[wsPreset(1)] != before);
}

TEST(the_preset_name_and_polyphony_round_trip_in_the_sidecar) {
    Harness h;
    App& a = *h.app;
    twoSounds(h);
    a.setSynthPresetName(1, "FM Bells");
    std::vector<uint8_t> buf(2 * ot3::kMaxFile);
    uint32_t len = 0;
    CHECK(a.encodeSidecar(buf.data(), uint32_t(buf.size()), &len));
    buf.resize(len);
    presets::mixTable()[0] = PresetMix{};
    CHECK(a.decodeSidecar(buf.data(), len));
    CHECK(std::strcmp(a.synthPresetName(1), "FM Bells") == 0);
    CHECK(a.synthPresetMix(1).flags & kPresetMixPoly);
    CHECK_EQ(int(a.synthPresetMix(1).polyphony), 2);
    // Save As / Open bring both back.
    a.markSidecarDirty();   // the decode above left the Workspace file behind
    CHECK(a.saveProjectAs("Nam"));
    CHECK(a.newProject());
    CHECK(std::strcmp(a.synthPresetName(1), "") == 0);
    CHECK(a.openProject("Projects", "Nam"));
    CHECK(std::strcmp(a.synthPresetName(1), "FM Bells") == 0);
    CHECK_EQ(int(a.synthPresetMix(1).polyphony), 2);
}

TEST(an_old_mix_chunk_keeps_the_slots_polyphony) {
    Harness h;
    App& a = *h.app;
    twoSounds(h);
    std::vector<uint8_t> buf(2 * ot3::kMaxFile);
    uint32_t len = 0;
    presets::mixTable()[0].flags = kPresetMixHas;   // as an older build saved it
    CHECK(a.encodeSidecar(buf.data(), uint32_t(buf.size()), &len));
    CHECK(a.decodeSidecar(buf.data(), len));
    CHECK(!(a.synthPresetMix(1).flags & kPresetMixPoly));
    a.engine.synth().presetSelect(0, 1);
    sound(h, 0);
    CHECK_EQ(int(a.synthTargetRecord(0).polyphony), 4);   // the slot's
    // Load of such a preset keeps the slot's polyphony; one with it copies it.
    CHECK(a.loadSynthPreset(2, 1));
    CHECK_EQ(int(a.synthSlot(2).polyphony), int(SynthSlotRecord::defaults(2).polyphony));
    a.storeSynthPreset(3, 1);
    CHECK(a.loadSynthPreset(2, 3));
    CHECK_EQ(int(a.synthSlot(2).polyphony), 2);
}

TEST(a_new_engine_on_the_target_stays_on_the_preset) {
    Harness h;
    App& a = *h.app;
    twoSounds(h);
    const int32_t acd = synthEngineId(SynthModel::ACD);
    a.engine.synth().presetSelect(0, 1);
    sound(h, 0);
    a.setSynthTargetEngine(0, acd);
    sound(h, 0);
    CHECK_EQ(int(a.synthEditTarget(0)), 1);
    CHECK_EQ(int(a.synthPreset(1).engine), int(acd));
    CHECK_EQ(int(a.engine.synth().params(0).engine), int(acd));
    CHECK(std::strcmp(a.synthPresetName(1), "Init") == 0);
    CHECK_EQ(int(a.slotEngineId(1)), int(synthEngineId(SynthModel::FAT)));   // the slots keep theirs
    CHECK(a.synthPresetDirty());
    // Init Patch on the target, too.
    synthParamSet(a.synthTargetPatch(0), 0, synthParamGet(a.synthTargetPatch(0), 0) + 0.25f);
    a.initSynthTargetPatch(0);
    SynthPatch init;
    patchInit(acd, &init);
    CHECK(std::memcmp(&a.synthPreset(1), &init, sizeof init) == 0);
    CHECK_EQ(int(a.synthEditTarget(0)), 1);
}

TEST(store_from_a_slot_on_a_preset_stores_the_preset) {
    Harness h;
    App& a = *h.app;
    twoSounds(h);
    a.engine.synth().presetSelect(0, 1);
    sound(h, 0);
    a.storeSynthPreset(2, 0);   // slot 0 plays P1: P2 = P1
    CHECK(std::memcmp(&a.synthPreset(2), &a.synthPreset(1), sizeof(SynthPatch)) == 0);
    CHECK_NEAR(a.synthPresetMix(2).gainDb, -3.0f, 1e-6);
    CHECK_EQ(int(a.synthPresetMix(2).polyphony), 2);
    CHECK(std::strcmp(a.synthPresetName(2), "Bells") == 0);
    CHECK_EQ(int(a.synthEditTarget(0)), 1);
    const uint8_t d = presets::dirty();
    a.storeSynthPreset(1, 0);   // onto itself: nothing
    CHECK_EQ(int(presets::dirty()), int(d));
    CHECK_EQ(int(a.synthEditTarget(0)), 1);
}

TEST(a_library_patch_preview_on_the_target_puts_the_preset_back) {
    Harness h;
    App& a = *h.app;
    twoSounds(h);
    CHECK(a.saveSynthLibraryPatch(1, "Other"));   // the own FAT patch, then a change to tell them apart
    a.engine.synth().presetSelect(0, 1);
    sound(h, 0);
    synthParamSet(a.synthTargetPatch(0), 0, synthParamGet(a.synthTargetPatch(0), 0) + 0.25f);
    a.markSynthTargetDirty(0);
    SynthPatch saved = a.synthPreset(1);
    CHECK(a.previewSynthLibraryPatch(0, "Other"));
    CHECK(synthParamGet(a.synthPreset(1), 0) == synthParamGet(a.synthPatch(1), 0));
    CHECK(!a.synthPresetDirty());   // the autosave never writes a previewed preset
    CHECK_EQ(int(a.synthEditTarget(0)), 1);
    a.endSynthPreview();
    CHECK(std::memcmp(&a.synthPreset(1), &saved, sizeof saved) == 0);
    CHECK(a.synthPresetDirty());
    // A load names the preset.
    CHECK(a.loadSynthTargetPatch(0, "Other"));
    CHECK(std::strcmp(a.synthPresetName(1), "Other") == 0);
    CHECK(std::strcmp(a.synthTargetRecord(0).patchName, "Other") == 0);
}

TEST(the_target_follows_the_stopped_chase) {
    Harness h;
    App& a = *h.app;
    twoSounds(h);
    a.storeSynthPreset(2, 0);
    Pattern& p = a.pattern();
    p.tracks[0].steps[0].note = 60;
    p.tracks[0].steps[0].instrument = uint8_t(kFirstSynthInstrument);
    putZ(p.tracks[0].steps[0], 1);
    putZ(p.tracks[0].steps[4], 2);
    a.patternEdited();
    cursorTo(h, 2);
    h.run();
    sound(h, 0);
    CHECK_EQ(int(a.synthEditTarget(0)), 1);
    cursorTo(h, 6);
    h.run();
    sound(h, 0);
    CHECK_EQ(int(a.synthEditTarget(0)), 2);
    CHECK_NEAR(a.synthTargetRecord(0).gainDb, -12.0f, 1e-6);   // P2 was stored from slot 0
}

// --- the Synth pages ---------------------------------------------------------------

SynthScreen& synthScreen(App& a) { return *static_cast<SynthScreen*>(a.ui.screen(kModSynth)); }
void openSynth(Harness& h, int slot) {
    App& a = *h.app;
    a.setCurrentInstrument(uint8_t(kFirstSynthInstrument + slot));
    h.tap(btn::Pattern);
    h.tap(btn::InstrParams);
    if (synthScreen(a).view != SynthScreen::View::Params) h.tap(btn::InstrParams);
}
bool drawn(Harness& h, const char* q) {
    h.app->ui.dirty();
    h.run(40);
    for (const std::string& l : h.disp.last)
        if (l.find(q) != std::string::npos) return true;
    return false;
}

TEST(synth_parameters_show_and_edit_the_preset_the_slot_plays) {
    Harness h;
    App& a = *h.app;
    twoSounds(h);
    openSynth(h, 0);
    CHECK(drawn(h, "Synth 1 (ACD)"));
    CHECK_STR(a.ui.bar.label[0].bottom, "ACD");
    a.engine.synth().presetSelect(0, 1);
    sound(h, 0);
    CHECK(drawn(h, "Synth 1 (FAT) P1"));
    CHECK_STR(a.ui.bar.label[0].bottom, "FAT");   // the model list: the preset's engine
    CHECK_STR(a.ui.bar.label[1].bottom, "2");     // its polyphony
    CHECK(drawn(h, "Bells"));                     // its name
    char want[24];
    synthui::gainText(-3.0f, want, sizeof want);
    CHECK_STR(a.ui.bar.label[2].bottom, want);
    // The encoder on Gain: the preset's gain, heard; the record stays.
    h.tap(btn::F3);
    h.enc(2);
    sound(h, 0);
    CHECK_NEAR(a.synthPresetMix(1).gainDb, -2.0f, 1e-6);
    CHECK_NEAR(gainOf(h, 0), dbToGain(-2.0f), 1e-6);
    CHECK_NEAR(a.synthSlot(0).gainDb, -12.0f, 1e-6);
    CHECK_EQ(int(a.synthEditTarget(0)), 1);
    // Polyphony.
    h.tap(btn::F2);
    h.enc(1);
    CHECK_EQ(int(a.synthPresetMix(1).polyphony), 3);
    CHECK_EQ(int(a.synthSlot(0).polyphony), 4);
    // Z0: the slot's own again.
    a.engine.synth().presetSelect(0, 0);
    sound(h, 0);
    CHECK(drawn(h, "Synth 1 (ACD)"));
    CHECK(!drawn(h, "Synth 1 (FAT) P1"));
    CHECK_STR(a.ui.bar.label[0].bottom, "ACD");
    CHECK_STR(a.ui.bar.label[1].bottom, "4");
}

TEST(edit_patch_edits_the_preset_and_sets_its_dirty_bit) {
    Harness h;
    App& a = *h.app;
    twoSounds(h);
    a.autosave();
    openSynth(h, 0);   // (the Pattern screen's stopped chase sets the slots on the way)
    a.engine.synth().presetSelect(0, 1);
    sound(h, 0);
    h.tap(btn::F8);   // Edit Patch
    CHECK(synthScreen(a).view == SynthScreen::View::Edit);
    CHECK(drawn(h, "Edit Patch: Bells"));
    const SynthPatch own = a.synthPatch(0);
    const SynthPatch before = a.synthPreset(1);
    bool moved = false;
    for (int r = 0; r < 8 && !moved; ++r) {   // the first row that moves
        h.enc(3);
        moved = std::memcmp(&before, &a.synthPreset(1), sizeof before) != 0;
        if (!moved) h.tap(btn::Down);
    }
    CHECK(moved);
    CHECK(std::memcmp(&own, &a.synthPatch(0), sizeof own) == 0);   // the slot's own patch is untouched
    CHECK(a.synthPresetDirty());
    CHECK(!a.synthPatchDirty(0));
    CHECK_EQ(int(a.synthEditTarget(0)), 1);
}

TEST(the_model_list_on_a_preset_changes_the_presets_engine) {
    Harness h;
    App& a = *h.app;
    twoSounds(h);
    openSynth(h, 0);   // (the Pattern screen's stopped chase sets the slots on the way)
    a.engine.synth().presetSelect(0, 1);
    sound(h, 0);
    h.tap(btn::F1);
    const int want = synthModelListIndex(synthEngineId(SynthModel::ACD));
    const int cur = synthModelListIndex(synthEngineId(SynthModel::FAT));
    for (int k = 0; k < 20 && synthModelListIndex(a.synthPreset(1).engine) != want; ++k) {
        h.enc(want > cur ? 1 : -1);
        if (a.ui.askOpen()) h.tap(btn::F8);
    }
    CHECK_EQ(int(a.synthPreset(1).engine), int(synthEngineId(SynthModel::ACD)));
    CHECK_EQ(int(a.slotEngineId(0)), int(synthEngineId(SynthModel::ACD)));
    CHECK_EQ(int(a.slotEngineId(1)), int(synthEngineId(SynthModel::FAT)));   // the slot it came from keeps its own
    CHECK_EQ(int(a.synthEditTarget(0)), 1);
}

TEST(macros_show_the_presets_names_and_the_slots_values) {
    Harness h;
    App& a = *h.app;
    twoSounds(h);
    std::snprintf(presets::table()[0].macros[0].name, sizeof presets::table()[0].macros[0].name, "Shine");
    openSynth(h, 0);   // (the Pattern screen's stopped chase sets the slots on the way)
    a.engine.synth().presetSelect(0, 1);
    sound(h, 0);
    h.tap(btn::InstrParams);
    CHECK(synthScreen(a).view == SynthScreen::View::Macros);
    CHECK(drawn(h, "Shine"));
    const uint8_t v = a.synthSlot(0).macros[0];
    h.tap(btn::F1);
    h.enc(1);
    CHECK_EQ(int(a.synthSlot(0).macros[0]), int(v) + 1);
    CHECK_EQ(int(a.synthEditTarget(0)), 1);
}

TEST(instrument_params_on_a_playing_preset_instrument_opens_its_slot) {
    Harness h;
    App& a = *h.app;
    twoSounds(h);
    h.tap(btn::Pattern);
    a.previewOn(3, uint8_t(kFirstPresetInstrument), 60, 127);   // pad: P1 binds a slot
    h.audio.pump(2);
    const int s = a.engine.synth().slotOnPreset(1);
    CHECK(s >= 0);
    a.setCurrentInstrument(kFirstPresetInstrument);
    h.tap(btn::InstrParams);
    CHECK_EQ(a.ui.active(), kModSynth);
    CHECK_EQ(synthScreen(a).slot, s);
    CHECK(synthScreen(a).view == SynthScreen::View::Params);
    char t[24];
    std::snprintf(t, sizeof t, "Synth %d (FAT) P1", s + 1);
    CHECK(drawn(h, t));
    a.previewOff(3);
    // An own note of that slot takes it back: the page shows the slot's own.
    a.previewOn(4, uint8_t(kFirstSynthInstrument + s), 60, 127);
    h.audio.pump(2);
    a.previewOff(4);
    h.audio.pump(2);
    CHECK_EQ(int(a.synthEditTarget(s)), 0);
    CHECK(!drawn(h, t));
}
#endif

}  // namespace
