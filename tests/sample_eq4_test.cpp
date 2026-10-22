// Sample Editor EQ4 (OpenTracker extension): the offline 4-band EQ effect
// (sample_fx kEq4) and its two pages (bands, band parameters).
#include <cmath>
#include <cstdio>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "app/sample_fx.h"
#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;
using namespace ot::audio;

namespace {
EditorScreen& ed(Harness& h) { return *static_cast<EditorScreen*>(h.app->ui.screen(kModEditor)); }

std::vector<int16_t> sine(uint32_t n, double hz, double amp) {
    std::vector<int16_t> s(n);
    for (uint32_t i = 0; i < n; ++i) s[i] = int16_t(amp * std::sin(2.0 * M_PI * hz * double(i) / 44117.647));
    return s;
}
// RMS of the second half (past the filter's settling).
double rmsTail(const std::vector<int16_t>& v) {
    double a = 0;
    const size_t s = v.size() / 2;
    for (size_t i = s; i < v.size(); ++i) a += double(v[i]) * double(v[i]);
    return std::sqrt(a / double(v.size() - s));
}
double gainDb(const TrackEqParams& t, double hz) {
    float p[sfx::kMaxParams];
    sfx::eq4Pack(t, p);
    const auto in = sine(44100, hz, 6000);
    std::vector<int16_t> out(in.size());
    sfx::process(sfx::kEq4, p, in.data(), uint32_t(in.size()), out.data(), uint32_t(out.size()));
    return 20.0 * std::log10(rmsTail(out) / rmsTail(in));
}
TrackEqParams factory() {
    TrackEqParams t;
    for (int i = 0; i < kEqBands; ++i) t.band[i] = eqDefaultBand(i);
    return t;
}
}  // namespace

TEST(eq4_table_defaults_and_pack) {
    CHECK_STR(sfx::effect(sfx::kEq4).name, "EQ4");
    CHECK_STR(sfx::effect(sfx::kEq4 - 1).name, "Equalizer");   // right after Equalizer
    CHECK_EQ(sfx::effect(sfx::kEq4).params, 24);
    float d[sfx::kMaxParams], p[sfx::kMaxParams];
    sfx::defaults(sfx::kEq4, d);
    sfx::eq4Pack(factory(), p);
    for (int i = 0; i < 24; ++i) CHECK_NEAR(d[i], p[i], 1e-5);
    // Round trip, and a slope a band cannot have is dropped.
    TrackEqParams t = factory(), u;
    t.band[0].on = true;
    t.band[0].slope = 1;
    t.band[1].type = kEqLowCut;
    t.band[1].slope = 1;   // band 2 has no 48 dB option
    t.band[2].gainDb10 = -73;
    t.band[2].q100 = 333;
    sfx::eq4Pack(t, p);
    sfx::eq4Unpack(p, &u);
    CHECK(u.band[0] == t.band[0]);
    CHECK_EQ(u.band[1].slope, 0);
    CHECK_EQ(u.band[2].gainDb10, -73);
    CHECK_EQ(u.band[2].q100, 333);
    char v[24];
    sfx::formatParam(sfx::kEq4, 1, float(kEqHighCut), v, sizeof(v));
    CHECK_STR(v, "High Cut");
    sfx::formatParam(sfx::kEq4, 4, 1.0f, v, sizeof(v));
    CHECK_STR(v, "48 dB");
}

TEST(eq4_dsp_neutral_and_response) {
    // Nothing on, or only neutral bands: untouched.
    TrackEqParams t = factory();
    float p[sfx::kMaxParams];
    const auto in = sine(5000, 1000, 9000);
    std::vector<int16_t> out(in.size());
    sfx::eq4Pack(t, p);
    sfx::process(sfx::kEq4, p, in.data(), 5000, out.data(), 5000);
    CHECK(out == in);
    t.band[1].on = true;   // bell with 0 dB
    sfx::eq4Pack(t, p);
    sfx::process(sfx::kEq4, p, in.data(), 5000, out.data(), 5000);
    CHECK(out == in);
    CHECK_EQ(sfx::outputFrames(sfx::kEq4, p, 5000), 5000u);

    // Bell +12 dB at 1 kHz.
    t = factory();
    t.band[1].on = true;
    t.band[1].freqHz = 1000;
    t.band[1].gainDb10 = 120;
    CHECK_NEAR(gainDb(t, 1000), 12.0, 0.3);
    CHECK(std::fabs(gainDb(t, 60)) < 0.5);
    // The sound matches the curve drawn on the screen.
    for (double hz : {100.0, 400.0, 700.0, 1000.0, 1700.0, 5000.0})
        CHECK_NEAR(gainDb(t, hz), double(eqResponseDb(t, float(hz))), 0.25);
    // Cuts: 12 dB/oct vs 48 dB/oct low cut at 1 kHz, tone at 250 Hz.
    t = factory();
    t.band[0].on = true;
    t.band[0].freqHz = 1000;
    const double g12 = gainDb(t, 250);
    t.band[0].slope = 1;
    const double g48 = gainDb(t, 250);
    CHECK(g12 < -20.0 && g12 > -40.0);
    CHECK(g48 < -60.0);
    CHECK(std::fabs(gainDb(t, 8000)) < 0.5);
    // High cut at 2 kHz removes 10 kHz.
    t = factory();
    t.band[3].on = true;
    t.band[3].freqHz = 2000;
    t.band[3].slope = 1;
    CHECK(gainDb(t, 10000) < -50.0);
    CHECK(std::fabs(gainDb(t, 200)) < 0.5);
    // Loud input with +15 dB saturates instead of wrapping.
    t = factory();
    t.band[1].on = true;
    t.band[1].freqHz = 1000;
    t.band[1].gainDb10 = 150;
    sfx::eq4Pack(t, p);
    const auto loud = sine(8000, 1000, 30000);
    std::vector<int16_t> lo(loud.size());
    sfx::process(sfx::kEq4, p, loud.data(), 8000, lo.data(), 8000);
    int peak = 0;
    for (int16_t s : lo) peak = std::max(peak, std::abs(int(s)));
    CHECK(peak >= 32767);
    CHECK_EQ(sfx::process(sfx::kEq4, p, loud.data(), 0, lo.data(), 10), 0u);
}

TEST(eq4_editor_pages) {
    Harness h;
    App& a = *h.app;
    // A 1 kHz tone as the sample.
    const auto src = sine(30000, 1000, 8000);
    std::vector<uint8_t> w(src.size() * 2 + 64);
    w.resize(wavEncode(src.data(), uint32_t(src.size()), 1, w.data(), w.size()));
    h.fs.files["t.wav"] = w;
    CHECK(a.importSample("t.wav", 0));
    a.setCurrentInstrument(0);
    h.tap(btn::SampleEditor);
    EditorScreen& e = ed(h);
    CHECK(!e.inert);
    e.effect = sfx::kEq4;
    h.tap(btn::F6);   // page 2: the bands
    CHECK_EQ(e.page, 2);
    CHECK_STR(a.ui.bar.label[0].top, "Band 1");
    CHECK_STR(a.ui.bar.label[0].bottom, "Off");
    CHECK_STR(a.ui.bar.label[3].top, "Band 4");
    CHECK_STR(a.ui.bar.label[4].top, "Params");
    CHECK_STR(a.ui.bar.label[5].top, "Back");
    CHECK_STR(a.ui.bar.label[6].top, "Preview");
    CHECK_STR(a.ui.bar.label[7].top, "Apply");
    h.run();
    h.tap(btn::F2);   // band 2 on
    CHECK_STR(a.ui.bar.label[1].bottom, "On");
    CHECK_EQ(e.eqBand, 1);
    h.tap(btn::F2);   // and off
    CHECK_STR(a.ui.bar.label[1].bottom, "Off");
    h.tap(btn::Right);
    CHECK_EQ(e.eqBand, 2);   // Right selects without switching
    CHECK_STR(a.ui.bar.label[2].bottom, "Off");
    h.tap(btn::Left);
    h.tap(btn::F5);   // band parameters, with the graph
    CHECK_EQ(e.page, 3);
    h.run();
    CHECK_STR(a.ui.bar.label[0].top, "Type");
    CHECK_STR(a.ui.bar.label[0].bottom, "Bell");
    CHECK_STR(a.ui.bar.label[1].top, "Freq");
    CHECK_STR(a.ui.bar.label[1].bottom, "500 Hz");
    CHECK_STR(a.ui.bar.label[2].top, "Gain");
    CHECK_STR(a.ui.bar.label[3].top, "Q");
    CHECK_STR(a.ui.bar.label[4].top, "Band");
    CHECK_STR(a.ui.bar.label[4].bottom, "2");
    // Gain +6 dB with the jog wheel switches the band on.
    h.tap(btn::F3);
    CHECK_EQ(e.eqParam, 2);
    for (int i = 0; i < 12; ++i) h.enc(1);
    CHECK_STR(a.ui.bar.label[2].bottom, "+6.0 dB");
    h.tap(btn::F2);
    for (int i = 0; i < 12; ++i) h.enc(-1);
    CHECK(e.params[sfx::kEq4][6] == 1.0f);   // band 2 on
    h.run();
    // F5 = next band; band 1 is a cut: its third value is the slope.
    h.tap(btn::F5);
    h.tap(btn::F5);
    h.tap(btn::F5);
    CHECK_EQ(e.eqBand, 0);
    CHECK_STR(a.ui.bar.label[2].top, "Slope");
    CHECK_STR(a.ui.bar.label[2].bottom, "12 dB");
    h.tap(btn::F3);
    h.enc(1);
    CHECK_STR(a.ui.bar.label[2].bottom, "48 dB");
    h.tap(btn::Delete);   // factory value of the parameter
    CHECK_STR(a.ui.bar.label[2].bottom, "12 dB");
    // Back: params -> bands -> markers.
    h.tap(btn::F6);
    CHECK_EQ(e.page, 2);
    h.tap(btn::F6);
    CHECK_EQ(e.page, 1);
    // Back in, then Apply from the parameter page: the +gain bell at 500 Hz
    // changes the tone, the page returns to 1 and Undo restores it.
    h.tap(btn::F6);
    h.tap(btn::F5);
    h.tap(btn::F7);   // preview works
    CHECK(e.previewPlaying);
    h.tap(btn::F7);
    CHECK(!e.previewPlaying);
    h.tap(btn::F8);
    CHECK_EQ(e.page, 1);
    CHECK(e.undoAvail);
    std::vector<int16_t> now(a.bank.left(0), a.bank.left(0) + a.bank.frames(0));
    CHECK_EQ(now.size(), src.size());
    CHECK(now != src);
    h.tap(btn::F4);   // Undo
    std::vector<int16_t> back(a.bank.left(0), a.bank.left(0) + a.bank.frames(0));
    CHECK(back == src);
}

// The single-precision loop against a double-precision reference (same
// coefficients, transposed direct form II): worst settings, noise input.
TEST(eq4_single_precision_matches_double) {
    struct Case {
        int band;
        uint8_t type;
        uint16_t freq;
        uint16_t q;
        uint8_t slope;
        int16_t gain;
    };
    const Case cases[] = {{0, kEqLowCut, 20, 71, 1, 0},   {0, kEqLowCut, 40, 1800, 1, 0}, {0, kEqLowCut, 80, 71, 1, 0},
                          {3, kEqHighCut, 20000, 71, 1, 0}, {3, kEqHighCut, 200, 1800, 1, 0}, {1, kEqPeak, 30, 1800, 0, 150},
                          {1, kEqLowShelf, 20, 71, 0, -150}, {2, kEqNotch, 50, 1800, 0, 0}};
    std::vector<int16_t> in(60000);
    uint32_t x = 99;
    for (auto& s : in) {
        x = x * 1103515245u + 12345u;
        s = int16_t(int((x >> 16) % 20001) - 10000);
    }
    for (const Case& k : cases) {
        TrackEqParams t = factory();
        EqBand& b = t.band[k.band];
        b.on = true;
        b.type = k.type;
        b.freqHz = k.freq;
        b.q100 = k.q;
        b.slope = k.slope;
        b.gainDb10 = k.gain;
        float p[sfx::kMaxParams];
        sfx::eq4Pack(t, p);
        std::vector<int16_t> out(in.size());
        sfx::process(sfx::kEq4, p, in.data(), uint32_t(in.size()), out.data(), uint32_t(out.size()));
        // Reference.
        struct St {
            double c[5], z1 = 0, z2 = 0;
        } st[4];
        const int ns = eqStageCount(b);
        for (int s = 0; s < ns; ++s) {
            float c[5];
            eqStageCoefficients(b, s, c);
            for (int j = 0; j < 5; ++j) st[s].c[j] = double(c[j]);
        }
        int worst = 0;
        for (size_t i = 0; i < in.size(); ++i) {
            double v = in[i];
            for (int s = 0; s < ns; ++s) {
                const double y = st[s].c[0] * v + st[s].z1;
                st[s].z1 = st[s].c[1] * v - st[s].c[3] * y + st[s].z2;
                st[s].z2 = st[s].c[2] * v - st[s].c[4] * y;
                v = y;
            }
            const double r = v > 32767.0 ? 32767.0 : v < -32768.0 ? -32768.0 : std::round(v);
            worst = std::max(worst, std::abs(int(out[i]) - int(r)));
        }
        CHECK(worst <= 24);   // float rounding: 1-2 LSB at normal settings, up to ~18 (-65 dBFS) at 20-50 Hz extremes
        std::printf("  eq4 float vs double: band %d type %d %u Hz -> worst %d LSB\n", k.band, int(k.type), unsigned(k.freq), worst);
    }
}

namespace {
struct Seen {
    uint32_t calls = 0, last = 0, total = 0;
    bool monotonic = true;
};
void onProgress(void* c, uint32_t done, uint32_t total) {
    auto* s = static_cast<Seen*>(c);
    if (done < s->last || done >= total) s->monotonic = false;
    s->last = done;
    s->total = total;
    ++s->calls;
}
}  // namespace

TEST(eq4_reports_progress) {
    TrackEqParams t = factory();
    t.band[1].on = true;
    t.band[1].gainDb10 = 60;
    float p[sfx::kMaxParams];
    sfx::eq4Pack(t, p);
    const auto in = sine(100000, 440, 5000);
    std::vector<int16_t> out(in.size());
    Seen s;
    sfx::process(sfx::kEq4, p, in.data(), uint32_t(in.size()), out.data(), uint32_t(out.size()), onProgress, &s);
    CHECK(s.calls >= 10);   // about every 8192 frames
    CHECK(s.monotonic);
    CHECK_EQ(s.total, 100000u);
    // Same audio with or without the callback.
    std::vector<int16_t> out2(in.size());
    sfx::process(sfx::kEq4, p, in.data(), uint32_t(in.size()), out2.data(), uint32_t(out2.size()));
    CHECK(out == out2);
}

TEST(eq4_preview_label_returns_when_the_sample_ends) {
    Harness h;
    App& a = *h.app;
    const auto src = sine(20000, 1000, 8000);
    std::vector<uint8_t> w(src.size() * 2 + 64);
    w.resize(wavEncode(src.data(), uint32_t(src.size()), 1, w.data(), w.size()));
    h.fs.files["t.wav"] = w;
    CHECK(a.importSample("t.wav", 0));
    a.setCurrentInstrument(0);
    h.tap(btn::SampleEditor);
    EditorScreen& e = ed(h);
    e.effect = sfx::kEq4;
    h.tap(btn::F6);
    h.tap(btn::F7);   // Preview
    CHECK(e.previewPlaying);
    CHECK_STR(a.ui.bar.label[6].top, "Stop");
    h.audio.pump(20);
    h.run();
    CHECK(e.previewPlaying);   // still playing after a short while
    h.audio.pump(600);        // far past the 20000 frames
    const int swapsBefore = h.disp.swaps;
    h.run();
    CHECK(h.disp.swaps > swapsBefore);   // the frame was redrawn (not just the label changed)
    CHECK(!a.engine.previewBufferActive());
    CHECK(!e.previewPlaying);
    CHECK_STR(a.ui.bar.label[6].top, "Preview");
    // The same on the band parameter page, and a second Preview plays again.
    h.tap(btn::F5);
    h.tap(btn::F7);
    CHECK_STR(a.ui.bar.label[6].top, "Stop");
    h.audio.pump(600);
    h.run();
    CHECK_STR(a.ui.bar.label[6].top, "Preview");
}
