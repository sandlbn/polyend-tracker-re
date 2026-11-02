// Pads held together on a synth instrument play a chord up to the slot's
// polyphony (tester report on 3.0.0 preview: JU6 at Polyphony 4 played only the last
// pad held, from the pattern editor and the synth page; the sim's computer
// keyboard sends the same pad presses). Each held pad takes a synth live
// track, as MIDI input notes do; the Enc-push audition stays one note.
#include "test.h"
#include "ui_fakes.h"

#include "audio/synth_bank.h"
#include "audio/synth_engine.h"
#include "ui/screens.h"

using namespace ot;

namespace {

// Notes held (gate on) by the synth framework, on every track.
int gated(App& a) {
    int n = 0;
    for (int t = 0; t < audio::kSynthAllTracks; ++t) {
        const audio::SynthVoice* v = a.engine.synth().voiceOfTrack(t);
        n += v && v->gate ? 1 : 0;
    }
    return n;
}

// The model whose engine is `engineId`.
void setEngine(App& a, int s, int32_t engineId) {
    for (int m = 0; m < kSynthModelCount; ++m)
        if (synthEngineId(SynthModel(m)) == engineId) a.setSynthModel(s, m);
}

// Slot 0 on `engine` at polyphony `poly`, current instrument S1; `synthPage`
// opens its synth page, else the pattern editor is shown.
void setUp(Harness& h, int32_t engine, int poly, bool synthPage) {
    App& a = *h.app;
    setEngine(a, 0, engine);
    SynthSlotRecord r = a.synthSlot(0);
    r.polyphony = uint32_t(poly);
    a.setSynthSlotRecord(0, r);
    a.setCurrentInstrument(kFirstSynthInstrument);
    h.tap(btn::Pattern);
    if (synthPage) h.tap(btn::InstrParams);
    h.audio.pump(2);
}

void padDown(Harness& h, uint8_t p) {
    h.push(InputEvent::kPadDown, p);
    h.audio.pump(2);
}
void padUp(Harness& h, uint8_t p) {
    h.push(InputEvent::kPadUp, p);
    h.audio.pump(2);
}

// The poly engines. Not PERC: every hit hands the whole kit, its ringing
// drums included, to the newest voice, so its chords are not counted in voices.
template <typename F>
void forEachPolyEngine(F f) {
    for (int32_t id : {kSynthEngineAcd, kSynthEngineFat, kSynthEngineVap, kSynthEngineWtfm})
        if (audio::synthEngineFor(id)) f(id);
}

}  // namespace

TEST(pads_play_a_chord_in_the_pattern_editor) {
    forEachPolyEngine([](int32_t id) {
        Harness h;
        setUp(h, id, 4, false);
        padDown(h, 0);   // pad 0: voice id 0 is a real pad, not "free"
        padDown(h, 4);
        padDown(h, 7);
        CHECK_EQ(gated(*h.app), 3);
        padUp(h, 4);
        CHECK_EQ(gated(*h.app), 2);
        padUp(h, 0);
        padUp(h, 7);
        CHECK_EQ(gated(*h.app), 0);
    });
}

TEST(pads_play_a_chord_on_the_synth_page) {
    forEachPolyEngine([](int32_t id) {
        Harness h;
        setUp(h, id, 4, true);
        CHECK_EQ(h.app->ui.active(), kModSynth);
        padDown(h, 20);
        padDown(h, 24);
        padDown(h, 27);
        CHECK_EQ(gated(*h.app), 3);
        padUp(h, 24);
        CHECK_EQ(gated(*h.app), 2);
        // Leaving the page releases the held pads.
        h.tap(btn::Pattern);
        h.audio.pump(2);
        CHECK_EQ(gated(*h.app), 0);
    });
}

TEST(pads_follow_the_polyphony) {
    forEachPolyEngine([](int32_t id) {
        // Polyphony 2: the slot plays two of the three held pads.
        Harness h;
        setUp(h, id, 2, false);
        padDown(h, 0);
        padDown(h, 4);
        padDown(h, 7);
        CHECK_EQ(gated(*h.app), 2);
        // Polyphony 1: one voice (legato / note priority as for MIDI keys).
        Harness m;
        setUp(m, id, 1, false);
        padDown(m, 0);
        padDown(m, 4);
        CHECK_EQ(gated(*m.app), 1);
        padUp(m, 0);
        padUp(m, 4);
        CHECK_EQ(gated(*m.app), 0);
    });
}

TEST(pads_and_enc_push_audition_share_the_page) {
    forEachPolyEngine([](int32_t id) {
        Harness h;
        setUp(h, id, 4, true);
        padDown(h, 20);
        // The centre button auditions note 60 on top of the held pad and
        // releases it on its own release; the pad keeps sounding.
        h.down(btn::EncPush);
        h.audio.pump(2);
        CHECK_EQ(gated(*h.app), 2);
        h.up(btn::EncPush);
        h.audio.pump(2);
        CHECK_EQ(gated(*h.app), 1);
        padUp(h, 20);
        CHECK_EQ(gated(*h.app), 0);
    });
}

TEST(pads_play_up_to_polyphony_8) {
    forEachPolyEngine([](int32_t id) {
        // Config "Synth voices" 8 (the default budget is 4 voices in all).
        Harness h;
        h.app->cfg.synthVoices = 8;
        h.app->engine.governor().setBudget(8);
        setUp(h, id, 8, false);
        CHECK_EQ(h.app->engine.synth().slotView(0).playable, 8);
        const uint8_t p[6] = {0, 2, 4, 5, 7, 9};
        for (uint8_t q : p) padDown(h, q);
        CHECK_EQ(gated(*h.app), 6);
        for (uint8_t q : p) padUp(h, q);
        CHECK_EQ(gated(*h.app), 0);
    });
}

TEST(pads_and_midi_keys_share_the_live_tracks) {
    forEachPolyEngine([](int32_t id) {
        Harness h;
        h.app->cfg.notesIn = 1;
        setUp(h, id, 8, false);
        auto midi = [&](uint8_t st, uint8_t n) {
            h.midi.in.insert(h.midi.in.end(), {st, n, uint8_t(st == 0x90 ? 100 : 0)});
            h.run(5);
            h.audio.pump(2);
        };
        midi(0x90, 50);
        padDown(h, 0);
        padDown(h, 4);
        CHECK_EQ(gated(*h.app), 3);
        midi(0x80, 50);   // the key releases its own note only
        CHECK_EQ(gated(*h.app), 2);
        padUp(h, 0);
        padUp(h, 4);
        CHECK_EQ(gated(*h.app), 0);
    });
}

TEST(pad_released_after_opening_edit_patch) {
    forEachPolyEngine([](int32_t id) {
        // A pad of the section block held on Params, then Edit Patch (F8):
        // its release still ends the note.
        Harness h;
        setUp(h, id, 4, true);
        padDown(h, 0);
        CHECK_EQ(gated(*h.app), 1);
        h.tap(btn::F8);
        CHECK(static_cast<SynthScreen*>(h.app->ui.screen(kModSynth))->view == SynthScreen::View::Edit);
        padUp(h, 0);
        CHECK_EQ(gated(*h.app), 0);
    });
}
