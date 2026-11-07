// [3.x] A synth preset carries the slot insert: while a slot plays preset n
// (Z), it runs preset n's insert -- the one the slot had when the preset was
// stored, or none if it had none. A preset saved before presets had inserts
// keeps the slot's own insert. Z0, Stop, an edit of the slot's patch or of
// its own insert (App) bring the slot's own insert back, with its patch. Store, Load and Clear carry
// the insert; it is saved in project.ot3x, one chunk per preset.
#include "test.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "audio/audio_tables.h"
#include "audio/inserts.h"
#include "audio/synth_bank.h"
#include "audio/synth_params.h"
#include "formats/ot3.h"
#include "model/fx.h"
#include "formats/patch_pb.h"
#include "model/project.h"
#include "modules/module.h"
#include "modules/synth_models.h"
#include "ui/screens.h"
#include "ui_fakes.h"
#if __has_include("ot_profile.h")
#include "ot_profile.h"
#endif

using namespace ot;
using namespace ot::audio;

#if defined(OT_SLOT_INSERTS) && OT_SLOT_INSERTS

namespace {

#if defined(OT_MOD_ACD1) && OT_MOD_ACD1
// A gain (x(1 + v/100) when on), counting the blocks it ran.
class Gain final : public IEffect {
public:
    explicit Gain(const char* id) : id_(id) {}
    const char* id() const override { return id_; }
    const char* name() const override { return "Gain"; }
    uint32_t stateBytes() const override { return sizeof(float); }
    int paramCount() const override { return 1; }
    const EffectParam& param(int) const override {
        static const EffectParam p = {"Gain", 100, 100, nullptr};
        return p;
    }
    bool process(void* state, void*, const EffectSettings& s, bool on, float* l, float* r, uint32_t n) override {
        float& g = *static_cast<float*>(state);
        const float g0 = g == 0.0f ? 1.0f : g;
        const float g1 = on ? 1.0f + float(s.v[0]) * 0.01f : 1.0f;
        if (!on && g0 == 1.0f) return false;
        for (uint32_t i = 0; i < n; ++i) {
            const float k = g0 + (g1 - g0) * float(i + 1) / float(n);
            l[i] *= k;
            r[i] *= k;
        }
        g = g1;
        ++blocks;
        return on || g1 != 1.0f;
    }
    int blocks = 0;

private:
    const char* id_;
};

EffectSettings gainSet(uint8_t v) {
    EffectSettings s{};
    s.v[0] = v;
    return s;
}

struct Rig {
    SynthPatch own;
    SynthPatch presets[kSynthPresetCount] = {};
    PresetInsert ins[kSynthPresetCount] = {};
    Governor gov;
    SynthBank bank;
    SynthMixInputs in;
    int16_t lines[kSynthSlots * kEffectWorkMax / 2] = {};
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    std::vector<float> outL;
    Rig() {
        initTables();
        gov.setBudget(8);
        bank.setGovernor(&gov);
        bank.setStartsPerBlock(0);
        bank.setInsertMemory(lines);
        patchInit(synthEngineId(SynthModel::ACD), &own);
        for (SynthPatch& p : presets) p = own;
        presets[kSynthPresetCount - 1].engine = 0;   // preset 8 empty
        bank.bindPatch(0, &own);
        bank.bindPresets(presets);
        bank.bindPresetInserts(ins);
        SynthSlotRecord r = SynthSlotRecord::defaults(0);
        r.model = uint32_t(SynthModel::ACD);
        r.polyphony = 4;
        bank.setSlotRecord(0, r);
        block();
    }
    void presetIns(int n, IEffect* fx, const EffectSettings& set, const char* id = nullptr, bool off = false) {
        PresetInsert& p = ins[n - 1];
        p = PresetInsert{};
        p.fx = fx;
        p.set = set;
        const char* i = id ? id : (fx ? fx->id() : nullptr);
        if (i) std::memcpy(p.id, i, 4);
        p.flags = uint8_t(kPresetInsertHas | (off ? kPresetInsertOff : 0));
    }
    void block() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        bank.render(in, L, R, D, V, nullptr);
        outL.insert(outL.end(), L, L + kBlock);
    }
    void blocks(int n) {
        for (int b = 0; b < n; ++b) block();
    }
    void on(int note = 48) {
        SynthNoteOn n;
        n.slot = 0;
        n.note = uint8_t(note);
        n.volume = -1;
        bank.noteOn(0, n);
    }
};

double energy(const std::vector<float>& x, size_t from, size_t to) {
    double e = 0.0;
    for (size_t i = from; i < to && i < x.size(); ++i) e += double(x[i]) * x[i];
    return e;
}
#endif  // OT_MOD_ACD1

// --- App level ---------------------------------------------------------------

IEffect* anEffect(int i = 0) {
    mod::ModuleHost& mh = mod::ModuleHost::instance();
    return i < mh.effectCount() ? mh.effectAt(i) : nullptr;
}

std::vector<uint8_t> presetChunks(const std::vector<uint8_t>& f, std::vector<ot3::Chunk>* out) {
    std::vector<uint8_t> keep = f;   // payload pointers point into it
    ot3::Reader r;
    if (r.open(keep.data(), uint32_t(keep.size()), ot3::kKindProjectSidecar) != ot3::Error::kOk) return keep;
    ot3::Chunk c;
    while (r.next(c))
        if (c.tag == ot3::kTagPresetInsert) out->push_back(c);
    return keep;
}

std::vector<uint8_t> sidecarImage(App& a) {
    std::vector<uint8_t> buf(2 * ot3::kMaxFile);
    uint32_t len = 0;
    CHECK(a.encodeSidecar(buf.data(), uint32_t(buf.size()), &len));
    buf.resize(len);
    return buf;
}

// A note on slot s: the bank takes a Z at its next block with sound (here the note's).
void sound(Harness& h, int s) {
    h.app->engine.synth().auditionOn(s, 60);
    h.audio.pump(8);
    h.app->engine.synth().auditionOff();
    h.audio.pump(1);
}

// The Pattern screen's cursor row (the stopped chase reads up to it).
void cursorTo(Harness& h, int step) {
    static_cast<PatternScreen*>(h.app->ui.screen(kModPattern))->step = step;
}
void putZ(Step& s, int n) { s.fx[1] = StepFx{0, uint8_t(kFxPresetCmdBase + n)}; }   // FX-A

SynthScreen& synthScreen(App& a) { return *static_cast<SynthScreen*>(a.ui.screen(kModSynth)); }
void openPresets(Harness& h, int slot) {
    App& a = *h.app;
    a.setCurrentInstrument(uint8_t(kFirstSynthInstrument + slot));
    h.tap(btn::Pattern);
    h.tap(btn::InstrParams);
    if (synthScreen(a).view != SynthScreen::View::Macros) h.tap(btn::InstrParams);
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

#if defined(OT_MOD_ACD1) && OT_MOD_ACD1

TEST(z_switches_patch_and_insert_together_and_z0_restores_own) {
    Gain a("TGNA"), b("TGNB");
    Rig r;
    r.bank.setSlotInsert(0, &a, gainSet(50));
    r.presetIns(1, &b, gainSet(100));
    r.on();
    r.blocks(4);
    CHECK(r.bank.slotInsert(0) == &a);
    r.bank.presetSelect(0, 1);
    r.blocks(6);   // the own insert fades out, then the preset's runs
    CHECK(r.bank.slotInsert(0) == &b);
    r.bank.presetSelect(0, 0);
    r.blocks(6);
    CHECK(r.bank.slotInsert(0) == &a);
}

TEST(preset_without_insert_data_keeps_the_own_insert) {
    Gain a("TGNA");
    Rig r;
    r.bank.setSlotInsert(0, &a, gainSet(50));
    r.on();
    r.blocks(4);
    r.bank.presetSelect(0, 2);   // no insert data: saved before presets had inserts
    r.blocks(6);
    CHECK_EQ(int(r.bank.presetOf(0)), 2);
    CHECK(r.bank.slotInsert(0) == &a);
    const int ran = a.blocks;
    r.blocks(4);
    CHECK_EQ(a.blocks, ran + 4);   // never faded or parked
}

TEST(preset_with_no_insert_removes_it_while_selected) {
    Gain a("TGNA");
    Rig r;
    r.bank.setSlotInsert(0, &a, gainSet(50));
    r.presetIns(3, nullptr, EffectSettings{});   // stored with no insert
    r.on();
    r.blocks(4);
    r.bank.presetSelect(0, 3);
    r.blocks(6);
    CHECK(r.bank.slotInsert(0) == nullptr);
    r.bank.presetSelect(0, 0);
    r.blocks(2);
    CHECK(r.bank.slotInsert(0) == &a);
}

TEST(own_insert_edit_while_on_a_preset_waits_for_z0) {
    Gain a("TGNA"), b("TGNB"), c("TGNC");
    Rig r;
    r.bank.setSlotInsert(0, &a, gainSet(50));
    r.presetIns(1, &b, gainSet(100));
    r.bank.presetSelect(0, 1);
    r.blocks(6);
    CHECK(r.bank.slotInsert(0) == &b);
    r.bank.setSlotInsert(0, &c, gainSet(20));   // the bank's own insert set (the App also calls patchChanged)
    r.blocks(6);
    CHECK(r.bank.slotInsert(0) == &b);
    r.bank.presetSelect(0, 0);
    r.blocks(6);
    CHECK(r.bank.slotInsert(0) == &c);
}

TEST(transport_stop_patch_changed_and_reset_restore_own_insert) {
    Gain a("TGNA"), b("TGNB");
    for (int k = 0; k < 3; ++k) {
        Rig r;
        r.bank.setSlotInsert(0, &a, gainSet(50));
        r.presetIns(1, &b, gainSet(100));
        r.bank.presetSelect(0, 1);
        r.blocks(6);
        CHECK(r.bank.slotInsert(0) == &b);
        if (k == 0) r.bank.transportStop();
        if (k == 1) r.bank.patchChanged(0);
        if (k == 2) r.bank.reset();
        r.blocks(6);
        CHECK_EQ(int(r.bank.presetOf(0)), 0);
        CHECK(r.bank.slotInsert(0) == &a);
    }
}

TEST(same_kind_switch_only_updates_settings_other_kind_fades_first) {
    Gain a("TGNA"), b("TGNB");
    Rig dry, wet;
    wet.bank.setSlotInsert(0, &a, gainSet(0));
    wet.presetIns(1, &a, gainSet(100));   // the same kind, other settings
    dry.on();
    wet.on();
    dry.blocks(6);
    wet.blocks(6);
    const int ran = a.blocks;
    wet.bank.presetSelect(0, 1);
    dry.bank.presetSelect(0, 1);   // (no insert table entries matter here: none bound)
    dry.blocks(1);
    wet.blocks(1);
    CHECK(wet.bank.slotInsert(0) == &a);
    CHECK_EQ(a.blocks, ran + 1);   // ran on through the switch
    dry.blocks(10);
    wet.blocks(10);
    const size_t from = 9 * kBlock, to = 17 * kBlock;
    CHECK_NEAR(energy(wet.outL, from, to) / energy(dry.outL, from, to), 4.0, 1e-2);
    // Another kind: the bound one fades out first (a block here), then the next runs.
    wet.presetIns(2, &b, gainSet(100));
    wet.bank.presetSelect(0, 2);
    const int aRan = a.blocks;
    wet.blocks(1);
    CHECK_EQ(a.blocks, aRan + 1);
    CHECK_EQ(b.blocks, 0);
    wet.blocks(6);
    CHECK(wet.bank.slotInsert(0) == &b);
    CHECK(b.blocks > 0);
}

TEST(a_silent_slot_switches_its_insert_at_once) {
    // Nothing sounds on the slot: the next kind binds in the same block (a
    // Z before the note of its step must not run that note through a fade).
    Gain a("TGNA"), b("TGNB");
    Rig r;
    r.bank.setSlotInsert(0, &a, gainSet(50));
    r.presetIns(1, &b, gainSet(100));
    r.blocks(2);
    CHECK(r.bank.slotInsert(0) == &a);
    r.bank.presetSelect(0, 1);
    r.on();   // the note's sync takes the switch first
    CHECK(r.bank.slotInsert(0) == &b);
    const int aRan = a.blocks;
    r.blocks(2);
    CHECK_EQ(a.blocks, aRan);
    CHECK(b.blocks >= 2);
}

TEST(unresolved_effect_pointer_leaves_the_slot_dry) {
    Gain a("TGNA");
    Rig r;
    r.bank.setSlotInsert(0, &a, gainSet(50));
    r.presetIns(1, nullptr, gainSet(10), "QQQQ");   // an effect this build lacks
    r.bank.presetSelect(0, 1);
    r.blocks(6);
    CHECK(r.bank.slotInsert(0) == nullptr);
    r.bank.presetSelect(0, 0);
    r.blocks(6);
    CHECK(r.bank.slotInsert(0) == &a);
}

#endif  // OT_MOD_ACD1

TEST(store_captures_the_slot_insert_and_load_restores_it) {
    Harness h;
    App& a = *h.app;
    IEffect* fx = anEffect();
    CHECK(fx != nullptr);
    if (!fx) return;
    a.setSlotInsertKind(0, fx->id());
    a.setSlotInsertValue(0, 1, 0);
    const EffectSettings set = a.slotInsertSettings(0);
    a.storeSynthPreset(1, 0);
    a.setSlotInsertKind(0, nullptr);
    a.storeSynthPreset(2, 0);   // stored with no insert
    // Z1 runs the stored insert on slot 0, Z2 none.
    a.engine.synth().presetSelect(0, 1);
    sound(h, 0);
    CHECK(a.engine.synth().slotInsert(0) == fx);
    a.engine.synth().presetSelect(0, 2);
    sound(h, 0);
    CHECK(a.engine.synth().slotInsert(0) == nullptr);
    a.engine.synth().presetSelect(0, 0);
    // Load: the slot's own insert becomes the preset's.
    CHECK(a.loadSynthPreset(1, 1));
    CHECK(a.slotInsertEffect(1) == fx);
    CHECK(std::memcmp(a.slotInsertSettings(1).v, set.v, sizeof set.v) == 0);
    CHECK(a.loadSynthPreset(1, 2));
    CHECK(a.slotInsertId(1) == nullptr);
}

TEST(own_insert_edit_while_the_stopped_chase_holds_a_preset_is_heard) {
    // Like a patch edit: the slot goes to its own patch and insert, and the
    // chase leaves it there until its inputs change.
    Harness h;
    App& a = *h.app;
    IEffect* fx = anEffect();
    CHECK(fx != nullptr);
    if (!fx || fx->paramCount() < 1) return;
    SynthBank& b = a.engine.synth();
    a.setSlotInsertKind(0, fx->id());
    a.storeSynthPreset(1, 0);        // presets 1 and 2: the insert, on
    a.storeSynthPreset(2, 0);
    a.setSlotInsertOff(0, true);     // own: the same insert, off
    Pattern& p = a.pattern();
    p.tracks[0].steps[0].note = 60;
    p.tracks[0].steps[0].instrument = uint8_t(kFirstSynthInstrument);
    putZ(p.tracks[0].steps[0], 1);
    putZ(p.tracks[0].steps[4], 2);   // no note: track 1's Synth 1
    a.patternEdited();
    cursorTo(h, 2);
    h.run();
    sound(h, 0);
    CHECK_EQ(a.synthPresetActive(0), 1);
    CHECK(b.slotInsert(0) == fx);
    CHECK(!b.slotInsertOff(0));
    // A value of the own insert.
    const uint8_t v0 = a.slotInsertSettings(0).v[0];
    a.setSlotInsertValue(0, 0, uint8_t(v0 ? 0 : fx->param(0).max));
    h.run();
    sound(h, 0);
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 0);
    CHECK(b.slotInsert(0) == fx);
    CHECK(b.slotInsertOff(0));       // the own insert, as it is set
    // The cursor moves to another preset: the chase takes the slot there.
    cursorTo(h, 5);
    h.run();
    sound(h, 0);
    CHECK_EQ(a.synthPresetActive(0), 2);
    CHECK(!b.slotInsertOff(0));
    // Own insert on/off.
    a.setSlotInsertOff(0, false);
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 0);
    a.setSlotInsertOff(0, true);
    cursorTo(h, 3);
    h.run();
    sound(h, 0);
    CHECK_EQ(a.synthPresetActive(0), 1);
    // Own insert kind, and a whole own insert set at once.
    a.setSlotInsertKind(0, nullptr);
    h.run();
    sound(h, 0);
    CHECK_EQ(a.synthPresetActive(0), 0);
    CHECK(b.slotInsert(0) == nullptr);
    cursorTo(h, 6);
    h.run();
    sound(h, 0);
    CHECK_EQ(a.synthPresetActive(0), 2);
    CHECK(b.slotInsert(0) == fx);
    a.setSlotInsert(0, fx->id(), a.slotInsertSettings(0), true);
    h.run();
    sound(h, 0);
    CHECK_EQ(a.synthPresetActive(0), 0);
    CHECK(b.slotInsert(0) == fx);
    CHECK(b.slotInsertOff(0));
}

TEST(clear_drops_the_insert_and_dirties_the_sidecar) {
    Harness h;
    App& a = *h.app;
    IEffect* fx = anEffect();
    if (!fx) return;
    a.setSlotInsertKind(0, fx->id());
    a.storeSynthPreset(3, 0);
    CHECK(a.sidecarDirty());
    CHECK(a.saveProjectAs("Clr"));
    CHECK(!a.sidecarDirty());
    a.clearSynthPreset(3);
    CHECK(a.sidecarDirty());
    std::vector<ot3::Chunk> cs;
    const std::vector<uint8_t> keep = presetChunks(sidecarImage(a), &cs);
    CHECK(cs.empty());
    // Stored again from a slot without an insert: it has none.
    a.setSlotInsertKind(0, nullptr);
    a.storeSynthPreset(3, 0);
    a.setSlotInsertKind(1, fx->id());
    CHECK(a.loadSynthPreset(1, 3));
    CHECK(a.slotInsertId(1) == nullptr);
}

TEST(sidecar_round_trip_preserves_preset_inserts_and_old_projects_write_no_chunk) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    // Nothing stored: no preset chunk, and no sidecar file at all.
    {
        std::vector<ot3::Chunk> cs;
        const std::vector<uint8_t> keep = presetChunks(sidecarImage(a), &cs);
        CHECK(cs.empty());
    }
    IEffect* fx = anEffect();
    if (!fx) return;
    a.setSlotInsertKind(0, fx->id());
    a.setSlotInsertOff(0, true);
    const EffectSettings set = a.slotInsertSettings(0);
    a.storeSynthPreset(1, 0);
    a.setSlotInsertKind(0, nullptr);
    a.storeSynthPreset(2, 0);
    {
        std::vector<ot3::Chunk> cs;
        const std::vector<uint8_t> keep = presetChunks(sidecarImage(a), &cs);
        CHECK_EQ(int(cs.size()), 2);
        if (cs.size() == 2) {
            CHECK_EQ(int(cs[0].payload[0]), 1);
            CHECK(std::memcmp(cs[0].payload + 2, fx->id(), 4) == 0);
            CHECK_EQ(int(cs[1].payload[0]), 2);
            CHECK(cs[1].payload[1] & ot3::kPresetInsertNone);
        }
    }
    CHECK(a.saveProjectAs("PIns"));
    CHECK(h.fs.files.count("Projects/PIns/project.ot3x") == 1);
    const std::vector<uint8_t> sidecar = h.fs.files["Projects/PIns/project.ot3x"];
    CHECK(a.newProject());
    CHECK(a.openProject("Projects", "PIns"));
    CHECK(a.loadSynthPreset(2, 1));
    CHECK(a.slotInsertEffect(2) == fx);
    CHECK(a.slotInsertOff(2));
    CHECK(std::memcmp(a.slotInsertSettings(2).v, set.v, sizeof set.v) == 0);
    a.setSlotInsertKind(1, fx->id());
    CHECK(a.loadSynthPreset(1, 2));
    CHECK(a.slotInsertId(1) == nullptr);
    // A project saved before presets had inserts (no chunk, here no file):
    // its presets keep the slot's own insert, and it saves no chunk for them.
    h.fs.files.erase("Projects/PIns/project.ot3x");
    CHECK(a.openProject("Projects", "PIns"));
    CHECK(a.synthPresetUsed(1));
    a.setSlotInsertKind(1, fx->id());
    CHECK(a.loadSynthPreset(1, 1));
    CHECK(a.slotInsertEffect(1) == fx);
    a.setSlotInsertKind(1, nullptr);
    std::vector<ot3::Chunk> cs;
    const std::vector<uint8_t> keep = presetChunks(sidecarImage(a), &cs);
    CHECK(cs.empty());
}

TEST(unknown_effect_id_is_kept_and_saved_but_not_bound) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.setSlotInsertKind(0, "QQQQ");   // not in this build
    a.storeSynthPreset(4, 0);
    IEffect* fx = anEffect();
    a.setSlotInsertKind(0, fx ? fx->id() : nullptr);
    sound(h, 0);
    CHECK(a.engine.synth().slotInsert(0) == fx);
    a.engine.synth().presetSelect(0, 4);
    sound(h, 0);
    CHECK(a.engine.synth().slotInsert(0) == nullptr);   // dry while on preset 4
    a.engine.synth().presetSelect(0, 0);
    a.setSlotInsertKind(0, nullptr);
    CHECK(a.saveProjectAs("Unk"));
    CHECK(a.newProject());
    CHECK(a.openProject("Projects", "Unk"));
    std::vector<ot3::Chunk> cs;
    const std::vector<uint8_t> keep = presetChunks(sidecarImage(a), &cs);
    CHECK_EQ(int(cs.size()), 1);
    if (!cs.empty()) CHECK(std::memcmp(cs[0].payload + 2, "QQQQ", 4) == 0);
    CHECK(a.loadSynthPreset(0, 4));
    CHECK(a.slotInsertId(0) != nullptr && std::memcmp(a.slotInsertId(0), "QQQQ", 4) == 0);
    CHECK(a.slotInsertEffect(0) == nullptr);
}

TEST(presets_page_names_the_insert) {
    Harness h;
    App& a = *h.app;
    IEffect* fx = anEffect();
    if (!fx) return;
    a.setSlotInsertKind(1, fx->id());
    a.storeSynthPreset(1, 1);
    a.setSlotInsertKind(1, nullptr);
    a.storeSynthPreset(2, 1);
    a.setSlotInsertKind(1, "QQQQ");
    a.storeSynthPreset(3, 1);
    openPresets(h, 1);
    CHECK(synthScreen(a).view == SynthScreen::View::Presets);
    CHECK(drawn(h, fx->name()));
    CHECK(drawn(h, "no insert"));
    CHECK(drawn(h, "QQQQ"));
}

#else
TEST(preset_inserts_not_in_this_build) { CHECK(true); }
#endif
