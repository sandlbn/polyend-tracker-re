// [3.x] Preset instruments: instruments 67..74 play the synth presets P1..P8
// like samples. A note of P<k> borrows a synth slot for preset k: a slot
// already on k, else an idle slot (S3 first), else it steals the slot whose
// notes are all in their release, else the one whose last note-on is the
// oldest (its voices fade over one block). The slot then plays P<k> with
// its own polyphony, mono / legato rules and note-offs by track. A note of
// the slot's own instrument (S1..S3) takes it back to what Z / the chase
// selected for it; a Z on a borrowed slot wins at once. An empty preset, or
// one whose engine this build lacks, is silent.
#include "test.h"

#include <cmath>
#include <cstring>

#include "audio/audio_tables.h"
#include "audio/synth_bank.h"
#include "formats/patch_pb.h"
#include "model/constants.h"
#include "model/fx.h"
#include "modules/synth_models.h"
#if __has_include("ot_profile.h")
#include "ot_profile.h"
#endif

using namespace ot;
using namespace ot::audio;

namespace {

TEST(preset_instruments_are_67_to_74) {
    CHECK_EQ(kFirstPresetInstrument, 67);
    CHECK_EQ(kLastPresetInstrument, 74);
    CHECK_EQ(kPresetInstrumentCount, kSynthPresetCount);
    CHECK(!isPresetInstrument(66) && isPresetInstrument(67) && isPresetInstrument(74) && !isPresetInstrument(75));
    CHECK_EQ(int(presetOfInstrument(67)), 1);
    CHECK_EQ(int(presetOfInstrument(74)), 8);
    CHECK_EQ(int(presetOfInstrument(66)), 0);
    CHECK_EQ(int(presetOfInstrument(75)), 0);
    CHECK(isSynthInstrument(64) && isSynthInstrument(74) && !isSynthInstrument(75) && !isSynthInstrument(63));
    CHECK(isSynthSlotInstrument(66) && !isSynthSlotInstrument(67));
    CHECK_EQ(synthSlotOf(66), 2);
    CHECK_EQ(synthSlotOf(67), -1);
    CHECK_EQ(synthSlotOf(74), -1);
}

TEST(the_note_record_does_not_grow) {
    // The preset rides in a padding byte of the note record (and of the
    // bank's queued / pending notes).
    CHECK_EQ(int(sizeof(SynthNoteOn)), 12);
    SynthNoteOn n;
    CHECK_EQ(int(n.preset), 0);
}

#if defined(OT_MOD_ACD1) && OT_MOD_ACD1
constexpr int kMissingEngine = 0x7F7F7F;   // an engine no build has

struct Rig {
    SynthPatch own[kSynthSlots];
    SynthPatch presets[kSynthPresetCount] = {};
    PresetMix mix[kSynthPresetCount] = {};
    Governor gov;
    SynthBank bank;
    SynthMixInputs in;
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    explicit Rig(int poly = 4, int budget = 8) {
        initTables();
        gov.setBudget(budget);
        bank.setGovernor(&gov);
        bank.setStartsPerBlock(0);
        for (int s = 0; s < kSynthSlots; ++s) {
            patchInit(synthEngineId(SynthModel::ACD), &own[s]);
            bank.bindPatch(s, &own[s]);
            bank.setSlotRecord(s, record(poly));
        }
        for (SynthPatch& p : presets) p = own[0];
        presets[6].engine = kMissingEngine;   // P7: an engine this build lacks
        presets[7].engine = 0;                // P8: empty
        bank.bindPresets(presets);
        bank.bindPresetMix(mix);
        block();
    }
    static SynthSlotRecord record(int poly, float gainDb = -9.0f) {
        SynthSlotRecord r = SynthSlotRecord::defaults(0);
        r.model = uint32_t(SynthModel::ACD);
        r.polyphony = uint32_t(poly);
        r.gainDb = gainDb;
        return r;
    }
    void block() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        bank.render(in, L, R, D, V, nullptr);
    }
    void blocks(int n) {
        for (int b = 0; b < n; ++b) block();
    }
    void p(int track, int k, int note = 48, uint8_t ccType = 0, uint8_t ccValue = 0) {
        SynthNoteOn n;
        n.slot = 0;
        n.preset = uint8_t(k);
        n.note = uint8_t(note);
        n.fxA = StepFx{ccType, ccValue};
        bank.noteOn(track, n);
    }
    void s(int track, int slot, int note = 48) {
        SynthNoteOn n;
        n.slot = uint8_t(slot);
        n.note = uint8_t(note);
        bank.noteOn(track, n);
    }
    int slotOfTrack(int t) const {
        const SynthVoice* v = bank.voiceOfTrack(t);
        return v ? int(v->slot) : -1;
    }
    bool gated(int t) const {
        const SynthVoice* v = bank.voiceOfTrack(t);
        return v && v->gate;
    }
};

TEST(a_preset_note_takes_an_idle_slot_s3_first_and_then_reuses_it) {
    Rig r;
    r.p(0, 1);
    CHECK_EQ(r.slotOfTrack(0), 2);
    CHECK_EQ(int(r.bank.presetOf(2)), 1);
    CHECK_EQ(int(r.bank.selectedPresetOf(2)), 0);   // the slot's own selection stays
    r.block();
    r.p(1, 1, 55);   // P1 again: the slot already on it
    CHECK_EQ(r.slotOfTrack(1), 2);
    CHECK_EQ(r.bank.voicesOf(2), 2);
    r.p(2, 2);       // P2: the next idle slot
    CHECK_EQ(r.slotOfTrack(2), 1);
    CHECK_EQ(int(r.bank.presetOf(1)), 2);
    r.p(3, 3);       // P3: the last one
    CHECK_EQ(r.slotOfTrack(3), 0);
    CHECK_EQ(int(r.bank.presetOf(0)), 3);
    r.blocks(2);
    CHECK_EQ(r.bank.voicesOf(2), 2);
    CHECK(r.gated(0) && r.gated(1) && r.gated(2) && r.gated(3));
}

TEST(a_busy_slot_is_not_idle_and_an_s_note_does_not_move) {
    Rig r;
    r.s(0, 2);   // S3 plays
    r.block();
    r.p(1, 4);
    CHECK_EQ(r.slotOfTrack(1), 1);
    CHECK_EQ(r.slotOfTrack(0), 2);
    CHECK(r.gated(0));
    CHECK_EQ(int(r.bank.presetOf(2)), 0);
}

TEST(steal_prefers_a_released_slot_then_the_oldest_last_note) {
    Rig r;
    r.p(0, 1);   // slot 2
    r.block();
    r.p(1, 2);   // slot 1
    r.block();
    r.p(2, 3);   // slot 0
    r.block();
    r.p(3, 1, 60);   // slot 2 again: its last note-on is now the newest
    r.block();
    r.bank.noteOff(2, NoteOffMode::Off);   // slot 0 all released
    r.block();
    r.p(4, 4);
    CHECK_EQ(r.slotOfTrack(4), 0);   // the released slot, though not the oldest
    CHECK_EQ(int(r.bank.presetOf(0)), 4);
    r.block();
    // Every slot holds a gated note now: the oldest last note-on goes (slot 1).
    r.p(5, 5);
    CHECK_EQ(r.slotOfTrack(5), 1);
    CHECK_EQ(r.slotOfTrack(1), -1);   // its note was taken (faded)
    CHECK_EQ(r.bank.voicesOf(1), 1);
    r.block();
    CHECK_EQ(int(r.bank.presetOf(1)), 5);
    CHECK_EQ(r.slotOfTrack(0), 2);   // the others play on
    CHECK_EQ(r.slotOfTrack(3), 2);
    CHECK(r.gated(0) && r.gated(3));
}

TEST(a_stolen_slots_own_note_is_taken_too) {
    Rig r;
    r.s(0, 0);
    r.block();
    r.s(1, 1);
    r.block();
    r.s(2, 2);
    r.block();
    r.p(3, 6);   // S1 has the oldest note
    CHECK_EQ(r.slotOfTrack(3), 0);
    CHECK_EQ(r.slotOfTrack(0), -1);
    r.blocks(2);
    CHECK_EQ(r.bank.voicesOf(0), 1);
    CHECK_EQ(r.bank.busyRecords(), 3);   // the faded note is gone after its block
    r.bank.noteOff(0, NoteOffMode::Off);  // its note-off later changes nothing
    CHECK(r.gated(3));
}

TEST(note_off_by_track_releases_the_preset_note) {
    Rig r;
    r.p(0, 1);
    r.p(1, 1, 60);
    r.block();
    r.bank.noteOff(0, NoteOffMode::Off);
    CHECK(!r.gated(0));
    CHECK(r.gated(1));
    r.bank.noteOff(1, NoteOffMode::Cut);
    CHECK(r.bank.voiceOfTrack(1) == nullptr);
}

TEST(a_new_note_of_a_track_releases_its_previous_preset_note) {
    Rig r;
    r.p(0, 1);
    r.block();
    const SynthVoice* first = r.bank.voiceOfTrack(0);
    r.p(0, 2);   // another preset on the same track: the P1 note is released
    CHECK_EQ(r.slotOfTrack(0), 1);
    CHECK(first && !first->gate);
    CHECK_EQ(r.bank.voicesOf(2), 1);
}

TEST(a_mono_slot_plays_a_preset_legato) {
    Rig r(1);
    r.p(0, 1);
    r.block();
    const SynthVoice* v = r.bank.voiceOfTrack(0);
    r.p(0, 1, 55);
    CHECK(r.bank.voiceOfTrack(0) == v);   // the same voice (legato)
    CHECK(v && v->gate && v->note == 55);
    CHECK_EQ(r.bank.voicesOf(2), 1);
}

TEST(an_s_note_takes_its_slot_back_to_what_z_selected) {
    Rig r;
    r.p(0, 1);   // slot 2 on P1
    r.block();
    r.s(1, 2);   // S3: the slot's own patch again, the P1 note fades
    CHECK_EQ(int(r.bank.presetOf(2)), 0);
    CHECK_EQ(r.slotOfTrack(0), -1);
    CHECK_EQ(r.slotOfTrack(1), 2);
    r.blocks(2);
    CHECK_EQ(r.bank.voicesOf(2), 1);
    // With a Z on the slot, the S note brings that preset back.
    CHECK(r.bank.presetSelect(2, 3));
    r.block();
    r.p(2, 5);   // steals... slot 1 and 0 are idle: no
    CHECK_EQ(r.slotOfTrack(2), 1);
    r.bank.noteOff(1, NoteOffMode::Cut);
    r.block();
    r.p(3, 4);   // slot 2 idle now (cut): taken
    CHECK_EQ(r.slotOfTrack(3), 2);
    CHECK_EQ(int(r.bank.presetOf(2)), 4);
    CHECK_EQ(int(r.bank.selectedPresetOf(2)), 3);
    r.block();
    r.s(4, 2);
    CHECK_EQ(int(r.bank.presetOf(2)), 3);
}

TEST(a_z_on_a_borrowed_slot_wins_at_once) {
    Rig r;
    r.p(0, 1);
    r.block();
    CHECK(r.bank.presetSelect(2, 2));
    r.block();
    CHECK_EQ(int(r.bank.presetOf(2)), 2);
    CHECK_EQ(int(r.bank.selectedPresetOf(2)), 2);
    CHECK(r.gated(0));   // the note plays on, now as P2
    // A P1 note finds no slot on P1: an idle one.
    r.p(1, 1);
    CHECK_EQ(r.slotOfTrack(1), 1);
    // A repeated Z of what the slot plays costs nothing (no restart).
    const SynthVoice* v = r.bank.voiceOfTrack(1);
    CHECK(r.bank.presetSelect(1, 1));
    r.block();
    CHECK(r.bank.voiceOfTrack(1) == v && r.gated(1));
    CHECK_EQ(int(r.bank.selectedPresetOf(1)), 1);
}

TEST(an_empty_or_unavailable_preset_is_silent_and_releases_the_track) {
    Rig r;
    r.p(0, 1);
    r.block();
    for (int k : {7, 8}) {
        r.p(1, k);
        CHECK(r.bank.voiceOfTrack(1) == nullptr);
    }
    r.p(0, 8);   // the track's previous note is released
    CHECK(!r.gated(0));
    CHECK_EQ(int(r.bank.presetOf(0)), 0);
    CHECK_EQ(int(r.bank.presetOf(1)), 0);
    CHECK_EQ(int(r.bank.presetOf(2)), 1);
    r.bank.bindPresets(nullptr);
    r.p(2, 1);
    CHECK(r.bank.voiceOfTrack(2) == nullptr);
}

TEST(a_slot_with_polyphony_0_is_never_borrowed) {
    Rig r;
    r.bank.setSlotRecord(2, Rig::record(0));
    r.block();
    r.p(0, 1);
    CHECK_EQ(r.slotOfTrack(0), 1);
}

TEST(a_bind_brings_the_presets_mix_and_drops_an_own_mix_edit) {
    Rig r;
    r.mix[0].gainDb = -3.0f;
    r.mix[0].flags = kPresetMixHas;
    CHECK(r.bank.presetSelect(2, 1));
    r.block();
    r.bank.ownMixWins(2);   // an edit of the slot's own mix on P1
    r.bank.setSlotRecord(2, Rig::record(4, -12.0f));
    r.block();
    CHECK(std::fabs(r.bank.slotGain(2) - dbToGain(-12.0f)) < 1e-4f);
    r.p(0, 2);   // slot 2 is idle (on P1, no notes): P2 takes it
    CHECK_EQ(r.slotOfTrack(0), 2);
    r.block();
    CHECK(std::fabs(r.bank.slotGain(2) - dbToGain(-12.0f)) < 1e-4f);   // P2: no mix of its own
    r.s(3, 2);   // back to P1, with P1's mix (the own-mix edit went with the bind)
    r.block();
    CHECK_EQ(int(r.bank.presetOf(2)), 1);
    CHECK(std::fabs(r.bank.slotGain(2) - dbToGain(-3.0f)) < 1e-4f);
}

TEST(macro_cc_of_a_preset_note_goes_to_the_slot_it_plays_on) {
    Rig r;
    r.p(0, 1, 48, 8, 100);   // CC a = 100 in the note's FX
    r.block();
    CHECK(r.bank.slotView(2).overridden[0]);
    CHECK_EQ(int(r.bank.slotView(2).macro[0]), 100);
    r.bank.applyFx(0, 9, 30);   // an FX-only step after it: CC b
    r.block();
    CHECK(r.bank.slotView(2).overridden[1]);
    CHECK_EQ(int(r.bank.slotView(2).macro[1]), 30);
    CHECK(!r.bank.slotView(1).overridden[1]);
}

TEST(stop_and_a_patch_edit_forget_the_bindings) {
    Rig r;
    CHECK(r.bank.presetSelect(1, 3));
    r.p(0, 1);
    r.block();
    r.bank.transportStop();
    r.block();
    CHECK_EQ(int(r.bank.presetOf(2)), 0);
    CHECK_EQ(int(r.bank.selectedPresetOf(1)), 0);
    r.p(1, 1);
    r.block();
    r.bank.patchChanged(2);
    r.block();
    CHECK_EQ(int(r.bank.presetOf(2)), 0);
    CHECK_EQ(int(r.bank.selectedPresetOf(2)), 0);
}

TEST(a_sequencer_note_that_binds_starts_in_the_next_block) {
    Rig r;
    r.bank.setStartsPerBlock(SynthBank::kStartsPerBlock);
    r.p(0, 1);   // binds slot 2; the note waits one block (a bind costs about a note start)
    CHECK_EQ(int(r.bank.presetOf(2)), 1);
    CHECK(r.bank.voiceOfTrack(0) == nullptr);
    CHECK_EQ(r.bank.queuedNotes(), 1);
    r.p(1, 2);   // slot 2 is kept for its waiting note: slot 1
    r.p(2, 3);   // slot 0
    r.p(3, 4);   // past the cap: waits, no bind
    CHECK_EQ(int(r.bank.presetOf(1)), 2);
    CHECK_EQ(int(r.bank.presetOf(0)), 3);
    CHECK_EQ(r.bank.queuedNotes(), 4);
    r.block();   // (the sequencer's notes come before the block they belong to)
    CHECK_EQ(r.bank.queuedNotes(), 4);
    r.block();   // they start first in the next one
    CHECK_EQ(r.slotOfTrack(1), 1);
    CHECK_EQ(r.slotOfTrack(2), 0);
    // The fourth sound waited for the cap: it binds in this block (the slot of
    // the oldest note: P1's, just started, is the one that goes) and starts
    // in the next one, not both at once.
    CHECK_EQ(int(r.bank.presetOf(2)), 4);
    CHECK_EQ(r.slotOfTrack(0), -1);
    CHECK(r.bank.voiceOfTrack(3) == nullptr);
    CHECK_EQ(r.bank.queuedNotes(), 1);
    r.block();
    CHECK_EQ(r.slotOfTrack(3), 2);
    CHECK_EQ(r.bank.queuedNotes(), 0);
    r.block();
    r.p(4, 4);   // a slot already on P4: no wait
    CHECK_EQ(r.slotOfTrack(4), 2);
    // A slot instrument's note that takes its slot back waits as well.
    r.s(5, 2);
    CHECK_EQ(int(r.bank.presetOf(2)), 0);
    CHECK(r.bank.voiceOfTrack(5) == nullptr);
    r.blocks(2);
    CHECK_EQ(r.slotOfTrack(5), 2);
}

TEST(a_preset_chord_starts_together_after_its_bind) {
    for (int poly : {4, 1}) {
        Rig r(poly);
        r.bank.setStartsPerBlock(SynthBank::kStartsPerBlock);
        r.p(0, 1, 48);   // binds slot 2
        r.p(1, 1, 55);   // the same preset, the same step: waits with it, in step order
        CHECK(r.bank.voiceOfTrack(0) == nullptr);
        CHECK(r.bank.voiceOfTrack(1) == nullptr);
        CHECK_EQ(r.bank.queuedNotes(), 2);
        r.blocks(2);
        if (poly == 4) {
            CHECK_EQ(r.slotOfTrack(0), 2);
            CHECK_EQ(r.slotOfTrack(1), 2);
            CHECK(r.gated(0) && r.gated(1));
        } else {   // mono: the last note wins
            CHECK_EQ(r.slotOfTrack(1), 2);
            CHECK(r.gated(1));
            CHECK(r.bank.voiceOfTrack(1) && r.bank.voiceOfTrack(1)->note == 55);
            CHECK_EQ(r.bank.voicesOf(2), 1);
        }
        // The next block's notes on that slot do not wait.
        r.block();
        r.p(2, 1, 60);
        CHECK_EQ(r.slotOfTrack(2), 2);
    }
}

TEST(a_live_bind_does_not_hold_up_the_next_blocks_notes) {
    Rig r;
    r.bank.setStartsPerBlock(SynthBank::kStartsPerBlock);
    r.bank.liveOn(0, 0, 62, -1, 3);   // binds and starts in the block (live: no wait)
    r.block();
    CHECK_EQ(r.slotOfTrack(kSynthLiveTrack0), 2);
    r.p(0, 3, 50);   // the sequencer's next note on that slot starts at once
    CHECK_EQ(r.slotOfTrack(0), 2);
}

TEST(audition_and_live_notes_play_presets) {
    Rig r;
    r.bank.auditionOn(0, 60, -1, 2);
    r.block();
    CHECK_EQ(r.slotOfTrack(kSynthAuditionTrack), 2);
    CHECK_EQ(int(r.bank.presetOf(2)), 2);
    r.bank.auditionOff();
    r.block();
    CHECK(!r.gated(kSynthAuditionTrack));
    r.bank.liveOn(0, 0, 62, -1, 5);
    r.block();
    CHECK_EQ(r.slotOfTrack(kSynthLiveTrack0), 1);   // slot 2 still rings (released): the idle slot 1
    CHECK_EQ(int(r.bank.presetOf(1)), 5);
    CHECK(r.bank.voiceOfTrack(kSynthLiveTrack0) && r.bank.voiceOfTrack(kSynthLiveTrack0)->note == 62);
    r.bank.liveOn(1, 1, 64, 80, 0);   // S2 live: the slot goes back to its own patch
    r.block();
    CHECK_EQ(r.slotOfTrack(kSynthLiveTrack0 + 1), 1);
    CHECK_EQ(r.slotOfTrack(kSynthLiveTrack0), -1);
    CHECK_EQ(int(r.bank.presetOf(1)), 0);
    r.bank.liveOn(0, 0, 65, -1, 5);
    r.bank.liveOff(0);
    r.block();
    CHECK(!r.gated(kSynthLiveTrack0));
}

// A kit engine (PERC, T-Type: state beyond the voice) preset stealing a slot
// of the same engine in the middle of its hits: the cut restart zeroes the
// slot state while the hits fade; no NaN, no click above the hits' own
// edges, and the kit goes quiet after the notes end (no stuck state).
struct KitRun {
    double steadyJump = 0.0, stealJump = 0.0, tail = 0.0;
    bool finite = true;
    int borrowed = -1, busy = -1;
};
// steal false: the same hit as a note of the slot's own instrument (the reference).
KitRun kitRun(int32_t engine, bool steal) {
    KitRun out;
    initTables();
    static SynthPatch own[kSynthSlots], presets[kSynthPresetCount];
    Governor gov;
    gov.setBudget(8);
    static SynthBank bank;
    bank.reset();
    bank.stopAll();
    bank.setGovernor(&gov);
    bank.setStartsPerBlock(SynthBank::kStartsPerBlock);
    SynthSlotRecord rec = SynthSlotRecord::defaults(0);
    rec.model = engine < 0x80 ? uint32_t(engine == synthEngineId(SynthModel::PERC) ? SynthModel::PERC : SynthModel::ACD)
                              : uint32_t(SynthModel::ACD);
    rec.polyphony = 8;
    for (int k = 0; k < kSynthSlots; ++k) {
        CHECK(patchInit(engine, &own[k]));
        bank.setSlotEngine(k, engine >= 0x80 ? engine : 0);
        bank.bindPatch(k, &own[k]);
        rec.polyphony = k == 2 ? 8 : 0;   // only slot 2 plays: the preset must steal it
        bank.setSlotRecord(k, rec);
    }
    for (SynthPatch& p : presets) p = own[0];
    bank.bindPresets(presets);
    bank.bindPresetMix(nullptr);
    SynthMixInputs in;
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    double& steadyJump = out.steadyJump;
    double& stealJump = out.stealJump;
    double& tail = out.tail;
    bool& finite = out.finite;
    int& borrowed = out.borrowed;
    float prevL = 0.0f, prevR = 0.0f;
    for (int b = 0; b < 700; ++b) {
        if (b < 120 && b % 7 == 0)
            for (int t = 0; t < 3; ++t) {   // hits on 3 tracks, kit notes 36, 38, 42
                SynthNoteOn n;
                n.slot = 2;
                n.note = uint8_t(36 + 3 * t);
                bank.noteOn(t, n);
            }
        if (b == 60 || b == 61) {   // the preset steals the slot right on a hit (and the block after)
            SynthNoteOn n;
            n.preset = steal ? 1 : 0;
            n.slot = 2;
            n.note = 36;
            bank.noteOn(5, n);
        }
        if (b == 120)
            for (int t = 0; t < 8; ++t) bank.noteOff(t, NoteOffMode::Off);
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        bank.render(in, L, R, D, V, nullptr);
        if (b == 62) borrowed = bank.presetOf(2);
        double jump = 0.0;
        for (uint32_t i = 0; i < kBlock; ++i) {
            finite = finite && std::isfinite(L[i]) && std::isfinite(R[i]) && std::isfinite(D[i]) && std::isfinite(V[i]);
            jump = std::fmax(jump, std::fmax(std::fabs(L[i] - prevL), std::fabs(R[i] - prevR)));
            prevL = L[i];
            prevR = R[i];
            if (b >= 640) tail = std::fmax(tail, std::fmax(std::fabs(L[i]), std::fabs(R[i])));
        }
        if (b >= 10 && b < 58) steadyJump = std::fmax(steadyJump, jump);
        if (b >= 60 && b < 66) stealJump = std::fmax(stealJump, jump);
    }
    out.busy = bank.busyRecords();
    return out;
}

void kitSteal(int32_t engine) {
    if (!synthEngineFor(engine)) return;   // not in this build
    const KitRun ref = kitRun(engine, false), st = kitRun(engine, true);
    CHECK(st.finite && ref.finite);
    CHECK(ref.stealJump > 0.0);
    CHECK(st.stealJump <= ref.stealJump * 1.1 + 1e-6);   // no edge beyond the same hit's own
    CHECK(st.tail < 1e-4);
    CHECK_EQ(st.busy, 0);
    CHECK_EQ(st.borrowed, 1);   // (the next hits take the slot back)
    std::printf("  kit 0x%02x: hit edge %.3f, with the steal %.3f, tail %.2g\n", unsigned(engine), ref.stealJump,
                st.stealJump, st.tail);
}

TEST(a_kit_preset_steals_a_kit_slot_mid_hit_cleanly) {
    kitSteal(synthEngineId(SynthModel::PERC));
    kitSteal(0x85);   // T-Type
}
#endif

}  // namespace
