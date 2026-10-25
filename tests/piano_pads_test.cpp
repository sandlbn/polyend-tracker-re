// [I] #42 Config > General > Pad layout "Piano" (not in stock): a semitone per
// column, black keys in the row above the white ones, two octaves from the
// root note's octave; gaps play nothing and the pad lights draw the keyboard.
#include "test.h"
#include "ui_fakes.h"
#include "nvstore_fake.h"

#include "app/project_fields.h"
#include "ui/notes.h"
#include "ui/screens.h"

using namespace ot;

namespace {
constexpr uint8_t kChromatic = 0, kMajor = 2;   // scale indices (kScaleNames)
// Physical pads (row 0 = top): the bottom row pair is the lower octave.
constexpr uint8_t kLowC = 36, kLowCs = 25, kLowD = 38, kHighC = 12, kHighCs = 1;
PatternScreen& pe(Harness& h) { return *static_cast<PatternScreen*>(h.app->ui.screen(kModPattern)); }
}  // namespace

TEST(piano_map_semitone_per_column) {
    PadNotes p;
    p.build(kChromatic, 0, 24, true);   // root C3: the bottom C is note 36
    CHECK(p.piano());
    CHECK_EQ(p.note(kLowC), 36);
    CHECK_EQ(p.note(kLowCs), 37);
    CHECK_EQ(p.note(kLowD), 38);
    CHECK_EQ(p.note(47), 47);       // bottom right: B
    CHECK_EQ(p.note(kHighC), 48);   // the upper pair: an octave higher
    CHECK_EQ(p.note(kHighCs), 49);
    // Gaps: a black key's column in the white row and the other way round.
    CHECK_EQ(p.note(37), PadNotes::kNoNote);
    CHECK_EQ(p.note(24), PadNotes::kNoNote);
    CHECK_EQ(p.note(0), PadNotes::kNoNote);
    CHECK_EQ(p.note(40), 40);   // E and F are both white
    CHECK_EQ(p.note(41), 41);
    CHECK_EQ(p.note(29), PadNotes::kNoNote);
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
    CHECK_EQ(lv[37], 0);       // gaps dark
    CHECK_EQ(lv[0], 0);
    // D major: D brightest, C and C# outside the scale barely lit, F# a
    // black key in the scale.
    std::memset(lv, 0, sizeof(lv));
    p.build(kMajor, 0, 26, true);
    p.pianoLeds(lv, 10, 31);
    CHECK_EQ(lv[kLowD], 20);
    CHECK_EQ(lv[kLowC], 2);
    CHECK_EQ(lv[kLowCs], 6);   // C# is in D major
    CHECK_EQ(lv[30], 6);       // F#
    CHECK_EQ(lv[41], 2);       // F
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
    CHECK_EQ(nv.mem[0xF4 + 4], 41);       // payload length
    CHECK_EQ(nv.mem[0xF4 + 5 + 40], 1);   // payload byte 41
    Config r;
    CHECK(r.load(nv));
    CHECK_EQ(r.padLayout, 1);
    c.padLayout = 2;
    c.sanitize();
    CHECK_EQ(c.padLayout, 0);
    // A record from before the byte (payload 40) keeps Stock.
    Config o;
    o.noteNames = 1;
    uint8_t ext[Config::kExtSize];
    o.toExt(ext);
    ext[4] = 40;
    uint8_t sum = 0;
    for (int i = 0; i < 40; ++i) sum = uint8_t(sum + ext[5 + i]);
    ext[5 + 40] = sum;
    Config k;
    k.padLayout = 1;
    CHECK(k.fromExt(ext, 5 + 40 + 1));
    CHECK_EQ(k.noteNames, 1);
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
    CHECK_EQ(h.leds.level[37], 0);
    // Edit mode: a gap enters nothing, a key enters its note.
    h.tap(btn::Rec);
    const int s0 = pe(h).step;
    h.pad(37);
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
