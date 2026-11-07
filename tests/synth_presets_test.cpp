// The project's synth presets: 8 patches shared by the slots, read when a project
// opens so a Z never touches the card. presets/<n>.patch in the project folder,
// mirrored in the Workspace; nothing is written while none is used.
#include "test.h"

#include <cstring>
#include <string>
#include <vector>

#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "ui_fakes.h"

using namespace ot;

namespace {
std::string wsPreset(int n) { return "Workspace/presets/" + std::to_string(n) + ".patch"; }
std::string projPreset(const char* name, int n) { return std::string("Projects/") + name + "/presets/" + std::to_string(n) + ".patch"; }
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
    a.storeSynthPreset(1, 2);   // slot 3: VAP by default
    const int32_t eng = a.synthPatch(2).engine;
    CHECK(a.synthPatch(0).engine != eng);
    CHECK(a.loadSynthPreset(0, 1));
    CHECK_EQ(int(a.slotEngineId(0)), int(eng));
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
