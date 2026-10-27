// Song 19 "OT Test 12 - Stems" (TESTSONGS.md; docs/PLAN_SD_STREAMING_P1C_P2.md
// C12). Included by testsongs.cpp inside its anonymous namespace.
//
// The SD streaming test project: four long stereo stems (GenOptions::
// stemSeconds, default 30 s; `make testsongs STEMS_SECONDS=180` for the long
// hardware run) and four short drums. The stems together are larger than the
// sample memory (4 x 30 s stereo = 21 MB, 4 x 180 s = 127 MB), so the project
// opens only with Config > Stream from card on (All, or Auto with the stems
// over the threshold); the drums are far below every threshold and stay in
// RAM.
//
// The stems are chord pads with slow filter sweeps and slow stereo movement:
// continuous, smooth material in which a dropped or repeated chunk is a clear
// gap or jump. Stem A and B are OneShot, C and D ForwardLoop with the loop
// start far past the resident head (kStreamHeadFrames), so the loop head is
// resident too and every wrap is served from it.
//
// The stems are written straight into the Workspace as planar stereo .pti
// files (they cannot pass through the sample memory) and the instruments are
// set up header-only; Save As copies them into the project. No source WAV is
// written for a stem (only for the drums).
//
// Song (patterns of 64 steps at 124 BPM, 7.74 s; n = patterns per stem
// length): "Stems in" (all four stems start, light drums) + n-1 x "Hold",
// ceil(n/2) x "Groove" (the loops wrap, A / B have ended), ceil(n/2) x
// "Restarts" (A and B retriggered every pattern while playing), "Both again"
// (A and B from the start with both loops: four streams for a whole stem
// length) + n-1 x "Groove", "Outro" (note-offs). 30 s stems: 13 patterns,
// 101 s; 180 s stems: 73 patterns, 9.4 min.

namespace stems {

constexpr int kDefaultSeconds = 30, kMinSeconds = 10, kMaxSeconds = 600;
enum : int { kIA = 0, kIB, kIC, kID, kIKick, kISnare, kIHat, kIClap };
enum : int { kTA = 0, kTB, kTC, kTD, kTKick, kTSnare, kTHat, kTClap };

int clampSeconds(int s) { return s < kMinSeconds ? kMinSeconds : (s > kMaxSeconds ? kMaxSeconds : s); }
double patternSeconds() { return 64.0 * 60.0 / kTempo / 4.0; }
int patternsPerStem(int seconds) { return int(std::ceil(double(clampSeconds(seconds)) / patternSeconds())); }

// Song order (pattern numbers) for `seconds`-long stems.
std::vector<int> order(int seconds) {
    const int n = patternsPerStem(seconds), h = (n + 1) / 2;
    std::vector<int> o;
    o.push_back(1);
    for (int i = 1; i < n; ++i) o.push_back(2);
    for (int i = 0; i < h; ++i) o.push_back(3);
    for (int i = 0; i < h; ++i) o.push_back(4);
    o.push_back(5);
    for (int i = 1; i < n; ++i) o.push_back(3);
    o.push_back(6);
    return o;
}
[[maybe_unused]] double songSeconds(int seconds) { return double(order(seconds).size()) * patternSeconds(); }

// One stem: per bar of the Am - F - C - G cycle a chord (or one held chord),
// two detuned saws per note (left / right), a resonant low-pass whose cutoff
// sweeps slowly between cutLo and cutHi, and a slow auto-pan.
struct StemSpec {
    int chords[4][4];      // MIDI notes per bar (0 = none); follows = false: chords[0] held
    bool follows;
    double cutLo, cutHi;   // Hz
    double sweepSec, sweepPhase;
    double damp;           // SVF damping (lower = more resonance)
    double detuneCents;
    double panSec, panDepth;
    uint32_t seed;
};

double midiHz(int n) { return 440.0 * std::pow(2.0, (n - 69) / 12.0); }

void renderStem(const StemSpec& sp, int n, syn::Buf& l, syn::Buf& r) {
    using namespace syn;
    l.assign(size_t(n), 0.0f);
    r.assign(size_t(n), 0.0f);
    const int barFrames = frames(4.0 * 60.0 / kTempo);
    const int xf = frames(0.12);   // chord change crossfade
    Saw osc[2][4][2];              // bank (bar parity), note, channel
    Rng rng(sp.seed);
    for (auto& b : osc)
        for (auto& nn : b)
            for (Saw& s : nn) s.ph = rng.uni();
    int active[2][4] = {};
    auto setBank = [&](int bank, int bar) {
        const int* c = sp.chords[sp.follows ? bar % 4 : 0];
        for (int k = 0; k < 4; ++k) {
            active[bank][k] = c[k];
            if (!c[k]) continue;
            const double hz = midiHz(c[k]);
            osc[bank][k][0].freq(hz * std::pow(2.0, -sp.detuneCents / 1200.0));
            osc[bank][k][1].freq(hz * std::pow(2.0, sp.detuneCents / 1200.0));
        }
    };
    setBank(0, 0);
    Svf f[2];
    int bar = 0;
    for (int i = 0; i < n; ++i) {
        if (i / barFrames != bar) {
            bar = i / barFrames;
            setBank(bar & 1, bar);
        }
        const double t = double(i) / kRate;
        if ((i & 15) == 0) {
            const double sw = 0.5 - 0.5 * std::cos(2.0 * kPi * (t / sp.sweepSec + sp.sweepPhase));
            const double cut = sp.cutLo * std::pow(sp.cutHi / sp.cutLo, sw);
            f[0].set(cut, sp.damp);
            f[1].set(cut * 1.06, sp.damp);
        }
        const int pos = i - bar * barFrames;
        const double gIn = (bar == 0 || pos >= xf) ? 1.0 : double(pos) / xf;
        double s[2] = {0, 0};
        for (int b = 0; b < 2; ++b) {
            const double g = b == (bar & 1) ? gIn : 1.0 - gIn;
            if (g <= 0.0) continue;
            for (int k = 0; k < 4; ++k) {
                if (!active[b][k]) continue;
                s[0] += osc[b][k][0].next() * g;
                s[1] += osc[b][k][1].next() * g;
            }
        }
        const double pan = sp.panDepth * std::sin(2.0 * kPi * t / sp.panSec);
        double lo, bp, hi;
        f[0].run(s[0], lo, bp, hi);
        l[size_t(i)] = float(lo * (1.0 - pan));
        f[1].run(s[1], lo, bp, hi);
        r[size_t(i)] = float(lo * (1.0 + pan));
    }
    const int fade = frames(0.5);
    for (int i = 0; i < fade && i < n; ++i) {
        l[size_t(i)] *= float(i) / fade;
        r[size_t(i)] *= float(i) / fade;
    }
}

// Writes Workspace/instruments/instrument_NN.pti (planar stereo) and sets up
// the instrument header-only (no sample memory). loopStart = 0: OneShot.
bool addStem(Gen& g, int slot, const char* name, const syn::Buf& lf, const syn::Buf& rf, PlayMode mode,
             double loopStartFrac) {
    using namespace syn;
    const uint32_t n = uint32_t(lf.size());
    Buf l = lf, r = rf;
    Instrument ins;
    ins.setDefault();
    std::snprintf(ins.name, sizeof(ins.name), "%s", name);
    ins.sampleType = uint8_t(SampleType::WaveFile);
    ins.stereo = true;
    ins.lengthFrames = n;
    ins.playMode = uint8_t(mode);
    ins.startPoint = 0;
    ins.endPoint = 65535;
    ins.loopPoint2 = 65534;
    if (mode == PlayMode::ForwardLoop) {
        ins.loopPoint1 = Gen::point(n, uint32_t(loopStartFrac * n));
        // The voice's loop frames (voice.cpp pointToFrames): crossfade the
        // loop end into the frames before the loop start.
        const int ls = int(uint64_t(n) * ins.loopPoint1 / 65535u), le = int(uint64_t(n) * ins.loopPoint2 / 65535u);
        crossfadeLoop(l, ls, le, frames(1.0));
        crossfadeLoop(r, ls, le, frames(1.0));
    } else {
        declick(l, 32, frames(0.5));
        declick(r, 32, frames(0.5));
    }
    ins.env[kAutoVolume].enabled = 1;
    ins.env[kAutoVolume].attackMs = 0;
    ins.env[kAutoVolume].sustain = 1.0f;
    ins.env[kAutoVolume].releaseMs = 700;
    std::vector<int16_t> l16, r16;
    toI16Stereo(l, r, l16, r16, 0.45);
    std::vector<int16_t> planar(l16);
    planar.insert(planar.end(), r16.begin(), r16.end());
    char path[200];
    if (!workspaceInstrumentPath(path, sizeof(path), kWorkspace, slot + 1) ||
        saveInstrumentFile(g.fs, path, ins, planar.data()) != FmtStatus::Ok) {
        g.fail("write stem .pti");
        return false;
    }
    g.app.instruments[slot] = ins;
    g.app.instrPresent[slot] = true;
    g.app.markInstrumentDirty(slot);   // header only: the file is written
    return true;
}

// Drums on tracks 5-8: kick, snare, hats, clap. light = kick and hats only.
void drums(Gen& g, bool light) {
    for (int s = 0; s < 64; s += light ? 16 : 4) g.note(kTKick, s, 60, kIKick, {FxId::Volume, vp(light ? 60 : 80)});
    for (int s = 2; s < 64; s += light ? 4 : 2)
        g.note(kTHat, s, 60, kIHat, {FxId::Volume, vp((s % 4 == 2) ? 45 : 25)});
    if (light) return;
    for (int b = 0; b < 4; ++b) {
        g.note(kTSnare, b * kBar + 4, 60, kISnare, {FxId::Volume, vp(60)});
        g.note(kTSnare, b * kBar + 12, 60, kISnare, {FxId::Volume, vp(60)});
    }
    g.note(kTClap, 3 * kBar + 12, 60, kIClap, {FxId::Volume, vp(50)});
}

bool stemSong(Gen& g, int seconds) {
    using namespace syn;
    seconds = clampSeconds(seconds);
    if (!g.begin()) return false;
    // Am F C G (A minor), voiced around middle C; follows the song's bars.
    const StemSpec specA = {{{57, 60, 64, 69}, {53, 57, 60, 65}, {55, 60, 64, 67}, {55, 59, 62, 67}},
                            true, 260.0, 2600.0, 12.0, 0.0, 0.55, 7.0, 9.0, 0.25, 101};
    // An octave up, brighter, faster sweep, wider movement.
    const StemSpec specB = {{{69, 72, 76, 0}, {69, 72, 77, 0}, {67, 72, 76, 0}, {67, 71, 74, 0}},
                            true, 500.0, 5200.0, 7.5, 0.3, 0.45, 11.0, 5.0, 0.45, 202};
    // Held A drone (A2 E3 A3 B3), dark, slow sweep.
    const StemSpec specC = {{{45, 52, 57, 59}, {}, {}, {}}, false, 150.0, 1400.0, 9.0, 0.6, 0.5, 5.0, 13.0, 0.2, 303};
    // Held high fifths (E5 A5 B5), airy, quick sweep and auto-pan.
    const StemSpec specD = {{{76, 81, 83, 0}, {}, {}, {}}, false, 900.0, 7000.0, 6.0, 0.15, 0.6, 9.0, 3.0, 0.6, 404};
    const int n = seconds * kRate;
    Buf l, r;
    renderStem(specA, n, l, r);
    if (!addStem(g, kIA, "Stem Pad A", l, r, PlayMode::OneShot, 0.0)) return false;
    renderStem(specB, n, l, r);
    if (!addStem(g, kIB, "Stem Pad B", l, r, PlayMode::OneShot, 0.0)) return false;
    renderStem(specC, n, l, r);
    if (!addStem(g, kIC, "Stem Drone C", l, r, PlayMode::ForwardLoop, 0.3)) return false;
    renderStem(specD, n, l, r);
    if (!addStem(g, kID, "Stem Air D", l, r, PlayMode::ForwardLoop, 0.45)) return false;
    // Short RAM drums (far below every stream threshold).
    g.addMono(kIKick, "Stem Kick", toI16(kick()));
    g.addMono(kISnare, "Stem Snare", toI16(snare()));
    g.addMono(kIHat, "Stem Hat", toI16(hat(0.04, 0.1)));
    g.addMono(kIClap, "Stem Clap", toI16(clap()));
    if (!g.ok) return false;
    g.ins(kIHat).panning = 62;
    g.ins(kIClap).reverbSend = 25;
    g.ins(kIB).reverbSend = 20;
    g.ins(kID).reverbSend = 30;
    trackNames(g, {"Stem A", "Stem B", "Drone C", "Air D", "Kick", "Snare", "Hat", "Clap"});

    g.beginPattern(1, 64, "Stems in");   // all four start, staggered
    g.note(kTA, 0, 60, kIA);
    g.note(kTC, 0, 60, kIC);
    g.note(kTB, 16, 60, kIB);
    g.note(kTD, 32, 60, kID);
    drums(g, true);
    g.endPattern();
    g.beginPattern(2, 64, "Hold");
    drums(g, true);
    g.endPattern();
    g.beginPattern(3, 64, "Groove");
    drums(g, false);
    g.endPattern();
    g.beginPattern(4, 64, "Restarts");   // A / B restart while playing
    g.note(kTA, 0, 60, kIA);
    g.note(kTB, 32, 60, kIB);
    drums(g, false);
    g.endPattern();
    g.beginPattern(5, 64, "Both again");
    g.note(kTA, 0, 60, kIA);
    g.note(kTB, 0, 60, kIB);
    drums(g, false);
    g.endPattern();
    g.beginPattern(6, 64, "Outro");
    for (int t = kTA; t <= kTD; ++t) g.off(t, 32);
    g.note(kTKick, 0, 60, kIKick, {FxId::Volume, vp(60)});
    g.note(kTKick, 16, 60, kIKick, {FxId::Volume, vp(40)});
    g.endPattern();

    const std::vector<int> o = order(seconds);
    std::memset(g.app.project.playlist, 0, sizeof(g.app.project.playlist));
    for (size_t i = 0; i < o.size() && i < size_t(kSongSlots); ++i) g.app.project.playlist[i] = uint8_t(o[i]);
    g.app.project.playlistPos = 0;
    g.app.markProjectDirty();
    return g.ok;
}

}  // namespace stems
