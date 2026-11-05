// Spec 07 (addendum: audio engine and mixer) -- every answer Q1..Q10 that
// lands in the core, asserted with the addendum's numbers.
#include <cmath>
#include <memory>
#include <vector>

#include "app/capture.h"
#include "audio/engine.h"
#include "formats/project_mt.h"
#include "formats/wav.h"
#include "model/project.h"
#include "test.h"

using namespace ot;
using namespace ot::audio;

namespace {

struct Rig {
    std::vector<int16_t> fx = std::vector<int16_t>(AudioEngine::kFxMemorySamples);
    std::vector<int16_t> pool = std::vector<int16_t>(1u << 20);
    std::unique_ptr<Instrument[]> ins = std::unique_ptr<Instrument[]>(new Instrument[kSampleInstrumentCount]);
    std::unique_ptr<AudioEngine> eng = std::unique_ptr<AudioEngine>(new AudioEngine());
    Rig() { CHECK(eng->init(fx.data(), fx.size(), pool.data(), pool.size(), ins.get())); }
    void load(int i, const std::vector<int16_t>& d, PlayMode m, bool stereo = false) {
        ins[i].lengthFrames = uint32_t(stereo ? d.size() / 2 : d.size());
        ins[i].playMode = uint8_t(m);
        CHECK(eng->bank().store(i, d.data(), ins[i].lengthFrames, stereo));
    }
    void note(uint8_t track, uint8_t instr, uint8_t n = 60) {
        VoiceNoteOn v;
        v.instrument = instr;
        v.note = n;
        eng->voiceNoteOn(track, v);
    }
};

std::vector<int16_t> dc(uint32_t len, int16_t v) { return std::vector<int16_t>(len, v); }

int peakAbs(const int16_t* x, size_t n, int stride = 1, int off = 0) {
    int p = 0;
    for (size_t i = off; i < n; i += size_t(stride)) p = std::max(p, std::abs(int(x[i])));
    return p;
}

double db(double r) { return 20.0 * std::log10(r); }

// Bare voice on a constant 0.5 loop, envelope off, VOL[50] = 1.
struct VoiceRig {
    std::vector<int16_t> d = dc(44100, 16384);
    Instrument ins;
    Voice v;
    VoiceRig() {
        initTables();
        ins.lengthFrames = uint32_t(d.size());
        ins.playMode = uint8_t(PlayMode::ForwardLoop);
        ins.env[kAutoVolume].enabled = 0;
    }
    void on(int16_t step0 = 0) {
        v.reset();
        v.setLfoStepLatch(step0);
        v.noteOn(&ins, d.data(), uint32_t(d.size()), 60, -1, 0, 0, 0, 0);
    }
    // Two blocks (ramps settle); returns the last sample of amp and delay send.
    void run(const VoiceControl& c, float& amp, float& ds) {
        VoiceOut o;
        for (int b = 0; b < 2; ++b) {
            float dsend[kBlock] = {}, rsend[kBlock] = {};
            v.control(c);
            v.render(c, o, dsend, rsend);
            ds = dsend[kBlock - 1];
        }
        amp = o.amp[kBlock - 1];
    }
};

}  // namespace

// --- Q1: stock 1.9.x played the left channel only; 2.x plays both (2x-04 §3) ---

TEST(q1_track_voice_plays_both_channels_2x) {
    Rig r;
    std::vector<int16_t> st(2 * 20000);
    for (size_t i = 0; i < 20000; ++i) {
        st[i] = 8000;              // left block
        st[20000 + i] = -8000;     // right block (planar)
    }
    r.load(0, st, PlayMode::ForwardLoop, true);
    r.note(0, 0);
    int16_t out[kBlock * 2];
    for (int b = 0; b < 3; ++b) r.eng->renderLive(out);
    for (uint32_t i = 0; i < kBlock; ++i) {
        CHECK(out[2 * i] > 7000);        // left block on the left output
        CHECK(out[2 * i + 1] < -7000);   // right block on the right output
    }
}

TEST(q1_file_preview_streams_stereo) {
    Rig r;
    std::vector<int16_t> st(2 * 4000);
    for (size_t i = 0; i < 4000; ++i) {
        st[i] = 8000;
        st[4000 + i] = -8000;
    }
    r.eng->previewBuffer(st.data(), 4000, true);
    int16_t out[kBlock * 2];
    r.eng->renderLive(out);
    CHECK(out[2 * 10] > 7000);
    CHECK(out[2 * 10 + 1] < -7000);
}

// --- Q2: limiter mapping and storage -----------------------------------------

TEST(q2_limiter_threshold_mapping) {
    initTables();
    // idx = v*100/32767 into LIMTHR; displayed dBFS = 20 log10(THR/32767).
    CHECK_NEAR(limiterThreshold(30200), 16422.0f / 32767.0f, 1e-7);   // idx 92
    CHECK_NEAR(db(limiterThreshold(30200)), -6.0, 0.01);
    CHECK_NEAR(limiterThreshold(16384), 479.0f / 32767.0f, 1e-7);      // idx 50
    CHECK_NEAR(db(limiterThreshold(16384)), -36.7, 0.05);
    CHECK_NEAR(limiterThreshold(32767), 1.0f, 0);
    // Coefficients: mode 0 = 1-exp(-2.2*1000/(t*44100)), mode 1 = t/1000.
    CHECK_NEAR(limiterCoef(0.512f, 0), 0.0928f, 0.0005);   // New Project release
    CHECK_NEAR(limiterCoef(100.0f, 1), 0.1f, 1e-6);
}

TEST(q2_limiter_engine_reset_and_project_values) {
    MixerSettings m;   // engine reset (FW:0x15DB8)
    CHECK_EQ(m.limiterThreshold, 32767);
    CHECK_NEAR(m.limiterAttackMs, 300.0f, 0);
    CHECK_NEAR(m.limiterReleaseMs, 10.0f, 0);
    CHECK_EQ(m.sidechain, 0);
    static Project p;   // New Project
    CHECK_EQ(p.u16(0x120), 100);
    CHECK_NEAR(p.f32(0x124), 0.512f, 0);
    CHECK_EQ(p.u16(0x128), 30200);
    CHECK_EQ(p.u8(0x12A), 0x10);
    CHECK_EQ(p.u8(0x12B), 0x01);
    CHECK_EQ(p.u8(0x4FA), 0);
    CHECK_EQ(p.u8(0x4FB), 0);
    p.setU8(0x4FA, 1);
    p.setU8(0x4FB, 3);
    m.loadFromProject(p);
    CHECK_EQ(m.limiterThreshold, 30200);
    CHECK_NEAR(m.limiterAttackMs, 100.0f, 0);
    CHECK_NEAR(m.limiterReleaseMs, 0.512f, 0);   // reproduced, not "fixed"
    CHECK_EQ(m.limiterMode, 1);
    CHECK_EQ(m.sidechain, 3);
    p.setU16(0x128, 40000);   // u16 above 32767 -> min(stored, 32767)
    m.loadFromProject(p);
    CHECK_EQ(m.limiterThreshold, 32767);
}

TEST(q2_q6_truncated_files_read_zero) {
    static Project np;
    static uint8_t file[mt::kFileSize];
    encodeProject(np, file);
    static Project full, cut;
    CHECK(decodeProject(file, sizeof(file), full) == FmtStatus::Ok);
    CHECK_EQ(full.u16(0x128), 30200);
    CHECK_EQ(full.u8(0x4D0), 86);
    CHECK_EQ(full.u8(0x4DD), 86);
    // spec 10 §0.2 (corrects the Q2/Q6 pre-fill note): the reader zero-fills,
    // so a file cut before the limiter fields reads 0 there, not 0x4000/100.
    CHECK(decodeProject(file, 0x120, cut) == FmtStatus::Ok);
    CHECK_EQ(cut.u16(0x128), 0);
    for (uint32_t i = 0; i < 10; ++i) CHECK_EQ(cut.u8(0x4D0 + i), 0);
    CHECK_EQ(cut.u8(0x4DD), 0);
}

// --- Q3 / Q4: Global Mixer sends and Dry Mix ----------------------------------

TEST(q3_send_gain_formula) {
    VoiceRig r;
    r.ins.delaySend = 100;   // SEND[100] = 1
    r.on();
    VoiceControl c;
    c.delayLevel = trkGain(50);
    c.trackGain = trkGain(60);
    c.dryGain = 1.0f;
    float amp, ds;
    // Pre-fader: g = SEND[s] * TRK[delayLevel].
    c.postFaderSends = false;
    r.run(c, amp, ds);
    CHECK_NEAR(ds, 0.5f * trkGain(50), 1e-4);
    // Post-fader: * E_amt * VOL[v] * TRK[trackLevel].
    c.postFaderSends = true;
    r.run(c, amp, ds);
    CHECK_NEAR(ds, 0.5f * trkGain(50) * trkGain(60), 1e-4);
    // SEND law at s = 50: -20 dB.
    r.ins.delaySend = 50;
    c.postFaderSends = false;
    r.run(c, amp, ds);
    CHECK_NEAR(ds, 0.5f * 0.1f * trkGain(50), 1e-4);
    r.ins.delaySend = 100;
    // Muted track: pre-fader still feeds the effect, post-fader does not; dry is 0.
    c.muted = true;
    r.run(c, amp, ds);
    CHECK_NEAR(ds, 0.5f * trkGain(50), 1e-4);
    CHECK_NEAR(amp, 0.0f, 0);
    c.postFaderSends = true;
    r.run(c, amp, ds);
    CHECK_NEAR(ds, 0.0f, 1e-6);
    // FX mute: the engine passes level 0.
    c.muted = false;
    c.delayLevel = 0.0f;
    r.run(c, amp, ds);
    CHECK_NEAR(ds, 0.0f, 1e-6);
}

TEST(q4_dry_mix_scales_amp_not_sends) {
    VoiceRig r;
    r.ins.delaySend = 100;
    r.on();
    VoiceControl c;
    c.delayLevel = 1.0f;
    c.trackGain = 1.0f;
    c.dryGain = trkGain(43);
    float amp, ds;
    r.run(c, amp, ds);
    CHECK_NEAR(amp, 0.5f * trkGain(43), 1e-4);   // AMP = VOL*E*TRK[track]*TRK[dry]
    CHECK_NEAR(ds, 0.5f, 1e-4);                  // not part of the send gain
}

TEST(q3_q4_engine_global_mixer) {
    Rig r;
    r.load(0, dc(44100, 16384), PlayMode::ForwardLoop);
    r.ins[0].delaySend = 100;
    MixerSettings& m = r.eng->settings();
    CHECK_EQ(m.delayLevel, 86);
    CHECK_EQ(m.reverbLevel, 86);
    CHECK_EQ(m.dryMix, 86);
    CHECK(m.postFaderSends);
    m.delayTimeMs = 5;
    m.delayFeedback = 0;
    static StemBlock st;
    int16_t master[kBlock * 2];
    auto run = [&](int blocks, int stem) {
        int pk = 0;
        for (int b = 0; b < blocks; ++b) {
            r.eng->renderOfflineBlock(master, &st);
            if (b >= blocks / 2) pk = std::max(pk, peakAbs(st.data[stem], kBlock * 2));
        }
        return pk;
    };
    r.eng->beginOffline();
    r.note(0, 0);
    const int dry0 = run(20, 0), del0 = run(20, 9);
    CHECK(dry0 > 15000);
    CHECK(del0 > 10000);   // returns fixed at 1.0
    // Dry Mix 1 = -36 dB on the track stem; the delay return is unaffected.
    m.dryMix = 1;
    const int dry1 = run(20, 0), del1 = run(20, 9);
    CHECK_NEAR(db(double(dry1) / dry0), -36.0, 0.5);
    CHECK(std::abs(del1 - del0) < del0 / 50);
    // Dry mute -> 0; Global Delay level 43 scales every track's send.
    m.dryMute = true;
    CHECK_EQ(run(10, 0), 0);
    m.dryMute = false;
    m.dryMix = 86;
    m.delayLevel = 43;
    const int del2 = run(40, 9);
    CHECK_NEAR(db(double(del2) / del0), db(trkGain(43)), 0.5);
    m.delayMute = true;
    run(20, 9);
    CHECK_EQ(run(20, 9), 0);
    r.eng->endOffline();
}

// --- Q5: LFO shapes, polarity, speed table, phase -----------------------------

TEST(q5_speed_table) {
    initTables();
    CHECK_EQ(kRateCount, 29);
    const float rate[29] = {128, 96, 64, 48, 32, 24, 16, 12, 8, 6, 4, 3, 2, 1.5f, 1, 0.75f, 0.5f, 0.375f,
                            1.0f / 3, 0.25f, 0.1875f, 1.0f / 6, 0.125f, 1.0f / 12, 1.0f / 16, 1.0f / 24,
                            1.0f / 32, 1.0f / 48, 1.0f / 64};
    for (int i = 0; i < 29; ++i) CHECK_NEAR(lfoRateSteps(i), rate[i], 1e-7);
}

TEST(q5_lfo_shapes_bipolar_tick_locked) {
    VoiceRig r;
    Envelope& e = r.ins.env[kAutoPanning];
    e.enabled = 1;
    e.loop = 1;
    Lfo& l = r.ins.lfo[kAutoPanning];
    l.speed = 14;   // RATE 1 step = 576 ticks
    l.amount = 0.5f;
    VoiceControl c;
    c.seqPlaying = true;
    auto at = [&](uint8_t shape, uint32_t tick) {
        l.shape = shape;
        c.seqTick = tick;
        r.v.control(c);
        return r.v.modOutput(kAutoPanning);
    };
    r.on(0);
    // 0 decay: +A -> -A.
    CHECK_NEAR(at(0, 0), 0.5f, 1e-5);
    CHECK_NEAR(at(0, 288), 0.0f, 1e-5);
    // 1 attack (saw up): -A -> +A.
    CHECK_NEAR(at(1, 0), -0.5f, 1e-5);
    CHECK_NEAR(at(1, 432), 0.25f, 1e-5);
    CHECK_NEAR(at(1, 576 + 144), -0.25f, 1e-5);   // locked to tick mod 576
    // 2 triangle -A -> +A -> -A.
    CHECK_NEAR(at(2, 0), -0.5f, 1e-5);
    CHECK_NEAR(at(2, 288), 0.5f, 1e-5);
    CHECK_NEAR(at(2, 432), 0.0f, 1e-5);
    // 3 square: +A for half a period, then -A.
    CHECK_NEAR(at(3, 100), 0.5f, 1e-5);
    CHECK_NEAR(at(3, 400), -0.5f, 1e-5);
    // 4 sample & hold: a level in [-A, +A], held for the period.
    const float h0 = at(4, 10);
    CHECK(h0 >= -0.5f && h0 <= 0.5f);
    CHECK_NEAR(at(4, 500), h0, 0);
    bool changed = false;
    for (uint32_t p = 1; p < 20 && !changed; ++p) {
        const float h = at(4, p * 576 + 5);
        CHECK(h >= -0.5f && h <= 0.5f);
        changed = h != h0;
    }
    CHECK(changed);
    // S_on: pos = tick + S_on*576; with a 2-step period a latch of 1 shifts half a cycle.
    l.speed = 12;   // RATE 2 steps
    r.on(1);
    CHECK_NEAR(at(1, 0), 0.0f, 1e-5);
    r.on(0);
    CHECK_NEAR(at(1, 0), -0.5f, 1e-5);
}

TEST(q5_lfo_free_running_when_stopped) {
    VoiceRig r;
    Envelope& e = r.ins.env[kAutoCutoff];
    e.enabled = 1;
    e.loop = 1;
    Lfo& l = r.ins.lfo[kAutoCutoff];
    l.speed = 14;   // 1 step = 125 ms at 120 BPM
    l.shape = 1;
    l.amount = 0.5f;
    VoiceControl c;
    c.seqPlaying = false;
    c.tempoBpm = 120.0f;
    c.blockUs = 2500.0f;
    r.on();
    r.v.control(c);
    CHECK_NEAR(r.v.modOutput(kAutoCutoff), -0.5f, 1e-5);   // starts at phase 0 on note-on
    for (int i = 0; i < 24; ++i) r.v.control(c);
    r.v.control(c);   // t = 25 * 2.5 ms = half the period
    CHECK_NEAR(r.v.modOutput(kAutoCutoff), 0.0f, 1e-4);
    for (int i = 0; i < 25; ++i) r.v.control(c);
    CHECK_NEAR(r.v.modOutput(kAutoCutoff), -0.5f, 1e-4);   // loops
    r.v.noteOff(-4);
    r.v.control(c);
    CHECK_NEAR(r.v.modOutput(kAutoCutoff), 0.0f, 0);       // note-off -> idle
}

TEST(q5_envelope_mode_unipolar) {
    VoiceRig r;
    Envelope& e = r.ins.env[kAutoCutoff];
    e.enabled = 1;
    e.loop = 0;
    e.amount = 0.8f;
    e.attackMs = 10;
    e.sustain = 1.0f;
    r.on();
    VoiceControl c;
    float last = 0.0f;
    for (int i = 0; i < 20; ++i) {
        r.v.control(c);
        last = r.v.modOutput(kAutoCutoff);
        CHECK(last >= 0.0f && last <= 0.8f);
    }
    CHECK_NEAR(last, 0.8f, 1e-4);
}

// --- Q6: track level default 86 = 0 dB -----------------------------------------

TEST(q6_new_project_levels) {
    initTables();
    CHECK_NEAR(trkGain(86), 1.0f, 1e-6);
    static Project p;
    for (uint32_t t = 0; t < 8; ++t) {
        CHECK_EQ(p.u8(0x4D0 + t), 86);
        CHECK_EQ(p.u8(0x12C + t), 0);
    }
    CHECK_EQ(p.u8(0x4D8), 86);
    CHECK_EQ(p.u8(0x4D9), 86);
    CHECK_EQ(p.u8(0x4DA), 0);
    CHECK_EQ(p.u8(0x4DB), 0);
    CHECK_EQ(p.u8(0x4DC), 0);
    CHECK_EQ(p.u8(0x4DD), 86);
    CHECK_EQ(p.u8(0x309), 86);
    CHECK_EQ(p.u8(0x4FC), 1);
    CHECK_EQ(p.u16(0x5FE), 20000);
    MixerSettings m;
    m.loadFromProject(p);
    for (int t = 0; t < 8; ++t) CHECK_EQ(m.trackLevel[t], 86);
    CHECK_EQ(m.dryMix, 86);
    CHECK_EQ(m.lineInLevel, 86);
    CHECK(m.postFaderSends);
    // The pre-fill (truncated files only) is the old +6 dB value.
    static Project q;
    q.setLoadPrefill();
    CHECK_EQ(q.u8(0x4D0), 100);
    CHECK_EQ(q.u16(0x128), 0x4000);
}

// --- Q8: WAV import -------------------------------------------------------------

TEST(q8_mono_import_takes_left_channel) {
    std::vector<int16_t> inter(2 * 1000);
    for (int i = 0; i < 1000; ++i) {
        inter[2 * i] = 1000;
        inter[2 * i + 1] = -3000;
    }
    std::vector<uint8_t> file(4000 + 64);
    file.resize(wavEncode(inter.data(), 1000, 2, file.data(), file.size()));
    std::vector<int16_t> out(4000);
    WavImportResult res;
    WavImportOptions opt;   // mono
    CHECK(wavImportMemory(file.data(), file.size(), opt, out.data(), out.size(), res) == FmtStatus::Ok);
    CHECK(!res.stereo);
    CHECK_EQ(res.frames, 1000u);
    for (uint32_t i = 0; i < res.frames; ++i) CHECK_EQ(out[i], 1000);   // no averaging
}

TEST(q8_float_decode_truncates_and_saturates) {
    auto f = [](float x) {
        uint8_t b[4];
        std::memcpy(b, &x, 4);
        return wavDecodeSample(b, wav::kFormatFloat, 32);
    };
    CHECK_EQ(f(0.25f), 8191);      // 8191.75 -> toward zero
    CHECK_EQ(f(-0.25f), -8191);
    CHECK_EQ(f(1.0f), 32767);
    CHECK_EQ(f(1.5f), 32767);      // past full scale: held at the top, not wrapped into noise
    CHECK_EQ(f(-2.625f), -32768);
    CHECK_EQ(f(1e30f), 32767);
}

TEST(q8_resampler_truncates_then_clamps) {
    // 22050 Hz source with overshooting cubic segments.
    const int16_t src[8] = {0, 32767, 32767, -32768, -32768, 1001, -1001, 0};
    std::vector<int16_t> out(32);
    CubicResampler rs;
    rs.begin(22050, 8, out.data());
    for (int16_t s : src) rs.push(s);
    rs.finish();
    CHECK_EQ(rs.produced(), 16u);
    const float ratio = 22050.0f / 44100.0f;
    auto at = [&](int64_t j) { return j < 0 || j >= 8 ? 0.0f : float(src[j]); };
    bool clamped = false;
    for (uint32_t i = 0; i < 16; ++i) {
        const float t = float(i) * ratio;   // single precision (spec 07 Q8)
        const int64_t n = int64_t(std::floor(t));
        const float mu = t - float(n);
        const float p0 = at(n - 1), p1 = at(n), p2 = at(n + 1), p3 = at(n + 2);
        const float a0 = p3 - p2 - p0 + p1, a1 = p0 - p1 - a0, a2 = p2 - p0;
        const float y = ((a0 * mu + a1) * mu + a2) * mu + p1;
        int32_t v = int32_t(y);   // toward zero
        if (v > 32767 || v < -32768) clamped = true;
        v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
        CHECK_EQ(out[i], v);
    }
    CHECK(clamped);
}

// --- Q9: delay filter -------------------------------------------------------------

TEST(q9_delay_filter_coefficients) {
    int32_t b = 0, c = 0;
    delayFilterCoefs(20000.0f, b, c);
    CHECK_EQ(b, 891);
    CHECK_EQ(c, 760);
    delayFilterCoefs(100.0f, b, c);
    CHECK_EQ(b, 7);
    CHECK_EQ(c, -1008);
    // Stable over the whole 100..20000 Hz range (|pole| < 1), DC gain ~1.
    for (int f = 100; f <= 20000; f += 100) {
        delayFilterCoefs(float(f), b, c);
        CHECK(std::abs(c) < 1024);
        CHECK_NEAR(double(2 * b - c) / 1024.0, 1.0, 0.02);
    }
}

TEST(q9_delay_filter_stable_at_high_cutoff) {
    initTables();
    std::vector<int16_t> mem(Delay::kMemorySamples);
    Delay d;
    d.init(mem.data());
    d.setFilter(15000.0f);   // unstable with the swapped formula
    d.setTimeSamples(100);
    d.setFeedbackK(delayFeedbackK(90));
    float in[kBlock] = {}, l[kBlock], r[kBlock];
    in[0] = 0.5f;
    float last = 0.0f;
    for (int b = 0; b < 400; ++b) {
        d.process(in, l, r);
        in[0] = 0.0f;
        for (float v : l) last = std::max(last, std::fabs(v));
        if (b == 399) CHECK(last < 0.5f);
        if (b % 50 == 49) last = 0.0f;
    }
}

// --- Q10: metronome ---------------------------------------------------------------

TEST(q10_click_sound_and_level) {
    Rig r;
    MixerSettings& m = r.eng->settings();
    CHECK_EQ(m.metronomeVolume, 50);   // default -> SEND[50] = -20 dB
    int16_t out[kBlock * 2];
    auto clickPeak = [&](bool accent) {
        r.eng->metronomeClick(accent, accent ? 100 : 45);
        int pk = 0;
        for (int b = 0; b < 30; ++b) {
            r.eng->renderLive(out);
            pk = std::max(pk, peakAbs(out, kBlock * 2));
            CHECK_EQ(out[0], out[1]);   // centred
        }
        return pk;
    };
    m.metronomeVolume = 100;
    const int p100 = clickPeak(true);
    CHECK(p100 > 5000);
    CHECK(!r.eng->metronomeActive());   // ~52 ms one-shot, then silent
    m.metronomeVolume = 50;
    const int p50 = clickPeak(true);
    CHECK_NEAR(db(double(p50) / p100), -20.0, 0.5);
    // Accent +100 cents / normal +45 cents at note index 30 (linear finetune).
    const float* t = pitchTable();
    for (int b = 0; b < 30; ++b) r.eng->renderLive(out);
    r.eng->metronomeClick(true, 100);
    CHECK_NEAR(r.eng->voice(AudioEngine::kMetronomeVoice).rate(), t[31], 1e-6);
    r.eng->metronomeClick(false, 45);
    r.eng->renderLive(out);   // a retrigger starts after the running click's fast release
    r.eng->renderLive(out);
    CHECK_NEAR(r.eng->voice(AudioEngine::kMetronomeVoice).rate(), t[30] + 0.45f * (t[31] - t[30]), 1e-6);
    CHECK_NEAR(t[30], 0.1767f, 0.0005);
    // Added after the limiter and the master volume.
    m.limiterThreshold = 0;
    m.masterVolume = 0;
    m.metronomeVolume = 100;
    CHECK(clickPeak(false) > 5000);
}

TEST(q10_click_not_in_exports) {
    Rig r;
    r.eng->beginOffline();
    r.eng->metronomeClick(true, 100);
    int16_t out[kBlock * 2];
    r.eng->renderOfflineBlock(out, nullptr);
    CHECK_EQ(peakAbs(out, kBlock * 2), 0);
    r.eng->endOffline();
}

TEST(q10_sample_recorder_metronome) {
    Rig r;
    std::vector<int16_t> buf(44100 * 4);
    int16_t in[kBlock * 2] = {};
    Capture cap;
    cap.setBuffer(buf.data(), uint32_t(buf.size()));
    // Recording with the metronome on (not the radio): clicks at step boundaries.
    cap.start(0, 0.125f, 4, 16, true);
    cap.process(in, kBlock, r.eng.get());
    CHECK(r.eng->metronomeActive());
    // Metronome off: no clicks while recording.
    Rig q;
    cap.start(0, 0.125f, 4, 16, false);
    for (int b = 0; b < 200; ++b) cap.process(in, kBlock, q.eng.get());
    CHECK(!q.eng->metronomeActive());
    // Count-in clicks.
    Rig c;
    cap.start(16, 0.125f, 4, 16, false);
    cap.process(in, kBlock, c.eng.get());
    CHECK(c.eng->metronomeActive());
}
