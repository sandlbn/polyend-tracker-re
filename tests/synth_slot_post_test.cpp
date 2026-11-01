// The per-slot engine hook ISynthEngine::slotPost (3.x, JU6's built-in
// chorus): called once per block for every slot with sounding voices, after
// they rendered, on that slot's own bus (the voices' sum, before slot gain /
// pan / sends) with the slot's index; while it reports a tail the slot keeps
// rendering after its last voice ended (and the bank stays active); an idle
// slot without a tail never calls it; an engine change drops the tail.
#include "test.h"

#include <cmath>
#include <cstring>

#include "audio/governor.h"
#include "audio/synth_bank.h"
#include "audio/synth_engine.h"
#include "formats/patch_pb.h"
#include "modules/module.h"
#include "modules/synth_descriptor.h"

using namespace ot;
using namespace ot::audio;

namespace {

constexpr int32_t kPostEngine = 0xF2;   // test-only id
constexpr float kDc = 0.25f;            // each voice adds kDc x its gain
constexpr int kVoiceBlocks = 4;         // a voice lasts this many blocks
constexpr int kTailBlocks = 3;          // the slot effect rings this many blocks after its input

struct PostLog {
    int calls[kSynthSlots] = {};
    float in[kSynthSlots] = {};   // L[0] the last call saw (before its own output)
    int tail[kSynthSlots] = {};
};
PostLog gLog;

class PostEngine final : public ISynthEngine {
public:
    int32_t id() const override { return kPostEngine; }
    const char* name() const override { return "POST"; }
    void noteOn(SynthVoice& v, const SynthParams&, const SynthNote&) override {
        v.as<int>() = kVoiceBlocks;
        v.level = 1.0f;
    }
    void noteOff(SynthVoice&, const SynthParams&) override {}
    bool render(SynthVoice& v, const SynthParams&, float g0, float gInc, float* L, float* R) override {
        int& left = v.as<int>();
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float g = g0 + gInc * float(i);
            L[i] += kDc * g;
            R[i] += kDc * g;
        }
        return --left > 0;
    }
    bool slotPost(int slot, SynthSlotScratch&, const SynthParams&, float* L, float* R) override {
        CHECK(slot >= 0 && slot < kSynthSlots);
        if (slot < 0 || slot >= kSynthSlots) return false;
        ++gLog.calls[slot];
        gLog.in[slot] = L[0];
        int& t = gLog.tail[slot];
        if (L[0] != 0.0f) {
            t = kTailBlocks;
        } else if (t > 0) {
            for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = 0.1f;
            --t;
        }
        return t > 0;
    }
};
PostEngine gEngine;

const PatchParamRange kParams[1] = {{1, 0, 2}};   // Volume
const synthui::ParamUi kUi[1] = {{"Volume", synthui::kFmtPct, nullptr, 0}};
const uint8_t kMain[] = {0};
const synthui::Section kSections[] = {OT_SYNTH_SECTION("Main", "Main", kMain), OT_SYNTH_MACRO_SECTIONS};
const mod::SynthDescriptor kDesc = {kPostEngine, "POST", 0, 0, 1, kParams, kUi, kSections, 8, 0, nullptr};

class PostModule final : public mod::IModule {
public:
    const mod::ModuleInfo& info() const override {
        static const mod::ModuleInfo i = {"TPST", mod::kModuleApi, 100, "Test slot post",
                                          mod::kCapSynthEngine | mod::kCapSynthDescriptor, 0, 0, 0, 0, 8};
        return i;
    }
    bool attach(mod::ModuleHost&) override { return true; }
    ISynthEngine* synthEngine(int32_t id) override { return id == kPostEngine ? &gEngine : nullptr; }
    const mod::SynthDescriptor* synthDescriptor(int i) const override { return i == 0 ? &kDesc : nullptr; }
};
PostModule gModule;

struct Bank {
    SynthPatch patch[kSynthSlots];
    Governor gov;
    SynthBank bank;
    SynthMixInputs in;
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    Bank() {
        static bool reg = false;
        if (!reg) reg = mod::ModuleHost::instance().addForTest(gModule);
        CHECK(reg);
        gov.setBudget(8);
        bank.setGovernor(&gov);
        bank.setStartsPerBlock(0);
        for (int s = 0; s < kSynthSlots; ++s) {
            patchInit(kPostEngine, &patch[s]);
            bank.bindPatch(s, &patch[s]);
            SynthSlotRecord r = SynthSlotRecord::defaults(s);
            r.model = uint32_t(SynthModel::ACD);
            r.polyphony = 4;
            r.delaySend = 0.0f;
            r.reverbSend = 0.0f;
            bank.setSlotRecord(s, r);
            bank.setSlotEngine(s, kPostEngine);
        }
        gLog = PostLog{};
        block();
    }
    // One block; the peak of the master mix.
    float block() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        bank.render(in, L, R, D, V, nullptr);
        float pk = 0.0f;
        for (uint32_t i = 0; i < kBlock; ++i) pk = std::fmax(pk, std::fabs(L[i]));
        return pk;
    }
    void on(int track, int slot) {
        SynthNoteOn n;
        n.slot = uint8_t(slot);
        n.note = 60;
        n.volume = -1;
        bank.noteOn(track, n);
    }
};

}  // namespace

TEST(slot_post_not_called_for_idle_slots) {
    Bank b;
    for (int k = 0; k < 10; ++k) CHECK_EQ(b.block(), 0.0f);
    for (int s = 0; s < kSynthSlots; ++s) CHECK_EQ(gLog.calls[s], 0);
    CHECK(!b.bank.active());
}

TEST(slot_post_once_per_block_on_the_slot_bus) {
    Bank b;
    b.on(0, 1);
    b.on(1, 1);   // two voices on slot 1: one call with their sum
    b.block();
    CHECK_EQ(gLog.calls[0], 0);
    CHECK_EQ(gLog.calls[1], 1);
    CHECK_EQ(gLog.calls[2], 0);
    // The bus before slot gain (-9 dB default) and pan: the two voices' sum
    // x the Volume parameter (1) x their gain ramp (from 0 at the note start
    // or already 1).
    CHECK(gLog.in[1] >= 0.0f && gLog.in[1] <= 2.0f * kDc + 1e-6f);
    b.block();
    CHECK_EQ(gLog.calls[1], 2);
    CHECK(std::fabs(gLog.in[1] - 2.0f * kDc) < 1e-6f);
    // A second slot: its own call with its own index.
    b.on(2, 2);
    b.block();
    CHECK_EQ(gLog.calls[1], 3);
    CHECK_EQ(gLog.calls[2], 1);
    CHECK_EQ(gLog.calls[0], 0);
}

TEST(slot_post_tail_outlives_the_voices) {
    Bank b;
    b.on(0, 0);
    int blocks = 0;
    while (b.bank.voicesOf(0) > 0 && blocks < 20) {
        b.block();
        ++blocks;
    }
    CHECK(blocks <= kVoiceBlocks + 1);
    const int before = gLog.calls[0];
    // No voice left: the tail still renders (and reaches the mix) while
    // slotPost says so, and the bank counts as active for it.
    for (int k = 0; k < kTailBlocks; ++k) {
        CHECK(b.bank.active());
        CHECK(b.block() > 0.0f);
    }
    CHECK_EQ(gLog.calls[0], before + kTailBlocks);
    CHECK(!b.bank.active());
    for (int k = 0; k < 5; ++k) CHECK_EQ(b.block(), 0.0f);
    CHECK_EQ(gLog.calls[0], before + kTailBlocks);
}

TEST(slot_post_tail_dropped_on_engine_change) {
    Bank b;
    b.on(0, 0);
    for (int k = 0; k < kVoiceBlocks; ++k) b.block();
    CHECK(b.bank.active());   // the tail rings
    b.bank.setSlotEngine(0, 0);   // back to the record's Mini model (no patch of it: silent)
    const int before = gLog.calls[0];
    b.block();
    CHECK_EQ(gLog.calls[0], before);
    CHECK(!b.bank.active());
}
