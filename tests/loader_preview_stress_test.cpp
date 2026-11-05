// Sample Loader: Preview held while the SD Card list scrolls fast through
// many files (tester report: some previews turned into harsh noise and stayed
// so when tried again). Every file the cursor lands on must play its own
// samples: while the preview runs, the decoded part of the player's buffer
// equals a clean decode of that file (16 / 24 / 32-bit, float, 8-bit; mono /
// stereo; 22.05 / 44.1 / 48 kHz; files longer than the free memory play
// their first part), with sample streaming off and on.
#include "test.h"
#include "ui_fakes.h"

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;

namespace {

struct Spec {
    uint16_t bits, channels, format;   // format 1 PCM, 3 float
    uint32_t rate, frames;
};

void put16(std::vector<uint8_t>& w, uint32_t v) {
    w.push_back(uint8_t(v));
    w.push_back(uint8_t(v >> 8));
}
void put32(std::vector<uint8_t>& w, uint32_t v) {
    put16(w, v & 0xFFFF);
    put16(w, v >> 16);
}

// A tone with a little noise, different per file (seed).
std::vector<uint8_t> makeWav(const Spec& s, uint32_t seed) {
    const uint32_t bps = s.bits / 8u, align = bps * s.channels, data = s.frames * align;
    std::vector<uint8_t> w;
    w.reserve(44 + data + 1);
    w.insert(w.end(), {'R', 'I', 'F', 'F'});
    put32(w, 36 + data + (data & 1));
    w.insert(w.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(w, 16);
    put16(w, s.format);
    put16(w, s.channels);
    put32(w, s.rate);
    put32(w, s.rate * align);
    put16(w, align);
    put16(w, s.bits);
    w.insert(w.end(), {'d', 'a', 't', 'a'});
    put32(w, data);
    uint32_t x = seed * 2654435761u + 1;
    const double hz = 110.0 + 37.0 * double(seed % 23);
    for (uint32_t i = 0; i < s.frames; ++i)
        for (uint16_t c = 0; c < s.channels; ++c) {
            x = x * 1664525u + 1013904223u;
            const double v = 0.6 * std::sin(6.283185307 * hz * (c ? 1.5 : 1.0) * double(i) / double(s.rate)) +
                             0.05 * (double(int32_t(x >> 8) - (1 << 23)) / double(1 << 23));
            if (s.format == 3) {
                const float f = float(v);
                uint32_t b;
                std::memcpy(&b, &f, 4);
                put32(w, b);
            } else if (s.bits == 8) {
                w.push_back(uint8_t(int(std::lround(v * 127.0)) + 128));
            } else if (s.bits == 16) {
                put16(w, uint16_t(int16_t(std::lround(v * 32767.0))));
            } else if (s.bits == 24) {
                const int32_t q = int32_t(std::lround(v * 8388607.0));
                w.push_back(uint8_t(q));
                w.push_back(uint8_t(q >> 8));
                w.push_back(uint8_t(q >> 16));
            } else {
                put32(w, uint32_t(int32_t(std::lround(v * 2147483647.0))));
            }
        }
    if (data & 1) w.push_back(0);
    return w;
}

// The whole file decoded as the SD preview decodes it (stereo kept).
struct Ref {
    std::vector<int16_t> pcm;   // planar: L then R
    uint32_t frames = 0;
    bool stereo = false;
};
Ref reference(const std::vector<uint8_t>& f) {
    Ref r;
    WavInfo info;
    CHECK(wavParseHeader(f.data(), f.size(), info));
    WavImportOptions opt;
    opt.stereo = true;
    const WavImportPlan plan = wavPlanImport(info, opt);
    r.pcm.resize(size_t(plan.outFrames) * plan.outChannels + 16);
    WavImporter imp;
    CHECK(imp.begin(info, opt, r.pcm.data(), r.pcm.size()) == FmtStatus::Ok);
    imp.feed(f.data() + info.dataOffset, info.dataSize);
    imp.finish();
    r.frames = plan.outFrames;
    r.stereo = plan.outChannels == 2;
    return r;
}

LoaderScreen& ld(Harness& h) { return *static_cast<LoaderScreen*>(h.app->ui.screen(kModLoader)); }
std::string label(Harness& h, int i) {
    char b[wg::kListItemCap];
    ld(h).entryLabel(i, b, sizeof(b));
    return b;
}

// The player's decoded part against the file's clean decode; false (and
// what differs) when it plays something else.
bool playsItsFile(Harness& h, const Ref& r, const std::string& name, int move) {
    const audio::AudioEngine& e = h.app->engine;
    if (!e.previewBufferActive()) return true;
    const int16_t* L = e.previewBufferLeft();
    const int16_t* R = e.previewBufferRight();
    uint32_t n = e.previewBufferAvail();
    if (n > r.frames) n = r.frames;
    uint32_t bad = 0, first = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const bool diff = L[i] != r.pcm[i] || (r.stereo && R && R[i] != r.pcm[r.frames + i]);
        if (diff && !bad++) first = i;
    }
    if (bad)
        std::printf("       move %d %s: %u of %u decoded frames differ, first at %u (got %d, want %d)\n", move,
                    name.c_str(), bad, n, first, L[first], r.pcm[first]);
    return bad == 0;
}

void stress(int streamMode, uint32_t bankSamples) {
    Harness h(true, bankSamples);
    App& a = *h.app;
    if (streamMode) {
        a.cfg.streamMode = 1;
        a.autosave();
        CHECK(a.loadWorkspace());
    }
    const Spec kinds[] = {
        {16, 1, 1, 44100, 30000},  {16, 2, 1, 44100, 50000},  {24, 1, 1, 44100, 40000},  {24, 2, 1, 44100, 60000},
        {32, 2, 3, 44100, 30000},  {32, 1, 1, 48000, 30000},  {8, 1, 1, 22050, 20000},   {16, 2, 1, 48000, 70000},
        {24, 2, 1, 48000, 90000},  {16, 1, 1, 22050, 15000},  {24, 2, 1, 44100, 400000}, {16, 2, 1, 44100, 500000},
        {24, 1, 1, 96000, 50000},  {32, 2, 3, 48000, 300000}, {16, 1, 1, 44100, 1200},   {24, 2, 1, 44100, 7},
    };
    h.fs.mkdir("Samples");
    std::map<std::string, Ref> refs;
    int n = 0;
    for (int rep = 0; rep < 3; ++rep)
        for (const Spec& s : kinds) {
            char nm[48];
            std::snprintf(nm, sizeof nm, "s%02d_%dbit_%dch_%u.wav", n, s.bits, s.channels, unsigned(s.rate));
            const std::vector<uint8_t> f = makeWav(s, uint32_t(n + 1));
            refs[nm] = reference(f);
            h.fs.files[std::string("Samples/") + nm] = f;
            ++n;
        }
    h.tap(btn::SampleLoader);
    LoaderScreen& l = ld(h);
    for (int i = 0; i < l.count; ++i)
        if (label(h, i) == "/Samples") l.sdCursor = i;
    h.tap(btn::F1);
    for (int k = 0; k < 30; ++k) h.run(20);
    CHECK(l.count >= n);
    l.sdCursor = 0;
    h.down(btn::F3);   // Preview held from here on
    uint32_t x = 12345;
    int bad = 0, checked = 0;
    for (int move = 0; move < 260; ++move) {
        x = x * 1664525u + 1013904223u;
        // Mostly Down, some Up; fast taps, some waits (a file plays on).
        h.tap((x >> 28) < 12 ? btn::Down : btn::Up);
        const int wait = (x >> 20) % 7 == 0 ? 400 : int((x >> 12) % 4) * 10;
        for (int t = 0; t <= wait; t += 10) {
            h.audio.pump(2);
            h.run(10);
        }
        const std::string name = label(h, l.sdCursor);
        auto it = refs.find(name);
        if (it == refs.end()) continue;
        ++checked;
        if (!playsItsFile(h, it->second, name, move)) ++bad;
    }
    h.up(btn::F3);
    std::printf("  stream %d, bank %u: %d checks, %d played another sound\n", streamMode, unsigned(bankSamples), checked,
                bad);
    CHECK(checked > 100);
    CHECK_EQ(bad, 0);
}

}  // namespace

TEST(loader_preview_held_scroll_plays_each_file) { stress(0, 4 * 1024 * 1024); }
TEST(loader_preview_held_scroll_small_memory) { stress(0, 600000); }
TEST(loader_preview_held_scroll_streaming) { stress(1, 2200000); }
