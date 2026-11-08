// pattern_NN.mtp codec tests (spec 02 §4).
#include "test.h"

#include "formats/bytes.h"
#include "formats/crc32.h"
#include "formats/pattern_mtp.h"
#include "model/fx.h"
#include "model/instrument.h"
#include "mini_golden.h"

using namespace ot;

static uint8_t g_buf[mtp::kFileSize];
static uint8_t g_buf2[mtp::kFileSize];
static Pattern g_p, g_q;

static uint32_t stepOff(int t, int s) { return 0x1C + t * 0x301 + 1 + s * 6; }

static void fillRandom(Pattern& p, uint32_t seed) {
    for (int t = 0; t < kStockTrackCount; ++t) {
        p.tracks[t].lastStep = uint8_t((seed >> 3) % 128);
        for (int s = 0; s < kStepCount; ++s) {
            seed = seed * 1664525u + 1013904223u;
            Step& st = p.tracks[t].steps[s];
            st.note = int8_t(int(seed >> 24) % 132 - 4);
            st.instrument = uint8_t((seed >> 8) % 64);
            st.fx[0] = {uint8_t((seed >> 12) % kFxCount), uint8_t(seed >> 16)};
            st.fx[1] = {uint8_t((seed >> 4) % kFxCount), uint8_t(seed >> 20)};
        }
    }
}

TEST(sizes) {
    CHECK_EQ(mtp::kFileSize, 9260u);
    // Header 0x10 + reserved 12 = 0x1C bytes before the tracks. (spec 02 §4.1
    // states "32 + 769*T + 4", which does not add up; the stated totals 9260 /
    // 6184 and the 0x1C track offset imply 28.)
    CHECK_EQ(28u + 769u * 12u + 4u, mtp::kFileSize);
    CHECK_EQ(28u + 769u * 8u + 4u, mtp::kLegacyFileSize);
}

TEST(default_golden) {
    g_p.setDefault();
    encodePattern(g_p, g_buf);
    CHECK_BYTES(g_buf, 'K', 'S', 2, 0, 1, 9, 2, 1, 5, 5, 5, 5, 0x2C, 0x24, 0, 0);
    for (int i = 0x10; i < 0x1C; ++i) CHECK_EQ(g_buf[i], 0);
    for (int t = 0; t < kStockTrackCount; ++t) CHECK_EQ(g_buf[0x1C + t * 0x301], 31);
    CHECK_BYTES(g_buf + stepOff(0, 0), 0xFF, 0, 0, 0, 0, 0);
    CHECK_BYTES(g_buf + stepOff(11, 127), 0xFF, 0, 0, 0, 0, 0);
    CHECK_EQ(stepOff(11, 127) + 6, 0x2428u);
    CHECK_EQ(rdU32(g_buf + 0x2428), crc32(g_buf, 0x2428));
}

TEST(step_layout_fx_order) {
    g_p.setDefault();
    Step& s = g_p.tracks[2].steps[5];
    s.note = 60;
    s.instrument = 49;
    s.fx[0] = {uint8_t(FxId::Volume), 80};    // slot 0 -> bytes +4/+5
    s.fx[1] = {uint8_t(FxId::Panning), 25};   // slot 1 -> bytes +2/+3
    g_p.tracks[2].lastStep = 63;
    g_p.tracks[4].steps[0].note = kNoteOff;
    g_p.tracks[4].steps[1].note = kNoteCut;
    g_p.tracks[4].steps[2].note = kNoteFade;
    encodePattern(g_p, g_buf);
    CHECK_EQ(g_buf[0x1C + 2 * 0x301], 63);
    CHECK_BYTES(g_buf + stepOff(2, 5), 60, 49, 31, 25, 18, 80);
    CHECK_BYTES(g_buf + stepOff(4, 0), 0xFC, 0, 0, 0, 0, 0, 0xFD, 0, 0, 0, 0, 0, 0xFE);
}

TEST(round_trip_random) {
    for (uint32_t seed : {1u, 77u, 0xDEADBEEFu}) {
        fillRandom(g_p, seed);
        encodePattern(g_p, g_buf);
        PatternLoadInfo info;
        decodePattern(g_buf, sizeof(g_buf), g_q, nullptr, &info);
        CHECK(!info.upgraded);
        CHECK(!info.upperTracksReset);
        CHECK_EQ(info.structVersion, 5);
        CHECK_EQ(info.storedCrc, info.computedCrc);
        bool same = true;
        for (int t = 0; t < kStockTrackCount; ++t) {
            same = same && g_p.tracks[t].lastStep == g_q.tracks[t].lastStep;
            for (int s = 0; s < kStepCount; ++s) same = same && g_p.tracks[t].steps[s] == g_q.tracks[t].steps[s];
        }
        CHECK(same);
        encodePattern(g_q, g_buf2);
        CHECK(std::memcmp(g_buf, g_buf2, sizeof(g_buf)) == 0);
    }
}

TEST(ids_type_crc_not_checked) {
    fillRandom(g_p, 5);
    encodePattern(g_p, g_buf);
    g_buf[0] = 'P';
    g_buf[1] = 'M';
    wrU16(g_buf + 2, 0);
    wrU32(g_buf + 0x2428, 0);
    PatternLoadInfo info;
    decodePattern(g_buf, sizeof(g_buf), g_q, nullptr, &info);
    CHECK(!info.upperTracksReset);
    CHECK(g_q.tracks[11].steps[7] == g_p.tracks[11].steps[7]);
    CHECK(info.storedCrc != info.computedCrc);
}

TEST(size_field_wrong_resets_upper_tracks) {
    // tracker-lib writes size 0 (spec 02 §11): tracks 9..12 get wiped.
    fillRandom(g_p, 9);
    g_p.tracks[0].lastStep = 15;
    encodePattern(g_p, g_buf);
    wrU16(g_buf + 0x0C, 0);
    PatternLoadInfo info;
    decodePattern(g_buf, sizeof(g_buf), g_q, nullptr, &info);
    CHECK(info.upperTracksReset);
    CHECK(!info.upgraded);
    for (int t = 0; t < 8; ++t) CHECK(g_q.tracks[t].steps[3] == g_p.tracks[t].steps[3]);
    for (int t = 8; t < 12; ++t) {
        CHECK_EQ(g_q.tracks[t].lastStep, 15);
        for (int s = 0; s < kStepCount; ++s) {
            const Step& st = g_q.tracks[t].steps[s];
            CHECK(st.note == kNoteEmpty && st.instrument == 0 && st.fx[0].type == 0 && st.fx[1].type == 0);
            // Values untouched by the firmware clear routine, except one that
            // would read as a Z command.
            const uint8_t was = g_p.tracks[t].steps[s].fx[0].value;
            CHECK_EQ(st.fx[0].value, isPresetValue(was) ? 0 : was);
        }
    }
}

TEST(legacy_8_track_file) {
    // A 6184-byte 8-track file: structure 5 but size field 6184.
    fillRandom(g_p, 3);
    g_p.tracks[0].lastStep = 63;
    encodePattern(g_p, g_buf);
    wrU16(g_buf + 0x0C, uint16_t(mtp::kLegacyFileSize));
    PatternLoadInfo info;
    decodePattern(g_buf, mtp::kLegacyFileSize, g_q, nullptr, &info);
    CHECK(info.upperTracksReset);
    for (int t = 0; t < 8; ++t) CHECK(g_q.tracks[t].steps[100] == g_p.tracks[t].steps[100]);
    for (int t = 8; t < 12; ++t) {
        CHECK_EQ(g_q.tracks[t].lastStep, 63);
        CHECK_EQ(g_q.tracks[t].steps[0].note, kNoteEmpty);
    }
}

static uint8_t plusOne(uint8_t v) { return uint8_t(v + 1); }
static uint8_t timesTwo(uint8_t v) { return uint8_t(v * 2); }

TEST(upgrade_chain) {
    g_p.setDefault();
    // Track 0: beat-slice instrument 3 note, normal instrument 4 note, volume FX.
    g_p.tracks[0].steps[0] = Step{40, 3, {{18, 10}, {0, 0}}};
    g_p.tracks[0].steps[1] = Step{40, 4, {{0, 0}, {18, 20}}};
    g_p.tracks[0].steps[2] = Step{118, 3, {}};
    g_p.tracks[0].steps[3] = Step{kNoteOff, 3, {}};
    g_p.tracks[0].lastStep = 47;
    // Track 9 data must be wiped by v<5; FX 18 there must not be scanned.
    g_p.tracks[9].steps[0] = Step{50, 3, {{18, 10}, {0, 0}}};
    encodePattern(g_p, g_buf);
    std::memset(g_buf + 8, 1, 4);  // structure 1.1.1.1

    uint8_t modes[kSampleInstrumentCount] = {};
    modes[3] = uint8_t(PlayMode::BeatSlice);
    PatternUpgradeContext ctx;
    ctx.instrumentPlayModes = modes;
    ctx.beatSliceReference = 7;  // transpose by 12 - 7 = +5
    ctx.volumeCurveV2 = &plusOne;
    ctx.volumeCurveV4 = &timesTwo;
    PatternLoadInfo info;
    decodePattern(g_buf, sizeof(g_buf), g_q, &ctx, &info);
    CHECK(info.upgraded);
    CHECK(info.upperTracksReset);
    CHECK_EQ(info.structVersion, 1);
    CHECK_EQ(g_q.tracks[0].steps[0].note, 45);
    CHECK_EQ(g_q.tracks[0].steps[1].note, 40);
    CHECK_EQ(g_q.tracks[0].steps[2].note, 120);  // clamped
    CHECK_EQ(g_q.tracks[0].steps[3].note, kNoteOff);
    CHECK_EQ(g_q.tracks[0].steps[0].fx[0].value, (10 + 1) * 2);  // v2 then v4 curve
    CHECK_EQ(g_q.tracks[0].steps[1].fx[1].value, (20 + 1) * 2);
    CHECK_EQ(g_q.tracks[9].steps[0].note, kNoteEmpty);
    CHECK_EQ(g_q.tracks[9].lastStep, 47);

    // Structure 3: only the v<4 curve and the track reset apply.
    std::memset(g_buf + 8, 3, 4);
    decodePattern(g_buf, sizeof(g_buf), g_q, &ctx, &info);
    CHECK_EQ(g_q.tracks[0].steps[0].note, 40);
    CHECK_EQ(g_q.tracks[0].steps[0].fx[0].value, 20);

    // Structure 4 without a context: only the track reset.
    std::memset(g_buf + 8, 4, 4);
    decodePattern(g_buf, sizeof(g_buf), g_q, nullptr, &info);
    CHECK(info.upgraded);
    CHECK_EQ(g_q.tracks[0].steps[0].fx[0].value, 10);
    CHECK_EQ(g_q.tracks[9].steps[0].note, kNoteEmpty);
}

TEST(empty_file) {
    PatternLoadInfo info;
    decodePattern(nullptr, 0, g_q, nullptr, &info);
    CHECK(info.upgraded);  // structure byte reads 0
    CHECK_EQ(g_q.tracks[0].lastStep, 0);
    CHECK_EQ(g_q.tracks[0].steps[0].note, 0);
    CHECK_EQ(g_q.tracks[8].steps[0].note, kNoteEmpty);
}

// ---------------------------------------------------------------------------
// 16-track Tracker Mini layout (spec 21 §4): encodePattern16 / decodePattern16.

static uint8_t g_mbuf[mtp::kMiniFileSize];
static uint8_t g_mbuf2[mtp::kMiniFileSize];
static Pattern g_m;

static bool blankDefault(const Track& t, uint8_t last) { return t.lastStep == last && t.blank(); }

TEST(mini_sizes_tracker_lib) {
    // tracker-lib PatternConstants: HEADER 14 + PADDING 2 + UNUSED 12, track =
    // 1 + 6 * 128, CRC 4; TRACK_COUNT_MINI_PLUS 16.
    const uint32_t base = 14 + 2 + 12, track = 1 + 6 * 128;
    CHECK_EQ(base, mtp::kTracksOff);
    CHECK_EQ(track, mtp::kTrackSize);
    CHECK_EQ(base + track * 16 + 4, mtp::kMiniFileSize);
    CHECK_EQ(mtp::kMiniFileSize, 12336u);
    CHECK_EQ(base + track * 16, mtp::kMiniCrcOff);
    CHECK_EQ(base + track * 12 + 4, mtp::kStockFileSize);
    CHECK_EQ(base + track * 8 + 4, mtp::kLegacyFileSize);
    CHECK_EQ(kFirstSynthInstrument, 64);
    CHECK(!isSynthInstrument(63) && isSynthInstrument(64) && isSynthInstrument(66) && !isSynthInstrument(67));
    CHECK_EQ(synthSlotOf(65), 1);
    CHECK_EQ(synthSlotOf(48), -1);
}

TEST(mini_golden) {
    const std::vector<uint8_t> golden = minigold::loadHex("mini_pattern_golden.hex");
    CHECK_EQ(golden.size(), size_t(mtp::kMiniFileSize));
    if (golden.size() != mtp::kMiniFileSize) return;
    minigold::goldenPattern(g_m, 0);
    CHECK_EQ(encodePattern16(g_m, g_mbuf), size_t(mtp::kMiniFileSize));
    CHECK(std::memcmp(g_mbuf, golden.data(), mtp::kMiniFileSize) == 0);
    CHECK_BYTES(g_mbuf, 'K', 'S', 2, 0, 1, 9, 2, 1, 5, 5, 5, 5, 0x30, 0x30, 0, 0);
    CHECK_EQ(rdU32(g_mbuf + 0x302C), crc32(g_mbuf, 0x302C));
    // Synth instruments 64..66 are in the golden on tracks 0..15.
    int synth = 0;
    for (const Track& t : g_m.tracks)
        for (const Step& s : t.steps) synth += isSynthInstrument(s.instrument);
    CHECK(synth > 0);

    PatternLoadInfo info;
    decodePattern16(golden.data(), golden.size(), g_q, nullptr, &info);
    CHECK(!info.upgraded);
    CHECK(!info.upperTracksReset);
    CHECK_EQ(info.firstResetTrack, 16);
    CHECK_EQ(info.sizeField, 0x3030);
    CHECK_EQ(info.storedCrc, info.computedCrc);
    bool same = true;
    for (int t = 0; t < kTrackCount; ++t) same = same && minigold::sameTrack(g_q.tracks[t], g_m.tracks[t]);
    CHECK(same);
}

TEST(mini_split_encode_decode) {
    // PatternStock + ext pool tracks == the 16-track Pattern; a null ext
    // track encodes as the default (empty, track 0's length).
    minigold::goldenPattern(g_m, 4);
    static PatternStock s;
    g_m.copyStockTo(s);
    const Track* ext[kExtTrackCount] = {&g_m.tracks[12], nullptr, &g_m.tracks[14], nullptr};
    CHECK_EQ(encodePattern16(s, ext, g_mbuf), size_t(mtp::kMiniFileSize));
    g_m.tracks[13].clear(g_m.tracks[0].lastStep);
    g_m.tracks[15].clear(g_m.tracks[0].lastStep);
    for (int t : {13, 15})
        for (Step& st : g_m.tracks[t].steps) st = Step{};
    encodePattern16(g_m, g_mbuf2);
    CHECK(std::memcmp(g_mbuf, g_mbuf2, mtp::kMiniFileSize) == 0);
    static Track e[kExtTrackCount];
    static PatternStock s2;
    encodePattern16(s, nullptr, g_mbuf);   // all ext tracks default
    decodePattern16(g_mbuf, sizeof(g_mbuf), s2, e);
    for (const Track& t : e) CHECK(blankDefault(t, s.tracks[0].lastStep));
    decodePattern16(g_mbuf2, sizeof(g_mbuf2), s2, e);
    bool same = true;
    for (int t = 0; t < kStockTrackCount; ++t) same = same && minigold::sameTrack(s2.tracks[t], g_m.tracks[t]);
    for (int k = 0; k < kExtTrackCount; ++k) same = same && minigold::sameTrack(e[k], g_m.tracks[12 + k]);
    CHECK(same);
}

TEST(mini_stock12_layout) {
    // MtpLayout::Stock12 writes exactly what the 12-track encoder writes.
    for (uint32_t seed : {2u, 99u}) {
        fillRandom(g_p, seed);
        encodePattern(g_p, g_buf);
        CHECK_EQ(encodePattern16(g_p, g_mbuf, MtpLayout::Stock12), size_t(mtp::kStockFileSize));
        CHECK(std::memcmp(g_buf, g_mbuf, mtp::kStockFileSize) == 0);
    }
}

// Upgrade case 1 (spec 21 §4.2 step 3): a 12-track 0x242C 1.9.x file.
TEST(mini_upgrade_242c) {
    fillRandom(g_p, 21);
    encodePattern(g_p, g_buf);
    minigold::goldenPattern(g_q, 1);   // garbage in the target
    PatternLoadInfo info;
    decodePattern16(g_buf, sizeof(g_buf), g_q, nullptr, &info);
    CHECK(!info.upgraded);
    CHECK(info.upperTracksReset);
    CHECK_EQ(info.firstResetTrack, 12);
    bool same = true;
    for (int t = 0; t < kStockTrackCount; ++t) same = same && minigold::sameTrack(g_q.tracks[t], g_p.tracks[t]);
    CHECK(same);
    for (int t = 12; t < 16; ++t) CHECK(blankDefault(g_q.tracks[t], g_p.tracks[0].lastStep));
    // Re-encoding in the stock layout gives the original file back.
    encodePattern16(g_q, g_mbuf, MtpLayout::Stock12);
    CHECK(std::memcmp(g_buf, g_mbuf, mtp::kStockFileSize) == 0);
}

// Upgrade case 2: an 8-track legacy file (structure 5, size 6184) and
// tracker-lib's size 0: tracks 8..15 reset.
TEST(mini_upgrade_8track) {
    fillRandom(g_p, 3);
    g_p.tracks[0].lastStep = 63;
    for (uint16_t size : {uint16_t(mtp::kLegacyFileSize), uint16_t(0)}) {
        encodePattern(g_p, g_buf);
        wrU16(g_buf + 0x0C, size);
        minigold::goldenPattern(g_q, 2);
        PatternLoadInfo info;
        decodePattern16(g_buf, size ? mtp::kLegacyFileSize : sizeof(g_buf), g_q, nullptr, &info);
        CHECK(!info.upgraded);
        CHECK_EQ(info.firstResetTrack, 8);
        bool same = true;
        for (int t = 0; t < 8; ++t) same = same && minigold::sameTrack(g_q.tracks[t], g_p.tracks[t]);
        CHECK(same);
        for (int t = 8; t < 16; ++t) {
            CHECK_EQ(g_q.tracks[t].lastStep, 63);
            CHECK(g_q.tracks[t].blank());
        }
    }
}

// Upgrade case 3: structure < 5 clears tracks 8..15 even in a 0x3030 file,
// after the 1.9.2 conversions on tracks 0..7.
TEST(mini_upgrade_v_lt_5) {
    minigold::goldenPattern(g_m, 0);
    g_m.tracks[0].steps[0] = Step{40, 3, {{18, 10}, {0, 0}}};
    g_m.tracks[0].lastStep = 47;
    g_m.tracks[13].steps[0] = Step{50, 3, {{18, 10}, {0, 0}}};
    encodePattern16(g_m, g_mbuf);
    std::memset(g_mbuf + 8, 1, 4);
    PatternUpgradeContext ctx;
    ctx.volumeCurveV2 = &plusOne;
    ctx.volumeCurveV4 = &timesTwo;
    PatternLoadInfo info;
    decodePattern16(g_mbuf, sizeof(g_mbuf), g_q, &ctx, &info);
    CHECK(info.upgraded);
    CHECK_EQ(info.firstResetTrack, 8);
    CHECK_EQ(g_q.tracks[0].steps[0].fx[0].value, (10 + 1) * 2);
    for (int t = 8; t < 16; ++t) CHECK(blankDefault(g_q.tracks[t], 47));
    // Tracks 0..7 match the stock decoder on the same bytes.
    static Pattern ref;
    decodePattern(g_mbuf, sizeof(g_mbuf), ref, &ctx, nullptr);
    bool same = true;
    for (int t = 0; t < kStockTrackCount; ++t) same = same && minigold::sameTrack(g_q.tracks[t], ref.tracks[t]);
    CHECK(same);
}

TEST(mini_synth_steps_round_trip) {
    g_m.setDefault();
    for (int i = 0; i < 3; ++i) {
        g_m.tracks[12 + i].steps[i] = Step{int8_t(60 + i), uint8_t(kFirstSynthInstrument + i), {{20, 7}, {0, 0}}};
        g_m.tracks[i].steps[5] = Step{48, uint8_t(64 + i), {}};
    }
    encodePattern16(g_m, g_mbuf);
    CHECK_BYTES(g_mbuf + stepOff(14, 2), 62, 66, 0, 0, 20, 7);
    CHECK_BYTES(g_mbuf + stepOff(1, 5), 48, 65, 0, 0, 0, 0);
    decodePattern16(g_mbuf, sizeof(g_mbuf), g_q);
    CHECK(g_q.tracks[13].steps[1] == g_m.tracks[13].steps[1]);
    CHECK_EQ(g_q.tracks[2].steps[5].instrument, 66);
}

TEST(mini_empty_and_short_file) {
    PatternLoadInfo info;
    decodePattern16(nullptr, 0, g_q, nullptr, &info);
    CHECK(info.upgraded);
    CHECK_EQ(info.firstResetTrack, 8);
    CHECK_EQ(g_q.tracks[0].steps[0].note, 0);
    CHECK_EQ(g_q.tracks[15].steps[0].note, kNoteEmpty);
    // A 0x3030 file cut short: missing bytes read as zero, no reset.
    minigold::goldenPattern(g_m, 0);
    encodePattern16(g_m, g_mbuf);
    decodePattern16(g_mbuf, 0x2500, g_q, nullptr, &info);
    CHECK(!info.upperTracksReset);
    CHECK(minigold::sameTrack(g_q.tracks[11], g_m.tracks[11]));
    CHECK_EQ(g_q.tracks[15].lastStep, 0);
}
