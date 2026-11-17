// [OT] FX a-f on a synth step set the slot's macros 1-6 (spec 2x-05 §4.3):
// the FX list, its description and the top bar name the macro there, and
// keep the MIDI CC names on other steps.
#include <cstdio>
#include <cstring>

#include "test.h"
#include "ui_fakes.h"

#include "model/fx.h"
#include "ui/screens.h"

using namespace ot;

namespace {
PatternScreen& pe(Harness& h) { return *static_cast<PatternScreen*>(h.app->ui.screen(kModPattern)); }
int menuIndex(FxId id) {
    for (int i = 0; i < kFxCount; ++i)
        if (kFxMenuOrder[i] == uint8_t(id)) return i;
    return -1;
}
// The FX list, pad on CC A (or F), the description window on.
void openOnCc(Harness& h, FxId id) {
    h.down(btn::Fx1);
    h.run(300);
    CHECK(pe(h).popup().isOpen());
    const int i = menuIndex(id);
    CHECK(i >= 0 && i < 48);
    h.pad(uint8_t(i));
    if (!pe(h).popup().descriptionOpen()) h.tap(btn::F7);
    CHECK(pe(h).popup().descriptionOpen());
    h.run(40);
}
void closeList(Harness& h) {
    h.tap(btn::F7);
    h.up(btn::Fx1);
    h.run(40);
}
}  // namespace

TEST(fx_list_names_the_macro_on_a_synth_step) {
    Harness h;
    App& a = *h.app;
    std::snprintf(a.synthPatch(0).macros[0].name, sizeof(a.synthPatch(0).macros[0].name), "Cutoff");
    a.synthPatch(0).macros[5].name[0] = 0;
    Step& s = a.pattern().tracks[0].steps[0];
    s.note = 60;
    s.instrument = uint8_t(kFirstSynthInstrument);   // S1
    openOnCc(h, FxId::MidiCcA);
    CHECK(h.disp.lastHasText("a - M1 Cutoff"));
    CHECK(!h.disp.lastHasText("MIDI CC A"));
    CHECK(h.disp.lastHasText("macro"));
    closeList(h);
    openOnCc(h, FxId::MidiCcF);
    CHECK(h.disp.lastHasText("f - Macro 6"));
    closeList(h);
    // The top bar on the FX column.
    s.fx[0].type = uint8_t(FxId::MidiCcA);
    s.fx[0].value = 40;
    h.tap(btn::Fx1);
    a.ui.dirty();
    h.run(40);
    CHECK(h.disp.lastHasText("M1 Cutoff"));
}

TEST(fx_list_keeps_midi_cc_on_a_sample_step) {
    Harness h;
    App& a = *h.app;
    Step& s = a.pattern().tracks[0].steps[0];
    s.note = 60;
    s.instrument = 0;   // sample instrument 1
    openOnCc(h, FxId::MidiCcA);
    CHECK(h.disp.lastHasText("a - MIDI CC A"));
    CHECK(h.disp.lastHasText("Sends the MIDI instrument's CC A"));
    closeList(h);
}

// A preset instrument (P1..P8) note: its FX a-f set the preset's macros.
TEST(fx_list_names_the_preset_macro_on_a_preset_instrument_step) {
    Harness h;
    App& a = *h.app;
    SynthPatch pp = a.synthPatch(0);
    std::snprintf(pp.macros[1].name, sizeof(pp.macros[1].name), "Drive");
    a.setSynthPresetPatch(2, pp, "Grit");
    Step& s = a.pattern().tracks[0].steps[0];
    s.note = 60;
    s.instrument = uint8_t(kFirstPresetInstrument + 1);   // P2
    openOnCc(h, FxId::MidiCcB);
    CHECK(h.disp.lastHasText("b - M2 Drive"));
    closeList(h);
}

// Steps without a note follow the sequencer: the synth of the track's last
// note; a MIDI step or a track with no synth note before keeps the CC names.
TEST(fx_list_on_steps_without_a_note_follows_the_tracks_last_note) {
    Harness h;
    App& a = *h.app;
    std::snprintf(a.synthPatch(1).macros[0].name, sizeof(a.synthPatch(1).macros[0].name), "Wobble");
    a.setCurrentInstrument(uint8_t(kFirstSynthInstrument));   // S1: not what the step uses
    Track& t0 = a.pattern().tracks[0];
    t0.steps[0].note = 60;
    t0.steps[0].instrument = uint8_t(kFirstSynthInstrument + 1);   // S2 note
    t0.steps[1].note = -1;
    t0.steps[1].instrument = 0;
    h.tap(btn::Down);   // cursor on step 1 (FX only)
    h.run(40);
    openOnCc(h, FxId::MidiCcA);
    CHECK(h.disp.lastHasText("a - M1 Wobble"));
    closeList(h);
    // No note before it on the track: nothing to set a macro on.
    t0.steps[0].note = -1;
    openOnCc(h, FxId::MidiCcA);
    CHECK(h.disp.lastHasText("a - MIDI CC A"));
    closeList(h);
    // A MIDI instrument on the step sends the CC, after a synth note too.
    t0.steps[0].note = 60;
    t0.steps[1].instrument = uint8_t(kFirstMidiInstrument);
    openOnCc(h, FxId::MidiCcA);
    CHECK(h.disp.lastHasText("a - MIDI CC A"));
    closeList(h);
}
