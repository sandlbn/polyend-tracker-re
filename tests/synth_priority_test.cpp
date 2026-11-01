// [3.x] C1 (docs/PLAN_CLASSIC.md §3): the held-note registry of SynthBank,
// the Poly 1 note priority (Last / Low / High) and return-to-held for an
// engine whose descriptor has kSynthFlagNotePriority. Every track (sequencer,
// audition, MIDI input) holds at most one note; a slot's held notes reach its
// engine as SynthParams::held (count, low, high). On a flagged Poly 1 slot a
// new note sounds only when it wins; a losing note is held silently; when the
// sounding note is released while others are held, the voice plays the
// winner of the rest (noteOn reused, overlap). An engine without the flag
// keeps last-note priority and releases at the note-off (unchanged).
// Runs in every profile (test-only engines 0xF3 / 0xF4, module TPRI).
#include "test.h"

#include <vector>

#include "audio/governor.h"
#include "audio/synth_bank.h"
#include "audio/synth_engine.h"
#include "formats/patch_pb.h"
#include "modules/module.h"
#include "modules/synth_descriptor.h"

using namespace ot;
using namespace ot::audio;

namespace {

constexpr int32_t kPrio = 0xF3;     // with kSynthFlagNotePriority, Priority = parameter 0
constexpr int32_t kPlain = 0xF4;    // without the flag
enum { kLast = 0, kLow, kHigh };

struct On {
    float pitch;
    bool reused, overlap;
    SynthHeld held;
    float velocity;
};
struct Log {
    std::vector<On> ons;
    int offs = 0;
    SynthHeld renderHeld{};
};
Log gLog;

// The test engine (a sine with a gate envelope) that records its calls.
class SpyEngine final : public ISynthEngine {
public:
    explicit SpyEngine(int32_t id) : id_(id) {}
    int32_t id() const override { return id_; }
    const char* name() const override { return "SPY"; }
    void noteOn(SynthVoice& v, const SynthParams& p, const SynthNote& n) override {
        gLog.ons.push_back(On{n.pitch, n.reused, n.overlap, p.held, n.velocity});
        inner_->noteOn(v, p, n);
    }
    void noteOff(SynthVoice& v, const SynthParams& p) override {
        ++gLog.offs;
        inner_->noteOff(v, p);
    }
    bool render(SynthVoice& v, const SynthParams& p, float g0, float gi, float* L, float* R) override {
        gLog.renderHeld = p.held;
        return inner_->render(v, p, g0, gi, L, R);
    }

private:
    int32_t id_;
    ISynthEngine* inner_ = synthTestEngine();
};
SpyEngine gPrioEngine(kPrio), gPlainEngine(kPlain);

const char* const kPrioName[3] = {"Last", "Low", "High"};
const PatchParamRange kParams[2] = {{0, 0, 2}, {1, 0, 2}};   // Priority, Volume
const synthui::ParamUi kUi[2] = {{"Priority", OT_SYNTH_ENUM(kPrioName)}, {"Volume", synthui::kFmtPct, nullptr, 0}};
const uint8_t kMain[] = {0, 1};
const synthui::Section kSections[] = {OT_SYNTH_SECTION("Main", "Main", kMain), OT_SYNTH_MACRO_SECTIONS};
const mod::SynthDescriptor kPrioDesc = {kPrio, "TPRIO", mod::kSynthFlagNotePriority, 1, 2, kParams, kUi, kSections, 8, 0,
                                        nullptr, nullptr, 0};
const mod::SynthDescriptor kPlainDesc = {kPlain, "TPLAIN", 0, 1, 2, kParams, kUi, kSections, 8, 0, nullptr};

class PrioModule final : public mod::IModule {
public:
    const mod::ModuleInfo& info() const override {
        static const mod::ModuleInfo i = {"TPRI", mod::kModuleApi, 100, "Test note priority",
                                          mod::kCapSynthEngine | mod::kCapSynthDescriptor, 0, 0, 0, 0, 8};
        return i;
    }
    bool attach(mod::ModuleHost&) override { return true; }
    ISynthEngine* synthEngine(int32_t id) override {
        return id == kPrio ? &gPrioEngine : (id == kPlain ? &gPlainEngine : nullptr);
    }
    const mod::SynthDescriptor* synthDescriptor(int i) const override {
        return i == 0 ? &kPrioDesc : (i == 1 ? &kPlainDesc : nullptr);
    }
};
PrioModule gModule;

// Slot 0 on `engine` with polyphony `poly` and Priority `prio`.
struct Bank {
    SynthPatch patch[kSynthSlots];
    Governor gov;
    SynthBank bank;
    SynthMixInputs in;
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    explicit Bank(int32_t engine = kPrio, int prio = kLast, int poly = 1) {
        static bool reg = false;
        if (!reg) reg = mod::ModuleHost::instance().addForTest(gModule);
        CHECK(reg);
        gov.setBudget(8);
        bank.setGovernor(&gov);
        bank.setStartsPerBlock(0);
        for (int s = 0; s < kSynthSlots; ++s) {
            patchInit(engine, &patch[s]);
            synthParamSet(patch[s], 0, float(prio));
            bank.bindPatch(s, &patch[s]);
            SynthSlotRecord r = SynthSlotRecord::defaults(s);
            r.model = uint32_t(SynthModel::ACD);
            r.polyphony = uint32_t(poly);
            bank.setSlotRecord(s, r);
            bank.setSlotEngine(s, engine);
        }
        gLog = Log{};
        block();
    }
    void block() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        bank.render(in, L, R, D, V, nullptr);
    }
    void on(int track, int note, int slot = 0, StepFx a = {}, int volume = -1) {
        SynthNoteOn n;
        n.slot = uint8_t(slot);
        n.note = uint8_t(note);
        n.volume = int8_t(volume);
        n.fxA = a;
        bank.noteOn(track, n);
    }
    void off(int track, NoteOffMode m = NoteOffMode::Off) { bank.noteOff(track, m); }
    const SynthHeld& held(int s = 0) const { return bank.params(s).held; }
    // The note the slot's voice plays and whether its gate is held.
    int sounding(int track) const {
        const SynthVoice* v = bank.voiceOfTrack(track);
        return v && v->gate ? int(v->note) : -1;
    }
};

}  // namespace

TEST(prio_descriptor_flag_and_param) {
    Bank b;
    const mod::SynthDescriptor* d = mod::ModuleHost::instance().synthDescriptor(kPrio);
    CHECK(d != nullptr);
    if (!d) return;
    CHECK(d->flags & mod::kSynthFlagNotePriority);
    CHECK_EQ(int(d->priorityParam), 0);
    // Aggregates without the field: no priority parameter.
    CHECK_EQ(int(mod::ModuleHost::instance().synthDescriptor(kPlain)->priorityParam), -1);
}

TEST(prio_held_registry_reaches_the_engine) {
    Bank b;
    CHECK_EQ(int(b.held().count), 0);
    b.on(0, 48);
    CHECK_EQ(int(b.held().mono), 1);   // the slot plays polyphony 1
    CHECK_EQ(gLog.ons.size(), 1u);
    CHECK_EQ(int(gLog.ons[0].held.count), 1);
    CHECK_EQ(int(gLog.ons[0].held.low), 48);
    CHECK_EQ(int(gLog.ons[0].held.high), 48);
    b.on(1, 60);
    CHECK_EQ(int(b.held().count), 2);
    CHECK_EQ(int(b.held().low), 48);
    CHECK_EQ(int(b.held().high), 60);
    CHECK_EQ(int(gLog.ons.back().held.count), 2);
    b.block();
    CHECK_EQ(int(gLog.renderHeld.count), 2);
    // A track holds one note: its new note replaces the old one.
    b.on(1, 55);
    CHECK_EQ(int(b.held().count), 2);
    CHECK_EQ(int(b.held().high), 55);
    // Other slots keep their own notes.
    b.on(2, 70, 1);
    CHECK_EQ(int(b.held(1).count), 1);
    CHECK_EQ(int(b.held().count), 2);
    // A track moving to another slot leaves the first one.
    b.on(1, 72, 1);
    CHECK_EQ(int(b.held().count), 1);
    CHECK_EQ(int(b.held(1).count), 2);
    b.off(0);
    b.off(1);
    b.off(2);
    CHECK_EQ(int(b.held().count), 0);
    CHECK_EQ(int(b.held(1).count), 0);
    // stopAll forgets every held note.
    b.on(3, 50);
    b.on(4, 52);
    b.bank.stopAll();
    CHECK_EQ(int(b.held().count), 0);
}

TEST(prio_last_returns_to_the_held_note) {
    Bank b(kPrio, kLast);
    b.on(0, 48);
    b.block();
    b.on(1, 60);   // the last note wins: legato on the voice
    CHECK_EQ(gLog.ons.size(), 2u);
    CHECK_EQ(gLog.ons[1].pitch, 60.0f);
    CHECK(gLog.ons[1].reused && gLog.ons[1].overlap);
    CHECK_EQ(b.sounding(1), 60);
    b.block();
    b.off(1);   // track 0 still holds 48: back to it, no release
    CHECK_EQ(gLog.offs, 0);
    CHECK_EQ(gLog.ons.size(), 3u);
    CHECK_EQ(gLog.ons[2].pitch, 48.0f);
    CHECK(gLog.ons[2].reused && gLog.ons[2].overlap);
    CHECK_EQ(int(gLog.ons[2].held.count), 1);
    CHECK_EQ(b.sounding(0), 48);
    CHECK_EQ(b.bank.voicesOf(0), 1);
    b.block();
    b.off(0);   // nothing held: the release
    CHECK_EQ(gLog.offs, 1);
    CHECK_EQ(b.sounding(0), -1);
    // Three keys: the most recent of the rest comes back.
    b.on(0, 40);
    b.on(1, 44);
    b.on(2, 42);
    b.off(2);
    CHECK_EQ(gLog.ons.back().pitch, 44.0f);
    b.off(1);
    CHECK_EQ(gLog.ons.back().pitch, 40.0f);
    b.off(0);
    // The key order survives a long session (renumbered before it wraps).
    b.on(0, 40);
    for (int k = 0; k < 600; ++k) {
        b.on(1, 50 + k % 5);
        b.off(1);
    }
    b.on(2, 44);
    b.on(3, 47);
    b.off(3);
    CHECK_EQ(gLog.ons.back().pitch, 44.0f);
    b.off(2);
    CHECK_EQ(gLog.ons.back().pitch, 40.0f);
}

TEST(prio_low_and_high) {
    {
        Bank b(kPrio, kLow);
        b.on(0, 48);
        b.on(1, 60);   // higher: held silently, the bass keeps sounding
        CHECK_EQ(gLog.ons.size(), 1u);
        CHECK_EQ(b.sounding(0), 48);
        CHECK_EQ(int(b.held().count), 2);
        b.on(2, 40);   // lower: wins
        CHECK_EQ(gLog.ons.size(), 2u);
        CHECK_EQ(gLog.ons[1].pitch, 40.0f);
        CHECK_EQ(b.sounding(2), 40);
        b.off(1);      // a silent note goes: nothing changes
        CHECK_EQ(gLog.ons.size(), 2u);
        CHECK_EQ(gLog.offs, 0);
        b.off(2);      // back to the lowest held one
        CHECK_EQ(gLog.ons.back().pitch, 48.0f);
        CHECK_EQ(b.sounding(0), 48);
        // The sounding track plays a note that now loses: the voice moves
        // to the winner of the held notes.
        b.on(1, 52);
        CHECK_EQ(gLog.ons.size(), 3u);
        b.on(0, 70);
        CHECK_EQ(gLog.ons.back().pitch, 52.0f);
        CHECK_EQ(b.sounding(1), 52);
        CHECK_EQ(gLog.offs, 0);
    }
    {
        Bank b(kPrio, kHigh);
        b.on(0, 60);
        b.on(1, 48);   // lower: silent
        CHECK_EQ(gLog.ons.size(), 1u);
        b.on(2, 67);   // higher: wins
        CHECK_EQ(gLog.ons.back().pitch, 67.0f);
        b.off(2);
        CHECK_EQ(gLog.ons.back().pitch, 60.0f);
        b.off(0);
        CHECK_EQ(gLog.ons.back().pitch, 48.0f);
        b.off(1);
        CHECK_EQ(gLog.offs, 1);
    }
}

TEST(prio_return_keeps_the_held_tracks_tune) {
    Bank b(kPrio, kLast);
    b.on(0, 48, 0, StepFx{40, 36});   // Tune +12 on the held note
    b.block();
    b.on(1, 60);
    b.block();
    b.off(1);
    const SynthVoice* v = b.bank.voiceOfTrack(0);
    CHECK(v != nullptr);
    if (v) CHECK_EQ(v->bend, 12.0f);
}

TEST(prio_midi_input_tracks_share_the_registry) {
    Bank b(kPrio, kLast);
    b.bank.liveOn(0, 0, 50, -1);
    b.block();
    b.bank.liveOn(1, 0, 57, -1);
    b.block();
    CHECK_EQ(int(b.held().count), 2);
    CHECK_EQ(gLog.ons.back().pitch, 57.0f);
    b.bank.liveOff(1);
    b.block();
    CHECK_EQ(gLog.ons.back().pitch, 50.0f);
    CHECK_EQ(gLog.offs, 0);
    b.bank.liveOff(0);
    b.block();
    CHECK_EQ(gLog.offs, 1);
}

TEST(prio_cut_and_poly_slots) {
    // Cut ends the voice and forgets the note; nothing comes back.
    {
        Bank b(kPrio, kLast);
        b.on(0, 48);
        b.on(1, 60);
        b.off(1, NoteOffMode::Cut);
        CHECK_EQ(int(b.held().count), 1);
        CHECK_EQ(b.bank.voicesOf(0), 0);
        CHECK_EQ(gLog.ons.size(), 2u);
    }
    // Polyphony 2: the registry is kept (held), the voices play as before.
    {
        Bank b(kPrio, kLow, 2);
        b.on(0, 48);
        b.on(1, 60);
        CHECK_EQ(gLog.ons.size(), 2u);
        CHECK(!gLog.ons[1].reused);
        CHECK_EQ(b.bank.voicesOf(0), 2);
        CHECK_EQ(int(b.held().count), 2);
        CHECK_EQ(int(b.held().mono), 0);
        b.off(1);
        CHECK_EQ(gLog.offs, 1);
        CHECK_EQ(gLog.ons.size(), 2u);
    }
}

TEST(prio_without_the_flag_nothing_changes) {
    // Last-note: the second key takes the voice; its release releases the
    // voice even though the first key is still held (as before C1).
    Bank b(kPlain, kLow);
    b.on(0, 48);
    b.on(1, 60);
    CHECK_EQ(gLog.ons.size(), 2u);
    CHECK_EQ(gLog.ons[1].pitch, 60.0f);
    b.off(1);
    CHECK_EQ(gLog.offs, 1);
    CHECK_EQ(gLog.ons.size(), 2u);
    CHECK_EQ(int(b.held().count), 1);   // the registry is still kept (for the engine)
    b.off(0);
    CHECK_EQ(gLog.offs, 1);   // track 0 lost the voice to track 1
}

TEST(prio_return_keeps_the_held_notes_velocity) {
    // The held note comes back at its own velocity, not the released one's.
    Bank b(kPrio, kLast);
    b.on(0, 48, 0, StepFx{}, 80);
    b.block();
    b.on(1, 60, 0, StepFx{}, 20);
    CHECK_NEAR(gLog.ons.back().velocity, 0.2f, 1e-6);
    b.block();
    b.off(1);
    CHECK_EQ(gLog.ons.back().pitch, 48.0f);
    CHECK_NEAR(gLog.ons.back().velocity, 0.8f, 1e-6);
}

TEST(prio_holds_a_losing_note_without_a_voice) {
    // Low: the held low note's voice was taken (a steal); a higher note on
    // another track still loses: nothing starts, and the held note is not
    // fired again either.
    Bank b(kPrio, kLow);
    b.on(0, 48);
    b.block();
    CHECK(b.bank.stealOne());
    b.block();
    CHECK_EQ(b.bank.voicesOf(0), 0);
    const size_t n = gLog.ons.size();
    b.on(1, 60);
    CHECK_EQ(gLog.ons.size(), n);
    CHECK_EQ(b.bank.voicesOf(0), 0);
    b.on(1, 62);
    CHECK_EQ(gLog.ons.size(), n);
    // A lower note wins and plays.
    b.on(2, 40);
    CHECK_EQ(gLog.ons.size(), n + 1);
    CHECK_EQ(gLog.ons.back().pitch, 40.0f);
}
