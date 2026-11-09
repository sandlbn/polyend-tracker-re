// The project's synth presets: 8 patches shared by the slots, read when a project
// opens so a Z never touches the card. presets/<n>.patch in the project folder,
// mirrored in the Workspace; nothing is written while none is used.
#include "test.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "audio/synth_engine.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "model/fx.h"
#include "ui/screens.h"
#include "ui_fakes.h"

using namespace ot;

namespace {
std::string wsPreset(int n) { return "Workspace/presets/" + std::to_string(n) + ".patch"; }
std::string projPreset(const char* name, int n) { return std::string("Projects/") + name + "/presets/" + std::to_string(n) + ".patch"; }
// A Mini model other than ACD that this build has (profiles differ).
int otherModel() {
    for (int m = 0; m < kSynthModelCount; ++m)
        if (SynthModel(m) != SynthModel::ACD && audio::synthEngineFor(synthEngineId(SynthModel(m)))) return m;
    return 0;
}
bool anyUnder(Harness& h, const std::string& prefix) {
    for (const auto& kv : h.fs.files)
        if (kv.first.compare(0, prefix.size(), prefix) == 0) return true;
    return false;
}
}  // namespace

TEST(presets_start_empty_and_new_project_clears) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    for (int n = 1; n <= kSynthPresetCount; ++n) CHECK(!a.synthPresetUsed(n));
    a.storeSynthPreset(1, 0);
    CHECK(a.synthPresetUsed(1));
    CHECK(a.newProject());
    CHECK(!a.synthPresetUsed(1));
    CHECK(!anyUnder(h, "Workspace/presets/"));
}

TEST(store_load_clear_in_memory_and_dirty) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    CHECK(!a.synthPresetDirty());
    a.storeSynthPreset(2, 1);
    CHECK(a.synthPresetDirty());
    CHECK(a.unsavedChanges());
    CHECK_EQ(int(a.synthPreset(2).engine), int(a.synthPatch(1).engine));
    CHECK(!a.synthPresetUsed(0));
    CHECK(!a.synthPresetUsed(9));
    a.clearSynthPreset(2);
    CHECK(!a.synthPresetUsed(2));
    CHECK(!a.loadSynthPreset(0, 2));   // empty
}

TEST(store_and_clear_take_a_slot_off_that_preset) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.storeSynthPreset(1, 0);
    a.engine.synth().presetSelect(0, 1);
    CHECK_EQ(a.synthPresetActive(0), 1);
    a.clearSynthPreset(1);             // a slot must not go on playing an empty entry
    CHECK_EQ(a.synthPresetActive(0), 0);
}

TEST(autosave_writes_only_dirty_presets_and_no_folder_when_unused) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.autosave();
    CHECK(!anyUnder(h, "Workspace/presets/"));
    a.storeSynthPreset(3, 0);
    a.autosave();
    CHECK(!a.synthPresetDirty());
    CHECK(h.fs.files.count(wsPreset(3)) == 1);
    CHECK(h.fs.files.count(wsPreset(4)) == 0);
    a.clearSynthPreset(3);
    a.autosave();
    CHECK(h.fs.files.count(wsPreset(3)) == 0);
}

TEST(save_as_open_and_workspace_load_bring_presets_back) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.storeSynthPreset(1, 0);
    a.storeSynthPreset(8, 1);
    const int32_t e1 = a.synthPreset(1).engine, e8 = a.synthPreset(8).engine;
    CHECK(a.saveProjectAs("Pre"));
    CHECK(h.fs.files.count(projPreset("Pre", 1)) == 1);
    CHECK(h.fs.files.count(projPreset("Pre", 8)) == 1);
    CHECK(h.fs.files[projPreset("Pre", 1)] == h.fs.files[wsPreset(1)]);
    CHECK(a.loadWorkspace());
    CHECK_EQ(int(a.synthPreset(1).engine), int(e1));
    CHECK(a.newProject());
    CHECK(!a.synthPresetUsed(1));
    CHECK(a.openProject("Projects", "Pre"));
    CHECK_EQ(int(a.synthPreset(1).engine), int(e1));
    CHECK_EQ(int(a.synthPreset(8).engine), int(e8));
    CHECK(!a.synthPresetUsed(2));
    CHECK(h.fs.files.count(wsPreset(8)) == 1);
    CHECK(!a.synthPresetDirty());
    // A cleared preset leaves the project on the next save.
    a.clearSynthPreset(8);
    CHECK(a.saveProject());
    CHECK(h.fs.files.count(projPreset("Pre", 8)) == 0);
    CHECK(h.fs.files.count(projPreset("Pre", 1)) == 1);
}

TEST(damaged_preset_file_is_skipped_and_kept_on_save) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.storeSynthPreset(1, 0);
    CHECK(a.saveProjectAs("Dmg"));
    const std::vector<uint8_t> bad(40, 0xFF);
    h.fs.files[projPreset("Dmg", 2)] = bad;
    CHECK(a.openProject("Projects", "Dmg"));
    CHECK(a.synthPresetUsed(1));
    CHECK(!a.synthPresetUsed(2));
    CHECK(a.saveProject());
    CHECK(h.fs.files[projPreset("Dmg", 2)] == bad);   // not ours to delete
}

TEST(load_preset_into_a_slot_takes_its_engine_and_patch) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.setSynthModel(2, otherModel());
    a.storeSynthPreset(1, 2);
    const int32_t eng = a.synthPatch(2).engine;
    CHECK(a.synthPatch(0).engine != eng);
    CHECK(a.loadSynthPreset(0, 1));
    CHECK_EQ(int(a.synthPatch(0).engine), int(eng));
    CHECK_EQ(int(a.synthPatch(0).engine), int(eng));
    CHECK(a.synthPatchDirty(0));
    const SynthSlotRecord rec = a.synthSlot(0);
    CHECK_STR(rec.patchName, "Preset 1");
}

TEST(a_project_without_presets_saves_no_new_data) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    CHECK(a.saveProjectAs("Plain"));
    CHECK(!anyUnder(h, "Projects/Plain/presets/"));
    CHECK(h.fs.files.count("Projects/Plain/project.ot3x") == 0);
}

TEST(orphan_preset_tmp_is_restored_or_dropped) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.storeSynthPreset(1, 0);
    a.autosave();
    // A power cut between writing the .tmp and renaming it: a complete file comes back.
    h.fs.files[wsPreset(1) + ".tmp"] = h.fs.files[wsPreset(1)];
    h.fs.files.erase(wsPreset(1));
    CHECK(a.loadWorkspace());
    CHECK(a.synthPresetUsed(1));
    CHECK(h.fs.files.count(wsPreset(1)) == 1);
    CHECK(h.fs.files.count(wsPreset(1) + ".tmp") == 0);
    // A partial one goes.
    h.fs.files[wsPreset(5) + ".tmp"] = std::vector<uint8_t>(10, 0xFF);
    CHECK(a.loadWorkspace());
    CHECK(!a.synthPresetUsed(5));
    CHECK(h.fs.files.count(wsPreset(5) + ".tmp") == 0);
}

TEST(foreign_engine_preset_survives_open_and_save) {
    // A preset made with a module this build does not have: its file stays byte for byte.
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.storeSynthPreset(1, 0);
    CHECK(a.saveProjectAs("Far"));
    SynthPatch p = a.synthPreset(1);
    p.engine = 0x7F;
    std::vector<uint8_t> bytes(kPatchMaxFileSize);
    size_t n = 0;
    CHECK(encodePatch(p, bytes.data(), bytes.size(), &n) == PatchStatus::kOk);
    bytes.resize(n);
    h.fs.files[projPreset("Far", 4)] = bytes;
    CHECK(a.openProject("Projects", "Far"));
    CHECK(a.saveProject());
    CHECK(h.fs.files[projPreset("Far", 4)] == bytes);
    CHECK(!a.loadSynthPreset(0, 4));   // and it never replaces a slot's patch here
    CHECK(a.synthPatch(0).engine != 0x7F);
}

// --- The Presets page: Synth Macros F8 ------------------------------------------

namespace {
SynthScreen& screen(App& a) { return *static_cast<SynthScreen*>(a.ui.screen(kModSynth)); }
// Slot 1 (0-based) on ACD, on its Presets page.
void openPresets(Harness& h) {
    App& a = *h.app;
    if (a.synthSlot(1).model != uint32_t(SynthModel::ACD)) a.setSynthModel(1, int(SynthModel::ACD));
    a.setCurrentInstrument(kFirstSynthInstrument + 1);
    h.tap(btn::Pattern);
    h.tap(btn::InstrParams);
    h.tap(btn::InstrParams);   // Synth Macros
    h.tap(btn::F8);
}
bool drawn(Harness& h, const char* q) {
    h.app->ui.dirty();
    h.run(40);
    for (const std::string& l : h.disp.last)
        if (l.find(q) != std::string::npos) return true;
    return false;
}
}  // namespace

TEST(presets_view_from_macros_f8_and_back) {
    Harness h;
    App& a = *h.app;
    openPresets(h);
    CHECK(screen(a).view == SynthScreen::View::Presets);
    CHECK(drawn(h, "Synth Presets"));
    CHECK(drawn(h, "P1"));
    CHECK(drawn(h, "P8"));
    CHECK_STR(a.ui.bar.label[0].top, "Store");
    CHECK_STR(a.ui.bar.label[1].top, "Load");
    CHECK_STR(a.ui.bar.label[2].top, "Clear");
    CHECK_STR(a.ui.bar.label[7].top, "Back");
    h.tap(btn::F8);
    CHECK(screen(a).view == SynthScreen::View::Macros);
    h.tap(btn::F8);
    h.tap(btn::InstrParams);
    CHECK(screen(a).view == SynthScreen::View::Params);
}

TEST(store_load_clear_keys_with_confirmations) {
    Harness h;
    App& a = *h.app;
    openPresets(h);
    h.tap(btn::Down);          // P2
    CHECK_EQ(screen(a).row, 1);
    h.tap(btn::F1);            // empty: stores at once
    CHECK(a.synthPresetUsed(2));
    CHECK(drawn(h, "ACD"));
    h.tap(btn::F1);            // used: asks, and No keeps it
    CHECK(a.ui.askOpen());
    h.tap(btn::F7);
    CHECK(!a.ui.askOpen());
    h.tap(btn::F3);            // Clear asks too
    CHECK(a.ui.askOpen());
    h.tap(btn::F7);
    CHECK(a.synthPresetUsed(2));
    h.tap(btn::F3);
    h.tap(btn::F8);            // Yes
    CHECK(!a.synthPresetUsed(2));
    // Load on an empty row does nothing; on a used one the slot takes it.
    h.tap(btn::F2);
    CHECK(!a.ui.askOpen());
    a.setSynthModel(2, otherModel());
    a.storeSynthPreset(3, 2);
    h.tap(btn::Down);          // P3
    const int32_t eng = a.synthPreset(3).engine;
    h.tap(btn::F2);
    CHECK_EQ(int(a.synthPatch(1).engine), int(eng));
}

TEST(preview_keys_switch_while_held_and_restore) {
    Harness h;
    App& a = *h.app;
    a.storeSynthPreset(1, 1);
    openPresets(h);
    h.down(btn::F5);
    CHECK_EQ(a.synthPresetActive(1), 1);
    h.up(btn::F5);
    CHECK_EQ(a.synthPresetActive(1), 0);
    // An empty row previews nothing.
    h.tap(btn::Down);
    h.down(btn::F6);
    CHECK_EQ(a.synthPresetActive(1), 0);
    h.up(btn::F6);
}

TEST(active_preset_marker_and_title_suffix) {
    Harness h;
    App& a = *h.app;
    a.storeSynthPreset(4, 1);
    openPresets(h);
    CHECK(!drawn(h, "* P4"));
    a.engine.synth().presetSelect(1, 4);
    h.audio.pump(1);
    CHECK(drawn(h, "* P4"));
    CHECK(drawn(h, " P4"));
    h.tap(btn::InstrParams);   // Params page
    CHECK(drawn(h, "(ACD) P4"));
}

TEST(unavailable_engine_row_text) {
    Harness h;
    App& a = *h.app;
    a.storeSynthPreset(1, 1);
    CHECK(a.saveProjectAs("Una"));
    SynthPatch p = a.synthPreset(1);
    p.engine = 0x7F;
    std::vector<uint8_t> bytes(kPatchMaxFileSize);
    size_t n = 0;
    CHECK(encodePatch(p, bytes.data(), bytes.size(), &n) == PatchStatus::kOk);
    bytes.resize(n);
    h.fs.files[projPreset("Una", 6)] = bytes;
    CHECK(a.openProject("Projects", "Una"));
    openPresets(h);
    CHECK(drawn(h, "not available"));
}

TEST(a_preset_of_an_absent_module_never_replaces_a_slots_patch) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.storeSynthPreset(1, 0);
    CHECK(a.saveProjectAs("Abs"));
    SynthPatch p = a.synthPreset(1);
    p.engine = 0xF7;   // a module id, not in this build
    std::vector<uint8_t> bytes(kPatchMaxFileSize);
    size_t n = 0;
    CHECK(encodePatch(p, bytes.data(), bytes.size(), &n) == PatchStatus::kOk);
    bytes.resize(n);
    h.fs.files[projPreset("Abs", 2)] = bytes;
    CHECK(a.openProject("Projects", "Abs"));
    const int32_t was = a.synthPatch(0).engine;
    CHECK(!a.loadSynthPreset(0, 2));
    CHECK_EQ(int(a.synthPatch(0).engine), int(was));
}

TEST(held_preview_ends_with_the_page_and_blocks_other_keys) {
    Harness h;
    App& a = *h.app;
    a.storeSynthPreset(1, 1);
    openPresets(h);
    h.down(btn::F5);
    CHECK_EQ(a.synthPresetActive(1), 1);
    h.tap(btn::F3);            // no question may swallow the F5 release
    CHECK(!a.ui.askOpen());
    h.tap(btn::F8);            // leaving the page ends the preview
    CHECK(screen(a).view == SynthScreen::View::Macros);
    CHECK_EQ(a.synthPresetActive(1), 0);
    h.up(btn::F5);
    CHECK_EQ(a.synthPresetActive(1), 0);
    h.tap(btn::F8);
    h.down(btn::F6);
    CHECK_EQ(a.synthPresetActive(1), 1);
    h.tap(btn::InstrParams);
    CHECK(screen(a).view == SynthScreen::View::Params);
    CHECK_EQ(a.synthPresetActive(1), 0);
    h.up(btn::F6);
}

TEST(held_preview_does_not_undo_a_stop_or_a_z) {
    Harness h;
    App& a = *h.app;
    a.storeSynthPreset(1, 1);
    a.storeSynthPreset(2, 1);
    a.engine.synth().presetSelect(1, 2);
    openPresets(h);
    h.down(btn::F5);           // P1, over the Z's P2
    CHECK_EQ(a.synthPresetActive(1), 1);
    a.engine.synth().transportStop();
    CHECK_EQ(a.synthPresetActive(1), 0);
    h.up(btn::F5);             // not back on P2: Stop ended that
    CHECK_EQ(a.synthPresetActive(1), 0);
    h.down(btn::F5);
    a.engine.synth().presetSelect(1, 2);   // a Z during the hold stays
    h.up(btn::F5);
    CHECK_EQ(a.synthPresetActive(1), 2);
}

// --- The slot mix a preset carries (gain, pan, reverb / delay send) ------------

namespace {
bool hasField17(const std::vector<uint8_t>& b) {
    size_t beg = 0, end = 0;
    return patchFieldSpan(b.data(), b.size(), kPatchMixField, &beg, &end);
}
SynthSlotRecord mixed(App& a, int s, float gain, float pan, float reverb, float delay) {
    SynthSlotRecord r = a.synthSlot(s);
    r.gainDb = gain;
    r.pan = pan;
    r.reverbSend = reverb;
    r.delaySend = delay;
    a.setSynthSlotRecord(s, r);
    return r;
}
std::vector<uint8_t> encoded(const SynthPatch& p) {
    std::vector<uint8_t> b(kPatchMaxFileSize);
    size_t n = 0;
    CHECK(encodePatch(p, b.data(), b.size(), &n) == PatchStatus::kOk);
    b.resize(n);
    return b;
}
}  // namespace

TEST(store_captures_the_slot_mix_and_z_leaves_the_record) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    mixed(a, 0, -4.5f, 30.0f, 20.0f, 70.0f);
    a.storeSynthPreset(1, 0);
    const SynthPatch& p = a.synthPreset(1);
    CHECK(patchHasMix(p));
    CHECK(patchMix(p, kPatchMixGain) == -4.5f);
    CHECK(patchMix(p, kPatchMixPan) == 30.0f);
    CHECK(patchMix(p, kPatchMixReverb) == 20.0f);
    CHECK(patchMix(p, kPatchMixDelay) == 70.0f);
    CHECK_EQ(int(a.synthPatch(0).mixCount), 0);   // the slot's own patch carries none
    // On the preset, the Mixer and Synth pages keep the slot's own values.
    mixed(a, 1, -12.0f, -50.0f, 0.0f, 0.0f);
    CHECK(a.engine.synth().presetSelect(1, 1));
    CHECK(a.synthSlot(1).gainDb == -12.0f);
    CHECK(a.synthSlot(1).pan == -50.0f);
}

TEST(load_copies_the_mix_into_the_record_and_dirties_the_project) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    mixed(a, 0, -4.5f, 30.0f, 20.0f, 70.0f);
    a.storeSynthPreset(1, 0);
    mixed(a, 1, -12.0f, -50.0f, 5.0f, 0.0f);
    CHECK(a.saveProjectAs("Ld"));
    CHECK(!a.unsavedChanges());
    const uint32_t model = a.synthSlot(1).model, poly = a.synthSlot(1).polyphony;
    CHECK(a.loadSynthPreset(1, 1));
    const SynthSlotRecord r = a.synthSlot(1);
    CHECK(r.gainDb == -4.5f);
    CHECK(r.pan == 30.0f);
    CHECK(r.reverbSend == 20.0f);
    CHECK(r.delaySend == 70.0f);
    CHECK_EQ(r.polyphony, poly);   // polyphony and model stay the slot's (same engine here)
    CHECK_EQ(r.model, model);
    CHECK(a.unsavedChanges());
    CHECK_EQ(int(a.synthPatch(1).mixCount), 0);
    // Neither a library save nor the slot's own patch file carries the mix.
    CHECK(a.saveSynthLibraryPatch(1, "FromPreset"));
    CHECK(h.fs.files.count("Patches/ACD/FromPreset.patch") == 1);
    CHECK(!hasField17(h.fs.files["Patches/ACD/FromPreset.patch"]));
    CHECK(a.saveProject());
    CHECK(h.fs.files.count("Projects/Ld/1.patch") == 1);
    CHECK(h.fs.files.count("Workspace/1.patch") == 1);
    CHECK(!hasField17(h.fs.files["Projects/Ld/1.patch"]));
    CHECK(!hasField17(h.fs.files["Workspace/1.patch"]));
    CHECK(hasField17(h.fs.files[projPreset("Ld", 1)]));
}

TEST(a_mix_out_of_the_record_range_loads_held_at_the_limits) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.storeSynthPreset(2, 1);
    SynthPatch p = a.synthPreset(2);
    patchSetMix(p, 40.0f, -300.0f, -1.0f, 150.0f);
    CHECK(a.saveProjectAs("Rg"));
    h.fs.files[projPreset("Rg", 2)] = encoded(p);
    CHECK(a.openProject("Projects", "Rg"));
    mixed(a, 1, -9.0f, 0.0f, 33.0f, 0.0f);
    CHECK(a.loadSynthPreset(1, 2));
    const SynthSlotRecord r = a.synthSlot(1);
    CHECK(r.valid());
    CHECK(r.gainDb == 12.0f);
    CHECK(r.pan == -100.0f);
    CHECK(r.reverbSend == 0.0f);
    CHECK(r.delaySend == 100.0f);
    // A value that is not a number: no mix at all, the slot keeps its own.
    patchSetMix(p, -3.0f, 10.0f, NAN, 20.0f);
    h.fs.files[projPreset("Rg", 2)] = encoded(p);
    CHECK(a.openProject("Projects", "Rg"));
    CHECK(!patchHasMix(a.synthPreset(2)));
    const SynthSlotRecord own = mixed(a, 1, -9.0f, 0.0f, 33.0f, 0.0f);
    CHECK(a.loadSynthPreset(1, 2));
    CHECK(a.synthSlot(1).gainDb == own.gainDb && a.synthSlot(1).reverbSend == own.reverbSend);
}

TEST(preset_files_keep_the_mix_through_save_and_open) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    mixed(a, 2, -1.5f, -20.0f, 40.0f, 10.0f);
    a.storeSynthPreset(3, 2);
    CHECK(a.saveProjectAs("Mx"));
    CHECK(hasField17(h.fs.files[projPreset("Mx", 3)]));
    CHECK(a.newProject());
    CHECK(!a.synthPresetUsed(3));
    CHECK(a.openProject("Projects", "Mx"));
    const SynthPatch& p = a.synthPreset(3);
    CHECK(patchHasMix(p));
    CHECK(patchMix(p, kPatchMixGain) == -1.5f);
    CHECK(patchMix(p, kPatchMixDelay) == 10.0f);
    // Clear drops it with the entry.
    a.clearSynthPreset(3);
    CHECK_EQ(int(a.synthPreset(3).mixCount), 0);
}

TEST(a_preset_saved_without_a_mix_keeps_the_slots_on_load) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.storeSynthPreset(1, 1);
    CHECK(a.saveProjectAs("Old"));
    SynthPatch p = a.synthPreset(1);
    p.mixCount = 0;   // as a build before the mix wrote it
    const std::vector<uint8_t> bytes = encoded(p);
    h.fs.files[projPreset("Old", 1)] = bytes;
    CHECK(a.openProject("Projects", "Old"));
    CHECK(!patchHasMix(a.synthPreset(1)));
    const SynthSlotRecord before = mixed(a, 1, -20.0f, 60.0f, 15.0f, 25.0f);
    CHECK(a.loadSynthPreset(1, 1));
    const SynthSlotRecord r = a.synthSlot(1);
    CHECK(r.gainDb == before.gainDb && r.pan == before.pan);
    CHECK(r.reverbSend == before.reverbSend && r.delaySend == before.delaySend);
    // A fader move does not take a slot off a preset without a mix.
    CHECK(a.engine.synth().presetSelect(1, 1));
    mixed(a, 1, -3.0f, 60.0f, 15.0f, 25.0f);
    CHECK_EQ(a.synthPresetActive(1), 1);
    // The file stays as it was when nothing changed it.
    CHECK(a.saveProject());
    CHECK(h.fs.files[projPreset("Old", 1)] == bytes);
}

TEST(a_slot_patch_file_with_a_mix_is_read_without_it) {
    // A preset file copied over a slot's patch or into the library: the slot
    // takes the patch, never the mix.
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    mixed(a, 1, -4.0f, 10.0f, 0.0f, 0.0f);
    a.storeSynthPreset(1, 1);
    CHECK(a.saveProjectAs("Cp"));
    const std::vector<uint8_t> pre = h.fs.files[projPreset("Cp", 1)];
    CHECK(hasField17(pre));
    h.fs.files["Projects/Cp/1.patch"] = pre;
    CHECK(a.openProject("Projects", "Cp"));
    CHECK_EQ(int(a.synthPatch(1).mixCount), 0);
    h.fs.dirs.insert("Patches");
    h.fs.dirs.insert("Patches/ACD");
    h.fs.files["Patches/ACD/Copied.patch"] = pre;
    CHECK(a.loadSynthLibraryPatch(1, "Copied"));
    CHECK_EQ(int(a.synthPatch(1).mixCount), 0);
    CHECK(a.previewSynthLibraryPatch(1, "Copied"));
    CHECK_EQ(int(a.synthPatch(1).mixCount), 0);
    a.endSynthPreview();
    CHECK(a.saveProject());
    CHECK(h.fs.files.count("Projects/Cp/1.patch") == 1);
    CHECK(!hasField17(h.fs.files["Projects/Cp/1.patch"]));
}

TEST(a_slot_patch_file_with_a_mix_is_written_again_without_it) {
    // Opened with no other edit: the next save drops field 17 from the slot's
    // files too.
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    mixed(a, 1, -4.0f, 10.0f, 0.0f, 0.0f);
    a.storeSynthPreset(1, 1);
    CHECK(a.saveProjectAs("Cq"));
    const std::vector<uint8_t> pre = h.fs.files[projPreset("Cq", 1)];
    CHECK(hasField17(pre));
    h.fs.files["Projects/Cq/1.patch"] = pre;
    CHECK(a.openProject("Projects", "Cq"));
    CHECK(hasField17(h.fs.files["Workspace/1.patch"]));   // the copy Open made
    CHECK(a.synthPatchDirty(1));
    CHECK(!a.synthPatchDirty(0));
    CHECK(a.saveProject());
    CHECK(!hasField17(h.fs.files["Workspace/1.patch"]));
    CHECK(!hasField17(h.fs.files["Projects/Cq/1.patch"]));
    CHECK(hasField17(h.fs.files[projPreset("Cq", 1)]));
}

TEST(presets_page_shows_the_gain_a_preset_carries) {
    Harness h;
    App& a = *h.app;
    mixed(a, 1, -4.5f, 0.0f, 0.0f, 0.0f);
    a.storeSynthPreset(2, 1);
    openPresets(h);
    CHECK(drawn(h, "P2  ACD  -4.50 dB"));
    // One saved without a mix shows none.
    SynthPatch p = a.synthPreset(2);
    p.mixCount = 0;
    CHECK(a.saveProjectAs("Pg"));
    h.fs.files[projPreset("Pg", 2)] = encoded(p);
    CHECK(a.openProject("Projects", "Pg"));
    openPresets(h);
    CHECK(!drawn(h, "dB"));
    CHECK(drawn(h, "P2  ACD"));
}

// --- Edit what you hear: a slot on a preset edits that preset -----------------

namespace {
// Slot 1 on ACD named "Lead" at polyphony 3 and a mix; preset 1 stored from
// slot 2 on another model; slot 1 on preset 1.
void onPreset(App& a) {
    CHECK(a.newProject());
    SynthSlotRecord r = a.synthSlot(2);
    a.setSynthModel(2, otherModel());
    r = a.synthSlot(2);
    std::snprintf(r.patchName, sizeof(r.patchName), "Bell");
    r.polyphony = 3;
    a.setSynthSlotRecord(2, r);
    mixed(a, 2, -6.0f, 20.0f, 30.0f, 40.0f);
    a.storeSynthPreset(1, 2);
    mixed(a, 1, -12.0f, 0.0f, 0.0f, 0.0f);
    a.autosave();
    CHECK(a.engine.synth().presetSelect(1, 1));
}
bool hasField(const std::vector<uint8_t>& b, uint32_t f) {
    size_t beg = 0, end = 0;
    return patchFieldSpan(b.data(), b.size(), f, &beg, &end);
}
}  // namespace

TEST(store_carries_the_name_and_polyphony_and_load_takes_the_polyphony) {
    Harness h;
    App& a = *h.app;
    onPreset(a);
    const SynthPatch& p = a.synthPreset(1);
    CHECK_STR(p.presetName, "Bell");
    CHECK(patchHasPoly(p));
    CHECK_EQ(int(p.poly), 3);
    CHECK(a.saveProjectAs("Nm"));
    const std::vector<uint8_t> f = h.fs.files[projPreset("Nm", 1)];
    CHECK(hasField(f, kPatchMixField) && hasField(f, kPatchPresetNameField) && hasField(f, kPatchPolyField));
    for (int s = 0; s < kSynthSlotCount; ++s) {   // never in the slot's own patch
        const std::vector<uint8_t> own = h.fs.files["Projects/Nm/" + std::to_string(s + 1) + ".patch"];
        CHECK(!hasField(own, kPatchPresetNameField) && !hasField(own, kPatchPolyField));
    }
    CHECK(a.openProject("Projects", "Nm"));
    CHECK_STR(a.synthPreset(1).presetName, "Bell");
    CHECK_EQ(int(a.synthPreset(1).poly), 3);
    CHECK(a.loadSynthPreset(0, 1));
    CHECK_EQ(int(a.synthSlot(0).polyphony), 3);
    CHECK_EQ(int(a.synthPatch(0).presetNameLen), 0);
    CHECK(!patchHasPoly(a.synthPatch(0)));
    // A preset saved without a polyphony keeps the slot's on load.
    SynthPatch q = a.synthPreset(1);
    q.polyPresent = false;
    CHECK(a.saveProjectAs("Np"));
    h.fs.files[projPreset("Np", 1)] = encoded(q);
    CHECK(a.openProject("Projects", "Np"));
    SynthSlotRecord r = a.synthSlot(0);
    r.polyphony = 5;
    a.setSynthSlotRecord(0, r);
    CHECK(a.loadSynthPreset(0, 1));
    CHECK_EQ(int(a.synthSlot(0).polyphony), 5);
}

TEST(the_target_is_the_preset_while_the_slot_is_on_it) {
    Harness h;
    App& a = *h.app;
    onPreset(a);
    CHECK_EQ(a.synthEditTarget(1), 1);
    CHECK(&a.synthTargetPatch(1) == &a.synthPreset(1));
    const SynthSlotRecord t = a.synthTargetRecord(1);
    CHECK_EQ(int(t.model), otherModel());
    CHECK_STR(t.patchName, "Bell");
    CHECK_EQ(int(t.polyphony), 3);
    CHECK(t.gainDb == -6.0f && t.pan == 20.0f && t.reverbSend == 30.0f && t.delaySend == 40.0f);
    CHECK_EQ(a.synthEditTarget(0), 0);   // the other slots: their own
    a.engine.synth().presetSelect(1, 0);   // Z0
    CHECK_EQ(a.synthEditTarget(1), 0);
    CHECK(&a.synthTargetPatch(1) == &a.synthPatch(1));
    CHECK(a.synthTargetRecord(1).gainDb == -12.0f);
    CHECK_EQ(int(a.synthTargetRecord(1).model), int(SynthModel::ACD));
    a.engine.synth().presetSelect(1, 1);
    a.engine.synth().transportStop();   // Stop
    CHECK_EQ(a.synthEditTarget(1), 0);
}

TEST(a_mix_or_polyphony_edit_on_the_target_changes_the_preset) {
    Harness h;
    App& a = *h.app;
    onPreset(a);
    CHECK(!a.synthPresetDirty());
    SynthSlotRecord t = a.synthTargetRecord(1);
    t.gainDb = -1.5f;
    t.polyphony = 2;
    t.macros[0] = 77;
    a.setSynthTargetRecord(1, t);
    CHECK(patchMix(a.synthPreset(1), kPatchMixGain) == -1.5f);
    CHECK_EQ(int(a.synthPreset(1).poly), 2);
    CHECK(a.synthSlot(1).gainDb == -12.0f);   // the record keeps its own
    CHECK_EQ(int(a.synthSlot(1).polyphony), 1);
    CHECK_EQ(int(a.synthSlot(1).macros[0]), 77);   // macro values are the slot's
    CHECK(a.synthPresetDirty());
    CHECK_EQ(a.synthPresetActive(1), 1);
    a.autosave();
    SynthPatch w;
    const std::vector<uint8_t>& f = h.fs.files[wsPreset(1)];
    CHECK(decodePatch(f.data(), f.size(), &w) == PatchStatus::kOk);
    CHECK(patchMix(w, kPatchMixGain) == -1.5f);
    CHECK_EQ(int(w.poly), 2);
    // A preset without a mix takes the whole mix at the first edit.
    a.engine.synth().presetSelect(1, 0);
    a.storeSynthPreset(2, 1);
    SynthPatch& e = const_cast<SynthPatch&>(a.synthPreset(2));
    patchStripPreset(e);
    a.engine.synth().presetSelect(1, 2);
    t = a.synthTargetRecord(1);
    CHECK(t.gainDb == -12.0f);
    CHECK_EQ(int(t.polyphony), 1);
    t.pan = 50.0f;
    a.setSynthTargetRecord(1, t);
    CHECK(patchHasMix(a.synthPreset(2)));
    CHECK(patchMix(a.synthPreset(2), kPatchMixGain) == -12.0f);
    CHECK(patchMix(a.synthPreset(2), kPatchMixPan) == 50.0f);
    CHECK(!patchHasPoly(a.synthPreset(2)));
    CHECK(a.synthSlot(1).pan == 0.0f);
}

TEST(a_value_edit_on_the_target_changes_the_preset_and_is_saved) {
    Harness h;
    App& a = *h.app;
    onPreset(a);
    const std::vector<uint8_t> ownBefore = h.fs.files["Workspace/2.patch"];
    SynthPatch& p = a.synthTargetPatch(1);
    const int32_t eng = p.engine;
    const PatchParamRange* r = audio::synthParamRange(eng, 0);
    CHECK(r != nullptr);
    const float v = audio::synthParamGet(p, 0) == r->max ? r->min : r->max;
    CHECK(audio::synthParamSet(p, 0, v));
    a.markSynthTargetDirty(1);
    CHECK_EQ(a.synthPresetActive(1), 1);   // still on it
    CHECK(!a.synthPatchDirty(1));
    a.autosave();
    SynthPatch w;
    const std::vector<uint8_t>& f = h.fs.files[wsPreset(1)];
    CHECK(decodePatch(f.data(), f.size(), &w) == PatchStatus::kOk);
    CHECK(audio::synthParamGet(w, 0) == v);
    CHECK_STR(w.presetName, "Bell");
    CHECK(h.fs.files["Workspace/2.patch"] == ownBefore);
}

TEST(model_init_and_library_load_on_the_target_replace_the_presets_sound) {
    Harness h;
    App& a = *h.app;
    onPreset(a);
    SynthSlotRecord r = a.synthSlot(1);
    r.macros[0] = 99;
    a.setSynthSlotRecord(1, r);
    // A model change: the Init patch of the new model, in the preset.
    a.setSynthTargetModel(1, int(SynthModel::ACD));
    CHECK_EQ(int(a.synthPreset(1).engine), int(kSynthEngineAcd));
    CHECK_STR(a.synthPreset(1).presetName, "Init");
    CHECK(patchMix(a.synthPreset(1), kPatchMixGain) == -6.0f);   // the mix and polyphony stay
    CHECK_EQ(int(a.synthPreset(1).poly), 3);
    CHECK_EQ(a.synthPresetActive(1), 1);
    CHECK_EQ(int(a.synthSlot(1).macros[0]), int(audio::synthMacroDefault(a.synthPreset(1).macros[0].bipolar)));
    CHECK_EQ(int(a.synthSlot(1).model), int(SynthModel::ACD));
    // Init Patch.
    audio::synthParamSet(a.synthTargetPatch(1), 0, audio::synthParamRange(kSynthEngineAcd, 0)->max);
    a.initSynthTargetPatch(1);
    SynthPatch init;
    CHECK(patchInit(kSynthEngineAcd, &init));
    CHECK(audio::synthParamGet(a.synthPreset(1), 0) == audio::synthParamGet(init, 0));
    // A library patch, saved from the target: no preset fields in the file,
    // and the preset takes the name.
    CHECK(a.saveSynthTargetLibraryPatch(1, "Keep"));
    const std::vector<uint8_t> lib = h.fs.files["Patches/ACD/Keep.patch"];
    CHECK(!lib.empty());
    CHECK(!hasField(lib, kPatchMixField) && !hasField(lib, kPatchPresetNameField) && !hasField(lib, kPatchPolyField));
    CHECK_STR(a.synthPreset(1).presetName, "Keep");
    audio::synthParamSet(a.synthTargetPatch(1), 0, audio::synthParamRange(kSynthEngineAcd, 0)->max);
    CHECK(a.loadSynthTargetLibraryPatch(1, "Keep"));
    CHECK(audio::synthParamGet(a.synthPreset(1), 0) == audio::synthParamGet(init, 0));
    CHECK_EQ(a.synthPresetActive(1), 1);
}

TEST(a_preview_on_the_target_puts_the_preset_back_byte_for_byte) {
    Harness h;
    App& a = *h.app;
    onPreset(a);
    a.autosave();
    const int m = otherModel();
    SynthPatch lib;
    CHECK(patchInit(synthEngineId(SynthModel(m)), &lib));
    audio::synthParamSet(lib, 0, audio::synthParamRange(lib.engine, 0)->max);
    h.fs.dirs.insert("Patches");
    h.fs.dirs.insert(std::string("Patches/") + synthModelName(SynthModel(m)));
    h.fs.files[std::string("Patches/") + synthModelName(SynthModel(m)) + "/Try.patch"] = encoded(lib);
    const SynthPatch before = a.synthPreset(1);
    CHECK(a.previewSynthLibraryPatch(1, "Try"));
    CHECK_EQ(a.synthPresetActive(1), 1);
    CHECK(audio::synthParamGet(a.synthPreset(1), 0) == audio::synthParamGet(lib, 0));
    CHECK(patchMix(a.synthPreset(1), kPatchMixGain) == -6.0f);
    CHECK(!a.synthPresetDirty());
    a.endSynthPreview();
    CHECK(std::memcmp(&before, &a.synthPreset(1), sizeof(SynthPatch)) == 0);
    CHECK_EQ(a.synthPresetActive(1), 1);
    CHECK(!a.synthPresetDirty());
}

TEST(store_from_a_slot_on_a_preset_stores_what_it_plays) {
    Harness h;
    App& a = *h.app;
    onPreset(a);
    a.storeSynthPreset(2, 1);   // slot 1 plays preset 1
    CHECK_EQ(int(a.synthPreset(2).engine), int(a.synthPreset(1).engine));
    CHECK_STR(a.synthPreset(2).presetName, "Bell");
    CHECK(patchMix(a.synthPreset(2), kPatchMixGain) == -6.0f);
    CHECK_EQ(int(a.synthPreset(2).poly), 3);
    const SynthPatch before = a.synthPreset(1);
    a.storeSynthPreset(1, 1);   // onto itself: nothing
    CHECK(std::memcmp(&before, &a.synthPreset(1), sizeof(SynthPatch)) == 0);
    CHECK_EQ(a.synthPresetActive(1), 1);
}

TEST(an_edit_resolved_before_a_z0_or_stop_still_goes_to_the_sound_it_read) {
    Harness h;
    App& a = *h.app;
    onPreset(a);
    const int n = a.synthEditTarget(1);
    CHECK_EQ(n, 1);
    SynthSlotRecord t = a.synthTargetRecord(1, n);
    t.gainDb = -2.5f;
    a.engine.synth().transportStop();   // between the read and the write
    CHECK_EQ(a.synthEditTarget(1), 0);
    const SynthSlotRecord own = a.synthSlot(1);
    a.setSynthTargetRecord(1, n, t);
    CHECK(patchMix(a.synthPreset(1), kPatchMixGain) == -2.5f);
    CHECK(a.synthSlot(1).gainDb == own.gainDb);
    CHECK(&a.synthTargetPatch(1, n) == &a.synthPreset(1));
    a.markSynthTargetDirty(1, n);
    CHECK(!a.synthPatchDirty(1));
    // A preset emptied since: the edit is dropped.
    a.clearSynthPreset(1);
    t.gainDb = -1.0f;
    a.setSynthTargetRecord(1, n, t);
    CHECK(a.synthSlot(1).gainDb == own.gainDb);
    CHECK(&a.synthTargetPatch(1, n) == &a.synthPatch(1));
    // The own target never takes a model or a name from the edited record.
    SynthSlotRecord o = a.synthTargetRecord(1, 0);
    o.model = uint32_t(otherModel());
    std::snprintf(o.patchName, sizeof(o.patchName), "Other");
    o.pan = 33.0f;
    a.setSynthTargetRecord(1, 0, o);
    const SynthSlotRecord after = a.synthSlot(1);
    CHECK_EQ(int(after.model), int(SynthModel::ACD));
    CHECK_STR(after.patchName, own.patchName);
    CHECK(after.pan == 33.0f);
}

TEST(a_mix_that_is_not_a_number_never_reaches_the_record) {
    Harness h;
    App& a = *h.app;
    onPreset(a);
    SynthPatch& e = a.synthTargetPatch(1, 1);
    e.mix[kPatchMixGain].bits = 0x7FC00000u;   // NaN
    CHECK(!patchHasMix(e));
    const SynthSlotRecord t = a.synthTargetRecord(1, 1);
    CHECK(std::isfinite(t.gainDb) && std::isfinite(t.pan));
    CHECK(t.gainDb == a.synthSlot(1).gainDb);
    CHECK(a.loadSynthPreset(0, 1));
    CHECK(std::isfinite(a.synthSlot(0).gainDb));
}

TEST(load_names_the_slot_after_the_preset) {
    Harness h;
    App& a = *h.app;
    onPreset(a);   // P1 "Bell"
    CHECK(a.loadSynthPreset(0, 1));
    const SynthSlotRecord r = a.synthSlot(0);
    CHECK_STR(r.patchName, "Bell");
}

TEST(a_new_project_drops_an_open_patch_preview) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    CHECK(a.saveSynthLibraryPatch(0, "Own"));
    CHECK(a.previewSynthLibraryPatch(0, "Own"));
    CHECK_EQ(a.synthPreviewSlot(), 0);
    CHECK(a.newProject());
    CHECK_EQ(a.synthPreviewSlot(), -1);
    const SynthPatch now = a.synthPatch(0);
    a.endSynthPreview();   // nothing of the old project is put back
    CHECK(std::memcmp(&now, &a.synthPatch(0), sizeof now) == 0);
}
