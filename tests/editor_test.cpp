// Sample Editor (spec 06 §6): the 17 effects with their parameter tables,
// page 1/2 navigation, apply on the Start..End selection, preview, the
// one-level undo, and the length limits.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "test.h"
#include "ui_fakes.h"

#include "app/sample_fx.h"
#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;

namespace {
EditorScreen& ed(Harness& h) { return *static_cast<EditorScreen*>(h.app->ui.screen(kModEditor)); }

// Mono WAV with a deterministic pseudo-random signal (+-12000).
std::vector<int16_t> addNoise(Harness& h, const char* path, uint32_t frames) {
    std::vector<int16_t> s(frames);
    uint32_t x = 12345;
    for (uint32_t i = 0; i < frames; ++i) {
        x = x * 1103515245u + 12345u;
        s[i] = int16_t(int((x >> 16) % 24001) - 12000);
    }
    std::vector<uint8_t> out(frames * 2 + 64);
    out.resize(wavEncode(s.data(), frames, 1, out.data(), out.size()));
    h.fs.files[path] = out;
    return s;
}

std::vector<int16_t> slot0(App& a) { return std::vector<int16_t>(a.bank.left(0), a.bank.left(0) + a.bank.frames(0)); }
}  // namespace

TEST(sfx_catalogue_and_dsp) {
    // spec 06 §6.4: 17 effects; parameter counts and defaults.
    CHECK_STR(sfx::effect(0).name, "Normalizer");
    // Title case as in the stock list (manual p.207, STRINGS_DIFF_1.9.3 §4;
    // MANUAL_AUDIT_2026-10-23 #31).
    CHECK_STR(sfx::effect(13).name, "Timestretch Beat");
    CHECK_STR(sfx::effect(14).name, "Timestretch Note");
    CHECK_STR(sfx::effect(15).name, "Fade In");
    CHECK_STR(sfx::effect(16).name, "Fade Out");
    const int counts[17] = {1, 0, 0, 1, 2, 2, 2, 2, 4, 9, 3, 4, 2, 4, 2, 0, 0};
    for (int e = 0; e < sfx::kEffectCount; ++e) CHECK_EQ(sfx::effect(e).params, counts[e]);
    float p[9];
    sfx::defaults(sfx::kDelay, p);
    CHECK_NEAR(p[0], 0.95, 1e-6);
    CHECK_NEAR(p[1], 50, 1e-6);
    sfx::defaults(sfx::kCompressor, p);
    CHECK_NEAR(p[0], -12, 1e-6);
    CHECK_NEAR(p[1], 4, 1e-6);
    char v[24];
    sfx::formatParam(sfx::kDelay, 0, 0.95f, v, sizeof(v));
    CHECK_STR(v, "95%");
    sfx::formatParam(sfx::kChorus, 0, 441.0f, v, sizeof(v));
    CHECK_STR(v, "10.0 ms");
    sfx::formatParam(sfx::kStretchBeat, 3, 4.0f, v, sizeof(v));
    CHECK_STR(v, "1.00");
    CHECK_NEAR(sfx::clampParam(sfx::kAmplifier, 0, 9.0f), 5.0, 1e-6);
    CHECK_NEAR(sfx::clampParam(sfx::kAmplifier, 0, 1.004f), 1.0, 1e-6);

    const uint32_t n = 20000;
    std::vector<int16_t> in(n), out(600000);
    for (uint32_t i = 0; i < n; ++i) in[i] = int16_t(8000.0 * std::sin(double(i) * 0.05));
    for (int e = 0; e < sfx::kEffectCount; ++e) {
        sfx::defaults(e, p);
        if (e == sfx::kWavetableSmoother) {
            p[0] = 8;
            p[1] = 256;
        }
        const uint32_t m = sfx::outputFrames(e, p, n);
        CHECK_EQ(sfx::process(e, p, in.data(), n, out.data(), uint32_t(out.size())), m);
    }
    auto peak = [&](uint32_t m) {
        int pk = 0;
        for (uint32_t i = 0; i < m; ++i) pk = std::abs(out[i]) > pk ? std::abs(out[i]) : pk;
        return pk;
    };
    sfx::defaults(sfx::kNormalizer, p);
    sfx::process(sfx::kNormalizer, p, in.data(), n, out.data(), n);
    CHECK(peak(n) >= 32766);
    p[0] = 50;
    sfx::process(sfx::kNormalizer, p, in.data(), n, out.data(), n);
    CHECK(std::abs(peak(n) - 16383) <= 2);
    sfx::process(sfx::kReverse, p, in.data(), n, out.data(), n);
    CHECK_EQ(out[0], in[n - 1]);
    sfx::process(sfx::kFadeIn, p, in.data(), n, out.data(), n);
    CHECK_EQ(out[0], 0);
    sfx::defaults(sfx::kBitcrusher, p);   // 4 bits
    sfx::process(sfx::kBitcrusher, p, in.data(), n, out.data(), n);
    for (uint32_t i = 0; i < n; ++i) CHECK(out[i] % 4096 == 0);
    sfx::defaults(sfx::kLimiter, p);   // -6 dB
    sfx::defaults(sfx::kAmplifier, p);
    p[0] = 4.0f;
    std::vector<int16_t> loud(n);
    sfx::process(sfx::kAmplifier, p, in.data(), n, loud.data(), n);
    sfx::defaults(sfx::kLimiter, p);
    sfx::process(sfx::kLimiter, p, loud.data(), n, out.data(), n);
    // spec 18 §2.2: smoothed gain, no hard clip; settles near -6 dB.
    int late = 0;
    for (uint32_t i = n / 2; i < n; ++i) late = std::abs(out[i]) > late ? std::abs(out[i]) : late;
    CHECK(late < 21000);
    sfx::defaults(sfx::kStretchNote, p);
    p[0] = 0.25f;
    CHECK_EQ(sfx::outputFrames(sfx::kStretchNote, p, n), 11025u);
    sfx::defaults(sfx::kStretchBeat, p);   // 120 BPM, 16 steps = 2 s
    CHECK_EQ(sfx::outputFrames(sfx::kStretchBeat, p, n), 88200u);
    sfx::defaults(sfx::kWavetableSmoother, p);
    CHECK_EQ(sfx::outputFrames(sfx::kWavetableSmoother, p, n), 256u * 2048u);
}

TEST(editor_apply_reverse_and_undo) {
    Harness h;
    App& a = *h.app;
    const auto src = addNoise(h, "n.wav", 30000);
    CHECK(a.importSample("n.wav", 0));
    a.setCurrentInstrument(0);
    h.tap(btn::SampleEditor);
    EditorScreen& e = ed(h);
    CHECK(!e.inert);
    CHECK(h.disp.swaps > 0);
    CHECK_STR(a.ui.bar.label[3].top, "Undo");
    CHECK(!a.ui.bar.label[3].enabled);
    // F7/F8: first press focuses the list, then previous/next.
    h.tap(btn::F8);
    CHECK_EQ(e.focus, 3);
    h.tap(btn::F8);
    h.tap(btn::F8);
    CHECK_EQ(e.effect, int(sfx::kReverse));
    CHECK_STR(a.ui.bar.label[6].bottom, "Reverse");
    CHECK_STR(a.ui.bar.label[5].top, "Apply");
    h.tap(btn::F6);
    auto now = slot0(a);
    CHECK_EQ(now.size(), src.size());
    CHECK_EQ(now[0], src[src.size() - 1]);
    CHECK_EQ(now[100], src[src.size() - 101]);
    CHECK(e.undoAvail);
    CHECK(a.ui.bar.label[3].enabled);
    // F4 swaps back: exactly one level.
    h.tap(btn::F4);
    CHECK(slot0(a) == src);
    CHECK(!e.undoAvail);
    h.tap(btn::F4);
    CHECK(slot0(a) == src);
    // Selection: reverse only the second half.
    e.startM = 32768;
    h.tap(btn::F6);
    now = slot0(a);
    const uint32_t s0 = uint32_t(uint64_t(32768) * 30000 / 65535);
    CHECK_EQ(now[0], src[0]);
    CHECK_EQ(now[s0 - 1], src[s0 - 1]);
    CHECK_EQ(now[s0], src[29999]);
    CHECK_EQ(now[29999], src[s0]);
}

TEST(editor_parameters_page_preview_apply) {
    Harness h;
    App& a = *h.app;
    const auto src = addNoise(h, "n.wav", 20000);
    CHECK(a.importSample("n.wav", 0));
    a.setCurrentInstrument(0);
    h.tap(btn::SampleEditor);
    EditorScreen& e = ed(h);
    h.tap(btn::F8);   // focus list
    h.enc(3);         // Amplifier
    CHECK_EQ(e.effect, int(sfx::kAmplifier));
    CHECK_STR(a.ui.bar.label[5].top, "Select");
    h.run(30);
    CHECK(h.disp.lastHasText("Sample Editor 1/2"));
    h.tap(btn::F6);   // page 2
    CHECK_EQ(e.page, 2);
    CHECK_STR(a.ui.bar.label[0].top, "Gain");
    CHECK_STR(a.ui.bar.label[0].bottom, "1.00");
    CHECK_STR(a.ui.bar.label[7].top, "Apply");
    h.enc(-50);
    CHECK_STR(a.ui.bar.label[0].bottom, "0.50");
    h.tap(btn::Delete);   // default
    CHECK_STR(a.ui.bar.label[0].bottom, "1.00");
    h.enc(-50);
    // Preview: processed, then played; parameter changes reset the state.
    h.tap(btn::F7);
    CHECK_EQ(e.previewState, 1);
    CHECK(a.engine.previewBufferActive());
    CHECK_STR(a.ui.bar.label[6].top, "Stop");
    h.tap(btn::F7);
    CHECK(!a.engine.previewBufferActive());
    h.tap(btn::Up);
    CHECK_EQ(e.previewState, 0);
    h.tap(btn::Down);
    h.tap(btn::F8);   // Apply
    CHECK_EQ(e.page, 1);
    const auto now = slot0(a);
    for (uint32_t i = 0; i < 20000; i += 997) CHECK(std::abs(now[i] - src[i] / 2) <= 1);
    // Equalizer: 9 parameters on 3 pages via More.
    e.effect = sfx::kEqualizer;
    h.tap(btn::F6);
    CHECK_STR(a.ui.bar.label[4].top, "More");
    h.tap(btn::F5);
    CHECK_EQ(e.paramPage, 1);
    CHECK_STR(a.ui.bar.label[0].top, "Mid freq");
    h.tap(btn::F5);
    h.tap(btn::F5);
    CHECK_EQ(e.paramPage, 0);
    h.tap(btn::F6);   // back
    CHECK_EQ(e.page, 1);
}

TEST(editor_length_changes_and_limits) {
    Harness h(true, 200000);
    App& a = *h.app;
    addNoise(h, "n.wav", 44100);
    CHECK(a.importSample("n.wav", 0));
    a.setCurrentInstrument(0);
    h.tap(btn::SampleEditor);
    EditorScreen& e = ed(h);
    // Timestretch (note) to 0.5 s.
    e.effect = sfx::kStretchNote;
    e.params[sfx::kStretchNote][0] = 0.5f;
    CHECK(e.apply());
    CHECK_EQ(a.bank.frames(0), 22050u);
    CHECK_EQ(a.instruments[0].lengthFrames, 22050u);
    CHECK(e.undo());
    CHECK_EQ(a.bank.frames(0), 44100u);
    // Delay lengthens the selection by K*D (spec 18 §2.2): f = 0.5, 10 ms
    // -> D = 441, K = 7; the rest of the sample follows the echo tail.
    const auto before = slot0(a);
    e.effect = sfx::kDelay;
    e.params[sfx::kDelay][0] = 0.5f;
    e.params[sfx::kDelay][1] = 10.0f;
    e.startM = 0;
    e.endM = 32767;
    const uint32_t s1 = uint32_t(uint64_t(32767) * 44100 / 65535);
    CHECK(e.apply());
    CHECK_EQ(a.bank.frames(0), 44100u + 7u * 441u);
    CHECK_EQ(slot0(a)[0], before[0]);
    CHECK_EQ(slot0(a)[44100 + 7 * 441 - 1], before[44099]);
    CHECK_EQ(slot0(a)[s1 + 7 * 441], before[s1]);
    CHECK(e.undo());
    CHECK(slot0(a) == before);
    e.startM = 0;
    e.endM = 65535;
    // Crop keeps Start..End.
    e.effect = sfx::kCrop;
    e.startM = 0;
    e.endM = 32767;
    CHECK(e.apply());
    CHECK_EQ(a.bank.frames(0), uint32_t(uint64_t(32767) * 44100 / 65535));
    CHECK_EQ(e.endM, 65535);
    // A result beyond the free memory is refused with a popup.
    e.effect = sfx::kStretchNote;
    e.startM = 0;
    e.endM = 65535;
    e.params[sfx::kStretchNote][0] = 8.0f;
    CHECK(!e.apply());
    CHECK(a.ui.askOpen());
    h.tap(btn::F8);
    // MIDI instrument: inert.
    a.setCurrentInstrument(50);
    h.tap(btn::Pattern);
    h.tap(btn::SampleEditor);
    CHECK(e.inert);
    CHECK(!e.apply());
}

// 1.9.5 stock chrome: the heaviest waveform screens stay inside the FT81x
// display list (2048 words): a stereo sample in the Sample Editor (two
// 300-column views, flags, the 14-row list, the selector) and Sample
// Playback in a loop mode (four markers with flags).
TEST(sample_screens_display_list_budget) {
    Harness h;
    App& a = *h.app;
    // WAV imports are mono; a stereo sample (as from a .pti) goes straight
    // into the bank, planar L then R.
    const uint32_t frames = 40000;
    CHECK(a.bank.freeSamples() > frames * 2);
    int16_t* t = a.bank.tail();
    for (uint32_t i = 0; i < frames * 2; ++i) t[i] = int16_t(12000.0 * std::sin(double(i) * 0.013));
    a.bank.commitTail(0, frames, true);
    a.instruments[0].setDefault();
    std::snprintf(a.instruments[0].name, sizeof(a.instruments[0].name), "stereo");
    a.instruments[0].lengthFrames = frames;
    a.instrPresent[0] = true;
    a.setCurrentInstrument(0);
    CHECK(a.bank.stereo(0));
    h.tap(btn::SampleEditor);
    h.app->ui.dirty();
    h.run(40);
    CHECK(a.ui.lastDlWords() > 1000);
    CHECK(a.ui.lastDlWords() < 2048);
    h.tap(btn::SamplePlayback);
    h.tap(btn::F8);   // Forward Loop: start, end and both loop markers
    h.app->ui.dirty();
    h.run(40);
    CHECK_EQ(int(a.instruments[0].playMode), 1);
    CHECK(a.ui.lastDlWords() < 2048);
}

// Pads in the Sample Editor audition the sample being edited (the processed
// selection, like Preview), not the instrument through the Sample Playback
// points and envelope; releasing the pad stops it at once (user report).
TEST(editor_pads_audition_the_edit) {
    Harness h;
    App& a = *h.app;
    addNoise(h, "n.wav", 30000);
    CHECK(a.importSample("n.wav", 0));
    a.setCurrentInstrument(0);
    h.tap(btn::SampleEditor);
    EditorScreen& e = ed(h);
    CHECK(!e.inert);
    h.push(InputEvent::kPadDown, 5);
    h.run(10);
    CHECK(a.engine.previewBufferActive());   // the edit preview plays
    CHECK(e.previewPlaying);
    bool trackVoice = false;
    for (int t = 0; t < 8; ++t) trackVoice |= a.engine.voice(t).active();
    CHECK(!trackVoice);                      // not the instrument on a track voice
    h.push(InputEvent::kPadUp, 5);
    h.run(10);
    CHECK(!a.engine.previewBufferActive());  // cut on release
    CHECK(!e.previewPlaying);
}

// Moving a marker after a preview: the next preview plays the new selection
// in full, from the new start (user report 2.0.7: moving Start cut the same
// amount off the end of the audition).
TEST(editor_preview_follows_moved_markers) {
    Harness h;
    App& a = *h.app;
    const auto src = addNoise(h, "n.wav", 40000);
    CHECK(a.importSample("n.wav", 0));
    a.setCurrentInstrument(0);
    h.tap(btn::SampleEditor);
    EditorScreen& e = ed(h);
    e.effect = 1;   // Crop: the preview is the selection itself
    h.push(InputEvent::kPadDown, 0);
    h.run(10);
    h.push(InputEvent::kPadUp, 0);
    h.run(10);
    e.startM = 30000;   // move Start to ~46 %
    h.push(InputEvent::kPadDown, 0);
    h.run(10);
    const uint32_t len = a.bank.frames(0);
    const uint32_t s0 = uint32_t(uint64_t(e.startM) * len / 65535), s1 = uint32_t(uint64_t(e.endM) * len / 65535);
    CHECK_EQ(a.engine.previewBufferAvail(), s1 - s0);   // the whole new selection
    const int16_t* played = a.bank.tail();
    const int16_t* cur = a.bank.data(0);
    bool same = true;
    for (uint32_t i = 0; same && i < 200; ++i) same = played[i] == cur[s0 + i];
    CHECK(same);                                       // from the new start
    h.push(InputEvent::kPadUp, 0);
    h.run(10);
}

// 2.x: pads play the edit preview at their note (pad -> note map, note 60 =
// own pitch) through the player's 16.16 step; MIDI notes and Preview keep
// the own pitch. A lower pad plays the same selection for longer.
TEST(editor_pads_play_pitched) {
    Harness h;
    App& a = *h.app;
    addNoise(h, "n.wav", 20000);
    CHECK(a.importSample("n.wav", 0));
    a.setCurrentInstrument(0);
    h.tap(btn::SampleEditor);
    EditorScreen& e = ed(h);
    e.effect = 1;   // Crop: the preview is the selection itself
    uint8_t lo = 0, hi = 0;
    for (uint8_t p = 0; p < 48; ++p) {
        if (a.padNote(p) < a.padNote(lo)) lo = p;
        if (a.padNote(p) > a.padNote(hi)) hi = p;
    }
    CHECK(a.padNote(lo) < a.padNote(hi));
    auto playBlocks = [&](uint8_t pad) {
        h.push(InputEvent::kPadDown, pad);
        h.run(10);
        CHECK_EQ(a.engine.previewBufferStep(), App::previewStep(a.padNote(pad)));
        int n = 0;
        while (a.engine.previewBufferActive() && n < 100000) {
            h.audio.pump(1);
            ++n;
        }
        h.push(InputEvent::kPadUp, pad);
        h.run(10);
        return n;
    };
    const int nLo = playBlocks(lo), nHi = playBlocks(hi);
    CHECK(nLo > nHi);
    const double ratio = double(App::previewStep(a.padNote(hi))) / double(App::previewStep(a.padNote(lo)));
    CHECK(std::fabs(double(nLo) / double(nHi) - ratio) / ratio < 0.1);
    // A MIDI note keeps the own pitch.
    a.cfg.notesIn = 1;
    h.midi.in.insert(h.midi.in.end(), {uint8_t(0x90), uint8_t(40), uint8_t(100)});
    h.run(10);
    CHECK(a.engine.previewBufferActive());
    CHECK_EQ(a.engine.previewBufferStep(), 65536u);
}

// Preview playhead (owner request): while the edit preview plays, its
// position (mapped back onto the selection) moves across the sample.
TEST(editor_preview_shows_a_playhead) {
    Harness h;
    App& a = *h.app;
    addNoise(h, "p.wav", 40000);
    CHECK(a.importSample("p.wav", 0));
    a.setCurrentInstrument(0);
    h.tap(btn::SampleEditor);
    EditorScreen& e = ed(h);
    e.effect = 1;        // Crop: the preview is the selection itself
    e.startM = 16384;    // ~25 %
    CHECK(e.playheadFrac() < 0.0f);
    h.push(InputEvent::kPadDown, 0);
    h.run(10);
    h.audio.pump(5);
    const float f0 = e.playheadFrac();
    h.audio.pump(60);
    const float f1 = e.playheadFrac();
    CHECK(f0 >= 0.24f && f0 < 0.4f);   // starts at the selection start
    CHECK(f1 > f0);
    h.push(InputEvent::kPadUp, 0);
    h.run(10);
    CHECK(e.playheadFrac() < 0.0f);
}

TEST(editor_timestretch_length_starts_at_the_sample_length) {
    // MANUAL_AUDIT_2026-10-23 #14 (manual p.218: Timestretch Note "Length
    // 3.198 s" = the sample's length): the Length starts at the sample's
    // length (clamped to 0.1..8 s), not 1.00 s.
    Harness h(true, 400000);
    App& a = *h.app;
    addNoise(h, "n.wav", 3 * 44100 / 2);   // 1.5 s
    CHECK(a.importSample("n.wav", 0));
    a.setCurrentInstrument(0);
    h.tap(btn::SampleEditor);
    CHECK(std::fabs(ed(h).params[sfx::kStretchNote][0] - 1.5f) < 0.011f);
}

TEST(editor_db_values_show_their_step) {
    // MANUAL_AUDIT_2026-10-23 #15 (manual p.213 "Low 0.00 dB"; thresholds
    // "-6 dB"): EQ gains step 0.05 dB and show two decimals; the 1-dB
    // thresholds show whole dB.
    char a[24], b[24];
    sfx::formatParam(sfx::kEqualizer, 0, 0.05f, a, sizeof(a));
    sfx::formatParam(sfx::kEqualizer, 0, 0.10f, b, sizeof(b));
    CHECK(std::string(a) == "0.05 dB");
    CHECK(std::string(b) == "0.10 dB");
    sfx::formatParam(sfx::kLimiter, 0, -6.0f, a, sizeof(a));
    CHECK(std::string(a) == "-6 dB");
}

TEST(loader_memory_bar_shows_the_change_in_colour) {
    // MANUAL_AUDIT_2026-10-23 #20 (manual p.44-45: "red element if memory will
    // increase and green bar if memory will decrease").
    Harness h(true, 2000000);
    App& a = *h.app;
    addNoise(h, "big.wav", 88200);
    addNoise(h, "small.wav", 4410);
    CHECK(a.importSample("big.wav", 0));    // slot 1 holds the big sample
    h.tap(btn::SampleLoader);
    h.run(2000);
    LoaderScreen& l = *static_cast<LoaderScreen*>(a.ui.screen(kModLoader));
    auto pointAt = [&](const char* name, int slot) {
        l.insCursor = slot;
        int idx = -1;
        for (int i = 0; i < l.count; ++i)
            if (std::strcmp(l.entries[i], name) == 0) idx = i;
        CHECK(idx >= 0);
        for (int g = 0; g < 200 && l.sdCursor != idx; ++g) h.tap(l.sdCursor < idx ? btn::Down : btn::Up);
        h.run(500);
        a.ui.dirty();
        h.run(40);
    };
    auto hasColour = [&](uint32_t c) {
        char want[16];
        std::snprintf(want, sizeof(want), "COLOR %06X", unsigned(c & 0xFFFFFF));
        for (const std::string& s : h.disp.last)
            if (s == want) return true;
        return false;
    };
    pointAt("big.wav", 2);    // into an empty slot: memory grows
    CHECK(hasColour(a.ui.th().alert));
    pointAt("small.wav", 0);  // replaces the big sample: memory shrinks
    CHECK(hasColour(a.ui.th().good));
}
