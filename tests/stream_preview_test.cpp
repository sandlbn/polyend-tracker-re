// Sample Loader previews around a streamed Add (device report: previews
// looked silent after an Add) and the stream ring of a voice that ended
// (telemetry kept "active 1" after the release finished).
#include "test.h"
#include "ui_fakes.h"

#include <cstring>
#include <vector>

#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;

namespace {
constexpr uint32_t kBank = 2200000;

std::vector<uint8_t> makeWav(uint32_t frames, uint16_t channels, uint32_t seed) {
    std::vector<int16_t> s(size_t(frames) * channels);
    uint32_t x = seed;
    for (auto& v : s) {
        x = x * 1664525u + 1013904223u;
        v = int16_t(int32_t(x >> 17) - 16384);
    }
    std::vector<uint8_t> w(s.size() * 2 + 64);
    w.resize(wavEncode(s.data(), frames, channels, w.data(), w.size()));
    return w;
}
LoaderScreen& ld(Harness& h) { return *static_cast<LoaderScreen*>(h.app->ui.screen(kModLoader)); }
int find(Harness& h, const char* want) {
    LoaderScreen& l = ld(h);
    char b[wg::kListItemCap];
    for (int i = 0; i < l.count; ++i) {
        l.entryLabel(i, b, sizeof(b));
        if (std::string(b) == want) return i;
    }
    return -1;
}
// Hold F3 for `ms`, rendering audio between loop passes; returns the peak.
int holdPreview(Harness& h, int ms) {
    h.down(btn::F3);
    int peak = 0;
    for (int t = 0; t < ms; t += 10) {
        int p = h.audio.pump(2);
        if (p > peak) peak = p;
        h.run(10);
    }
    h.up(btn::F3);
    for (int t = 0; t < 300; t += 10) {
        h.audio.pump(2);
        h.run(10);
    }
    return peak;
}
}  // namespace

TEST(previews_after_streamed_add_and_ring_released) {
    Harness h(true, kBank);
    App& a = *h.app;
    a.cfg.streamMode = 1;
    a.autosave();
    CHECK(a.loadWorkspace());
    CHECK(a.streamingOn());
    h.fs.mkdir("Samples");
    h.fs.files["Samples/long.wav"] = makeWav(1500000, 2, 3);
    h.fs.files["Samples/short.wav"] = makeWav(40000, 1, 7);
    h.tap(btn::SampleLoader);
    int i = find(h, "/Samples");
    CHECK(i >= 0);
    ld(h).sdCursor = i;
    h.tap(btn::F1);
    for (int k = 0; k < 50; ++k) h.run(20);
    int s = find(h, "short.wav"), L = find(h, "long.wav");
    CHECK(s >= 0 && L >= 0);
    ld(h).sdCursor = s;
    const int before = holdPreview(h, 500);
    ld(h).sdCursor = L;
    h.tap(btn::F1);   // Add -> streamed import
    for (int k = 0; k < 2000 && a.jobBusy(); ++k) h.run(20);
    for (int k = 0; k < 50; ++k) h.run(20);
    ld(h).sdCursor = find(h, "short.wav");
    const int after = holdPreview(h, 500);
    ld(h).sdCursor = find(h, "long.wav");
    const int afterLong = holdPreview(h, 500);
    h.tap(btn::Right);
    ld(h).insCursor = 0;
    const int inst = holdPreview(h, 1500);
    for (int t = 0; t < 5000; t += 10) {
        h.audio.pump(2);
        h.run(10);
    }
    CHECK_EQ(a.streamActive(), 0);   // the voice ended: its ring is released
    CHECK(before > 0);
    CHECK(after > 0);
    CHECK(afterLong > 0);
    CHECK(inst > 0);
}

TEST(import_of_a_file_too_long_for_memory_names_add) {
    // Device report: F5 Import (the trim view decodes the whole file into
    // sample memory) on a 5 min stereo WAV said only "Cannot read file".
    Harness h(true, kBank);
    App& a = *h.app;
    a.cfg.streamMode = 1;
    a.autosave();
    CHECK(a.loadWorkspace());
    h.fs.mkdir("Samples");
    h.fs.files["Samples/long.wav"] = makeWav(1500000, 2, 3);
    h.tap(btn::SampleLoader);
    ld(h).sdCursor = find(h, "/Samples");
    h.tap(btn::F1);
    for (int k = 0; k < 50; ++k) h.run(20);
    ld(h).sdCursor = find(h, "long.wav");
    h.tap(btn::F5);
    CHECK(std::strcmp(a.ui.notice(), "Too long for Import: use Add next (streams)") == 0);
    CHECK(!ld(h).trim);
    a.cfg.streamMode = 0;   // applies at New
    CHECK(a.newProject());
    CHECK(!a.streamingOn());
    h.tap(btn::SampleLoader);
    ld(h).sdCursor = find(h, "long.wav");
    CHECK(ld(h).sdCursor >= 0);
    h.tap(btn::F5);
    CHECK(std::strcmp(a.ui.notice(), "Too long for Import: not enough sample memory") == 0);
}
