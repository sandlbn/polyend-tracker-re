// SD streaming C7 (docs/PLAN_SD_STREAMING_P1C_P2.md §2.1, §2.6): which
// instruments stream (pure rules), what stays resident, the projected memory,
// and the Config fields in the OpenTracker extension record v2.
#include "test.h"
#include "ui_fakes.h"

#include "app/config.h"
#include "app/stream_policy.h"

using namespace ot;

namespace {
StreamDecisionIn in(StreamMode m, uint8_t play, uint32_t frames, bool over = false) {
    StreamDecisionIn d;
    d.mode = m;
    d.playMode = play;
    d.frames = frames;
    d.thresholdSec = 10;
    d.overBudget = over;
    d.readerAvailable = true;
    d.streamsInUse = 0;
    return d;
}
}  // namespace

TEST(decision_table) {
    const uint8_t one = uint8_t(PlayMode::OneShot), loop = uint8_t(PlayMode::ForwardLoop),
                  slice = uint8_t(PlayMode::Slice), gran = uint8_t(PlayMode::Granular);
    const uint32_t s = 44100;
    CHECK(!streamDecision(in(StreamMode::Off, one, 60 * s)));
    CHECK(streamDecision(in(StreamMode::All, one, 60 * s)));
    CHECK(streamDecision(in(StreamMode::All, loop, 60 * s)));
    CHECK(!streamDecision(in(StreamMode::All, slice, 60 * s)));   // needs memory
    CHECK(!streamDecision(in(StreamMode::All, gran, 60 * s)));
    CHECK(!streamDecision(in(StreamMode::All, one, 65536)));      // too short to gain anything
    CHECK(streamDecision(in(StreamMode::All, one, 65537)));
    CHECK(!streamDecision(in(StreamMode::Auto, one, 9 * s)));     // under the 10 s threshold
    CHECK(streamDecision(in(StreamMode::Auto, one, 10 * s)));
    CHECK(streamDecision(in(StreamMode::Auto, one, 3 * s, true)));   // the project does not fit
    CHECK(!streamDecision(in(StreamMode::Auto, one, s, true)));      // ... but not under 2 s
    StreamDecisionIn d = in(StreamMode::All, one, 60 * s);
    d.readerAvailable = false;
    CHECK(!streamDecision(d));
    d = in(StreamMode::All, one, 60 * s);
    d.streamsInUse = audio::kMaxStreams;
    CHECK(!streamDecision(d));
}

namespace {
ImportDecisionIn imp(StreamMode m, uint8_t play, uint32_t frames, bool fits) {
    ImportDecisionIn d;
    d.mode = m;
    d.playMode = play;
    d.frames = frames;
    d.thresholdSec = 10;
    d.fitsWhole = fits;
    d.readerAvailable = true;
    d.streamsInUse = 0;
    return d;
}
}  // namespace

// The Sample Loader import (sample_import.cpp) follows the open rule above,
// with "the project does not fit" read as "this sample does not fit".
TEST(import_decision_follows_the_open_rule_for_streamable_modes) {
    const uint8_t one = uint8_t(PlayMode::OneShot), loop = uint8_t(PlayMode::ForwardLoop);
    const uint32_t s = 44100;
    ImportDecision d = importDecision(imp(StreamMode::Auto, one, 12 * s, true));
    CHECK(d.streams);
    CHECK(d.why == StreamWhy::Threshold);
    CHECK(!d.forcedOneShot);
    CHECK(!importDecision(imp(StreamMode::Auto, one, 9 * s, true)).streams);   // under 10 s, fits
    ImportDecisionIn t = imp(StreamMode::Auto, loop, 12 * s, true);
    t.thresholdSec = 20;
    CHECK(!importDecision(t).streams);
    t.thresholdSec = 5;
    CHECK(importDecision(t).why == StreamWhy::Threshold);
    d = importDecision(imp(StreamMode::All, loop, 65537, true));
    CHECK(d.streams);
    CHECK(d.why == StreamWhy::All);
    CHECK(!importDecision(imp(StreamMode::All, one, 65536, true)).streams);
    CHECK(!importDecision(imp(StreamMode::Off, one, 60 * s, true)).streams);
    ImportDecisionIn r = imp(StreamMode::All, one, 60 * s, true);
    r.readerAvailable = false;
    CHECK(!importDecision(r).streams);
    // 16 already stream: one that fits loads into memory, as at an open.
    r = imp(StreamMode::All, one, 60 * s, true);
    r.streamsInUse = audio::kMaxStreams;
    CHECK(!importDecision(r).streams);
    // Does not fit, over 2 s: streams for that reason.
    d = importDecision(imp(StreamMode::Auto, one, 3 * s, false));
    CHECK(d.streams);
    CHECK(d.why == StreamWhy::NoFit);
}

TEST(import_decision_keeps_non_streamable_modes_that_fit) {
    const uint32_t s = 44100;
    const uint8_t modes[] = {uint8_t(PlayMode::Slice), uint8_t(PlayMode::Granular), uint8_t(PlayMode::Wavetable),
                             uint8_t(PlayMode::BackwardLoop), uint8_t(PlayMode::PingPongLoop)};
    for (uint8_t m : modes) {
        CHECK(!importDecision(imp(StreamMode::Auto, m, 30 * s, true)).streams);
        CHECK(!importDecision(imp(StreamMode::All, m, 30 * s, true)).streams);
    }
}

TEST(import_decision_no_fit_fallback_forces_oneshot_without_a_length_floor) {
    const uint32_t s = 44100;
    // A non-streamable mode that does not fit: streams as 1-Shot.
    ImportDecision d = importDecision(imp(StreamMode::Auto, uint8_t(PlayMode::Slice), 30 * s, false));
    CHECK(d.streams);
    CHECK(d.why == StreamWhy::NoFit);
    CHECK(d.forcedOneShot);
    // Short (under 2 s, under 65,536 frames) and still too big: streams.
    d = importDecision(imp(StreamMode::Auto, uint8_t(PlayMode::OneShot), 30000, false));
    CHECK(d.streams);
    CHECK(d.why == StreamWhy::NoFit);
    CHECK(!d.forcedOneShot);
    d = importDecision(imp(StreamMode::All, uint8_t(PlayMode::Granular), 30000, false));
    CHECK(d.streams && d.forcedOneShot);
    // Off or no reader: refused (the caller says why).
    CHECK(!importDecision(imp(StreamMode::Off, uint8_t(PlayMode::OneShot), 30 * s, false)).streams);
}

TEST(resident_layout_head_and_loop_head) {
    const StreamResident a = streamResident(uint8_t(PlayMode::OneShot), 500000, 30000);
    CHECK_EQ(a.headFrames, audio::kStreamHeadFrames);
    CHECK_EQ(a.loopFrames, 0u);
    CHECK_EQ(a.framesPerChannel(), audio::kStreamHeadFrames);
    // Forward loop starting past the head: a loop head is kept too.
    const StreamResident b = streamResident(uint8_t(PlayMode::ForwardLoop), 500000, 32768);
    CHECK_EQ(b.loopStart, uint32_t(uint64_t(500000) * 32768 / 65535));
    CHECK_EQ(b.loopFrames, audio::kStreamHeadFrames);
    CHECK_EQ(b.framesPerChannel(), 2 * audio::kStreamHeadFrames);
    // Loop starting inside the head: the head covers it.
    const StreamResident c = streamResident(uint8_t(PlayMode::ForwardLoop), 500000, 100);
    CHECK_EQ(c.loopFrames, 0u);
}

TEST(projected_bytes) {
    // Two long mono payloads and one short: Off counts everything, All only
    // the resident heads of the long ones.
    const uint32_t pay[3] = {20000000, 4000000, 100000};
    const bool has[3] = {true, true, true};
    CHECK_EQ(streamProjectedBytes(pay, has, 3, StreamMode::Off, 10), uint64_t(24100000));
    const uint64_t head = uint64_t(2) * 2 * audio::kStreamHeadFrames * 2;   // max resident: stereo head + loop head
    CHECK_EQ(streamProjectedBytes(pay, has, 3, StreamMode::All, 10), 2 * head + 100000);
}

TEST(config_ext_v2_round_trip_nv_and_file) {
    struct Nv : INvStore {
        uint8_t mem[512] = {};
        uint32_t size() const override { return sizeof mem; }
        bool read(uint32_t off, void* dst, uint32_t n) override {
            if (off + n > sizeof mem) return false;
            std::memcpy(dst, mem + off, n);
            return true;
        }
        bool write(uint32_t off, const void* src, uint32_t n) override {
            if (off + n > sizeof mem) return false;
            std::memcpy(mem + off, src, n);
            return true;
        }
    } nv;
    Config a;
    a.streamMode = 1;
    a.streamThreshold = 3;
    a.cpuMeter = 2;
    CHECK(a.save(nv));
    Config b;
    CHECK(b.load(nv));
    CHECK_EQ(b.streamMode, 1);
    CHECK_EQ(b.streamThreshold, 3);
    CHECK_EQ(b.cpuMeter, 2);
    FakeStorage fs;
    CHECK(a.save(fs));
    Config c;
    CHECK(c.load(fs));
    CHECK_EQ(c.streamMode, 1);
    CHECK_EQ(c.streamThreshold, 3);
}

TEST(config_ext_v1_record_still_loads_with_stream_defaults) {
    // A v1 record (2 payload bytes, an older 2.x build): CPU meter and synth
    // voices come back, the stream fields stay at their defaults.
    uint8_t v1[8] = {'O', 'T', 'X', 1, 2, 1, 6, 7};
    Config c;
    c.streamMode = 2;
    c.streamThreshold = 0;
    CHECK(c.fromExt(v1, sizeof v1));
    CHECK_EQ(c.cpuMeter, 1);
    CHECK_EQ(c.synthVoices, 6);
    CHECK_EQ(c.streamMode, 0);
    CHECK_EQ(c.streamThreshold, 1);
}
