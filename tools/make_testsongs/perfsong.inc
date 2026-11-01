// Songs 20-22, the Performance screen checks (PERFORMANCE_CHECK.md, the
// checklist that comes with the songs; issue #86). Included by
// testsongs.cpp inside its anonymous namespace.
//
// Made to be played on stock 1.9.2 and on OpenTracker with the same steps:
// sample instruments on tracks 1-8 only, and the 12 Performance columns the
// checklist names, stored as the project's presets (Config > Performance
// presets: Per Project loads them).
//
//   "OT Perf 01 - Tracks": pattern 1 is an 8-note rising run, note k on
//   track k with its own sound, so an FX that reaches the wrong track is
//   heard on the wrong note. Pattern 2: every track plays every step (solo
//   one with Shift + track key mutes) for Step Repeater and play modes.
//   "OT Perf 02 - Filters": one held saw per track with different
//   instrument filter settings (off / LP / HP / off with a low cutoff) and
//   a held noise, for the Performance LP / HP / BP columns.
//   "OT Perf 03 - Song Length": three 16-step patterns in the song; track 1
//   ticks every step with a rising pitch, track 2 plays p bells at the start
//   of pattern p, track 3 holds the pattern's bass note.

namespace perfcheck {

constexpr double kC4 = 261.6256;   // the samples play their own pitch at note 60

// The checklist's columns 1-12 (rows 1..3 below the top "off" row).
void presets(Gen& g) {
    const perf::Source src = perf::projectSource(g.app.project);
    struct Slot {
        uint8_t fx;
        int16_t v[3];
    };
    const Slot slots[perf::kSlots] = {
        {perf::kVolume, {-100, -50, 50}},      // 1: row 1 silences every track the FX reaches
        {perf::kRepeater, {11, 9, 5}},         // 2: "1/4", "1/2", "4" steps
        {perf::kPlayMode, {1, 2, 13}},         // 3: Back, Rnd, C11 (quarter speed)
        {perf::kPatternLength, {3, 4, 2}},     // 4: 4, 8, 2 steps (global)
        {perf::kLowPass, {-50, -80, -95}},     // 5
        {perf::kHighPass, {30, 60, 90}},       // 6
        {perf::kBandPass, {-40, 20, 60}},      // 7
        {perf::kTune, {12, -12, 7}},           // 8
        {perf::kPanning, {-100, 100, -50}},    // 9
        {perf::kDirection, {1, 1, 1}},         // 10: "<<<"
        {perf::kBitDepth, {-6, -9, -12}},      // 11
        {perf::kReverbSend, {50, 100, 25}},    // 12
    };
    for (int s = 0; s < perf::kSlots; ++s) {
        src.setType(s, slots[s].fx);
        src.setValue(s, 0, 0);
        for (int r = 1; r < perf::kRows; ++r) src.setValue(s, r, slots[s].v[r - 1]);
        src.setRow(s, 0);
    }
    src.validate();
    g.app.markProjectDirty();
}

// A short tone at C4 with its own colour (k = 0..7), peak -6 dBFS.
std::vector<int16_t> tone(int k) {
    using namespace syn;
    if (k == 3) return toI16(fm(kC4, 3.5, 3.0, 0.2, 0.002, 0.3, 0.6), 0.5);    // bell
    if (k == 5) return toI16(fm(kC4, 1.0, 2.0, 0.05, 0.001, 0.12, 0.4), 0.5);  // pluck
    if (k == 6) return toI16(fm(kC4, 2.0, 1.5, 1.0, 0.01, 0.2, 0.5), 0.5);     // reed
    const double len = k == 7 ? 0.12 : 0.4;
    Buf b(frames(len));
    Saw saw;
    saw.freq(kC4);
    OnePole lp;
    lp.cutoff(4000);
    for (size_t i = 0; i < b.size(); ++i) {
        const double t = double(i) / kRate, ph = 2 * kPi * kC4 * t;
        double v = 0;
        switch (k) {
            case 0: v = std::sin(ph); break;                                                // sine
            case 1: v = std::tanh(4.0 * std::sin(ph)); break;                              // square
            case 2: v = lp(saw.next()); break;                                             // saw
            case 4: v = std::sin(ph) + 0.5 * std::sin(2 * ph) + 0.33 * std::sin(3 * ph); break;   // organ
            default: v = std::sin(2 * ph); break;                                          // blip
        }
        b[i] = float(v * env(t, 0.002, k == 7 ? 0.04 : 0.15));
    }
    declick(b, 16, 512);
    return toI16(b, 0.5);
}

bool tracksSong(Gen& g) {
    g.tempo = 110.0f;
    if (!g.begin()) return false;
    const char* names[8] = {"Sine", "Square", "Saw", "Bell", "Organ", "Pluck", "Reed", "Blip"};
    for (int k = 0; k < 8; ++k)
        if (!g.addMono(k, names[k], tone(k))) return false;
    trackNames(g, {"1 Sine", "2 Square", "3 Saw", "4 Bell", "5 Organ", "6 Pluck", "7 Reed", "8 Blip"});
    const int scale[8] = {0, 2, 4, 5, 7, 9, 11, 12};
    g.beginPattern(1, 16, "Run one note per track");
    for (int t = 0; t < 8; ++t) g.note(t, 2 * t, 60 + scale[t], t);
    g.endPattern();
    g.beginPattern(2, 16, "All tracks every step");
    for (int t = 0; t < 8; ++t)
        for (int s = 0; s < 16; ++s) g.note(t, s, 60 + scale[s % 8] + (s >= 8 ? 12 : 0), t);
    g.endPattern();
    Project& p = g.app.project;
    for (int t = 0; t < 8; ++t) p.setU8(pf::kTrackVolume + uint32_t(t), 60);
    g.app.markProjectDirty();
    g.playlist({1, 2});
    presets(g);
    return g.ok;
}

bool filtersSong(Gen& g) {
    g.tempo = 120.0f;
    if (!g.begin()) return false;
    using namespace syn;
    int loopAt = 0;
    const std::vector<int16_t> saw = toI16(sawTone(130.8128, 8.0, 9000.0, 2.0, 0.5, loopAt), 0.5);   // C3
    const char* sawNames[5] = {"Saw filter off", "Saw LP 50", "Saw HP 50", "", "Saw off cutoff 20"};
    for (int k : {0, 1, 2, 4}) {
        if (!g.addMono(k, sawNames[k], saw)) return false;
        g.loop(k, PlayMode::ForwardLoop, uint32_t(loopAt));
    }
    Buf n(frames(2.0));
    Rng r(71);
    for (float& v : n) v = float(r.bi());
    crossfadeLoop(n, frames(0.5), int(n.size()), frames(0.08));
    if (!g.addMono(3, "Noise filter off", toI16(n, 0.4))) return false;
    g.loop(3, PlayMode::ForwardLoop, uint32_t(frames(0.5)));
    // Instrument filters: 1 LP 50 %, 2 HP 50 %, 4 off with the cutoff at 20 %.
    g.ins(1).filterEnabled = 1;
    g.ins(1).filterType = uint8_t(FilterType::LowPass);
    g.ins(1).cutoff = 0.5f;
    g.ins(2).filterEnabled = 1;
    g.ins(2).filterType = uint8_t(FilterType::HighPass);
    g.ins(2).cutoff = 0.5f;
    g.ins(4).filterEnabled = 0;
    g.ins(4).cutoff = 0.2f;
    trackNames(g, {"1 No filter", "2 Instr LP", "3 Instr HP", "4 Noise", "5 Off cut 20"});
    g.beginPattern(1, 32, "Held tones");
    for (int t = 0; t < 5; ++t) g.note(t, 0, 60, t);
    g.endPattern();
    Project& p = g.app.project;
    for (int t = 0; t < 5; ++t) p.setU8(pf::kTrackVolume + uint32_t(t), 60);
    g.app.markProjectDirty();
    g.playlist({1, 1, 1, 1});   // several slots: the song position moves on (host test)
    presets(g);
    return g.ok;
}

bool songLengthSong(Gen& g) {
    g.tempo = 100.0f;
    if (!g.begin()) return false;
    using namespace syn;
    Buf tick(frames(0.08));
    for (size_t i = 0; i < tick.size(); ++i) {
        const double t = double(i) / kRate;
        tick[i] = float(std::sin(2 * kPi * kC4 * t) * env(t, 0.001, 0.025));
    }
    declick(tick, 8, 256);
    if (!g.addMono(0, "Tick", toI16(tick, 0.5))) return false;
    if (!g.addMono(1, "Bell", toI16(fm(kC4, 3.5, 3.0, 0.2, 0.002, 0.3, 0.6), 0.5))) return false;
    int loopAt = 0;
    if (!g.addMono(2, "Bass", toI16(sawTone(65.4064, 6.0, 1200.0, 2.0, 0.5, loopAt), 0.5))) return false;   // C2
    g.loop(2, PlayMode::ForwardLoop, uint32_t(loopAt));
    trackNames(g, {"1 Step tick", "2 Pattern no", "3 Bass"});
    const int bass[3] = {60, 65, 67};   // C, F, G
    const char* pnames[3] = {"Pattern 1: one bell", "Pattern 2: two bells", "Pattern 3: three bells"};
    for (int pn = 1; pn <= 3; ++pn) {
        g.beginPattern(pn, 16, pnames[pn - 1]);
        for (int s = 0; s < 16; ++s) g.note(0, s, 60 + s, 0);   // the pitch climbs with the step
        for (int s = 0; s < pn; ++s) g.note(1, s, 84, 1);
        g.note(2, 0, bass[pn - 1], 2);
        g.endPattern();
    }
    Project& p = g.app.project;
    for (int t = 0; t < 3; ++t) p.setU8(pf::kTrackVolume + uint32_t(t), 66);
    g.app.markProjectDirty();
    g.playlist({1, 2, 3, 1, 2, 3});
    presets(g);
    return g.ok;
}

}  // namespace perfcheck
