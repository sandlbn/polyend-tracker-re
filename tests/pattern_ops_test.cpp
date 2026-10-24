// Pattern edit operations (spec 06 §1.6-§1.11) and the undo ring (spec 05 §5.1).
#include <vector>

#include "test.h"

#include "app/pattern_ops.h"
#include "app/undo.h"
#include "sequencer/seq_tables.h"

using namespace ot;

static SelRect rect(int s0, int s1, int t0, int t1) {
    SelRect r;
    r.s0 = s0;
    r.s1 = s1;
    r.t0 = t0;
    r.t1 = t1;
    return r;
}

TEST(insert_and_backspace) {
    Pattern p;
    p.setDefault();
    for (int s = 0; s < 4; ++s) p.tracks[0].steps[s].note = int8_t(40 + s);
    pops::insertRows(p, rect(1, 1, 0, 0));
    CHECK(p.tracks[0].steps[0].note == 40);
    CHECK(p.tracks[0].steps[1].note == kNoteEmpty);
    CHECK(p.tracks[0].steps[2].note == 41);
    CHECK(p.tracks[0].steps[4].note == 43);
    CHECK(pops::backspaceRows(p, rect(2, 2, 0, 0)));
    CHECK(p.tracks[0].steps[1].note == 41);
    CHECK(!pops::backspaceRows(p, rect(0, 0, 0, 0)));
}

TEST(clear_modes) {
    Pattern p;
    p.setDefault();
    Step& s = p.tracks[2].steps[3];
    s.note = 60;
    s.instrument = 5;
    s.fx[0] = {18, 50};
    s.fx[1] = {31, 20};
    pops::clearRect(p, rect(3, 3, 2, 2), ColMode::Fx1);
    CHECK(s.fx[0].type == 0 && s.fx[0].value == 50 && s.fx[1].type == 31);
    pops::clearRect(p, rect(3, 3, 2, 2), ColMode::Note);
    CHECK(s.note == kNoteEmpty && s.instrument == 0 && s.fx[1].type == 0);
}

TEST(write_note_rules) {
    Pattern p;
    p.setDefault();
    pops::writeNote(p, rect(0, 0, 0, 0), 64, 7);
    CHECK(p.tracks[0].steps[0].note == 64 && p.tracks[0].steps[0].instrument == 7);
    pops::writeNote(p, rect(0, 0, 9, 9), 64, 7);   // MIDI track maps instrument to 48..63
    CHECK(p.tracks[9].steps[0].instrument == 48);
    // Rectangle: only cells holding a note change.
    pops::writeNote(p, rect(0, 3, 0, 0), 50, 7);
    CHECK(p.tracks[0].steps[0].note == 50 && p.tracks[0].steps[1].note == kNoteEmpty);
    // Encoder on an empty cell: down gives fade/cut/off.
    CHECK(pops::stepNote(p, rect(5, 5, 0, 0), -1, 0) == kNoteFade);
    CHECK(pops::stepNote(p, rect(6, 6, 0, 0), 1, 0) == 60);
}

TEST(duplicate_expand_shrink_limits) {
    Pattern p;
    p.setDefault();
    pops::setLength(p, 128);
    CHECK(!pops::duplicate(p));
    CHECK(!pops::expand(p));
    pops::setLength(p, 1);
    CHECK(!pops::shrink(p));
    pops::setLength(p, 3);
    p.tracks[0].steps[2].note = 10;
    CHECK(pops::shrink(p));
    CHECK(pops::length(p) == 2);
    CHECK(p.tracks[0].steps[1].note == 10);
}

TEST(invert_and_paste) {
    Pattern p, clip;
    p.setDefault();
    for (int s = 0; s < 4; ++s) p.tracks[0].steps[s].note = int8_t(s);
    pops::invert(p, rect(0, 3, 0, 0), ColMode::All);
    CHECK(p.tracks[0].steps[0].note == 3 && p.tracks[0].steps[3].note == 0);
    clip = p;
    pops::paste(p, clip, rect(0, 1, 0, 0), 30, 11, ColMode::All, 0);
    CHECK(p.tracks[11].steps[30].note == kNoteEmpty);   // sample instrument dropped on MIDI track
    pops::paste(p, clip, rect(0, 3, 0, 0), 30, 1, ColMode::All, 0);
    CHECK(p.tracks[1].steps[30].note == 3 && p.tracks[1].steps[31].note == 2);
    CHECK(p.tracks[1].steps[32].note == kNoteEmpty);   // beyond length 32 dropped
}

static uint32_t lcg(void* c) {
    auto* s = static_cast<uint32_t*>(c);
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

TEST(fill_every_n_and_from_to) {
    Pattern p;
    p.setDefault();
    pops::FillParams f;
    f.where = 5;
    f.param[5] = 4;
    f.type = 1;
    f.from = 48;
    f.to = 60;
    f.scaleOrFx = 0;
    uint32_t seed = 1;
    pops::fill(p, rect(0, 15, 0, 0), 0, f, 0, 3, lcg, &seed);
    CHECK(p.tracks[0].steps[0].note == 48);
    CHECK(p.tracks[0].steps[1].note == kNoteEmpty);
    CHECK(p.tracks[0].steps[4].note == 48 + 12 * 4 / 15);
    CHECK(p.tracks[0].steps[0].instrument == 3);
    // Euclidean: 4 events over 16 rows.
    Pattern q;
    q.setDefault();
    f.where = 6;
    f.param[6] = 4;
    f.type = 0;
    pops::fill(q, rect(0, 15, 1, 1), 0, f, 0, 0, lcg, &seed);
    int n = 0;
    for (int s = 0; s < 16; ++s) n += q.tracks[1].steps[s].note >= 0;
    CHECK(n == 4);
}

TEST(undo_ring_depth) {
    std::vector<Pattern> slots(20);
    uint8_t nums[20];
    UndoRing u;
    u.attach(slots.data(), nums, 20);
    Pattern cur;
    cur.setDefault();
    for (int i = 0; i < 25; ++i) {
        u.push(cur, 1);
        cur.tracks[0].steps[0].note = int8_t(i);
    }
    CHECK(u.depth() == 19);
    int undone = 0;
    uint8_t num;
    while (u.undo(cur, 1, &num)) ++undone;
    CHECK(undone == 19);
    CHECK(cur.tracks[0].steps[0].note == 5);
    int redone = 0;
    while (u.redo(cur, 1, &num)) ++redone;
    CHECK(redone == 19);
    CHECK(cur.tracks[0].steps[0].note == 24);
}

// 2.x (2x-01 §2.5): paste reaches tracks 13..16 and clamps at track 16;
// a sample cell pasted onto a MIDI track (9..16) drops its note.
TEST(paste_over_16_tracks_clamps) {
    std::vector<Pattern> buf(2);
    Pattern& p = buf[0];
    Pattern& clip = buf[1];
    p.setDefault();
    clip.setDefault();
    for (int t = 0; t < 4; ++t) {
        clip.tracks[t].steps[0].note = int8_t(50 + t);
        clip.tracks[t].steps[0].instrument = uint8_t(t < 2 ? kFirstMidiInstrument + t : 3);
    }
    pops::paste(p, clip, rect(0, 0, 0, 3), 0, 13, ColMode::All, 0);
    CHECK_EQ(int(p.tracks[13].steps[0].note), 50);
    CHECK_EQ(int(p.tracks[14].steps[0].note), 51);
    CHECK(p.tracks[15].steps[0].note == kNoteEmpty);   // sample instrument dropped on a MIDI track
    CHECK(p.tracks[12].steps[0].note == kNoteEmpty);
    // Clear / invert / insert accept rectangles up to 16 tracks wide.
    pops::clearRect(p, rect(0, 0, 0, kTrackCount - 1), ColMode::All);
    CHECK(p.tracks[14].steps[0].note == kNoteEmpty);
    p.tracks[15].steps[0].note = 70;
    pops::insertRows(p, rect(0, 0, 0, kTrackCount - 1));
    CHECK_EQ(int(p.tracks[15].steps[1].note), 70);
    pops::setLength(p, 16);
    CHECK_EQ(int(p.tracks[15].lastStep), 15);
}

TEST(write_instrument_selection_moves_off_steps_too) {
    // 2.x (user request): changing the instrument of a selection also moves
    // its OFF / CUT / FADE steps; empty steps stay empty.
    Pattern p;
    Track& t = p.tracks[0];
    t.steps[0].note = 60;
    t.steps[0].instrument = 1;
    t.steps[1].note = kNoteOff;
    t.steps[1].instrument = 1;
    t.steps[2].note = kNoteCut;
    t.steps[2].instrument = 1;
    pops::writeInstrument(p, rect(0, 3, 0, 0), 5);
    CHECK_EQ(int(t.steps[0].instrument), 5);
    CHECK_EQ(int(t.steps[1].instrument), 5);
    CHECK_EQ(int(t.steps[2].instrument), 5);
    CHECK(t.steps[3].note == kNoteEmpty);
}

TEST(new_fx_type_value_inside_its_range) {
    // MANUAL_AUDIT_2026-10-23 #9 (manual p.176 Bit Depth "4-16 Bits"):
    // choosing an Fx type writes a value inside that Fx's range (Bit Depth
    // stored 0, shown "E  0").
    Pattern p;
    const SelRect r = rect(0, 0, 0, 0);
    for (int i = 1; i < kFxCount; ++i) {
        const uint8_t t = kFxMenuOrder[i];
        pops::writeFxType(p, r, 2, t);
        const uint8_t v = p.tracks[0].steps[0].fx[fxSlotForColumn(2)].value;
        CHECK(v >= seq::fxMin(t) && v <= seq::fxMax(t));
    }
    pops::writeFxType(p, r, 2, uint8_t(FxId::BitDepth));
    CHECK_EQ(int(p.tracks[0].steps[0].fx[fxSlotForColumn(2)].value), 4);
}
