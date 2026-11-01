// [3.x] T0 (docs/PLAN_TECHNO.md §2): the kit rules of SynthBank key on the
// descriptor flag kSynthFlagKitState, not on the PERC engine id. A kit engine
// keeps state beyond a voice (PERC's and T-Type's kit in the slot scratch):
// a voice stolen in the block it started is not freed at once but renders
// its last block with SynthVoice::fading set, so the engine releases the
// kit. An engine without the flag loses such a voice at once (no render).
// Runs in every profile (test-only engines 0xF5 / 0xF6, module TKIT).
#include "test.h"

#include "audio/governor.h"
#include "audio/synth_bank.h"
#include "audio/synth_engine.h"
#include "formats/patch_pb.h"
#include "modules/module.h"
#include "modules/synth_descriptor.h"

using namespace ot;
using namespace ot::audio;

namespace {

constexpr int32_t kKit = 0xF5;     // with kSynthFlagKitState
constexpr int32_t kPlain = 0xF6;   // without it

int gFadingRenders = 0;
int gRenders = 0;

// The test engine (a sine with a gate envelope) that counts its renders.
class SpyEngine final : public ISynthEngine {
public:
    explicit SpyEngine(int32_t id) : id_(id) {}
    int32_t id() const override { return id_; }
    const char* name() const override { return "SPY"; }
    void noteOn(SynthVoice& v, const SynthParams& p, const SynthNote& n) override { inner_->noteOn(v, p, n); }
    void noteOff(SynthVoice& v, const SynthParams& p) override { inner_->noteOff(v, p); }
    bool render(SynthVoice& v, const SynthParams& p, float g0, float gi, float* L, float* R) override {
        ++gRenders;
        gFadingRenders += v.fading;
        return inner_->render(v, p, g0, gi, L, R);
    }

private:
    int32_t id_;
    ISynthEngine* inner_ = synthTestEngine();
};
SpyEngine gKitEngine(kKit), gPlainEngine(kPlain);

const PatchParamRange kParams[1] = {{1, 0, 2}};   // Volume
const synthui::ParamUi kUi[1] = {{"Volume", synthui::kFmtPct, nullptr, 0}};
const uint8_t kMain[] = {0};
const synthui::Section kSections[] = {OT_SYNTH_SECTION("Main", "Main", kMain), OT_SYNTH_MACRO_SECTIONS};
const mod::SynthDescriptor kKitDesc = {kKit, "TKIT", mod::kSynthFlagKitState, 0, 1, kParams, kUi, kSections, 8, 0, nullptr};
const mod::SynthDescriptor kPlainDesc = {kPlain, "TNOKIT", 0, 0, 1, kParams, kUi, kSections, 8, 0, nullptr};

class KitModule final : public mod::IModule {
public:
    const mod::ModuleInfo& info() const override {
        static const mod::ModuleInfo i = {"TKIT", mod::kModuleApi, 100, "Test kit flag",
                                          mod::kCapSynthEngine | mod::kCapSynthDescriptor, 0, 0, 0, 0, 8};
        return i;
    }
    bool attach(mod::ModuleHost&) override { return true; }
    ISynthEngine* synthEngine(int32_t id) override {
        return id == kKit ? &gKitEngine : (id == kPlain ? &gPlainEngine : nullptr);
    }
    const mod::SynthDescriptor* synthDescriptor(int i) const override {
        return i == 0 ? &kKitDesc : (i == 1 ? &kPlainDesc : nullptr);
    }
};
KitModule gModule;

// Slot 0 on `engine` at polyphony 2.
struct Bank {
    SynthPatch patch;
    Governor gov;
    SynthBank bank;
    SynthMixInputs in;
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    explicit Bank(int32_t engine) {
        static bool reg = false;
        if (!reg) reg = mod::ModuleHost::instance().addForTest(gModule);
        CHECK(reg);
        gov.setBudget(8);
        bank.setGovernor(&gov);
        bank.setStartsPerBlock(0);
        patchInit(engine, &patch);
        bank.bindPatch(0, &patch);
        SynthSlotRecord r = SynthSlotRecord::defaults(0);
        r.model = uint32_t(SynthModel::VAP);
        r.polyphony = 2;
        bank.setSlotRecord(0, r);
        bank.setSlotEngine(0, engine);
        block();
        gRenders = gFadingRenders = 0;
    }
    void block() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        bank.render(in, L, R, D, V, nullptr);
    }
    void on(int track, int note) {
        SynthNoteOn n;
        n.slot = 0;
        n.note = uint8_t(note);
        n.volume = -1;
        bank.noteOn(track, n);
    }
};

}  // namespace

// Three notes in one block on a Poly 2 slot: the third steals a voice that
// never rendered. A kit engine renders it once more with `fading` set.
TEST(kit_flag_fresh_steal_renders_the_fade) {
    Bank b(kKit);
    b.on(0, 60);
    b.on(1, 62);
    b.on(2, 64);
    b.block();
    CHECK_EQ(gFadingRenders, 1);
    CHECK_EQ(gRenders, 3);
}

TEST(kit_flag_absent_fresh_steal_ends_at_once) {
    Bank b(kPlain);
    b.on(0, 60);
    b.on(1, 62);
    b.on(2, 64);
    b.block();
    CHECK_EQ(gFadingRenders, 0);
    CHECK_EQ(gRenders, 2);
}
