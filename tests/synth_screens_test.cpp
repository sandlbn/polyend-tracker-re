// 2.x synth screens (WP-G, docs/MINI_PLAN.md §3 UI; ui/screen_synth*.cpp):
// entry from synth steps / instruments, page and view navigation, parameter
// edits applying live and marking the patch dirty, library load / save /
// Save As / Init, the "not available" read-only state, the Instrument popup
// S1..S3 entries and the Global Mixer synth columns.
#include "test.h"
#include "ui_fakes.h"

#include <cstring>
#include <cmath>
#include <string>

#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "ui/screens.h"

using namespace ot;

namespace {

SynthScreen& synth(App& a) { return *static_cast<SynthScreen*>(a.ui.screen(kModSynth)); }

// True when the last frame drew `s` as a text run.
bool frameHas(Harness& h, const char* s) {
    h.app->ui.dirty();
    h.run(40);
    const std::string q = std::string("\"") + s + "\"";
    for (const std::string& l : h.disp.last)
        if (l.find(q) != std::string::npos) return true;
    return false;
}

// Boot, then put slot s on `model` (ACD unless given) and open its page.
void openSlot(Harness& h, int s, SynthModel model = SynthModel::ACD) {
    App& a = *h.app;
    if (a.synthSlot(s).model != uint32_t(model)) a.setSynthModel(s, int(model));
    a.setCurrentInstrument(kFirstSynthInstrument + s);
    h.tap(btn::Pattern);
    h.tap(btn::InstrParams);
}

std::vector<uint8_t> encode(const SynthPatch& p) {
    std::vector<uint8_t> out(kPatchMaxFileSize);
    size_t n = 0;
    CHECK(encodePatch(p, out.data(), out.size(), &n) == PatchStatus::kOk);
    out.resize(n);
    return out;
}

}  // namespace

TEST(entry_from_instrument_and_step) {
    Harness h;
    App& a = *h.app;
    // A sample instrument opens the Instrument screen.
    a.setCurrentInstrument(0);
    h.tap(btn::Pattern);
    h.tap(btn::InstrParams);
    CHECK(a.ui.active() == kModInstrument);
    // Current instrument S2 (65): the synth page of slot 1.
    a.setCurrentInstrument(65);
    CHECK(a.currentInstrument() == 65);
    h.tap(btn::Pattern);
    h.tap(btn::InstrParams);
    CHECK(a.ui.active() == kModSynth);
    CHECK(synth(a).slot == 1);
    CHECK(synth(a).view == SynthScreen::View::Params);
    CHECK(frameHas(h, "Synth Parameters 1/2"));
    CHECK(frameHas(h, "Synth 2 (ACD)"));
    // The Pattern cursor on an S3 step: slot 2, and S3 becomes current.
    a.setCurrentInstrument(0);
    h.tap(btn::Pattern);
    PatternScreen& ps = *static_cast<PatternScreen*>(a.ui.screen(kModPattern));
    Step& st = a.pattern().tracks[ps.track].steps[ps.step];
    st.note = 60;
    st.instrument = 66;
    h.tap(btn::InstrParams);
    CHECK(a.ui.active() == kModSynth);
    CHECK(synth(a).slot == 2);
    CHECK(a.currentInstrument() == 66);
}

TEST(page_and_view_navigation) {
    Harness h;
    App& a = *h.app;
    openSlot(h, 1);
    SynthScreen& s = synth(a);
    // The Instrument Params key toggles pages 1/2.
    h.tap(btn::InstrParams);
    CHECK(a.ui.active() == kModSynth);
    CHECK(s.view == SynthScreen::View::Macros);
    CHECK(frameHas(h, "Synth Macros 2/2"));
    h.tap(btn::InstrParams);
    CHECK(s.view == SynthScreen::View::Params);
    // F2..F6 select the parameter; Left / Right move.
    h.tap(btn::F3);
    CHECK(s.param == 2);
    h.tap(btn::Right);
    CHECK(s.param == 3);
    // F8 Edit Patch: sections by Left / Right and by the pads.
    h.tap(btn::F8);
    CHECK(s.view == SynthScreen::View::Edit);
    CHECK(frameHas(h, "Edit Patch: Init"));
    CHECK(frameHas(h, "Osc"));
    CHECK(frameHas(h, "M7"));
    h.tap(btn::Right);
    CHECK(s.section == 1);
    h.pad(2);   // third cell of the first row: Amp Env
    CHECK(s.section == 2);
    h.pad(12);  // second row, first cell: Voice (ACD: Osc Filter AmpEnv Env1 / LFO1 Voice M1 M2 ...)
    CHECK(s.section == 4);
    h.tap(btn::Down);
    CHECK(s.row == 1);
    // Pad LEDs: the section block is lit, the current one full.
    uint8_t lv[48] = {};
    s.padLeds(lv);
    CHECK(lv[12] == a.cfg.padFull());
    CHECK(lv[0] == a.cfg.padDim());
    CHECK(lv[4] == 0);
    h.tap(btn::F8);
    CHECK(s.view == SynthScreen::View::Params);
    // F7 Load Patch: the popup; F7 Cancel.
    h.tap(btn::F7);
    CHECK(s.view == SynthScreen::View::Browser);
    CHECK(frameHas(h, "Load Patch"));
    CHECK(frameHas(h, "Cancel"));
    CHECK(frameHas(h, "Confirm"));
    h.tap(btn::F7);
    CHECK(s.view == SynthScreen::View::Params);
    // Instrument Params in the popup cancels it (page 1/2).
    h.tap(btn::F7);
    h.tap(btn::InstrParams);
    CHECK(s.view == SynthScreen::View::Params);
    // Instrument Params from the Edit view goes back to page 1.
    h.tap(btn::F8);
    h.tap(btn::InstrParams);
    CHECK(s.view == SynthScreen::View::Params);
}

TEST(record_edits_apply_live) {
    Harness h;
    App& a = *h.app;
    openSlot(h, 1);
    SynthScreen& s = synth(a);
    const SynthSlotRecord r0 = a.synthSlot(1);
    h.tap(btn::F2);   // Polyphony
    h.tap(btn::Up);
    CHECK(a.synthSlot(1).polyphony == r0.polyphony + 1);
    h.tap(btn::F3);   // Gain +0.5 dB x 2
    h.enc(2);
    CHECK(std::fabs(a.synthSlot(1).gainDb - (r0.gainDb + 1.0f)) < 1e-4f);
    h.tap(btn::F4);
    h.enc(-10);
    CHECK(a.synthSlot(1).pan == -10.0f);
    h.tap(btn::F5);
    h.enc(50);
    CHECK(a.synthSlot(1).reverbSend == 50.0f);
    CHECK(frameHas(h, "-20.00 dB"));   // SEND law: -40 + 0.4 x 50
    h.tap(btn::F6);
    h.enc(5);
    CHECK(a.synthSlot(1).delaySend == 5.0f);
    // The record reached the framework (next audio block with work).
    CHECK(a.engine.synth().slotView(1).polyphony == int(r0.polyphony) + 1);
    a.engine.synth().auditionOn(1, 60);
    h.audio.pump(1);
    a.engine.synth().auditionOff();
    h.audio.pump(1);
    CHECK(a.engine.synth().slotRecord(1).polyphony == r0.polyphony + 1);
    CHECK(a.engine.synth().slotRecord(1).pan == -10.0f);
    // Macros page: M1 page value.
    h.tap(btn::InstrParams);
    h.tap(btn::F1);
    h.enc(7);
    CHECK(a.synthSlot(1).macros[0] == uint8_t(r0.macros[0] + 7));
    (void)s;
}

TEST(macro_edit_takes_over_from_fx_override) {
    // User report: after an FX CC a set M1 during playback, M1 stayed yellow
    // and turning it did nothing until Stop. A hand edit (encoder, Up/Down,
    // Delete) now releases that macro's override.
    Harness h;
    App& a = *h.app;
    openSlot(h, 1);
    h.tap(btn::InstrParams);   // Synth Macros 2/2
    audio::SynthBank& bank = a.engine.synth();
    const uint8_t page0 = a.synthSlot(1).macros[0];
    bank.macroFx(1, 0, 100);
    bank.macroFx(1, 1, 100);
    CHECK(bank.slotView(1).overridden[0]);
    h.tap(btn::F1);
    h.enc(3);
    CHECK_EQ(int(a.synthSlot(1).macros[0]), int(page0) + 3);
    CHECK(!bank.slotView(1).overridden[0]);
    CHECK_EQ(int(bank.slotView(1).macro[0]), int(page0) + 3);
    CHECK(bank.slotView(1).overridden[1]);   // only the edited macro
    bank.auditionOn(1, 60);
    h.audio.pump(1);
    bank.auditionOff();
    h.audio.pump(1);
    CHECK(!bank.slotView(1).overridden[0]);
    // Delete on an overridden macro: back to its default, override gone.
    h.tap(btn::F2);
    h.tap(btn::Delete);
    CHECK(!bank.slotView(1).overridden[1]);
}

TEST(model_change_loads_init) {
    Harness h;
    App& a = *h.app;
    openSlot(h, 1);
    h.tap(btn::F2);
    h.tap(btn::F1);   // Model selected
    CHECK(a.synthSlot(1).model == uint32_t(SynthModel::ACD));
    h.tap(btn::F1);   // pressed again: next model (FAT)
    CHECK(a.synthSlot(1).model == uint32_t(SynthModel::FAT));
    CHECK(a.synthPatch(1).engine == kSynthEngineFat);
    CHECK(std::strcmp(a.synthSlot(1).patchName, "Init") == 0);
    h.tap(btn::Down);  // list: Down = next (VAP)
    CHECK(a.synthSlot(1).model == uint32_t(SynthModel::VAP));
    CHECK(a.synthPatch(1).engine == kSynthEngineVap);
    // The Model key cycles and wraps (manual p.147): VAP, WTFM, PERC, ACD.
    h.tap(btn::F1);
    h.tap(btn::F1);
    CHECK(a.synthSlot(1).model == uint32_t(SynthModel::PERC));
    h.tap(btn::F1);
    CHECK(a.synthSlot(1).model == uint32_t(SynthModel::ACD));
    CHECK(a.synthPatch(1).engine == kSynthEngineAcd);
    // The list stops at its ends.
    h.tap(btn::Up);
    CHECK(a.synthSlot(1).model == uint32_t(SynthModel::ACD));
}

TEST(parameter_edit_dirty_and_saved) {
    Harness h;
    App& a = *h.app;
    openSlot(h, 1);
    a.autosave();
    CHECK(!a.synthPatchDirty(1));
    SynthScreen& s = synth(a);
    h.tap(btn::F8);    // Edit Patch
    h.tap(btn::Right); // Filter: Cutoff first
    CHECK(s.section == 1 && s.row == 0);
    const float c0 = audio::synthParamGet(a.synthPatch(1), 9);
    h.enc(24);         // one octave up
    const float c1 = audio::synthParamGet(a.synthPatch(1), 9);
    CHECK(std::fabs(c1 - c0 * 2.0f) < 1.0f);
    CHECK(a.synthPatchDirty(1));
    // The framework re-reads the patch: the slot plays the new cutoff.
    a.engine.synth().auditionOn(1, 60);
    h.audio.pump(2);
    CHECK(std::fabs(a.engine.synth().params(1).end[9] - c1) < 1.0f);
    a.engine.synth().auditionOff();
    // F1 Save on "Init" asks for a name (Save As).
    h.tap(btn::F1);
    CHECK(a.ui.kb.isOpen());
    h.type("bass");
    h.tap(btn::F8);
    CHECK(!a.ui.kb.isOpen());
    CHECK(std::strcmp(a.synthSlot(1).patchName, "bass") == 0);
    CHECK(h.fs.files.count("Patches/ACD/bass.patch") == 1);
    SynthPatch back;
    const std::vector<uint8_t>& f = h.fs.files["Patches/ACD/bass.patch"];
    CHECK(decodePatch(f.data(), f.size(), &back) == PatchStatus::kOk);
    CHECK(std::fabs(audio::synthParamGet(back, 9) - c1) < 1e-3f);
    // A second edit + F1 Save overwrites the named file (no keyboard).
    h.enc(-24);
    h.tap(btn::F1);
    CHECK(!a.ui.kb.isOpen());
    const std::vector<uint8_t>& f2 = h.fs.files["Patches/ACD/bass.patch"];
    CHECK(decodePatch(f2.data(), f2.size(), &back) == PatchStatus::kOk);
    CHECK(std::fabs(audio::synthParamGet(back, 9) - c0) < 1.0f);
    // The autosave writes Workspace/1.patch and clears the dirty bit.
    a.autosave();
    CHECK(!a.synthPatchDirty(1));
    CHECK(h.fs.files.count("Workspace/1.patch") == 1);
}

TEST(save_as_keyboard_name) {
    Harness h;
    App& a = *h.app;
    openSlot(h, 2, SynthModel::VAP);
    h.tap(btn::F8);
    h.tap(btn::F2);   // Save As
    CHECK(a.ui.kb.isOpen());
    h.type("pad one");
    h.tap(btn::F8);
    CHECK(h.fs.files.count("Patches/VAP/pad one.patch") == 1);
    CHECK(std::strcmp(a.synthSlot(2).patchName, "pad one") == 0);
    CHECK(frameHas(h, "Edit Patch: pad one"));
    // Cancel (F7) writes nothing.
    h.tap(btn::F2);
    h.type("x");
    h.tap(btn::F7);
    CHECK(h.fs.files.count("Patches/VAP/x.patch") == 0);
    CHECK(std::strcmp(a.synthSlot(2).patchName, "pad one") == 0);
}

TEST(load_from_library) {
    Harness h(false);
    // Library: two ACD patches (one with a distinct cutoff and macro name), a
    // damaged one and a non-patch file.
    SynthPatch p;
    CHECK(patchInit(kSynthEngineAcd, &p));
    audio::synthParamSet(p, 9, 1234.0f);
    std::snprintf(p.macros[0].name, sizeof(p.macros[0].name), "Wobble");
    p.macros[0].nameLen = 6;
    h.fs.dirs.insert("Patches");
    h.fs.dirs.insert("Patches/ACD");
    h.fs.files["Patches/ACD/Acid Lead.patch"] = encode(p);
    SynthPatch q;
    CHECK(patchInit(kSynthEngineAcd, &q));
    h.fs.files["Patches/ACD/basic.patch"] = encode(q);
    h.fs.files["Patches/ACD/broken.patch"] = std::vector<uint8_t>{0xFF, 0xFF, 0xFF};
    h.fs.files["Patches/ACD/readme.txt"] = std::vector<uint8_t>{'x'};
    h.app->boot();
    App& a = *h.app;
    openSlot(h, 1);
    SynthScreen& s = synth(a);
    h.tap(btn::F7);
    CHECK(s.view == SynthScreen::View::Browser);
    CHECK(s.browserCount == 3);   // sorted, case-insensitive; .txt skipped
    CHECK(std::strcmp(s.browser[0], "Acid Lead") == 0);
    CHECK(std::strcmp(s.browser[1], "basic") == 0);
    CHECK(std::strcmp(s.browser[2], "broken") == 0);
    CHECK(frameHas(h, "Acid Lead"));
    h.tap(btn::F8);   // Confirm the first: loaded, the popup closes
    CHECK(s.view == SynthScreen::View::Params);
    CHECK(std::strcmp(a.synthSlot(1).patchName, "Acid Lead") == 0);
    CHECK(std::fabs(audio::synthParamGet(a.synthPatch(1), 9) - 1234.0f) < 1e-3f);
    CHECK(a.synthPatchSource(1) == App::PatchSource::Library);
    CHECK(a.synthPatchDirty(1));
    // The damaged file leaves the slot unchanged (Preview and Confirm).
    h.tap(btn::F7);
    CHECK_EQ(s.browserSel, 0);   // the loaded patch is highlighted
    h.tap(btn::Down);
    h.tap(btn::Down);
    h.tap(btn::F5);
    CHECK_EQ(s.previewSel(), -1);
    CHECK_EQ(a.synthPreviewSlot(), -1);
    h.tap(btn::F8);
    CHECK(s.view == SynthScreen::View::Browser);
    CHECK(std::strcmp(a.synthSlot(1).patchName, "Acid Lead") == 0);
    CHECK(std::fabs(audio::synthParamGet(a.synthPatch(1), 9) - 1234.0f) < 1e-3f);
    // Back on the Macros page the macro name shows in the label and legend.
    h.tap(btn::F7);
    h.tap(btn::InstrParams);
    CHECK(frameHas(h, "Wobble"));
}

// Load Patch popup (Tracker+ 1.0, docs/SYNTH_UX_MINI_VS_OT.md row 24): F5-F6
// Preview and the pads play the highlighted patch in the slot; Cancel puts
// the slot's patch back byte for byte; Confirm loads; leaving cancels.
TEST(load_popup_preview_cancel_confirm) {
    Harness h(false);
    SynthPatch pa;
    CHECK(patchInit(kSynthEngineAcd, &pa));
    audio::synthParamSet(pa, 9, 1234.0f);
    pa.macros[0].bipolar = true;
    SynthPatch pb;
    CHECK(patchInit(kSynthEngineAcd, &pb));
    audio::synthParamSet(pb, 9, 2000.0f);
    h.fs.dirs.insert("Patches");
    h.fs.dirs.insert("Patches/ACD");
    h.fs.files["Patches/ACD/Acid Lead.patch"] = encode(pa);
    h.fs.files["Patches/ACD/basic.patch"] = encode(pb);
    h.app->boot();
    App& a = *h.app;
    openSlot(h, 1);
    SynthScreen& s = synth(a);
    // An edited (dirty) patch in the slot.
    h.tap(btn::F8);
    h.tap(btn::Right);
    h.enc(5);
    h.tap(btn::F8);
    const float own = audio::synthParamGet(a.synthPatch(1), 9);
    SynthPatch before;
    std::memcpy(&before, &a.synthPatch(1), sizeof(SynthPatch));
    uint8_t recBefore[mt::kSynthSlotSize], rec[mt::kSynthSlotSize];
    a.synthSlot(1).encode(recBefore);
    const bool dirtyBefore = a.synthPatchDirty(1);
    const App::PatchSource srcBefore = a.synthPatchSource(1);
    CHECK(dirtyBefore);
    h.tap(btn::F7);
    CHECK(s.view == SynthScreen::View::Browser);
    CHECK(a.ui.popupHeader() == Ui::PopupHeader::kRightBar);
    CHECK_STR(a.ui.bar.label[4].top, "Preview");
    CHECK(a.ui.bar.label[4].wide);
    CHECK_STR(a.ui.bar.label[6].top, "Cancel");
    CHECK_STR(a.ui.bar.label[7].top, "Confirm");
    CHECK(frameHas(h, "Synth Parameters 1/2"));   // the page stays under the popup
    CHECK(frameHas(h, "basic"));
    // Moving the highlight loads nothing.
    h.tap(btn::Down);
    CHECK_EQ(s.browserSel, 1);
    CHECK_EQ(s.previewSel(), -1);
    CHECK(std::memcmp(&a.synthPatch(1), &before, sizeof(SynthPatch)) == 0);
    // F5 Preview: "basic" plays in the slot; the record, source and dirty
    // flag stay (no autosave of a previewed patch).
    h.down(btn::F5);
    h.audio.pump(4);
    CHECK(a.engine.synth().voicesOf(1) == 1);
    CHECK_EQ(s.previewSel(), 1);
    CHECK_EQ(a.synthPreviewSlot(), 1);
    CHECK(audio::synthParamGet(a.synthPatch(1), 9) == 2000.0f);
    a.synthSlot(1).encode(rec);
    CHECK(std::memcmp(rec, recBefore, sizeof(rec)) == 0);
    CHECK(!a.synthPatchDirty(1));
    h.up(btn::F5);
    h.audio.pump(4);
    const audio::SynthVoice* v = a.engine.synth().voiceOfTrack(audio::kSynthAuditionTrack);
    CHECK(v == nullptr || !v->gate);
    // A pad plays the highlighted patch: "Acid Lead" after Up.
    h.tap(btn::Up);
    h.push(InputEvent::kPadDown, 30);
    h.audio.pump(4);
    CHECK(a.engine.synth().voicesOf(1) == 1);
    CHECK(audio::synthParamGet(a.synthPatch(1), 9) == 1234.0f);
    h.push(InputEvent::kPadUp, 30);
    h.audio.pump(4);
    // Cancel: the slot's own patch, byte for byte.
    h.tap(btn::F7);
    CHECK(s.view == SynthScreen::View::Params);
    CHECK(a.ui.popupHeader() == Ui::PopupHeader::kNone);
    CHECK_EQ(a.synthPreviewSlot(), -1);
    CHECK(std::memcmp(&a.synthPatch(1), &before, sizeof(SynthPatch)) == 0);
    CHECK(audio::synthParamGet(a.synthPatch(1), 9) == own);
    a.synthSlot(1).encode(rec);
    CHECK(std::memcmp(rec, recBefore, sizeof(rec)) == 0);
    CHECK(a.synthPatchDirty(1) == dirtyBefore);
    CHECK(a.synthPatchSource(1) == srcBefore);
    // Confirm after a Preview loads the highlighted patch (edits: asks).
    h.tap(btn::F7);
    h.tap(btn::Down);
    h.tap(btn::F6);
    h.tap(btn::F8);
    CHECK(a.ui.askOpen());
    h.tap(btn::F8);   // Yes
    CHECK(s.view == SynthScreen::View::Params);
    CHECK(std::strcmp(a.synthSlot(1).patchName, "basic") == 0);
    CHECK(audio::synthParamGet(a.synthPatch(1), 9) == 2000.0f);
    CHECK(a.synthPatchSource(1) == App::PatchSource::Library);
    // Leaving the screen during a preview cancels it.
    std::memcpy(&before, &a.synthPatch(1), sizeof(SynthPatch));
    h.tap(btn::F7);
    h.tap(btn::Up);
    h.tap(btn::F5);
    CHECK(audio::synthParamGet(a.synthPatch(1), 9) == 1234.0f);
    h.tap(btn::Pattern);
    CHECK_EQ(a.synthPreviewSlot(), -1);
    CHECK(std::memcmp(&a.synthPatch(1), &before, sizeof(SynthPatch)) == 0);
    CHECK(std::strcmp(a.synthSlot(1).patchName, "basic") == 0);
}

TEST(init_patch_with_confirmation) {
    Harness h;
    App& a = *h.app;
    openSlot(h, 1);
    h.tap(btn::F8);
    h.tap(btn::Right);
    h.enc(10);
    const float edited = audio::synthParamGet(a.synthPatch(1), 9);
    CHECK(edited != 400.0f);
    h.tap(btn::F3);   // Init Patch: asks
    CHECK(a.ui.askOpen());
    h.tap(btn::F7);   // No
    CHECK(audio::synthParamGet(a.synthPatch(1), 9) == edited);
    h.tap(btn::F3);
    h.tap(btn::F8);   // Yes
    CHECK(!a.ui.askOpen());
    CHECK(audio::synthParamGet(a.synthPatch(1), 9) == 400.0f);
    CHECK(std::strcmp(a.synthSlot(1).patchName, "Init") == 0);
    CHECK(a.synthPatchSource(1) == App::PatchSource::Init);
}

TEST(macro_section_editing) {
    Harness h;
    App& a = *h.app;
    openSlot(h, 1);
    SynthScreen& s = synth(a);
    h.tap(btn::F8);
    const synthui::Section* sec = nullptr;
    const int n = synthui::sections(kSynthEngineAcd, &sec);
    int m1 = -1;
    for (int i = 0; i < n; ++i)
        if (sec[i].macro == 0) m1 = i;
    CHECK(m1 >= 0);
    s.section = m1;
    s.row = 0;
    // Name row: F8 "Edit" (Mini manual p.155) opens the naming keyboard; the
    // value column above F8 shows the Edit key.
    h.app->ui.dirty();
    h.run(40);
    CHECK_STR(a.ui.bar.label[7].top, "Edit");
    CHECK(frameHas(h, "Edit"));
    h.tap(btn::F8);
    CHECK(a.ui.kb.isOpen());
    h.tap(btn::F7);   // keyboard Cancel
    CHECK(!a.ui.kb.isOpen());
    CHECK(s.view == SynthScreen::View::Edit);
    // The encoder push still opens it.
    h.tap(btn::EncPush);
    CHECK(a.ui.kb.isOpen());
    for (int i = 0; i < 20; ++i) h.tap(btn::Delete);
    h.type("cut");
    h.tap(btn::F8);
    CHECK(std::strcmp(a.synthPatch(1).macros[0].name, "cut") == 0);
    CHECK(a.synthPatch(1).macros[0].nameLen == 3);
    // Bipolar toggles; F8 is Back again off the Name row.
    h.tap(btn::Down);
    h.app->ui.dirty();
    h.run(40);
    CHECK_STR(a.ui.bar.label[7].top, "Back");
    const bool bi = a.synthPatch(1).macros[0].bipolar;
    {   // the macro was turned away from its neutral value first
        SynthSlotRecord rec = a.synthSlot(1);
        rec.macros[0] = 20;
        a.setSynthSlotRecord(1, rec);
    }
    h.enc(1);
    CHECK(a.synthPatch(1).macros[0].bipolar == !bi);
    // User report: the value restarts at the new mode's neutral position
    // (bipolar centre 63 = "0", else 0), not at a leftover -63.
    CHECK_EQ(int(a.synthSlot(1).macros[0]), !bi ? 63 : 0);
    h.enc(1);
    CHECK(a.synthPatch(1).macros[0].bipolar == bi);
    CHECK_EQ(int(a.synthSlot(1).macros[0]), bi ? 63 : 0);
    h.enc(1);   // back to the toggled state for the rest of the test
    // A new target in the first free slot: Parameter row of target count.
    const int count = audio::synthMacroTargets(a.synthPatch(1).macros[0]);
    s.row = 2 + 2 * count;
    h.enc(1);   // first choice: Osc Saw Mix (index 0)
    const PatchMacro& mc = a.synthPatch(1).macros[0];
    CHECK(audio::synthMacroTargets(mc) == count + 1);
    CHECK(mc.targets[count].param == 0);
    CHECK(frameHas(h, "Osc Saw Mix"));
    // Its amount.
    h.tap(btn::Down);
    h.enc(-50);
    CHECK(std::fabs(patchBitsToFloat(mc.targets[count].amountBits) - 0.5f) < 1e-4f);
    // Back to Unassigned removes the trailing target.
    h.tap(btn::Up);
    h.enc(-1);
    CHECK(audio::synthMacroTargets(mc) == count);
    CHECK(a.synthPatchDirty(1));
}

TEST(not_available_is_read_only) {
    Harness h;
    App& a = *h.app;
    // A New Project's Synth 1 is ACD in 2.0. Every Mini model has an engine
    // since 2.2: WTFM is hidden here to test the "not available" path.
    audio::synthEngineHide(kSynthEngineWtfm, true);
    CHECK(a.synthSlot(0).model == uint32_t(SynthModel::ACD));
    a.setSynthModel(0, int(SynthModel::WTFM));
    CHECK(a.synthSlot(0).model == uint32_t(SynthModel::WTFM));
    a.setCurrentInstrument(64);
    h.tap(btn::Pattern);
    h.tap(btn::InstrParams);
    SynthScreen& s = synth(a);
    CHECK(a.ui.active() == kModSynth && s.slot == 0);
    CHECK(!s.available());
    CHECK(frameHas(h, "Synth 1 (WTFM) not available"));
    const SynthSlotRecord r0 = a.synthSlot(0);
    // Values cannot be selected or edited; F7 Load is off.
    h.tap(btn::F3);
    CHECK(s.param == 0);
    h.tap(btn::Right);
    CHECK(s.param == 0);
    h.tap(btn::F7);
    CHECK(s.view == SynthScreen::View::Params);
    CHECK(!a.ui.bar.label[2].enabled);
    // Edit Patch opens for viewing; edits, Init, Save, Save As do nothing.
    const SynthPatch p0 = a.synthPatch(0);
    h.tap(btn::F8);
    CHECK(s.view == SynthScreen::View::Edit);
    h.enc(5);
    h.tap(btn::F1);
    CHECK(!a.ui.askOpen());
    h.tap(btn::F2);
    h.tap(btn::F3);
    CHECK(!a.ui.kb.isOpen());
    CHECK(std::memcmp(&p0.params, &a.synthPatch(0).params, sizeof(p0.params)) == 0);
    h.tap(btn::F8);
    // Macros page is read-only too.
    h.tap(btn::InstrParams);
    h.enc(5);
    CHECK(a.synthSlot(0).macros[0] == r0.macros[0]);
    h.tap(btn::InstrParams);
    // The model can still be changed (to an available one).
    h.tap(btn::F1);
    h.tap(btn::Down);   // WTFM -> PERC
    CHECK(a.synthSlot(0).model == uint32_t(SynthModel::PERC));
    CHECK(s.available());
    CHECK(frameHas(h, "Synth 1 (PERC)"));
    audio::synthEngineHide(kSynthEngineWtfm, false);
}

// FAT has an engine since 2.1: the header names it without "not available",
// values edit, and the pads play the slot.
TEST(fat_slot_is_live) {
    Harness h;
    App& a = *h.app;
    a.setSynthModel(0, int(SynthModel::FAT));
    CHECK(a.synthPatch(0).engine == kSynthEngineFat);
    a.setCurrentInstrument(64);
    h.tap(btn::Pattern);
    h.tap(btn::InstrParams);
    SynthScreen& s = synth(a);
    CHECK(a.ui.active() == kModSynth && s.slot == 0);
    CHECK(s.available());
    CHECK(frameHas(h, "Synth 1 (FAT)"));
    CHECK(!frameHas(h, "not available"));
    // Edit Patch: the first value (Fatness) moves with the encoder.
    const float f0 = audio::synthParamGet(a.synthPatch(0), 0);
    h.tap(btn::F8);
    CHECK(s.view == SynthScreen::View::Edit);
    h.enc(5);
    CHECK(audio::synthParamGet(a.synthPatch(0), 0) > f0);
    h.tap(btn::F8);
    // Pads audition the FAT slot.
    h.push(InputEvent::kPadDown, 5);
    h.audio.pump(4);
    CHECK(a.engine.synth().voicesOf(0) == 1);
    h.push(InputEvent::kPadUp, 5);
    h.audio.pump(4);
}

TEST(pads_audition_the_slot) {
    Harness h;
    App& a = *h.app;
    openSlot(h, 1);
    h.push(InputEvent::kPadDown, 5);
    h.audio.pump(4);
    CHECK(a.engine.synth().voiceOfTrack(audio::kSynthAuditionTrack) != nullptr);
    CHECK(a.engine.synth().voicesOf(1) == 1);
    h.push(InputEvent::kPadUp, 5);
    h.audio.pump(4);
    const audio::SynthVoice* v = a.engine.synth().voiceOfTrack(audio::kSynthAuditionTrack);
    CHECK(v == nullptr || !v->gate);
}

// Tracker+ manual p.100 figure: "Synth 1 (WTFM)" .. "Synth 3 (VAP)" are the
// first three entries, before "1. ..."; the stored numbers stay 64..66. The
// pads keep the stock map (1-48 = sample instruments; synths by encoder only).
TEST(instrument_popup_lists_synths) {
    Harness h;
    App& a = *h.app;
    a.setSynthModel(0, int(SynthModel::VAP));
    h.tap(btn::Pattern);
    h.tap(btn::Rec);
    PatternScreen& ps = *static_cast<PatternScreen*>(a.ui.screen(kModPattern));
    ps.col = 1;   // instrument column
    a.setCurrentInstrument(0);
    h.down(btn::Instr);
    h.run(300);
    CHECK(ps.popup().isOpen());
    CHECK_EQ(ps.popup().cursor(), 3);   // instrument 1 sits after the synths
    CHECK(frameHas(h, "Synth 1 (VAP)"));
    CHECK(frameHas(h, "Synth 2 (ACD)"));
    CHECK(frameHas(h, "Synth 3 (VAP)"));
    CHECK(frameHas(h, "1. "));
    // Pads pick sample instruments (pad 5 = instrument 6), not the synths.
    h.pad(5);
    CHECK_EQ(synthui::popupInstrument(ps.popup().cursor()), 5);
    uint8_t lv[48] = {};
    ps.padLeds(lv);
    CHECK(lv[5] == a.cfg.padFull());
    CHECK(lv[0] == a.cfg.padDim());
    // Up to the first entry: Synth 1, then Synth 2; commit on release.
    for (int i = 0; i < 70; ++i) h.enc(-1);
    CHECK_EQ(ps.popup().cursor(), 0);
    h.enc(1);
    for (int p = 0; p < 48; ++p) lv[p] = 0xEE;
    ps.padLeds(lv);
    for (int p = 0; p < 48; ++p) CHECK(lv[p] != a.cfg.padFull());
    h.up(btn::Instr);
    CHECK(a.currentInstrument() == 65);
    CHECK(a.pattern().tracks[ps.track].steps[ps.step].instrument == 65);
    // The end of the list is MIDI 16 (63); a synth current instrument opens
    // the popup on its own entry.
    h.down(btn::Instr);
    h.run(300);
    CHECK_EQ(ps.popup().cursor(), 1);
    for (int i = 0; i < 70; ++i) h.enc(1);
    CHECK_EQ(synthui::popupInstrument(ps.popup().cursor()), 63);
    h.pad(2);   // on the MIDI page the pads pick MIDI 1..16
    CHECK_EQ(synthui::popupInstrument(ps.popup().cursor()), 50);
    h.up(btn::Instr);
    CHECK(a.currentInstrument() == 50);
}

// USER_REPORTS_R2 item 5 (Mini manual §1.8: "tracks 9-16 are used for MIDI
// and synths"): on tracks 9-16 the Instrument popup lists Synth 1..3, then
// MIDI 1..16; Instr-column pads 1-16 = MIDI 1..16, 17-19 = Synth 1..3 [OT].
TEST(midi_track_popup_lists_synths) {
    Harness h;
    App& a = *h.app;
    a.setSynthModel(0, int(SynthModel::VAP));
    h.tap(btn::Pattern);
    PatternScreen& ps = *static_cast<PatternScreen*>(a.ui.screen(kModPattern));
    for (int i = 0; i < kTrackCount; ++i) h.tap(btn::Right);
    while (ps.track > 12) h.tap(btn::Left);
    CHECK_EQ(ps.track, 12);   // track 13
    h.tap(btn::Rec);
    ps.col = 1;
    ps.step = 0;
    a.setCurrentInstrument(kFirstMidiInstrument + 2);   // MIDI 3
    h.down(btn::Instr);
    h.run(300);
    CHECK(ps.popup().isOpen());
    CHECK_EQ(ps.popup().kind(), 5);
    CHECK_EQ(ps.popup().cursor(), 5);   // MIDI 3 after the three synths
    CHECK(frameHas(h, "Synth 1 (VAP)"));
    CHECK(frameHas(h, "Synth 3 (VAP)"));
    CHECK(frameHas(h, "MIDI Channel 1"));
    for (int i = 0; i < 40; ++i) h.enc(1);
    CHECK(frameHas(h, "MIDI Channel 16"));
    CHECK_EQ(ps.popup().cursor(), synthui::kMidiPopupCount - 1);   // MIDI 16 ends the list
    CHECK_EQ(synthui::popupInstrumentMidi(ps.popup().cursor()), kInstrumentCount - 1);
    // Pads: 1-16 pick MIDI 1..16, 17-19 Synth 1..3, the rest nothing.
    h.pad(3);
    CHECK_EQ(synthui::popupInstrumentMidi(ps.popup().cursor()), kFirstMidiInstrument + 3);
    h.pad(17);
    CHECK_EQ(synthui::popupInstrumentMidi(ps.popup().cursor()), kFirstSynthInstrument + 1);
    h.pad(30);
    CHECK_EQ(synthui::popupInstrumentMidi(ps.popup().cursor()), kFirstSynthInstrument + 1);
    uint8_t lv[48] = {};
    ps.padLeds(lv);
    CHECK(lv[17] == a.cfg.padFull());
    CHECK(lv[0] == a.cfg.padDim());
    CHECK(lv[19] == 0);
    // Up to the first entry: Synth 1; commit on release.
    for (int i = 0; i < 40; ++i) h.enc(-1);
    CHECK_EQ(ps.popup().cursor(), 0);
    h.up(btn::Instr);
    CHECK(a.currentInstrument() == kFirstSynthInstrument);
    CHECK_EQ(int(a.pattern().tracks[12].steps[0].instrument), kFirstSynthInstrument);
    // A synth current instrument reopens the popup on its own entry (the
    // MIDI-only list put the cursor at 16, past its 16 entries).
    h.down(btn::Instr);
    h.run(300);
    CHECK_EQ(ps.popup().cursor(), 0);
    h.enc(1);
    h.enc(1);
    h.up(btn::Instr);
    CHECK(a.currentInstrument() == kFirstSynthInstrument + 2);
    // MIDI channels still commit as 48..63.
    h.down(btn::Instr);
    h.run(300);
    CHECK_EQ(ps.popup().cursor(), 2);
    h.enc(1);
    h.up(btn::Instr);
    CHECK(a.currentInstrument() == kFirstMidiInstrument);
    CHECK_EQ(int(a.pattern().tracks[12].steps[0].instrument), kFirstMidiInstrument);
    // Instr-column pads in edit mode: pad 18 writes Synth 2, pad 4 MIDI 5;
    // the step LEDs light pad 18 for the synth.
    ps.step = 1;
    h.pad(17);
    CHECK_EQ(int(a.pattern().tracks[12].steps[1].instrument), kFirstSynthInstrument + 1);
    ps.step = 1;
    for (int p = 0; p < 48; ++p) lv[p] = 0;
    ps.padLeds(lv);
    CHECK(lv[17] == a.cfg.padFull());
    ps.step = 2;
    h.pad(4);
    CHECK_EQ(int(a.pattern().tracks[12].steps[2].instrument), kFirstMidiInstrument + 4);
    ps.step = 3;
    h.pad(20);   // past the synths: the stock MIDI wrap
    CHECK_EQ(int(a.pattern().tracks[12].steps[3].instrument), kFirstMidiInstrument + 4);
}

// A synth step on track 13 plays the synth; a MIDI step on track 14 still
// sends MIDI (USER_REPORTS_R2 item 5).
TEST(midi_track_synth_step_plays_the_synth) {
    Harness h;
    App& a = *h.app;
    a.setSynthModel(0, int(SynthModel::VAP));
    a.cfg.midiOut = 3;   // MIDI out on both ports
    a.applySequencerSettings();
    Step& s13 = a.pattern().tracks[12].steps[0];
    s13.note = 60;
    s13.instrument = uint8_t(kFirstSynthInstrument);
    Step& s14 = a.pattern().tracks[13].steps[0];
    s14.note = 62;
    s14.instrument = uint8_t(kFirstMidiInstrument + 1);
    a.patternEdited();
    const size_t n0 = h.midi.sent.size();
    h.tap(btn::Pattern);
    h.tap(btn::Play);
    for (int i = 0; i < 6; ++i) {
        h.audio.pump(4);
        h.run(10);
    }
    const audio::SynthVoice* v = a.engine.synth().voiceOfTrack(12);
    CHECK(v != nullptr);
    if (v) CHECK_EQ(int(v->slot), 0);
    CHECK(a.engine.synth().voiceOfTrack(13) == nullptr);
    bool noteOn = false;
    for (size_t i = n0; i + 2 < h.midi.sent.size(); ++i)
        if (h.midi.sent[i] == 0x91 && h.midi.sent[i + 1] == 62) noteOn = true;   // channel 2, note 62
    CHECK(noteOn);
    if (!noteOn)
        for (size_t i = n0; i < h.midi.sent.size(); ++i) std::printf(" %02x", h.midi.sent[i]);
    h.tap(btn::Play);
}

// The title bar names a synth current instrument "Synth 1 (VAP)" (Tracker+
// 1.0, docs/SYNTH_UX_MINI_VS_OT.md row 3), never "65. MIDI Channel 17".
TEST(title_names_synth_instrument) {
    Harness h;
    App& a = *h.app;
    a.setSynthModel(0, int(SynthModel::VAP));
    a.setCurrentInstrument(kFirstSynthInstrument);
    h.tap(btn::Pattern);
    CHECK(frameHas(h, "Synth 1 (VAP)"));
    CHECK(!frameHas(h, "65. MIDI Channel 17"));
    a.setCurrentInstrument(kFirstSynthInstrument + 2);
    CHECK(frameHas(h, "Synth 3 (VAP)"));
    // A MIDI instrument keeps the stock text.
    a.setCurrentInstrument(kFirstMidiInstrument + 1);
    CHECK(frameHas(h, "50. MIDI Channel 2"));
    // Sample Playback / Sample Editor name the slot too.
    a.setCurrentInstrument(kFirstSynthInstrument + 1);
    h.tap(btn::SamplePlayback);
    CHECK(frameHas(h, "Synth 2 (ACD)"));
    h.tap(btn::SampleEditor);
    CHECK(frameHas(h, "Synth 2 (ACD)"));
    CHECK(frameHas(h, "Synth instrument"));
    CHECK(!frameHas(h, "MIDI instrument"));
}

TEST(global_mixer_synth_columns) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Master);
    h.tap(btn::Master);
    h.tap(btn::Master);
    MixerScreen& m = *static_cast<MixerScreen*>(a.ui.screen(kModMaster));
    CHECK(m.page == 2);
    CHECK(frameHas(h, "Synth 1"));
    CHECK(frameHas(h, "Synth 3"));
    CHECK(frameHas(h, "-9.00 dB"));
    // Hold F6 (Synth 2) + Up: +0.5 dB; release without change: no mute.
    const float g0 = a.synthSlot(1).gainDb;
    h.down(btn::F6);
    h.tap(btn::Up);
    h.up(btn::F6);
    CHECK(std::fabs(a.synthSlot(1).gainDb - (g0 + 0.5f)) < 1e-4f);
    const SynthSlotRecord r1 = a.synthSlot(1);
    h.tap(btn::F6);
    CHECK(a.synthSlot(1).gainDb == r1.gainDb);
    // Hold + Delete: 0 dB.
    h.down(btn::F5);
    h.tap(btn::Delete);
    h.up(btn::F5);
    CHECK(a.synthSlot(0).gainDb == 0.0f);
    CHECK(frameHas(h, "0.00 dB"));
    // Live meters (manual p.220 "right is the actual"): Synth 2 sounding
    // lights its meter only.
    CHECK_EQ(m.synthMeter[1], 0);
    audio::synthParamSet(a.synthPatch(1), 17, 1.0f);       // ACD Amp Sustain: a held tone
    audio::synthParamSet(a.synthPatch(1), 9, 5000.0f);     // Cutoff
    a.markSynthPatchDirty(1);
    a.engine.synth().auditionOn(1, 60);
    for (int i = 0; i < 30; ++i) {
        h.audio.pump(4);
        h.run(12);
    }
    CHECK(m.synthMeter[1] > 0);
    CHECK_EQ(m.synthMeter[0], 0);
    CHECK_EQ(m.synthMeter[2], 0);
    a.engine.synth().auditionOff();
    for (int i = 0; i < 400; ++i) {
        h.audio.pump(4);
        h.run(12);
    }
    CHECK_EQ(m.synthMeter[1], 0);
    CHECK_EQ(MixerScreen::synthMeterBar(1.0f), 100);
    CHECK_EQ(MixerScreen::synthMeterBar(0.0f), 0);
    CHECK_EQ(MixerScreen::synthMeterBar(std::pow(10.0f, -24.0f / 20.0f)), 50);
}

// Opening a project with a WTFM slot shows one notice (MINI_PLAN §3; FAT
// plays since 2.1); a project whose slots all play shows none.
namespace {
std::string g_notes;
void onNote(const char* t) { g_notes += std::string(t) + "\n"; }
}  // namespace

TEST(unavailable_slot_notice_on_open) {
    Harness h;
    App& a = *h.app;
    a.ui.platform.notified = &onNote;
    CHECK(a.saveProjectAs("AllPlay"));
    a.setSynthModel(1, int(SynthModel::FAT));   // plays since 2.1: not named
    a.setSynthModel(2, int(SynthModel::WTFM));
    CHECK(a.saveProjectAs("Dead"));
    CHECK(a.newProject());
    g_notes.clear();
    CHECK(a.openProject("Projects", "AllPlay"));
    CHECK(g_notes.find("not available") == std::string::npos);
    // WTFM plays since 2.2: no notice either.
    g_notes.clear();
    CHECK(a.openProject("Projects", "Dead"));
    CHECK(g_notes.find("not available") == std::string::npos);
    // An engine missing from the build is named once (WTFM hidden).
    audio::synthEngineHide(kSynthEngineWtfm, true);
    CHECK(a.newProject());
    g_notes.clear();
    CHECK(a.openProject("Projects", "Dead"));
    CHECK_STR(g_notes.c_str(), "Synth 3 (WTFM) not available\n");
    audio::synthEngineHide(kSynthEngineWtfm, false);
}

// Edit Patch bottom bar (manual p.153): Save, Save As, Init Patch, -, Up,
// the section, Down, Back; macro sections show 5 target pairs, a stored 6th
// target is kept.
TEST(edit_patch_bar_follows_manual) {
    Harness h;
    App& a = *h.app;
    openSlot(h, 1);
    SynthScreen& s = synth(a);
    h.tap(btn::F8);
    const BottomBar& b = a.ui.bar;
    CHECK_STR(b.label[0].top, "Save");
    CHECK_STR(b.label[1].top, "Save As");
    CHECK_STR(b.label[2].top, "Init");
    CHECK_STR(b.label[2].bottom, "Patch");
    CHECK(b.label[3].hidden && b.label[4].hidden && b.label[6].hidden);
    CHECK_STR(b.label[5].top, "Oscillator");
    CHECK_STR(b.label[7].top, "Back");
    CHECK_EQ(int(b.separatorMask() & 0x60), 0);
    // F7 / F5 move the row, F6 steps the section (and wraps).
    h.tap(btn::F7);
    h.tap(btn::F7);
    CHECK_EQ(s.row, 2);
    h.tap(btn::F5);
    CHECK_EQ(s.row, 1);
    h.tap(btn::F6);
    CHECK_EQ(s.section, 1);
    CHECK_EQ(s.row, 0);
    CHECK_STR(b.label[5].top, "Filter");
    const synthui::Section* sec = nullptr;
    const int n = synthui::sections(kSynthEngineAcd, &sec);
    s.section = n - 1;
    h.tap(btn::F6);
    CHECK_EQ(s.section, 0);
    // Macro sections: Name, Bipolar and 5 Parameter / Amount pairs.
    int m1 = -1;
    for (int i = 0; i < n; ++i)
        if (sec[i].macro == 0) m1 = i;
    s.section = m1;
    s.row = 0;
    CHECK_EQ(s.rows(), 12);
    CHECK(frameHas(h, "Parameter 5"));
    CHECK(!frameHas(h, "Parameter 6"));
    // A patch with six targets keeps the sixth through edits of the others.
    PatchMacro& mc = a.synthPatch(1).macros[0];
    mc.count = 6;
    for (int k = 0; k < 6; ++k) mc.targets[k] = PatchTarget{k, patchFloatToBits(0.1f * float(k + 1))};
    s.row = 3;   // Amount 1
    h.enc(5);
    CHECK_EQ(mc.count, 6);
    CHECK_EQ(mc.targets[5].param, 5);
    CHECK(mc.targets[5].amountBits == patchFloatToBits(0.6f));
}

// Entry resets the Edit Patch section (no stale index from another model),
// and focus never rests on a disabled column.
TEST(section_reset_and_no_disabled_focus) {
    Harness h;
    App& a = *h.app;
    openSlot(h, 2, SynthModel::VAP);   // 10 + 7 sections
    SynthScreen& s = synth(a);
    h.tap(btn::F8);
    s.section = 12;
    s.row = 5;
    h.tap(btn::Pattern);
    openSlot(h, 1);   // ACD: 6 + 7 sections
    CHECK_EQ(s.section, 0);
    CHECK_EQ(s.row, 0);
    h.tap(btn::F8);
    CHECK(frameHas(h, "Oscillator"));
    h.tap(btn::F8);
    // Params: F4 selected, then the slot becomes read-only (WTFM hidden:
    // a model without an engine in this build).
    h.tap(btn::F4);
    CHECK_EQ(s.param, 3);
    audio::synthEngineHide(kSynthEngineWtfm, true);
    a.setSynthModel(1, int(SynthModel::WTFM));
    h.app->ui.dirty();
    h.run(40);
    CHECK_EQ(s.param, 0);
    CHECK(a.ui.bar.label[0].highlight);
    CHECK(!a.ui.bar.label[3].highlight);
    // Macros page of a read-only slot: no focus.
    h.tap(btn::InstrParams);
    h.app->ui.dirty();
    h.run(40);
    for (int k = 0; k < 8; ++k) CHECK(!a.ui.bar.label[k].highlight);
    audio::synthEngineHide(kSynthEngineWtfm, false);
}

// "Discard edits?" before Load / a model change when the patch was edited;
// "Overwrite?" on Save As to an existing name; an empty name is refused.
TEST(patch_io_confirmations) {
    Harness h;
    App& a = *h.app;
    g_notes.clear();
    a.ui.platform.notified = &onNote;
    openSlot(h, 1);
    SynthScreen& s = synth(a);
    h.tap(btn::F8);
    h.tap(btn::F2);   // Save As "keep"
    for (int i = 0; i < 40; ++i) h.tap(btn::Delete);
    h.type("keep");
    h.tap(btn::F8);
    CHECK(h.fs.files.count("Patches/ACD/keep.patch") == 1);
    const std::vector<uint8_t> kept = h.fs.files["Patches/ACD/keep.patch"];
    // Edit, then Save As the same name: asks; Cancel writes nothing.
    h.tap(btn::Right);
    h.enc(10);
    h.tap(btn::F2);
    for (int i = 0; i < 40; ++i) h.tap(btn::Delete);
    h.type("keep");
    h.tap(btn::F8);
    CHECK(a.ui.askOpen());
    h.tap(btn::F7);   // Cancel
    CHECK(!a.ui.askOpen());
    CHECK(h.fs.files["Patches/ACD/keep.patch"] == kept);
    // Empty name: a notice, nothing written.
    g_notes.clear();
    h.tap(btn::F2);
    for (int i = 0; i < 40; ++i) h.tap(btn::Delete);
    h.tap(btn::F8);
    CHECK(!a.ui.askOpen());
    CHECK(g_notes.find("Patch name is empty") != std::string::npos);
    // Load with unsaved edits asks; No keeps the edit, Yes loads.
    const float edited = audio::synthParamGet(a.synthPatch(1), 9);
    h.tap(btn::F8);   // back to Params
    h.tap(btn::F7);   // Load Patch popup
    CHECK(s.view == SynthScreen::View::Browser);
    h.tap(btn::F8);   // Confirm
    CHECK(a.ui.askOpen());
    h.tap(btn::F7);   // No: the popup stays, the edit too
    CHECK(s.view == SynthScreen::View::Browser);
    CHECK(audio::synthParamGet(a.synthPatch(1), 9) == edited);
    h.tap(btn::F8);
    h.tap(btn::F8);   // Yes
    CHECK(s.view == SynthScreen::View::Params);
    CHECK(audio::synthParamGet(a.synthPatch(1), 9) != edited);
    // Clean after the load: the next Confirm does not ask.
    h.tap(btn::F7);
    h.tap(btn::F8);
    CHECK(!a.ui.askOpen());
    CHECK(s.view == SynthScreen::View::Params);
    // A model change with edits asks too.
    h.tap(btn::F8);   // Edit Patch
    h.tap(btn::Right);
    h.enc(3);
    h.tap(btn::F8);
    h.tap(btn::F1);
    h.tap(btn::F1);   // next model
    CHECK(a.ui.askOpen());
    h.tap(btn::F7);
    CHECK(a.synthSlot(1).model == uint32_t(SynthModel::ACD));
    h.tap(btn::F1);
    h.tap(btn::F8);
    CHECK(a.synthSlot(1).model == uint32_t(SynthModel::FAT));
    // Overwrite confirmed writes the file.
    a.setSynthModel(1, int(SynthModel::ACD));
    CHECK(a.loadSynthLibraryPatch(1, "keep"));
    h.tap(btn::F8);
    h.tap(btn::Right);
    h.enc(-7);
    h.tap(btn::F2);
    h.tap(btn::F8);   // the keyboard starts with the record name "keep"
    CHECK(a.ui.askOpen());
    h.tap(btn::F8);   // Overwrite
    CHECK(h.fs.files["Patches/ACD/keep.patch"] != kept);
}

// Delete resets the selected value (2x-05 §6.1 / §6.2).
TEST(delete_resets_to_default) {
    Harness h;
    App& a = *h.app;
    openSlot(h, 1);
    h.tap(btn::F3);   // Gain
    h.enc(10);
    CHECK(a.synthSlot(1).gainDb != -9.0f);
    h.tap(btn::Delete);
    CHECK(a.synthSlot(1).gainDb == -9.0f);
    h.tap(btn::F2);
    h.enc(3);
    h.tap(btn::Delete);
    CHECK_EQ(a.synthSlot(1).polyphony, 1u);
    h.tap(btn::F4);
    h.enc(-30);
    h.tap(btn::Delete);
    CHECK(a.synthSlot(1).pan == 0.0f);
    h.tap(btn::F1);
    h.tap(btn::Delete);   // the model is not reset
    CHECK(a.synthSlot(1).model == uint32_t(SynthModel::ACD));
    h.tap(btn::InstrParams);
    a.synthPatch(1).macros[2].bipolar = true;
    h.tap(btn::F3);
    h.enc(20);
    h.tap(btn::Delete);
    CHECK_EQ(a.synthSlot(1).macros[2], 63);
    h.tap(btn::F1);
    h.enc(20);
    h.tap(btn::Delete);
    CHECK_EQ(a.synthSlot(1).macros[0], 0);
}

// "Init patch loaded" after a model change and after Init Patch (Tracker+
// 1.0 frames V1 2:14-2:30; docs/SYNTH_UX_MINI_VS_OT.md rows 7, 22).
TEST(init_patch_loaded_notice) {
    Harness h;
    App& a = *h.app;
    g_notes.clear();
    a.ui.platform.notified = &onNote;
    openSlot(h, 1);
    h.tap(btn::F2);
    h.tap(btn::F1);
    CHECK(g_notes.empty());
    h.tap(btn::F1);   // Model pressed again: the next model
    CHECK(a.synthSlot(1).model == uint32_t(SynthModel::FAT));
    CHECK(g_notes.find("Init patch loaded") != std::string::npos);
    g_notes.clear();
    h.tap(btn::Down);   // the list moves too
    CHECK(g_notes.find("Init patch loaded") != std::string::npos);
    // Init Patch: only after Yes.
    a.setSynthModel(1, int(SynthModel::ACD));
    h.tap(btn::F8);
    g_notes.clear();
    h.tap(btn::F3);
    h.tap(btn::F7);   // No
    CHECK(g_notes.empty());
    h.tap(btn::F3);
    h.tap(btn::F8);   // Yes
    CHECK(g_notes.find("Init patch loaded") != std::string::npos);
}

// Delete in Edit Patch: the selected row back to the Init patch's value
// (Tracker+ tutorial V2 6:58; docs/SYNTH_UX_MINI_VS_OT.md row 15).
TEST(edit_patch_delete_resets_to_init) {
    Harness h;
    App& a = *h.app;
    openSlot(h, 1);
    SynthScreen& s = synth(a);
    SynthPatch init;
    CHECK(patchInit(kSynthEngineAcd, &init));
    h.tap(btn::F8);
    h.tap(btn::Right);   // Filter: Cutoff (index 9)
    h.enc(10);
    CHECK(audio::synthParamGet(a.synthPatch(1), 9) != patchBitsToFloat(init.params[9].bits));
    h.tap(btn::Delete);
    CHECK(s.view == SynthScreen::View::Edit);
    CHECK(a.synthPatch(1).params[9].present == init.params[9].present);
    CHECK(a.synthPatch(1).params[9].bits == init.params[9].bits);
    // Every row of every section of every engine resets to its Init value.
    for (SynthModel m : {SynthModel::ACD, SynthModel::VAP, SynthModel::PERC}) {
        a.setSynthModel(1, int(m));
        const int32_t eng = synthEngineId(m);
        CHECK(patchInit(eng, &init));
        const synthui::Section* sec = nullptr;
        const int n = synthui::sections(eng, &sec);
        for (int i = 0; i < n; ++i) {
            if (sec[i].macro >= 0) continue;
            for (int r = 0; r < sec[i].count; ++r) {
                s.section = i;
                s.row = r;
                h.enc(7);
                h.tap(btn::Delete);
                const int idx = sec[i].params[r];
                CHECK(a.synthPatch(1).params[idx].present == init.params[idx].present);
                CHECK(!init.params[idx].present || a.synthPatch(1).params[idx].bits == init.params[idx].bits);
            }
        }
    }
    // Macro rows: name, bipolar, a target and its amount.
    a.setSynthModel(1, int(SynthModel::ACD));
    CHECK(patchInit(kSynthEngineAcd, &init));
    const synthui::Section* sec = nullptr;
    const int n = synthui::sections(kSynthEngineAcd, &sec);
    int m1 = -1, m7 = -1;
    for (int i = 0; i < n; ++i) {
        if (sec[i].macro == 0) m1 = i;
        if (sec[i].macro == 6) m7 = i;
    }
    PatchMacro& mc = a.synthPatch(1).macros[0];
    std::snprintf(mc.name, sizeof(mc.name), "cut");
    mc.nameLen = 3;
    mc.bipolar = true;
    s.section = m1;
    s.row = 0;
    h.tap(btn::Delete);
    CHECK_STR(mc.name, "Macro 1");
    CHECK_EQ(int(mc.nameLen), 7);
    s.row = 1;
    h.tap(btn::Delete);
    CHECK(!mc.bipolar);
    s.row = 2;
    h.enc(3);   // a target on M1 (Init has none)
    CHECK_EQ(audio::synthMacroTargets(mc), 1);
    h.tap(btn::Down);
    h.enc(-25);
    h.tap(btn::Delete);   // amount: 1.00 (a new target's amount)
    CHECK(patchBitsToFloat(mc.targets[0].amountBits) == 1.0f);
    h.tap(btn::Up);
    h.tap(btn::Delete);   // Parameter 1: back to Unassigned
    CHECK_EQ(audio::synthMacroTargets(mc), 0);
    // M7 Velocity: its Init target (Volume, +1) comes back.
    PatchMacro& v = a.synthPatch(1).macros[6];
    s.section = m7;
    s.row = 2;
    h.enc(2);
    s.row = 3;
    h.enc(-40);
    CHECK(v.targets[0].param != init.macros[6].targets[0].param || v.targets[0].amountBits != init.macros[6].targets[0].amountBits);
    h.tap(btn::Delete);
    s.row = 2;
    h.tap(btn::Delete);
    CHECK_EQ(audio::synthMacroTargets(v), 1);
    CHECK_EQ(v.targets[0].param, init.macros[6].targets[0].param);
    s.row = 3;
    h.tap(btn::Delete);
    CHECK(v.targets[0].amountBits == init.macros[6].targets[0].amountBits);
    CHECK(a.synthPatchDirty(1));
}

// A model with an engine that holds another engine's patch is read-only but
// not "not available": the header names the patch's engine.
TEST(foreign_engine_patch_header) {
    Harness h;
    App& a = *h.app;
    openSlot(h, 1);
    CHECK(patchInit(kSynthEngineVap, &a.synthPatch(1)));
    a.markSynthPatchDirty(1);
    SynthScreen& s = synth(a);
    CHECK(!s.available());
    CHECK(frameHas(h, "Synth 2 (ACD) VAP patch"));
    CHECK(!frameHas(h, "Synth 2 (ACD) not available"));
    CHECK(frameHas(h, "Patch of another model"));
}

// Macro targets stay packed (no -1 inside count), like the factory files.
TEST(macro_targets_stay_packed) {
    Harness h;
    App& a = *h.app;
    openSlot(h, 1);
    SynthScreen& s = synth(a);
    h.tap(btn::F8);
    const synthui::Section* sec = nullptr;
    const int n = synthui::sections(kSynthEngineAcd, &sec);
    int m2 = -1;
    for (int i = 0; i < n; ++i)
        if (sec[i].macro == 1) m2 = i;
    s.section = m2;
    PatchMacro& mc = a.synthPatch(1).macros[1];
    mc.count = 0;
    for (auto& t : mc.targets) t = PatchTarget{0, 0};
    auto packed = [&mc]() {
        for (int j = 0; j < mc.count; ++j)
            if (mc.targets[j].param < 0) return false;
        for (int j = mc.count; j < kPatchMacroTargets; ++j)
            if (mc.targets[j].param != 0 || mc.targets[j].amountBits != 0) return false;
        return true;
    };
    s.row = 2 + 2 * 2;   // Parameter 3 on an empty macro: appended as target 1
    h.enc(1);
    CHECK_EQ(mc.count, 1);
    CHECK(packed());
    CHECK_EQ(s.row, 2);
    s.row = 2 + 2 * 4;   // Parameter 5: target 2
    h.enc(2);
    CHECK_EQ(mc.count, 2);
    CHECK(packed());
    const int second = mc.targets[1].param;
    s.row = 2;           // clear target 1: target 2 moves up
    h.enc(-10);
    CHECK_EQ(mc.count, 1);
    CHECK_EQ(mc.targets[0].param, second);
    CHECK(packed());
    // A hole read from a file (-1 inside count) still reads as empty and is
    // closed by the next edit.
    mc.count = 3;
    mc.targets[0] = PatchTarget{5, patchFloatToBits(0.5f)};
    mc.targets[1] = PatchTarget{-1, 0};
    mc.targets[2] = PatchTarget{7, patchFloatToBits(0.25f)};
    CHECK(frameHas(h, "Unassigned"));   // Mini manual p.173 (was "None")
    CHECK(!frameHas(h, "None"));
    s.row = 2 + 2 * 2;
    h.enc(1);
    CHECK_EQ(mc.count, 2);
    CHECK(packed());
    CHECK_EQ(mc.targets[0].param, 5);
    CHECK_EQ(s.row, 2 + 2 * 1);
}

// 32-character patch names end to end (the record holds 32 bytes; the Mini
// keeps 32 non-NUL bytes): browser, load, record, mark, library fallback,
// the Save As keyboard.
TEST(patch_names_32_chars) {
    Harness h(false);
    const char* name = "abcdefghijklmnopqrstuvwxyz012345";   // 32
    CHECK_EQ(std::strlen(name), size_t(32));
    SynthPatch p;
    CHECK(patchInit(kSynthEngineAcd, &p));
    audio::synthParamSet(p, 9, 777.0f);
    h.fs.dirs.insert("Patches");
    h.fs.dirs.insert("Patches/ACD");
    h.fs.files[std::string("Patches/ACD/") + name + ".patch"] = encode(p);
    h.app->boot();
    App& a = *h.app;
    openSlot(h, 1);
    SynthScreen& s = synth(a);
    h.tap(btn::F7);
    CHECK_EQ(s.browserCount, 1);
    CHECK_STR(s.browser[0], name);
    h.tap(btn::F8);   // Confirm
    CHECK(std::strcmp(a.synthSlot(1).patchName, name) == 0);
    // The record keeps all 32 bytes (no NUL).
    SynthSlotRecord r = a.synthSlot(1);
    uint8_t raw[mt::kSynthSlotSize];
    r.encode(raw);
    CHECK(std::memcmp(raw + mt::kSynthPatchName, name, 32) == 0);
    // The browser marks it as the slot's patch.
    h.tap(btn::F7);
    CHECK_EQ(s.browserSel, 0);
    h.tap(btn::F7);   // Cancel
    // Library fallback: no Workspace/1.patch -> Patches/ACD/<name>.patch.
    a.autosave();
    h.fs.files.erase("Workspace/1.patch");
    CHECK(a.loadWorkspace());
    CHECK(a.synthPatchSource(1) == App::PatchSource::Library);
    CHECK(std::fabs(audio::synthParamGet(a.synthPatch(1), 9) - 777.0f) < 1e-3f);
    // Save As takes a 32-character name from the keyboard.
    openSlot(h, 1);
    h.tap(btn::F8);
    h.tap(btn::F2);
    for (int i = 0; i < 40; ++i) h.tap(btn::Delete);
    h.type("0123456789012345678901234567890123");   // 34 typed, 32 kept
    CHECK_EQ(std::strlen(a.ui.kb.text()), size_t(32));
    h.tap(btn::F8);
    CHECK(h.fs.files.count("Patches/ACD/01234567890123456789012345678901.patch") == 1);
    CHECK(std::strcmp(a.synthSlot(1).patchName, "01234567890123456789012345678901") == 0);
}

// Display units per the Mini manual (p.154-169); stored values unchanged.
TEST(display_units_follow_manual) {
    char t[48];
    synthui::formatValue(kSynthEngineAcd, 10, 1.5f, t, sizeof t);    // Resonance 0..1.5
    CHECK_STR(t, "100 %");
    synthui::formatValue(kSynthEngineAcd, 10, 0.75f, t, sizeof t);
    CHECK_STR(t, "50 %");
    synthui::formatValue(kSynthEngineAcd, 8, -3.0f, t, sizeof t);    // Pitch LFO -3..3
    CHECK_STR(t, "-100 %");
    synthui::formatValue(kSynthEngineVap, 44, 2.0f, t, sizeof t);    // Mod Amt 1 -4..4
    CHECK_STR(t, "50 %");
    synthui::formatValue(kSynthEnginePerc, 2, 100.0f, t, sizeof t);  // Kick Body Attack
    CHECK_STR(t, "10.00 ms");
    synthui::formatValue(kSynthEnginePerc, 23, 0.8f, t, sizeof t);   // Snap Attack
    CHECK_STR(t, "0.08 ms");
    synthui::formatValue(kSynthEnginePerc, 1, 12.0f, t, sizeof t);   // Pitch C0..B8
    CHECK_STR(t, "C0");
    synthui::formatValue(kSynthEnginePerc, 1, 119.0f, t, sizeof t);
    CHECK_STR(t, "B8");
    synthui::formatValue(kSynthEnginePerc, 19, 60.0f, t, sizeof t);
    CHECK_STR(t, "C4");
    synthui::formatValue(kSynthEnginePerc, 0, 0.0f, t, sizeof t);    // Body Wave
    CHECK_STR(t, "Sat-Sin 0 %");
    synthui::formatValue(kSynthEnginePerc, 0, 175.0f, t, sizeof t);
    CHECK_STR(t, "Glitch 4 100 %");
    synthui::formatValue(kSynthEnginePerc, 0, 14.0f, t, sizeof t);
    CHECK_STR(t, "Triangle 30 %");
    // Tom: Trans Timbre (13, 0..2) 0-200 before Trans Level (12, 0..1) 0-100 %.
    CHECK_STR(synthui::paramUi(kSynthEnginePerc, 13)->label, "Trans Timbre");
    CHECK_STR(synthui::paramUi(kSynthEnginePerc, 12)->label, "Trans Level");
    synthui::formatValue(kSynthEnginePerc, 13, 2.0f, t, sizeof t);
    CHECK_STR(t, "200");
    const synthui::Section* sec = nullptr;
    const int n = synthui::sections(kSynthEnginePerc, &sec);
    CHECK(n > 7);
    CHECK_EQ(sec[1].params[7], 13);
    CHECK_EQ(sec[1].params[8], 12);
    CHECK_EQ(sec[7].params[0], 80);   // FX: Type, then Level
    CHECK_EQ(sec[7].params[1], 79);
    // Steps: 1 shown % (Resonance 0.015), 0.1 ms, one wave position.
    CHECK(std::fabs(synthui::stepValue(kSynthEngineAcd, 10, 0.75f, 1) - 0.765f) < 1e-5f);
    CHECK(std::fabs(synthui::stepValue(kSynthEnginePerc, 2, 1.8f, 1) - 3.0f) < 1e-5f);
    CHECK(synthui::stepValue(kSynthEnginePerc, 0, 10.0f, 1) == 11.0f);
}

// Stop (Play key while playing) cuts synth voices at once, like the stock
// sample tracks (the Mini manual does not specify release or cut).
TEST(stop_cuts_synth_voices) {
    Harness h;
    App& a = *h.app;
    audio::synthParamSet(a.synthPatch(1), 17, 1.0f);   // held tone
    audio::synthParamSet(a.synthPatch(1), 18, 3.0f);   // long release
    a.markSynthPatchDirty(1);
    Step& st = a.pattern().tracks[0].steps[0];
    st.note = 60;
    st.instrument = 65;
    a.patternEdited();
    h.tap(btn::Play);
    for (int i = 0; i < 10; ++i) {
        h.audio.pump(4);
        h.run(10);
    }
    CHECK(a.engine.synth().sounding() > 0);
    h.tap(btn::Play);
    h.audio.pump(1);
    CHECK_EQ(a.engine.synth().busyRecords(), 0);
}

// Issue #117: in edit mode the note encoder on a synth step held the note
// forever. Stock re-triggers the edited row (spec 06 §1.6 Encoder, Note
// column), so the note gets one step and then the patch's release.
TEST(note_encoder_on_a_synth_step_releases) {
    Harness h;
    App& a = *h.app;
    audio::synthParamSet(a.synthPatch(1), 17, 1.0f);    // held tone
    audio::synthParamSet(a.synthPatch(1), 18, 0.01f);   // short release
    a.markSynthPatchDirty(1);
    Step& st = a.pattern().tracks[0].steps[0];
    st.note = 60;
    st.instrument = 65;
    a.patternEdited();
    h.tap(btn::Rec);
    CHECK(a.editMode);
    h.enc(1);
    CHECK_EQ(int(a.pattern().tracks[0].steps[0].note), 61);
    bool sounded = false;
    for (int i = 0; i < 10; ++i) {
        h.audio.pump(4);
        h.run(10);
        sounded = sounded || a.engine.synth().sounding() > 0;
    }
    CHECK(sounded);
    for (int i = 0; i < 200; ++i) {   // ~2 s: one step at 130 BPM, then the release
        h.audio.pump(4);
        h.run(10);
    }
    CHECK_EQ(a.engine.synth().sounding(), 0);
}

TEST(synth_edit_wheel_hint_lists_and_values) {
    // The simulator flips its wheel on lists (Ui::encoderOnList): in Edit
    // Patch the enum rows (Filter Type, LFO Waveform) are lists, value rows
    // (Cutoff, Resonance) are not; the patch browser is a list.
    Harness h;
    App& a = *h.app;
    openSlot(h, 1);
    SynthScreen& s = synth(a);
    if (s.view != SynthScreen::View::Params) h.tap(btn::InstrParams);
    h.tap(btn::F8);
    CHECK(s.view == SynthScreen::View::Edit);
    const synthui::Section* sec = nullptr;
    const int n = synthui::sections(s.engine(), &sec);
    int lists = 0, values = 0;
    for (int i = 0; i < n; ++i) {
        if (std::strcmp(sec[i].name, "Filter") != 0) continue;
        s.section = i;
        for (int r = 0; r < sec[i].count; ++r) {
            s.row = r;
            const synthui::ParamUi* u = synthui::paramUi(s.engine(), sec[i].params[r]);
            const bool isEnum = u && u->fmt == synthui::kFmtEnum;
            CHECK_EQ(a.ui.encoderOnList(), isEnum);
            (isEnum ? lists : values)++;
        }
    }
    CHECK(lists >= 1);
    CHECK(values >= 2);
}

TEST(instr_hold_chooser_on_the_synth_page) {
    // User report: holding Instr on a synth page did nothing. It opens the
    // instrument chooser (samples, MIDI, then Synth 1..3); releasing it opens
    // the picked instrument's page.
    Harness h;
    App& a = *h.app;
    openSlot(h, 0);
    CHECK(a.ui.active() == kModSynth);
    h.down(btn::Instr);
    h.run(250);
    CHECK(a.ui.picker.isOpen());
    CHECK_EQ(a.ui.picker.cursor(), kFirstSynthInstrument);
    // Pick sample instrument 3: the Instrument page.
    for (int i = 0; i < 8; ++i) h.app->ui.picker.move(-8);
    h.app->ui.picker.move(2);
    h.up(btn::Instr);
    h.run(60);
    CHECK_EQ(a.currentInstrument(), 2);
    CHECK(a.ui.active() == kModInstrument);
    // And back to Synth 2 from the Instrument page's chooser.
    h.down(btn::Instr);
    h.run(250);
    CHECK(a.ui.picker.isOpen());
    h.app->ui.picker.move(kFirstSynthInstrument + 1 - 2);
    h.up(btn::Instr);
    h.run(60);
    CHECK_EQ(a.currentInstrument(), kFirstSynthInstrument + 1);
    CHECK(a.ui.active() == kModSynth);
}

TEST(wtfm_ratio_shows_its_value) {
    // WTFM Ratio (spec 2x-08 §1): index i shown as (i + 1) / 4, one index per detent.
    char t[16];
    synthui::formatValue(kSynthEngineWtfm, 0, 3.0f, t, sizeof t);
    CHECK_STR(t, "1.00");
    synthui::formatValue(kSynthEngineWtfm, 1, 0.0f, t, sizeof t);
    CHECK_STR(t, "0.25");
    synthui::formatValue(kSynthEngineWtfm, 0, 47.0f, t, sizeof t);
    CHECK_STR(t, "12.00");
    CHECK_NEAR(synthui::stepValue(kSynthEngineWtfm, 0, 3.0f, 1), 4.0f, 1e-6);
    CHECK_NEAR(synthui::stepValue(kSynthEngineWtfm, 1, 3.0f, -2), 1.0f, 1e-6);
    // FM 0..10 reads 0..1000 %.
    synthui::formatValue(kSynthEngineWtfm, 11, 10.0f, t, sizeof t);
    CHECK_STR(t, "1000 %");
}

// Stereo S3: Width is the last row of the Voice section (ACD, FAT, VAP, WTFM),
// FAT Stereo the last row of FAT's Osc section. The rows edit, show and reset
// like any other; macros cannot target them; PERC has neither.
TEST(width_and_stereo_rows) {
    Harness h;
    App& a = *h.app;
    SynthScreen& s = synth(a);
    for (SynthModel m : {SynthModel::ACD, SynthModel::FAT, SynthModel::VAP, SynthModel::WTFM}) {
        openSlot(h, 1, m);
        const int32_t eng = synthEngineId(m);
        const int wi = audio::synthWidthParam(eng);
        const synthui::Section* sec = nullptr;
        const int n = synthui::sections(eng, &sec);
        int voice = -1;
        for (int i = 0; i < n; ++i)
            if (sec[i].macro < 0 && std::strcmp(sec[i].name, "Voice") == 0) voice = i;
        CHECK(voice >= 0);
        if (voice < 0) continue;
        CHECK_EQ(int(sec[voice].params[sec[voice].count - 1]), wi);
        h.tap(btn::F8);   // Edit view
        s.section = voice;
        s.row = sec[voice].count - 1;
        CHECK(frameHas(h, "Width"));
        CHECK(frameHas(h, "0 %"));
        h.enc(25);
        CHECK(audio::synthParamGet(a.synthPatch(1), wi) > 0.2f);
        CHECK(a.synthPatchDirty(1));
        h.tap(btn::Delete);   // back to the Init value: absent (mono)
        CHECK(audio::synthParamGet(a.synthPatch(1), wi) == 0.0f);
        CHECK(!(wi - audio::synthParamCount(eng) < a.synthPatch(1).extCount &&
                a.synthPatch(1).ext[wi - audio::synthParamCount(eng)].present));
        // Macro target list: Mini parameters only.
        for (int k = 1; k < synthui::targetChoices(eng); ++k)
            CHECK(synthui::targetChoice(eng, k) < audio::synthParamCount(eng));
        CHECK_EQ(synthui::targetChoiceIndex(eng, wi), 0);
    }
    // FAT Stereo: last row of Osc.
    openSlot(h, 1, SynthModel::FAT);
    const synthui::Section* sec = nullptr;
    const int n = synthui::sections(kSynthEngineFat, &sec);
    CHECK(n > 0 && std::strcmp(sec[0].name, "Osc") == 0);
    CHECK_EQ(int(sec[0].params[sec[0].count - 1]), audio::synthFatStereoParam());
    char label[32];
    synthui::paramFullName(kSynthEngineFat, audio::synthFatStereoParam(), label, sizeof label);
    CHECK_STR(label, "Osc Stereo");
    // PERC: no Width row anywhere.
    const int np = synthui::sections(kSynthEnginePerc, &sec);
    for (int i = 0; i < np; ++i)
        for (int r = 0; r < sec[i].count; ++r) CHECK(sec[i].params[r] < audio::synthParamCount(kSynthEnginePerc));
}

// A macro name with a control byte (a device report: "Cutoff\r" from a patch
// made elsewhere drew "Cutoff" with a stray glyph) is shown without it; the
// patch keeps its bytes.
TEST(macro_names_hide_control_bytes) {
    Harness h;
    App& a = *h.app;
    openSlot(h, 1);
    PatchMacro& m = a.synthPatch(1).macros[0];
    std::snprintf(m.name, sizeof(m.name), "Cutoff\r");
    m.nameLen = 7;
    h.tap(btn::InstrParams);   // Synth Macros 2/2
    CHECK(frameHas(h, "Cutoff"));
    CHECK(frameHas(h, "M1 Cutoff"));
    bool raw = false;
    for (const std::string& l : h.disp.last) raw |= l.find('\r') != std::string::npos;
    CHECK(!raw);
    CHECK_EQ(int(a.synthPatch(1).macros[0].nameLen), 7);   // stored as read
}
