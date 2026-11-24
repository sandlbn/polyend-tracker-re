// Performance and the Song screen (issue #185): the Song screen's play head
// for a run started on the Performance screen.
#include "test.h"
#include "ui_fakes.h"
#include "nvstore_fake.h"

#include "app/perf_presets.h"
#include "ui/screens.h"

using namespace ot;

namespace {
struct Rig {
    FakeNvStore nv;
    std::unique_ptr<PerfPatternMemory> mem{new PerfPatternMemory};
    Harness h{false};
    Rig() {
        h.app->setNvStore(&nv);
        h.app->seq.setPerfPatternMemory(mem.get());
        h.app->boot();
        // Song 1, 2, 2, 3 of 16-step patterns.
        App& a = *h.app;
        const uint8_t song[] = {1, 2, 2, 3, 0};
        for (int i = 0; i < 5; ++i) a.project.playlist[i] = song[i];
        for (auto& t : a.pattern().tracks) t.lastStep = 15;
        a.patternEdited();
        for (int n = 2; n <= 3; ++n) a.storePattern(n, a.pattern());
    }
    App& a() { return *h.app; }
    PerformanceScreen& perf() { return *static_cast<PerformanceScreen*>(h.app->ui.screen(kModPerformance)); }
    SongScreen& song() { return *static_cast<SongScreen*>(h.app->ui.screen(kModSong)); }
    // Performance with tracks 1-4 on and Tune -24 latched (the report's set-up).
    void perfSetup() {
        h.tap(btn::Performance);
        for (uint8_t k : {btn::F1, btn::F2, btn::F3, btn::F4}) h.tap(k);
        perf().src.setType(2, perf::kTune);
        perf().src.setValue(2, 1, -24);
        h.pad(12 + 2);
        CHECK_EQ(a().engine.voice(0).perf().tune, -24);
    }
};
}  // namespace

// Play on the Performance screen starts the current pattern (spec 06 §3.4).
// The Song screen showed no play head for it and its cursor stayed where it
// was. Stock moves the playing-slot marker with the sequencer's song position
// and the cursor follows it (spec 06 §2.6): the slot at the song position
// holds the pattern that plays, so it is marked.
TEST(song_shows_the_play_head_of_a_performance_run) {
    Rig r;
    App& a = r.a();
    r.h.tap(btn::Song);
    for (int i = 0; i < 3; ++i) r.h.tap(btn::Down);
    CHECK_EQ(r.song().cursor, 3);
    r.perfSetup();
    r.h.tap(btn::Play);
    CHECK(a.seq.isPlaying() && !a.seq.isSongMode());
    CHECK_EQ(a.patternNumber(), 1);
    r.h.tap(btn::Song);
    r.h.run(200);
    CHECK_EQ(r.song().transportState(), 5);
    CHECK_EQ(r.song().playingRow(), 0);
    CHECK_EQ(r.song().cursor, 0);   // follows the play head (not in edit mode)
    r.h.tap(btn::Down);             // browsing still works
    r.h.run(200);
    CHECK_EQ(r.song().cursor, 1);
    CHECK_EQ(r.song().playingRow(), 0);
    a.seq.stop();
}

// "Loop Pattern" belongs to the run it was set in. Looping a slot of the song,
// then stopping and starting again on the Performance screen, left the Song
// screen showing "Continue Song" and the loop icon for the new run, and F1
// then switched that pattern run into the song instead of stopping it.
TEST(song_loop_state_ends_with_its_run) {
    Rig r;
    App& a = r.a();
    r.h.tap(btn::Song);
    r.h.tap(btn::F2);   // Play Song
    r.h.tap(btn::F1);   // Loop Pattern at the next step
    for (int i = 0; i < 20 && !r.song().loopingInSong(); ++i) {
        r.h.audio.pump(10);
        r.h.run(20);
    }
    CHECK(r.song().loopingInSong());
    CHECK_EQ(r.song().transportState(), 3);
    r.perfSetup();
    r.h.tap(btn::Play);   // stop
    r.h.tap(btn::Play);   // a new run, from Performance
    CHECK(a.seq.isPlaying() && !a.seq.isSongMode());
    r.h.tap(btn::Song);
    r.h.run(200);
    CHECK(!r.song().loopingInSong());
    CHECK_EQ(r.song().transportState(), 5);   // Stop / Stop: a run started elsewhere
    CHECK_STR(a.ui.bar.label[0].top, "Stop");
    r.h.tap(btn::F1);
    CHECK(!a.seq.isPlaying());
}
