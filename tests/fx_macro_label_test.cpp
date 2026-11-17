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
