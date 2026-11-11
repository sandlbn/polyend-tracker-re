// Module host (modules/module.h): the profile's modules register, and the
// synth engine registry serves exactly the engines they provide.
#include "test.h"

#include <cstring>

#include "audio/governor.h"
#include "audio/synth_bank.h"
#include "audio/synth_engine.h"
#include "formats/patch_pb.h"
#include "model/project.h"
#include "model/synth_patch.h"
#include "modules/module.h"
#include "modules/synth_descriptor.h"
#include "formats/patch_pb.h"
#if __has_include("ot_profile.h")
#include "ot_profile.h"
#else   // in-tree engines (before profiles): all four synth modules
#define OT_PROFILE_MODULE_COUNT 5
#define OT_MOD_ACD1 1
#define OT_MOD_FAT1 1
#define OT_MOD_VAP1 1
#define OT_MOD_PRC1 1
#define OT_MOD_WTF1 1
#endif
#ifndef OT_PROFILE_SDRAM_BYTES
#define OT_PROFILE_SDRAM_BYTES 0
#endif

using namespace ot;

namespace {
bool has(int32_t id) { return audio::synthEngineFor(id) != nullptr; }
}  // namespace

TEST(module_host_registers_profile_modules) {
    mod::ModuleHost& h = mod::ModuleHost::instance();
    h.init();
    h.init();   // idempotent
    CHECK_EQ(h.count(), OT_PROFILE_MODULE_COUNT);
    CHECK_STR(h.refused(), "");
    for (int i = 0; i < h.count(); ++i) {
        const mod::ModuleInfo& in = h.at(i)->info();
        CHECK_EQ(int(std::strlen(in.id)), 4);
        CHECK_EQ(int(in.api), int(mod::kModuleApi));
        CHECK(h.find(in.id) == h.at(i));
        for (int j = 0; j < i; ++j) CHECK(std::strcmp(h.at(j)->info().id, in.id) != 0);
    }
    CHECK(h.at(h.count()) == nullptr);
    CHECK(h.find("NONE") == nullptr);
}

TEST(module_host_serves_profile_engines) {
#ifdef OT_MOD_ACD1
    CHECK(has(kSynthEngineAcd));
#else
    CHECK(!has(kSynthEngineAcd));
#endif
#ifdef OT_MOD_FAT1
    CHECK(has(kSynthEngineFat));
#else
    CHECK(!has(kSynthEngineFat));
#endif
#ifdef OT_MOD_VAP1
    CHECK(has(kSynthEngineVap));
#else
    CHECK(!has(kSynthEngineVap));
#endif
#ifdef OT_MOD_PRC1
    CHECK(has(kSynthEnginePerc));
#else
    CHECK(!has(kSynthEnginePerc));
#endif
#ifdef OT_MOD_WTF1
    CHECK(has(kSynthEngineWtfm));
#else
    CHECK(!has(kSynthEngineWtfm));
#endif
    // The internal test engine is core.
    CHECK(!has(kSynthEngineNone));
    CHECK(audio::synthEngineFor(audio::kSynthEngineTest) == audio::synthTestEngine());
}

TEST(module_host_slot_availability_follows_profile) {
    // A synth slot is available exactly when the profile has its engine's
    // module; otherwise it keeps its patch, shows "not available" and stays silent.
    struct M {
        SynthModel model;
        bool in;
    };
    const M models[] = {
#ifdef OT_MOD_ACD1
        {SynthModel::ACD, true},
#else
        {SynthModel::ACD, false},
#endif
#ifdef OT_MOD_FAT1
        {SynthModel::FAT, true},
#else
        {SynthModel::FAT, false},
#endif
#ifdef OT_MOD_VAP1
        {SynthModel::VAP, true},
#else
        {SynthModel::VAP, false},
#endif
#ifdef OT_MOD_PRC1
        {SynthModel::PERC, true},
#else
        {SynthModel::PERC, false},
#endif
#ifdef OT_MOD_WTF1
        {SynthModel::WTFM, true},
#else
        {SynthModel::WTFM, false},
#endif
    };
    for (const M& m : models) {
        audio::SynthBank bank;
        SynthPatch patch;
        patchInit(synthEngineId(m.model), &patch);
        bank.bindPatch(0, &patch);
        SynthSlotRecord r = SynthSlotRecord::defaults(0);
        r.model = uint32_t(m.model);
        bank.setSlotRecord(0, r);
        audio::Governor gov;
        bank.setGovernor(&gov);
        audio::SynthMixInputs in;
        float L[audio::kBlock] = {}, R[audio::kBlock] = {}, D[audio::kBlock] = {}, V[audio::kBlock] = {};
        bank.render(in, L, R, D, V, nullptr);   // applies the record (audio context)
        CHECK_EQ(bank.slotAvailable(0), m.in);
        CHECK_EQ(bank.slotView(0).available, m.in);
        CHECK_STR(bank.slotView(0).model, synthModelName(m.model));
    }
}

TEST(module_host_synth_descriptors_are_consistent) {
    // Every engine module brings its descriptor (synth_descriptor.h); the core
    // has no engine table of its own.
    mod::ModuleHost& h = mod::ModuleHost::instance();
    int engines = 0;
    for (int32_t id : {kSynthEngineAcd, kSynthEngineFat, kSynthEngineVap, kSynthEngineWtfm, kSynthEnginePerc})
        engines += has(id);
    int moduleEngines = 0;   // module engines with their own ids (0x80..0xFF)
    for (int k = 0; k < h.engineCount(); ++k) moduleEngines += h.engineAt(k)->engineId >= 0x80;
    CHECK_EQ(h.engineCount(), engines + moduleEngines);
    for (int k = 0; k < h.engineCount(); ++k) {
        const mod::SynthDescriptor* d = h.engineAt(k);
        CHECK(d != nullptr);
        CHECK(h.synthDescriptor(d->engineId) == d);
        CHECK(audio::synthEngineFor(d->engineId) != nullptr);
        const size_t len = std::strlen(d->name);
        CHECK(len >= 1 && len <= 8);
        CHECK_STR(patchEngineName(d->engineId), d->name);   // Mini folder names match
        CHECK(d->paramCount >= 1 && d->paramCount <= kPatchMaxParams);
        CHECK(d->volumeParam < int(d->paramCount));
        // [OT] extension parameters (Width first): a range and a shown row each.
        CHECK(d->extCount <= kPatchExtMax && (d->extCount == 0 || d->ext != nullptr));
        for (int k = 0; k < d->extCount; ++k) CHECK(d->ui[d->paramCount + k].label != nullptr);
        CHECK(d->sectionCount >= 7 && d->sectionCount <= synthui::kMaxSections);
        // Parameter sections first, then the 7 macro sections M1..M7 in order.
        const int firstMacro = d->sectionCount - 7;
        for (int s = 0; s < d->sectionCount; ++s) {
            const synthui::Section& sec = d->sections[s];
            if (s < firstMacro) {
                CHECK(sec.macro < 0 && sec.count > 0);
                for (int i = 0; i < sec.count; ++i) CHECK(sec.params[i] < d->paramCount + d->extCount);
            } else {
                CHECK_EQ(int(sec.macro), s - firstMacro);
            }
        }
        // Every shown parameter has a range; enum names cover the range.
        for (int i = 0; i < d->paramCount; ++i) {
            const synthui::ParamUi& u = d->ui[i];
            if (!u.label || u.fmt != synthui::kFmtEnum) continue;
            CHECK(u.names != nullptr && int(u.nameCount) >= int(d->params[i].max) + 1);
        }
        SynthPatch p;
        CHECK(patchInit(d->engineId, &p));
        CHECK_EQ(validatePatch(p, nullptr, 0), 0);
    }
}

// SynthParams holds kSynthParamMax values: the Mini parameters and the [OT]
// extension ones together must fit (reloadParams fills both in the audio block).
namespace {
PatchParamRange gBigParams[kPatchMaxParams];
synthui::ParamUi gBigUi[kPatchMaxParams + kPatchExtMax];
const uint8_t kBigMain[] = {0};
const synthui::Section kBigSections[] = {OT_SYNTH_SECTION("Main", "Main", kBigMain), OT_SYNTH_MACRO_SECTIONS};
const mod::SynthDescriptor kBigDesc = {0xFE, "BIG", 0, -1, uint8_t(kPatchMaxParams), gBigParams, gBigUi,
                                       kBigSections, 8, 1, gBigParams};
class BigModule final : public mod::IModule {
public:
    const mod::ModuleInfo& info() const override {
        static const mod::ModuleInfo i = {"TBIG", mod::kModuleApi, 100, "Too many", mod::kCapSynthDescriptor,
                                          0, 0, 0, 0, 1};
        return i;
    }
    bool attach(mod::ModuleHost&) override { return true; }
    const mod::SynthDescriptor* synthDescriptor(int i) const override { return i == 0 ? &kBigDesc : nullptr; }
};
BigModule gBig;
}  // namespace

TEST(module_host_refuses_too_many_parameters) {
    CHECK(!mod::ModuleHost::instance().addForTest(gBig));   // 84 + 1 > kSynthParamMax
    CHECK(mod::ModuleHost::instance().synthDescriptor(0xFE) == nullptr);
}

// [3.x] E1 (docs/PLAN_TECHNO.md §2): module memory in the SDRAM. The host's
// block is the sum of the profile modules' declared sdram_bytes
// (OT_PROFILE_SDRAM_BYTES, in .sdram_bss on the device); ModuleHost::sdram()
// hands it out from attach() in 16-byte aligned pieces and returns nullptr
// once a request does not fit (the module then leaves itself out).
namespace {
void* gSdramGot[3] = {};
class SdramModule final : public mod::IModule {
public:
    const mod::ModuleInfo& info() const override {
        static const mod::ModuleInfo i = {"TSDR", mod::kModuleApi, 100, "SDRAM user", 0, 0, 64, 0, 0, 1};
        return i;
    }
    bool attach(mod::ModuleHost& h) override {
        gSdramGot[0] = h.sdram(40);
        gSdramGot[1] = h.sdram(24);
        gSdramGot[2] = h.sdram(1000);   // over the block: refused
        return gSdramGot[0] != nullptr;
    }
};
SdramModule gSdramModule;
}  // namespace

TEST(module_host_sdram_block_is_the_profile_sum) {
    mod::ModuleHost& h = mod::ModuleHost::instance();
    h.init();
    CHECK_EQ(h.sdramSize(), uint32_t(OT_PROFILE_SDRAM_BYTES));
    CHECK(h.sdramUsed() <= h.sdramSize());
}

TEST(module_host_sdram_hands_out_aligned_pieces) {
    mod::ModuleHost& h = mod::ModuleHost::instance();
    h.init();
    alignas(16) static uint8_t arena[16 + 128];
    const mod::ModuleHost::SdramArena saved = h.sdramArenaForTest({arena + 4, 128, 0});
    CHECK(h.sdram(0) == nullptr);
    CHECK(h.addForTest(gSdramModule));
    uint8_t* a = static_cast<uint8_t*>(gSdramGot[0]);
    uint8_t* b = static_cast<uint8_t*>(gSdramGot[1]);
    CHECK(a != nullptr && b != nullptr);
    CHECK(gSdramGot[2] == nullptr);
    CHECK_EQ(reinterpret_cast<uintptr_t>(a) % 16u, 0u);
    CHECK_EQ(reinterpret_cast<uintptr_t>(b) % 16u, 0u);
    CHECK(a >= arena + 4 && b >= a + 40 && b + 24 <= arena + 4 + 128);
    // What is left still serves a fitting request; the end of the block is a hard edge.
    void* c = h.sdram(16);
    CHECK(c != nullptr);
    CHECK(h.sdram(64) == nullptr);
    CHECK(h.sdramUsed() <= 128u);
    h.sdramArenaForTest(saved);
}

// The descriptor lookup remembers the last engine it served (the audio block
// reads a slot's whole patch through it); a module that leaves takes its
// engine with it, also when that was the one looked up last.
namespace {
PatchParamRange gSmlParams[1] = {{0.0f, 1.0f, 0.5f, 0}};
synthui::ParamUi gSmlUi[1];
const mod::SynthDescriptor kSmlDesc = {0xFD, "SML", 0, -1, 1, gSmlParams, gSmlUi, kBigSections, 8, 0, nullptr};
class SmallModule final : public mod::IModule {
public:
    const mod::ModuleInfo& info() const override {
        static const mod::ModuleInfo i = {"TSML", mod::kModuleApi, 100, "Small", mod::kCapSynthDescriptor,
                                          0, 0, 0, 0, 1};
        return i;
    }
    bool attach(mod::ModuleHost&) override { return true; }
    const mod::SynthDescriptor* synthDescriptor(int i) const override { return i == 0 ? &kSmlDesc : nullptr; }
};
SmallModule gSml;
}  // namespace

TEST(module_host_lookup_forgets_a_removed_engine) {
    mod::ModuleHost& h = mod::ModuleHost::instance();
    h.init();
    CHECK(h.addForTest(gSml));
    CHECK(h.synthDescriptor(0xFD) == &kSmlDesc);
    CHECK(h.synthDescriptor(0xFD) == &kSmlDesc);   // again: the remembered one
    h.removeLoaded(gSml);
    CHECK(h.synthDescriptor(0xFD) == nullptr);
    for (int k = 0; k < h.engineCount(); ++k) {
        const mod::SynthDescriptor* d = h.engineAt(k);
        CHECK(h.synthDescriptor(d->engineId) == d);
    }
    CHECK(h.synthDescriptor(0x7F) == nullptr);
}
