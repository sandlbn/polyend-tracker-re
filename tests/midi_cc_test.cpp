// MIDI CC input (spec 12 §B, spec 05 §2.5 + appendix C §8) and the
// Instrument MIDI Mapping page. PLAN_PHASE2 B2 (T2a, T2b).
#include <cstdio>
#include <string>

#include "test.h"
#include "ui_fakes.h"

#include "app/instrument_edit.h"
#include "app/midi_cc.h"
#include "app/project_fields.h"
#include "ui/screens.h"

using namespace ot;

static InstrumentScreen& instrScreen(Harness& h) {
    return *static_cast<InstrumentScreen*>(h.app->ui.screen(kModInstrument));
}

static void cc(Harness& h, uint8_t ch, uint8_t num, uint8_t val) {
    h.midi.in.push_back(uint8_t(0xB0 | (ch & 15)));
    h.midi.in.push_back(num);
    h.midi.in.push_back(val);
    h.run(10);
}

// ---- pure helpers ------------------------------------------------------------

TEST(port_and_channel_filter) {
    // CC in: 0 off, 1 USB, 2 DIN, 3 both.
    CHECK(!midicc::portAccepted(0, true));
    CHECK(!midicc::portAccepted(0, false));
    CHECK(midicc::portAccepted(1, true));
    CHECK(!midicc::portAccepted(1, false));
    CHECK(!midicc::portAccepted(2, true));
    CHECK(midicc::portAccepted(2, false));
    CHECK(midicc::portAccepted(3, true));
    CHECK(midicc::portAccepted(3, false));
    // CC in channel: 0 all, else 1..16 == status nibble + 1.
    for (int c = 0; c < 16; ++c) CHECK(midicc::channelAccepted(0, uint8_t(c)));
    CHECK(midicc::channelAccepted(1, 0));
    CHECK(!midicc::channelAccepted(1, 1));
    CHECK(midicc::channelAccepted(16, 15));
    CHECK(!midicc::channelAccepted(16, 0));
}

TEST(cc_list_layout) {
    // 47 rows = 11 parameters + 6 automation blocks of 6 (appendix C §8).
    CHECK_EQ(midicc::kCcListCount, 47);
    // Stock table order, "Name = CC" (spec 16 §A.13, manual p.265;
    // MANUAL_AUDIT_2026-10-23 #7): Tune first ... Finetune Amount = 119 last.
    const uint8_t singles[11] = {5, 3, 7, 10, 9, 1, 11, 12, 13, 14, 15};
    for (int i = 0; i < 11; ++i) CHECK_EQ(midicc::kCcList[i].cc, singles[i]);
    CHECK(std::string(midicc::kCcList[0].label) == "Tune");
    CHECK(std::string(midicc::kCcList[1].label) == "FineTune");
    CHECK(std::string(midicc::kCcList[11].label) == "Volume Automation Type");
    CHECK(std::string(midicc::kCcList[12].label) == "Volume Attack - Shape LFO");
    CHECK_EQ(midicc::kCcList[46].cc, 119);
    CHECK(std::string(midicc::kCcList[46].label) == "Finetune Amount");
    const uint8_t bases[6] = {20, 26, 83, 102, 108, 114};
    for (int d = 0; d < 6; ++d)
        for (int k = 0; k < 6; ++k) {
            const midicc::CcRow* r = midicc::findRow(uint8_t(bases[d] + k));
            CHECK(r != nullptr);
            if (!r) continue;
            CHECK_EQ(r->dest, d);
            CHECK(int(r->param) == int(InstrParam::AutoType) + k);
        }
    CHECK(midicc::findRow(2) == nullptr);
    CHECK(midicc::findRow(41) == nullptr);
    CHECK(midicc::findRow(71) == nullptr);
    CHECK(midicc::findRow(120) == nullptr);
}

TEST(value_conversion) {
    // Appendix C §8 examples.
    CHECK_EQ(midicc::toUnits(0, 0, 100), 0);
    CHECK_EQ(midicc::toUnits(127, 0, 100), 100);
    CHECK_EQ(midicc::toUnits(64, 0, 100), 64 * 100 / 127);
    CHECK_EQ(midicc::toUnits(0, -100, 100), -100);
    CHECK_EQ(midicc::toUnits(127, -100, 100), 100);
    CHECK_EQ(midicc::toUnits(64, -100, 100), 64 * 200 / 127 - 100);
    CHECK_EQ(midicc::toUnits(0, -24, 24), -24);
    CHECK_EQ(midicc::toUnits(127, -24, 24), 24);
    // Master level v*100/127 (spec 12 §B.4).
    CHECK_EQ(midicc::masterLevel(127), 100);
    CHECK_EQ(midicc::masterLevel(0), 0);
    CHECK_EQ(midicc::masterLevel(64), 50);
    CHECK_EQ(midicc::masterOffset(71), pf::kTrackVolume);
    CHECK_EQ(midicc::masterOffset(78), pf::kTrackVolume + 7);
    CHECK_EQ(midicc::masterOffset(79), pf::kMixDelay);
    CHECK_EQ(midicc::masterOffset(80), pf::kMixReverb);
    CHECK_EQ(midicc::masterOffset(81), pf::kMixDry);
    CHECK_EQ(midicc::masterOffset(82), pf::kLineInLevel);
    CHECK_EQ(midicc::masterOffset(70), 0u);
    CHECK_EQ(midicc::masterOffset(83), 0u);
}

TEST(instrument_map_and_clamps) {
    Instrument in;
    CHECK(midicc::applyInstrumentCc(in, 7, 127));
    CHECK_EQ(in.volume, 100);
    CHECK(midicc::applyInstrumentCc(in, 10, 0));
    CHECK_EQ(in.panning, 0);
    CHECK(midicc::applyInstrumentCc(in, 5, 127));
    CHECK_EQ(in.tune, 24);
    CHECK(midicc::applyInstrumentCc(in, 3, 0));
    CHECK_EQ(in.finetune, -100);
    CHECK(midicc::applyInstrumentCc(in, 13, 0));
    CHECK_EQ(in.bitDepth, 4);
    CHECK(midicc::applyInstrumentCc(in, 12, 127));
    CHECK_EQ(in.overdrive, 100);
    CHECK(midicc::applyInstrumentCc(in, 14, 127));
    CHECK_EQ(in.reverbSend, 100);
    CHECK(midicc::applyInstrumentCc(in, 15, 127));
    CHECK_EQ(in.delaySend, 100);
    // Cutoff/Resonance only act while the filter is enabled (§4 col5/6).
    const float cut = in.cutoff;
    CHECK(!midicc::applyInstrumentCc(in, 1, 0));
    CHECK(in.cutoff == cut);
    CHECK(midicc::applyInstrumentCc(in, 9, 127));   // band-pass
    CHECK_EQ(in.filterEnabled, 1);
    CHECK_EQ(in.filterType, 2);
    CHECK(midicc::applyInstrumentCc(in, 1, 64));
    CHECK_EQ(iedit::get(in, InstrParam::Cutoff), 50);
    CHECK(midicc::applyInstrumentCc(in, 11, 127));
    CHECK(std::fabs(in.resonance - 4.3f) < 1e-4f);
    CHECK(midicc::applyInstrumentCc(in, 9, 0));     // disabled
    CHECK_EQ(in.filterEnabled, 0);
    // Same value again: no change reported.
    CHECK(!midicc::applyInstrumentCc(in, 7, 127));
}

TEST(automation_cc_blocks) {
    Instrument in;
    // Panning automation starts Off: only the type CC acts.
    CHECK(!midicc::applyInstrumentCc(in, 27, 127));
    CHECK(midicc::applyInstrumentCc(in, 26, 64));   // 64*2/127 = 1 -> envelope
    CHECK_EQ(iedit::autoType(in, 1), 1);
    CHECK(midicc::applyInstrumentCc(in, 27, 127));  // attack knob 190 -> 10 s
    CHECK_EQ(in.env[1].attackMs, 10000);
    CHECK(!midicc::applyInstrumentCc(in, 28, 0));   // decay already 0
    CHECK(midicc::applyInstrumentCc(in, 28, 64));   // P(64) = 100 -> 1000 ms (spec 17 §1.6)
    CHECK_EQ(in.env[1].decayMs, 1000);
    CHECK(midicc::applyInstrumentCc(in, 29, 0));
    CHECK(in.env[1].sustain == 0.0f);
    CHECK(midicc::applyInstrumentCc(in, 30, 127));
    CHECK_EQ(in.env[1].releaseMs, 10000);
    CHECK(midicc::applyInstrumentCc(in, 31, 0));
    CHECK(in.env[1].amount == 0.0f);
    // Volume block as LFO: shape / speed (0..23 for Volume) / amount.
    CHECK(midicc::applyInstrumentCc(in, 20, 127));
    CHECK_EQ(iedit::autoType(in, 0), 2);
    CHECK(midicc::applyInstrumentCc(in, 21, 127));
    CHECK_EQ(in.lfo[0].shape, 4);
    // LFO speed: exact target (OpenTracker fix, no +-5 limit).
    CHECK(midicc::applyInstrumentCc(in, 22, 127));
    CHECK_EQ(in.lfo[0].speed, 23);
    CHECK(!midicc::applyInstrumentCc(in, 23, 0));  // sustain: envelope only
    CHECK(midicc::applyInstrumentCc(in, 25, 127));
    CHECK(in.lfo[0].amount == 1.0f);
    // Finetune block LFO speed reaches 28.
    CHECK(midicc::applyInstrumentCc(in, 114, 127));
    CHECK(midicc::applyInstrumentCc(in, 116, 127));
    CHECK_EQ(in.lfo[5].speed, 28);
}

TEST(shared_clamps_match_ui_steps) {
    Instrument in;
    CHECK(iedit::step(in, InstrParam::Volume, 0, 500));
    CHECK_EQ(in.volume, 100);
    CHECK(!iedit::step(in, InstrParam::Volume, 0, 1));
    CHECK(iedit::step(in, InstrParam::BitDepth, 0, -100));
    CHECK_EQ(in.bitDepth, 4);
    // Filter type wraps on a tap, clamps otherwise.
    CHECK(iedit::set(in, InstrParam::FilterType, 0, 3));
    CHECK(!iedit::step(in, InstrParam::FilterType, 0, 1, false));
    CHECK(iedit::step(in, InstrParam::FilterType, 0, 1, true));
    CHECK_EQ(iedit::get(in, InstrParam::FilterType), 0);
    CHECK_EQ(iedit::msToKnob(1000), 100);
    CHECK_EQ(iedit::knobToMs(190), 10000);
    CHECK_EQ(iedit::defaultValue(InstrParam::Volume), 50);
    CHECK_EQ(iedit::defaultValue(InstrParam::BitDepth), 16);
}

// ---- App wiring --------------------------------------------------------------

TEST(app_cc_port_channel_and_running_status) {
    Harness h;
    App& a = *h.app;
    a.instrPresent[0] = true;
    a.setCurrentInstrument(0);
    a.instruments[0].volume = 50;
    cc(h, 0, 7, 127);                    // CC in = off (default)
    CHECK_EQ(a.instruments[0].volume, 50);
    a.cfg.ccIn = 2;                      // DIN only; the fake delivers USB
    cc(h, 0, 7, 127);
    CHECK_EQ(a.instruments[0].volume, 50);
    a.cfg.ccIn = 1;                      // USB
    cc(h, 0, 7, 127);
    CHECK_EQ(a.instruments[0].volume, 100);
    a.cfg.ccInChannel = 5;
    cc(h, 0, 7, 0);                      // channel 1: filtered
    CHECK_EQ(a.instruments[0].volume, 100);
    cc(h, 4, 7, 0);                      // channel 5
    CHECK_EQ(a.instruments[0].volume, 0);
    // Running status: one status byte, two CCs.
    h.midi.in.insert(h.midi.in.end(), {0xB4, 7, 10, 7, 20});
    h.run(10);
    CHECK_EQ(a.instruments[0].volume, 20 * 100 / 127);
    // Note-in settings do not gate CC.
    CHECK_EQ(a.cfg.notesIn, 0);
    CHECK(a.unsavedChanges());
}

TEST(app_cc_applies_while_playing) {
    // spec 17 §1.6 correction to appendix C §8: no play-state gating.
    Harness h;
    App& a = *h.app;
    a.cfg.ccIn = 3;
    a.instrPresent[0] = true;
    a.setCurrentInstrument(0);
    a.instruments[0].volume = 50;
    a.togglePlay(false);
    h.run(20);
    CHECK(a.seq.isPlaying() || a.seq.isArmed());
    cc(h, 0, 7, 127);
    CHECK_EQ(a.instruments[0].volume, 100);
    cc(h, 0, 71, 127);                   // master: always
    CHECK_EQ(a.project.u8(pf::kTrackVolume), 100);
    a.togglePlay(false);
    h.run(20);
    CHECK(!a.seq.isPlaying());
    cc(h, 0, 7, 0);
    CHECK_EQ(a.instruments[0].volume, 0);
}

TEST(app_master_levels) {
    Harness h;
    App& a = *h.app;
    a.cfg.ccIn = 1;
    cc(h, 0, 78, 64);
    CHECK_EQ(a.project.u8(pf::kTrackVolume + 7), 50);
    cc(h, 0, 79, 127);
    CHECK_EQ(a.project.u8(pf::kMixDelay), 100);
    cc(h, 0, 80, 0);
    CHECK_EQ(a.project.u8(pf::kMixReverb), 0);
    cc(h, 0, 81, 127);
    CHECK_EQ(a.project.u8(pf::kMixDry), 100);
    cc(h, 0, 82, 127);
    CHECK_EQ(a.project.u8(pf::kLineInLevel), 100);
    CHECK(a.unsavedChanges());           // OpenTracker: marked changed
}

struct CountingSink : PerformanceCcSink {
    int tracks = 0, slots = 0, lastIndex = -1, lastValue = -1;
    void perfTrackPatternCc(uint8_t t, uint8_t v) override { ++tracks; lastIndex = t; lastValue = v; }
    void perfSlotCc(uint8_t s, uint8_t v) override { ++slots; lastIndex = s; lastValue = v; }
};

TEST(app_performance_cc_only_on_performance_screen) {
    Harness h;
    App& a = *h.app;
    CountingSink sink;
    a.perfCc = &sink;
    a.cfg.ccIn = 1;
    cc(h, 0, 41, 3);
    cc(h, 0, 51, 3);
    CHECK_EQ(sink.tracks + sink.slots, 0);
    h.tap(btn::Performance);
    CHECK_EQ(a.ui.active(), int(kModPerformance));
    cc(h, 0, 48, 9);
    CHECK_EQ(sink.tracks, 1);
    CHECK_EQ(sink.lastIndex, 7);
    CHECK_EQ(sink.lastValue, 9);
    cc(h, 0, 62, 127);
    CHECK_EQ(sink.slots, 1);
    CHECK_EQ(sink.lastIndex, 11);
    cc(h, 0, 49, 1);                     // not a perf CC
    CHECK_EQ(sink.tracks + sink.slots, 2);
    a.perfCc = nullptr;
    cc(h, 0, 41, 3);                     // null sink is safe
}

TEST(app_cc_target_follows_mapping_list_for_midi_instrument) {
    Harness h;
    App& a = *h.app;
    a.cfg.ccIn = 1;
    a.instrPresent[3] = true;
    a.instruments[3].volume = 50;
    a.ccMapInstrument = 3;
    a.setCurrentInstrument(kFirstMidiInstrument);
    CHECK_EQ(a.ccTargetInstrument(), 3);
    cc(h, 0, 7, 0);
    CHECK_EQ(a.instruments[3].volume, 0);
    // Empty slot: ignored.
    a.ccMapInstrument = 4;
    const uint8_t v = a.instruments[4].volume;
    cc(h, 0, 7, 127);
    CHECK_EQ(a.instruments[4].volume, v);
}

// ---- MIDI Mapping page (appendix C §8) ---------------------------------------

TEST(mapping_page_navigation) {
    Harness h;
    App& a = *h.app;
    a.instrPresent[2] = true;
    a.setCurrentInstrument(2);
    h.tap(btn::InstrParams);
    InstrumentScreen& s = instrScreen(h);
    CHECK_EQ(s.mode, 0);
    h.tap(btn::F8);                      // More -> Effects
    CHECK(s.effects);
    CHECK(std::strcmp(a.ui.bar.label[4].top, "MIDI Map") == 0);
    h.tap(btn::F5);                      // -> MIDI Mapping
    CHECK_EQ(s.mode, 2);
    CHECK_EQ(a.ccMapInstrument, 2);
    CHECK_EQ(s.column, 0);
    CHECK(std::strcmp(a.ui.bar.label[1].top, "CC List") == 0);
    CHECK(std::strcmp(a.ui.bar.label[5].top, "Instruments") == 0);
    CHECK(std::strcmp(a.ui.bar.label[7].top, "Back") == 0);
    // CC list scrolling: encoder, F4 down, F1 up, clamped 0..46.
    h.enc(3);
    CHECK_EQ(s.ccScroll, 3);
    h.tap(btn::F4);
    CHECK_EQ(s.ccScroll, 4);
    h.tap(btn::F1);
    CHECK_EQ(s.ccScroll, 3);
    for (int i = 0; i < 10; ++i) h.enc(10);
    CHECK_EQ(s.ccScroll, 46);
    h.tap(btn::Up);
    CHECK_EQ(s.ccScroll, 45);
    // F5 focuses the instrument pane; then F7 next, F5 previous.
    h.tap(btn::F5);
    CHECK_EQ(s.column, 1);
    CHECK_EQ(a.ccMapInstrument, 2);
    h.tap(btn::F7);
    CHECK_EQ(a.ccMapInstrument, 3);
    CHECK_EQ(a.currentInstrument(), 3);
    h.tap(btn::F5);
    CHECK_EQ(a.ccMapInstrument, 2);
    h.enc(-10);
    CHECK_EQ(a.ccMapInstrument, 0);
    // Pads select directly; LED shows the selection.
    h.pad(40);
    CHECK_EQ(a.ccMapInstrument, 40);
    CHECK_EQ(a.currentInstrument(), 40);
    uint8_t lv[48] = {};
    s.padLeds(lv);
    CHECK(lv[40] > 0);
    // Delete / Instr are disabled here.
    h.tap(btn::Delete);
    h.tap(btn::Instr);
    CHECK_EQ(s.mode, 2);
    CHECK(!a.ui.picker.isOpen());
    // Draw both panes without overflowing the display list.
    a.ui.dirty();
    h.run(40);
    CHECK(a.ui.lastDlWords() < 2048);
    // F8 back to Effects.
    h.tap(btn::F8);
    CHECK_EQ(s.mode, 0);
    CHECK(s.effects);
}

TEST(mapping_page_not_kept_on_reentry) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::InstrParams);
    h.tap(btn::F8);
    h.tap(btn::F5);
    InstrumentScreen& s = instrScreen(h);
    CHECK_EQ(s.mode, 2);
    h.tap(btn::Pattern);
    h.tap(btn::InstrParams);
    CHECK_EQ(s.mode, 0);
    (void)a;
}

TEST(midi_instrument_page_offset_column) {
    Harness h;
    App& a = *h.app;
    a.setCurrentInstrument(kFirstMidiInstrument + 1);
    h.tap(btn::InstrParams);
    InstrumentScreen& s = instrScreen(h);
    CHECK_EQ(s.mode, 3);
    CHECK(std::strcmp(a.ui.bar.label[0].top, "Velocity") == 0);
    CHECK(std::strcmp(a.ui.bar.label[1].top, "Offset") == 0);
    h.tap(btn::F2);
    h.enc(5);
    CHECK_EQ(a.project.midiInstrument(1).extra[0], 5);   // sequencer delay byte (spec 13 §I.1)
    for (int i = 0; i < 30; ++i) h.enc(10);
    CHECK_EQ(a.project.midiInstrument(1).extra[0], 100);
    h.tap(btn::Delete);
    CHECK_EQ(a.project.midiInstrument(1).extra[0], 0);
    h.tap(btn::F1);
    h.enc(-27);
    CHECK_EQ(a.project.midiInstrument(1).velocity, 100);
}

TEST(ui_edit_and_cc_agree) {
    // An encoder edit and a CC to the same value land on the same field.
    Harness h;
    App& a = *h.app;
    a.cfg.ccIn = 1;
    a.instrPresent[0] = true;
    a.setCurrentInstrument(0);
    h.tap(btn::InstrParams);
    h.tap(btn::F4);                      // Finetune
    h.enc(-100);
    h.enc(-100);
    CHECK_EQ(a.instruments[0].finetune, -100);
    cc(h, 0, 3, 127);
    CHECK_EQ(a.instruments[0].finetune, 100);
    h.enc(5);
    CHECK_EQ(a.instruments[0].finetune, 100);   // same clamp
    // The bottom bar follows a CC change.
    cc(h, 0, 3, 0);
    a.ui.dirty();
    h.run(20);
    CHECK(std::strcmp(a.ui.bar.label[3].bottom, "-100") == 0);
}

TEST(cc_list_rows_fit_the_reference_pane) {
    // "Name = CC" rows are drawn at x 12 in a 397-px pane (screen_instrument).
    for (int fonts = 0; fonts < 2; ++fonts) {
        RecordingDisplay d;
        d.customFonts = fonts == 1;
        applyFontSet(&d, 0);
        for (int i = 0; i < midicc::kCcListCount; ++i) {
            char row[48];
            std::snprintf(row, sizeof(row), "%s = %d", midicc::kCcList[i].label, midicc::kCcList[i].cc);
            CHECK(textWidth(kFontText, row) <= 397 - 12 - 4);
            if (textWidth(kFontText, row) > 381) std::printf("       too wide (%d): %s\n", textWidth(kFontText, row), row);
        }
        d.customFonts = false;
        applyFontSet(&d, 0);
    }
}

TEST(midi_instrument_cc_columns_are_lists) {
    // MANUAL_AUDIT_2026-10-23 #6 (manual p.256-261, spec 19 §4: CC A-F are
    // six 131-item list controls): the columns list CC names around the
    // chosen one, not a value bar.
    Harness h;
    App& a = *h.app;
    a.setCurrentInstrument(uint8_t(kFirstMidiInstrument));
    h.tap(btn::InstrParams);
    auto& s = *static_cast<InstrumentScreen*>(a.ui.screen(kModInstrument));
    CHECK_EQ(s.mode, 3);
    a.ui.dirty();
    h.run(40);
    char want[16];
    midicc::ccValueName(2, want, sizeof(want));   // CC A default 1: its neighbour 2
    CHECK(h.disp.lastHasText(want));
    // Six lists still fit the FT81x display list, ROM and custom fonts.
    for (int fonts = 0; fonts < 2; ++fonts) {
        h.disp.customFonts = fonts == 1;
        applyFontSet(&h.disp, 0);
        a.ui.dirty();
        h.run(40);
        std::printf("MIDI instrument page, %s fonts: %u DL words\n", fonts ? "custom" : "ROM", unsigned(a.ui.lastDlWords()));
        CHECK(a.ui.lastDlWords() < 1900);
    }
    h.disp.customFonts = false;
    applyFontSet(&h.disp, 0);
}

TEST(midi_instrument_title_names_the_channel) {
    // Manual p.256 "51. MIDI Channel 1" style / spec 20 §1.2 (pattern screen
    // already prints "N. MIDI Channel n"): MANUAL_AUDIT_2026-10-23 #26.
    Harness h;
    App& a = *h.app;
    a.setCurrentInstrument(uint8_t(kFirstMidiInstrument));
    h.tap(btn::InstrParams);
    a.ui.dirty();
    h.run(40);
    char want[32];
    std::snprintf(want, sizeof(want), "%d. MIDI Channel 1", kFirstMidiInstrument + 1);
    CHECK(h.disp.lastHasText(want));
}
