// Audio engine tests (spec 04).
#include <chrono>
#include <memory>
#include <vector>

#include "audio/dsp.h"
#include "audio/engine.h"
#include "audio/engines/acd.h"
#include "audio/synth_engine.h"
#include "formats/patch_pb.h"
#include "formats/wav.h"
#include "mem_storage.h"
#include "sequencer/sequencer.h"
#include "test.h"

using namespace ot;
using namespace ot::audio;

namespace {

constexpr float kPiF = 3.14159265f;

// Ramp sample: data[i] = i - 32768, so a rendered value maps back to its index.
std::vector<int16_t> rampSample(uint32_t len = 65535) {
    std::vector<int16_t> d(len);
    for (uint32_t i = 0; i < len; ++i) d[i] = int16_t(int32_t(i) - 32768);
    return d;
}
int idxOf(float v) { return int(std::lround(double(v) * 32768.0)) + 32768; }

Instrument plainInstrument(uint32_t len, PlayMode mode) {
    Instrument ins;
    ins.lengthFrames = len;
    ins.playMode = uint8_t(mode);
    ins.env[0].enabled = 0;   // flat gate: voice output == sample value
    return ins;
}

// Renders `blocks` blocks of a bare voice; collects post-volume mono output.
std::vector<float> renderVoice(Voice& v, int blocks, VoiceControl c = VoiceControl{}) {
    std::vector<float> out;
    VoiceOut o;
    float ds[kBlock] = {}, rs[kBlock] = {};
    for (int b = 0; b < blocks; ++b) {
        v.control(c);
        v.render(c, o, ds, rs);
        out.insert(out.end(), o.amp, o.amp + kBlock);
    }
    return out;
}

struct Rig {
    std::vector<int16_t> fx = std::vector<int16_t>(AudioEngine::kFxMemorySamples);
    std::vector<int16_t> pool = std::vector<int16_t>(4u << 20);
    std::unique_ptr<Instrument[]> ins = std::unique_ptr<Instrument[]>(new Instrument[kSampleInstrumentCount]);
    std::unique_ptr<AudioEngine> eng = std::unique_ptr<AudioEngine>(new AudioEngine());
    Rig() { CHECK(eng->init(fx.data(), fx.size(), pool.data(), pool.size(), ins.get())); }
    void load(int i, const std::vector<int16_t>& d, PlayMode m = PlayMode::OneShot) {
        ins[i].lengthFrames = uint32_t(d.size());
        ins[i].playMode = uint8_t(m);
        CHECK(eng->bank().store(i, d.data(), uint32_t(d.size()), false));
    }
    void note(uint8_t track, uint8_t instr, uint8_t note, uint8_t aT = 0, uint8_t aV = 0, uint8_t bT = 0,
              uint8_t bV = 0, int8_t vol = -1) {
        VoiceNoteOn n;
        n.instrument = instr;
        n.note = note;
        n.volume = vol;
        n.fxA = StepFx{aT, aV};
        n.fxB = StepFx{bT, bV};
        eng->voiceNoteOn(track, n);
    }
};

std::vector<int16_t> sine(uint32_t len, float hz, float amp = 0.8f) {
    std::vector<int16_t> d(len);
    for (uint32_t i = 0; i < len; ++i) d[i] = int16_t(amp * 32767.0f * std::sin(2.0f * kPiF * hz * float(i) / 44100.0f));
    return d;
}

uint64_t fnv(uint64_t h, const int16_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        h ^= uint16_t(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}

float peakOf(const float* x, size_t n) {
    float p = 0;
    for (size_t i = 0; i < n; ++i) p = std::fmax(p, std::fabs(x[i]));
    return p;
}

}  // namespace

// --- mapping tables (spec 04 §7) ----------------------------------------------

TEST(mapping_tables) {
    initTables();
    CHECK_NEAR(pitchRate(60), 0.9996f, 1e-6);
    CHECK_NEAR(pitchRate(72), 2.0f * 0.9996f, 1e-5);
    CHECK_NEAR(pitchRate(48), 0.5f * 0.9996f, 1e-6);
    CHECK_NEAR(pitchRate(500), pitchRate(119), 0);
    // Wavetable: one window cycle at index 60 lasts 1/523.25 s.
    CHECK_NEAR(wavetablePitchTable(2048)[60] / 2048.0f * 44117.647f, 523.25f * 0.9996f * 44117.647f / 44100.0f, 0.05);
    CHECK_NEAR(volGain(50), 1.0f, 1e-6);
    CHECK_NEAR(20.0f * std::log10(volGain(100)), 24.0f, 1e-3);
    CHECK_NEAR(20.0f * std::log10(volGain(1)), -24.0f, 1e-3);
    CHECK_EQ(volGain(0), 0.0f);
    CHECK_NEAR(trkGain(86), 1.0f, 1e-6);
    CHECK_NEAR(20.0f * std::log10(trkGain(100)), 6.0f, 1e-3);
    CHECK_NEAR(20.0f * std::log10(trkGain(1)), -36.0f, 1e-3);
    CHECK_NEAR(sendGain(100), 1.0f, 1e-5);
    CHECK_NEAR(20.0f * std::log10(sendGain(50)), -20.0f, 1e-3);
    CHECK_NEAR(cutoffHz(0.0f), 20.0f, 1e-3);
    CHECK_NEAR(cutoffHz(1.0f), 20.0f * std::exp2(477.0f / 50.0f), 0.1);
    CHECK_NEAR(svfDamp(0.0f), 1.0f / 0.7f, 1e-6);
    CHECK_NEAR(svfDamp(4.3f), 0.2f, 1e-6);
    CHECK_NEAR(overdrivePreGain(100), 928.0f / 256.0f, 1e-6);
    CHECK_EQ(overdrivePreGain(0), 0.0f);
    CHECK_EQ(bitMask(16), 0xFFFF);
    CHECK_EQ(bitMask(4), 0xF000);
    CHECK_EQ(bitMask(1), 0xF000);
    CHECK_EQ(delayFeedbackK(100), 252);
    CHECK_EQ(delayFeedbackK(50), 127);
    CHECK_EQ(delayTimeSamples(500.0f), 22050u);
    CHECK_EQ(delayTimeSamples(9999.0f), 154350u);
    CHECK_NEAR(delaySyncMs(120.0f, 6), 125.0f, 1e-3);   // 1 step at 120 BPM
    CHECK_NEAR(limiterThreshold(32767), 1.0f, 1e-6);
    CHECK_NEAR(limiterThreshold(0), 0.0f, 1e-6);
    CHECK_EQ(envMsToCount(10.5f), uint16_t((uint32_t(10.5f * 44.1176f) + 7) >> 3));
    CHECK_EQ(envMsToCount(100000.0f), 65535);
    CHECK_EQ(glideFxMs(0), 0u);
    CHECK_EQ(glideFxMs(49), 1000u);
    CHECK_EQ(glideFxMs(100), 15000u);
    float l, r;
    panGains(0, l, r);
    CHECK(l == 1.0f && r == 0.0f);
    panGains(75, l, r);
    CHECK(l == 0.5f && r == 1.0f);
    CHECK_EQ(grainWindow(0)[500], 65535);
    CHECK_EQ(grainWindow(1)[0], 0);
    CHECK_NEAR(grainWindow(2)[500], 65535, 1);
    // Saturation shapers stay bounded and continuous.
    for (int m = 0; m <= 4; ++m) {
        float prev = saturate(-3.0f, m);
        for (float u = -3.0f; u <= 3.0f; u += 0.001f) {
            const float y = saturate(u, m);
            CHECK(y >= -1.0f && y <= 1.0f);
            if (std::fabs(y - prev) > 0.02f) {
                CHECK(false);
                break;
            }
            prev = y;
        }
    }
    CHECK_NEAR(saturate(0.1f, 4), 0.3f, 1e-6);
    CHECK_NEAR(saturate(0.5f, 2), 0.6875f, 1e-6);
}

// --- pitch (spec 04 §2.4) -----------------------------------------------------

TEST(pitch_ratio_accuracy) {
    initTables();
    auto data = rampSample();
    Instrument ins = plainInstrument(65535, PlayMode::OneShot);
    VoiceControl c;
    c.antiAliasing = false;
    const int notes[] = {48, 60, 67, 72, 84};
    for (int n : notes) {
        Voice v;
        v.noteOn(&ins, data.data(), 65535, uint8_t(n), -1, 0, 0, 0, 0);
        auto out = renderVoice(v, 8, c);
        const double rate = double(pitchRate(n));
        // Output sample k reads position floor(k * rate) (16-bit fraction).
        int worst = 0;
        for (int k = 0; k < 1000; ++k) worst = std::max(worst, std::abs(idxOf(out[k]) - int(double(k) * rate)));
        CHECK(worst <= 1);
        CHECK_NEAR(v.rate(), pitchRate(n), 1e-6);
    }
    // Finetune: linear between semitones (+50 cents at note 60).
    ins.finetune = 50;
    Voice v;
    v.noteOn(&ins, data.data(), 65535, 60, -1, 0, 0, 0, 0);
    renderVoice(v, 1, c);
    CHECK_NEAR(v.rate(), pitchRate(60) + (pitchRate(61) - pitchRate(60)) * 0.5f, 1e-6);
    ins.finetune = -100;
    v.noteOn(&ins, data.data(), 65535, 60, -1, 0, 0, 0, 0);
    renderVoice(v, 1, c);
    CHECK_NEAR(v.rate(), pitchRate(59), 1e-6);
    // Tune + micro-tune FX (34: v-99 cents) and tune FX (40: v-24 semitones).
    ins.finetune = 0;
    v.noteOn(&ins, data.data(), 65535, 60, -1, kFxTune, 36, 0, 0);
    renderVoice(v, 1, c);
    CHECK_NEAR(v.rate(), pitchRate(72), 1e-6);
    // Glide: starts at the previous rate and reaches the new one after g*44.1 samples.
    Voice g;
    g.noteOn(&ins, data.data(), 65535, 60, -1, 0, 0, 0, 0);
    renderVoice(g, 2, c);
    g.noteOn(&ins, data.data(), 65535, 72, -1, kFxGlide, 10, 0, 0);   // 10 -> 204 ms
    renderVoice(g, 1, c);   // FORCED retrigger fade, new note starts in this block
    const float mid = g.rate();
    CHECK(mid > pitchRate(60) && mid < pitchRate(72));
    renderVoice(g, 80, c);   // > 204 ms
    CHECK_NEAR(g.rate(), pitchRate(72), 1e-6);
}

// --- loop modes (spec 04 §2.2) ------------------------------------------------

TEST(loop_modes_boundaries) {
    initTables();
    auto data = rampSample();
    VoiceControl c;
    c.antiAliasing = false;
    const int L1 = 10000, L2 = 12000, END = 30000;
    auto mk = [&](PlayMode m) {
        Instrument ins = plainInstrument(65535, m);
        ins.startPoint = 9000;
        ins.loopPoint1 = L1;
        ins.loopPoint2 = L2;
        ins.endPoint = END;
        return ins;
    };
    {   // one-shot: plays start..end then stops
        Instrument ins = mk(PlayMode::OneShot);
        Voice v;
        v.noteOn(&ins, data.data(), 65535, 60, -1, 0, 0, 0, 0);
        auto out = renderVoice(v, 200, c);
        CHECK_EQ(idxOf(out[0]), 9000);
        int maxIdx = 0;
        size_t lastNz = 0;
        for (size_t k = 0; k < out.size(); ++k)
            if (out[k] != 0.0f) {
                maxIdx = std::max(maxIdx, idxOf(out[k]));
                lastNz = k;
            }
        CHECK(maxIdx <= END - 1 && maxIdx >= END - 2);
        CHECK(lastNz > 20000 && lastNz < 21100);
        CHECK(!v.playing());
    }
    {   // reverse one-shot: end-1 down to start
        Instrument ins = mk(PlayMode::OneShot);
        Voice v;
        v.noteOn(&ins, data.data(), 65535, 60, -1, kFxReverse, 1, 0, 0);
        auto out = renderVoice(v, 200, c);
        CHECK_EQ(idxOf(out[0]), END - 1);
        CHECK(idxOf(out[100]) < END - 90);
        CHECK(!v.playing());
    }
    {   // forward loop
        Instrument ins = mk(PlayMode::ForwardLoop);
        Voice v;
        v.noteOn(&ins, data.data(), 65535, 60, -1, 0, 0, 0, 0);
        auto out = renderVoice(v, 100, c);
        int wraps = 0;
        for (size_t k = 1; k < out.size(); ++k) {
            const int a = idxOf(out[k - 1]), b = idxOf(out[k]);
            CHECK(b < L2);
            if (k > 2000) CHECK(b >= L1);
            if (b < a) {
                ++wraps;
                CHECK(a >= L2 - 2 && b <= L1 + 1);
            }
        }
        CHECK(wraps >= 5);
        CHECK(v.playing());
        v.noteOff(-4);   // loop persists through the release
    }
    {   // backward loop: forward to loop2, then loops downward wrapping at loop1
        Instrument ins = mk(PlayMode::BackwardLoop);
        Voice v;
        v.noteOn(&ins, data.data(), 65535, 60, -1, 0, 0, 0, 0);
        auto out = renderVoice(v, 100, c);
        int jumpsUp = 0, downSteps = 0;
        for (size_t k = 3200; k < out.size(); ++k) {
            const int a = idxOf(out[k - 1]), b = idxOf(out[k]);
            CHECK(b >= L1 && b < L2);
            if (b > a + 2) {
                ++jumpsUp;
                CHECK(a <= L1 + 1 && b >= L2 - 2);
            } else if (b < a) {
                ++downSteps;
            }
        }
        CHECK(jumpsUp >= 4);
        CHECK(downSteps > 5000);
    }
    {   // ping-pong: reflects at both ends, never jumps
        Instrument ins = mk(PlayMode::PingPongLoop);
        Voice v;
        v.noteOn(&ins, data.data(), 65535, 60, -1, 0, 0, 0, 0);
        auto out = renderVoice(v, 100, c);
        int turns = 0;
        int prevDir = 1;
        for (size_t k = 3200; k < out.size(); ++k) {
            const int a = idxOf(out[k - 1]), b = idxOf(out[k]);
            CHECK(b >= L1 && b < L2);
            CHECK(std::abs(b - a) <= 2);
            const int d = b > a ? 1 : (b < a ? -1 : prevDir);
            if (d != prevDir) ++turns;
            prevDir = d;
        }
        CHECK(turns >= 4);
    }
    {   // slices: slice 1 plays from slice[1] to slice[2]
        Instrument ins = plainInstrument(65535, PlayMode::Slice);
        ins.sliceCount = 3;
        ins.slices[0] = 0;
        ins.slices[1] = 20000;
        ins.slices[2] = 21000;
        Voice v;
        v.noteOn(&ins, data.data(), 65535, 72, -1, kFxSlice, 1, 0, 0);
        auto out = renderVoice(v, 10, c);
        CHECK_EQ(idxOf(out[0]), 20000);
        CHECK(idxOf(out[400]) <= 20999);
        CHECK_EQ(out[600], 0.0f);   // 1000 frames at rate ~2 -> ~500 samples
        // Beat slice always plays at note 60.
        ins.playMode = uint8_t(PlayMode::BeatSlice);
        Voice b;
        b.noteOn(&ins, data.data(), 65535, 84, -1, kFxSlice, 1, 0, 0);
        renderVoice(b, 1, c);
        CHECK_NEAR(b.rate(), pitchRate(60), 1e-6);
    }
    {   // wavetable: stays inside the selected window, wraps modulo W
        Instrument ins = plainInstrument(65535, PlayMode::Wavetable);
        ins.wavetableWindowSize = 256;
        ins.wavetableWindowCount = 255;
        ins.wavetableCurrentWindow = 3;
        Voice v;
        v.noteOn(&ins, data.data(), 65535, 60, -1, 0, 0, 0, 0);
        auto out = renderVoice(v, 20, c);
        for (float s : out) CHECK(idxOf(s) >= 768 && idxOf(s) < 1024);
        CHECK_NEAR(v.rate(), wavetablePitchTable(256)[60], 1e-6);
    }
    {   // granular: grain of L samples around P, windowed
        Instrument ins = plainInstrument(65535, PlayMode::Granular);
        ins.grainLength = 1000;
        ins.granularPosition = 30000;
        ins.grainShape = uint8_t(GrainShape::Square);
        Voice v;
        v.noteOn(&ins, data.data(), 65535, 60, -1, 0, 0, 0, 0);
        auto out = renderVoice(v, 40, c);
        for (float s : out) {
            const int i = idxOf(s);
            CHECK(i >= 29500 && i < 30501);
        }
        // Triangle window: output near the grain start is attenuated.
        ins.grainShape = uint8_t(GrainShape::Triangle);
        Voice t;
        t.noteOn(&ins, data.data(), 65535, 60, -1, 0, 0, 0, 0);
        auto o2 = renderVoice(t, 1, c);
        CHECK(std::fabs(o2[0]) < 0.01f);
        CHECK(std::fabs(o2[127]) > 0.05f * std::fabs(float(29500 + 127 - 32768) / 32768.0f));
    }
}

// --- Beat Slice: the note picks the slice (USER_REPORTS_R2 §1, item 1a) -------

TEST(beat_slice_note_selects_slice) {
    initTables();
    auto data = rampSample();
    VoiceControl c;
    c.antiAliasing = false;
    Instrument ins = plainInstrument(65535, PlayMode::BeatSlice);
    ins.sliceCount = 4;
    ins.slices[0] = 0;
    ins.slices[1] = 10000;
    ins.slices[2] = 20000;
    ins.slices[3] = 30000;
    ins.selectedSlice = 0;   // the Sample Playback selection must not matter in Beat Slice
    auto first = [&](uint8_t note, uint8_t fxT = 0, uint8_t fxV = 0) {
        Voice v;
        v.noteOn(&ins, data.data(), 65535, note, -1, fxT, fxV, 0, 0);
        auto out = renderVoice(v, 1, c);
        CHECK_NEAR(v.rate(), pitchRate(60), 1e-6);   // unity regardless of the note
        return idxOf(out[0]);
    };
    for (int k = 0; k < 4; ++k) CHECK_EQ(first(uint8_t(24 + k)), k * 10000);
    CHECK_EQ(first(10), 0);        // below 24 -> first slice
    CHECK_EQ(first(60), 30000);    // past the count -> last slice
    // A Slice FX wins over the note and wraps modulo the count (spec 16 §A.11).
    CHECK_EQ(first(24, kFxSlice, 2), 20000);
    CHECK_EQ(first(27, kFxSlice, 5), 10000);
    // Slice mode (4) keeps playing the selected slice, pitched by the note.
    ins.playMode = uint8_t(PlayMode::Slice);
    ins.selectedSlice = 2;
    Voice s;
    s.noteOn(&ins, data.data(), 65535, 72, -1, 0, 0, 0, 0);
    auto out = renderVoice(s, 1, c);
    CHECK_EQ(idxOf(out[0]), 20000);
    CHECK_NEAR(s.rate(), pitchRate(72), 1e-6);
}

// --- envelope (spec 04 §4.7) --------------------------------------------------

TEST(envelope_timing) {
    initTables();
    std::vector<int16_t> dc(65535, 16384);
    Instrument ins = plainInstrument(65535, PlayMode::ForwardLoop);
    ins.env[0].enabled = 1;
    ins.env[0].amount = 1.0f;
    ins.env[0].attackMs = 100;
    ins.env[0].holdMs = 0;
    ins.env[0].decayMs = 100;
    ins.env[0].sustain = 0.5f;
    ins.env[0].releaseMs = 200;
    Voice v;
    v.noteOn(&ins, dc.data(), 65535, 60, -1, 0, 0, 0, 0);
    auto out = renderVoice(v, 200, VoiceControl{});   // 580 ms
    auto at = [&](float ms) { return out[size_t(ms * 44.1176f)] * 2.0f; };   // DC is 0.5
    CHECK_NEAR(at(50.0f), 0.5f, 0.02);
    CHECK_NEAR(at(100.0f), 1.0f, 0.02);
    CHECK_NEAR(at(150.0f), 0.75f, 0.02);
    CHECK_NEAR(at(210.0f), 0.5f, 0.01);
    CHECK_NEAR(at(500.0f), 0.5f, 0.01);
    v.noteOff(-4);
    auto rel = renderVoice(v, 80, VoiceControl{});   // 232 ms
    CHECK_NEAR(rel[size_t(100.0f * 44.1176f)] * 2.0f, 0.25f, 0.02);
    CHECK_EQ(rel[size_t(205.0f * 44.1176f)], 0.0f);
    CHECK(!v.active());
    // Cut stops immediately; fade is short.
    v.noteOn(&ins, dc.data(), 65535, 60, -1, 0, 0, 0, 0);
    renderVoice(v, 10, VoiceControl{});
    v.noteOff(-3);
    CHECK(!v.active());
    v.noteOn(&ins, dc.data(), 65535, 60, -1, 0, 0, 0, 0);
    renderVoice(v, 10, VoiceControl{});
    v.noteOff(-2);
    renderVoice(v, 6, VoiceControl{});   // 17 ms > 10 ms fade
    CHECK(!v.active());
    // Volume LFO (looping envelope): triangle at 1 step per cycle, 120 BPM -> 125 ms period.
    Instrument lfo = ins;
    lfo.env[0].loop = 1;
    lfo.lfo[0].shape = uint8_t(LfoShape::Triangle);
    lfo.lfo[0].speed = 9;     // RATE_V[9] = 1 step
    lfo.lfo[0].amount = 1.0f;
    VoiceControl c120;
    c120.tempoBpm = 120.0f;
    Voice w;
    w.control(c120);
    w.noteOn(&lfo, dc.data(), 65535, 60, -1, 0, 0, 0, 0);
    auto lo = renderVoice(w, 60, c120);
    int peaks = 0;
    for (size_t k = 1; k + 1 < lo.size(); ++k)
        if (lo[k] > 0.49f && lo[k] >= lo[k - 1] && lo[k] > lo[k + 1]) ++peaks;
    // 60 blocks = 174 ms -> 1..2 peaks at a 125 ms period.
    CHECK(peaks >= 1 && peaks <= 2);
    float mn = 1.0f;
    for (size_t k = 3000; k < 6000; ++k) mn = std::fmin(mn, lo[k]);
    CHECK(mn < 0.05f);
}

// --- bit depth (spec 04 §4.6) -------------------------------------------------

// OpenTracker rounds to the nearest step of stock's mask grid (stock floors,
// which turns quiet material into a negative pulse train with DC).
TEST(bit_depth_quantiser_is_mid_tread) {
    initTables();
    // -60 dBFS sine at 8 bits (step 256): every sample is within half a step
    // of 0, so the output is exactly silent (floor: -256 on each negative half).
    const auto quiet = sine(44100, 441.0f, 0.001f);
    Instrument ins = plainInstrument(44100, PlayMode::ForwardLoop);
    ins.bitDepth = 8;
    Voice v;
    v.noteOn(&ins, quiet.data(), 44100, 60, -1, 0, 0, 0, 0);
    auto out = renderVoice(v, 40);
    float mx = 0.0f;
    for (size_t i = kBlock; i < out.size(); ++i) mx = std::fmax(mx, std::fabs(out[i]));
    CHECK_EQ(mx, 0.0f);
    // 0.89 full-scale sine at 4 bits: no DC (floor: about -1/16).
    const auto loud = sine(44100, 441.0f, 0.89f);
    ins.bitDepth = 4;
    Voice w;
    w.noteOn(&ins, loud.data(), 44100, 60, -1, 0, 0, 0, 0);
    out = renderVoice(w, 101);
    double sum = 0.0;
    for (size_t i = kBlock; i < out.size(); ++i) sum += out[i];
    const double mean = sum / double(out.size() - kBlock);
    CHECK(std::fabs(mean) < 1.0 / 256.0);
}

// --- filter (spec 04 §4.4) ----------------------------------------------------

namespace {
float svfGain(int type, float fc, float f, float res = 0.0f) {
    Svf s;
    const float fm = svfFmult(fc), dp = svfDamp(res);
    s.fm = fm;
    s.dp = dp;
    float x[kBlock];
    float peak = 0.0f;
    uint32_t n = 0;
    for (int b = 0; b < 120; ++b) {
        for (uint32_t i = 0; i < kBlock; ++i, ++n) x[i] = 0.25f * std::sin(2.0f * kPiF * f * float(n) / audio::kSampleRate);
        s.process(x, kBlock, type, fm, dp);
        if (b >= 60) peak = std::fmax(peak, peakOf(x, kBlock));
    }
    return peak / 0.25f;
}
}  // namespace

TEST(filter_response) {
    initTables();
    const float fc = 1000.0f;
    CHECK_NEAR(svfGain(0, fc, 100.0f), 1.0f, 0.03);
    CHECK_NEAR(svfGain(0, fc, fc), 0.707f, 0.08);       // ~ -3 dB at cutoff (q = 0.7)
    CHECK(svfGain(0, fc, 8000.0f) < 0.03f);             // 12 dB/oct
    CHECK_NEAR(svfGain(1, fc, 8000.0f), 1.0f, 0.1);
    CHECK_NEAR(svfGain(1, fc, fc), 0.707f, 0.08);
    CHECK(svfGain(1, fc, 100.0f) < 0.03f);
    CHECK(svfGain(2, fc, fc) > 0.5f);
    CHECK(svfGain(2, fc, 50.0f) < 0.1f);
    // Resonance raises the gain at cutoff (q = 5 -> ~ +14 dB).
    CHECK(svfGain(0, fc, fc, 4.3f) > 3.5f);
    // Mapped cutoff c = 0.5 -> 20 * 2^(238/50) Hz.
    const float c05 = cutoffHz(0.5f);
    CHECK_NEAR(c05, 20.0f * std::exp2(238.0f / 50.0f), 0.01);
    CHECK_NEAR(svfGain(0, c05, c05), 0.707f, 0.08);
}

// --- delay (spec 04 §5.1) -----------------------------------------------------

TEST(delay_time_accuracy) {
    initTables();
    std::vector<int16_t> mem(Delay::kMemorySamples);
    Delay d;
    d.init(mem.data());
    d.setFilter(20000.0f);
    d.setFeedbackK(0);
    const uint32_t T = delayTimeSamples(250.0f);
    CHECK_EQ(T, 11025u);
    d.setTimeSamples(T);
    float in[kBlock] = {}, l[kBlock], r[kBlock];
    in[0] = 0.5f;
    int64_t argmax = -1;
    float best = 0.0f;
    for (uint32_t b = 0; b < 100; ++b) {
        d.process(in, l, r);
        in[0] = 0.0f;
        for (uint32_t i = 0; i < kBlock; ++i)
            if (std::fabs(l[i]) > best) {
                best = std::fabs(l[i]);
                argmax = int64_t(b * kBlock + i);
            }
    }
    CHECK_EQ(argmax, int64_t(T));
    CHECK(best > 0.3f);
    // Feedback + ping-pong: echoes alternate R, L, R ... (input feeds line B first).
    std::vector<int16_t> mem2(Delay::kMemorySamples);
    Delay p;
    p.init(mem2.data());
    p.setFilter(20000.0f);
    p.setTimeSamples(1000);
    p.setFeedbackK(delayFeedbackK(80));
    p.setPingPong(true);
    std::vector<float> L, R;
    float imp[kBlock] = {};
    imp[0] = 0.5f;
    for (int b = 0; b < 40; ++b) {
        p.process(imp, l, r);
        imp[0] = 0.0f;
        L.insert(L.end(), l, l + kBlock);
        R.insert(R.end(), r, r + kBlock);
    }
    auto en = [&](const std::vector<float>& x, int at) {
        float s = 0;
        for (int i = at - 5; i < at + 10; ++i) s += std::fabs(x[size_t(i)]);
        return s;
    };
    CHECK(en(R, 1000) > 0.3f && en(L, 1000) < 0.01f);
    CHECK(en(L, 2000) > 0.2f && en(R, 2000) < 0.01f);
    CHECK(en(R, 3000) > 0.1f);
}

// Switching ping-pong while echoes ring continues the tail (stock writes all
// three lines every sample; the mode only picks the output).
TEST(delay_pingpong_switch_keeps_tail) {
    initTables();
    for (int dir = 0; dir < 2; ++dir) {
        std::vector<int16_t> mem(Delay::kMemorySamples);
        Delay d;
        d.init(mem.data());
        d.setFilter(20000.0f);
        d.setTimeSamples(1000);
        d.setFeedbackK(delayFeedbackK(80));
        d.setPingPong(dir == 1);
        float x[kBlock], l[kBlock], r[kBlock];
        uint32_t n = 0;
        auto rms = [&]() {
            double e = 0;
            for (uint32_t i = 0; i < kBlock; ++i) e += double(l[i]) * l[i] + double(r[i]) * r[i];
            return std::sqrt(e / (2.0 * kBlock));
        };
        // 2048-sample sine burst, then silence while the echoes ring.
        double before = 0;
        for (int b = 0; b < 40; ++b) {
            for (uint32_t i = 0; i < kBlock; ++i, ++n) x[i] = n < 2048 ? 0.5f * std::sin(2.0f * kPiF * 300.0f * float(n) / 44100.0f) : 0.0f;
            d.process(x, l, r);
            before = rms();
        }
        CHECK(before > 0.05);
        d.setPingPong(dir == 0);
        for (uint32_t i = 0; i < kBlock; ++i) x[i] = 0.0f;
        d.process(x, l, r);
        const double after = rms();
        std::printf("  ping-pong %s: rms %.4f -> %.4f\n", dir == 0 ? "on" : "off", before, after);
        CHECK(after > before * 0.5 && after < before * 2.0);   // within 6 dB
    }
}

// --- limiter (spec 04 §5.3) ---------------------------------------------------

TEST(limiter_ceiling) {
    initTables();
    Limiter lim;
    const int v = 32767 * 80 / 100 + 1;   // index 80 -> LIMTHR 6045 (-14.7 dB)
    const float thr = limiterThreshold(v);
    CHECK_NEAR(thr, 6045.0f / 32767.0f, 1e-6);
    lim.setThreshold(thr);
    lim.setCoefs(limiterCoef(1.0f, 0), limiterCoef(50.0f, 0));
    float x[kBlock], y[kBlock];
    float peak = 0.0f;
    uint32_t n = 0;
    for (int b = 0; b < 200; ++b) {
        for (uint32_t i = 0; i < kBlock; ++i, ++n) x[i] = 0.9f * std::sin(2.0f * kPiF * 200.0f * float(n) / audio::kSampleRate);
        lim.process(x, nullptr, y);
        if (b >= 100) peak = std::fmax(peak, peakOf(y, kBlock));
    }
    CHECK(peak <= thr * 1.12f);
    CHECK(peak >= thr * 0.6f);
    CHECK(lim.meanGain() < 0.3f);
    // Below threshold: unity, 4-sample look-ahead delay.
    Limiter u;
    u.setThreshold(1.0f);
    u.setCoefs(limiterCoef(1.0f, 0), limiterCoef(50.0f, 0));
    float imp[kBlock] = {};
    imp[0] = 0.5f;
    u.process(imp, nullptr, y);
    CHECK_NEAR(y[4], 0.5f, 1e-6);
    CHECK_EQ(y[0], 0.0f);
    // Side-chain: ducks the main input, mixes the trigger back.
    Limiter s;
    s.setThreshold(0.1f);
    s.setCoefs(limiterCoef(1.0f, 0), limiterCoef(50.0f, 0));
    float main[kBlock], sc[kBlock];
    for (int b = 0; b < 20; ++b) {
        for (uint32_t i = 0; i < kBlock; ++i) {
            main[i] = 0.5f;
            sc[i] = 0.8f;
        }
        s.process(main, sc, y);
    }
    CHECK(s.meanGain() < 0.2f);
    CHECK(y[100] > 0.6f);   // mostly side-chain signal
}

// --- reverb (spec 04 §5.2) ----------------------------------------------------

TEST(reverb_tail) {
    initTables();
    std::vector<int16_t> mem(Reverb::kMemorySamples);
    Reverb rv;
    rv.init(mem.data());
    rv.setSize(0.7f);
    rv.setDiffusion(0.625f);
    rv.setDamping(0.2f);
    rv.setPredelay(0.0f);
    float in[kBlock] = {}, l[kBlock], r[kBlock];
    for (uint32_t i = 0; i < 64; ++i) in[i] = 0.9f;
    float e1 = 0, e2 = 0;
    for (int b = 0; b < 400; ++b) {
        rv.process(in, l, r);
        for (uint32_t i = 0; i < kBlock; ++i) {
            in[i] = 0.0f;
            CHECK(std::isfinite(l[i]) && std::isfinite(r[i]));
            if (b > 20 && b < 60) e1 += l[i] * l[i] + r[i] * r[i];
            if (b > 300) e2 += l[i] * l[i] + r[i] * r[i];
        }
    }
    CHECK(e1 > 1e-4f);    // a tail exists
    CHECK(e2 < e1);       // and decays
    // Pre-delay 1.0 -> 5000 samples before anything comes out.
    std::vector<int16_t> mem2(Reverb::kMemorySamples);
    Reverb pd;
    pd.init(mem2.data());
    pd.setPredelay(1.0f);
    pd.setAmount(1.0f);
    float imp[kBlock] = {};
    imp[0] = 0.9f;
    int first = -1;
    for (int b = 0; b < 60 && first < 0; ++b) {
        pd.process(imp, l, r);
        imp[0] = 0.0f;
        for (uint32_t i = 0; i < kBlock; ++i)
            if (first < 0 && std::fabs(l[i]) > 1e-4f) first = int(b * int(kBlock) + int(i));
    }
    CHECK(first >= 5000);
}

// --- sample bank (spec 02 §8.2) -----------------------------------------------

TEST(sample_bank_packing) {
    std::vector<int16_t> pool(1000);
    SampleBank bank;
    bank.init(pool.data(), pool.size());
    std::vector<int16_t> a(100, 1), b(200, 2), c(50, 3);
    CHECK(bank.store(0, a.data(), 100, false));
    CHECK(bank.store(5, b.data(), 100, true));    // planar stereo, 200 samples
    CHECK_EQ(bank.left(5) - bank.left(0), 100);
    CHECK_EQ(bank.right(5) - bank.left(5), 100);
    CHECK(bank.store(2, c.data(), 50, false));    // inserted between: 5 moves up
    CHECK_EQ(bank.left(2) - bank.left(0), 100);
    CHECK_EQ(bank.left(5) - bank.left(0), 150);
    CHECK_EQ(bank.left(5)[0], 2);
    CHECK_EQ(bank.usedSamples(), 350u);
    CHECK(!bank.store(7, nullptr, 700, false));   // not enough memory
    bank.remove(2);
    CHECK_EQ(bank.left(5) - bank.left(0), 100);
    CHECK_EQ(bank.left(5)[199], 2);
    CHECK(!bank.loaded(2));
    CHECK_EQ(bank.usedSamples(), 300u);
}

// The Sample Editor's apply / undo (SampleBank::replaceFromTail): the new data
// waits in tail(); afterwards the slot holds it and tail() holds the old data.
// Same size: the two swap, nothing else moves. Another size: the slots after
// it move.
TEST(sample_bank_replace_from_tail) {
    std::vector<int16_t> pool(2000);
    SampleBank bank;
    bank.init(pool.data(), pool.size());
    std::vector<int16_t> a(100), b(300), c(50);
    for (int i = 0; i < 100; ++i) a[size_t(i)] = int16_t(1000 + i);
    for (int i = 0; i < 300; ++i) b[size_t(i)] = int16_t(2000 + i);
    for (int i = 0; i < 50; ++i) c[size_t(i)] = int16_t(3000 + i);
    CHECK(bank.store(0, a.data(), 100, false));
    CHECK(bank.store(1, b.data(), 150, true));   // stereo, 300 samples
    CHECK(bank.store(2, c.data(), 50, false));
    const int16_t* c0 = bank.left(2);
    // Same size, stereo: 300 new samples into slot 1 (a 300-sample swap, in
    // 128-sample blocks plus a partial one).
    for (int i = 0; i < 300; ++i) bank.tail()[i] = int16_t(-i);
    CHECK(bank.replaceFromTail(1, 150, true));
    CHECK_EQ(bank.left(2), c0);                  // nothing after it moved
    for (int i = 0; i < 300; ++i) CHECK_EQ(bank.left(1)[i], -i);
    for (int i = 0; i < 300; ++i) CHECK_EQ(bank.tail()[i], 2000 + i);   // the old data, for undo
    for (int i = 0; i < 50; ++i) CHECK_EQ(bank.left(2)[i], 3000 + i);
    for (int i = 0; i < 100; ++i) CHECK_EQ(bank.left(0)[i], 1000 + i);
    // Undo: the old data back in.
    CHECK(bank.replaceFromTail(1, 150, true));
    for (int i = 0; i < 300; ++i) CHECK_EQ(bank.left(1)[i], 2000 + i);
    // Another size: slot 0 grows from 100 to 130 frames, slots 1 and 2 move up.
    for (int i = 0; i < 130; ++i) bank.tail()[i] = int16_t(5000 + i);
    CHECK(bank.replaceFromTail(0, 130, false));
    CHECK_EQ(bank.left(1) - bank.left(0), 130);
    CHECK_EQ(bank.left(2) - bank.left(0), 430);
    for (int i = 0; i < 130; ++i) CHECK_EQ(bank.left(0)[i], 5000 + i);
    for (int i = 0; i < 300; ++i) CHECK_EQ(bank.left(1)[i], 2000 + i);
    for (int i = 0; i < 50; ++i) CHECK_EQ(bank.left(2)[i], 3000 + i);
    for (int i = 0; i < 100; ++i) CHECK_EQ(bank.tail()[i], 1000 + i);
    CHECK_EQ(bank.usedSamples(), 480u);
    // Shrink: slot 1 from 150 stereo frames to 40 mono, slots after it move down.
    for (int i = 0; i < 40; ++i) bank.tail()[i] = int16_t(7000 + i);
    CHECK(bank.replaceFromTail(1, 40, false));
    CHECK_EQ(bank.left(2) - bank.left(1), 40);
    for (int i = 0; i < 40; ++i) CHECK_EQ(bank.left(1)[i], 7000 + i);
    for (int i = 0; i < 50; ++i) CHECK_EQ(bank.left(2)[i], 3000 + i);
    for (int i = 0; i < 130; ++i) CHECK_EQ(bank.left(0)[i], 5000 + i);
    for (int i = 0; i < 300; ++i) CHECK_EQ(bank.tail()[i], 2000 + i);   // the old stereo data
    CHECK_EQ(bank.usedSamples(), 220u);
}

// Size changes with too little free memory to park the blocks: the rotation
// fallback gives the same layout.
TEST(sample_bank_replace_from_tail_tight) {
    std::vector<int16_t> pool(430);
    SampleBank bank;
    bank.init(pool.data(), pool.size());
    std::vector<int16_t> a(100), b(200);
    for (int i = 0; i < 100; ++i) a[size_t(i)] = int16_t(1000 + i);
    for (int i = 0; i < 200; ++i) b[size_t(i)] = int16_t(2000 + i);
    CHECK(bank.store(0, a.data(), 100, false));
    CHECK(bank.store(1, b.data(), 200, false));
    // Grow slot 0 to 120: 10 samples free after N, 220 needed to park.
    for (int i = 0; i < 120; ++i) bank.tail()[i] = int16_t(5000 + i);
    CHECK(bank.replaceFromTail(0, 120, false));
    for (int i = 0; i < 120; ++i) CHECK_EQ(bank.left(0)[i], 5000 + i);
    for (int i = 0; i < 200; ++i) CHECK_EQ(bank.left(1)[i], 2000 + i);
    for (int i = 0; i < 100; ++i) CHECK_EQ(bank.tail()[i], 1000 + i);
    // Shrink slot 1 to 30: 80 free after N, 200 needed to park.
    for (int i = 0; i < 30; ++i) bank.tail()[i] = int16_t(6000 + i);
    CHECK(bank.replaceFromTail(1, 30, false));
    for (int i = 0; i < 120; ++i) CHECK_EQ(bank.left(0)[i], 5000 + i);
    for (int i = 0; i < 30; ++i) CHECK_EQ(bank.left(1)[i], 6000 + i);
    for (int i = 0; i < 200; ++i) CHECK_EQ(bank.tail()[i], 2000 + i);
}

// --- full engine --------------------------------------------------------------

TEST(engine_extremes_no_clip_nan) {
    Rig rig;
    auto s = sine(44100, 220.0f, 1.0f);
    auto noise = std::vector<int16_t>(44100);
    uint32_t seed = 1;
    for (auto& v : noise) {
        seed = seed * 1103515245u + 12345u;
        v = int16_t(seed >> 16);
    }
    rig.load(0, s, PlayMode::PingPongLoop);
    rig.load(1, noise, PlayMode::Granular);
    for (int i = 0; i < 2; ++i) {
        Instrument& in = rig.ins[i];
        in.volume = 100;
        in.overdrive = 100;
        in.filterEnabled = 1;
        in.resonance = 4.3f;
        in.cutoff = 0.3f;
        in.delaySend = 100;
        in.reverbSend = 100;
        in.bitDepth = 6;
        in.env[2].enabled = 1;
        in.env[2].loop = 1;   // cutoff LFO
        in.lfo[2].speed = 20;
        in.lfo[2].amount = 1.0f;
        in.env[1].enabled = 1;   // pan envelope
        in.env[1].attackMs = 10;
    }
    MixerSettings& m = rig.eng->settings();
    for (auto& l : m.trackLevel) l = 100;
    m.dryMix = 100;
    m.masterVolume = 100;
    m.delayFeedback = 100;
    m.delayTimeMs = 30;
    m.delayPingPong = true;
    m.reverbSize = 1.0f;
    m.reverbDiffusion = 1.0f;
    m.reverbDamping = 0.0f;
    m.delayLevel = 100;
    m.reverbLevel = 100;
    int16_t out[kBlock * 2];
    for (int mode = 0; mode <= 4; ++mode) {
        m.saturationMode = uint8_t(mode);
        for (int t = 0; t < 8; ++t) rig.note(uint8_t(t), uint8_t(t & 1), uint8_t(24 + 12 * t), kFxFilterLfo, 25);
        int16_t mn = 0, mx = 0;
        for (int b = 0; b < 150; ++b) {
            rig.eng->renderLive(out);
            for (int16_t v : out) {
                mn = std::min(mn, v);
                mx = std::max(mx, v);
            }
        }
        CHECK(mx > 1000 && mn < -1000);   // produces signal
        CHECK(std::isfinite(rig.eng->reverbLevel()));
        CHECK(std::isfinite(rig.eng->limiterGainReduction()));
        for (int t = 0; t < 8; ++t) CHECK(std::isfinite(rig.eng->voice(t).envelopeLevel()));
    }
    // With the limiter engaged the output peak respects its ceiling (+ overshoot margin).
    m.saturationMode = 0;
    m.masterVolume = 86;
    m.limiterThreshold = 32767 * 90 / 100 + 1;   // LIMTHR[90] = 14074 (-7.3 dB)
    m.limiterAttackMs = 0.5f;
    int peak = 0;
    for (int b = 0; b < 300; ++b) {
        rig.eng->renderLive(out);
        if (b > 100)
            for (int16_t v : out) peak = std::max(peak, std::abs(int(v)));
    }
    CHECK(peak <= 14074 * 13 / 10);
}

TEST(engine_mixer_routing) {
    Rig rig;
    rig.load(0, sine(44100, 441.0f, 0.5f), PlayMode::ForwardLoop);
    MixerSettings& m = rig.eng->settings();
    int16_t out[kBlock * 2];
    auto rms = [&](int blocks, bool left) {
        double s = 0;
        for (int b = 0; b < blocks; ++b) {
            rig.eng->renderLive(out);
            for (uint32_t i = 0; i < kBlock; ++i) s += double(out[2 * i + (left ? 0 : 1)]) * out[2 * i + (left ? 0 : 1)];
        }
        return std::sqrt(s / (blocks * kBlock));
    };
    rig.note(0, 0, 60);
    rms(5, true);
    const double ref = rms(20, true);
    CHECK(ref > 5000.0);   // 0.5 amplitude sine -> ~11585 RMS
    // Pan hard left: right channel silent.
    rig.ins[0].panning = 0;
    rms(2, true);
    CHECK(rms(10, false) < 1.0);
    rig.ins[0].panning = 50;
    // Track level 1 = -36 dB.
    m.trackLevel[0] = 1;
    rms(2, true);
    CHECK_NEAR(20.0 * std::log10(rms(20, true) / ref), -36.0, 0.5);
    m.trackLevel[0] = 86;
    // Mute / solo.
    m.trackMute[0] = true;
    rms(2, true);
    CHECK(rms(10, true) < 1.0);
    m.trackMute[0] = false;
    m.trackSolo[3] = true;
    rms(2, true);
    CHECK(rms(10, true) < 1.0);
    m.trackSolo[3] = false;
    // Master volume 100 -> +6 dB (no limiter engaged).
    m.masterVolume = 100;
    rms(2, true);
    CHECK_NEAR(20.0 * std::log10(rms(20, true) / ref), 6.0, 0.3);
    m.masterVolume = 86;
    // Extra headroom: -6.02 dB.
    m.extraHeadroom = true;
    rms(2, true);
    CHECK_NEAR(20.0 * std::log10(rms(20, true) / ref), -6.02, 0.3);
    m.extraHeadroom = false;
    // Line input: stereo passes through, left->both mode duplicates left.
    rig.eng->voiceNoteOff(0, NoteOffMode::Cut);
    int16_t line[kBlock * 2];
    for (uint32_t i = 0; i < kBlock; ++i) {
        line[2 * i] = 8000;
        line[2 * i + 1] = 0;
    }
    rig.eng->setLineInput(line);
    rig.eng->renderLive(out);
    CHECK(out[2 * 100] > 7900 && out[2 * 100] < 8100);
    CHECK_EQ(out[2 * 100 + 1], 0);
    m.lineInChannel = 1;
    rig.eng->setLineInput(line);
    rig.eng->renderLive(out);
    CHECK(out[2 * 100 + 1] > 7900);
    rig.eng->renderLive(out);   // no input -> silence
    CHECK_EQ(out[2 * 100], 0);
}

namespace {
uint64_t scriptedRender(Rig& rig) {
    rig.load(0, sine(30000, 220.0f), PlayMode::ForwardLoop);
    auto saw = std::vector<int16_t>(20000);
    for (size_t i = 0; i < saw.size(); ++i) saw[i] = int16_t((int(i * 300) % 65536) - 32768);
    rig.load(1, saw, PlayMode::OneShot);
    std::vector<int16_t> wt(2048 * 8);
    for (size_t i = 0; i < wt.size(); ++i)
        wt[i] = int16_t(20000.0f * std::sin(2.0f * kPiF * float(i % 2048) / 2048.0f * float(1 + i / 2048)));
    rig.load(2, wt, PlayMode::Wavetable);
    rig.ins[2].wavetableWindowSize = 2048;
    rig.ins[2].wavetableWindowCount = 8;
    rig.ins[2].env[3].enabled = 1;
    rig.ins[2].env[3].loop = 1;
    rig.ins[2].lfo[3].speed = 14;
    rig.ins[0].delaySend = 60;
    rig.ins[1].reverbSend = 80;
    rig.ins[1].filterEnabled = 1;
    rig.ins[1].cutoff = 0.6f;
    rig.ins[0].env[0].attackMs = 20;
    rig.ins[0].env[0].releaseMs = 100;
    MixerSettings& m = rig.eng->settings();
    m.delayTimeMs = 120;
    m.delayFeedback = 60;
    m.saturationMode = 2;
    m.limiterThreshold = 30000;
    uint64_t h = 1469598103934665603ull;
    int16_t master[kBlock * 2];
    StemBlock* stems = new StemBlock;
    rig.eng->beginOffline();
    for (int b = 0; b < 600; ++b) {
        if (b == 0) rig.note(0, 0, 60);
        if (b == 10) rig.note(1, 1, 48, kFxBitDepth, 8, kFxPanning, 20);
        if (b == 30) rig.note(2, 2, 64, 0, 0, 0, 0, 80);
        if (b == 50) rig.note(0, 0, 67, kFxGlide, 20);
        if (b == 80) rig.eng->voiceApplyFx(1, 0, kFxHighPass, 40);
        if (b == 120) rig.eng->voiceNoteOff(0, NoteOffMode::Off);
        if (b == 200) rig.note(3, 1, 55, kFxReverse, 1, kFxReverbSend, 100);
        if (b == 300) rig.eng->voiceNoteOff(2, NoteOffMode::Fade);
        rig.eng->renderOfflineBlock(master, stems);
        h = fnv(h, master, kBlock * 2);
        h = fnv(h, stems->data[0], kBlock * 2);
    }
    rig.eng->endOffline();
    delete stems;
    return h;
}
}  // namespace

TEST(deterministic_render_hash) {
    Rig a, b;
    const uint64_t ha = scriptedRender(a);
    const uint64_t hb = scriptedRender(b);
    CHECK_EQ(ha, hb);
    std::printf("  scripted render hash: %016llx\n", (unsigned long long)ha);
    // Golden value (host x86-64 SSE float); update deliberately when the DSP changes.
    // 1.9.9: bit depth rounds to nearest (was 0x4e126ff813060a1b with the floor).
#if defined(__x86_64__)
    CHECK_EQ(ha, 0xe2e2f8dd73d3e8d6ull);   // spec 07: Q9 delay filter, Q5 bipolar LFOs
#else
    std::puts("  x86-64 render golden skipped on this host architecture");
#endif
}

TEST(export_stems_wav) {
    Rig rig;
    rig.load(0, sine(20000, 330.0f), PlayMode::OneShot);
    MemStorage fs;
    struct Ctx {
        Rig* rig;
        uint32_t blocks;
    } ctx{&rig, 0};
    // The renderer resets all tracks first (spec 04 §6.1); notes come from the
    // per-block callback (normally the sequencer).
    auto running = [](void* c) {
        Ctx* x = static_cast<Ctx*>(c);
        if (x->blocks == 0) x->rig->note(0, 0, 60);
        return ++x->blocks <= 50;
    };
    const int32_t n =
        rig.eng->exportWav(fs, "Export/p/p_P1_S0001", true, 1000, running, &ctx, /*resampleTo44100=*/true);
    CHECK_EQ(n, 50);
    const char* names[] = {"track1", "track2", "track8", "reverb", "delay", "master"};
    for (const char* nm : names) {
        std::string p = std::string("Export/p/p_P1_S0001/") + nm + ".wav";
        CHECK(fs.files.count(p) == 1);
        const auto& f = fs.files[p];
        CHECK_EQ(f.size(), 44u + 6397u * 4u);
        if (f.size() < 44) continue;
        CHECK_EQ(uint32_t(f[24] | f[25] << 8 | f[26] << 16 | f[27] << 24), 44100u);
        CHECK_EQ(uint32_t(f[28] | f[29] << 8 | f[30] << 16 | f[31] << 24), 176400u);
    }
    // track1 carries the note, track2 is silent.
    const auto& t1 = fs.files["Export/p/p_P1_S0001/track1.wav"];
    const auto& t2 = fs.files["Export/p/p_P1_S0001/track2.wav"];
    int nz1 = 0, nz2 = 0;
    for (size_t i = 44; i < t1.size(); ++i) nz1 += t1[i] != 0;
    for (size_t i = 44; i < t2.size(); ++i) nz2 += t2[i] != 0;
    CHECK(nz1 > 1000);
    CHECK_EQ(nz2, 0);
    // Master-only export.
    rig.note(0, 0, 60);
    CHECK_EQ(rig.eng->exportWav(fs, "Export/p/p_Song_0001.wav", false, 20, nullptr, nullptr,
                                /*resampleTo44100=*/true),
             20);
    CHECK_EQ(fs.files["Export/p/p_Song_0001.wav"].size(), 44u + 2558u * 4u);
}

// Stock export (spec 04 §6.1): blocks x 128 device-rate frames, 44117 Hz and
// byte rate 44117 x 4 in the header.
TEST(export_stems_wav_device_rate) {
    Rig rig;
    rig.load(0, sine(20000, 330.0f), PlayMode::OneShot);
    MemStorage fs;
    struct Ctx {
        Rig* rig;
        uint32_t blocks;
    } ctx{&rig, 0};
    auto running = [](void* c) {
        Ctx* x = static_cast<Ctx*>(c);
        if (x->blocks == 0) x->rig->note(0, 0, 60);
        return ++x->blocks <= 50;
    };
    const int32_t n =
        rig.eng->exportWav(fs, "Export/p/p_P1_S0001", true, 1000, running, &ctx, /*resampleTo44100=*/false);
    CHECK_EQ(n, 50);
    const char* names[] = {"track1", "track2", "track8", "reverb", "delay", "master"};
    for (const char* nm : names) {
        std::string p = std::string("Export/p/p_P1_S0001/") + nm + ".wav";
        CHECK(fs.files.count(p) == 1);
        const auto& f = fs.files[p];
        CHECK_EQ(f.size(), 44u + 50u * kBlock * 4u);
        if (f.size() < 44) continue;
        CHECK_EQ(uint32_t(f[24] | f[25] << 8 | f[26] << 16 | f[27] << 24), 44117u);
        CHECK_EQ(uint32_t(f[28] | f[29] << 8 | f[30] << 16 | f[31] << 24), 176468u);
    }
    // Master-only export.
    rig.note(0, 0, 60);
    CHECK_EQ(rig.eng->exportWav(fs, "Export/p/p_Song_0001.wav", false, 20, nullptr, nullptr,
                                /*resampleTo44100=*/false),
             20);
    const auto& m = fs.files["Export/p/p_Song_0001.wav"];
    CHECK_EQ(m.size(), 44u + 20u * kBlock * 4u);
    if (m.size() < 44) return;
    CHECK_EQ(uint32_t(m[24] | m[25] << 8 | m[26] << 16 | m[27] << 24), 44117u);
    CHECK_EQ(uint32_t(m[28] | m[29] << 8 | m[30] << 16 | m[31] << 24), 176468u);
}

namespace {
// Export a held ACD sine and check the WAV, then the instrument that the
// Render & Load import (wavImport with stereo on) makes from it.
void checkSynthExport(bool resampleTo44100) {
    Rig rig;
    SynthPatch patch;
    CHECK(patchInit(kSynthEngineAcd, &patch));
    patch.params[acd::kAmpS] = PatchParam{patchFloatToBits(1.0f), true};
    SynthBank& synth = rig.eng->synth();
    synth.setEngineOverride(synthTestEngine());
    synth.bindPatch(0, &patch);
    SynthSlotRecord record = SynthSlotRecord::defaults(0);
    record.model = uint32_t(SynthModel::ACD);
    record.polyphony = 1;
    synth.setSlotRecord(0, record);

    constexpr uint32_t kBlocks = 2600;  // A sustained note for a little over four bars at 130 BPM.
    struct Ctx {
        AudioEngine* engine;
        uint32_t blocks = 0;
    } ctx{rig.eng.get()};
    auto running = [](void* p) {
        Ctx& c = *static_cast<Ctx*>(p);
        if (c.blocks == 0) {
            SynthNoteOn note;
            note.slot = 0;
            note.note = 60;  // C4, 261.6256 Hz.
            c.engine->synthNoteOn(0, note);
        }
        return ++c.blocks <= kBlocks;
    };

    MemStorage fs;
    CHECK_EQ(rig.eng->exportWav(fs, "Export/synth", true, kBlocks, running, &ctx, resampleTo44100),
             int32_t(kBlocks));
    const auto& wav = fs.files["Export/synth/synth_1.wav"];
    constexpr size_t kHeaderBytes = 44;
    const uint32_t deviceFrames = kBlocks * kBlock;
    const uint32_t expectedFrames = resampleTo44100 ? uint32_t(uint64_t(deviceFrames) * 2499 / 2500) : deviceFrames;
    CHECK_EQ(wav.size(), kHeaderBytes + size_t(expectedFrames) * 4);
    CHECK(wav.size() >= kHeaderBytes);
    if (wav.size() < kHeaderBytes) return;
    const uint32_t sampleRate = uint32_t(wav[24]) | (uint32_t(wav[25]) << 8) | (uint32_t(wav[26]) << 16) |
                                (uint32_t(wav[27]) << 24);
    const uint32_t byteRate = uint32_t(wav[28]) | (uint32_t(wav[29]) << 8) | (uint32_t(wav[30]) << 16) |
                              (uint32_t(wav[31]) << 24);
    CHECK_EQ(sampleRate, resampleTo44100 ? 44100u : 44117u);
    CHECK_EQ(byteRate, sampleRate * 4);
    if (wav.size() < kHeaderBytes + size_t(expectedFrames) * 4) return;

    std::vector<int16_t> left(expectedFrames);
    int peak = 0;
    int maxStep = 0;
    int clipped = 0;
    bool stereoEqual = true;
    double mean = 0.0;
    for (uint32_t i = 0; i < expectedFrames; ++i) {
        const size_t offset = kHeaderBytes + size_t(i) * 4;
        const uint16_t rawL = uint16_t(uint8_t(wav[offset])) | (uint16_t(uint8_t(wav[offset + 1])) << 8);
        const uint16_t rawR = uint16_t(uint8_t(wav[offset + 2])) | (uint16_t(uint8_t(wav[offset + 3])) << 8);
        left[i] = int16_t(rawL);
        const int sample = left[i];
        peak = std::max(peak, std::abs(sample));
        clipped += sample == 32767 || sample == -32768;
        mean += sample;
        stereoEqual &= int16_t(rawR) == left[i];
        if (i > 0) maxStep = std::max(maxStep, std::abs(sample - int(left[i - 1])));
    }
    const uint32_t dataBytes = uint32_t(wav[40]) | (uint32_t(wav[41]) << 8) | (uint32_t(wav[42]) << 16) |
                               (uint32_t(wav[43]) << 24);
    CHECK_EQ(dataBytes, expectedFrames * 4);
    CHECK(stereoEqual);
    CHECK(peak > 1000);
    CHECK_EQ(clipped, 0);
    CHECK(std::fabs(mean / expectedFrames) < 20.0);
    CHECK(maxStep < peak / 5);  // No single-sample jumps characteristic of a resampling glitch.

    // The import takes a 44100 Hz file as is and converts a 44117 Hz file
    // (spec 02 §8.1). The header rate is not the device rate (44117.647 Hz),
    // so the two modes give different instrument lengths.
    WavImportOptions opt;
    opt.stereo = true;
    std::vector<int16_t> imported(2 * size_t(deviceFrames));
    WavImportResult res;
    CHECK(wavImportMemory(wav.data(), wav.size(), opt, imported.data(), imported.size(), res) == FmtStatus::Ok);
    const uint32_t importedFrames =
        resampleTo44100 ? expectedFrames : uint32_t(uint64_t(deviceFrames) * 44100 / 44117);
    CHECK_EQ(res.frames, importedFrames);
    CHECK(res.stereo);
    if (res.frames != importedFrames) return;
    const int16_t* loaded = imported.data();   // planar: left channel first

    // Measure positive zero-crossing periods away from the attack and tail.
    const uint32_t firstFrame = 44100 / 4;
    const uint32_t lastFrame = importedFrames - 44100 / 4;
    uint32_t firstCrossing = 0, lastCrossing = 0, crossings = 0;
    for (uint32_t i = firstFrame + 1; i < lastFrame; ++i) {
        if (loaded[i - 1] < 0 && loaded[i] >= 0) {
            if (crossings == 0) firstCrossing = i;
            lastCrossing = i;
            ++crossings;
        }
    }
    CHECK(crossings > 1000);
    if (crossings > 1) {
        const double measuredHz = double(crossings - 1) * 44100.0 / double(lastCrossing - firstCrossing);
        CHECK_NEAR(measuredHz, 261.6256, 0.02);   // 0.04 % off would be 0.105 Hz
        std::printf("  synth export (%s): %.4f Hz, %u file frames, %u loaded, peak %d, max step %d\n",
                    resampleTo44100 ? "44100" : "44117", measuredHz, expectedFrames, importedFrames, peak, maxStep);
    }
}
}  // namespace

TEST(export_synth_sine_pitch_duration_and_signal_quality) {
    checkSynthExport(/*resampleTo44100=*/true);
}

TEST(export_synth_sine_device_rate_pitch_and_duration) {
    checkSynthExport(/*resampleTo44100=*/false);
}

namespace {
struct SeqHook {
    Sequencer* seq;
    static void fn(void* ctx, float seconds) { static_cast<SeqHook*>(ctx)->seq->processElapsed(seconds); }
};
}  // namespace

TEST(sequencer_drives_engine) {
    Rig rig;
    rig.load(0, sine(44100, 440.0f), PlayMode::OneShot);
    std::unique_ptr<Sequencer> seq(new Sequencer());
    std::unique_ptr<Project> proj(new Project());
    std::unique_ptr<Pattern> pa(new Pattern()), pb(new Pattern());
    pa->setDefault();
    pb->setDefault();
    pa->tracks[0].steps[0].note = 60;
    pa->tracks[0].steps[0].instrument = 0;
    pa->tracks[0].steps[4].note = 72;
    seq->attach(rig.eng.get(), nullptr, proj.get(), pa.get(), pb.get());
    SeqHook hook{seq.get()};
    rig.eng->setPreBlockHook(&SeqHook::fn, &hook);
    int16_t out[kBlock * 2];
    rig.eng->renderLive(out);
    CHECK_EQ(out[200], 0);
    seq->play();
    int loud = 0;
    for (int b = 0; b < 100; ++b) {
        rig.eng->renderLive(out);
        for (int16_t v : out) loud += std::abs(int(v)) > 1000;
    }
    CHECK(loud > 1000);
    CHECK_NEAR(rig.eng->tempo(), proj->tempo(), 1e-3);
    // Via the IAudioOut fill callback.
    AudioEngine::fillCallback(out, kBlock, rig.eng.get());
    seq->stop();
}

TEST(cpu_budget_estimate) {
    Rig rig;
    auto noise = std::vector<int16_t>(88200);
    uint32_t seed = 7;
    for (auto& v : noise) {
        seed = seed * 1103515245u + 12345u;
        v = int16_t(seed >> 16);
    }
    rig.load(0, noise, PlayMode::Granular);
    rig.load(1, noise, PlayMode::PingPongLoop);
    for (int i = 0; i < 2; ++i) {
        rig.ins[i].filterEnabled = 1;
        rig.ins[i].overdrive = 40;
        rig.ins[i].bitDepth = 12;
        rig.ins[i].delaySend = 50;
        rig.ins[i].reverbSend = 50;
        rig.ins[i].env[2].enabled = 1;
        rig.ins[i].env[2].loop = 1;
    }
    rig.eng->settings().sidechain = 1;
    rig.eng->settings().saturationMode = 3;
    for (int t = 0; t < 8; ++t) rig.note(uint8_t(t), uint8_t(t & 1), uint8_t(50 + t));
    int16_t out[kBlock * 2];
    const int N = 2000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < N; ++b) rig.eng->renderLive(out);
    const auto t1 = std::chrono::steady_clock::now();
    const double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / N;
    std::printf("  host time per block (8 voices + fx, this build's flags): %.1f us (budget 2901 us)\n", us);
    CHECK(us > 0.0);
}

// 2.x pitched raw preview: planar lane pointers (previewBuffer stereo = R
// after L; previewBufferLR separate lanes) and the 16.16 step with linear
// interpolation per channel.
TEST(preview_lr_pointers_and_pitched_step) {
    Rig r;
    std::vector<int16_t> st(2 * 1000, 0);
    r.eng->previewBuffer(st.data(), 1000, true);
    CHECK(r.eng->previewBufferLeft() == st.data());
    CHECK(r.eng->previewBufferRight() == st.data() + 1000);
    CHECK_EQ(r.eng->previewBufferStep(), 65536u);
    r.eng->previewBuffer(st.data(), 1000, false);
    CHECK(r.eng->previewBufferRight() == nullptr);
    // Separate lanes at twice the rate: 1000 frames play in 500 output frames.
    std::vector<int16_t> L(1000, 8000), R(1000, -8000);
    r.eng->previewBufferLR(L.data(), R.data(), 1000, 131072u);
    CHECK(r.eng->previewBufferLeft() == L.data());
    CHECK(r.eng->previewBufferRight() == R.data());
    CHECK_EQ(r.eng->previewBufferStep(), 131072u);
    int16_t out[kBlock * 2];
    r.eng->renderLive(out);
    CHECK(out[2 * 10] > 7000);
    CHECK(out[2 * 10 + 1] < -7000);
    r.eng->renderLive(out);
    r.eng->renderLive(out);
    CHECK(r.eng->previewBufferActive());   // 384 output frames < 500
    r.eng->renderLive(out);
    CHECK(!r.eng->previewBufferActive());  // 512 >= 500
    CHECK(out[2 * 115] > 7000);            // frame 499 still plays
    CHECK_EQ(out[2 * 117], 0);             // past the end: silence
    // Half rate on a ramp: the odd output frames are the interpolated midpoints.
    std::vector<int16_t> ramp(1000);
    for (int i = 0; i < 1000; ++i) ramp[size_t(i)] = int16_t(i * 20);
    r.eng->previewBufferLR(ramp.data(), nullptr, 1000, 32768u);
    r.eng->renderLive(out);
    CHECK(r.eng->previewBufferRight() == nullptr);
    for (int k = 10; k < 60; k += 7) {
        const int a0 = out[2 * (2 * k)], m = out[2 * (2 * k + 1)], a1 = out[2 * (2 * k + 2)];
        CHECK(a1 > a0);
        CHECK(std::abs(2 * m - a0 - a1) <= 2);
        CHECK_EQ(out[2 * (2 * k + 1)], out[2 * (2 * k + 1) + 1]);   // mono on both sides
    }
}
