// Performance screen: preset storage (spec 12 §A), per-track patterns (§C),
// MIDI CC 41-48 / 51-62 (§B), FX conversion (§E.2), value labels and
// start / type-change rules (spec 06 §3). VERIFICATION_1TO1 P1-P3, P12-P14.
#include <cstring>

#include "test.h"
#include "ui_fakes.h"
#include "nvstore_fake.h"

#include "app/perf_presets.h"
#include "app/project_fields.h"
#include "formats/project_mt.h"
#include "ui/screens.h"

using namespace ot;

namespace {

struct PerfRig {
    FakeNvStore nv;
    std::unique_ptr<PerfPatternMemory> mem{new PerfPatternMemory};
    Harness h{false};
    explicit PerfRig(bool boot = true) {
        h.app->setNvStore(&nv);
        h.app->seq.setPerfPatternMemory(mem.get());
        if (boot) h.app->boot();
    }
    App& a() { return *h.app; }
    PerformanceScreen& p() { return *static_cast<PerformanceScreen*>(h.app->ui.screen(kModPerformance)); }
};

int16_t imgValue(const std::vector<uint8_t>& m, int s, int r) {
    const int o = Config::kOffPerfValues + 8 * s + 2 * r;
    return int16_t(uint16_t(m[o] | (m[o + 1] << 8)));
}

void putPattern(App& a, int num, uint8_t note0) {
    a.storePattern(num, a.pattern());
    PatternStock& p = a.storedStock(num);
    for (auto& t : p.tracks) t.steps[0].note = kNoteEmpty;
    p.tracks[0].steps[0].note = int8_t(note0);
    a.markPatternDirty(num);
}

}  // namespace

// ---- P1: presets ----------------------------------------------------------

TEST(perf_preset_defaults_and_labels) {
    PerfRig r;
    r.h.tap(btn::Performance);
    PerformanceScreen& p = r.p();
    CHECK_EQ(p.sourceIndex, 0);   // global by default (spec 05 §5.4)
    for (int s = 0; s < 12; ++s) {
        CHECK_EQ(p.src.type(s), s + 1);   // slots show FX 1..12
        CHECK_EQ(p.src.row(s), 0);
        for (int k = 0; k < 4; ++k) CHECK_EQ(p.src.value(s, k), 0);
    }
    // P13 value texts (spec 06 §3.2, spec 12 §D/§E.2).
    char t[16];
    perf::valueText(perf::kVolume, 0, t, sizeof t);
    CHECK_STR(t, perf::kZeroText);
    perf::valueText(perf::kVolume, 25, t, sizeof t);
    CHECK_STR(t, "+25");
    perf::valueText(perf::kTune, -7, t, sizeof t);
    CHECK_STR(t, "-7");
    perf::valueText(perf::kDirection, 1, t, sizeof t);
    CHECK_STR(t, "<<<");
    perf::valueText(perf::kRepeater, 0, t, sizeof t);
    CHECK_STR(t, "Off");
    perf::valueText(perf::kRepeater, 15, t, sizeof t);
    CHECK_STR(t, "1/16");
    perf::valueText(perf::kPlayMode, 0, t, sizeof t);
    CHECK_STR(t, "Fwd");
    perf::valueText(perf::kPlayMode, 22, t, sizeof t);
    CHECK_STR(t, "C20");
    perf::valueText(perf::kPatternLength, 8, t, sizeof t);
    CHECK_STR(t, "128");
    // Ranges (spec 06 §3.2).
    CHECK_EQ(perf::fx(perf::kTune).max, 48);
    CHECK_EQ(perf::fx(perf::kVolLfo).min, -23);
    CHECK_EQ(perf::fx(perf::kPanLfo).max, 28);
    CHECK_EQ(perf::fx(perf::kPlayMode).max, 22);
    CHECK(perf::fx(perf::kPatternLength).global);
}

TEST(perf_global_presets_persist_in_eeprom) {
    PerfRig r;
    App& a = r.a();
    r.h.tap(btn::Performance);
    PerformanceScreen& p = r.p();
    r.h.tap(btn::Rec);                       // edit mode
    CHECK(p.edit);
    r.h.push(InputEvent::kPadDown, 12 + 1);  // slot 1 (panning), row 1, held
    r.h.enc(5);
    r.h.enc(7);
    r.h.push(InputEvent::kPadUp, 12 + 1);
    CHECK_EQ(p.src.value(1, 1), 12);
    CHECK_EQ(a.cfg.perfValues()[8 * 1 + 2], 12);   // the config image holds it
    r.h.tap(btn::Pattern);                   // leave: forced config write
    r.h.run(20);
    CHECK_EQ(imgValue(r.nv.mem, 1, 1), 12);
    CHECK_EQ(r.nv.mem[Config::kOffPerfTypes + 1], 2);
    CHECK_EQ(r.nv.mem[Config::kOffPerfRows + 1], 1);   // rows persist too
    // A new boot on the same EEPROM loads them; fresh entry clears the rows.
    PerfRig r2(false);
    r2.nv.mem = r.nv.mem;
    r2.h.app->boot();
    r2.h.tap(btn::Performance);
    CHECK_EQ(r2.p().src.value(1, 1), 12);
    CHECK_EQ(r2.p().src.row(1), 0);
}

TEST(perf_project_presets_and_selector) {
    PerfRig r;
    App& a = r.a();
    a.cfg.perfPresets = 1;   // per project
    r.h.tap(btn::Performance);
    PerformanceScreen& p = r.p();
    CHECK_EQ(p.sourceIndex, 1);
    r.h.tap(btn::Rec);
    r.h.tap(btn::Up);        // column 0 type 1 -> 2: values/row cleared (spec 06 §3.4)
    CHECK_EQ(a.project.u8(pf::kPerfTypes), 2);
    r.h.push(InputEvent::kPadDown, 24);   // row 2
    r.h.enc(-9);
    r.h.push(InputEvent::kPadUp, 24);
    CHECK_EQ(int16_t(a.project.u16(pf::kPerfValues + 4)), -9);
    CHECK_EQ(a.project.u8(pf::kPerfRows), 2);   // rows at 0x2F5 (spec 12 §A.1)
    CHECK_EQ(a.cfg.perfTypes()[0], 1);          // global source untouched
    r.h.tap(btn::Pattern);   // leave: Workspace autosave
    Project chk;
    const std::vector<uint8_t>& f = r.h.fs.files["Workspace/project.mt"];
    CHECK(decodeProject(f.data(), f.size(), chk) == FmtStatus::Ok);
    CHECK_EQ(int16_t(chk.u16(pf::kPerfValues + 4)), -9);
    // Changing the selector while the screen is open takes effect on the
    // next entry (spec 12 §A.3).
    r.h.tap(btn::Performance);
    a.cfg.perfPresets = 0;
    CHECK_EQ(r.p().sourceIndex, 1);
    r.h.tap(btn::Pattern);
    r.h.tap(btn::Performance);
    CHECK_EQ(r.p().sourceIndex, 0);
    CHECK_EQ(r.p().src.type(0), 1);
}

TEST(perf_preset_validation) {
    Config c;
    c.perfTypes()[3] = 40;
    c.perfRows()[2] = 9;
    c.perfValues()[8 * 5 + 2] = 0x2C;   // [5][1] = 300
    c.perfValues()[8 * 5 + 3] = 0x01;
    c.perfValues()[8 * 6] = 7;          // [6][0] must be 0
    c.sanitize();
    CHECK_EQ(c.perfTypes()[3], 4);
    CHECK_EQ(c.perfRows()[2], 0);
    CHECK_EQ(c.perfValues()[8 * 5 + 2], 0);
    CHECK_EQ(c.perfValues()[8 * 6], 0);
}

// ---- P2: per-track patterns -------------------------------------------------

TEST(perf_track_key_patterns) {
    PerfRig r;
    App& a = r.a();
    putPattern(a, 2, 40);
    putPattern(a, 3, 41);
    r.h.tap(btn::Performance);
    CHECK(a.seq.performanceMode());
    PerformanceScreen& p = r.p();
    CHECK_EQ(p.shownPattern(2), 1);
    // Track key + encoder: immediate (stopped: committed now, spec 12 §C.2).
    r.h.down(btn::F3);
    r.h.enc(1);
    CHECK_EQ(a.seq.trackPattern(2), 2);
    CHECK_EQ(a.project.u16(pf::kPerfPatterns + 4), 2);
    r.h.up(btn::F3);
    CHECK(p.fxOn[2]);   // key used for a pattern change: no FX toggle
    // Track key + Right: pending, queued on release.
    r.h.down(btn::F4);
    r.h.tap(btn::Right);
    r.h.tap(btn::Right);
    CHECK_EQ(p.shownPattern(3), 3);
    CHECK_EQ(a.seq.queuedTrackPattern(3), 0);
    r.h.up(btn::F4);
    CHECK_EQ(a.seq.queuedTrackPattern(3), 3);
    CHECK_EQ(a.project.u16(pf::kPerfPatterns + 6), 3);
    CHECK(p.fxOn[3]);
    // Plain tap toggles the track's FX enable.
    r.h.tap(btn::F5);
    CHECK(!p.fxOn[4]);
    // Leaving reverts: overrides dropped, perf mode off (spec 12 §C.4).
    r.h.tap(btn::Pattern);
    CHECK(!a.seq.performanceMode());
    CHECK_EQ(a.seq.trackPattern(2), 0);
    // Fresh entry resets the labels to the current pattern (spec 06 §3.1).
    r.h.tap(btn::Performance);
    CHECK_EQ(a.project.u16(pf::kPerfPatterns + 4), 1);
}

// ---- P3: MIDI CC -------------------------------------------------------------

TEST(perf_cc_slot_live_value) {
    PerfRig r;
    App& a = r.a();
    r.h.tap(btn::Performance);
    PerformanceScreen& p = r.p();
    a.handleControlChange(51, 127);   // slot 0 = volume
    CHECK_EQ(a.engine.voice(0).perf().volume, 0);   // latched only
    r.h.run(60);
    CHECK_EQ(p.src.row(0), perf::kLiveRow);
    CHECK(p.liveValid[0]);
    CHECK_EQ(p.live[0], 100);
    CHECK_EQ(a.engine.voice(0).perf().volume, 100);
    a.handleControlChange(51, 64);
    r.h.run(60);
    CHECK_EQ(p.live[0], 0);
    a.handleControlChange(51, 0);
    r.h.run(60);
    CHECK_EQ(a.engine.voice(3).perf().volume, -100);
    // The encoder never stores into the live row (spec 12 §B.2 quirk guarded).
    r.h.tap(btn::Rec);
    r.h.push(InputEvent::kPadDown, 36);   // pad press replaces the live row
    CHECK(!p.liveValid[0]);
    CHECK_EQ(p.src.row(0), 3);
    r.h.push(InputEvent::kPadUp, 36);
    CHECK_EQ(p.src.value(1, 0), 0);
    // A second slot with the same type is reset by a CC on the first.
    p.src.setType(5, perf::kVolume);
    p.src.setValue(5, 1, 20);
    r.h.pad(12 + 5);
    CHECK_EQ(p.src.row(5), 1);
    a.handleControlChange(51, 100);
    r.h.run(60);
    CHECK_EQ(p.src.row(5), 0);
    CHECK_EQ(p.cur[5], 0);
    // Step repeater 0..15: v 127 -> 15; play mode 0..22.
    CHECK_EQ(perf::ccToValue(perf::kRepeater, 127), 15);
    CHECK_EQ(perf::ccToValue(perf::kPlayMode, 127), 22);
    CHECK_EQ(perf::ccToValue(perf::kVolume, 1), -99);   // truncates toward zero
}

TEST(perf_cc_pattern_length_slot_is_applied) {
    // OpenTracker fix (README): stock only shows a CC value on a global FX.
    PerfRig r;
    App& a = r.a();
    r.h.tap(btn::Performance);
    PerformanceScreen& p = r.p();
    p.src.setType(11, perf::kPatternLength);
    a.handleControlChange(62, 16);   // 8 * 16 / 127 = 1 -> length 1
    r.h.run(60);
    CHECK_EQ(p.cur[11], 1);
    r.h.tap(btn::Play);
    CHECK(a.seq.isPlaying());
    r.h.audio.pump(400);
    CHECK_EQ(a.seq.step(0), 0);   // one-step loop
    r.h.tap(btn::Play);
}

TEST(perf_cc_track_pattern_queued) {
    PerfRig r;
    App& a = r.a();
    putPattern(a, 5, 50);
    r.h.tap(btn::Performance);
    a.handleControlChange(44, 4);   // track 4 -> pattern 5
    r.h.run(60);
    CHECK_EQ(a.seq.queuedTrackPattern(3), 5);
    CHECK_EQ(a.project.u16(pf::kPerfPatterns + 6), 5);
    CHECK_EQ(r.p().shownPattern(3), 5);
    // Not on another screen (spec 12 §B.1).
    r.h.tap(btn::Pattern);
    a.handleControlChange(44, 9);
    r.h.run(60);
    CHECK_EQ(a.project.u16(pf::kPerfPatterns + 6), 5);
}

// ---- P12: FX conversion -------------------------------------------------------

TEST(perf_fx_conversion) {
    CHECK_EQ(perf::startOffset(-100), -65535);
    CHECK_EQ(perf::startOffset(0), 0);
    CHECK_EQ(perf::startOffset(100), 65535);
    CHECK_EQ(perf::startOffset(1), 655);
    CHECK_EQ(perf::endOffset(1), 655);   // 655.35 rounds down
    CHECK_EQ(perf::endOffset(-1), -655);
    CHECK_EQ(perf::lengthOverride(0), -1);
    CHECK_EQ(perf::lengthOverride(1), 0);
    CHECK_EQ(perf::lengthOverride(8), 127);
    PerfRig r;
    App& a = r.a();
    r.h.tap(btn::Performance);
    PerformanceScreen& p = r.p();
    p.src.setValue(1, 3, -70);
    r.h.pad(36 + 1);   // panning row 3
    CHECK_EQ(a.engine.voice(0).perf().pan, -70);   // P12: not halved
    p.src.setValue(2, 1, 12);
    r.h.pad(12 + 2);   // tune: also MIDI transpose
    CHECK_EQ(a.engine.voice(0).perf().tune, 12);
}

// ---- P14: start / type-change rules -----------------------------------------

TEST(perf_start_and_type_change_rules) {
    PerfRig r;
    App& a = r.a();
    r.h.tap(btn::Performance);
    PerformanceScreen& p = r.p();
    p.src.setValue(0, 1, 50);
    r.h.pad(12);
    CHECK_EQ(a.engine.voice(0).perf().volume, 50);
    // Type change in edit mode reverts the old type and clears the slot.
    r.h.tap(btn::Rec);
    r.h.tap(btn::Down);   // type 1 -> 0
    CHECK_EQ(p.src.type(0), 0);
    CHECK_EQ(p.src.row(0), 0);
    CHECK_EQ(p.src.value(0, 1), 0);
    CHECK_EQ(a.engine.voice(0).perf().volume, 0);
    r.h.tap(btn::Up);
    r.h.tap(btn::Up);     // 0 -> 2 (panning): values stay 0, no default ramp
    CHECK_EQ(p.src.type(0), 2);
    for (int k = 0; k < 4; ++k) CHECK_EQ(p.src.value(0, k), 0);
    // Copy / paste in edit mode: type + values, row reset.
    p.src.setValue(0, 2, 33);
    r.h.tap(btn::Copy);
    r.h.tap(btn::Right);
    r.h.down(btn::Shift);
    r.h.tap(btn::Copy);
    r.h.up(btn::Shift);
    CHECK_EQ(p.column, 1);
    CHECK_EQ(p.src.type(1), 2);
    CHECK_EQ(p.src.value(1, 2), 33);
    CHECK_EQ(p.src.row(1), 0);
    // Rows are cleared and the patterns reset on a fresh entry; all tracks enabled.
    r.h.pad(24 + 1);
    r.h.tap(btn::F2);
    CHECK(!p.fxOn[1]);
    r.h.tap(btn::Pattern);
    r.h.tap(btn::Performance);
    CHECK_EQ(p.src.row(1), 0);
    CHECK(p.fxOn[1]);
    CHECK(!p.edit);
}

// ---- 2.x M1-WP6: bank B (docs/specs/2x-01 §5, M1 plan §4) -------------------

TEST(perf_bank_toggle_by_performance_key) {
    PerfRig r;
    App& a = r.a();
    r.h.tap(btn::Performance);
    PerformanceScreen& p = r.p();
    CHECK(a.ui.active() == kModPerformance);
    CHECK_EQ(p.bank, 0);
    CHECK_STR(a.ui.bar.label[0].top, "Track 1");
    r.h.tap(btn::Performance);   // on the screen: toggles the bank
    CHECK(a.ui.active() == kModPerformance);
    CHECK_EQ(p.bank, 1);
    CHECK_STR(a.ui.bar.label[0].top, "Midi 9");
    CHECK_STR(a.ui.bar.label[7].top, "Midi 16");
    CHECK_STR(a.ui.bar.label[4].bottom, "Patt: 1");
    // Shift shows the mute action; Shift + F6 mutes track 14 (project.mt 0x742[5]).
    r.h.down(btn::Shift);
    CHECK_STR(a.ui.bar.label[5].bottom, "Mute");
    r.h.tap(btn::F6);
    CHECK_STR(a.ui.bar.label[5].bottom, "Unmute");
    r.h.up(btn::Shift);
    CHECK(trk::muted(a, 13));
    CHECK_EQ(a.project.u8(0x742 + 5), 1);      // project.mt (Tracker Mini, U13)
    CHECK(!a.trackMuted(5));
    // Shift + F1 in bank B: track 9 (stock mute byte).
    r.h.down(btn::Shift);
    r.h.tap(btn::F1);
    r.h.up(btn::Shift);
    CHECK(a.trackMuted(8));
    CHECK(!a.trackMuted(0));
    r.h.tap(btn::Performance);
    CHECK_EQ(p.bank, 0);
    CHECK_STR(a.ui.bar.label[0].top, "Track 1");
}

TEST(perf_bank_b_no_pattern_overrides) {
    // The Tracker Mini stores per-track patterns for tracks 1-8 only: bank B
    // keys show the current pattern and only toggle the FX (session state,
    // reset at every entry like tracks 1-8).
    PerfRig r;
    App& a = r.a();
    putPattern(a, 2, 40);
    putPattern(a, 5, 41);
    r.h.tap(btn::Performance);
    PerformanceScreen& p = r.p();
    r.h.tap(btn::Performance);
    CHECK_EQ(p.bank, 1);
    static Project before;
    before = a.project;
    // Hold F5 + encoder / F8 + Right / F6 + Up: nothing changes.
    r.h.down(btn::F5);
    r.h.enc(1);
    r.h.up(btn::F5);
    CHECK_EQ(p.shownPattern(12), 1);
    CHECK(p.fxOn[12]);                        // key used: no FX toggle
    r.h.down(btn::F8);
    for (int i = 0; i < 4; ++i) r.h.tap(btn::Right);
    CHECK_EQ(p.shownPattern(15), 1);
    r.h.up(btn::F8);
    CHECK(p.fxOn[15]);
    CHECK_STR(a.ui.bar.label[7].bottom, "Patt: 1");
    r.h.down(btn::F6);
    r.h.tap(btn::Up);
    r.h.up(btn::F6);
    CHECK_EQ(p.shownPattern(13), 1);
    CHECK(std::memcmp(before.values, a.project.values, sizeof(before.values)) == 0);
    // Plain tap: FX enable of tracks 13-16 (owner decision), not stored.
    r.h.tap(btn::F7);
    CHECK(!p.fxOn[14]);
    CHECK(std::memcmp(before.values, a.project.values, sizeof(before.values)) == 0);
    // Bank A keys still address tracks 1-8.
    r.h.tap(btn::Performance);
    r.h.down(btn::F1);
    r.h.enc(1);
    r.h.up(btn::F1);
    CHECK_EQ(a.project.u16(pf::kPerfPatterns + 0), 2);
    // A fresh entry: FX enabled again on every track.
    r.h.tap(btn::Pattern);
    r.h.tap(btn::Performance);
    CHECK(r.p().fxOn[14]);
    CHECK_EQ(r.p().shownPattern(15), 1);
}

TEST(perf_apply_all_covers_16_tracks) {
    PerfRig r;
    App& a = r.a();
    a.cfg.perfPresets = 1;
    r.h.tap(btn::Performance);
    PerformanceScreen& p = r.p();
    // Slot 0 = Play mode, row 1 = 3 (spec 12 §D): a sequencer-side FX.
    r.h.tap(btn::Rec);
    for (int i = 0; i < perf::kPlayMode - 1; ++i) r.h.tap(btn::Up);
    CHECK_EQ(p.src.type(0), perf::kPlayMode);
    r.h.push(InputEvent::kPadDown, 12);
    r.h.enc(3);
    r.h.push(InputEvent::kPadUp, 12);
    r.h.tap(btn::Rec);
    r.h.pad(12);
    CHECK_EQ(p.cur[0], 3);
    // Tracks 1-8 and the MIDI tracks of the sequencer take it.
    CHECK_EQ(a.seq.perfPlayMode(0), 3);
    CHECK_EQ(a.seq.perfPlayMode(8), 3);
    CHECK_EQ(a.seq.perfPlayMode(11), 3);
    // Disable track 10 in bank B: reverted there only.
    r.h.tap(btn::Performance);
    r.h.tap(btn::F2);
    CHECK(!p.fxOn[9]);
    CHECK_EQ(a.seq.perfPlayMode(9), 0);
    CHECK_EQ(a.seq.perfPlayMode(8), 3);
    r.h.tap(btn::F2);
    CHECK_EQ(a.seq.perfPlayMode(9), 3);
    // Leaving reverts every track.
    r.h.tap(btn::Pattern);
    for (int t = 0; t < 12; ++t) CHECK_EQ(a.seq.perfPlayMode(t), 0);
}

TEST(perf_fx_off_tracks_have_red_labels) {
    // Stock 1.9.2 (owner photo + note, 2026-10-23): all tracks start with
    // performance FX on and unlit (spec 06 §3.1 step 3); a track turned off
    // with its key is lit red. (MANUAL_AUDIT_2026-10-23 #5 read the manual's
    // older UI the other way round.)
    Harness h;
    h.tap(btn::Performance);
    auto redInLabel = [&](int t) {
        h.app->ui.dirty();
        h.run(40);
        bool red = false, inRed = false;
        for (const std::string& l : h.disp.last) {
            if (l.rfind("COLOR ", 0) == 0) inRed = l == "COLOR CD1818";
            int x, y;
            if (inRed && std::sscanf(l.c_str(), "VERTEX %d %d", &x, &y) == 2 && x >= 100 * t + 3 &&
                x <= 100 * t + 97 && y >= 424)
                red = true;
        }
        return red;
    };
    for (int t = 0; t < 8; ++t) CHECK(!redInLabel(t));   // all on, unlit
    h.tap(btn::F3);                                       // track 3 off
    CHECK(redInLabel(2));
    CHECK(!redInLabel(1));
    h.tap(btn::F3);                                       // on again
    CHECK(!redInLabel(2));
}

TEST(perf_fx_names_follow_stock) {
    // Stock 1.9.2 screen (owner photo, 2026-10-23), not the manual's list
    // (MANUAL_AUDIT_2026-10-23 #37 corrected): the first 12 effects.
    const char* const want[] = {"---", "Volume/Velocity", "Sample panning", "Tune", "Sample LP cutoff",
                                "Sample HP cutoff", "Sample BP cutoff", "Sample delay send", "Sample reverb snd",
                                "Sample position", "Sample end", "Sample playback", "Volume LFO speed"};
    for (int i = 0; i < 13; ++i) CHECK(std::string(perf::fx(uint8_t(i)).name) == want[i]);
}

TEST(perf_queued_pattern_blinks_and_edit_rows_are_red) {
    // MANUAL_AUDIT_2026-10-23 #39 (manual p.278 "The pattern will flash red
    // to show this is cued"): a queued track pattern blinks on its label.
    // #41 (manual p.274 "Values and bounding box are red"; spec 06 §3.3): the
    // active rows are drawn in the alert colour in edit mode.
    PerfRig r;
    App& a = r.a();
    putPattern(a, 2, 40);
    putPattern(a, 3, 41);
    r.h.tap(btn::Performance);
    r.h.down(btn::F4);
    r.h.tap(btn::Right);
    r.h.tap(btn::Right);
    r.h.up(btn::F4);
    CHECK_EQ(a.seq.queuedTrackPattern(3), 3);
    int shown = 0, hidden = 0;
    for (int i = 0; i < 6; ++i) {
        a.ui.dirty();
        r.h.run(110);
        if (r.h.disp.lastHasText("Patt: 3")) ++shown;
        else ++hidden;
    }
    CHECK(shown > 0);
    CHECK(hidden > 0);
    // Edit mode: alert-coloured rows.
    char want[16];
    std::snprintf(want, sizeof(want), "COLOR %06X", unsigned(a.ui.th().alert & 0xFFFFFF));
    auto count = [&]() {
        a.ui.dirty();
        r.h.run(40);
        int n = 0;
        for (const std::string& l : r.h.disp.last) n += l == want;
        return n;
    };
    const int before = count();
    r.p().edit = true;
    CHECK(count() > before);
    r.p().edit = false;
}
