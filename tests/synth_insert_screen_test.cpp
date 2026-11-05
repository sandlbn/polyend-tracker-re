// The Synth Insert view (Shift + Instrument Params on a synth slot): F1 the
// insert (None, then the build's effects; F1 again steps it, wrapping), F2..F7
// its settings, F8 Back; Instrument Params returns to page 1/2. Edits go to
// the App (saved in project.ot3x) and the synth bank.
#include "test.h"

#include <cstring>
#include <string>

#include "modules/module.h"
#include "ui/screens.h"
#include "ui_fakes.h"

using namespace ot;

#if OT_SLOT_INSERTS

namespace {
SynthScreen& synth(App& a) { return *static_cast<SynthScreen*>(a.ui.screen(kModSynth)); }

void openInsert(Harness& h, int slot) {
    h.app->setCurrentInstrument(uint8_t(64 + slot));
    h.tap(btn::Pattern);
    h.tap(btn::InstrParams);
    h.down(btn::Shift);
    h.tap(btn::InstrParams);
    h.up(btn::Shift);
}
}  // namespace

TEST(synth_insert_view_opens_and_returns) {
    Harness h;
    App& a = *h.app;
    openInsert(h, 1);
    SynthScreen& s = synth(a);
    CHECK(a.ui.active() == kModSynth);
    CHECK(s.view == SynthScreen::View::Insert);
    CHECK(h.disp.lastHasText("Synth Insert"));
    CHECK_STR(a.ui.bar.label[0].top, "Insert");
    CHECK_STR(a.ui.bar.label[0].bottom, "None");
    CHECK_STR(a.ui.bar.label[7].top, "Back");
    // Instrument Params goes back to page 1/2, and from there it toggles as before.
    h.tap(btn::InstrParams);
    CHECK(s.view == SynthScreen::View::Params);
    h.tap(btn::InstrParams);
    CHECK(s.view == SynthScreen::View::Macros);
    // F8 Back too.
    h.down(btn::Shift);
    h.tap(btn::InstrParams);
    h.up(btn::Shift);
    CHECK(s.view == SynthScreen::View::Insert);
    h.tap(btn::F8);
    CHECK(s.view == SynthScreen::View::Params);
}

TEST(synth_insert_view_edits_the_insert) {
    Harness h;
    App& a = *h.app;
    mod::ModuleHost& mh = mod::ModuleHost::instance();
    CHECK(mh.effectCount() >= 1);
    audio::IEffect* first = mh.effectAt(0);
    openInsert(h, 0);
    SynthScreen& s = synth(a);
    CHECK(a.slotInsertId(0) == nullptr);
    // F1 on the focused list: the next insert.
    h.tap(btn::F1);
    CHECK(a.slotInsertEffect(0) == first);
    CHECK_STR(a.ui.bar.label[0].bottom, first->name());
    CHECK_STR(a.ui.bar.label[1].top, first->param(0).label);
    char want[24];
    first->valueText(1, first->param(1).def, want, sizeof(want));
    CHECK_STR(a.ui.bar.label[2].bottom, want);
    CHECK(a.sidecarDirty());
    // F3: the second setting; the wheel changes it; Delete back to its default.
    h.tap(btn::F3);
    CHECK_EQ(s.insFocus, 2);
    const uint8_t def = first->param(1).def;
    h.enc(int8_t(def < first->param(1).max ? 1 : -1));
    CHECK(a.slotInsertSettings(0).v[1] != def);
    h.tap(btn::Delete);
    CHECK_EQ(int(a.slotInsertSettings(0).v[1]), int(def));
    // F4..F7 past the effect's settings are not selectable.
    if (first->paramCount() < 6) {
        h.tap(uint8_t(btn::F1 + first->paramCount() + 1));
        CHECK_EQ(s.insFocus, 2);
    }
    // F1 twice: focus the list, then step on; wrapping back to None.
    h.tap(btn::F1);
    CHECK_EQ(s.insFocus, 0);
    for (int k = 0; k < mh.effectCount() && a.slotInsertId(0); ++k) h.tap(btn::F1);
    CHECK(a.slotInsertId(0) == nullptr);
    CHECK_STR(a.ui.bar.label[0].bottom, "None");
    // The synth bank gets it at the next block.
    h.tap(btn::F1);
    h.audio.pump(1);
    CHECK(a.engine.synth().slotInsert(0) == first);
}

// The first setting starts with "Off" (Chorus: Off, I, II, I+II; Drive:
// Off, Soft, ...): the wheel below the first value turns the insert off and
// keeps its settings; above it, on again. A new insert starts on.
TEST(synth_insert_first_setting_has_off) {
    Harness h;
    App& a = *h.app;
    audio::IEffect* first = mod::ModuleHost::instance().effectAt(0);
    CHECK(first->param(0).names != nullptr);
    openInsert(h, 0);
    h.tap(btn::F1);
    CHECK(a.slotInsertEffect(0) == first);
    CHECK(!a.slotInsertOff(0));
    h.tap(btn::F2);
    a.setSlotInsertValue(0, 1, 7);
    for (int k = 0; k <= first->param(0).max; ++k) h.tap(btn::Down);   // a list: Down = the item below
    CHECK(!a.slotInsertOff(0));
    CHECK_EQ(int(a.slotInsertSettings(0).v[0]), int(first->param(0).max));
    for (int k = 0; k <= first->param(0).max; ++k) h.tap(btn::Up);
    CHECK(a.slotInsertOff(0));
    CHECK_STR(a.ui.bar.label[1].bottom, "Off");
    CHECK_EQ(int(a.slotInsertSettings(0).v[1]), 7);   // kept
    h.audio.pump(1);
    CHECK(a.engine.synth().slotInsertOff(0));
    h.tap(btn::Down);
    CHECK(!a.slotInsertOff(0));
    CHECK_EQ(int(a.slotInsertSettings(0).v[0]), 0);
    char want[24];
    first->valueText(0, 0, want, sizeof(want));
    CHECK_STR(a.ui.bar.label[1].bottom, want);
    // Off, then Delete: the default (on).
    h.tap(btn::Up);
    CHECK(a.slotInsertOff(0));
    h.tap(btn::Delete);
    CHECK(!a.slotInsertOff(0));
    // Another insert starts on.
    a.setSlotInsertOff(0, true);
    h.tap(btn::F1);
    h.tap(btn::F1);
    CHECK(!a.slotInsertOff(0));
}

#else
TEST(synth_insert_view_not_in_this_build) { CHECK(true); }
#endif
