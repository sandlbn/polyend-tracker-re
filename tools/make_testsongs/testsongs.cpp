// OpenTracker 2.x test songs (testsongs.h). Songs 1-5: A minor,
// Am - F - C - G, one chord per bar, 16 steps per bar, 124 BPM. Songs 6-12:
// BLAST GRID remixes (bgsongs.inc, bg_data.h). Song 13 "OT Test 06": the
// PERC engine alone (percsong.inc). Song 14 "OT Test 07": the VAP engine
// alone (vapsong.inc). Song 16 "OT Test 09": the FAT engine alone
// (fatsong.inc). Song 17 "OT Test 10": the WTFM engine alone (wtfmsong.inc).
// Song 18 "OT Test 11 - IDM": PERC, ACD and WTFM with dense sequencer FX (idmsong.inc).
// Song 19 "OT Test 12 - Stems": long stereo stems for SD streaming (stemsong.inc).
#include "testsongs.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "app/app.h"
#include "app/perf_presets.h"
#include "app/project_fields.h"
#include "audio/audio_tables.h"
#include "audio/engines/acd.h"
#include "audio/engines/fat.h"
#include "audio/engines/perc.h"
#include "audio/engines/vap.h"
#include "audio/engines/wtfm.h"
#include "audio/synth_params.h"
#include "features.h"
#include "formats/file_io.h"
#include "formats/patch_pb.h"
#include "formats/pattern_mtp.h"
#include "formats/wav.h"
#include "model/fx.h"
#include "sequencer/seq_tables.h"
#include "synth.h"
#include "bg_data.h"

using namespace ot;

namespace ts {

namespace {

const SongInfo kSongs[kSongCount] = {
    {SongId::TwelveTracks, "OT Test 01 - Twelve Tracks", "stock 1.9.2 / 1.9.3", true},
    {SongId::SixteenTracks, "OT Test 02 - Sixteen Tracks", "M1 (16 tracks, Tracker Mini format)", kHas16Tracks},
    {SongId::LoadStress, "OT Test 03 - Load Stress", "M0 / M1", true},
    {SongId::Stereo, "OT Test 04 - Stereo", "M2 (stereo playback)", kHasStereo},
    {SongId::Synths, "OT Test 05 - Synths", "M3 (synth engines) + M1", kHasSynths && kHas16Tracks},
    {SongId::PercOnly, "OT Test 06 - PERC Only", "M3 (PERC engine) + M1", kHasSynths && kHas16Tracks},
    {SongId::VapOnly, "OT Test 07 - VAP Only", "M3 (VAP engine) + M1", kHasSynths && kHas16Tracks},
    {SongId::MeterCheck, "OT Test 08 - Meter Check", "stock 1.9.2 / 1.9.3", true},
    {SongId::FatOnly, "OT Test 09 - FAT Only", "2.1 (FAT engine) + M1", kHasSynths && kHas16Tracks},
    {SongId::WtfmOnly, "OT Test 10 - WTFM", "2.2 (WTFM engine) + M1", kHasSynths && kHas16Tracks},
    {SongId::Idm, "OT Test 11 - IDM", "2.2 (all synth engines) + M1", kHasSynths && kHas16Tracks},
    {SongId::Stems, "OT Test 12 - Stems", "2.x SD streaming (Stream from card on)", true},
    {SongId::BgTitle, "BG Title", "M3 (synth engines) + M1", kHasSynths && kHas16Tracks},
    {SongId::BgWorld1, "BG World 1", "M3 (synth engines) + M1", kHasSynths && kHas16Tracks},
    {SongId::BgWorld4, "BG World 4", "M3 (synth engines) + M1", kHasSynths && kHas16Tracks},
    {SongId::BgBattle, "BG Battle", "M3 (synth engines) + M1", kHasSynths && kHas16Tracks},
    {SongId::BgBattleFm, "BG Battle FM", "M3 (synth engines) + M1", kHasSynths && kHas16Tracks},
    {SongId::BgBoss, "BG Boss", "M3 (synth engines) + M1", kHasSynths && kHas16Tracks},
    {SongId::BgFinalBoss, "BG Final Boss", "M3 (synth engines) + M1", kHasSynths && kHas16Tracks},
};

constexpr float kTempo = 124.0f;
constexpr int kBar = 16;

// Volume FX on a sample track is the instrument level law VOL[] (spec 04
// §4.2: 50 = 0 dB, 0.48 dB per step, 100 = +24 dB), not a percentage: the
// step volume for `pct` % of full level (100 % = 50 = 0 dB). Synth and MIDI
// tracks take Volume as a velocity 0..100 and use the value directly.
int vp(int pct) {
    if (pct <= 0) return 0;
    const int v = int(std::lround(50.0 + 20.0 * std::log10(pct / 100.0) / 0.48));
    return v < 2 ? 2 : (v > 50 ? 50 : v);
}
constexpr int kMidi = kFirstMidiInstrument;   // instrument 48 = MIDI channel 1

// Track layout (0-based).
enum : int { kTKick, kTSnare, kTHats, kTBass, kTLead, kTPad, kTPerc, kTFx, kTMidi9, kTMidi10, kTMidi11, kTMidi12,
             kTExt13, kTExt14, kTExt15, kTExt16 };

// Instrument slots of songs 1, 2 and 5 (0-based).
enum : int { kIKick, kISnare, kIHatC, kIBass, kILead, kIPad, kIPerc, kIRiser, kIHatO, kIImpact, kIClap };

// Chord per bar: root (octave 3), MIDI Chord FX value (spec 03 §3.5).
struct Chord {
    int root;
    int chordFx;
};
constexpr Chord kProg[4] = {{57, 4}, {53, 5}, {60, 5}, {55, 5}};   // Am F C G

struct FxSet {
    FxId id = FxId::None;
    int v = 0;
};

class Gen {
public:
    Gen(App& a, IStorage& f, const GenOptions& o, const char* sub) : app(a), fs(f), opt(o) {
        std::snprintf(dir, sizeof(dir), "%s/%s", o.samplesRoot, sub);
    }

    App& app;
    IStorage& fs;
    const GenOptions& opt;
    char dir[160];
    float tempo = kTempo;   // project tempo set by finish()
    bool ok = true;
    std::string err;

    void fail(const char* what) {
        if (ok) err = what;
        ok = false;
    }

    // samples = false: a song without sample instruments (no source folder).
    bool begin(bool samples = true) {
        if (!app.newProject()) {
            fail("newProject");
            return false;
        }
        if (samples) mkdirs(dir);
        return ok;
    }

    void mkdirs(const char* path) {
        char p[200];
        std::snprintf(p, sizeof(p), "%s", path);
        for (char* s = p; *s; ++s)
            if (*s == '/') {
                *s = 0;
                if (!fs.exists(p)) fs.mkdir(p);
                *s = '/';
            }
        if (!fs.exists(p) && !fs.mkdir(p)) fail("mkdir");
    }

    // ---- samples ----------------------------------------------------------
    // Writes <dir>/<name>.wav (the instrument takes the file name, spec 06 §5.4)
    // and imports it into `slot`.
    bool addMono(int slot, const char* name, const std::vector<int16_t>& s) {
        char path[220];
        std::snprintf(path, sizeof(path), "%s/%s.wav", dir, name);
        if (!app.writeWav(path, s.data(), uint32_t(s.size()))) {
            fail("write wav");
            return false;
        }
        if (!app.importSample(path, slot)) {
            fail("importSample");
            return false;
        }
        return true;
    }

    // Stereo source WAV (interleaved, for the M2 import test) + instrument.
    // TODO(M2): import the WAV in stereo mode once the importer keeps both
    // channels; until then the stock importer takes the left channel and the
    // planar stereo .pti is written over the saved instrument after Save As
    // (saveStereoInstruments), which stock firmware loads as left-only.
    bool addStereo(int slot, const char* name, const std::vector<int16_t>& l, const std::vector<int16_t>& r) {
        char path[220];
        std::snprintf(path, sizeof(path), "%s/%s.wav", dir, name);
        if (fs.exists(path)) fs.remove(path);
        WavWriter w;
        bool good = w.open(fs, path, 2);
        std::vector<int16_t> il(2 * l.size());
        for (size_t i = 0; i < l.size(); ++i) {
            il[2 * i] = l[i];
            il[2 * i + 1] = r[i];
        }
        for (size_t i = 0; good && i < l.size(); i += 4096) {
            const uint32_t n = uint32_t(l.size() - i < 4096 ? l.size() - i : 4096);
            good = w.write(il.data() + 2 * i, n);
        }
        if (!w.close() || !good) {
            fail("write stereo wav");
            return false;
        }
        if (!app.importSample(path, slot)) {
            fail("importSample (stereo)");
            return false;
        }
        Stereo st;
        st.slot = slot;
        st.planar = l;
        st.planar.insert(st.planar.end(), r.begin(), r.end());
        stereo.push_back(st);
        return true;
    }

    Instrument& ins(int slot) {
        app.markInstrumentDirty(slot);
        return app.instruments[slot];
    }

    static uint16_t point(uint32_t len, uint32_t frame) {
        if (!len) return 0;
        const uint64_t v = (uint64_t(frame) * 65535u + len / 2) / len;
        return uint16_t(v > 65535 ? 65535 : v);
    }
    void loop(int slot, PlayMode mode, uint32_t loopStartFrame) {
        Instrument& i = ins(slot);
        i.playMode = uint8_t(mode);
        i.startPoint = 0;
        i.loopPoint1 = point(i.lengthFrames, loopStartFrame);
        i.loopPoint2 = 65534;
        i.endPoint = 65535;
    }
    void slices(int slot, PlayMode mode, const std::vector<uint32_t>& starts) {
        Instrument& i = ins(slot);
        i.playMode = uint8_t(mode);
        i.sliceCount = uint8_t(starts.size());
        for (size_t k = 0; k < starts.size() && k < size_t(kSliceCount); ++k) i.slices[k] = point(i.lengthFrames, starts[k]);
        i.selectedSlice = 0;
    }
    void release(int slot, uint16_t ms, uint16_t attack = 0) {
        Instrument& i = ins(slot);
        i.env[kAutoVolume].enabled = 1;
        i.env[kAutoVolume].attackMs = attack;
        i.env[kAutoVolume].sustain = 1.0f;
        i.env[kAutoVolume].releaseMs = ms;
    }

    struct Stereo {
        int slot;
        std::vector<int16_t> planar;
    };
    std::vector<Stereo> stereo;

    // ---- patterns ---------------------------------------------------------
    Pattern* pat = nullptr;
    int len = 64;

    void beginPattern(int num, int steps, const char* name) {
        app.selectPattern(num);
        pat = &app.pattern();
        pat->setDefault();
        len = steps;
        for (int t = 0; t < kTrackCount; ++t) pat->tracks[t].lastStep = uint8_t(steps - 1);
        app.names.set(num, name);
    }
    void endPattern() {
        app.patternEdited();
        pat = nullptr;
    }
    // Tracks beyond this build's kTrackCount (13-16 before M1) are skipped.
    Step* step(int t, int s) {
        if (!pat || t < 0 || t >= kTrackCount || s < 0 || s >= len) return nullptr;
        return &pat->tracks[t].steps[s];
    }
    static uint8_t fxValue(FxId id, int v) {
        // spec 02 §4.3 range, narrowed where the stock engine's range
        // (spec 03 §3 table) is smaller: LFO rates and bit depth.
        const FxInfo& fi = fxInfo(id);
        int lo = fi.minValue, hi = fi.maxValue;
        switch (id) {
            case FxId::VolumeLfo: hi = 23; break;
            case FxId::PanningLfo:
            case FxId::FinetuneLfo:
            case FxId::FilterLfo:
            case FxId::PositionLfo: hi = 28; break;
            case FxId::BitDepth: lo = 4; break;
            default: break;
        }
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        return uint8_t(v);
    }
    void fx(int t, int s, int col, FxId id, int v) {
        if (Step* st = step(t, s)) {
            st->fx[col].type = uint8_t(id);
            st->fx[col].value = fxValue(id, v);
        }
    }
    void note(int t, int s, int n, int instr, FxSet a = {}, FxSet b = {}) {
        Step* st = step(t, s);
        if (!st) return;
        st->note = int8_t(n);
        st->instrument = uint8_t(instr);
        if (a.id != FxId::None) fx(t, s, 0, a.id, a.v);
        if (b.id != FxId::None) fx(t, s, 1, b.id, b.v);
    }
    void off(int t, int s) {
        if (Step* st = step(t, s)) st->note = kNoteOff;
    }

    // ---- project ----------------------------------------------------------
    void playlist(std::initializer_list<int> pats) {
        std::memset(app.project.playlist, 0, sizeof(app.project.playlist));
        int i = 0;
        for (int p : pats)
            if (i < kSongSlots) app.project.playlist[i++] = uint8_t(p);
        app.project.playlistPos = 0;
        app.markProjectDirty();
    }
    void playlistRange(int slots, int patterns) {
        std::memset(app.project.playlist, 0, sizeof(app.project.playlist));
        for (int i = 0; i < slots && i < kSongSlots; ++i) app.project.playlist[i] = uint8_t(1 + i % patterns);
        app.project.playlistPos = 0;
        app.markProjectDirty();
    }

    bool finish(const char* name) {
        if (!ok) return false;
        app.selectPattern(1);
        app.project.setCurrentPattern(1);
        app.project.setTempo(tempo);
        app.setCurrentInstrument(0);
        app.markProjectDirty();
        app.markNamesDirty();
        app.applySequencerSettings();
        if (!app.saveProjectAs(name)) {
            fail("saveProjectAs");
            return false;
        }
        app.autosave();
        saveStereoInstruments(name);
        return ok;
    }

    void saveStereoInstruments(const char* name) {
        char base[200], path[300];
        std::snprintf(base, sizeof(base), "%s/%s", kProjectsDir, name);
        for (const Stereo& s : stereo) {
            Instrument i = app.instruments[s.slot];
            if (!savedInstrumentPath(path, sizeof(path), base, s.slot + 1, i.name)) {
                fail("stereo instrument path");
                return;
            }
            i.stereo = true;
            i.lengthFrames = uint32_t(s.planar.size() / 2);
            if (saveInstrumentFile(fs, path, i, s.planar.data()) != FmtStatus::Ok) fail("write stereo .pti");
        }
    }
};

// ---------------------------------------------------------------------------
// Shared song parts (songs 1, 2, 5).

void midiSetup(Gen& g) {
    // Records 0..15 = instruments 48..63 = channels 1..16. CC A..F:
    // cutoff, resonance, volume, pan, reverb, mod wheel.
    for (int i = 0; i < kMidiInstrumentCount; ++i) {
        MidiInstrument m = g.app.project.midiInstrument(i);
        m.velocity = uint8_t(100 + (i % 4) * 9);
        const uint8_t cc[6] = {74, 71, 7, 10, 91, 1};
        for (int k = 0; k < 6; ++k) m.cc[k] = cc[k];
        g.app.project.setMidiInstrument(i, m);
    }
}

void trackNames(Gen& g, std::initializer_list<const char*> names) {
    int t = 0;
    for (const char* n : names) g.app.project.setTrackName(t++, n);
}

void drumKitSamples(Gen& g) {
    using namespace syn;
    g.addMono(kIKick, "Kick", toI16(kick()));
    g.addMono(kISnare, "Snare", toI16(snare()));
    g.addMono(kIHatC, "Hat Closed", toI16(hat(0.04, 0.1)));
    int ls = 0;
    g.addMono(kIBass, "Saw Bass", toI16(sawTone(kC4, 7, 1600, 1.2, 0.4, ls, 0.6)));
    g.loop(kIBass, PlayMode::ForwardLoop, uint32_t(ls));
    g.release(kIBass, 120);
    g.addMono(kILead, "FM Lead", toI16(fm(kC4, 2.0, 3.0, 0.25, 0.004, 0.9, 1.4, 0.2)));
    Buf l;
    pad(kC4, 3.0, 1.0, l, nullptr, ls);
    g.addMono(kIPad, "Fifth Pad", toI16(l));
    g.loop(kIPad, PlayMode::ForwardLoop, uint32_t(ls));
    g.release(kIPad, 900, 200);
    g.addMono(kIPerc, "FM Perc", toI16(fm(kC4 * 2, 3.5, 4.0, 0.03, 0.0005, 0.09, 0.3)));
    g.addMono(kIRiser, "Noise Riser", toI16(riser(1.9)));
    g.addMono(kIHatO, "Hat Open", toI16(hat(0.25, 0.5, 23)));
    g.addMono(kIImpact, "Impact", toI16(impact()));
    g.addMono(kIClap, "Clap", toI16(clap()));
    if (!g.ok) return;
    // Mixer-ish instrument settings: pan, sends, volume.
    g.ins(kIHatC).panning = 60;
    g.ins(kIHatO).panning = 62;
    g.ins(kIHatO).reverbSend = 15;
    g.ins(kIPerc).panning = 35;
    g.ins(kIPerc).delaySend = 35;
    g.ins(kILead).delaySend = 30;
    g.ins(kILead).reverbSend = 20;
    g.ins(kIPad).reverbSend = 45;
    g.ins(kIPad).volume = 40;
    g.ins(kIRiser).reverbSend = 40;
    g.ins(kIImpact).reverbSend = 60;
    g.ins(kIClap).reverbSend = 25;
    Instrument& b = g.ins(kIBass);
    b.filterEnabled = 1;
    b.filterType = uint8_t(FilterType::LowPass);
    b.cutoff = 0.55f;
    b.resonance = 0.2f;
}

// Mixer (spec 02 §3.3, project_fields.h): track levels (86 = 0 dB),
// global reverb / delay, ping-pong delay at 3/16.
void mixerSetup(Gen& g) {
    Project& p = g.app.project;
    // About 6 dB under the stock defaults: eight full-scale samples at 0 dB
    // would sum far past the master's full scale (the sum clips before the
    // limiter, spec 07 §5.4).
    const uint8_t lv[8] = {72, 66, 54, 70, 60, 56, 56, 50};
    for (int t = 0; t < 8; ++t) p.setU8(pf::kTrackVolume + uint32_t(t), lv[t]);
    p.setU8(pf::kMixReverb, 80);
    p.setU8(pf::kMixDelay, 72);
    p.setU8(pf::kDelayFeedback, 42);
    p.setU16(pf::kDelayTime, uint16_t(60000.0f / kTempo * 0.75f));   // 3/16 note, ms
    p.setU8(pf::kDelayFlags, uint8_t(p.u8(pf::kDelayFlags) | 0x80));   // ping-pong
    p.setF32(pf::kReverbSize, 0.75f);
    p.setF32(pf::kReverbDamp, 0.35f);
    g.app.markProjectDirty();
}

// Performance FX presets in the project (spec 12 §A.2). The Performance
// screen uses them when Config "Performance presets" is set to Project.
void perfSetup(Gen& g) {
    const perf::Source src = perf::projectSource(g.app.project);
    struct Slot {
        uint8_t fx;
        int16_t v[3];
    };
    const Slot slots[perf::kSlots] = {
        {perf::kVolume, {-30, -60, -100}}, {perf::kPanning, {-50, 50, -100}}, {perf::kTune, {12, -12, 7}},
        {perf::kLowPass, {-30, -60, -85}}, {perf::kHighPass, {25, 50, 80}},   {perf::kDelaySend, {30, 60, 100}},
        {perf::kReverbSend, {30, 60, 100}}, {perf::kRepeater, {3, 5, 7}},     {perf::kBitDepth, {-4, -8, -12}},
        {perf::kOverdrive, {30, 60, 100}}, {perf::kVolLfo, {6, 12, 18}},      {perf::kPatternLength, {3, 4, 5}},
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

// Song-1 style pattern. part: 0 intro, 1 groove A, 2 groove B, 3 break, 4 outro.
void corePattern(Gen& g, int part) {
    const bool drums = part == 1 || part == 2 || part == 4;
    for (int bar = 0; bar < 4; ++bar) {
        const int b0 = bar * kBar;
        const Chord ch = kProg[bar];
        // Kick: four on the floor (intro from bar 2, outro fades).
        if (drums || (part == 0 && bar >= 2))
            for (int s = 0; s < kBar; s += 4) {
                const int vol = part == 4 ? 100 - bar * 20 : 100;
                g.note(kTKick, b0 + s, 60, kIKick, {FxId::Volume, vp(vol)});
            }
        if (part == 1 && bar == 3) g.note(kTKick, b0 + 14, 60, kIKick, {FxId::Volume, vp(70)});
        // Snare / clap on 2 and 4; roll fill at the end of groove B and break.
        if (drums) {
            g.note(kTSnare, b0 + 4, 60, kISnare);
            g.note(kTSnare, b0 + 12, 60, part == 2 && bar % 2 ? kIClap : kISnare);
        }
        if ((part == 2 || part == 3) && bar == 3) {
            g.note(kTSnare, b0 + 14, 60, kISnare, {FxId::Roll, 2 * 9 + 5}, {FxId::Volume, vp(60)});   // rising roll
        }
        // Hats: closed 8ths with accents, open hat on the off-beat 'and'.
        if (part != 3)
            for (int s = 0; s < kBar; s += 2) {
                const bool openHat = (s == 6 || s == 14) && part != 0;
                if (openHat)
                    g.note(kTHats, b0 + s, 60, kIHatO, {FxId::Volume, vp(70)});
                else
                    g.note(kTHats, b0 + s, 60, kIHatC, {FxId::Volume, vp(s % 4 ? 80 : 45)},
                           s % 4 ? FxSet{FxId::MicroMove, 12} : FxSet{});   // late off-beats
            }
        if (part == 1 || part == 2)   // ghost 16ths with chance
            for (int s = 1; s < kBar; s += 4) g.note(kTHats, b0 + s, 60, kIHatC, {FxId::Volume, vp(30)}, {FxId::Chance, 50});
        // Bass.
        if (part == 1 || part == 2 || part == 4) {
            const int r = ch.root - 12;
            g.note(kTBass, b0 + 0, r, kIBass);
            g.note(kTBass, b0 + 3, r, kIBass, {FxId::LowPass, 40});
            g.note(kTBass, b0 + 6, r + 12, kIBass, {FxId::Glide, 8});
            g.note(kTBass, b0 + 8, r, kIBass, {FxId::LowPass, 70});
            g.note(kTBass, b0 + 11, r + 7, kIBass);
            g.note(kTBass, b0 + 14, r, kIBass, {FxId::LowPass, 55});
            g.off(kTBass, b0 + 15);
        }
        // Pad: one note per bar.
        g.note(kTPad, b0, ch.root, kIPad, {FxId::ReverbSend, part == 3 ? 70 : 45},
               {FxId::Volume, vp(part == 4 ? 70 - bar * 15 : 70)});
        // Perc: 3+3+2 figure, alternating pan.
        if (part == 1 || part == 2) {
            const int ps[4] = {3, 6, 10, 13};
            for (int k = 0; k < 4; ++k)
                g.note(kTPerc, b0 + ps[k], k % 2 ? 67 : 72, kIPerc, {FxId::Panning, k % 2 ? 20 : 80},
                       {FxId::DelaySend, 40});
        }
        // MIDI 9: chords on ch 1 (MIDI Chord FX), CC lane in column 1.
        g.note(kTMidi9, b0, ch.root, kMidi + 0, {FxId::MidiChord, ch.chordFx});
        // MIDI 10: 8th-note arpeggio on ch 2.
        if (part != 0)
            for (int s = 0; s < kBar; s += 2) {
                const int iv[4] = {0, ch.chordFx == 4 ? 3 : 4, 7, 12};
                g.note(kTMidi10, b0 + s, ch.root + 12 + iv[(s / 2) % 4], kMidi + 1, {FxId::Volume, 60 + (s % 4) * 10});
            }
        // MIDI 11: counter line on ch 3 (half notes).
        g.note(kTMidi11, b0, ch.root + 7, kMidi + 2, {FxId::GateLength, 90});
        g.note(kTMidi11, b0 + 8, ch.root + (bar % 2 ? 5 : 3) + 12, kMidi + 2);
        // MIDI 12: drum notes on ch 4 (36 kick, 38 snare, 42 hat).
        if (drums) {
            for (int s = 0; s < kBar; s += 4) g.note(kTMidi12, b0 + s, s % 8 ? 38 : 36, kMidi + 3, {FxId::Volume, 90});
            for (int s = 2; s < kBar; s += 4) g.note(kTMidi12, b0 + s, 42, kMidi + 3, {FxId::Volume, 50});
        }
        // CC A..F lanes: each MIDI track walks through the six CC letters,
        // one letter per bar, a ramp of four values (column 1, so they sit
        // next to the notes' column-0 FX).
        const FxId ccs[6] = {FxId::MidiCcA, FxId::MidiCcB, FxId::MidiCcC, FxId::MidiCcD, FxId::MidiCcE, FxId::MidiCcF};
        for (int t = 0; t < 4; ++t) {
            const FxId cc = ccs[(t + bar + part * 4) % 6];
            for (int k = 0; k < 4; ++k) {
                const int s = b0 + k * 4 + 1;
                // A CC on an empty step needs the MIDI instrument to resolve
                // the channel: give the step the track's instrument.
                Step* st = g.step(kTMidi9 + t, s);
                if (st && st->note == kNoteEmpty) st->instrument = uint8_t(kMidi + t);
                g.fx(kTMidi9 + t, s, 1, cc, 16 + k * 37);
            }
        }
    }
    // FX track: impact on the first step of groove A, riser into the next part.
    if (part == 1) g.note(kTFx, 0, 60, kIImpact, {FxId::ReverbSend, 60});
    if (part == 0 || part == 3) g.note(kTFx, 3 * kBar, 60, kIRiser, {FxId::HighPass, 20});
    if (part == 4) g.note(kTFx, 2 * kBar, 60, kIImpact, {FxId::ReversePlayback, 1});
    // Lead melody (A minor pentatonic) in groove B and the break.
    if (part == 2 || part == 3) {
        struct N {
            int s, n;
        };
        const N mel[] = {{0, 76}, {3, 74}, {6, 72}, {8, 69}, {12, 72}, {16, 69}, {20, 72}, {22, 74}, {26, 72},
                         {30, 69}, {32, 67}, {35, 69}, {38, 72}, {40, 76}, {44, 74}, {48, 74}, {52, 76},
                         {56, 74}, {58, 72}, {60, 69}};
        for (const N& m : mel)
            g.note(kTLead, m.s, m.n, kILead, {FxId::DelaySend, part == 3 ? 60 : 30},
                   {FxId::Volume, vp(m.s % 8 ? 65 : 85)});
        g.off(kTLead, 63);
    }
}

const char* const kCoreNames[5] = {"Intro", "Groove A", "Groove B", "Break", "Outro"};

bool coreSong(Gen& g, bool extTracks) {
    if (!g.begin()) return false;
    drumKitSamples(g);
    if (!g.ok) return false;
    midiSetup(g);
    trackNames(g, {"Kick", "Snare", "Hats", "Bass", "Lead", "Pad", "Perc", "FX", "Chords", "Arp", "Counter", "Drums"});
    for (int part = 0; part < 5; ++part) {
        g.beginPattern(part + 1, 64, kCoreNames[part]);
        corePattern(g, part);
        if (extTracks) {
            // Tracks 13-16: MIDI channels 5-8 (instruments 52-55), own parts.
            for (int bar = 0; bar < 4 && part != 0; ++bar) {
                const int b0 = bar * kBar;
                const Chord ch = kProg[bar];
                // 13 "Sub": octave bass on ch 5 with pitch bend and CC.
                for (int s = 0; s < kBar; s += 4)
                    g.note(kTExt13, b0 + s, ch.root - 24 + (s == 8 ? 12 : 0), kMidi + 4, {FxId::GateLength, 60},
                           {FxId::MidiCcA, 30 + s * 5});
                g.fx(kTExt13, b0 + 14, 0, FxId::MicroTune, 140);
                g.note(kTExt13, b0 + 14, ch.root - 12, kMidi + 4);
                // 14 "Stabs": off-beat 7th chords on ch 6.
                for (int s = 2; s < kBar; s += 4)
                    g.note(kTExt14, b0 + s, ch.root, kMidi + 5, {FxId::MidiChord, ch.chordFx == 4 ? 14 : 15},
                           {FxId::Volume, 55 + s * 2});
                // 15 "Bells": 16th arp via Arp FX + chord, chance thinning.
                g.note(kTExt15, b0, ch.root + 24, kMidi + 6, {FxId::MidiChord, ch.chordFx}, {FxId::Arp, 1 + 5});   // up, 1 note / step
                g.note(kTExt15, b0 + 8, ch.root + 31, kMidi + 6, {FxId::Chance, 60}, {FxId::MidiCcE, 40 + bar * 20});
                // 16 "Perc2": rimshots / toms on ch 8 with micro-move, random note.
                for (int s = 1; s < kBar; s += 3)
                    g.note(kTExt16, b0 + s, 37 + (s % 4), kMidi + 7, {FxId::MicroMove, 20 + s * 3},
                           {FxId::RandomNote, s % 2 ? 2 : 0});
            }
        }
        g.endPattern();
    }
    g.playlist({1, 2, 3, 2, 3, 4, 2, 3, 5});
    mixerSetup(g);
    perfSetup(g);
    return g.ok;
}

// ---------------------------------------------------------------------------
// Song 2 extras (M1, Tracker Mini format): names and mutes of tracks 13-16
// live in project.mt (0x603 slots 4..7, 0x742[4..7]; spec 21 §3.2), the
// notes of tracks 13-16 in the 0x3030 patterns. The Mini has no levels and
// no Performance pattern overrides for tracks 9-16.

void sixteenExtras(Gen& g) {
    App& a = g.app;
    const char* names[4] = {"Sub", "Stabs", "Bells", "Perc2"};
    for (int t = 0; t < 4; ++t) trk::setName(a, kTExt13 + t, names[t]);
    trk::setMuted(a, kTExt16, true);   // track 16 starts muted
    perf::setTrackPattern(a.project, kTPerc, 3);   // Performance: track 7 plays pattern 3
    a.markProjectDirty();
}

// ---------------------------------------------------------------------------
// Song 3: load stress. 48 instruments of ~2.2 s (about 9.3 MB of the 11.8 MB
// sample memory), every MIDI record, 32 distinct 128-step patterns, every
// track dense with two FX per step, a 160-slot song.

void stressSamples(Gen& g) {
    using namespace syn;
    const double len = 2.2;
    char name[32];
    for (int i = 0; i < kSampleInstrumentCount && g.ok; ++i) {
        const int kind = i % 8;
        const double f = kC4 * std::pow(2.0, ((i * 5) % 12) / 12.0);
        Buf b;
        int ls = 0;
        switch (kind) {
            case 0: b = kick(40 + i, 150 + 2 * i, 0.03, 0.3, len); std::snprintf(name, sizeof(name), "Kick %02d", i + 1); break;
            case 1: b = snare(150 + 3 * i, 0.12 + 0.002 * i, len, uint32_t(i + 3)); std::snprintf(name, sizeof(name), "Snare %02d", i + 1); break;
            case 2: b = hat(0.03 + 0.01 * (i % 5), len, uint32_t(i + 9)); std::snprintf(name, sizeof(name), "Hat %02d", i + 1); break;
            case 3: b = sawTone(f, 6 + i % 9, 900 + 40 * i, len, 0.6, ls, 0.4); std::snprintf(name, sizeof(name), "Bass %02d", i + 1); break;
            case 4: b = fm(f, 1.0 + (i % 4), 2.0 + (i % 3), 0.2, 0.003, 0.7, len, 0.1); std::snprintf(name, sizeof(name), "FM %02d", i + 1); break;
            case 5: pad(f, len, 0.7, b, nullptr, ls, uint32_t(i)); std::snprintf(name, sizeof(name), "Pad %02d", i + 1); break;
            case 6: b = fm(f * 2, 3.5, 4.0, 0.05, 0.0005, 0.25, len); std::snprintf(name, sizeof(name), "Bell %02d", i + 1); break;
            default: b = riser(len, uint32_t(i)); std::snprintf(name, sizeof(name), "Noise %02d", i + 1); break;
        }
        g.addMono(i, name, toI16(b));
        if (!g.ok) break;
        // Play modes across the slots: loops, ping-pong, slices, granular.
        Instrument& in = g.ins(i);
        const uint32_t n = in.lengthFrames;
        switch (kind) {
            case 3: g.loop(i, PlayMode::ForwardLoop, uint32_t(ls)); break;
            case 5: g.loop(i, i % 2 ? PlayMode::PingPongLoop : PlayMode::ForwardLoop, uint32_t(ls)); break;
            case 4:
                if (i % 3 == 0) g.loop(i, PlayMode::BackwardLoop, n / 3);
                break;
            case 7:
                if (i % 2) {
                    in.playMode = uint8_t(PlayMode::Granular);
                    in.grainLength = 2205;
                    in.granularPosition = 20000;
                } else {
                    std::vector<uint32_t> st;
                    for (int k = 0; k < 16; ++k) st.push_back(n * uint32_t(k) / 16);
                    g.slices(i, PlayMode::Slice, st);
                }
                break;
            default: break;
        }
        in.reverbSend = uint8_t(i % 5 * 10);
        in.delaySend = uint8_t(i % 3 * 15);
        in.panning = int16_t(30 + (i * 7) % 41);
    }
}

void stressPattern(Gen& g, int num) {
    syn::Rng r(uint32_t(1000 + num * 77));
    const int scale[5] = {0, 3, 5, 7, 10};   // A minor pentatonic
    const FxId sampleFx[] = {FxId::Volume, FxId::Panning, FxId::LowPass, FxId::HighPass, FxId::ReverbSend,
                             FxId::DelaySend, FxId::MicroMove, FxId::Glide, FxId::VolumeLfo, FxId::PanningLfo,
                             FxId::FilterLfo, FxId::Overdrive, FxId::BitDepth, FxId::Tune, FxId::Position,
                             FxId::Chance, FxId::RandomVolume, FxId::FinetuneLfo, FxId::GateLength, FxId::Roll};
    const FxId midiFx[] = {FxId::MidiCcA, FxId::MidiCcB, FxId::MidiCcC, FxId::MidiCcD, FxId::MidiCcE,
                           FxId::MidiCcF, FxId::Volume, FxId::MidiChord, FxId::MicroTune, FxId::GateLength};
    auto val = [&](FxId id) -> int {
        switch (id) {
            case FxId::BitDepth: return 8 + int(r.next() % 9);        // 8..16 (engine 4..16)
            case FxId::Roll: return 1 + int(r.next() % 4);            // plain roll, 1..4 hits
            case FxId::VolumeLfo: return int(r.next() % 24);
            case FxId::MidiChord: return int(r.next() % 16);
            case FxId::Tune: return 18 + int(r.next() % 13);          // -6..+6
            case FxId::Glide: return int(r.next() % 20);
            default: {
                const FxInfo& fi = fxInfo(id);
                return fi.minValue + int(r.next() % (fi.maxValue - fi.minValue + 1u));
            }
        }
    };
    char nm[32];
    std::snprintf(nm, sizeof(nm), "Stress %02d", num);
    g.beginPattern(num, kStepCount, nm);
    for (int t = 0; t < kTrackCount; ++t) {
        const bool midi = t >= 8;
        for (int s = 0; s < kStepCount; ++s) {
            const int deg = int(r.next() % 5), oct = int(r.next() % 3);
            const int n = 45 + scale[deg] + 12 * oct;
            // Sample tracks cycle through all 48 instruments; MIDI tracks
            // use instruments 48..63 (two channels per track).
            const int instr = midi ? kMidi + ((t - 8) * 2 + (s / 64)) % kMidiInstrumentCount
                                   : (t * 6 + s) % kSampleInstrumentCount;
            const FxId a = midi ? midiFx[r.next() % 10] : sampleFx[r.next() % 20];
            const FxId b = midi ? midiFx[r.next() % 6] : sampleFx[r.next() % 20];
            g.note(t, s, n, instr, {a, val(a)}, {b == a ? FxId::Volume : b, val(b)});
        }
    }
    g.endPattern();
}

bool stressSong(Gen& g) {
    if (!g.begin()) return false;
    stressSamples(g);
    if (!g.ok) return false;
    // Every MIDI record set (instruments 48..63).
    for (int i = 0; i < kMidiInstrumentCount; ++i) {
        MidiInstrument m = g.app.project.midiInstrument(i);
        m.velocity = uint8_t(64 + i * 4);
        for (int k = 0; k < 6; ++k) m.cc[k] = uint8_t(16 + i * 6 + k);
        g.app.project.setMidiInstrument(i, m);
    }
    for (int t = 0; t < kTrackCount && t < 16; ++t) {
        char nm[16];
        std::snprintf(nm, sizeof(nm), t < 8 ? "Stress %d" : "S%d", t + 1);
        trk::setName(g.app, t, nm);   // 9-16: project.mt 0x603 (spec 21 §3.2)
    }
    constexpr int kPatterns = 32;
    for (int n = 1; n <= kPatterns && g.ok; ++n) stressPattern(g, n);
    g.playlistRange(160, kPatterns);
    mixerSetup(g);
    perfSetup(g);
    return g.ok;
}

// ---------------------------------------------------------------------------
// Song 4: stereo (M2). Stereo instruments in one-shot, loop, slice and beat
// slice, each with a mono twin. Pattern 1 plays the stereo set, pattern 2
// the same notes on the mono twins (A/B): the image should collapse.

enum : int { kSDrumLoop, kSDrumBeat, kSPad, kSPing, kSClapL, kSRimR, kSPadMono, kSDrumMono, kSPingMono, kSKick,
             kSClapMono, kSRimMono };

// Panned drum loop, one bar at 124 BPM: kick centre, snare slightly left,
// hats hard right, toms sweeping left -> right. Returns the hit offsets.
void drumLoop(syn::Buf& l, syn::Buf& r, std::vector<uint32_t>& hits) {
    using namespace syn;
    const int step = frames(60.0 / kTempo / 4.0);
    const int n = step * 16;
    l.assign(n + frames(0.4), 0.0f);
    r.assign(l.size(), 0.0f);
    const Buf k = kick(), s = snare(), h = hat(0.04, 0.1), t1 = tom(140), t2 = tom(100);
    auto put = [&](const Buf& b, int at, double pan, double gain) {
        const double gl = std::cos(pan * kPi / 2) * gain, gr = std::sin(pan * kPi / 2) * gain;
        for (size_t i = 0; i < b.size() && at + int(i) < int(l.size()); ++i) {
            l[at + i] += float(b[i] * gl);
            r[at + i] += float(b[i] * gr);
        }
    };
    for (int i = 0; i < 16; ++i) {
        hits.push_back(uint32_t(i * step));
        if (i % 4 == 0) put(k, i * step, 0.5, 1.0);
        if (i == 4 || i == 12) put(s, i * step, 0.3, 0.8);
        if (i % 2 == 1) put(h, i * step, 1.0, 0.5);
        if (i == 10) put(t1, i * step, 0.1, 0.6);
        if (i == 13) put(t2, i * step, 0.9, 0.6);
    }
    l.resize(n);
    r.resize(n);
    declick(l, 4, 256);
    declick(r, 4, 256);
}

// Ping-pong blips: an FM blip echoing L, R, L, R with decay.
void pingPong(syn::Buf& l, syn::Buf& r) {
    using namespace syn;
    const Buf blip = fm(kC4 * 2, 2.0, 2.5, 0.05, 0.001, 0.08, 0.25);
    const int gap = frames(60.0 / kTempo * 0.75 / 2);
    l.assign(gap * 6 + blip.size(), 0.0f);
    r.assign(l.size(), 0.0f);
    for (int e = 0; e < 6; ++e) {
        syn::Buf& d = e % 2 ? r : l;
        const double gain = std::pow(0.65, e);
        for (size_t i = 0; i < blip.size(); ++i) d[e * gap + i] += float(blip[i] * gain);
    }
}

std::vector<int16_t> mono(const std::vector<int16_t>& l, const std::vector<int16_t>& r) {
    std::vector<int16_t> m(l.size());
    for (size_t i = 0; i < l.size(); ++i) m[i] = int16_t((int32_t(l[i]) + r[i]) / 2);
    return m;
}

bool stereoSong(Gen& g) {
    using namespace syn;
    if (!g.begin()) return false;
    std::vector<int16_t> l16, r16;
    // Drum loop: slice and beat-slice (stereo), slice (mono twin).
    Buf l, r;
    std::vector<uint32_t> hits;
    drumLoop(l, r, hits);
    toI16Stereo(l, r, l16, r16);
    g.addStereo(kSDrumLoop, "ST Drum Loop", l16, r16);
    g.addStereo(kSDrumBeat, "ST Drum Beat", l16, r16);
    g.addMono(kSDrumMono, "MONO Drum Loop", mono(l16, r16));
    // Wide pad: forward loop (stereo) and its mono twin.
    int ls = 0;
    pad(kC4, 3.0, 1.0, l, &r, ls, 77);
    toI16Stereo(l, r, l16, r16);
    g.addStereo(kSPad, "ST Wide Pad", l16, r16);
    g.addMono(kSPadMono, "MONO Pad", mono(l16, r16));
    // Ping-pong FX: one-shot.
    pingPong(l, r);
    toI16Stereo(l, r, l16, r16);
    g.addStereo(kSPing, "ST Ping Pong", l16, r16);
    g.addMono(kSPingMono, "MONO Ping Pong", mono(l16, r16));
    // Hard-panned one-shots: clap left only, rim right only.
    const Buf c = clap(), rim = fm(kC4 * 3, 1.5, 3.0, 0.01, 0.0005, 0.04, 0.2);
    Buf z(c.size(), 0.0f), z2(rim.size(), 0.0f);
    toI16Stereo(c, z, l16, r16);
    g.addStereo(kSClapL, "ST Clap Left", l16, r16);
    toI16Stereo(z2, rim, l16, r16);
    g.addStereo(kSRimR, "ST Rim Right", l16, r16);
    g.addMono(kSClapMono, "MONO Clap", toI16(c));
    g.addMono(kSRimMono, "MONO Rim", toI16(rim));
    g.addMono(kSKick, "MONO Kick", toI16(kick()));
    if (!g.ok) return false;
    g.slices(kSDrumLoop, PlayMode::Slice, hits);
    g.slices(kSDrumBeat, PlayMode::BeatSlice, hits);
    g.slices(kSDrumMono, PlayMode::Slice, hits);
    g.loop(kSPad, PlayMode::ForwardLoop, uint32_t(ls));
    g.loop(kSPadMono, PlayMode::ForwardLoop, uint32_t(ls));
    g.release(kSPad, 800, 150);
    g.release(kSPadMono, 800, 150);
    trackNames(g, {"Slices", "BeatSlice", "Pad", "PingPong", "Clap L", "Rim R", "Kick"});

    // Pattern n: 1 stereo, 2 mono twins, 3 stereo beat-slice + pad, 4 everything.
    auto part = [&](int num, const char* name, bool st, bool beat) {
        g.beginPattern(num, 64, name);
        const int loopI = st ? (beat ? kSDrumBeat : kSDrumLoop) : kSDrumMono;
        // Slices replayed in a new order, one slice per step (Slice FX 0..15).
        const int order[16] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 10, 11, 4, 13, 10, 15};
        for (int s = 0; s < 64; ++s) g.note(0, s, 60, loopI, {FxId::Slice, order[s % 16]});
        for (int bar = 0; bar < 4; ++bar) {
            const Chord ch = kProg[bar];
            g.note(2, bar * kBar, ch.root, st ? kSPad : kSPadMono, {FxId::ReverbSend, 30});
            g.note(3, bar * kBar + 6, 72, st ? kSPing : kSPingMono);
            g.note(4, bar * kBar + 4, 60, st ? kSClapL : kSClapMono, {FxId::Volume, vp(80)});
            g.note(5, bar * kBar + 12, 60, st ? kSRimR : kSRimMono, {FxId::Volume, vp(80)});
            for (int s = 0; s < kBar; s += 4) g.note(6, bar * kBar + s, 60, kSKick, {FxId::Volume, vp(60)});
        }
        // Track 2 doubles the loop in beat-slice mode in the last pattern.
        if (num == 4)
            for (int s = 0; s < 64; s += 2) g.note(1, s, 60, kSDrumBeat, {FxId::Slice, (s / 2) % 16}, {FxId::Volume, vp(40)});
        g.endPattern();
    };
    part(1, "Stereo slices", true, false);
    part(2, "Mono twins", false, false);
    part(3, "Stereo beat slice", true, true);
    part(4, "All stereo", true, true);
    g.playlist({1, 2, 1, 2, 3, 4});
    mixerSetup(g);
    return g.ok;
}

// ---------------------------------------------------------------------------
// Song 5: synths (M3). Synth 1 = VAP pad / lead (polyphony 4), Synth 2 = ACD
// acid bass (polyphony 1, glide), Synth 3 = PERC drum kit (instruments
// 64-66). The patches are our own, derived from each engine's built-in Init
// patch here (no factory patch is used); Save As writes them as N.patch.
// Chords are spread over the tracks (one note per track): in "Sixteen-track
// chords" every one of the 16 tracks plays a chord tone at once, 8 sample
// voices plus 7 synth note requests on the 4-voice synth pool, so voices are
// stolen inside Synth 1 and across the slots.

enum : int { kSlotVap = 0, kSlotAcd = 1, kSlotPerc = 2 };

void patchName(SynthSlotRecord& r, const char* name) { std::snprintf(r.patchName, sizeof(r.patchName), "%s", name); }

void setMacro(SynthPatch& p, int k, const char* name, int param, float amount, bool bipolar = false) {
    PatchMacro& m = p.macros[k];
    std::snprintf(m.name, sizeof(m.name), "%s", name);
    m.nameLen = uint8_t(std::strlen(m.name));
    m.bipolar = bipolar;
    m.count = 1;
    m.targets[0].param = param;
    m.targets[0].amountBits = patchFloatToBits(amount);
}

// One slot: model, the Init patch with `edits`, macros, then the record.
struct ParamEdit {
    int index;
    float value;
};
void setupSlot(Gen& g, int s, SynthModel model, const char* name, const std::vector<ParamEdit>& edits,
               int poly, float gainDb, float pan, float delay, float reverb, void (*macros)(SynthPatch&)) {
    App& a = g.app;
    a.setSynthModel(s, int(model));
    a.initSynthPatch(s);   // built-in Init (never a library patch from the card)
    SynthPatch& p = a.synthPatch(s);
    for (const ParamEdit& e : edits)
        if (!audio::synthParamSet(p, e.index, e.value)) g.fail("synth parameter");
    if (macros) macros(p);
    a.markSynthPatchDirty(s);
    SynthSlotRecord r = a.synthSlot(s);
    patchName(r, name);
    r.polyphony = uint32_t(poly);
    r.gainDb = gainDb;
    r.pan = pan;
    r.delaySend = delay;
    r.reverbSend = reverb;
    for (int k = 0; k < int(mt::kSynthMacroCount); ++k) r.macros[k] = audio::synthMacroDefault(p.macros[k].bipolar);
    a.setSynthSlotRecord(s, r);
}

void setupSynthSlots(Gen& g) {
    // VAP (spec 21 §8.4): saw + detuned saw pad, slow attack, LP 24 with a
    // tempo-synced LFO 1 (2 beats) on the cutoff; M1 cutoff, M2 resonance,
    // M3 LFO depth (mod slot 1 amount), M7 Velocity -> Volume (Init).
    setupSlot(g, kSlotVap, SynthModel::VAP, "OT Pad",
              {{audio::vap::kShape2, 0.5f}, {audio::vap::kTune2, 0.0f}, {audio::vap::kDetune, 18.0f},
               {audio::vap::kOscMix, 0.0f}, {audio::vap::kFilterType, 1.0f}, {audio::vap::kCutoff, 1800.0f},
               {audio::vap::kResonance, 0.35f}, {audio::vap::kFilterEnvAmt, 0.2f}, {audio::vap::kAmpA, 0.25f},
               {audio::vap::kAmpD, 1.0f}, {audio::vap::kAmpS, 0.8f}, {audio::vap::kAmpR, 0.9f},
               {audio::vap::kLfo1Sync, 1.0f}, {audio::vap::kLfo1Ratio, 6.0f}, {audio::vap::kModBase + 0, 3.0f},
               {audio::vap::kModBase + 1, 3.0f}, {audio::vap::kModBase + 2, 0.15f}},
              4, -9.0f, 0.0f, 10.0f, 35.0f, [](SynthPatch& p) {
                  setMacro(p, 0, "Cutoff", audio::vap::kCutoff, 0.3f, true);
                  setMacro(p, 1, "Resonance", audio::vap::kResonance, 0.5f);
                  setMacro(p, 2, "LFO Depth", audio::vap::kModBase + 2, 0.1f);
              });
    // ACD (spec 21 §8.2): saw + square, RD3 ladder, filter envelope, legato
    // glide; M1 cutoff, M2 envelope amount.
    setupSlot(g, kSlotAcd, SynthModel::ACD, "OT Acid Bass",
              {{audio::acd::kSquareMix, 0.4f}, {audio::acd::kSubMix, 0.3f}, {audio::acd::kFilterType, 2.0f},
               {audio::acd::kCutoff, 420.0f}, {audio::acd::kResonance, 0.9f}, {audio::acd::kFilterEnv, 0.45f},
               {audio::acd::kModD, 0.25f}, {audio::acd::kModS, 0.0f}, {audio::acd::kAmpD, 0.6f},
               {audio::acd::kAmpS, 0.7f}, {audio::acd::kAmpR, 0.12f}, {audio::acd::kGlideMode, 2.0f},
               {audio::acd::kGlideTime, 0.35f}},
              1, -4.0f, 0.0f, 20.0f, 0.0f, [](SynthPatch& p) {
                  setMacro(p, 0, "Cutoff", audio::acd::kCutoff, 0.2f, true);
                  setMacro(p, 1, "Env Amt", audio::acd::kFilterEnv, 0.4f, true);
              });
    // PERC (spec 21 §8.6): the Init kit with a longer, lower kick and a
    // slightly open closed hat; per-drum delay sends as in Init.
    setupSlot(g, kSlotPerc, SynthModel::PERC, "OT Kit",
              {{audio::perc::kKickPitch, 29.0f}, {audio::perc::kKickDecay, 0.4f}, {audio::perc::kHatClosed, 0.22f},
               {audio::perc::kMixer + 5 * audio::perc::kSnare + 3, 0.3f}},
              1, -9.0f, 0.0f, 0.0f, 10.0f, nullptr);
}

// Chord tone k (0..7) of bar chord ch, spread over octaves: root, third,
// fifth, seventh / octave, upwards.
int chordTone(const Chord& ch, int k) {
    const int third = ch.chordFx == 4 ? 3 : 4;
    const int iv[4] = {0, third, 7, ch.chordFx == 4 ? 10 : 11};
    return ch.root + iv[k % 4] + 12 * (k / 4);
}

bool synthSong(Gen& g) {
    if (!g.begin()) return false;
    drumKitSamples(g);
    if (!g.ok) return false;
    midiSetup(g);
    setupSynthSlots(g);
    if (!g.ok) return false;
    trackNames(g, {"Kick", "Snare", "Hats", "Bass", "Lead", "Pad", "Perc", "FX"});
    const char* names[8] = {"Chord 1", "Chord 2", "Chord 3", "Chord 4", "Drums", "Hats", "Acid", "Lead"};
    for (int t = 0; t < 8; ++t) trk::setName(g.app, kTMidi9 + t, names[t]);
    const int vap = kSynthBase + kSlotVap, acdI = kSynthBase + kSlotAcd, percI = kSynthBase + kSlotPerc;
    const FxId mac[6] = {FxId::MidiCcA, FxId::MidiCcB, FxId::MidiCcC, FxId::MidiCcD, FxId::MidiCcE, FxId::MidiCcF};
    for (int part = 0; part < 4; ++part) {
        static const char* const pn[4] = {"Synth intro", "Synth groove", "Sixteen-track chords", "Macros"};
        g.beginPattern(part + 1, 64, pn[part]);
        corePattern(g, part == 0 ? 0 : (part == 3 ? 2 : 1));
        // Tracks 9-12 carry synth chords here (no MIDI parts).
        for (int t = kTMidi9; t <= kTMidi12; ++t)
            for (int s = 0; s < 64; ++s)
                if (Step* st = g.step(t, s)) st->clear();
        // PERC plays the drums from pattern 2 on: no sample kick / snare / hats.
        if (part >= 1)
            for (int t = kTKick; t <= kTHats; ++t)
                for (int s = 0; s < 64; ++s)
                    if (Step* st = g.step(t, s)) st->clear();
        for (int bar = 0; bar < 4; ++bar) {
            const int b0 = bar * kBar;
            const Chord ch = kProg[bar];
            // Synth 1 chord: one tone per track 9-12 (4-note chord), re-struck
            // on step 8 in the groove patterns.
            for (int k = 0; k < 4; ++k) {
                g.note(kTMidi9 + k, b0, chordTone(ch, k), vap, {FxId::Volume, 70 + 5 * k});
                if (part == 1 || part == 3) g.note(kTMidi9 + k, b0 + 8, chordTone(ch, k + 1), vap, {FxId::Volume, 55});
            }
            // Synth 2 acid bass with legato glide (from bar 3 in the intro).
            if (part != 0 || bar >= 2) {
                const int r = ch.root - 24;
                const int line[8] = {0, 0, 12, 0, 7, 0, 12, 10};
                for (int k = 0; k < 8; ++k) {
                    const int st = b0 + 2 * k;
                    g.note(kTExt15, st, r + line[k], acdI, k == 2 || k == 6 ? FxSet{FxId::Glide, 6} : FxSet{},
                           part == 3 ? FxSet{mac[0], 20 + 12 * k + 8 * bar} : FxSet{FxId::Volume, k % 2 ? 60 : 90});
                }
            }
            // Synth 3 drums: kick / snare / perc on track 13, hats / cymbal on 14.
            if (part >= 1) {
                for (int s2 = 0; s2 < kBar; s2 += 4) g.note(kTExt13, b0 + s2, 60, percI);
                g.note(kTExt13, b0 + 4, 62, percI);
                g.note(kTExt13, b0 + 12, 62, percI);
                g.note(kTExt13, b0 + 10, 71, percI, {FxId::Volume, 60});
                for (int s2 = 2; s2 < kBar; s2 += 2)
                    if (s2 != 4 && s2 != 12) g.note(kTExt14, b0 + s2, s2 % 4 ? 64 : 66, percI, {FxId::Volume, s2 % 4 ? 70 : 50});
                if (bar == 0) g.note(kTExt14, b0, 69, percI);
                if (bar == 3) g.note(kTExt14, b0 + 14, 67, percI);
            } else if (bar >= 1) {
                for (int s2 = 2; s2 < kBar; s2 += 4) g.note(kTExt14, b0 + s2, 64, percI, {FxId::Volume, 60});
            }
            // Track 16: Synth 1 lead; CC A-F drive Macros 1-6, Volume = Macro 7
            // (ramps in "Macros").
            if (part == 1 || part == 3)
                for (int s2 = 0; s2 < kBar; s2 += 3) {
                    const int n = ch.root + 12 + (s2 % 2 ? 7 : 0);
                    g.note(kTExt16, b0 + s2, n, vap, {mac[(s2 / 3 + bar) % 6], (s2 * 8 + bar * 20) % 128},
                           {FxId::Volume, part == 3 ? 30 + s2 * 4 + bar * 5 : 80});
                }
            // Sixteen-track chords: every track sounds a chord tone on the
            // bar start (samples on 1-8, synths on 9-16). PERC has one tom
            // channel (a second tom on the same tick retriggers it), so one
            // tom per tick: the root on 13 at the bar start, the fifth on 14
            // two steps later.
            if (part == 2) {
                for (int t = 0; t < 8; ++t) {
                    const int instr = t == 3 ? kIBass : (t == 4 ? kILead : kIPad);
                    if (Step* st = g.step(t, b0)) st->clear();
                    g.note(t, b0, chordTone(ch, t) - (t == 3 ? 12 : 0), instr, {FxId::Volume, vp(35)});
                }
                g.note(kTExt13, b0, chordTone(ch, 0) - 12, percI);   // tom at the root
                g.note(kTExt14, b0 + 2, chordTone(ch, 2) - 12, percI);   // tom at the fifth
                g.note(kTExt16, b0, chordTone(ch, 6), vap, {FxId::Volume, 60});
                g.note(kTExt16, b0 + 8, chordTone(ch, 5), vap, {FxId::Volume, 50});
            }
        }
        g.endPattern();
    }
    g.playlist({1, 2, 3, 2, 4, 3});
    mixerSetup(g);
    perfSetup(g);
    return g.ok;
}

#include "bgsongs.inc"
#include "percsong.inc"
#include "vapsong.inc"
#include "metersong.inc"
#include "fatsong.inc"
#include "wtfmsong.inc"
#include "idmsong.inc"
#include "stemsong.inc"

}  // namespace

const SongInfo& songInfo(int index) { return kSongs[index < 0 ? 0 : (index >= kSongCount ? kSongCount - 1 : index)]; }

const SongInfo* findSong(SongId id) {
    for (const SongInfo& s : kSongs)
        if (s.id == id) return &s;
    return nullptr;
}

bool generateSong(App& app, IStorage& fs, SongId id, const GenOptions& opt, std::string& err) {
    const SongInfo* info = findSong(id);
    if (!info) {
        err = "unknown song";
        return false;
    }
    if (!info->available && !opt.force) {
        err = std::string("needs ") + info->milestone;
        return false;
    }
    char sub[8];
    std::snprintf(sub, sizeof(sub), "%02d", int(id));
    Gen g(app, fs, opt, sub);
    bool built = false;
    switch (id) {
        case SongId::TwelveTracks: built = coreSong(g, false); break;
        case SongId::SixteenTracks:
            built = coreSong(g, true);
            if (built) sixteenExtras(g);
            break;
        case SongId::LoadStress: built = stressSong(g); break;
        case SongId::Stereo: built = stereoSong(g); break;
        case SongId::Synths: built = synthSong(g); break;
        case SongId::BgTitle:
        case SongId::BgWorld1:
        case SongId::BgWorld4:
        case SongId::BgBattle:
        case SongId::BgBattleFm:
        case SongId::BgBoss:
        case SongId::BgFinalBoss: built = bgr::bgSong(g, id); break;
        case SongId::PercOnly: built = perconly::percOnlySong(g); break;
        case SongId::VapOnly: built = vaponly::vapOnlySong(g); break;
        case SongId::MeterCheck: built = metercheck::meterSong(g); break;
        case SongId::FatOnly: built = fatonly::fatOnlySong(g); break;
        case SongId::WtfmOnly: built = wtfmonly::wtfmOnlySong(g); break;
        case SongId::Idm: built = idm::idmSong(g); break;
        case SongId::Stems: built = stems::stemSong(g, opt.stemSeconds); break;
    }
    if (built) built = g.finish(info->name);
    if (!built) err = g.err.empty() ? "generation failed" : g.err;
    return built && g.ok;
}

}  // namespace ts
