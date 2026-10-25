// Song 18 "OT Test 11 - IDM" (TESTSONGS.md). Included by testsongs.cpp inside
// its anonymous namespace after percsong.inc (it reuses the PERC kit and its
// macros, Gen, setupSlot and setMacro).
//
// A dense IDM track on the three synth slots only, to stress the sequencer
// FX on synth tracks and the engines together: Synth 1 a PERC kit, Synth 2
// an ACD acid line, Synth 3 WTFM glass stabs (three voices). 168 BPM, F minor.
//
// Rhythm devices: a 3+3+2 kick against 4/4 hats, snare ratchets (Roll),
// hats and percussion on Chance with micro-moves, an acid line with Random
// Note and Tune jumps, glitch bars with rolled kicks and pitched toms.
//
// Levels (TESTSONGS.md Volume law): full hits carry no Volume FX, accents and
// ghosts are Volume 50 and below. Every macro moved is set back on the last
// step. The stabs end with a note-off before the pattern end.

namespace idm {

using namespace perconly;

constexpr float kTempoIdm = 168.0f;
constexpr int kDrums = kSynthBase + 0, kAcid = kSynthBase + 1, kGlass = kSynthBase + 2;
// Tracks (0-based): kick, snare, hats, perc / toms, acid, three stab voices,
// a macro lane.
enum : int { kTKick = 0, kTSnare, kTHat, kTPerc, kTAcid, kTG1, kTG2, kTG3, kTLane };

// Roll FX value: type * 9 + hits index + 1 (hits 1, 2, 3, 4, 6, 8, 12, 16).
constexpr int roll(int type, int hitsIdx) { return type * 9 + hitsIdx + 1; }

// Fm9 (no root), Dbmaj7, Ebsus, Cm7 in three voices; the acid roots.
constexpr int kStabs[4][3] = {{68, 72, 79}, {65, 68, 72}, {67, 70, 75}, {63, 67, 70}};
constexpr int kAcidRoots[4] = {41, 37, 39, 36};

void setupPatches(Gen& g) {
    setupSlot(g, 0, SynthModel::PERC, "OT IDM Kit", kitTight(), 1, -9.0f, 0.0f, 10.0f, 10.0f, kitMacros);
    {
        using namespace audio::acd;
        // Acid: saw into a resonant 24 dB filter, a snappy filter envelope,
        // glide on overlapping notes. M1 Cutoff, M2 Resonance.
        setupSlot(g, 1, SynthModel::ACD, "OT IDM Acid",
                  {{kSawMix, 1.0f}, {kSquareMix, 0.0f}, {kSubMix, 0.2f}, {kFilterType, float(kSvf24)},
                   {kCutoff, 500.0f}, {kResonance, 0.75f}, {kFilterEnv, 0.55f}, {kModA, 0.001f}, {kModD, 0.18f},
                   {kModS, 0.0f}, {kAmpA, 0.001f}, {kAmpD, 0.3f}, {kAmpS, 0.7f}, {kAmpR, 0.05f},
                   {kGlideMode, float(kOverlap)}, {kGlideTime, 0.06f}},
                  1, -7.0f, 0.0f, 20.0f, 5.0f, [](SynthPatch& p) {
                      setMacro(p, 0, "Cutoff", kCutoff, 0.35f, true);
                      setMacro(p, 1, "Reso", kResonance, 0.25f, true);
                  });
    }
    {
        using namespace audio::wtfm;
        // Glass: 2:1 FM with a short FM envelope, a quick decay, wide room.
        setupSlot(g, 2, SynthModel::WTFM, "OT IDM Glass",
                  {{kFilterType, 4.0f}, {kCutoff, 9000.0f}, {kFilterEnvAmt, 0.0f}, {kNoteTrack, 0.0f},
                   {kRatio2, 2.0f * 4.0f - 1.0f}, {kFm, 0.55f}, {kEnv2D, 0.35f}, {kEnv2S, 0.0f}, {kAmpA, 0.002f},
                   {kAmpD, 0.6f}, {kAmpS, 0.0f}, {kAmpR, 0.5f}, {kModBase, float(kSrcEnv2)},
                   {kModBase + 1, float(kDstFm)}, {kModBase + 2, 1.4f}},
                  3, -13.0f, 0.0f, 35.0f, 40.0f, [](SynthPatch& p) { setMacro(p, 0, "Bright", kFm, 0.2f, true); });
    }
}

// Delay 3/16 ping-pong, a short bright room.
void idmMixer(Gen& g) {
    Project& p = g.app.project;
    p.setU8(pf::kMixReverb, 70);
    p.setU8(pf::kMixDelay, 70);
    p.setU8(pf::kDelayFeedback, 45);
    p.setU16(pf::kDelayTime, uint16_t(60000.0f / kTempoIdm * 0.75f));
    p.setU8(pf::kDelayFlags, uint8_t(p.u8(pf::kDelayFlags) | 0x80));
    p.setF32(pf::kReverbSize, 0.55f);
    p.setF32(pf::kReverbDamp, 0.2f);
    g.app.markProjectDirty();
}

void hit(Gen& g, int t, int s, int note, int instr, int vol = -1, FxSet b = {}) {
    g.note(t, s, note, instr, vol >= 0 ? FxSet{FxId::Volume, vol > 50 ? 50 : vol} : FxSet{}, b);
}

// Kick 3+3+2 per bar with a pickup; `glitch` rolls the last kick.
void kicks(Gen& g, int b0, bool glitch) {
    const int s[4] = {0, 3, 6, 11};
    for (int k = 0; k < 4; ++k) hit(g, kTKick, b0 + s[k], nKick, kDrums, k == 3 ? 45 : -1);
    if (glitch) hit(g, kTKick, b0 + 14, nKick, kDrums, 40, {FxId::Roll, roll(0, 3)});   // 4 hits
}
// Backbeat snare with ghost notes; `ratchet` adds a rolled fill at the bar end.
void snares(Gen& g, int b0, bool ratchet) {
    hit(g, kTSnare, b0 + 4, nSnare, kDrums);
    hit(g, kTSnare, b0 + 12, nSnare, kDrums);
    hit(g, kTSnare, b0 + 7, nSnare, kDrums, 20, {FxId::MicroMove, 30});
    hit(g, kTSnare, b0 + 10, nSnare, kDrums, 15, {FxId::Chance, 50});
    if (ratchet) hit(g, kTSnare, b0 + 15, nSnare, kDrums, 35, {FxId::Roll, roll(2, 5)});   // V-roll, 8 hits
}
// 16th hats on Chance, open hat on the off-beat, micro-moved shuffle.
void hats(Gen& g, int b0) {
    for (int s = 0; s < kBar; ++s) {
        if (s % 4 == 2) {
            hit(g, kTHat, b0 + s, nHatO, kDrums, 30);
            continue;
        }
        const int vol = s % 2 ? 18 : 35;
        hit(g, kTHat, b0 + s, nHatC, kDrums, vol, s % 2 ? FxSet{FxId::MicroMove, 25} : FxSet{FxId::Chance, 80});
    }
}
// Clap / pitched toms on chance: the glitch layer.
void percs(Gen& g, int b0, int bar) {
    hit(g, kTPerc, b0 + 2, nClap, kDrums, 30, {FxId::Chance, 60});
    hit(g, kTPerc, b0 + 9, 48 + 3 * bar, kDrums, 40, {FxId::Tune, 24 + 7});   // tom +7 st
    hit(g, kTPerc, b0 + 13, 45 + 2 * bar, kDrums, 30, {FxId::Roll, roll(1, 2)});
}
// Acid bar: 16ths with slides (held notes), Random Note on some steps.
void acidBar(Gen& g, int b0, int bar, bool wild) {
    const int r = kAcidRoots[bar % 4];
    const int nt[16] = {r, r, r + 12, r, r + 3, r + 15, r, r + 7, r, r + 12, r + 10, r, r + 3, r + 12, r + 5, r + 7};
    for (int s = 0; s < kBar; ++s) {
        if (s == 5 || s == 13) continue;   // rests: the line breathes
        FxSet b{};
        if (wild && (s == 6 || s == 14)) b = {FxId::RandomNote, 12};
        else if (wild && s == 9) b = {FxId::Tune, 24 + 12};
        hit(g, kTAcid, b0 + s, nt[s], kAcid, s % 4 == 0 ? -1 : 40, b);
    }
}
void stab(Gen& g, int s, int bar, int vol = -1) {
    for (int k = 0; k < 3; ++k) hit(g, kTG1 + k, s, kStabs[bar % 4][k], kGlass, vol);
}
void stabsOff(Gen& g, int s) {
    for (int k = 0; k < 3; ++k) g.off(kTG1 + k, s);
}
void macroAt(Gen& g, int s, int macro, int v, int instr) {
    if (Step* st = g.step(kTLane, s)) st->instrument = uint8_t(instr);
    g.fx(kTLane, s, 0, macro == 0 ? FxId::MidiCcA : FxId::MidiCcB, v);
}

// 1 "Glass": the stabs alone, displaced, with hats.
void glassPattern(Gen& g) {
    g.beginPattern(1, 64, "Glass");
    for (int bar = 0; bar < 4; ++bar) {
        stab(g, bar * kBar + (bar % 2 ? 3 : 0), bar);
        stab(g, bar * kBar + 10, bar, 30);
        hats(g, bar * kBar);
    }
    stabsOff(g, 60);
    g.endPattern();
}

// 2 "Break": kick, snare, hats, percs and the acid line.
void breakPattern(Gen& g) {
    g.beginPattern(2, 64, "Break");
    for (int bar = 0; bar < 4; ++bar) {
        const int b0 = bar * kBar;
        kicks(g, b0, bar == 3);
        snares(g, b0, bar % 2 == 1);
        hats(g, b0);
        percs(g, b0, bar);
        acidBar(g, b0, bar, false);
    }
    g.off(kTAcid, 63);
    g.endPattern();
}

// 3 "Acid": the acid goes wild (Random Note, Tune), its cutoff and
// resonance macros sweep; stabs on the downbeats.
void acidPattern(Gen& g) {
    g.beginPattern(3, 64, "Acid");
    for (int bar = 0; bar < 4; ++bar) {
        const int b0 = bar * kBar;
        kicks(g, b0, bar == 1 || bar == 3);
        snares(g, b0, true);
        hats(g, b0);
        acidBar(g, b0, bar, true);
        stab(g, b0, bar, 35);
    }
    for (int s = 0; s < 48; ++s) macroAt(g, s, 0, 63 + (s < 24 ? s * 2 : (47 - s) * 2), kAcid);
    for (int s = 48; s < 63; ++s) macroAt(g, s, 1, 63 + (s - 48) * 4, kAcid);
    macroAt(g, 63, 1, 63, kAcid);
    stabsOff(g, 58);
    g.off(kTAcid, 63);
    g.endPattern();
}

// 4 "Glitch": half-time kick, snare rolls that speed up, rolled stabs,
// pitched toms everywhere.
void glitchPattern(Gen& g) {
    g.beginPattern(4, 64, "Glitch");
    for (int bar = 0; bar < 4; ++bar) {
        const int b0 = bar * kBar;
        hit(g, kTKick, b0, nKick, kDrums);
        hit(g, kTKick, b0 + 10, nKick, kDrums, 45, {FxId::MicroMove, 50});
        hit(g, kTSnare, b0 + 8, nSnare, kDrums, -1, {FxId::Roll, roll(0, 2 + bar)});   // 3, 4, 6, 8 hits
        for (int s = 0; s < kBar; s += 3) hit(g, kTPerc, b0 + s, 43 + s, kDrums, 25, {FxId::Chance, 70});
        hit(g, kTG1, b0 + 4, kStabs[bar][0], kGlass, 30, {FxId::Roll, roll(1, 4)});
        hit(g, kTHat, b0 + 12, nHatO, kDrums, 40, {FxId::Roll, roll(5, 5)});   // random roll
    }
    g.off(kTG1, 62);
    g.endPattern();
}

// 5 "Outro": one stab chord and a last kick into the room and the delay.
void outroPattern(Gen& g) {
    g.beginPattern(5, 32, "Outro");
    stab(g, 0, 0);
    hit(g, kTKick, 0, nKick, kDrums);
    hit(g, kTSnare, 6, nSnare, kDrums, 30, {FxId::Roll, roll(2, 6)});
    stabsOff(g, 14);
    g.endPattern();
}

bool idmSong(Gen& g) {
    g.tempo = kTempoIdm;
    if (!g.begin(false)) return false;
    setupPatches(g);
    if (!g.ok) return false;
    trackNames(g, {"Kick", "Snare", "Hats", "Perc", "Acid", "Glass 1", "Glass 2", "Glass 3", "Macros"});
    glassPattern(g);
    breakPattern(g);
    acidPattern(g);
    glitchPattern(g);
    outroPattern(g);
    g.playlist({1, 2, 2, 3, 4, 3, 2, 5});
    idmMixer(g);
    return g.ok;
}

}  // namespace idm
