// Instrument .pti codec tests (spec 02 §6).
#include "test.h"

#include <cmath>
#include <limits>
#include <vector>

#include "audio/voice.h"

#include "formats/bytes.h"
#include "formats/crc32.h"
#include "formats/instrument_pti.h"

using namespace ot;

static uint8_t g_hdr[pti::kHeaderBlockSize];

static Instrument makeCustom() {
    Instrument ins;
    std::strcpy(ins.name, "Snare 01");
    ins.sampleType = 1;
    ins.lengthFrames = 1234;
    ins.wavetableWindowSize = 512;
    ins.wavetableWindowCount = 7;
    ins.playMode = uint8_t(PlayMode::Slice);
    ins.startPoint = 100;
    ins.loopPoint1 = 200;
    ins.loopPoint2 = 300;
    ins.endPoint = 400;
    ins.wavetableCurrentWindow = 3;
    for (int i = 0; i < kAutomationCount; ++i) {
        ins.env[i].amount = 0.25f * float(i % 5);   // 0..1 (loads clamp to the editor range)
        ins.env[i].delayMs = uint16_t(10 + i);
        ins.env[i].attackMs = uint16_t(20 + i);
        ins.env[i].holdMs = uint16_t(30 + i);
        ins.env[i].decayMs = uint16_t(40 + i);
        ins.env[i].sustain = 0.5f;
        ins.env[i].releaseMs = uint16_t(50 + i);
        ins.env[i].loop = uint8_t(i & 1);
        ins.env[i].enabled = 1;
        ins.lfo[i].shape = uint8_t(i % 5);
        ins.lfo[i].speed = uint8_t(i * 4);
        ins.lfo[i].amount = 0.125f * float(i);
    }
    ins.cutoff = 0.75f;
    ins.resonance = 2.5f;
    ins.filterType = uint8_t(FilterType::BandPass);
    ins.filterEnabled = 1;
    ins.tune = -12;
    ins.finetune = -77;
    ins.volume = 88;
    ins.panning = 13;
    ins.delaySend = 44;
    for (int i = 0; i < kSliceCount; ++i) ins.slices[i] = uint16_t(i * 1000 + 1);
    ins.sliceCount = 48;
    ins.selectedSlice = 47;
    ins.grainLength = 44100;
    ins.granularPosition = 65535;
    ins.grainShape = 2;
    ins.granularLoop = 2;
    ins.reverbSend = 66;
    ins.overdrive = 99;
    ins.bitDepth = 4;
    return ins;
}

TEST(header_golden) {
    Instrument ins;
    encodePtiHeader(ins, g_hdr);
    CHECK_BYTES(g_hdr, 'T', 'I', 1, 0, 1, 9, 2, 1, 9, 9, 9, 9, 0x74, 0x01, 0, 0);
    CHECK_EQ(pti::kBodySize + 0x14, pti::kDataOff);
    CHECK_EQ(rdU32(g_hdr + 0x184), crc32(g_hdr, 0x184));
}

TEST(default_body_golden) {
    Instrument ins;
    encodePtiHeader(ins, g_hdr);
    const uint8_t* b = g_hdr + 0x10;
    CHECK_EQ(b[0x00], 1);
    CHECK_EQ(b[0x05], 0);
    CHECK_BYTES(b + 0x28, 0, 0, 0, 0);       // stale RAM pointer written 0
    CHECK_BYTES(b + 0x30, 0x00, 0x08);       // window size 2048
    CHECK_BYTES(b + 0x3E, 0, 0, 1, 0, 0xFE, 0xFF, 0xFF, 0xFF);  // start, loop1, loop2, end
    // env0: amount 1.0, attack 0, sustain 1.0, release 1000, enabled 1
    CHECK_BYTES(b + 0x4C, 0x00, 0x00, 0x80, 0x3F, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x00, 0x80, 0x3F, 0xE8, 0x03, 0, 1);
    // env1: attack 3000, disabled
    CHECK_BYTES(b + 0x4C + 20 + 6, 0xB8, 0x0B);
    CHECK_EQ(b[0x4C + 20 + 19], 0);
    // lfo0: triangle, speed 0, amount 0.5
    CHECK_BYTES(b + 0xC4, 2, 0, 0, 0, 0x00, 0x00, 0x00, 0x3F);
    CHECK_BYTES(b + 0xF4, 0x00, 0x00, 0x80, 0x3F);  // cutoff 1.0
    CHECK_EQ(b[0x100], 50);
    CHECK_BYTES(b + 0x104, 50, 0);
    CHECK_BYTES(b + 0x16A, 0xB9, 0x01);  // grain 441
    CHECK_EQ(b[0x172], 16);
}

TEST(custom_body_golden_offsets) {
    Instrument ins = makeCustom();
    encodePtiHeader(ins, g_hdr);
    // File offsets = struct offset + 0x10.
    CHECK_EQ(g_hdr[0x14], 1);  // sample type
    CHECK_BYTES(g_hdr + 0x15, 'S', 'n', 'a', 'r', 'e', ' ', '0', '1', 0);
    CHECK_EQ(rdU32(g_hdr + 0x3C), 1234u);  // length
    CHECK_EQ(rdU16(g_hdr + 0x40), 512u);
    CHECK_EQ(rdU32(g_hdr + 0x44), 7u);
    CHECK_EQ(g_hdr[0x4C], 4);  // play mode
    CHECK_EQ(rdU16(g_hdr + 0x4E), 100u);
    CHECK_EQ(rdU16(g_hdr + 0x54), 400u);
    CHECK_EQ(rdU32(g_hdr + 0x58), 3u);
    CHECK_EQ(rdU16(g_hdr + 0x5C + 5 * 20 + 16), 55u);  // env5 release
    CHECK_EQ(g_hdr[0xD4 + 3 * 8 + 1], 12);              // lfo3 speed
    CHECK_NEAR(rdF32(g_hdr + 0x108), 2.5, 0);           // resonance stored raw
    CHECK_EQ(g_hdr[0x10C], 2);
    CHECK_EQ(g_hdr[0x10D], 1);
    CHECK_EQ(int8_t(g_hdr[0x10E]), -12);
    CHECK_EQ(int8_t(g_hdr[0x10F]), -77);
    CHECK_EQ(g_hdr[0x110], 88);
    CHECK_EQ(rdI16(g_hdr + 0x114), 13);
    CHECK_EQ(g_hdr[0x116], 44);
    CHECK_EQ(rdU16(g_hdr + 0x118 + 47 * 2), 47001u);
    CHECK_EQ(g_hdr[0x178], 48);
    CHECK_EQ(g_hdr[0x179], 47);
    CHECK_EQ(rdU16(g_hdr + 0x17A), 44100u);
    CHECK_EQ(rdU16(g_hdr + 0x17C), 65535u);
    CHECK_EQ(g_hdr[0x17E], 2);
    CHECK_EQ(g_hdr[0x17F], 2);
    CHECK_EQ(g_hdr[0x180], 66);
    CHECK_EQ(g_hdr[0x181], 99);  // overdrive raw percent (not x100 like tracker-lib)
    CHECK_EQ(g_hdr[0x182], 4);
}

TEST(round_trip) {
    Instrument a = makeCustom();
    encodePtiHeader(a, g_hdr);
    Instrument b;
    PtiLoadInfo info;
    CHECK(decodePtiHeader(g_hdr, sizeof(g_hdr), b, &info) == FmtStatus::Ok);
    CHECK_EQ(info.bodySize, 0x174);
    CHECK_EQ(info.dataOffset, 0x188u);
    CHECK(!info.upgraded);
    uint8_t again[pti::kHeaderBlockSize];
    encodePtiHeader(b, again);
    CHECK(std::memcmp(g_hdr, again, sizeof(again)) == 0);
    CHECK_STR(b.name, "Snare 01");
    CHECK_EQ(b.finetune, -77);
    CHECK_NEAR(b.env[4].amount, 1.0, 0);
}

TEST(type_must_be_one) {
    Instrument a;
    encodePtiHeader(a, g_hdr);
    wrU16(g_hdr + 2, 0);
    Instrument b;
    CHECK(decodePtiHeader(g_hdr, sizeof(g_hdr), b) == FmtStatus::WrongType);
    CHECK(decodePtiHeader(g_hdr, 8, b) == FmtStatus::TooShort);
}

TEST(crc_not_checked_and_ram_pointer_ignored) {
    Instrument a = makeCustom();
    encodePtiHeader(a, g_hdr);
    wrU32(g_hdr + 0x184, 0x12345678);
    wrU32(g_hdr + 0x10 + 0x28, 0x8026A000);  // tracker-lib's constant
    Instrument b;
    CHECK(decodePtiHeader(g_hdr, sizeof(g_hdr), b) == FmtStatus::Ok);
    uint8_t again[pti::kHeaderBlockSize];
    encodePtiHeader(b, again);
    CHECK_BYTES(again + 0x38, 0, 0, 0, 0);
}

TEST(wav_suffix_stripped) {
    Instrument a;
    std::strcpy(a.name, "kick.WAV");
    encodePtiHeader(a, g_hdr);
    Instrument b;
    CHECK(decodePtiHeader(g_hdr, sizeof(g_hdr), b) == FmtStatus::Ok);
    CHECK_STR(b.name, "kick");
    // A full, unterminated name buffer (32 chars, no NUL): the field keeps 31.
    // (A longer strcpy here overflowed name[] into the next fields.)
    for (size_t i = 0; i < sizeof(a.name); ++i) a.name[i] = char('0' + i % 10);
    encodePtiHeader(a, g_hdr);
    CHECK_EQ(g_hdr[0x15 + 31], 0);
    decodePtiHeader(g_hdr, sizeof(g_hdr), b);
    CHECK_EQ(std::strlen(b.name), 31u);
}

TEST(short_body_reads_zero) {
    Instrument a = makeCustom();
    encodePtiHeader(a, g_hdr);
    Instrument b;
    CHECK(decodePtiHeader(g_hdr, 0x10 + 0x40, b) == FmtStatus::Ok);
    CHECK_EQ(b.lengthFrames, 1234u);
    CHECK_EQ(b.volume, 0);        // beyond the read bytes: 0 (spec 10 §0.1)
    CHECK_EQ(b.grainLength, 0);
}

static int g_hookFrom = -1;
static void ptiHook(uint8_t* body, uint32_t len, uint8_t from) {
    g_hookFrom = from;
    if (len == pti::kBodySize) body[0x100] = 42;
}

TEST(upgrade_hook) {
    Instrument a;
    encodePtiHeader(a, g_hdr);
    std::memset(g_hdr + 8, 8, 4);
    setPtiUpgradeHook(&ptiHook);
    Instrument b;
    PtiLoadInfo info;
    CHECK(decodePtiHeader(g_hdr, sizeof(g_hdr), b, &info) == FmtStatus::Ok);
    CHECK(info.upgraded);
    CHECK_EQ(g_hookFrom, 8);
    CHECK_EQ(b.volume, 42);
    setPtiUpgradeHook(nullptr);
}

TEST(sample_layout_rules) {
    PtiSampleLayout l = resolvePtiSample(100, 200, true);
    CHECK(l.frames == 100 && !l.stereoInFile && !l.useStereo && !l.headerDirty);
    l = resolvePtiSample(100, 400, true);
    CHECK(l.frames == 100 && l.stereoInFile && l.useStereo && !l.headerDirty);
    l = resolvePtiSample(100, 400, false);  // stereo setting off: left block only
    CHECK(l.frames == 100 && l.stereoInFile && !l.useStereo);
    l = resolvePtiSample(0, 400, true);     // length 0: repaired as mono bytes/2
    CHECK(l.frames == 200 && !l.stereoInFile && l.headerDirty);
    l = resolvePtiSample(100, 300, true);   // neither form
    CHECK(l.frames == 150 && l.headerDirty);
    l = resolvePtiSample(0, 0, true);
    CHECK(l.frames == 0 && l.headerDirty);
    l = resolvePtiSample(100, 401, true);   // odd byte count
    CHECK(l.frames == 200 && l.headerDirty);
}

TEST(full_file_mono_and_stereo) {
    const uint32_t n = 257;
    std::vector<int16_t> L(n), R(n);
    for (uint32_t i = 0; i < n; ++i) {
        L[i] = int16_t(i * 100 - 12000);
        R[i] = int16_t(-int(i) * 37);
    }
    Instrument a = makeCustom();
    a.lengthFrames = n;
    std::vector<uint8_t> file(ptiFileSize(n, true));
    CHECK_EQ(encodePti(a, L.data(), nullptr, file.data(), 10), size_t(0));
    // Stereo.
    size_t sz = encodePti(a, L.data(), R.data(), file.data(), file.size());
    CHECK_EQ(sz, size_t(0x188 + n * 4));
    CHECK_EQ(rdI16(file.data() + 0x188), L[0]);
    CHECK_EQ(rdI16(file.data() + 0x188 + 2 * n), R[0]);  // planar: R block after L block
    Instrument b;
    PtiLoadInfo info;
    CHECK(decodePtiHeader(file.data(), sz, b, &info) == FmtStatus::Ok);
    PtiSampleLayout lay = resolvePtiSample(b.lengthFrames, uint32_t(sz - info.dataOffset), true);
    std::vector<int16_t> out(2 * n);
    CHECK(decodePtiSamples(file.data(), sz, info, lay, b, out.data(), 2 * n - 1) == FmtStatus::NoSpace);
    CHECK(decodePtiSamples(file.data(), sz, info, lay, b, out.data(), out.size()) == FmtStatus::Ok);
    CHECK(b.stereo);
    CHECK(std::memcmp(out.data(), L.data(), n * 2) == 0);
    CHECK(std::memcmp(out.data() + n, R.data(), n * 2) == 0);
    // Mono.
    sz = encodePti(a, L.data(), nullptr, file.data(), file.size());
    CHECK_EQ(sz, size_t(0x188 + n * 2));
    CHECK(decodePtiHeader(file.data(), sz, b, &info) == FmtStatus::Ok);
    lay = resolvePtiSample(b.lengthFrames, uint32_t(sz - info.dataOffset), true);
    CHECK(!lay.stereoInFile);
    CHECK(decodePtiSamples(file.data(), sz, info, lay, b, out.data(), out.size()) == FmtStatus::Ok);
    CHECK(!b.stereo);
    CHECK(std::memcmp(out.data(), L.data(), n * 2) == 0);
}

// A damaged file (NaN / inf / out-of-range floats) loads with finite values
// in the editor ranges, and the voice renders finite audio through the filter.
TEST(damaged_floats_sanitised_on_load) {
    Instrument a = makeCustom();
    a.playMode = uint8_t(PlayMode::ForwardLoop);
    encodePtiHeader(a, g_hdr);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    uint8_t* body = g_hdr + 0x10;
    for (int i = 0; i < kAutomationCount; ++i) {
        wrF32(body + 0x4C + i * 20 + 0, i & 1 ? nan : -inf);   // env amount
        wrF32(body + 0x4C + i * 20 + 12, i & 1 ? 7.0f : nan);  // env sustain
        wrF32(body + 0xC4 + i * 8 + 4, i & 1 ? nan : -2.0f);   // lfo amount
    }
    wrF32(body + 0xF4, nan);   // cutoff
    wrF32(body + 0xF8, nan);   // resonance
    Instrument b;
    CHECK(decodePtiHeader(g_hdr, sizeof(g_hdr), b) == FmtStatus::Ok);
    for (int i = 0; i < kAutomationCount; ++i) {
        CHECK(std::isfinite(b.env[i].amount) && b.env[i].amount >= 0.0f && b.env[i].amount <= 1.0f);
        CHECK(std::isfinite(b.env[i].sustain) && b.env[i].sustain >= 0.0f && b.env[i].sustain <= 1.0f);
        CHECK(std::isfinite(b.lfo[i].amount) && b.lfo[i].amount >= 0.0f && b.lfo[i].amount <= 1.0f);
    }
    CHECK_EQ(b.env[1].amount, Envelope().amount);
    CHECK_EQ(b.env[0].sustain, Envelope().sustain);
    CHECK_EQ(b.env[1].sustain, 1.0f);
    CHECK_EQ(b.cutoff, Instrument().cutoff);
    CHECK_EQ(b.resonance, Instrument().resonance);
    wrF32(body + 0xF4, 3.0f);
    wrF32(body + 0xF8, inf);
    CHECK(decodePtiHeader(g_hdr, sizeof(g_hdr), b) == FmtStatus::Ok);
    CHECK_EQ(b.cutoff, 1.0f);
    CHECK_EQ(b.resonance, Instrument().resonance);
    wrF32(body + 0xF8, 9.0f);
    CHECK(decodePtiHeader(g_hdr, sizeof(g_hdr), b) == FmtStatus::Ok);
    CHECK_EQ(b.resonance, 4.3f);
    // Render through the (enabled, band-pass) filter: finite output.
    wrF32(body + 0xF4, nan);
    wrF32(body + 0xF8, nan);
    CHECK(decodePtiHeader(g_hdr, sizeof(g_hdr), b) == FmtStatus::Ok);
    audio::initTables();
    std::vector<int16_t> d(4410);
    for (size_t i = 0; i < d.size(); ++i) d[i] = int16_t(16000.0f * std::sin(6.2831853f * float(i) / 100.0f));
    b.lengthFrames = uint32_t(d.size());
    b.startPoint = 0;
    b.loopPoint1 = 1;
    b.loopPoint2 = 65534;
    b.endPoint = 65535;
    CHECK(b.filterEnabled == 1);
    audio::Voice v;
    v.noteOn(&b, d.data(), uint32_t(d.size()), 60, -1, 0, 0, 0, 0);
    audio::VoiceControl c;
    audio::VoiceOut o;
    float ds[audio::kBlock] = {}, rs[audio::kBlock] = {};
    bool finite = true;
    float peak = 0.0f;
    for (int blk = 0; blk < 50; ++blk) {
        v.control(c);
        v.render(c, o, ds, rs);
        for (uint32_t i = 0; i < audio::kBlock; ++i) {
            finite = finite && std::isfinite(o.amp[i]);
            peak = std::fmax(peak, std::fabs(o.amp[i]));
        }
    }
    CHECK(finite);
    CHECK(peak > 0.0f);
}
