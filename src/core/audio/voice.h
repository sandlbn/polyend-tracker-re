// One monophonic sample voice per track (spec 04 §1.2, §2, §3, §4):
// player (+headroom, +volume envelope) -> bit crusher -> overdrive -> SVF
// (-> delay/reverb sends) -> ramped volume -> pan.
//
// 2.x stereo playback (docs/specs/2x-04 §3, docs/MINI_PLAN.md §2): a planar
// stereo instrument in one-shot, loop, slice or beat-slice mode plays both
// channels through one shared playhead, with a second interpolation /
// crusher / overdrive / filter state; pan becomes balance (stock linear law
// on the far channel); the sends take (L + R) / 2. Mono instruments keep the
// unchanged mono path. Wavetable and granular play the left channel only
// (TODO(2.1): stereo wavetable / granular).
#pragma once

// Marker for the 2.x test-song generator (tools/make_testsongs/features.h).
#define OT_FEATURE_STEREO_PLAYBACK 1

#include <cstdint>

#include "audio/audio_tables.h"
#include "audio/dsp.h"
#include "audio/stream_source.h"
#include "model/instrument.h"

namespace ot {
namespace audio {

// Step FX ids handled by the voice (spec 03 §3.6).
enum VoiceFx : uint8_t {
    kFxNone = 0,
    kFxOff = 1,
    kFxRandomVolume = 7,
    kFxVolume = 18,
    kFxGlide = 19,
    kFxPosition = 22,
    kFxVolumeLfo = 23,
    kFxPanLfo = 24,
    kFxSlice = 25,
    kFxReverse = 26,
    kFxLowPass = 27,
    kFxHighPass = 28,
    kFxBandPass = 29,
    kFxDelaySend = 30,
    kFxPanning = 31,
    kFxReverbSend = 32,
    kFxFinetuneLfo = 33,
    kFxMicroTune = 34,
    kFxFilterLfo = 35,
    kFxPositionLfo = 36,
    kFxOverdrive = 38,
    kFxBitDepth = 39,
    kFxTune = 40,
    kFxSlideUp = 41,
    kFxSlideDown = 42,
};

// Per-block inputs computed by the engine (spec 04 §4.2, §4.9, §5.5; spec 07 Q3-Q5).
struct VoiceControl {
    float tempoBpm = 130.0f;
    float blockUs = kLiveBlockUs;
    bool seqPlaying = false;      // LFOs tempo-locked to seqTick while true (07 Q5)
    uint32_t seqTick = 0;         // sequencer tick counter, 576 per step
    float trackGain = 1.0f;       // TRK[trackLevel[t]]
    float dryGain = 1.0f;         // TRK[Dry Mix], 0 when Dry Mix is muted (07 Q4)
    float delayLevel = 1.0f;      // TRK[Global Mixer Delay], 0 when muted (07 Q3)
    float reverbLevel = 1.0f;     // TRK[Global Mixer Reverb], 0 when muted
    bool postFaderSends = true;   // Sends mode 1
    bool headroom = false;        // extra headroom (-6.02 dB at players, §1.4)
    bool antiAliasing = true;     // linear interpolation (§2.2)
    // Track mute flag (.mt 0x12C+t): dry AMP and post-fader sends are 0; in
    // pre-fader mode the track still feeds the effects (07 Q3).
    bool muted = false;
    // 2.x stereo voices: o.amp (the side-chain source) is only computed when
    // set; the engine sets it for the side-chain track (mono voices always
    // write o.amp).
    bool ampTap = true;
};

// Performance-mode offsets of one track (spec 06 §3.2 FX 1..16, 20, 21),
// applied on top of instrument values and step FX overrides. The
// Performance screen documents the UI-value -> offset mapping.
struct PerfOffsets {
    int16_t volume = 0;          // volume units (0..100 scale)
    int16_t pan = 0;             // panning units (0..100 scale)
    int16_t tune = 0;            // semitones
    int8_t filterType = -1;      // -1 none, 0 LP, 1 HP, 2 BP (forces the filter on)
    float cutoff = 0.0f;         // added to the 0..1 cutoff
    int16_t dsend = 0, rsend = 0;   // send units (0..100 scale)
    int32_t start = 0, end = 0;  // start / end marker offsets, 1/65535 units
    bool reverse = false;        // playback direction reversed
    int8_t lfoSpeed[kAutomationCount] = {};   // LFO speed-index offsets
    int16_t bits = 0;            // bit-depth offset (bits)
    int16_t od = 0;              // overdrive units (0..100 scale)
};

// Output buffers of one voice for one block (all normalised to +-1).
struct VoiceOut {
    float amp[kBlock];   // post-volume mono (side-chain source, §5.3)
    float l[kBlock];     // post-pan
    float r[kBlock];
};

class Voice {
public:
    void reset();
    // Sample source for the instrument (int16, frames per channel). Called by
    // the engine on note-on with the bank's current data; dataR is the right
    // block of a planar stereo instrument (nullptr = mono).
    void noteOn(const Instrument* ins, const int16_t* data, uint32_t frames, uint8_t note, int8_t volume,
                uint8_t fxAType, uint8_t fxAValue, uint8_t fxBType, uint8_t fxBValue,
                const int16_t* dataR = nullptr, const StreamSource* stream = nullptr, StreamRing* ring = nullptr);
    void noteOff(int8_t mode);   // NoteOffMode values: -2 fade, -3 cut, -4 off
    void applyFx(uint8_t slot, uint8_t type, uint8_t value);
    void resetFx();
    // Performance offsets (kept across reset(); spec 06 §3.4 apply/revert).
    void setPerf(const PerfOffsets& p);
    const PerfOffsets& perf() const { return perf_; }
    void stop();
    bool active() const { return playing_ || env_.active(); }
    bool playing() const { return playing_; }
    const Instrument* instrument() const { return ins_; }
    // The current note plays both channels (stereo instrument, stereo mode).
    bool stereo() const { return stereo_; }
    void setSeed(uint32_t s) { seed_ = s ? s : 1u; }
    // Track-0 step index at the next note-on / LFO FX: S_on of the synced
    // LFO phase (spec 07 Q5).
    void setLfoStepLatch(int16_t step0) { curStep0_ = step0; }

    // Per-block control (spec 04 §4.8/§4.9): modulators, parameter mapping.
    void control(const VoiceControl& c);
    // Render one block; adds the post-filter signal into dsend/rsend (§1.3).
    // Returns false when the voice produced silence (outputs zeroed).
    // mixL/mixR (optional): the post-pan output is also added into them;
    // with them, keepLR = false may skip writing o.l / o.r (o.amp is always
    // written).
    bool render(const VoiceControl& c, VoiceOut& o, float* dsend, float* rsend, float* mixL = nullptr,
                float* mixR = nullptr, bool keepLR = true);
    // Peak |post-pan output| of the last rendered block (meters).
    float blockPeak() const { return peak_; }
    // Sends the last render() added into: bit 0 delay, bit 1 reverb (0 when
    // it returned false). A send it did not touch keeps its input exactly.
    uint8_t sendsTouched() const { return sendsTouched_; }

    // Introspection for tests / UI.
    float rate() const { return targetRate_ + glideOff_; }
    float modOutput(int slot) const { return mods_[slot].out; }
    float envelopeLevel() const { return env_.level(); }
    // Play position (absolute sample frame) while the player runs, -1 otherwise.
    // Read from the UI as a snapshot: one aligned 32-bit load, no lock.
    int32_t playhead() const { return playing_ ? pos_ : -1; }

private:
    struct Pending {
        const Instrument* ins = nullptr;
        const int16_t* data = nullptr;
        uint32_t frames = 0;
        uint8_t note = 60;
        bool valid = false;
        const int16_t* dataR = nullptr;   // right block (stereo), else nullptr
        const StreamSource* stream = nullptr;   // streamed instrument (data = its head), else nullptr
        StreamRing* ring = nullptr;             // the track's read-ahead ring
    };
    struct Mod {          // automation slots 1..5 (spec 04 §4.8)
        uint8_t mode = 0; // 0 off, 1 envelope, 2 LFO
        uint8_t st = 0;   // envelope state 0 idle,1 delay,2 attack,3 hold,4 decay,5 sustain,6 release
        float t = 0.0f;   // µs in state
        float relStart = 0.0f;
        float out = 0.0f;
        uint32_t lastCycle = 0xFFFFFFFFu;
        float rnd = 0.0f;   // sample & hold level v in [0, A]
        int16_t stepOn = 0; // S_on latch (spec 07 Q5)
        uint32_t cyc = 0;   // free-running period counter
    };
    struct Overrides {
        int16_t volume = -1, pan = -1, dsend = -1, rsend = -1, od = -1, bits = -1;
        int16_t tune = -128, fine = -128;    // -128 = none
        float cutoff = -1.0f;
        int8_t ftype = -1;
        int16_t position = -1;               // 0..100 (FX 22)
        int8_t lfoSpeed[kAutomationCount] = {-1, -1, -1, -1, -1, -1};
        int8_t slideDir = 0;
        uint8_t slideVal = 0;
    };

    void startNote();
    void computePoints();
    void computeGrain();
    void setupVolumeEnvelope(float tempo);
    void rebuildOverrides();
    void applyOne(uint8_t type, uint8_t value);
    void updateMod(int slot, const VoiceControl& c);
    void updateLfo(int slot, const VoiceControl& c, int speed);
    int16_t curStep0_ = 0;
    uint32_t rnd(uint32_t n);
    void renderPlayer(float* out, uint32_t n);
    // Stereo player / block render (2x-04 §3). Kept apart from the mono
    // functions so the mono path compiles exactly as before.
    void renderPlayerSt(float* outL, float* outR, uint32_t n);
    bool renderSt(const VoiceControl& c, VoiceOut& o, float* dsend, float* rsend, float* mixL, float* mixR);
    float computeRate() const;

    // binding
    const Instrument* ins_ = nullptr;
    const int16_t* data_ = nullptr;
    uint32_t frames_ = 0;
    // SD streaming (audio/stream_source.h): frames come through streamRead.
    const StreamSource* stream_ = nullptr;
    StreamRing* ring_ = nullptr;
    uint32_t streamGen_ = 0;
    uint8_t note_ = 60;
    uint8_t sliceNote_ = 60;   // the note before the Beat Slice unity override
    Pending pending_;

    // player state (spec 04 §2.1); positions are absolute frame indices
    bool playing_ = false;
    uint8_t mode_ = 0;
    bool reverse_ = false;
    int8_t dir_ = 1;
    int32_t pos_ = 0;
    uint32_t frac_ = 0;
    int32_t start_ = 0, end_ = 0, loop1_ = 0, loop2_ = 0;
    // wavetable
    uint32_t wtSize_ = 2048, wtOffset_ = 0;
public:
    // Wavetable window the voice plays (Sample Playback view, MANUAL_AUDIT #12).
    uint32_t wavetableWindow() const { return wtSize_ ? wtOffset_ / wtSize_ : 0; }
    uint16_t grainPosition() const { return grainPos_; }
private:
    // granular
    int32_t gStart_ = 0, gEnd_ = 1;
    bool grainDirty_ = false;
    uint16_t grainPos_ = 0;
    // pitch
    float targetRate_ = 1.0f;
    float lastRate_ = 0.0f;    // for glide
    bool hadNote_ = false;
    float glideOff_ = 0.0f, glideInc_ = 0.0f;
    uint32_t glideLeft_ = 0;
    float slideSemis_ = 0.0f;
    bool slidePending_ = false, slideActive_ = false;
    int8_t slideDirNew_ = 0;
    uint8_t slideValNew_ = 0;
    float slideFrom_ = 0.0f, slideRatio_ = 0.0f, slideProg_ = 0.0f;
    uint8_t sliceSel_ = 0xFF;  // FX slice override
    uint8_t glideFx_ = 0xFF;

    // FX slots (spec 03 §6 voiceApplyFx)
    uint8_t slotType_[2] = {0, 0};
    uint8_t slotValue_[2] = {0, 0};
    int8_t noteVolume_ = -1;
    Overrides ov_;
    bool reverseFx_ = false;
    PerfOffsets perf_;
    int lfoSpeedFor(int slot, int base) const;

    // modulators
    VolumeEnvelope env_;
    float eAmt_ = 1.0f;
    float volLfoPeriodMs_ = -1.0f;
    Mod mods_[kAutomationCount];

    // processing state
    Svf svf_;
    uint8_t filtType_ = 0;
    bool filtOn_ = false;
    bool filtXfade_ = false;
    float filtLast_ = 0.0f;
    float fmTarget_ = 0.0f, dpTarget_ = 1.0f / 0.7f;
    float odGain_ = 0.0f;
    uint16_t mask_ = 0xFFFF;
    Ramp amp_, panL_, panR_, sendD_, sendR_;
    float lastOut_ = 0.0f;
    float peak_ = 0.0f;
    const VoiceOut* cleanOut_ = nullptr;   // o known to be all zero
    uint32_t seed_ = 0x1234567u;
    bool firstBlock_ = true;
    float tempo_ = 130.0f;
    bool aa_ = true;

    // Stereo state (2x-04 §3), after the mono members so their offsets stay.
    bool stereo_ = false;      // current note plays L and R (2x-04 §3.1)
    bool lastSt_ = false;      // the previous block ran the stereo chain
    bool stBlock_ = false;     // stereo_ || a stereo note is pending: next block runs the stereo chain
    const int16_t* dataR_ = nullptr;   // right block (stereo instrument), else nullptr
    float filtLastR_ = 0.0f;
    Svf svfR_;                 // right channel filter (stereo notes)
    uint8_t sendsTouched_ = 0; // see sendsTouched()
};

}  // namespace audio
}  // namespace ot
