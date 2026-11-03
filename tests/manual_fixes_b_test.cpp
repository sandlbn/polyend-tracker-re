// Manual-audit fixes, batch B (docs/MANUAL_AUDIT.md): Sample Playback play-mode
// list, Song paste at the grid position, mixer value formats, export naming,
// Performance label colours, File and Games labels.
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "test.h"
#include "ui_fakes.h"
#include "nvstore_fake.h"

#include "app/instrument_edit.h"
#include "app/perf_presets.h"

#include "app/project_fields.h"
#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;

namespace {
SongScreen& song(Harness& h) { return *static_cast<SongScreen*>(h.app->ui.screen(kModSong)); }
MixerScreen& mixer(Harness& h) { return *static_cast<MixerScreen*>(h.app->ui.screen(kModMaster)); }
const BarLabel& lab(Harness& h, int i) { return h.app->ui.bar.label[i]; }

void loadSaw(Harness& h) {
    std::vector<int16_t> s(8000);
    for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t((int(i) % 200 - 100) * 250);
    std::vector<uint8_t> w(s.size() * 2 + 64);
    w.resize(wavEncode(s.data(), uint32_t(s.size()), 1, w.data(), w.size()));
    h.fs.files["saw.wav"] = w;
    CHECK(h.app->importSample("saw.wav", 0));
    h.app->setCurrentInstrument(0);
}
}  // namespace

// Audit 6.1: the 8-item play-mode list is drawn, with the manual's names.
TEST(playback_draws_play_mode_list) {
    Harness h;
    loadSaw(h);
    h.tap(btn::SamplePlayback);
    h.app->ui.dirty();
    h.run(40);
    for (const char* n : {"1-Shot", "Forward Loop", "Backward Loop", "Pingpong Loop", "Slice", "Beat Slice",
                          "Wavetable", "Granular"})
        CHECK(h.disp.lastHasText(n));
    CHECK_STR(lab(h, 6).bottom, "1-Shot");
}

// Audit 9.1: the paste goes to the grid selection's top-left, track offset
// included, also within the same pattern.
TEST(song_paste_uses_grid_position) {
    Harness h;
    App& a = *h.app;
    a.project.playlist[0] = 1;
    a.project.playlist[1] = 2;
    a.project.playlist[2] = 3;
    a.project.playlist[3] = 0;
    a.selectPattern(1);
    a.pattern().tracks[0].steps[0].note = 60;
    a.storePattern(1, a.pattern());
    h.tap(btn::Song);
    SongScreen& s = song(h);
    h.tap(btn::Rec);
    h.tap(btn::Left);   // list -> grid (row = cursor 0, last track)
    CHECK_EQ(s.focus, 0);
    for (int i = 0; i < kTrackCount; ++i) h.tap(btn::Left);
    CHECK_EQ(s.selTrack0, 0);
    CHECK_EQ(s.selRow0, 0);
    h.tap(btn::Copy);   // copy slot 0, track 0
    h.tap(btn::Down);
    h.tap(btn::Right);
    h.tap(btn::Right);
    CHECK_EQ(s.selRow0, 1);
    CHECK_EQ(s.selTrack0, 2);
    h.down(btn::Shift);
    h.tap(btn::Copy);   // paste -> pattern 2, track 2
    h.up(btn::Shift);
    h.run(40);
    CHECK_EQ(a.stored(2).tracks[2].steps[0].note, 60);
    CHECK(a.stored(2).tracks[0].steps[0].note < 0);
    CHECK(a.stored(3).tracks[2].steps[0].note < 0);   // not the list cursor's slot + 1
    // Same pattern, other track.
    h.tap(btn::Up);
    h.tap(btn::Left);
    CHECK_EQ(s.selRow0, 0);
    CHECK_EQ(s.selTrack0, 1);
    h.down(btn::Shift);
    h.tap(btn::Copy);
    h.up(btn::Shift);
    h.run(40);
    CHECK_EQ(a.stored(1).tracks[1].steps[0].note, 60);
    CHECK_EQ(a.stored(1).tracks[0].steps[0].note, 60);
}

// Audit 10.1 / 10.2: delay sync rate and sidechain shown as names.
TEST(mixer_delay_rate_and_sidechain_names) {
    Harness h;
    App& a = *h.app;
    a.project.setU8(pf::kDelayFlags, 0x40 | 5);
    a.project.setU8(pf::kSidechain, 9);
    h.tap(btn::Master);
    MixerScreen& m = mixer(h);
    CHECK_EQ(m.page, 0);
    h.tap(btn::F3);
    CHECK_EQ(m.sub, 2);
    CHECK_STR(lab(h, 2).bottom, "3/2");
    a.project.setU8(pf::kDelayFlags, 0x40 | 18);
    h.tap(btn::Right);
    CHECK_STR(lab(h, 2).bottom, "1/32");
    h.tap(btn::F8);
    h.tap(btn::F4);
    CHECK_EQ(m.sub, 3);
    CHECK_STR(lab(h, 4).bottom, "Line in L");
    a.project.setU8(pf::kSidechain, 0);
    h.tap(btn::Right);
    CHECK_STR(lab(h, 4).bottom, "Disable");
    a.project.setU8(pf::kSidechain, 3);
    a.project.setTrackName(2, "Kick");
    h.tap(btn::Right);
    CHECK_STR(lab(h, 4).bottom, "Kick");   // the track's current name
}

// Spec 16 §B.6: master volume v/100*63.5 - 51.5 dB, 81 = exactly 0 dB.
TEST(mixer_master_volume_in_db) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Master);
    a.cfg.masterVolume = 81;
    h.tap(btn::Right);
    CHECK_STR(lab(h, 0).bottom, "0.00 dB");
    a.cfg.masterVolume = 0;
    h.tap(btn::Right);
    CHECK_STR(lab(h, 0).bottom, "-51.50 dB");
    a.cfg.masterVolume = 100;
    h.tap(btn::Right);
    CHECK_STR(lab(h, 0).bottom, "12.00 dB");
    a.cfg.masterVolume = 50;
    h.tap(btn::Right);
    CHECK_STR(lab(h, 0).bottom, "-19.75 dB");
}



// Spec 16 §A.12: a pattern export writes <proj>_P<n>_NNNN/master.wav; a
// cancelled one leaves no folder behind.
TEST(export_pattern_folder_with_master_wav) {
    Harness h;
    App& a = *h.app;
    char p[160];
    a.selectPattern(3);
    CHECK(a.exportName(App::ExportKind::Pattern, p, sizeof(p)));
    CHECK_STR(p, "Export/New Project/New Project_P3_0001/master.wav");
    CHECK(a.exportAudio(App::ExportKind::Pattern, p));
    CHECK(h.fs.files.count(p));
    CHECK(h.fs.dirs.count("Export/New Project/New Project_P3_0001"));
    CHECK(a.exportName(App::ExportKind::Pattern, p, sizeof(p)));
    CHECK_STR(p, "Export/New Project/New Project_P3_0002/master.wav");
    h.input.q.push_back(InputEvent{});
    h.input.q.back().type = InputEvent::kButtonDown;
    h.input.q.back().id = btn::F8;
    CHECK(!a.exportAudio(App::ExportKind::Pattern, p));
    CHECK(!h.fs.files.count(p));
    CHECK(!h.fs.dirs.count("Export/New Project/New Project_P3_0002"));
}

namespace {
struct PerfRig {
    FakeNvStore nv;
    std::unique_ptr<PerfPatternMemory> mem{new PerfPatternMemory};
    Harness h{false};
    PerfRig() {
        h.app->setNvStore(&nv);
        h.app->seq.setPerfPatternMemory(mem.get());
        h.app->boot();
    }
    PerformanceScreen& p() { return *static_cast<PerformanceScreen*>(h.app->ui.screen(kModPerformance)); }
};
}  // namespace

// Spec 16 §A.15: Performance stays live on Instrument Params and Master;
// stopped, any other screen reverts at once.
TEST(perf_stays_live_on_instr_params_and_master) {
    PerfRig r;
    App& a = *r.h.app;
    r.h.tap(btn::Performance);
    PerformanceScreen& p = r.p();
    r.h.pad(24 + 1);
    const int row = p.src.row(1);
    CHECK(row != 0);
    r.h.tap(btn::F2);   // FX on for track 2 (all start off, issue #86)
    CHECK(p.fxOn[1]);
    r.h.tap(btn::InstrParams);
    CHECK(a.ui.active() == kModInstrument);
    CHECK(a.seq.performanceMode());
    CHECK(p.keepAlive() == PerformanceScreen::KeepAlive::Always);
    r.h.tap(btn::Performance);
    CHECK_EQ(p.src.row(1), row);   // resumed, rows kept
    CHECK(p.fxOn[1]);
    r.h.tap(btn::Master);
    CHECK(a.seq.performanceMode());
    r.h.tap(btn::Performance);
    CHECK_EQ(p.src.row(1), row);
    r.h.tap(btn::Pattern);   // stopped: immediate revert
    CHECK(!a.seq.performanceMode());
    CHECK(p.keepAlive() == PerformanceScreen::KeepAlive::None);
    r.h.tap(btn::Performance);
    CHECK_EQ(p.src.row(1), 0);   // fresh entry
    CHECK(!p.fxOn[1]);
}

// Spec 16 §A.15: the stock handler runs on every screen key while the FX are
// live. From Instrument Params (kept indefinitely) a key to another screen
// reverts at once when stopped, and keeps them to the pattern end when playing.
TEST(perf_screen_key_from_instr_params_reverts_like_stock) {
    PerfRig r;
    App& a = *r.h.app;
    r.h.tap(btn::Performance);
    r.h.pad(24 + 1);
    r.h.tap(btn::InstrParams);
    CHECK(r.p().keepAlive() == PerformanceScreen::KeepAlive::Always);
    r.h.tap(btn::Master);   // Instrument Params / Master keep them
    CHECK(r.p().keepAlive() == PerformanceScreen::KeepAlive::Always);
    r.h.tap(btn::File);   // stopped: immediate revert
    CHECK(a.ui.active() == kModFile);
    CHECK(!a.seq.performanceMode());
    CHECK(r.p().keepAlive() == PerformanceScreen::KeepAlive::None);
    // Playing: until the pattern end.
    r.h.tap(btn::Performance);
    r.h.pad(24 + 1);
    r.h.tap(btn::InstrParams);
    r.h.tap(btn::Play);
    CHECK(a.seq.isPlaying());
    r.h.tap(btn::Pattern);
    CHECK(a.seq.performanceMode());
    CHECK(r.p().keepAlive() == PerformanceScreen::KeepAlive::UntilPatternEnd);
    a.seq.stop();
    r.h.run(5);
    CHECK(!a.seq.performanceMode());
    CHECK(r.p().keepAlive() == PerformanceScreen::KeepAlive::None);
}

// Exports run with the Performance FX reverted: kept live on Instrument
// Params (stopped) or to the pattern end on another screen (playing), the
// export equals the export made without them.
TEST(perf_export_while_fx_alive_equals_baseline) {
    PerfRig r;
    App& a = *r.h.app;
    {
        std::vector<int16_t> smp(3000);
        for (uint32_t i = 0; i < smp.size(); ++i) smp[i] = int16_t((i % 100) * 300 - 15000);
        std::vector<uint8_t> w(smp.size() * 2 + 64);
        w.resize(wavEncode(smp.data(), uint32_t(smp.size()), 1, w.data(), w.size()));
        r.h.fs.files["click.wav"] = w;
    }
    CHECK(a.importSample("click.wav", 0));
    for (int s = 0; s < 16; s += 4) {
        a.pattern().tracks[0].steps[s].note = 60;
        a.pattern().tracks[0].steps[s].instrument = 0;
    }
    a.patternEdited();
    auto exportBytes = [&]() {
        char p[160];
        CHECK(a.exportName(App::ExportKind::Pattern, p, sizeof(p)));
        CHECK(a.exportAudio(App::ExportKind::Pattern, p));
        return r.h.fs.files[p];
    };
    exportBytes();   // warm-up: the first export after boot starts from the live engine state
    const std::vector<uint8_t> base = exportBytes();
    CHECK(base.size() > 44100);
    CHECK(exportBytes() == base);   // the export itself is repeatable (after the warm-up)
    int loud = 0;
    for (size_t i = 44; i + 1 < base.size(); i += 2) loud += std::abs(int16_t(base[i] | (base[i + 1] << 8))) > 1000;
    CHECK(loud > 100);
    auto liveVolume = [&]() {
        r.h.tap(btn::Performance);
        PerformanceScreen& p = r.p();
        p.src.setType(0, perf::kVolume);
        p.src.setValue(0, 1, -80);
        r.h.tap(btn::F1);   // track 1 FX on (all start off, issue #86)
        r.h.pad(12 + 0);
        CHECK_EQ(p.src.row(0), 1);
        CHECK(a.engine.voice(0).perf().volume != 0);
    };
    // Kept indefinitely on Instrument Params, stopped.
    liveVolume();
    r.h.tap(btn::InstrParams);
    CHECK(r.p().keepAlive() == PerformanceScreen::KeepAlive::Always);
    CHECK(exportBytes() == base);
    CHECK(!a.seq.performanceMode());
    CHECK(r.p().keepAlive() == PerformanceScreen::KeepAlive::None);
    // Kept to the pattern end on another screen while playing.
    liveVolume();
    r.h.tap(btn::Play);
    r.h.tap(btn::Pattern);
    CHECK(r.p().keepAlive() == PerformanceScreen::KeepAlive::UntilPatternEnd);
    CHECK(exportBytes() == base);
    CHECK(!a.seq.performanceMode());
}

// Spec 16 §A.15: playing, another screen keeps the FX until the pattern ends.
TEST(perf_other_screen_while_playing_reverts_at_pattern_end) {
    PerfRig r;
    App& a = *r.h.app;
    r.h.tap(btn::Performance);
    r.h.tap(btn::Play);
    CHECK(a.seq.isPlaying());
    r.h.tap(btn::Song);
    CHECK(a.ui.active() == kModSong);
    CHECK(a.seq.performanceMode());
    CHECK(r.p().keepAlive() == PerformanceScreen::KeepAlive::UntilPatternEnd);
    const uint32_t e0 = a.patternEndCount();
    for (int i = 0; i < 20000 && a.patternEndCount() == e0; ++i) r.h.audio.pump(1);
    CHECK(a.patternEndCount() != e0);
    r.h.run(20);
    CHECK(!a.seq.performanceMode());
    CHECK(r.p().keepAlive() == PerformanceScreen::KeepAlive::None);
    a.seq.stop();
}

// Audit 12.1 / spec 16 §A.14: highlighted label = FX on (every track starts
// off, issue #86), dim text = muted; the live cell is drawn on top.
TEST(perf_label_colours_and_live_cell_on_top) {
    PerfRig r;
    App& a = *r.h.app;
    r.h.tap(btn::Performance);
    PerformanceScreen& p = r.p();
    CHECK(!a.ui.bar.label[0].highlight);
    CHECK(a.ui.bar.label[0].enabled);
    r.h.tap(btn::F1);   // FX on for track 1
    CHECK(p.fxOn[0]);
    CHECK(a.ui.bar.label[0].highlight);
    a.setTrackMute(1, true);
    r.h.tap(btn::Right);
    a.ui.dirty();
    r.h.run(200);
    CHECK(!a.ui.bar.label[1].enabled);
    CHECK(!a.ui.bar.label[1].highlight);
    a.setTrackMute(1, false);
    // Live value cell of column 0: the title band (text centre y 53) above
    // the four rows [S snap 021].
    p.liveValid[0] = true;
    p.live[0] = 25;
    a.ui.dirty();
    r.h.run(40);
    bool top = false;
    for (auto& l : r.h.disp.last)
        if (l.find("TEXT") == 0 && l.find("\"+25\"") != std::string::npos && l.find(" 53 ") != std::string::npos) top = true;
    CHECK(top);
}

TEST(automation_destination_and_type_up_moves_up) {
    // User report: on Instrument Automation 2/2, Up/Down were reversed in
    // the Destination and Type lists. Down moves down the list (Volume ->
    // Panning, Off -> Envelope), Up moves back up.
    Harness h;
    App& a = *h.app;
    a.setCurrentInstrument(0);
    h.tap(btn::InstrParams);
    auto& s = *static_cast<InstrumentScreen*>(a.ui.screen(kModInstrument));
    if (s.mode != 1) h.tap(btn::InstrParams);
    CHECK_EQ(s.mode, 1);
    s.column = 0;
    s.dest = 0;
    h.tap(btn::Down);
    CHECK_EQ(s.dest, 1);
    h.tap(btn::Up);
    CHECK_EQ(s.dest, 0);
    s.column = 1;
    const int t0 = iedit::autoType(a.instruments[0], 0);
    h.tap(btn::Down);
    CHECK_EQ(iedit::autoType(a.instruments[0], 0), t0 + 1);
    h.tap(btn::Up);
    CHECK_EQ(iedit::autoType(a.instruments[0], 0), t0);
}

TEST(automation_volume_lfo_speed_list_starts_at_24_steps) {
    // Stock 1.9.2: the Volume LFO speeds run 24 steps .. 1/64 step, so its
    // Speed list starts at "24 steps"; the other destinations list 128..
    Harness h;
    App& a = *h.app;
    a.setCurrentInstrument(0);
    h.tap(btn::InstrParams);
    auto& s = *static_cast<InstrumentScreen*>(a.ui.screen(kModInstrument));
    if (s.mode != 1) h.tap(btn::InstrParams);
    for (int d = 0; d < 2; ++d) {
        CHECK(iedit::set(a.instruments[0], InstrParam::AutoType, d, 2));   // LFO
        s.dest = d;
        s.column = 3;
        a.ui.dirty();
        h.run(60);
        CHECK(h.disp.lastHasText("24 steps"));
        CHECK_EQ(h.disp.lastHasText("128 steps"), d != 0);
        CHECK_EQ(h.disp.lastHasText("32 steps"), d != 0);
    }
}

TEST(confirm_popup_hides_the_bar_selector) {
    // User report: "Slices will be rewritten" on Sample Playback drew
    // Cancel / Confirm over the screen's own "Play Mode" selector. While a
    // confirmation is open only its labels are in the bar.
    Harness h;
    App& a = *h.app;
    a.setCurrentInstrument(1);
    h.tap(btn::SamplePlayback);
    h.run(60);
    CHECK(h.disp.lastHasText("Play Mode"));
    a.ui.ask("Slices will be rewritten", "Do you want to continue?", nullptr, "Cancel", "Confirm",
             [](void*, int) {}, nullptr);
    h.run(60);
    CHECK(h.disp.lastHasText("Confirm"));
    CHECK(!h.disp.lastHasText("Play Mode"));
}

TEST(song_undo_without_data_shows_stock_text) {
    // Stock 1.9.2 (checked on the device): Song Undo with nothing to undo
    // shows "No undo data :(" (slot add/delete is not undoable there either).
    Harness h;
    App& a = *h.app;
    h.tap(btn::Song);
    h.tap(btn::Rec);    // edit mode
    h.tap(btn::F3);     // Undo
    h.run(60);
    CHECK(h.disp.lastHasText("No undo data :("));
    (void)a;
}

TEST(automation_lfo_shape_up_moves_up) {
    // User report: with Type = LFO, Up/Down were reversed in the Shape list
    // (Rev saw, Saw, Triangle, Square, Random from the top).
    Harness h;
    App& a = *h.app;
    a.setCurrentInstrument(0);
    h.tap(btn::InstrParams);
    auto& s = *static_cast<InstrumentScreen*>(a.ui.screen(kModInstrument));
    if (s.mode != 1) h.tap(btn::InstrParams);
    CHECK(iedit::set(a.instruments[0], InstrParam::AutoType, 0, 2));   // LFO
    s.dest = 0;
    s.column = 2;
    const int sh0 = iedit::get(a.instruments[0], InstrParam::AutoAttackShape, 0);
    h.tap(btn::Down);
    CHECK_EQ(iedit::get(a.instruments[0], InstrParam::AutoAttackShape, 0), sh0 + 1);
    h.tap(btn::Up);
    CHECK_EQ(iedit::get(a.instruments[0], InstrParam::AutoAttackShape, 0), sh0);
}
