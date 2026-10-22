// 2.x stereo sample playback (docs/specs/2x-04 §3-§4, docs/MINI_PLAN.md §2):
// per-mode left/right correctness, balance law, sends, loop points and slice
// boundaries on both channels, wavetable / granular left only, mono path
// byte-exact, stereo WAV import -> planar .pti.
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "audio/engine.h"
#include "formats/instrument_pti.h"
#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;
using namespace ot::audio;

namespace {

constexpr uint32_t kLen = 30000;

// Left: rising ramp; right: a different, falling signal, so a channel swap
// or a right read at the wrong frame shows.
std::vector<int16_t> leftData(uint32_t n = kLen) {
    std::vector<int16_t> d(n);
    for (uint32_t i = 0; i < n; ++i) d[i] = int16_t(int32_t(i % 60000) - 30000);
    return d;
}
std::vector<int16_t> rightData(uint32_t n = kLen) {
    std::vector<int16_t> d(n);
    for (uint32_t i = 0; i < n; ++i) d[i] = int16_t(20000 - int32_t((i * 3) % 40000));
    return d;
}
// Planar L block then R block (bank / .pti layout).
std::vector<int16_t> planar(const std::vector<int16_t>& l, const std::vector<int16_t>& r) {
    std::vector<int16_t> p(l);
    p.insert(p.end(), r.begin(), r.end());
    return p;
}

Instrument baseInstrument(PlayMode m, uint32_t len = kLen) {
    Instrument ins;
    ins.lengthFrames = len;
    ins.playMode = uint8_t(m);
    ins.env[kAutoVolume].enabled = 0;   // flat gate
    return ins;
}

struct Out {
    std::vector<float> amp, l, r, ds, rs;
};

// Renders a bare voice. right == nullptr: mono note. fused: pass the dry-bus
// pointers (fast path when no crusher / overdrive / filter).
Out renderNote(const Instrument& ins, const int16_t* left, const int16_t* right, uint8_t note, int blocks,
               uint8_t aT = 0, uint8_t aV = 0, uint8_t bT = 0, uint8_t bV = 0, bool fused = false,
               VoiceControl c = VoiceControl{}) {
    initTables();
    std::unique_ptr<Voice> v(new Voice());
    v->reset();
    v->noteOn(&ins, left, ins.lengthFrames, note, -1, aT, aV, bT, bV, right);
    Out o;
    VoiceOut vo;
    for (int b = 0; b < blocks; ++b) {
        float ds[kBlock] = {}, rs[kBlock] = {}, ml[kBlock] = {}, mr[kBlock] = {};
        v->control(c);
        v->render(c, vo, ds, rs, fused ? ml : nullptr, fused ? mr : nullptr, true);
        o.amp.insert(o.amp.end(), vo.amp, vo.amp + kBlock);
        o.l.insert(o.l.end(), vo.l, vo.l + kBlock);
        o.r.insert(o.r.end(), vo.r, vo.r + kBlock);
        o.ds.insert(o.ds.end(), ds, ds + kBlock);
        o.rs.insert(o.rs.end(), rs, rs + kBlock);
        if (fused) {
            for (uint32_t i = 0; i < kBlock; ++i) {
                CHECK(ml[i] == vo.l[i]);
                CHECK(mr[i] == vo.r[i]);
            }
        }
    }
    return o;
}

bool same(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) {
            std::printf("    differ at %zu: %.8f vs %.8f\n", i, double(a[i]), double(b[i]));
            return false;
        }
    return true;
}

int nonZero(const std::vector<float>& a) {
    int n = 0;
    for (float v : a) n += v != 0.0f;
    return n;
}

struct ModeCase {
    PlayMode mode;
    const char* name;
};
const ModeCase kStereoModes[] = {
    {PlayMode::OneShot, "one-shot"},       {PlayMode::ForwardLoop, "forward loop"},
    {PlayMode::BackwardLoop, "backward loop"}, {PlayMode::PingPongLoop, "ping-pong"},
    {PlayMode::Slice, "slice"},            {PlayMode::BeatSlice, "beat slice"},
};

// Marker setup that exercises start / loop / end points and slices.
void markers(Instrument& ins) {
    ins.startPoint = 3000;
    ins.loopPoint1 = 9000;
    ins.loopPoint2 = 14000;
    ins.endPoint = 40000;
    ins.sliceCount = 4;
    ins.slices[0] = 2000;
    ins.slices[1] = 7000;
    ins.slices[2] = 12000;
    ins.slices[3] = 30000;
    ins.selectedSlice = 1;
}

}  // namespace

// --- each channel of a stereo note == the mono rendering of that channel -------
//
// Centre pan passes L and R unchanged (balance), so with one shared playhead
// the stereo o.l / o.r must equal, bit for bit, the mono voice playing the left
// / right block alone: same start, loop and end points, slices, pitch, glide,
// reverse, interpolation and processing.

TEST(stereo_channels_match_mono_per_mode) {
    const auto L = leftData(), R = rightData();
    struct Fx {
        uint8_t aT, aV, bT, bV;
        uint8_t note;
    };
    const Fx fxs[] = {
        {0, 0, 0, 0, 60},                    // unity
        {0, 0, 0, 0, 67},                    // fractional rate
        {0, 0, 0, 0, 31},                    // slow
        {kFxReverse, 1, 0, 0, 64},           // reversed playhead
        {kFxGlide, 30, kFxTune, 30, 55},     // glide + tune
        {kFxSlice, 2, 0, 0, 72},             // slice FX (slice modes)
        {kFxPosition, 40, 0, 0, 60},         // position FX
        {kFxBitDepth, 9, kFxOverdrive, 40, 62},   // crusher + overdrive (per channel)
        {kFxLowPass, 30, 0, 0, 70},          // filter (separate state per channel)
    };
    for (const ModeCase& mc : kStereoModes) {
        for (const Fx& f : fxs) {
            for (int aa = 0; aa < 2; ++aa) {
                Instrument ins = baseInstrument(mc.mode);
                markers(ins);
                VoiceControl c;
                c.antiAliasing = aa != 0;
                const Out st = renderNote(ins, L.data(), R.data(), f.note, 60, f.aT, f.aV, f.bT, f.bV, false, c);
                const Out ml = renderNote(ins, L.data(), nullptr, f.note, 60, f.aT, f.aV, f.bT, f.bV, false, c);
                const Out mr = renderNote(ins, R.data(), nullptr, f.note, 60, f.aT, f.aV, f.bT, f.bV, false, c);
                const bool okL = same(st.l, ml.amp), okR = same(st.r, mr.amp);
                if (!okL || !okR) std::printf("    mode %s note %d fx %d/%d aa %d\n", mc.name, f.note, f.aT, f.bT, aa);
                CHECK(okL);
                CHECK(okR);
                CHECK(nonZero(st.l) > 100);
                CHECK(nonZero(st.r) > 100);
            }
        }
    }
}

// The fused fast path (no crusher / overdrive / filter, dry bus given) gives
// the same channels as the stage-by-stage path.
TEST(stereo_fused_path_matches_staged) {
    const auto L = leftData(), R = rightData();
    for (const ModeCase& mc : kStereoModes) {
        Instrument ins = baseInstrument(mc.mode);
        markers(ins);
        ins.delaySend = 40;
        ins.reverbSend = 70;
        const Out a = renderNote(ins, L.data(), R.data(), 63, 40, 0, 0, 0, 0, true);
        const Out b = renderNote(ins, L.data(), R.data(), 63, 40, 0, 0, 0, 0, false);
        CHECK(same(a.l, b.l));
        CHECK(same(a.r, b.r));
        CHECK(same(a.amp, b.amp));
        for (size_t i = 0; i < a.ds.size(); ++i) {
            CHECK_NEAR(a.ds[i], b.ds[i], 1e-6f);
            CHECK_NEAR(a.rs[i], b.rs[i], 1e-6f);
        }
    }
}

// --- loop points and slice boundaries on both channels -------------------------

TEST(stereo_loop_points_both_channels) {
    // Index-coded channels: L = i - 15000, R = 15000 - i (both invertible).
    const uint32_t n = 30000;
    std::vector<int16_t> L(n), R(n);
    for (uint32_t i = 0; i < n; ++i) {
        L[i] = int16_t(int32_t(i) - 15000);
        R[i] = int16_t(15000 - int32_t(i));
    }
    Instrument ins = baseInstrument(PlayMode::ForwardLoop, n);
    ins.startPoint = 0;
    ins.loopPoint1 = uint16_t(65535u * 1000u / n + 1);   // frame 1000
    ins.loopPoint2 = uint16_t(65535u * 2000u / n + 1);   // frame 2000
    ins.endPoint = 65535;
    VoiceControl c;
    c.antiAliasing = false;   // drop-sample: every output is an exact frame
    const Out o = renderNote(ins, L.data(), R.data(), 60, 40, 0, 0, 0, 0, false, c);
    int wraps = 0;
    int32_t prev = -1;
    for (size_t k = 0; k < o.l.size(); ++k) {
        const int32_t fl = int32_t(std::lround(double(o.l[k]) * 32768.0)) + 15000;
        const int32_t fr = 15000 - int32_t(std::lround(double(o.r[k]) * 32768.0));
        CHECK_EQ(fl, fr);   // one playhead
        if (k > 2000) {
            CHECK(fl >= 1000 && fl < 2000);   // inside the loop on both channels
        }
        if (prev >= 0 && fl < prev) ++wraps;
        prev = fl;
    }
    CHECK(wraps >= 3);
}

TEST(stereo_slice_boundaries_both_channels) {
    const uint32_t n = 30000;
    std::vector<int16_t> L(n), R(n);
    for (uint32_t i = 0; i < n; ++i) {
        L[i] = int16_t(int32_t(i) - 15000);
        R[i] = int16_t(15000 - int32_t(i));
    }
    for (PlayMode m : {PlayMode::Slice, PlayMode::BeatSlice}) {
        Instrument ins = baseInstrument(m, n);
        ins.sliceCount = 3;
        ins.slices[0] = 0;
        ins.slices[1] = uint16_t(65535u * 10000u / n);
        ins.slices[2] = uint16_t(65535u * 12000u / n);
        ins.selectedSlice = 1;
        VoiceControl c;
        c.antiAliasing = false;
        // Beat Slice: note n plays slice n - 24 (1.9.7), so slice 1 is note 25.
        const Out o = renderNote(ins, L.data(), R.data(), m == PlayMode::BeatSlice ? 25 : 60, 40, 0, 0, 0, 0, false, c);
        const int32_t s = int32_t(uint64_t(n) * ins.slices[1] / 65535u);
        const int32_t e = int32_t(uint64_t(n) * ins.slices[2] / 65535u);
        int played = 0;
        bool ended = false;
        for (size_t k = 0; k < o.l.size(); ++k) {
            if (o.l[k] == 0.0f && o.r[k] == 0.0f) {
                ended = true;
                continue;
            }
            CHECK(!ended);   // nothing after the slice end
            const int32_t fl = int32_t(std::lround(double(o.l[k]) * 32768.0)) + 15000;
            const int32_t fr = 15000 - int32_t(std::lround(double(o.r[k]) * 32768.0));
            CHECK_EQ(fl, fr);
            CHECK(fl >= s && fl < e);
            if (k == 0) CHECK_EQ(fl, s);
            ++played;
        }
        // The unity rate of the pitch table is not exactly 1.0 (a frame may
        // repeat), so up to e - s + 2 outputs; the last one is the slice end.
        CHECK(played >= e - s && played <= e - s + 2);
        const int32_t lastL = int32_t(std::lround(double(o.l[size_t(played - 1)]) * 32768.0)) + 15000;
        CHECK_EQ(lastL, e - 1);
    }
}

// --- balance law (2x-04 §3.2) ----------------------------------------------------

TEST(stereo_balance_law) {
    const auto L = leftData(), R = rightData();
    for (int pan : {0, 10, 25, 49, 50, 51, 75, 90, 100}) {
        for (int fused = 0; fused < 2; ++fused) {
            Instrument ins = baseInstrument(PlayMode::ForwardLoop);
            ins.panning = int16_t(pan);
            Instrument centre = ins;
            centre.panning = 50;
            const Out o = renderNote(ins, L.data(), R.data(), 60, 6, 0, 0, 0, 0, fused != 0);
            const Out c = renderNote(centre, L.data(), R.data(), 60, 6, 0, 0, 0, 0, fused != 0);
            float gl, gr;
            panGains(pan, gl, gr);
            // Centre passes both channels; right of centre attenuates L
            // linearly, left of centre attenuates R (stock law).
            if (pan >= 50) CHECK_EQ(gr, 1.0f);
            if (pan <= 50) CHECK_EQ(gl, 1.0f);
            for (size_t k = 0; k < o.l.size(); ++k) {
                CHECK_NEAR(o.l[k], c.l[k] * gl, 1e-6f);
                CHECK_NEAR(o.r[k], c.r[k] * gr, 1e-6f);
            }
        }
    }
    // Hard right: the left channel is silent and the right one is untouched
    // (balance, not a mono pan: no left content moves to the right).
    Instrument ins = baseInstrument(PlayMode::ForwardLoop);
    ins.panning = 100;
    const Out o = renderNote(ins, L.data(), R.data(), 60, 4);
    const Out mr = renderNote(baseInstrument(PlayMode::ForwardLoop), R.data(), nullptr, 60, 4);
    CHECK_EQ(nonZero(o.l), 0);
    CHECK(same(o.r, mr.amp));
}

TEST(stereo_pan_fx_acts_as_balance) {
    const auto L = leftData(), R = rightData();
    Instrument ins = baseInstrument(PlayMode::ForwardLoop);
    const Out o = renderNote(ins, L.data(), R.data(), 60, 4, kFxPanning, 0);   // hard left
    CHECK_EQ(nonZero(o.r), 0);
    const Out ml = renderNote(ins, L.data(), nullptr, 60, 4);
    CHECK(same(o.l, ml.amp));
}

// --- sends and side-chain source -------------------------------------------------

TEST(stereo_sends_take_the_channel_mean) {
    const auto L = leftData(), R = rightData();
    Instrument ins = baseInstrument(PlayMode::ForwardLoop);
    ins.delaySend = 100;
    ins.reverbSend = 100;
    VoiceControl c;
    c.postFaderSends = false;
    const Out st = renderNote(ins, L.data(), R.data(), 60, 4, 0, 0, 0, 0, false, c);
    const float g = sendGain(100);
    for (size_t k = kBlock; k < st.ds.size(); ++k) {   // ramps settled
        const float m = 0.5f * (float(L[k]) + float(R[k])) / 32768.0f;
        CHECK_NEAR(st.ds[k], m * g, 2e-5f);
        CHECK_NEAR(st.rs[k], m * g, 2e-5f);
        CHECK_NEAR(st.amp[k], 0.5f * (st.l[k] + st.r[k]), 1e-6f);   // side-chain source
    }
    // Opposite channels cancel in the sends (drop-sample: the interpolation
    // rounds towards -inf, so it is not symmetric).
    std::vector<int16_t> neg(R.size());
    for (size_t i = 0; i < neg.size(); ++i) neg[i] = int16_t(-L[i]);
    c.antiAliasing = false;
    const Out z = renderNote(ins, L.data(), neg.data(), 60, 4, 0, 0, 0, 0, false, c);
    CHECK_EQ(nonZero(z.ds), 0);
    CHECK(nonZero(z.l) > 100);
}

// --- wavetable / granular: left channel only in 2.0 (TODO(2.1)) -----------------

TEST(stereo_wavetable_granular_play_left_only) {
    std::vector<int16_t> L(2048 * 8), R(2048 * 8);
    for (size_t i = 0; i < L.size(); ++i) {
        L[i] = int16_t(20000.0f * std::sin(6.2831853f * float(i % 2048) / 2048.0f));
        R[i] = int16_t(-7000);
    }
    for (PlayMode m : {PlayMode::Wavetable, PlayMode::Granular}) {
        Instrument ins = baseInstrument(m, uint32_t(L.size()));
        ins.wavetableWindowSize = 2048;
        ins.wavetableWindowCount = 8;
        std::unique_ptr<Voice> v(new Voice());
        v->reset();
        v->noteOn(&ins, L.data(), ins.lengthFrames, 60, -1, 0, 0, 0, 0, R.data());
        CHECK(!v->stereo());
        const Out st = renderNote(ins, L.data(), R.data(), 60, 20);
        const Out ml = renderNote(ins, L.data(), nullptr, 60, 20);
        CHECK(same(st.l, ml.l));
        CHECK(same(st.r, ml.r));   // centred mono voice of the left block
        CHECK(nonZero(st.l) > 100);
    }
}

// --- note changes between mono and stereo ----------------------------------------

TEST(stereo_mono_retrigger_transitions) {
    const auto L = leftData(), R = rightData();
    Instrument st = baseInstrument(PlayMode::ForwardLoop);
    Instrument mo = baseInstrument(PlayMode::ForwardLoop);
    st.env[kAutoVolume].enabled = 1;   // envelope: retrigger goes through FORCED
    mo.env[kAutoVolume].enabled = 1;
    st.filterEnabled = mo.filterEnabled = 1;
    st.cutoff = mo.cutoff = 0.4f;
    std::unique_ptr<Voice> v(new Voice());
    v->reset();
    VoiceControl c;
    VoiceOut o;
    float ds[kBlock], rs[kBlock];
    auto run = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) {
            v->control(c);
            v->render(c, o, ds, rs);
            for (uint32_t i = 0; i < kBlock; ++i) {
                CHECK(std::isfinite(o.l[i]) && std::isfinite(o.r[i]) && std::isfinite(o.amp[i]));
                CHECK(std::fabs(o.l[i]) <= 1.0f && std::fabs(o.r[i]) <= 1.0f);
            }
        }
    };
    v->noteOn(&mo, L.data(), kLen, 60, -1, 0, 0, 0, 0, nullptr);
    run(10);
    CHECK(!v->stereo());
    v->noteOn(&st, L.data(), kLen, 60, -1, 0, 0, 0, 0, R.data());   // mono -> stereo
    run(10);
    CHECK(v->stereo());
    bool differ = false;
    for (uint32_t i = 0; i < kBlock; ++i) differ |= o.l[i] != o.r[i];
    CHECK(differ);
    v->noteOn(&mo, L.data(), kLen, 60, -1, 0, 0, 0, 0, nullptr);    // stereo -> mono
    run(10);
    CHECK(!v->stereo());
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(o.l[i] == o.r[i]);   // centred mono
}

// --- identical channels stay identical across notes ------------------------------
//
// L == R data at centre pan gives L == R output. After a silent gap a new
// stereo note starts its filter at the new note's coefficients on both
// channels (the right filter must not ramp from the previous note's).

TEST(identical_channels_stay_identical_across_notes) {
    const auto D = leftData(3000);
    Instrument a = baseInstrument(PlayMode::OneShot, 3000);
    Instrument b = baseInstrument(PlayMode::OneShot, 3000);
    a.filterEnabled = b.filterEnabled = 1;
    a.filterType = b.filterType = uint8_t(FilterType::LowPass);
    a.cutoff = 0.8f;
    a.resonance = 0.3f;
    b.cutoff = 0.15f;
    b.resonance = 0.6f;
    std::unique_ptr<Voice> v(new Voice());
    v->reset();
    VoiceControl c;
    VoiceOut o;
    float ds[kBlock], rs[kBlock];
    int blocks = 0, mismatches = 0, sound = 0;
    auto run = [&](int n) {
        for (int k = 0; k < n; ++k, ++blocks) {
            v->control(c);
            v->render(c, o, ds, rs);
            for (uint32_t i = 0; i < kBlock; ++i) {
                mismatches += o.l[i] != o.r[i];
                sound += o.l[i] != 0.0f;
            }
        }
    };
    for (int note = 0; note < 4; ++note) {
        const Instrument& ins = note & 1 ? b : a;
        v->noteOn(&ins, D.data(), 3000, uint8_t(60 + note * 3), -1, 0, 0, 0, 0, D.data());
        CHECK(v->stereo());
        run(60);   // past the one-shot end: a silent gap before the next note
        CHECK(!v->active());
    }
    CHECK(sound > 0);
    if (mismatches) std::printf("    %d samples with L != R in %d blocks\n", mismatches, blocks);
    CHECK_EQ(mismatches, 0);
}

// --- mono path byte-exact ---------------------------------------------------------
//
// Hashes of the mono voice output (o.amp / o.l / o.r, both render paths) for
// every play mode, recorded with the 2.x code before the stereo path existed
// (253012b). Any change to the mono path shows here and in audio_test's
// deterministic_render_hash.

namespace {
uint64_t fnvF(uint64_t h, const std::vector<float>& v) {
    for (float f : v) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        h ^= u;
        h *= 1099511628211ull;
    }
    return h;
}
}  // namespace

TEST(mono_path_byte_exact) {
    const auto L = leftData();
    uint64_t h = 1469598103934665603ull;
    for (int m = 0; m < 8; ++m) {
        Instrument ins = baseInstrument(PlayMode(m), kLen);
        markers(ins);
        ins.wavetableWindowSize = 2048;
        ins.wavetableWindowCount = 8;
        ins.panning = int16_t(20 + 8 * m);
        ins.delaySend = 30;
        for (int fused = 0; fused < 2; ++fused) {
            const Out a = renderNote(ins, L.data(), nullptr, uint8_t(55 + m), 30, kFxGlide, 10, 0, 0, fused != 0);
            h = fnvF(h, a.amp);
            h = fnvF(h, a.l);
            h = fnvF(h, a.r);
            h = fnvF(h, a.ds);
            ins.filterEnabled = 1;
            ins.cutoff = 0.3f;
            ins.bitDepth = 10;
            const Out b = renderNote(ins, L.data(), nullptr, uint8_t(60 + m), 30, 0, 0, 0, 0, fused != 0);
            h = fnvF(h, b.amp);
            h = fnvF(h, b.l);
            h = fnvF(h, b.r);
            ins.filterEnabled = 0;
            ins.bitDepth = 16;
        }
    }
    std::printf("  mono voice hash: %016llx\n", (unsigned long long)h);
    CHECK_EQ(h, 0xd2404478ffae4c1bull);   // golden (host x86-64), re-recorded at 1.9.9 (bit depth rounds to nearest)
}

// --- engine: stereo bank instrument plays stereo, stems keep the image ------------

TEST(engine_stereo_instrument_and_stems) {
    std::vector<int16_t> fx(AudioEngine::kFxMemorySamples), pool(1u << 20);
    std::unique_ptr<Instrument[]> ins(new Instrument[kSampleInstrumentCount]);
    std::unique_ptr<AudioEngine> eng(new AudioEngine());
    CHECK(eng->init(fx.data(), fx.size(), pool.data(), pool.size(), ins.get()));
    const auto L = leftData(), R = rightData();
    const auto p = planar(L, R);
    ins[0] = baseInstrument(PlayMode::OneShot);
    CHECK(eng->bank().store(0, p.data(), kLen, true));
    eng->beginOffline();
    VoiceNoteOn n;
    n.instrument = 0;
    n.note = 60;
    n.volume = -1;
    eng->voiceNoteOn(0, n);
    CHECK(eng->voice(0).stereo());
    std::unique_ptr<StemBlock> stems(new StemBlock);
    int16_t master[kBlock * 2];
    for (int b = 0; b < 4; ++b) eng->renderOfflineBlock(master, stems.get());
    // Track 1 stem: L and R are the two blocks (unity gains, centred).
    int diff = 0;
    for (uint32_t i = 0; i < kBlock; ++i) {
        const uint32_t f = 3 * kBlock + i;
        CHECK(std::abs(stems->data[0][2 * i] - L[f]) <= 1);
        CHECK(std::abs(stems->data[0][2 * i + 1] - R[f]) <= 1);
        diff += master[2 * i] != master[2 * i + 1];
    }
    CHECK(diff > 100);   // the master keeps the image too
    eng->endOffline();
}

// --- import: a stereo WAV becomes a planar .pti ----------------------------------

TEST(loader_imports_stereo_wav_as_planar_pti) {
    Harness h;
    App& a = *h.app;
    const uint32_t n = 4410;
    std::vector<int16_t> inter(2 * n);
    for (uint32_t i = 0; i < n; ++i) {
        inter[2 * i] = int16_t(int32_t(i) * 5 - 11000);    // L
        inter[2 * i + 1] = int16_t(9000 - int32_t(i) * 3);  // R
    }
    std::vector<uint8_t> wav(inter.size() * 2 + 64);
    wav.resize(wavEncode(inter.data(), n, 2, wav.data(), wav.size()));
    h.fs.mkdir("Samples");
    h.fs.files["Samples/wide.wav"] = wav;
    std::vector<int16_t> mono(n, 1234);
    std::vector<uint8_t> mw(n * 2 + 64);
    mw.resize(wavEncode(mono.data(), n, 1, mw.data(), mw.size()));
    h.fs.files["Samples/zmono.wav"] = mw;
    // Memory projection: 4 bytes per stereo frame, 2 per mono frame.
    CHECK_EQ(a.sampleBytesNeeded("Samples/wide.wav"), n * 4);
    CHECK_EQ(a.sampleBytesNeeded("Samples/zmono.wav"), n * 2);
    // Through the Sample Loader: open Samples, add wide.wav to slot 1.
    h.tap(btn::SampleLoader);
    LoaderScreen& l = *static_cast<LoaderScreen*>(a.ui.screen(kModLoader));
    int idx = -1;
    for (int i = 0; i < l.count; ++i)
        if (!std::strcmp(l.entries[i], "Samples")) idx = i;
    CHECK(idx >= 0);
    while (l.sdCursor < idx) h.tap(btn::Down);
    h.tap(btn::F1);
    h.tap(btn::Down);   // ".." -> wide.wav
    h.tap(btn::F1);
    CHECK(a.instrPresent[0]);
    CHECK(a.bank.stereo(0));
    CHECK_EQ(a.bank.frames(0), n);
    for (uint32_t i = 0; i < n; i += 97) {
        CHECK_EQ(a.bank.left(0)[i], inter[2 * i]);
        CHECK_EQ(a.bank.right(0)[i], inter[2 * i + 1]);
    }
    // Pad audition plays both channels.
    a.previewOn(0, 0, 60, 127);
    int16_t buf[256];
    int differ = 0;
    for (int b = 0; b < 4; ++b) {
        h.audio.fn(buf, 128, h.audio.ctx);
        for (int k = 0; k < 128; ++k) differ += buf[2 * k] != buf[2 * k + 1];
    }
    CHECK(differ > 100);
    a.previewAllOff();
    // Autosave writes a planar stereo .pti (stock layout, spec 02 §6.3).
    a.autosave();
    const auto& f = h.fs.files["Workspace/instruments/instrument_01.pti"];
    CHECK_EQ(f.size(), ptiFileSize(n, true));
    Instrument back;
    PtiLoadInfo li;
    CHECK(decodePtiHeader(f.data(), f.size(), back, &li) == FmtStatus::Ok);
    const PtiSampleLayout lay = resolvePtiSample(back.lengthFrames, uint32_t(f.size() - li.dataOffset), true);
    CHECK(lay.stereoInFile && lay.useStereo);
    CHECK_EQ(lay.frames, n);
    for (uint32_t i = 0; i < n; i += 101) {
        const uint8_t* pl = f.data() + li.dataOffset + 2 * i;
        const uint8_t* pr = f.data() + li.dataOffset + 2 * (n + i);
        CHECK_EQ(int16_t(pl[0] | pl[1] << 8), inter[2 * i]);
        CHECK_EQ(int16_t(pr[0] | pr[1] << 8), inter[2 * i + 1]);
    }
    // A mono WAV stays mono.
    CHECK(a.importSample("Samples/zmono.wav", 1));
    CHECK(!a.bank.stereo(1));
    CHECK_EQ(a.bank.frames(1), n);
}
