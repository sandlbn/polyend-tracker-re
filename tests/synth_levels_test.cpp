// Gain staging of the synth engines (2.x): every engine's built-in Init
// patch, played through a real SynthBank slot at the Mini's default slot
// gain (SynthSlotRecord::defaults, -9 dB) and default sends off, must leave
// headroom on the slot bus: VAP one voice and 4-voice chords, ACD one voice,
// FAT and WTFM one voice (at ACD's level) and 4-voice chords, PERC each drum and the
// whole kit at once. A sample at full scale is ~0 dBFS on its track, so an
// engine at its default gain should sit well below that. Measured levels are printed for the record. Song-level checks
// (rendered test songs, no clipping stems): tests/testsongs_test.cpp.
#include "test.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "audio/engines/perc.h"
#include "audio/synth_bank.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "model/project.h"

using namespace ot;
using namespace ot::audio;

namespace {

// One slot of a real engine in a bare bank (Init patch, default slot record).
struct Slot {
    SynthPatch patch;
    Governor gov;
    SynthBank bank;
    SynthMixInputs in;
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    float peak = 0.0f;
    long clips = 0;
    Slot(SynthModel m, int32_t engine, int poly) {
        gov.setBudget(8);
        bank.setGovernor(&gov);
        bank.setStartsPerBlock(0);
        patchInit(engine, &patch);
        bank.bindPatch(0, &patch);
        SynthSlotRecord r = SynthSlotRecord::defaults(0);
        r.model = uint32_t(m);
        r.polyphony = uint32_t(poly);
        r.delaySend = 0.0f;
        r.reverbSend = 0.0f;
        bank.setSlotRecord(0, r);
        block();
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
    void off(int track) { bank.noteOff(track, NoteOffMode::Off); }
    // Renders n blocks; returns their peak (both channels) and counts
    // samples at or over full scale.
    float run(int n) {
        float pk = 0.0f;
        for (int b = 0; b < n; ++b) {
            block();
            for (uint32_t i = 0; i < kBlock; ++i) {
                const float a = std::max(std::fabs(L[i]), std::fabs(R[i]));
                pk = std::max(pk, a);
                if (a >= 1.0f) ++clips;
            }
        }
        peak = std::max(peak, pk);
        return pk;
    }
};

double dbfs(float x) { return 20.0 * std::log10(x > 1e-9f ? double(x) : 1e-9); }

const int kChords[4][4] = {{60, 64, 67, 71}, {57, 60, 64, 67}, {48, 55, 60, 64}, {62, 65, 69, 72}};

// Peak over 40 chords of n voices (free-running LFOs and detuned
// oscillators drift between notes; 40 chords cover the spread).
float chordPeak(SynthModel m, int32_t engine, int voices, long& clips) {
    Slot s(m, engine, 4);
    for (int trial = 0; trial < 40; ++trial) {
        for (int k = 0; k < voices; ++k) s.on(k, kChords[trial % 4][k]);
        s.run(120);
        for (int k = 0; k < voices; ++k) s.off(k);
        s.run(250);
    }
    clips = s.clips;
    return s.peak;
}

}  // namespace

TEST(levels_default_slot_gain_is_mini_default) {
    CHECK_NEAR(SynthSlotRecord::defaults(0).gainDb, -9.0f, 1e-6f);
}

TEST(levels_vap_init_one_voice_and_chords) {
    long c1 = 0, c4 = 0;
    const float p1 = chordPeak(SynthModel::VAP, kSynthEngineVap, 1, c1);
    const float p4 = chordPeak(SynthModel::VAP, kSynthEngineVap, 4, c4);
    std::printf("  VAP Init at -9 dB: 1 voice peak %.1f dBFS, 4-voice chords %.1f dBFS\n", dbfs(p1), dbfs(p4));
    CHECK(dbfs(p1) < -6.0);   // one voice well below full scale
    CHECK(dbfs(p4) < -1.0);   // a 4-voice chord does not reach it
    CHECK_EQ(c1, 0L);
    CHECK_EQ(c4, 0L);
}

// USER_REPORTS_R2 item 3 (report B): the VAP Init patch on track 8, note
// played, Stop, wait, play again: the same tone every time, after a single
// Stop (transport) or a double Stop (all voices off, slot reset).
TEST(vap_init_repeats_across_stop_cycles) {
    Slot s(SynthModel::VAP, kSynthEngineVap, 4);
    std::vector<std::vector<float>> notes;
    for (int k = 0; k < 4; ++k) {
        std::vector<float> out;
        s.on(7, 60);
        for (int b = 0; b < 150; ++b) {
            s.block();
            out.insert(out.end(), s.L, s.L + kBlock);
        }
        s.off(7);
        s.run(400);   // released to silence, "a few seconds"
        if (k == 1) s.bank.stopAll();
        else s.bank.transportStop();
        s.run(50);
        notes.push_back(out);
    }
    double e = 0.0;
    for (float x : notes[0]) e += double(x) * x;
    CHECK(e > 1.0);
    for (int k = 1; k < 4; ++k) CHECK(notes[k] == notes[0]);
}

TEST(levels_acd_init_one_voice) {
    long c = 0;
    const float p = chordPeak(SynthModel::ACD, kSynthEngineAcd, 1, c);
    std::printf("  ACD Init at -9 dB: peak %.1f dBFS\n", dbfs(p));
    CHECK(dbfs(p) < -6.0);
    CHECK_EQ(c, 0L);
}

// FAT (2.1): its Init sits at ACD's Init level (within 1.5 dB) and a
// 4-voice chord keeps the VAP headroom.
TEST(levels_fat_init_matches_acd_and_chords) {
    long ca = 0, c1 = 0, c4 = 0;
    const float pa = chordPeak(SynthModel::ACD, kSynthEngineAcd, 1, ca);
    const float p1 = chordPeak(SynthModel::FAT, kSynthEngineFat, 1, c1);
    const float p4 = chordPeak(SynthModel::FAT, kSynthEngineFat, 4, c4);
    std::printf("  FAT Init at -9 dB: 1 voice peak %.1f dBFS (ACD %.1f), 4-voice chords %.1f dBFS\n", dbfs(p1), dbfs(pa),
                dbfs(p4));
    CHECK(std::fabs(dbfs(p1) - dbfs(pa)) < 1.5);
    CHECK(dbfs(p1) < -6.0);
    CHECK(dbfs(p4) < -1.0);
    CHECK_EQ(c1, 0L);
    CHECK_EQ(c4, 0L);
}

// WTFM (2.2): its Init (a sine) sits at ACD's Init level (within 1.5 dB)
// and a 4-voice chord keeps the headroom.
TEST(levels_wtfm_init_matches_acd_and_chords) {
    long ca = 0, c1 = 0, c4 = 0;
    const float pa = chordPeak(SynthModel::ACD, kSynthEngineAcd, 1, ca);
    const float p1 = chordPeak(SynthModel::WTFM, kSynthEngineWtfm, 1, c1);
    const float p4 = chordPeak(SynthModel::WTFM, kSynthEngineWtfm, 4, c4);
    std::printf("  WTFM Init at -9 dB: 1 voice peak %.1f dBFS (ACD %.1f), 4-voice chords %.1f dBFS\n", dbfs(p1), dbfs(pa),
                dbfs(p4));
    CHECK(std::fabs(dbfs(p1) - dbfs(pa)) < 1.5);
    CHECK(dbfs(p1) < -6.0);
    CHECK(dbfs(p4) < -1.0);
    CHECK_EQ(c1, 0L);
    CHECK_EQ(c4, 0L);
}

TEST(levels_perc_init_drums_and_full_kit) {
    // Kick, tom, snare, closed / open hat, cymbal, perc: one at a time.
    const int notes[] = {60, 48, 62, 64, 67, 69, 71};
    float worst = 0.0f;
    for (int n : notes) {
        Slot s(SynthModel::PERC, kSynthEnginePerc, 1);
        s.on(0, n);
        const float p = s.run(400);
        worst = std::max(worst, p);
        CHECK(dbfs(p) < -6.0);
        CHECK_EQ(s.clips, 0L);
    }
    // Every drum on the same tick (one PERC voice plays the kit).
    Slot all(SynthModel::PERC, kSynthEnginePerc, 1);
    for (int k = 0; k < 6; ++k) all.on(k, notes[k == 4 ? 5 : (k == 5 ? 6 : k)]);
    const float pk = all.run(400);
    std::printf("  PERC Init at -9 dB: loudest drum %.1f dBFS, whole kit at once %.1f dBFS\n", dbfs(worst), dbfs(pk));
    CHECK(dbfs(pk) < -1.0);
    CHECK_EQ(all.clips, 0L);
}
