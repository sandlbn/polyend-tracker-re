// [I] Config > General > Pad layout "Piano" (not in stock): in each row
// pair the white keys along the lower row and the black keys above them,
// between their white keys; the upper pair an octave up. Gaps play nothing and
// the pad lights draw the keyboard.
#include "test.h"
#include "ui_fakes.h"
#include "nvstore_fake.h"

#include "app/project_fields.h"
#include "audio/synth_bank.h"
#include "audio/synth_engine.h"
#include "ui/notes.h"
#include "ui/screens.h"

using namespace ot;

namespace {
constexpr uint8_t kChromatic = 0, kMajor = 2;   // scale indices (kScaleNames)
// Physical pads (row 0 = top): the bottom row pair is the lower octave.
constexpr uint8_t kLowC = 36, kLowD = 37, kLowCs = 25, kLowGap = 24, kHighC = 12, kHighCs = 1;
PatternScreen& pe(Harness& h) { return *static_cast<PatternScreen*>(h.app->ui.screen(kModPattern)); }
}  // namespace

TEST(piano_map_white_row_black_row) {
    PadNotes p;
    p.build(kChromatic, 0, 24, true);   // root C3: the bottom C is note 36
    CHECK(p.piano());
    // The bottom row: C D E F G A B C D E F G.
    const int white[12] = {36, 38, 40, 41, 43, 45, 47, 48, 50, 52, 53, 55};
    for (int c = 0; c < 12; ++c) CHECK_EQ(p.note(uint8_t(36 + c)), white[c]);
    // Above it: each black key over the white key above it, none over C and F.
    const int black[12] = {-1, 37, 39, -1, 42, 44, 46, -1, 49, 51, -1, 54};
    for (int c = 0; c < 12; ++c)
        CHECK_EQ(int(p.note(uint8_t(24 + c))), black[c] < 0 ? int(PadNotes::kNoNote) : black[c]);
    // The upper pair: the same keyboard an octave up.
    CHECK_EQ(p.note(kHighC), 48);
    CHECK_EQ(p.note(kHighCs), 49);
    CHECK_EQ(p.note(0), PadNotes::kNoNote);
    CHECK_EQ(p.note(3), PadNotes::kNoNote);
    CHECK_EQ(p.note(23), 67);
    // The root's octave, not its pitch: D3 starts on the same C.
    p.build(kChromatic, 0, 26, true);
    CHECK_EQ(p.note(kLowC), 36);
    p.build(kChromatic, 0, 36, true);   // C4
    CHECK_EQ(p.note(kLowC), 48);
    // Stock again: no gaps.
    p.build(kChromatic, 0, 24, false);
    CHECK(!p.piano());
    for (uint8_t i = 0; i < 48; ++i) CHECK(p.note(i) != PadNotes::kNoNote);
}

TEST(piano_key_lights) {
    PadNotes p;
    uint8_t lv[48] = {};
    p.build(kChromatic, 0, 24, false);
    CHECK(!p.pianoLeds(lv, 10, 31));   // Stock: untouched
    for (uint8_t v : lv) CHECK_EQ(v, 0);
    p.build(kChromatic, 0, 24, true);
    CHECK(p.pianoLeds(lv, 10, 31));
    CHECK_EQ(lv[kLowC], 20);   // root brightest (still below a pressed pad)
    CHECK_EQ(lv[kHighC], 20);
    CHECK_EQ(lv[kLowD], 10);   // white key
    CHECK_EQ(lv[kLowCs], 6);   // black key dimmer
    CHECK_EQ(lv[kLowGap], 0);  // gaps dark
    CHECK_EQ(lv[0], 0);
    // D major: D brightest, C and C# outside the scale barely lit, F# a
    // black key in the scale.
    std::memset(lv, 0, sizeof(lv));
    p.build(kMajor, 0, 26, true);
    p.pianoLeds(lv, 10, 31);
    CHECK_EQ(lv[kLowD], 20);
    CHECK_EQ(lv[kLowC], 2);
    CHECK_EQ(lv[kLowCs], 6);   // C# is in D major
    CHECK_EQ(lv[28], 6);       // F#
    CHECK_EQ(lv[39], 2);       // F
    // Low pad brightness: never darker than 1, so the keyboard stays visible.
    std::memset(lv, 0, sizeof(lv));
    p.pianoLeds(lv, 3, 10);
    CHECK_EQ(lv[kLowC], 1);
    CHECK_EQ(lv[kLowCs], 1);
    CHECK_EQ(lv[kLowD], 6);
}

TEST(piano_config_record) {
    FakeNvStore nv;
    Config c;
    CHECK_EQ(c.padLayout, 0);
    c.padLayout = 1;   // alone writes the record
    CHECK(c.save(nv));
    CHECK_EQ(nv.mem[0xF4 + 4], 28);       // payload length
    CHECK_EQ(nv.mem[0xF4 + 5 + 27], 1);   // payload byte 28
    Config r;
    CHECK(r.load(nv));
    CHECK_EQ(r.padLayout, 1);
    c.padLayout = 2;
    c.sanitize();
    CHECK_EQ(c.padLayout, 0);
    // A record from before the byte (payload 27) keeps Stock.
    Config o;
    o.synthVoices = 6;
    uint8_t ext[Config::kExtSize];
    o.toExt(ext);
    ext[4] = 27;
    uint8_t sum = 0;
    for (int i = 0; i < 27; ++i) sum = uint8_t(sum + ext[5 + i]);
    ext[5 + 27] = sum;
    Config k;
    k.padLayout = 1;
    CHECK(k.fromExt(ext, 5 + 27 + 1));
    CHECK_EQ(k.synthVoices, 6);
    CHECK_EQ(k.padLayout, 0);
    // File fallback.
    FakeStorage fs;
    c.padLayout = 1;
    CHECK(c.save(fs));
    Config f;
    CHECK(f.load(fs));
    CHECK_EQ(f.padLayout, 1);
}

TEST(piano_gap_records_nothing_and_lights_show) {
    Harness h;
    App& a = *h.app;
    a.project.setU8(pf::kRootNote, 24);
    a.project.setU8(pf::kScale, kChromatic);
    a.cfg.padLayout = 1;
    h.run(40);
    // The Pattern screen (Note column, not editing) draws the keyboard.
    CHECK(h.leds.level[kLowC] > h.leds.level[kLowD]);
    CHECK(h.leds.level[kLowD] > h.leds.level[kLowCs]);
    CHECK(h.leds.level[kLowCs] > 0);
    CHECK_EQ(h.leds.level[kLowGap], 0);
    // Edit mode: a gap enters nothing, a key enters its note.
    h.tap(btn::Rec);
    const int s0 = pe(h).step;
    h.pad(kLowGap);
    CHECK_EQ(pe(h).step, s0);
    CHECK_EQ(a.pattern().tracks[0].steps[s0].note, -1);
    h.pad(kLowCs);
    CHECK_EQ(a.pattern().tracks[0].steps[s0].note, 37);
    h.tap(btn::Rec);
    // Stock: no key lights.
    a.cfg.padLayout = 0;
    h.run(40);
    CHECK_EQ(h.leds.level[kLowC], 0);
}

TEST(piano_key_lights_are_cached) {
    // The 48 levels are built with the table (PadNotes::build), not on every
    // main-loop pass: unchanged settings draw the cached levels.
    PadNotes p;
    uint8_t lv[48] = {};
    p.build(kMajor, 0, 26, true);
    const uint32_t b0 = p.ledBuilds();
    p.pianoLeds(lv, 10, 31);
    const uint32_t b1 = p.ledBuilds();
    for (int i = 0; i < 5; ++i) {
        uint8_t again[48] = {};
        p.pianoLeds(again, 10, 31);
        CHECK(std::memcmp(again, lv, sizeof(lv)) == 0);
        p.build(kMajor, 0, 26, true);   // same table: nothing rebuilt
    }
    CHECK_EQ(p.ledBuilds(), b1);
    CHECK(b1 - b0 <= 1);
    // A new pad brightness or a new table rebuilds them once.
    p.pianoLeds(lv, 3, 10);
    CHECK_EQ(lv[kLowD], 6);
    CHECK_EQ(p.ledBuilds(), b1 + 1);
    p.pianoLeds(lv, 3, 10);
    CHECK_EQ(p.ledBuilds(), b1 + 1);
    p.build(kChromatic, 0, 24, true);
    p.pianoLeds(lv, 3, 10);
    CHECK_EQ(p.ledBuilds(), b1 + 2);
    CHECK_EQ(lv[kLowC], 6);   // C3 root now: (3 + 10) / 2
    // In the app: passes of the main loop with nothing changed rebuild nothing.
    Harness h;
    h.app->cfg.padLayout = 1;
    h.run(40);
    const uint32_t a0 = h.app->padNotes.ledBuilds();
    h.app->ui.dirty();
    h.run(200);
    CHECK_EQ(h.app->padNotes.ledBuilds(), a0);
    CHECK(h.leds.level[kLowCs] > 0);
}

namespace {
int gated(App& a) {
    int n = 0;
    for (int t = 0; t < audio::kSynthAllTracks; ++t) {
        const audio::SynthVoice* v = a.engine.synth().voiceOfTrack(t);
        n += v && v->gate ? 1 : 0;
    }
    return n;
}
}  // namespace

TEST(piano_synth_page_pads_play_chords) {
    // The synth page after #129: held pads play a chord up to the slot's
    // polyphony; on the piano layout a gap plays nothing and the page
    // (outside Edit Patch) draws the keys.
    for (int m = 0; m < kSynthModelCount; ++m) {
        if (!audio::synthEngineFor(synthEngineId(SynthModel(m)))) continue;
        if (synthEngineId(SynthModel(m)) != kSynthEngineFat) continue;
        Harness h;
        App& a = *h.app;
        a.cfg.padLayout = 1;
        a.project.setU8(pf::kRootNote, 24);
        a.project.setU8(pf::kScale, kChromatic);
        a.setSynthModel(0, m);
        SynthSlotRecord r = a.synthSlot(0);
        r.polyphony = 4;
        a.setSynthSlotRecord(0, r);
        a.setCurrentInstrument(kFirstSynthInstrument);
        h.tap(btn::Pattern);
        h.tap(btn::InstrParams);
        CHECK_EQ(a.ui.active(), kModSynth);
        h.audio.pump(2);
        auto dn = [&](uint8_t p) { h.push(InputEvent::kPadDown, p); h.audio.pump(2); };
        auto upp = [&](uint8_t p) { h.push(InputEvent::kPadUp, p); h.audio.pump(2); };
        dn(kLowC);
        dn(kLowGap);   // a gap: no note
        dn(kLowCs);
        dn(kHighC);
        CHECK_EQ(gated(a), 3);
        h.run(40);
        CHECK(h.leds.level[kLowD] > 0);
        CHECK(h.leds.level[kLowCs] > 0);
        upp(kLowCs);
        CHECK_EQ(gated(a), 2);
        upp(kLowGap);
        upp(kLowC);
        upp(kHighC);
        CHECK_EQ(gated(a), 0);
    }
}
