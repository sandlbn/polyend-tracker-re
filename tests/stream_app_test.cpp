// SD streaming C5 (docs/PLAN_SD_STREAMING_P1C_P2.md §1.3-1.4): the App side.
// The rings are carved from the top of the sample bank only when streaming is
// on; the main-loop service touches the card only for active rings; streams
// are unbound when the computer takes the card; an export tops the rings up
// before each offline block. (C5 uses a test-only switch; C7 adds the option.)
#include "test.h"
#include "ui_fakes.h"

#include <vector>

using namespace ot;
using namespace ot::audio;

namespace {
// A streamable payload file and a source bound to it (instrument `ins`).
void bindStream(Harness& h, StreamSource& s, uint8_t ins, uint32_t frames) {
    std::vector<uint8_t> f(392 + frames * 2, 0);
    for (uint32_t i = 0; i < frames; ++i) f[392 + i * 2] = uint8_t(i);
    h.fs.files["Workspace/instruments/s.pti"] = f;
    static std::vector<int16_t> head(kStreamHeadFrames, 1);
    s = StreamSource{};
    s.instrument = ins;
    s.frames = frames;
    s.headL = head.data();
    s.headFrames = kStreamHeadFrames;
    CHECK(h.streamReader.bind(s, "Workspace/instruments/s.pti", 392, false, nullptr));
}
}  // namespace

TEST(rings_carved_only_when_on) {
    Harness h;
    App& a = *h.app;
    const uint32_t full = a.bank.capacityBytes();
    CHECK(!a.streamingOn());
    a.streamTestEnable = true;
    a.applyStreamOption();
    CHECK(a.streamingOn());
    CHECK_EQ(a.bank.capacityBytes(), full - kStreamRegionSamples * 2u);
    CHECK(a.engine.streamRings() == a.streamRings());
    a.streamTestEnable = false;
    a.applyStreamOption();
    CHECK(!a.streamingOn());
    CHECK_EQ(a.bank.capacityBytes(), full);
    CHECK(a.engine.streamRings() == nullptr);
    // A small bank (under twice the region) never carves.
    Harness small(true, 256 * 1024);
    small.app->streamTestEnable = true;
    small.app->applyStreamOption();
    CHECK(!small.app->streamingOn());
}

TEST(service_runs_only_with_active_rings) {
    Harness h;
    App& a = *h.app;
    a.streamTestEnable = true;
    a.applyStreamOption();
    StreamSource s;
    bindStream(h, s, 5, 200000);
    h.run(200);
    CHECK_EQ(h.streamReader.reads, 0u);   // nothing plays: no card access
    StreamRing& r = a.streamRings()[0];
    r.src = &s;
    r.need = 0;
    r.gen = 1;
    h.run(50);
    CHECK(h.streamReader.reads > 0);
    CHECK(r.avail > 0);
    CHECK(a.streamStats().chunks > 0);
}

TEST(streams_unbound_when_the_computer_takes_the_card) {
    Harness h;
    App& a = *h.app;
    a.streamTestEnable = true;
    a.applyStreamOption();
    StreamSource s;
    bindStream(h, s, 5, 200000);
    CHECK(h.streamReader.bound(5));
    a.streamUnbindAll();
    CHECK(!h.streamReader.bound(5));
}

TEST(export_tops_up_rings_before_each_block) {
    Harness h;
    App& a = *h.app;
    a.streamTestEnable = true;
    a.applyStreamOption();
    StreamSource s;
    bindStream(h, s, 5, 200000);
    StreamRing& r = a.streamRings()[0];
    r.src = &s;
    r.need = 0;
    r.gen = 1;
    const uint32_t before = h.streamReader.reads;
    h.fs.mkdir("Export");
    CHECK(a.exportAudio(App::ExportKind::Pattern, "Export/t", nullptr, false));
    CHECK(h.streamReader.reads > before);
}

// C11: the App hands out one telemetry window of the service stats and resets
// it; the underrun count stays the rings' running total.
TEST(stream_stats_window_resets) {
    Harness h;
    App& a = *h.app;
    a.streamTestEnable = true;
    a.applyStreamOption();
    audio::StreamServiceStats w = a.takeStreamStats();
    CHECK_EQ(w.chunks, 0u);
    CHECK_EQ(w.minLeadFrames, 0xFFFFFFFFu);
    CHECK_EQ(a.takeStreamStats().passes, 0u);
}
