// Track EQ page, opened from the Track Mixer (OpenTracker extension): F1..F4 select /
// toggle a band, F5..F8 select the parameter, the jog wheel edits it, Shift is
// coarse, Left / Right pick the track, Delete resets the band, and the values
// reach the audio engine.
#include <cstring>

#include "test.h"
#include "ui_fakes.h"

#include "ui/screens.h"

using namespace ot;
using namespace ot::audio;

namespace {
MixerScreen& mixer(Harness& h) { return *static_cast<MixerScreen*>(h.app->ui.screen(kModMaster)); }
// Open the EQ of track `t` (0..7): Master -> Track Mixer, hold the track key, press Insert.
MixerScreen& eqPage(Harness& h, int t = 0) {
    h.tap(btn::Master);   // enter Master (page 0)
    h.tap(btn::Master);   // track mixer
    CHECK_EQ(mixer(h).page, 1);
    h.down(uint8_t(btn::F1 + t));
    h.tap(btn::Insert);
    h.up(uint8_t(btn::F1 + t));
    CHECK_EQ(mixer(h).page, 3);
    CHECK_EQ(mixer(h).eqTrack, t);
    return mixer(h);
}
}  // namespace

TEST(eq_page_labels_and_selection) {
    Harness h;
    App& a = *h.app;
    MixerScreen& m = eqPage(h);
    CHECK_STR(a.ui.bar.label[0].top, "Band 1");
    CHECK_STR(a.ui.bar.label[0].bottom, "Off");
    CHECK_STR(a.ui.bar.label[4].top, "Type");
    CHECK_STR(a.ui.bar.label[4].bottom, "Low Cut");   // band 1 default
    CHECK_STR(a.ui.bar.label[5].bottom, "80 Hz");
    CHECK_EQ(m.eqBand, 0);
    // F3 selects band 3 (no toggle); a second F3 turns it on.
    h.tap(btn::F3);
    CHECK_EQ(m.eqBand, 2);
    CHECK(!a.trackEq.track[0].band[2].on);
    h.tap(btn::F3);
    CHECK(a.trackEq.track[0].band[2].on);
    CHECK_STR(a.ui.bar.label[2].bottom, "On");
    h.tap(btn::F3);
    CHECK(!a.trackEq.track[0].band[2].on);
    // F6 = frequency, F8 = Q.
    h.tap(btn::F6);
    CHECK_EQ(m.eqParam, 1);
    h.tap(btn::F8);
    CHECK_EQ(m.eqParam, 3);
}

TEST(eq_page_jog_edits_the_selected_parameter) {
    Harness h;
    App& a = *h.app;
    eqPage(h);
    h.tap(btn::F2);   // band 2: bell 500 Hz
    h.tap(btn::F6);   // frequency
    const uint16_t f0 = a.trackEq.track[0].band[1].freqHz;
    h.enc(1);
    CHECK_EQ(a.trackEq.track[0].band[1].freqHz, eqStepFreq(f0, 1, false));
    CHECK(a.trackEq.track[0].band[1].on);   // editing switches the band on
    // Shift = coarse.
    h.down(btn::Shift);
    const uint16_t f1 = a.trackEq.track[0].band[1].freqHz;
    h.enc(1);
    h.up(btn::Shift);
    CHECK_EQ(a.trackEq.track[0].band[1].freqHz, eqStepFreq(f1, 1, true));
    // Gain.
    h.tap(btn::F7);
    h.enc(4);
    CHECK_EQ(a.trackEq.track[0].band[1].gainDb10, 20);
    CHECK_STR(a.ui.bar.label[6].bottom, "+2.0 dB");
    // Type -> a cut has no gain: the gain label is disabled and the jog does nothing.
    h.tap(btn::F5);
    a.trackEq.track[0].band[1].type = kEqLowCut;
    h.enc(0);
    h.tap(btn::F7);
    h.enc(1);
    CHECK_EQ(a.trackEq.track[0].band[1].gainDb10, 20);
    // Q.
    h.tap(btn::F8);
    const uint16_t q0 = a.trackEq.track[0].band[1].q100;
    h.enc(1);
    CHECK_EQ(a.trackEq.track[0].band[1].q100, eqStepQ(q0, 1, false));
    // Delete restores the default band; it was edited so the project is dirty.
    CHECK(a.unsavedChanges());
    h.tap(btn::Delete);
    const EqBand d = eqDefaultBand(1);
    CHECK_EQ(a.trackEq.track[0].band[1].freqHz, d.freqHz);
    CHECK_EQ(a.trackEq.track[0].band[1].gainDb10, 0);
    CHECK(!a.trackEq.track[0].band[1].on);
}

TEST(eq_page_tracks_are_independent_and_reach_the_engine) {
    Harness h;
    App& a = *h.app;
    MixerScreen& m = eqPage(h);
    h.tap(btn::F6);
    h.enc(3);
    const uint16_t f0 = a.trackEq.track[0].band[0].freqHz;
    CHECK(f0 != eqDefaultBand(0).freqHz);
    h.tap(btn::Right);
    CHECK_EQ(m.eqTrack, 1);
    CHECK_EQ(a.trackEq.track[1].band[0].freqHz, eqDefaultBand(0).freqHz);   // untouched
    h.enc(-2);
    CHECK(a.trackEq.track[1].band[0].on);
    CHECK(a.trackEq.track[1].band[0].freqHz < eqDefaultBand(0).freqHz);
    // Clamped at the ends.
    for (int i = 0; i < 10; ++i) h.tap(btn::Right);
    CHECK_EQ(m.eqTrack, 7);
    for (int i = 0; i < 10; ++i) h.tap(btn::Left);
    CHECK_EQ(m.eqTrack, 0);
    // The engine receives the settings at the next block.
    h.audio.pump(2);
    CHECK_EQ(a.engine.settings().trackEq.track[0].band[0].freqHz, f0);
    CHECK(a.engine.settings().trackEq.track[1].band[0].on);
    CHECK(!a.engine.settings().trackEq.track[2].band[0].on);
}

TEST(eq_page_draws_within_the_display_list_budget) {
    Harness h;
    App& a = *h.app;
    eqPage(h);
    // All four bands on, extreme values: the frame must still render.
    for (int i = 0; i < kEqBands; ++i) {
        a.trackEq.track[0].band[i].on = true;
        a.trackEq.track[0].band[i].gainDb10 = i % 2 ? -150 : 150;
        a.trackEq.track[0].band[i].freqHz = i == 0 ? 20 : i == 3 ? 20000 : 1000;
        a.trackEq.track[0].band[i].q100 = 1800;
    }
    a.markProjectDirty();
    h.audio.pump(1);
    a.ui.dirty();
    h.tap(btn::F2);   // forces a redraw
    CHECK(h.disp.lastHasText("Track EQ"));
}

TEST(eq_page_slope_option_on_bands_1_and_4) {
    Harness h;
    App& a = *h.app;
    eqPage(h);
    // Band 1 is a Low Cut: F7 is "Slope", the jog switches 12 <-> 48 dB/oct.
    CHECK_STR(a.ui.bar.label[6].top, "Slope");
    CHECK_STR(a.ui.bar.label[6].bottom, "12 dB");
    h.tap(btn::F7);
    h.enc(1);
    CHECK_EQ(a.trackEq.track[0].band[0].slope, 1);
    CHECK(a.trackEq.track[0].band[0].on);
    CHECK_STR(a.ui.bar.label[6].bottom, "48 dB");
    h.enc(1);
    CHECK_EQ(a.trackEq.track[0].band[0].slope, 0);
    h.enc(1);
    CHECK_EQ(a.trackEq.track[0].band[0].slope, 1);
    // Band 4 (F4) is a High Cut: same control.
    h.tap(btn::F4);
    CHECK_STR(a.ui.bar.label[6].top, "Slope");
    h.enc(1);
    CHECK_EQ(a.trackEq.track[0].band[3].slope, 1);
    // Bands 2 and 3 (bells) keep Gain; a stray slope cannot appear there.
    h.tap(btn::F2);
    CHECK_STR(a.ui.bar.label[6].top, "Gain");
    // Changing band 1 to a bell drops the 48 dB flag instead of hiding it.
    h.tap(btn::F1);
    h.tap(btn::F5);
    h.enc(-1);   // one type back from Low Cut
    CHECK_EQ(a.trackEq.track[0].band[0].type, kEqHighShelf);   // one step back from Low Cut
    CHECK_EQ(a.trackEq.track[0].band[0].slope, 0);
    CHECK_STR(a.ui.bar.label[6].top, "Gain");
    // The engine gets the 48 dB setting.
    h.audio.pump(2);
    CHECK_EQ(a.engine.settings().trackEq.track[0].band[3].slope, 1);
}

TEST(eq_page_opens_from_track_mixer_and_returns) {
    Harness h;
    App& a = *h.app;
    MixerScreen& m = eqPage(h, 2);
    CHECK_STR(a.ui.bar.label[0].top, "Band 1");
    CHECK(!a.trackMuted(2));   // releasing the track key did not mute it
    h.tap(btn::Master);        // back to the Track Mixer
    CHECK_EQ(m.page, 1);
    h.tap(btn::Master);        // Global Mixer
    CHECK_EQ(m.page, 2);
    h.tap(btn::Master);        // Master (stock three-page cycle)
    CHECK_EQ(m.page, 0);
    // Leaving from the EQ page and entering Master again never lands on the EQ.
    h.tap(btn::Master);   // Track Mixer again
    h.down(btn::F2);
    h.tap(btn::Insert);
    h.up(btn::F2);
    CHECK_EQ(m.page, 3);
    CHECK_EQ(m.eqTrack, 1);
    h.tap(btn::Pattern);
    h.tap(btn::Master);
    CHECK(m.page != 3);
}

TEST(eq_opens_from_pattern_more_page_for_cursor_track) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Pattern);
    PatternScreen& ps = *static_cast<PatternScreen*>(a.ui.screen(kModPattern));
    ps.track = 3;
    h.tap(btn::F8);   // More -> page 1
    h.tap(btn::F8);   // More -> page 2 (names + Track EQ)
    CHECK_EQ(ps.page, 2);
    CHECK_STR(a.ui.bar.label[2].top, "Track EQ");
    h.tap(btn::F3);
    CHECK(a.ui.active() == kModMaster);
    CHECK_EQ(mixer(h).page, 3);
    CHECK_EQ(mixer(h).eqTrack, 3);
    // The bottom bar belongs to the EQ page, not the Pattern page it was opened from.
    CHECK_STR(a.ui.bar.label[0].top, "Band 1");
    CHECK_STR(a.ui.bar.label[4].top, "Type");
    // An edit lands on that track only.
    h.tap(btn::F2);
    h.tap(btn::F2);   // band 2 on
    CHECK(a.trackEq.track[3].band[1].on);
    CHECK(!a.trackEq.track[0].band[1].on);
    // ...and reaches the audio engine for that track (voice 3) only.
    h.audio.pump(1);
    CHECK(a.engine.settings().trackEq.track[3].band[1].on);
    CHECK(!a.engine.settings().trackEq.track[0].band[1].on);
    CHECK(!a.engine.settings().trackEq.track[2].band[1].on);
    // Pattern comes back; tracks 9-16 have no EQ.
    h.tap(btn::Pattern);
    CHECK(a.ui.active() == kModPattern);
    ps.track = 9;
    h.run(40);
    h.tap(btn::F3);
    CHECK(a.ui.active() == kModPattern);
}
