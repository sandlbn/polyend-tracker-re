// A synth engine from a module with its own engine id (0x80..0xFF): it joins
// the Synth page model list after the five Mini models, a slot plays it while
// its record keeps a Mini model, its patches live in Patches/<NAME>/, and the
// project sidecar carries the slot's engine through autosave, Save As and Open
// (docs/PLAN_3X_M2.md M-A / M-B). An engine with a display name ("T-Disp")
// shows it on the Synth page and keeps its descriptor name as the folder.
#include "test.h"

#include <cstring>

#include "audio/synth_engine.h"
#include "formats/patch_pb.h"
#include "modules/module.h"
#include "modules/synth_descriptor.h"
#include "modules/synth_models.h"
#include "ui_fakes.h"

using namespace ot;

namespace {

constexpr int32_t kTestEng = 0xF0;   // a test-only id (0x80 FM4S, 0x81 JU6S are real modules)
const PatchParamRange kParams[3] = {{0.5f, 0, 1}, {1000, 20, 20000}, {1, 0, 2}};
const synthui::ParamUi kUi[3] = {{"Chorus", synthui::kFmtPct, nullptr, 0},
                                 {"Cutoff", synthui::kFmtHz, nullptr, 0},
                                 {"Volume", synthui::kFmtPct, nullptr, 0}};
const uint8_t kMain[] = {0, 1, 2};
const synthui::Section kSections[] = {OT_SYNTH_SECTION("Main", "Main", kMain), OT_SYNTH_MACRO_SECTIONS};
const mod::SynthDescriptor kDesc = {kTestEng, "TENG", 0, 2, 3, kParams, kUi, kSections, 8, 0, nullptr};

class EngineModule final : public mod::IModule {
public:
    const mod::ModuleInfo& info() const override {
        static const mod::ModuleInfo i = {"TJU1", mod::kModuleApi, 100, "Test engine",
                                          mod::kCapSynthEngine | mod::kCapSynthDescriptor, 0, 0, 0, 0, 8};
        return i;
    }
    bool attach(mod::ModuleHost&) override { return true; }
    audio::ISynthEngine* synthEngine(int32_t id) override { return id == kTestEng ? audio::synthTestEngine() : nullptr; }
    const mod::SynthDescriptor* synthDescriptor(int i) const override { return i == 0 ? &kDesc : nullptr; }
};
EngineModule g_mod;

// A second test engine with a display name ('-' and lower case: not a folder name).
constexpr int32_t kDisp = 0xF2;
const mod::SynthDescriptor kDispDesc = {kDisp, "TDISP", 0, 2, 3, kParams, kUi, kSections, 8, 0, nullptr, "T-Disp"};
class DispModule final : public mod::IModule {
public:
    const mod::ModuleInfo& info() const override {
        static const mod::ModuleInfo i = {"TDN1", mod::kModuleApi, 100, "Test display name",
                                          mod::kCapSynthEngine | mod::kCapSynthDescriptor, 0, 0, 0, 0, 8};
        return i;
    }
    bool attach(mod::ModuleHost&) override { return true; }
    audio::ISynthEngine* synthEngine(int32_t id) override { return id == kDisp ? audio::synthTestEngine() : nullptr; }
    const mod::SynthDescriptor* synthDescriptor(int i) const override { return i == 0 ? &kDispDesc : nullptr; }
};
DispModule g_disp;

void reg() {
    static bool done = false;
    if (!done) done = mod::ModuleHost::instance().addForTest(g_mod) && mod::ModuleHost::instance().addForTest(g_disp);
    CHECK(done);
}

}  // namespace

TEST(module_engine_in_model_list) {
    reg();
    const int n = synthModelListCount();
    CHECK(n >= 6);
    for (int i = 0; i < 5; ++i) CHECK_EQ(synthModelListEngine(i), synthEngineId(SynthModel(i)));
    const int k = synthModelListIndex(kTestEng);
    CHECK(k >= 5);
    CHECK_STR(synthModelListName(k), "TENG");
    CHECK_STR(synthEngineFolder(kTestEng), "TENG");
    SynthPatch p;
    CHECK(patchInit(kTestEng, &p));
    CHECK_EQ(int(p.paramCount), 3);
    CHECK_EQ(p.macros[kPatchMacroVelocity].targets[0].param, 2);   // Volume
}

TEST(module_engine_slot_round_trip) {
    reg();
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    const uint32_t mini = a.synthSlot(1).model;
    a.setSynthEngine(1, kTestEng);
    CHECK_EQ(a.slotEngineId(1), kTestEng);
    CHECK_EQ(a.synthSlot(1).model, mini);   // the record keeps its Mini model (fallback)
    CHECK_EQ(a.synthPatch(1).engine, kTestEng);
    h.run(5);
    CHECK(a.engine.synth().slotAvailable(1));
    CHECK_STR(a.engine.synth().slotView(1).model, "TENG");
    // Library folder of the engine.
    CHECK(a.saveSynthLibraryPatch(1, "Mine"));
    CHECK(h.fs.files.count("Patches/TENG/Mine.patch") == 1);
    // Sidecar: autosave, Save As, New, Open.
    a.autosave();
    CHECK(h.fs.files.count("Workspace/project.ot3x") == 1);
    CHECK(a.saveProjectAs("Teng"));
    CHECK(h.fs.files.count("Projects/Teng/project.ot3x") == 1);
    CHECK(a.newProject());
    CHECK(a.slotEngineId(1) != kTestEng);
    CHECK(a.openProject("Projects", "Teng"));
    CHECK_EQ(a.slotEngineId(1), kTestEng);
    CHECK_EQ(a.synthPatch(1).engine, kTestEng);   // 1.patch of engine 0xF0 accepted
    // Back to a Mini model: the sidecar has nothing left to say.
    a.setSynthModel(1, int(SynthModel::VAP));
    CHECK_EQ(a.slotEngineId(1), int32_t(kSynthEngineVap));
    a.autosave();
    CHECK(h.fs.files.count("Workspace/project.ot3x") == 0);
}

TEST(module_engine_on_the_synth_page) {
    reg();
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.setSynthEngine(0, kTestEng);
    a.setCurrentInstrument(64);
    h.tap(btn::Pattern);
    h.tap(btn::InstrParams);
    h.run(5);
    CHECK(h.disp.lastHasText("Synth 1 (TENG)"));
    CHECK(h.disp.lastHasText("TENG"));
    // F1 Model moves along the list: from TENG to the next entry (wraps).
    h.tap(btn::F1);
    h.tap(btn::F1);
    h.run(5);
    CHECK(a.slotEngineId(0) != kTestEng);
}

TEST(module_engine_display_name) {
    reg();
    CHECK(kDesc.displayName == nullptr);   // an aggregate without it: the name is shown
    const int k = synthModelListIndex(kDisp);
    CHECK(k >= 5);
    CHECK_STR(synthModelListName(k), "T-Disp");
    CHECK_STR(synthModelListName(synthModelListIndex(kTestEng)), "TENG");
    CHECK_STR(synthEngineFolder(kDisp), "TDISP");
    CHECK_STR(synthEngineDisplayName(kDisp), "T-Disp");
    CHECK_STR(synthEngineDisplayName(kTestEng), "TENG");
    CHECK_STR(synthEngineDisplayName(synthEngineId(SynthModel::VAP)), synthModelName(SynthModel::VAP));
    CHECK_STR(synthEngineDisplayName(0x7D), "?");
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.setSynthEngine(0, kDisp);
    h.run(5);
    CHECK_STR(a.engine.synth().slotView(0).model, "T-Disp");
    // The library folder stays the descriptor name.
    CHECK(a.saveSynthLibraryPatch(0, "Mine"));
    CHECK(h.fs.files.count("Patches/TDISP/Mine.patch") == 1);
    a.setCurrentInstrument(64);
    h.tap(btn::Pattern);
    h.tap(btn::InstrParams);
    h.run(5);
    CHECK(h.disp.lastHasText("Synth 1 (T-Disp)"));
    CHECK(!h.disp.lastHasText("TDISP"));
}
