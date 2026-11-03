// Performance sequencer tests: per-track pattern overrides (spec 12 §C) and
// pattern play modes / roll injection (spec 12 §D).
#include "test.h"

#include <map>
#include <memory>
#include <vector>

#include "model/pattern.h"
#include "model/project.h"
#include "sequencer/seq_playmodes.h"
#include "sequencer/sequencer.h"

using namespace ot;

namespace {

// Records the note started at µstep 1 of each step, per track (the first
// note-on of a tick; later ones in the same tick are roll retriggers).
struct StepSink : ISequencerSink {
    Sequencer* seq = nullptr;
    std::vector<int> notes[12];
    uint64_t lastTick[12] = {};
    bool any[12] = {};
    std::vector<VoiceNoteOn> raw[12];
    void voiceNoteOn(uint8_t t, const VoiceNoteOn& n) override {
        raw[t].push_back(n);
        if (seq->microStep(t) != 1) return;
        if (any[t] && lastTick[t] == seq->tickCount()) return;
        any[t] = true;
        lastTick[t] = seq->tickCount();
        notes[t].push_back(n.note);
    }
    void voiceNoteOff(uint8_t, NoteOffMode) override {}
    void voiceApplyFx(uint8_t, uint8_t, uint8_t, uint8_t) override {}
    void resetAllVoiceFx() override {}
    void engineBlockUpdate(float) override {}
    void metronomeClick(bool, uint8_t) override {}
    void midiSend(uint8_t, const uint8_t*, uint8_t) override {}
};

// Pattern n: every track has last step len[n] (default 7) and note
// (n % 8) * 16 + s on step s (instrument 0).
struct PatHost : ISequencerHost {
    std::map<int, int> len;
    std::vector<std::pair<int, const Pattern*>> loads;
    static int note(int n, int s) { return (n % 8) * 16 + s; }
    void fill(uint8_t n, Pattern& dst) {
        dst.setDefault();
        const int l = len.count(n) ? len[n] : 7;
        for (auto& t : dst.tracks) {
            t.lastStep = uint8_t(l);
            for (int s = 0; s < 128; ++s) {
                t.steps[s].note = int8_t(note(n, s) & 127);
                t.steps[s].instrument = 0;
            }
        }
    }
    void loadPattern(uint8_t n, Pattern& dst, bool) override {
        loads.push_back({n, &dst});
        fill(n, dst);
    }
};

struct Rig {
    Project project;
    Pattern a, b;
    std::unique_ptr<PerfPatternMemory> mem{new PerfPatternMemory};
    StepSink sink;
    PatHost host;
    Sequencer seq;
    explicit Rig(int curPattern = 1) {
        project.setTempo(800.0f);
        project.setCurrentPattern(uint8_t(curPattern));
        host.fill(uint8_t(curPattern), a);
        b.setDefault();
        sink.seq = &seq;
        seq.attach(&sink, &host, &project, &a, &b);
        seq.setPerfPatternMemory(mem.get());
        host.loads.clear();
    }
    void block() { seq.processBlock(128); }
    // Run until track t has started n steps (counted from now).
    void steps(int t, size_t n) {
        const size_t target = sink.notes[t].size() + n;
        for (int i = 0; i < 200000 && sink.notes[t].size() < target; ++i) block();
    }
    // Run until track t has started its step `s` (micro 1 just passed).
    std::vector<int> tail(int t, size_t from) const {
        return std::vector<int>(sink.notes[t].begin() + long(from), sink.notes[t].end());
    }
};

bool eq(const std::vector<int>& got, const std::vector<int>& want) {
    if (got.size() < want.size()) {
        std::printf("       short: %zu < %zu\n", got.size(), want.size());
        return false;
    }
    for (size_t i = 0; i < want.size(); ++i)
        if (got[i] != want[i]) {
            std::printf("       at %zu: got %d want %d\n", i, got[i], want[i]);
            return false;
        }
    return true;
}

// --- spec 12 §D tables, re-encoded independently from the spec text --------

void run(std::vector<int>& v, int a, int b) { for (int i = a; i <= b; ++i) v.push_back(i); }
std::vector<int> times(const std::vector<int>& p, int n) {
    std::vector<int> r;
    for (int i = 0; i < n; ++i) r.insert(r.end(), p.begin(), p.end());
    return r;
}

std::vector<int> specSteps(int c) {  // c = 1..20
    std::vector<int> v;
    std::vector<int> s32;
    run(s32, 1, 32);
    switch (c) {
        case 1:
            v = {1, 2, 3, 4, 1, 2, 3, 4, 9, 10, 11, 12, 1, 1, 15, 16, 1, 2, 19, 20, 1, 1};
            run(v, 23, 32);
            v.insert(v.end(), {1, 1, 35, 36, 37, 2, 3});
            run(v, 40, 86);
            v.insert(v.end(), {1, 2, 3, 4});
            run(v, 91, 115);
            v.insert(v.end(), {1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4, 128});
            return v;
        case 2:
            v = {1, 2, 3, 4, 4, 3, 2, 1, 9, 10, 11, 12, 12, 11, 10, 9, 17, 18, 19, 20, 20, 19, 2, 17,
                 25, 26, 27, 28, 28, 2, 26, 25, 33, 34, 35, 36, 36, 35, 34, 33, 41, 1};
            run(v, 43, 68);
            v.push_back(2);
            run(v, 70, 81);
            v.push_back(2);
            run(v, 83, 99);
            v.push_back(2);
            run(v, 101, 105);
            v.insert(v.end(), {2, 107, 108, 109, 1, 111, 112, 113, 1, 115, 116, 117, 1, 119, 120, 121, 1, 1, 1,
                               125, 126, 1, 128});
            return v;
        case 3: case 4: case 9: case 10: case 13: case 14: case 15:
            return times(s32, 4);
        case 5: {
            std::vector<int> h;
            run(h, 1, 8);
            for (int i = 8; i >= 1; --i) h.push_back(i);
            run(h, 1, 8);
            h.insert(h.end(), {1, 1, 2, 2, 3, 3, 4, 4});
            v = times(h, 2);
            std::vector<int> e;
            run(e, 1, 8);
            const auto t8 = times(e, 8);
            v.insert(v.end(), t8.begin(), t8.end());
            return v;
        }
        case 6:
            return times({1, 2, 3, 4, 5, 6, 7, 7, 9, 10, 11, 12, 13, 14, 15, 15, 17, 18, 19, 20, 21, 22, 23, 23,
                          25, 26, 27, 28, 29, 30, 31, 31}, 4);
        case 7: {
            std::vector<int> p;
            run(p, 1, 13);
            p.insert(p.end(), {13, 15, 12});
            run(p, 17, 27);
            p.insert(p.end(), {26, 29, 26, 32, 25});
            return times(p, 4);
        }
        case 8:
            return times({1, 1, 1, 1, 5, 5, 5, 5, 9, 9, 9, 9, 13, 13, 13, 13, 17, 18, 17, 18, 21, 22, 21, 22,
                          32, 31, 30, 29, 29, 30, 31, 32}, 4);
        case 11: {
            std::vector<int> p;
            for (int i = 1; i <= 16; ++i) p.insert(p.end(), {i, i, i, i});
            return times(p, 2);
        }
        case 12: {
            std::vector<int> p;
            for (int i = 1; i <= 32; ++i) p.insert(p.end(), {i, i});
            return times(p, 2);
        }
        case 16:
            return times({1, 2, 3, 1, 2, 3, 5, 2, 2, 1, 2, 4, 1, 2, 5, 2, 1, 2, 3, 1, 2, 2, 5, 2, 4, 1, 2, 2,
                          1, 2, 2, 2}, 4);
        case 17: case 18:
            return times({1, 3, 3, 3, 5, 3, 3, 1, 3, 3, 1, 3, 5, 1, 3, 1, 3, 1, 3, 1, 5, 3, 1, 3, 1, 3, 1, 3,
                          5, 5, 5, 5}, 4);
        case 19:
            return times({1, 2, 30, 1, 2, 30, 1, 2, 2, 1, 2, 30, 1, 4, 5, 2, 1, 2, 30, 1, 2, 30, 1, 2, 30, 1,
                          2, 30, 5, 4, 5, 5}, 4);
        case 20:
            return times({1, 3, 3, 1, 5, 3, 3, 1, 3, 3, 1, 3, 5, 3, 3, 1, 2, 1, 5, 2, 2, 1, 2, 1, 5, 2, 2, 1,
                          2, 2, 5, 5}, 4);
    }
    return v;
}

int specRoll(int c, int k) {
    const int q = k % 32;
    auto in = [&](std::initializer_list<std::pair<int, int>> l) {
        for (auto& p : l)
            if (p.first == q) return p.second;
        return 0;
    };
    switch (c) {
        case 2:
            for (auto p : {std::pair<int, int>{0, 8}, {22, 10}, {41, 12}, {82, 10}, {117, 11}, {126, 6}})
                if (p.first == k) return p.second;
            return 0;
        case 3: return (k <= 28 || k == 127) ? 10 : 0;
        case 4: return k % 8 == 7 ? 12 : 0;
        case 7: return in({{13, 9}, {14, 12}, {27, 12}, {28, 12}, {29, 7}, {30, 11}});
        case 8: return q >= 24 ? 12 : 0;
        case 9: return in({{5, 10}, {6, 11}, {7, 9}, {13, 10}, {14, 11}, {15, 9}, {21, 10}, {22, 11}, {23, 9},
                           {29, 10}, {30, 11}, {31, 9}});
        case 10: return in({{0, 8}, {1, 7}, {2, 12}, {8, 8}, {9, 7}, {10, 12}, {16, 8}, {17, 7}, {18, 12},
                            {24, 8}, {25, 7}, {26, 12}});
        case 13: return k % 8 == 0 ? 13 : 0;
        case 14: return k % 4 == 0 ? 13 : 0;
        case 15: return 15;
        default: return 0;
    }
}

}  // namespace

// --- tables ------------------------------------------------------------------

TEST(playmode_tables_match_spec) {
    CHECK_EQ(sizeof(seq::kPlayModeTables), size_t(5120));
    for (int c = 1; c <= 20; ++c) {
        const auto st = specSteps(c);
        CHECK_EQ(st.size(), size_t(128));
        int bad = 0;
        for (int k = 0; k < 128 && k < int(st.size()); ++k) {
            if (seq::kPlayModeTables[c - 1][k][0] != st[size_t(k)]) ++bad;
            if (seq::kPlayModeTables[c - 1][k][1] != specRoll(c, k)) ++bad;
        }
        if (bad) std::printf("       table C%d: %d mismatches\n", c, bad);
        CHECK_EQ(bad, 0);
    }
    CHECK_STR(seq::playModeLabel(0), "Fwd");
    CHECK_STR(seq::playModeLabel(2), "Rnd");
    CHECK_STR(seq::playModeLabel(3), "C1");
    CHECK_STR(seq::playModeLabel(22), "C20");
}

// Drive the sequencer in every table mode and check the step order on track 2
// for L = 127 (straight table) and L = 11 (k wraps at L, stepByte mod L+1).
TEST(playmode_table_step_order) {
    for (int lastStep : {127, 11}) {
        for (int mode = 3; mode <= 22; ++mode) {
            Rig r;
            for (auto& t : r.a.tracks) {
                t.lastStep = uint8_t(lastStep);
                for (int s = 0; s < 128; ++s) t.steps[s].note = int8_t(s);
            }
            r.seq.setPerformanceMode(true);
            r.seq.setPerfPlayMode(2, uint8_t(mode));
            r.seq.play(0);
            const int len = lastStep + 1;
            const size_t n = size_t(len) * 2 + 3;
            r.steps(2, n);
            const auto st = specSteps(mode - 2);
            std::vector<int> want;
            for (size_t i = 0; i < n; ++i) want.push_back((st[i % size_t(len)] - 1) % len);
            const bool ok = eq(r.sink.notes[2], want);
            if (!ok) std::printf("       mode %d L %d\n", mode, lastStep);
            CHECK(ok);
            // Track 3 (mode 0) is unaffected.
            std::vector<int> fwd;
            for (size_t i = 0; i < n; ++i) fwd.push_back(int(i % size_t(len)));
            CHECK(eq(r.sink.notes[3], fwd));
        }
    }
}

TEST(playmode_fwd_back_rnd) {
    Rig r;
    for (auto& t : r.a.tracks) {
        t.lastStep = 5;
        for (int s = 0; s < 128; ++s) t.steps[s].note = int8_t(s);
    }
    r.seq.setPerformanceMode(true);
    r.seq.setPerfPlayMode(1, seq::kPlayBack);
    r.seq.setPerfPlayMode(2, seq::kPlayRnd);
    r.seq.setPerfPlayMode(9, seq::kPlayBack);  // 2.x: tracks 8..15 take play modes too (M1-WP4)
    r.seq.play(0);
    r.steps(0, 14);
    CHECK(eq(r.sink.notes[0], {0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 0, 1}));
    CHECK(eq(r.sink.notes[1], {5, 4, 3, 2, 1, 0, 5, 4, 3, 2, 1, 0, 5, 4}));
    CHECK_EQ(r.seq.step(9), r.seq.step(1));     // backward, like track 1 (stock: forward)
    bool varied = false;
    for (int v : r.sink.notes[2]) {
        CHECK(v >= 0 && v <= 5);
        if (v != r.sink.notes[2][0]) varied = true;
    }
    CHECK(varied);
    // Revert to Fwd re-syncs the step to the master position.
    r.seq.setPerfPlayMode(1, seq::kPlayFwd);
    const size_t at = r.sink.notes[1].size();
    r.steps(1, 3);
    CHECK(eq(r.tail(1, at), r.tail(0, at)));
}

// --- roll injection ------------------------------------------------------------

TEST(roll_selector_rules) {
    CHECK(seq::kFixStockRollCarry);
    seq::ParkMiller rng(1234);
    StepFx inj{3, 7};
    seq::applyRollSelector(inj, 0, rng);
    CHECK_EQ(inj.type, 0);
    CHECK_EQ(inj.value, 0);
    // 100 % selectors (rollSel-1) % 3 == 2: value by group.
    const int expect[5] = {4, 13, 11, 1, 10};
    for (int g = 0; g < 5; ++g) {
        StepFx f{};
        seq::applyRollSelector(f, uint8_t(g * 3 + 3), rng);
        CHECK_EQ(f.type, 3);
        CHECK_EQ(f.value, expect[g]);
    }
    // Hit rates 30 / 60 %.
    for (int sel : {1, 2, 4, 8}) {
        int hits = 0;
        for (int i = 0; i < 10000; ++i) {
            StepFx f{};
            seq::applyRollSelector(f, uint8_t(sel), rng);
            if (f.type == 3) ++hits;
        }
        const int want = ((sel - 1) % 3 == 0) ? 3000 : 6000;
        CHECK(hits > want - 300 && hits < want + 300);
    }
    // A miss clears only the type.
    for (int i = 0; i < 100; ++i) {
        StepFx f{3, 9};
        seq::ParkMiller probe = rng;
        if (probe.range(0, 100) < 30) { rng.next(); continue; }
        seq::applyRollSelector(f, 1, rng);
        CHECK_EQ(f.type, 0);
        CHECK_EQ(f.value, 9);
        break;
    }
}

// Stock bug: a roll-selector-2 miss leaves the previous roll (OT fixes it).
TEST(roll_selector2_miss_fixed) {
    seq::ParkMiller rng(99);
    // Find a state whose draw misses the 60 % threshold.
    for (int i = 0; i < 100; ++i) {
        seq::ParkMiller probe = rng;
        if (probe.range(0, 100) >= 60) break;
        rng.next();
    }
    seq::ParkMiller rng2 = rng;
    StepFx fixed{3, 4}, stock{3, 4};
    seq::applyRollSelector(fixed, 2, rng, true);
    seq::applyRollSelector(stock, 2, rng2, false);
    CHECK_EQ(fixed.type, 0);
    CHECK_EQ(stock.type, 3);  // stock behaviour kept behind the flag
    CHECK_EQ(stock.value, 4);
}

TEST(roll_injection_in_table_modes) {
    Rig r;
    for (auto& t : r.a.tracks) t.lastStep = 31;
    r.seq.setPerformanceMode(true);
    r.seq.setPerfPlayMode(0, 17);  // C15: rollSel 15 on every step -> Roll value 10
    r.seq.setPerfPlayMode(1, 3);   // C1: no rolls
    r.seq.setPerfInjectedFx(1, 3, 4);
    r.seq.play(0);
    // Evaluated with k = 0 at playback start.
    CHECK_EQ(r.seq.perfInjectedFx(0).type, 3);
    CHECK_EQ(r.seq.perfInjectedFx(0).value, 10);
    CHECK_EQ(r.seq.perfInjectedFx(1).type, 0);
    r.steps(0, 4);
    CHECK_EQ(r.seq.perfInjectedFx(0).type, 3);
    // The injected roll lands in the first empty column (FX-B) of each step.
    bool sawRoll = false;
    for (auto& n : r.sink.raw[0])
        if (n.fxB.type == 3 && n.fxB.value == 10) sawRoll = true;
    CHECK(sawRoll);
    for (auto& n : r.sink.raw[1]) CHECK(n.fxB.type == 0 && n.fxA.type == 0);
}

// --- per-track pattern override (spec 12 §C) -------------------------------------

TEST(track_pattern_needs_memory) {
    Rig r;
    r.seq.setPerfPatternMemory(nullptr);
    CHECK(!r.seq.setTrackPattern(0, 2, true));
    r.seq.setPerfPatternMemory(r.mem.get());
    CHECK(!r.seq.setTrackPattern(16, 2, true));  // tracks 0..15 (2.x: 8..15 too, M1-WP4)
    CHECK(!r.seq.setTrackPattern(0, 0, true));
    CHECK(r.seq.setTrackPattern(0, 2, true));
    CHECK(r.seq.performanceMode());              // entering sets perf mode
    CHECK_EQ(r.seq.trackPattern(0), 2);          // stopped: committed now
}

TEST(track_pattern_queued_switch_at_track_loop_end) {
    Rig r;
    r.seq.setPerformanceMode(true);
    r.seq.play(0);
    r.steps(0, 3);                               // steps 0,1,2 started
    CHECK(r.seq.setTrackPattern(0, 2, false));
    CHECK_EQ(r.seq.queuedTrackPattern(0), 2);
    CHECK_EQ(r.seq.trackPattern(0), 0);
    r.steps(0, 7);
    // Track 0 finishes pattern 1 (steps 3..7), then plays pattern 2 from step 0.
    CHECK(eq(r.sink.notes[0], {16, 17, 18, 19, 20, 21, 22, 23, 32, 33}));
    CHECK(eq(r.sink.notes[1], {16, 17, 18, 19, 20, 21, 22, 23, 16, 17}));
    CHECK_EQ(r.seq.trackPattern(0), 2);
    CHECK_EQ(r.seq.queuedTrackPattern(0), 0);
    // The editor's pattern (current play buffer) is not modified.
    CHECK_EQ(r.a.tracks[0].steps[0].note, 16);
    // Pattern-mode loop: the override persists.
    r.steps(0, 10);
    CHECK(eq(r.tail(0, 10), {34, 35, 36, 37, 38, 39, 32, 33, 34, 35}));
    // A transport stop keeps it (spec 12 §C.4).
    r.seq.stop();
    r.seq.play(0);
    const size_t at = r.sink.notes[0].size();
    r.steps(0, 2);
    CHECK(eq(r.tail(0, at), {32, 33}));
}

TEST(track_pattern_queue_own_length_and_replace) {
    Rig r;
    r.host.len[3] = 3;   // pattern 3 is 4 steps long
    r.seq.setPerformanceMode(true);
    r.seq.play(0);
    r.steps(0, 2);
    r.seq.setTrackPattern(0, 2, false);
    r.seq.setTrackPattern(0, 3, false);          // last one wins
    r.steps(0, 14);
    // Wrap at step 7, then pattern 3 loops on its own 4-step length.
    CHECK(eq(r.sink.notes[0], {16, 17, 18, 19, 20, 21, 22, 23, 48, 49, 50, 51, 48, 49, 50, 51}));
    CHECK(eq(r.sink.notes[1], {16, 17, 18, 19, 20, 21, 22, 23, 16, 17, 18, 19, 20, 21, 22, 23}));
}

TEST(track_pattern_queued_backward_commits_below_zero) {
    Rig r;
    r.seq.setPerformanceMode(true);
    r.seq.setPerfPlayMode(0, seq::kPlayBack);
    r.seq.play(0);
    r.steps(0, 2);                               // 7, 6
    r.seq.setTrackPattern(0, 2, false);
    r.steps(0, 8);
    CHECK(eq(r.sink.notes[0], {23, 22, 21, 20, 19, 18, 17, 16, 39, 38}));
}

TEST(track_pattern_immediate_switch) {
    Rig r;
    r.host.len[3] = 3;
    r.seq.setPerformanceMode(true);
    r.seq.play(0);
    r.steps(0, 3);                               // 0,1,2
    r.seq.setTrackPattern(0, 2, true);
    CHECK_EQ(r.seq.trackPattern(0), 0);          // takes effect at the next step
    r.steps(0, 2);
    CHECK_EQ(r.seq.trackPattern(0), 2);
    CHECK(eq(r.sink.notes[0], {16, 17, 18, 35, 36}));
    // Shorter pattern while beyond its length: step := master mod (L+1).
    r.steps(0, 2);                               // steps 5, 6
    r.seq.setTrackPattern(0, 3, true);
    r.steps(0, 3);
    // Master step 7 -> 7 mod 4 = 3, then wrap at the new length.
    CHECK(eq(r.tail(0, 7), {51, 48, 49}));
    CHECK(eq(r.tail(1, 7), {23, 16, 17}));
}

TEST(track_pattern_discarded_on_queued_pattern_change) {
    Rig r;
    r.seq.setPerformanceMode(true);
    r.seq.play(0);
    r.steps(0, 1);
    r.seq.setTrackPattern(0, 2, true);
    r.steps(0, 2);
    CHECK_EQ(r.seq.trackPattern(0), 2);
    r.seq.queuePattern(+3);                      // pattern 4 at the next pattern end
    r.steps(0, 8);
    CHECK_EQ(r.seq.currentPatternNumber(), 4);
    CHECK_EQ(r.seq.trackPattern(0), 0);
    CHECK(eq(r.tail(0, 8), {64, 65}));
}

TEST(track_pattern_song_advance_and_pending_queue) {
    Rig r;
    r.project.playlist[0] = 1;
    r.project.playlist[1] = 2;
    r.project.playlist[2] = 3;
    r.seq.setPerformanceMode(true);
    r.seq.playSong(0);
    r.steps(0, 1);
    r.seq.setTrackPattern(1, 5, true);           // immediate on track 1
    r.steps(0, 3);
    CHECK_EQ(r.seq.trackPattern(1), 5);
    r.steps(0, 6);                               // song advances to pattern 2
    CHECK_EQ(r.seq.currentPatternNumber(), 2);
    CHECK_EQ(r.seq.trackPattern(1), 0);          // discarded by the song advance
    CHECK(eq(r.tail(1, 8), {32, 33}));
    // A queue that commits at the same boundary as the song advance survives
    // (OT fix; stock commits into the outgoing buffer and loses it).
    r.seq.setTrackPattern(1, 6, false);
    r.steps(0, 8);                               // into pattern 3
    CHECK_EQ(r.seq.currentPatternNumber(), 3);
    CHECK_EQ(r.seq.trackPattern(1), 6);
    CHECK(eq(r.tail(1, 16), {96, 97}));
    CHECK(eq(r.tail(0, 16), {48, 49}));
}

// Issue #86 report 3 (stock 1.9.2, issue #86 stock check D4-D6): the
// Performance pattern length is global, so in Song mode the song moves on
// after the shortened length.
TEST(global_length_moves_the_song_on) {
    Rig r;
    r.project.playlist[0] = 1;
    r.project.playlist[1] = 2;
    r.project.playlist[2] = 3;
    r.seq.setPerformanceMode(true);
    r.seq.setPerfGlobalLength(3);                // 4 steps
    r.seq.playSong(0);
    r.steps(0, 12);
    CHECK(eq(r.tail(0, 0), {16, 17, 18, 19, 32, 33, 34, 35, 48, 49, 50, 51}));
    CHECK_EQ(r.seq.currentPatternNumber(), 3);
    // Length 8: back to pattern 1 (playlist end), 8 steps per pattern.
    r.seq.setPerfGlobalLength(7);
    size_t from = r.sink.notes[0].size();
    r.steps(0, 16);
    CHECK(eq(r.tail(0, from), {52, 53, 54, 55, 16, 17, 18, 19, 20, 21, 22, 23, 32, 33, 34, 35}));
    // Shortened below the position already played: moves on at the next step.
    r.seq.setPerfGlobalLength(1);
    from = r.sink.notes[0].size();
    r.steps(0, 3);
    CHECK(eq(r.tail(0, from), {48, 49, 16}));
    // Cleared: the patterns' own length again.
    r.seq.setPerfGlobalLength(-1);
    from = r.sink.notes[0].size();
    r.steps(0, 9);
    CHECK(eq(r.tail(0, from), {17, 18, 19, 20, 21, 22, 23, 32, 33}));
}

// Stock bug: the per-track loader used the preload buffer as scratch, so a
// load during the last step clobbered the next song pattern (spec 12 §C.1).
TEST(track_pattern_load_does_not_clobber_preload) {
    Rig r;
    r.project.playlist[0] = 1;
    r.project.playlist[1] = 2;
    r.seq.setPerformanceMode(true);
    r.seq.playSong(0);
    r.steps(0, 8);                               // last step: preload of pattern 2 issued
    CHECK_EQ(r.seq.preloadPattern()->tracks[3].steps[0].note, 32);
    r.host.loads.clear();
    r.seq.setTrackPattern(0, 7, false);
    CHECK_EQ(r.host.loads.size(), size_t(1));
    CHECK(r.host.loads[0].second == &r.mem->scratch);
    CHECK(r.host.loads[0].second != r.seq.preloadPattern());
    CHECK_EQ(r.seq.preloadPattern()->tracks[3].steps[0].note, 32);
    r.steps(0, 2);
    CHECK_EQ(r.seq.currentPatternNumber(), 2);
    CHECK(eq(r.tail(3, 8), {32, 33}));
    CHECK(eq(r.tail(0, 8), {112, 113}));         // the queue committed on top
}

TEST(track_pattern_revert_on_performance_stop) {
    Rig r;
    r.seq.setPerformanceMode(true);
    r.seq.setPerfPlayMode(2, seq::kPlayBack);
    r.seq.play(0);
    r.steps(0, 1);
    r.seq.setTrackPattern(0, 2, true);
    r.seq.setTrackPattern(1, 3, false);
    r.steps(0, 2);
    CHECK_EQ(r.seq.trackPattern(0), 2);
    CHECK_EQ(r.seq.queuedTrackPattern(1), 3);
    r.seq.setPerformanceMode(false);
    CHECK(!r.seq.performanceMode());
    CHECK_EQ(r.seq.trackPattern(0), 0);
    CHECK_EQ(r.seq.queuedTrackPattern(1), 0);
    CHECK_EQ(r.seq.perfPlayMode(2), 0);
    r.steps(0, 8);
    // All tracks back on the current pattern, track 2 re-synced to forward.
    CHECK(eq(r.tail(0, 3), {19, 20, 21, 22, 23, 16, 17, 18}));
    CHECK(eq(r.tail(1, 3), {19, 20, 21, 22, 23, 16, 17, 18}));
    CHECK(eq(r.tail(2, 3), {19, 20, 21, 22, 23, 16, 17, 18}));
}

TEST(track_pattern_stopped_queue_applies_after_start) {
    Rig r;
    r.seq.setTrackPattern(4, 2, false);
    CHECK_EQ(r.seq.queuedTrackPattern(4), 2);
    r.seq.play(0);
    r.steps(4, 10);
    CHECK(eq(r.sink.notes[4], {16, 17, 18, 19, 20, 21, 22, 23, 32, 33}));
}
