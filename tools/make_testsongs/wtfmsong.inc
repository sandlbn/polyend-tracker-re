// Song 17 "OT Test 10 - WTFM" (TESTSONGS.md). Included by testsongs.cpp
// inside its anonymous namespace (it uses Gen, setupSlot and setMacro).
//
// Only the WTFM engine plays (2.2): Synth 1-3 are three WTFM patches
// (instruments 64-66), our own, derived from the engine's built-in Init (no
// factory patch); no sample instrument, no other synth, no MIDI. 124 BPM,
// D minor, an 80s FM pop groove.
//
// Synth 1 "OT WT E-Piano" polyphony 4 (1:1 FM piano, the FM depth decaying
// with Env 2, four-note chords), Synth 2 "OT WT Bass" mono (1:1 FM bass with
// modulator feedback and a fast FM envelope, Reset On), Synth 3 "OT WT Bell"
// mono (3.5:1 bell, Reset Off: free-running operators). When the three play
// together the piano uses two tracks, so the held notes never pass the
// default budget of 4 voices.
//
// Levels: full notes carry no Volume FX; accents and ghosts are Volume 50
// and below. Held chords end with a note-off 12 steps before the pattern
// end, and every macro moved is set back on the last step.

namespace wtfmonly {

using namespace audio::wtfm;

constexpr float kTempoWtfm = 124.0f;
constexpr int kKeys = kSynthBase + 0, kBassI = kSynthBase + 1, kBell = kSynthBase + 2;
enum : int { kTK1 = 0, kTK2, kTK3, kTK4, kTBass, kTBell, kTLane };
constexpr int kMacroCentre = 63;
const FxId kMacroFx[3] = {FxId::MidiCcA, FxId::MidiCcB, FxId::MidiCcC};

// Dm9, Bbmaj7, Gm7, A7sus (close voicings) and the bass roots.
constexpr int kChords[4][4] = {{57, 60, 64, 65}, {57, 58, 62, 65}, {55, 58, 62, 65}, {57, 59, 62, 67}};
constexpr int kRoots[4] = {38, 34, 31, 33};

constexpr float ratio(float r) { return r * 4.0f - 1.0f; }

void setupPatches(Gen& g) {
    // Electric piano: 1:1, a little FM plus Env 2 -> FM (bright attack that
    // mellows), open LP SVF 12. M1 Brightness (FM), M2 Tine (Ratio 2).
    setupSlot(g, 0, SynthModel::WTFM, "OT WT E-Piano",
              {{kFilterType, 4.0f}, {kCutoff, 9000.0f}, {kFilterEnvAmt, 0.0f}, {kNoteTrack, 0.0f}, {kRatio2, ratio(1.0f)},
               {kFine2, 4.0f}, {kFm, 0.35f}, {kEnv2D, 1.2f}, {kEnv2S, 0.0f}, {kEnv2R, 0.4f}, {kAmpA, 0.002f},
               {kAmpD, 2.5f}, {kAmpS, 0.25f}, {kAmpR, 0.6f}, {kModBase, float(kSrcEnv2)}, {kModBase + 1, float(kDstFm)},
               {kModBase + 2, 1.6f}},
              4, -12.0f, 0.0f, 15.0f, 30.0f, [](SynthPatch& p) {
                  setMacro(p, 0, "Brightness", kFm, 0.15f, true);
                  setMacro(p, 1, "Tine", kRatio2, 0.2f);
              });
    // Bass: 1:1, operator 2 feedback, a fast Env 2 -> FM bite, LP MG 24,
    // Reset On (every note starts at the same phases).
    setupSlot(g, 1, SynthModel::WTFM, "OT WT Bass",
              {{kFilterType, 0.0f}, {kCutoff, 2400.0f}, {kResonance, 0.25f}, {kFilterEnvAmt, 0.0f}, {kNoteTrack, 0.5f},
               {kRatio2, ratio(1.0f)}, {kFm, 1.2f}, {kFb2, 0.4f}, {kEnv2D, 0.2f}, {kEnv2S, 0.0f}, {kAmpA, 0.001f},
               {kAmpD, 1.0f}, {kAmpS, 0.6f}, {kAmpR, 0.08f}, {kReset1, 1.0f}, {kReset2, 1.0f}, {kVolume, 1.6f},
               {kModBase, float(kSrcEnv2)}, {kModBase + 1, float(kDstFm)}, {kModBase + 2, 1.5f}},
              1, -4.0f, 0.0f, 0.0f, 0.0f, [](SynthPatch& p) {
                  setMacro(p, 0, "Brightness", kFm, 0.2f, true);
                  setMacro(p, 1, "Cutoff", kCutoff, 0.2f, true);
              });
    // Bell: 3.5:1 inharmonic partials, a long FM decay, Reset Off.
    setupSlot(g, 2, SynthModel::WTFM, "OT WT Bell",
              {{kFilterType, 4.0f}, {kCutoff, 12000.0f}, {kFilterEnvAmt, 0.0f}, {kNoteTrack, 0.0f}, {kRatio2, ratio(3.5f)},
               {kFm, 0.8f}, {kEnv2D, 1.5f}, {kEnv2S, 0.0f}, {kAmpA, 0.001f}, {kAmpD, 2.0f}, {kAmpS, 0.0f},
               {kAmpR, 1.2f}, {kReset1, 0.0f}, {kReset2, 0.0f}, {kModBase, float(kSrcEnv2)},
               {kModBase + 1, float(kDstFm)}, {kModBase + 2, 1.2f}},
              1, -8.0f, 0.0f, 35.0f, 25.0f, [](SynthPatch& p) { setMacro(p, 0, "Brightness", kFm, 0.15f, true); });
}

// Global mixer: delay 3/16 ping-pong, a big room.
void wtfmMixer(Gen& g) {
    Project& p = g.app.project;
    p.setU8(pf::kMixReverb, 85);
    p.setU8(pf::kMixDelay, 65);
    p.setU8(pf::kDelayFeedback, 35);
    p.setU16(pf::kDelayTime, uint16_t(60000.0f / kTempoWtfm * 0.75f));
    p.setU8(pf::kDelayFlags, uint8_t(p.u8(pf::kDelayFlags) | 0x80));
    p.setF32(pf::kReverbSize, 0.8f);
    p.setF32(pf::kReverbDamp, 0.35f);
    g.app.markProjectDirty();
}

void n(Gen& g, int t, int s, int note, int instr, int vol = -1) {
    g.note(t, s, note, instr, vol >= 0 ? FxSet{FxId::Volume, vol > 50 ? 50 : vol} : FxSet{});
}
void macroAt(Gen& g, int s, int col, int macro, int v, int instr) {
    Step* st = g.step(kTLane, s);
    if (!st) return;
    st->instrument = uint8_t(instr);
    g.fx(kTLane, s, col, kMacroFx[macro], v);
}
void macroRamp(Gen& g, int s0, int s1, int col, int macro, int v0, int v1, int instr) {
    for (int s = s0; s <= s1; ++s) macroAt(g, s, col, macro, v0 + (v1 - v0) * (s - s0) / (s1 - s0), instr);
}
void chord4(Gen& g, int s, int bar) {
    for (int k = 0; k < 4; ++k) n(g, kTK1 + k, s, kChords[bar % 4][k], kKeys);
}
void keysOff(Gen& g, int s, int tracks = 4) {
    for (int k = 0; k < tracks; ++k) g.off(kTK1 + k, s);
}
// Bass bar: a syncopated 16th line (root, octave, fifth).
void bassBar(Gen& g, int b0, int bar, bool accents) {
    const int r = kRoots[bar % 4];
    const int st[7] = {0, 3, 6, 8, 10, 12, 14};
    const int nt[7] = {r, r, r + 12, r, r + 7, r + 12, r + 10};
    for (int k = 0; k < 7; ++k) n(g, kTBass, b0 + st[k], nt[k], kBassI, !accents || k == 0 ? -1 : (k % 2 ? 40 : 50));
}
// The bell melody over two bars (D minor).
void bellPhrase(Gen& g, int b0) {
    const int s[8] = {0, 4, 6, 8, 12, 16, 20, 24};
    const int p[8] = {74, 77, 81, 79, 77, 76, 74, 69};
    for (int k = 0; k < 8; ++k) n(g, kTBell, b0 + s[k], p[k], kBell, k % 2 ? 45 : -1);
}

// 1 "Keys": four-note piano chords, one per bar.
void keysPattern(Gen& g) {
    g.beginPattern(1, 64, "Keys");
    for (int bar = 0; bar < 4; ++bar) chord4(g, bar * kBar, bar);
    keysOff(g, 52);
    g.endPattern();
}

// 2 "Bass": the bass alone; M1 Brightness up and down.
void bassPattern(Gen& g) {
    g.beginPattern(2, 64, "Bass");
    for (int bar = 0; bar < 4; ++bar) bassBar(g, bar * kBar, bar, bar >= 2);
    macroRamp(g, 0, 15, 0, 0, kMacroCentre, 127, kBassI);
    macroRamp(g, 16, 31, 0, 0, 127, 0, kBassI);
    macroAt(g, 32, 0, 0, kMacroCentre, kBassI);
    g.off(kTBass, 63);
    g.endPattern();
}

// 3 "Bells": the bell melody twice.
void bellPattern(Gen& g) {
    g.beginPattern(3, 64, "Bells");
    bellPhrase(g, 0);
    bellPhrase(g, 2 * kBar);
    g.off(kTBell, 63);
    g.endPattern();
}

// 4 "Groove": all three; the piano plays two-note shells on tracks 1-2.
void groovePattern(Gen& g) {
    g.beginPattern(4, 64, "Groove");
    for (int bar = 0; bar < 4; ++bar) {
        const int b0 = bar * kBar;
        n(g, kTK1, b0, kChords[bar][1], kKeys, 45);
        n(g, kTK2, b0, kChords[bar][3], kKeys, 45);
        bassBar(g, b0, bar, true);
    }
    bellPhrase(g, 0);
    bellPhrase(g, 2 * kBar);
    keysOff(g, 60, 2);
    g.off(kTBass, 63);
    g.off(kTBell, 63);
    g.endPattern();
}

// 5 "Keys macros": the chords with M1 Brightness swept up and back, then
// M2 Tine (the modulator ratio) in bar 4.
void keysMacroPattern(Gen& g) {
    g.beginPattern(5, 64, "Keys macros");
    for (int bar = 0; bar < 4; ++bar) chord4(g, bar * kBar, bar);
    macroRamp(g, 0, 15, 0, 0, kMacroCentre, 127, kKeys);
    macroRamp(g, 16, 31, 0, 0, 127, kMacroCentre, kKeys);
    macroRamp(g, 48, 62, 0, 1, 0, 127, kKeys);
    macroAt(g, 63, 0, 1, 0, kKeys);
    keysOff(g, 52);
    g.endPattern();
}

// 6 "Ending": one Dm chord, then a last bell over its release and the room.
void endingPattern(Gen& g) {
    g.beginPattern(6, 32, "Ending");
    chord4(g, 0, 0);
    keysOff(g, 12);
    n(g, kTBell, 13, 74, kBell);
    g.endPattern();
}

bool wtfmOnlySong(Gen& g) {
    g.tempo = kTempoWtfm;
    if (!g.begin(false)) return false;
    setupPatches(g);
    if (!g.ok) return false;
    trackNames(g, {"Keys 1", "Keys 2", "Keys 3", "Keys 4", "Bass", "Bell", "Macros"});
    keysPattern(g);
    bassPattern(g);
    bellPattern(g);
    groovePattern(g);
    keysMacroPattern(g);
    endingPattern(g);
    g.playlist({1, 2, 3, 4, 5, 4, 6});
    wtfmMixer(g);
    return g.ok;
}

}  // namespace wtfmonly
