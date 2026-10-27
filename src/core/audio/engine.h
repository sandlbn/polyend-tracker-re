// Audio engine root (spec 04 §1): 8 track voices + the ninth player (metronome), sends,
// delay, reverb, master saturation, limiter, master volume, line-in,
// live rendering through IAudioOut and offline (export) rendering.
//
// Implements ISequencerSink (spec 03 §6). Real-time contract: render paths
// never allocate; all memory is owned by the object or caller-provided.
#pragma once

#include <cstddef>
#include <cstdint>

#include "audio/audio_tables.h"
#include "audio/block_profile.h"
#include "audio/cpu_meter.h"
#include "audio/effects.h"
#include "audio/governor.h"
#include "audio/stream_source.h"
#include "audio/synth_bank.h"
#include "audio/voice.h"
#include "hal_api.h"
#include "model/instrument.h"
#include "model/project.h"
#include "sequencer/seq_sink.h"

namespace ot {
namespace audio {

// Sample memory: instruments packed contiguously in index order
// (spec 02 §8.2). The pool is caller-provided (SDRAM on the device).
class SampleBank {
public:
    static constexpr int kSlots = kSampleInstrumentCount;   // 48
    void init(int16_t* pool, size_t capacitySamples);
    // Make room for instrument idx (planar stereo = 2*frames samples) and
    // return the writable start, or nullptr when the bank is full
    // ("not enough memory in sample bank"). Moves higher instruments.
    // Not real-time: stop voices first (AudioEngine::stopAll()).
    int16_t* reserve(int idx, uint32_t frames, bool stereo);
    bool store(int idx, const int16_t* samples, uint32_t frames, bool stereo);
    void remove(int idx);
    void clear();
    // In-place loading: decoders write into the free region after the last
    // instrument (tail(), freeSamples() int16), then commitTail() moves the
    // data into index order. idx must not be loaded; frames == 0 leaves it empty.
    int16_t* tail() { return pool_ ? pool_ + usedSamples() : nullptr; }
    size_t freeSamples() const { return cap_ - reserved_ - usedSamples(); }
    // [I] Live resampling (app/resample.h): the last `samples` of the pool are
    // held back for a recording in progress; freeSamples() excludes them, so
    // loads into tail() can never reach them. 0 releases.
    bool setReserved(size_t samples);
    size_t reserved() const { return reserved_; }
    int16_t* reservedArea() { return pool_ && reserved_ ? pool_ + cap_ - reserved_ : nullptr; }
    // Highest loaded slot (-1: none); a commit above it moves no other instrument.
    int lastLoaded() const;
    bool commitTail(int idx, uint32_t frames, bool stereo);
    // Copy instrument src into dst (dst replaced). False when the bank is full.
    bool copy(int src, int dst);
    // Keep frames [start, start+n) of idx (sample editor crop).
    void crop(int idx, uint32_t start, uint32_t n);
    // Sample editor apply / one-level undo (spec 06 §6.3/§6.4): idx (loaded)
    // takes the block of `frames` written at tail(); its old data ends up at
    // the new tail() (same planar layout), so a second call with the old
    // size swaps back. Same size: a swap through a 256-byte stack buffer;
    // another size: block moves via the free pool memory after the new
    // block when it has room, else two in-place rotations.
    bool replaceFromTail(int idx, uint32_t frames, bool stereo);
    int16_t* data(int idx) { return loaded(idx) ? pool_ + slots_[idx].off : nullptr; }
    uint32_t samples(int idx) const { return valid(idx) ? slots_[idx].samples() : 0u; }
    uint32_t usedBytes() const { return uint32_t(usedSamples() * 2); }
    uint32_t capacityBytes() const { return uint32_t(cap_ * 2); }
    bool loaded(int idx) const { return valid(idx) && slots_[idx].loaded; }
    const int16_t* left(int idx) const;
    const int16_t* right(int idx) const;
    uint32_t frames(int idx) const { return loaded(idx) ? slots_[idx].frames : 0; }
    bool stereo(int idx) const { return loaded(idx) && slots_[idx].stereo; }
    size_t usedSamples() const;
    size_t capacitySamples() const { return cap_; }
    // SD streaming (docs/PLAN_SD_STREAMING_P1C_P2.md §2.3): a loaded slot that
    // holds only the head(s) of a streamed instrument of fullFrames frames.
    // Every call that rewrites the slot clears it. A partial slot never plays
    // without its stream source (AudioEngine::voiceNoteOn).
    bool setPartial(int idx, uint32_t fullFrames);
    bool partial(int idx) const { return loaded(idx) && slots_[idx].full != 0; }
    uint32_t fullFrames(int idx) const { return partial(idx) ? slots_[idx].full : frames(idx); }

private:
    struct Slot {
        uint32_t off = 0;
        uint32_t frames = 0;
        uint32_t full = 0;   // partial (streamed) slot: the instrument's whole length, else 0
        bool stereo = false;
        bool loaded = false;
        uint32_t samples() const { return loaded ? frames * (stereo ? 2u : 1u) : 0u; }
    };
    static bool valid(int i) { return i >= 0 && i < kSlots; }
    uint32_t startOf(int idx) const;
    int16_t* pool_ = nullptr;
    size_t cap_ = 0;
    size_t reserved_ = 0;
    Slot slots_[kSlots];
};

// Mixer / master / effect parameters in UI units (spec 04 §4.2, §5, §7;
// spec 07 Q2-Q4). The UI writes these; the engine reads them once per block.
// Defaults are the engine-reset values (spec 07 Q2) and New Project levels.
struct MixerSettings {
    uint8_t trackLevel[kVoices] = {86, 86, 86, 86, 86, 86, 86, 86};   // TRK index, 86 = 0 dB (07 Q6)
    bool trackMute[kVoices] = {};     // .mt 0x12C+t (03 §2.4): dry and post-fader sends 0
    bool trackSolo[kVoices] = {};
    // Global Mixer (spec 07 Q3/Q4): Delay / Reverb levels scale every track's
    // send gain; Dry Mix scales every track's AMP stage. Returns stay at 1.0.
    uint8_t delayLevel = 86;          // .mt 0x4D9 (the "S" scaler)
    uint8_t reverbLevel = 86;         // .mt 0x4D8
    uint8_t dryMix = 86;              // .mt 0x4DD
    bool delayMute = false;           // .mt 0x4DB
    bool reverbMute = false;          // .mt 0x4DA
    bool dryMute = false;             // .mt 0x4DC
    bool postFaderSends = true;       // Sends mode, .mt 0x4FC (New Project = 1)
    uint8_t masterVolume = 86;        // TRK index
    uint8_t saturationMode = 0;       // 0..4 (§5.4)
    bool extraHeadroom = false;
    bool antiAliasing = true;
    // delay (§5.1)
    uint16_t delayTimeMs = 500;
    uint8_t delayFeedback = 50;       // 0..100
    uint16_t delayFilterHz = 20000;
    bool delayPingPong = false;
    bool delaySync = false;
    uint8_t delaySyncIndex = 6;
    // reverb (§5.2), 0..1
    float reverbSize = 0.7f, reverbDamping = 0.2f, reverbPredelay = 0.0f, reverbDiffusion = 0.625f;
    // limiter (§5.3, spec 07 Q2); engine reset: attack 300, release 10.0,
    // threshold 32767, side-chain off until the project is applied.
    uint16_t limiterThreshold = 32767;   // stored value; LIMTHR[min(v,32767)*100/32767]
    float limiterAttackMs = 300.0f;
    float limiterReleaseMs = 10.0f;
    uint8_t limiterMode = 0;
    uint8_t sidechain = 0;            // 0 off, 1..8 track, 9/10 line-in L/R
    // line input (§1.3, §5.4, §5.5)
    uint8_t lineInChannel = 0;        // 0 stereo, 1 left->both, 2 right->both
    uint8_t lineInLevel = 86;
    bool lineInMute = false;
    uint8_t lineInDelaySend = 0;      // TRK index; 0.5*TRK, 0 below 0.01
    uint8_t lineInReverbSend = 0;
    // Settings (config, not project): metronome volume 0..100 through the
    // SEND law (spec 07 Q10, default 50 = -20 dB) and the loader
    // file-preview level (TRK index).
    uint8_t metronomeVolume = 50;
    uint8_t previewVolume = 86;

    // Fill from the project values block (file offsets; RAM 0x20004780 + f,
    // spec 02 §3.3, spec 07). Fields without a project location keep their value.
    void loadFromProject(const Project& p);
};

// One block of every stem, interleaved stereo int16 (spec 04 §6.1 queues):
// 0..7 track1..8 (post-pan), 8 reverb, 9 delay, 10 master, 11..13 Synth 1..3
// (2.x: slot bus after gain and pan, before the sends; 2x-04 §6, 2x-05 §9).
struct StemBlock {
    static constexpr int kStems = 14;
    static constexpr int kFirstSynthStem = 11;
    int16_t data[kStems][kBlock * 2];
};

// [I] Live resampling tap (app/resample.h): one stereo block of the chosen
// source per live block, in the audio context, after the mix.
class IResampleTap {
public:
    virtual ~IResampleTap() = default;
    // l / r null: a block of silence.
    virtual void tapBlock(const float* l, const float* r, uint32_t frames) = 0;
};

class AudioEngine : public ISequencerSink {
public:
    static constexpr size_t kFxMemorySamples = Delay::kMemorySamples + Reverb::kMemorySamples;
    static constexpr int kMetronomeVoice = kVoices;   // PREVIEWPLAYER (spec 07 Q10)

    AudioEngine();
    // fxMemory: kFxMemorySamples int16; samplePool: sample bank memory.
    // instruments: kSampleInstrumentCount entries owned by the caller.
    bool init(int16_t* fxMemory, size_t fxSamples, int16_t* samplePool, size_t poolSamples,
              const Instrument* instruments);
    SampleBank& bank() { return bank_; }
    // SD streaming (audio/stream_source.h, docs/PLAN_SD_STREAMING.md). rings:
    // kVoices read-ahead rings (caller memory; nullptr = streaming off). A
    // streamed instrument's bank data is its head; its source names the rest.
    // Main loop; set a source only while no voice plays the instrument.
    void setStreamRings(StreamRing* rings) { rings_ = rings; }
    StreamRing* streamRings() { return rings_; }
    void setStreamSource(int instrument, const StreamSource* s) {
        if (instrument >= 0 && instrument < kSampleInstrumentCount) streams_[instrument] = s;
    }
    const StreamSource* streamSource(int instrument) const {
        return instrument >= 0 && instrument < kSampleInstrumentCount ? streams_[instrument] : nullptr;
    }
    // Play modes a streamed instrument can play (P1: forward one-shot and forward loop).
    static bool streamablePlayMode(uint8_t m) { return m == uint8_t(PlayMode::OneShot) || m == uint8_t(PlayMode::ForwardLoop); }
    MixerSettings& settings() { return mix_; }
    const MixerSettings& settings() const { return mix_; }

    // Called at the start of every block with the sequencer advance in
    // seconds (spec 04 §1.1 step 1 / §6.1); the app runs the sequencer here.
    using PreBlockFn = void (*)(void* ctx, float seconds);
    void setPreBlockHook(PreBlockFn fn, void* ctx) { hook_ = fn; hookCtx_ = ctx; }
    void setMidiOut(IMidiPort* port) { midi_ = port; }

    // Live output.
    static void fillCallback(int16_t* out, uint32_t frames, void* ctx);   // IAudioOut::FillFn
    void attachOutput(IAudioOut& out) { out.setCallback(&AudioEngine::fillCallback, this); }
    void renderLive(int16_t* interleaved);   // one 128-frame block
    // Render callback disable flag (spec 04 §1.1 "audio callback disabled").
    void setCallbackDisabled(bool d) { callbackDisabled_ = d; }
    // Line input for the next block: 128 interleaved int16 frames, or nullptr (silence).
    void setLineInput(const int16_t* interleaved) { lineIn_ = interleaved; }
    // [I] Live resampling: source 0 = master (limiter output, before master
    // volume, metronome and file preview), 1..8 = sample track t-1 (post
    // pan, as its stem). kTapLineIn (the line input) is tapped by the app
    // from the input itself (App::preBlock), not here. Null tap = off. Live
    // blocks only.
    static constexpr uint8_t kTapLineIn = 9;
    void setResampleTap(IResampleTap* tap, uint8_t source) {
        tapSource_ = source;
        tap_ = tap;
    }
    // Recorder monitor (spec 22 §5): stock 0x153B4(0x56) writes table[0x56] =
    // 1.0 into the line-in MSUM inputs, so while the Recorder monitors, the
    // line input reaches the master at unity whatever the Global Mixer line
    // level / mute (new projects have line in muted, 0x30A = 1). false: the
    // Global Mixer level applies. Set per block before renderBlock.
    void setLineMonitor(bool unity) { lineMonitor_ = unity; }
    bool lineMonitor() const { return lineMonitor_; }
    // Peak |sample| (Q15) of the line input as mixed into the master while
    // the Recorder monitors, since the last call (diag).
    int takeLineMonitorPeak() {
        const int p = int(lineMonPeak_ * 32768.0f);
        lineMonPeak_ = 0.0f;
        return p;
    }

    // Pad / instrument audition uses the track voices (spec 07 Q7, see
    // App::previewOn); the ninth player only plays the metronome click.
    // SD file preview (WAVPREVIEW, OUT mixer input 1): planar int16 buffer
    // played once at MixerSettings::previewVolume, stereo files in stereo
    // (spec 07 Q1); nullptr stops it. The buffer must stay valid while
    // playing (stopAll() also stops it).
    // `avail`: frames already decoded (streamed preview), default all.
    // `step`: 16.16 playback rate (65536 = own pitch; 2.x pitched pads,
    // linear interpolation per channel).
    void previewBuffer(const int16_t* data, uint32_t frames, bool stereo, uint32_t avail = 0xFFFFFFFFu,
                       uint32_t step = 65536u);
    // Same with separate lanes: `right` nullptr = mono (2.x stereo recorder).
    void previewBufferLR(const int16_t* left, const int16_t* right, uint32_t frames, uint32_t step = 65536u);
    bool previewBufferActive() const { return raw_.active; }
    // Frame of the preview buffer playing now (from the data passed in), -1 idle.
    int32_t previewBufferPos() const { return raw_.active ? int32_t(raw_.pos) : -1; }
    uint32_t previewBufferAvail() const { return raw_.avail; }
    uint32_t previewBufferStep() const { return raw_.step; }
    const int16_t* previewBufferLeft() const { return raw_.data; }
    const int16_t* previewBufferRight() const { return raw_.right; }   // nullptr = mono
    void previewStop();   // file preview, immediately
    // Streamed file preview: previewBuffer(data, total, stereo) with only
    // `frames` decoded so far; the player waits (silence) at that point and
    // continues as more frames arrive.
    void previewAvail(uint32_t frames);
    // Sequencer transport for tempo-locked LFOs (spec 07 Q5): returns true
    // while the sequencer plays and reports its tick counter (576 per step)
    // and the track-0 step index. Called from the audio-block context.
    using TransportFn = bool (*)(void* ctx, uint32_t* tick, int16_t* step0);
    void setTransportSource(TransportFn fn, void* ctx) { transport_ = fn; transportCtx_ = ctx; }
    // Peak |post-pan| level of track t (0..1) since the previous call (UI meters).
    float trackPeak(int t);
    // Global Mixer 3/3 meters: peak of channel ch (0 Delay return, 1 Reverb
    // return, 2 Dry mix, 3 Line in after its level), side 0 L / 1 R, since the
    // last call (read and reset; main loop).
    float busPeak(int ch, int side);

    // Play position (sample frame) of track t's voice as of the last rendered
    // block, -1 when its player is stopped. A per-block snapshot, safe to read
    // from the UI without locking (USER_REPORTS_R2 §1c).
    int32_t voicePlayhead(int t) const { return t >= 0 && t < kVoices ? head_[t] : -1; }

    // Performance FX offsets for track t (spec 06 §3.2/§3.4).
    void setPerf(int track, const PerfOffsets& p) { if (track >= 0 && track < kVoices) voices_[track].setPerf(p); }

    void stopAll();
    void clearEffects();   // zero delay and reverb memory
    Voice& voice(int t) { return voices_[t]; }
    const Voice& voice(int t) const { return voices_[t]; }

    // Offline rendering (spec 04 §6.1).
    void beginOffline();
    void renderOfflineBlock(int16_t* masterInterleaved, StemBlock* stems);
    void endOffline();
    // Render into WAV files: stems=false -> `path` is the master file;
    // stems=true -> `path` is a directory receiving track1..8/reverb/delay/master
    // and synth_1..3.wav (Mini %s/synth_%d.wav, spec 21 §2; 1-based: U13).
    // Stops after maxBlocks or when running(ctx) returns false.
    // resampleTo44100=false: device-rate samples with 44117 in the header, as
    // stock (spec 04 §6.1). [I] resampleTo44100=true: the WAVs are resampled to
    // 44.1 kHz and say 44100. Returns blocks rendered, <0 on I/O error.
    int32_t exportWav(IStorage& fs, const char* path, bool stems, uint32_t maxBlocks, bool (*running)(void*),
                      void* ctx, bool resampleTo44100);
    // Stem block buffer for exportWav (~7 KB; SDRAM on the device). Without
    // one, exportWav allocates it for the export.
    void setExportScratch(StemBlock* s) { exportScratch_ = s; }

    // Meters.
    float limiterGainReduction() const { return limL_.meanGain(); }
    float reverbLevel() const { return reverb_.level(); }
    float tempo() const { return tempo_; }
    bool metronomeActive() const { return voices_[kMetronomeVoice].active(); }
    uint64_t blocksRendered() const { return blocks_; }
    // 2.x render load (live blocks through fillCallback, cpu_meter.h) and the
    // synth voice governor evaluated after each of them (governor.h).
    CpuMeter& cpu() { return cpu_; }
    const CpuMeter& cpu() const { return cpu_; }
    Governor& governor() { return gov_; }
    const Governor& governor() const { return gov_; }
    // 2.x synth slots, voice pool and slot mixer (synth_bank.h); the
    // governor's IVoicePool.
    SynthBank& synth() { return synth_; }
    const SynthBank& synth() const { return synth_; }
    // Peak-block attribution of the live render (block_profile.h): the
    // costliest block of each 1 s CpuMeter window, split by phase, with its
    // events. Filled only while the meter has a cycle source.
    const ProfileWindow& profile() const { return profWin_; }
    // The profile of the last timed live block (benches, tests).
    const BlockProfile& lastBlockProfile() const { return prof_; }
    // What the pre-block hook (App::preBlock, the sequencer) did in this
    // block (block_profile.h SeqWork bits, pattern tracks copied). Counted
    // only inside the hook of a timed live block; main-loop calls are not.
    void noteSeqWork(uint8_t why, uint32_t patTracks = 0) {
        if (!inHook_) return;
        prof_.seqWhy = uint8_t(prof_.seqWhy | why);
        BlockProfile::add(prof_.nPatTracks, patTracks);
    }

    // ISequencerSink (spec 03 §6).
    void voiceNoteOn(uint8_t track, const VoiceNoteOn& n) override;
    void voiceNoteOff(uint8_t track, NoteOffMode mode) override;
    // 2.x live MIDI input pitch bend of the note on a track (Voice::setBend).
    void voiceSetBend(uint8_t track, float semis) {
        if (track < kVoices) voices_[track].setBend(semis);
    }
    void voiceApplyFx(uint8_t track, uint8_t slot, uint8_t type, uint8_t value) override;
    void resetAllVoiceFx() override;
    void engineBlockUpdate(float smoothedTempoBpm) override { tempo_ = smoothedTempoBpm > 0.0f ? smoothedTempoBpm : tempo_; }
    void metronomeClick(bool accent, uint8_t velocity) override;
    void midiSend(uint8_t portMask, const uint8_t* bytes, uint8_t len) override;
    void synthNoteOn(uint8_t track, const SynthNoteOn& n) override;
    void synthNoteOff(uint8_t track, NoteOffMode mode) override;
    void synthApplyFx(uint8_t track, uint8_t type, uint8_t value) override;
    void synthMacroFx(uint8_t slot, uint8_t macro, uint8_t value) override;
    void synthTransportStop() override;

private:
    void renderBlock(bool offline, int16_t* master, StemBlock* stems);
    void updateParams();
    void controlBlock(float blockUs);
    template <int kMode>
    void masterSum(float l0, float li);
    void masterSumClean(float l0, float li);   // masterSum<0>

    SampleBank bank_;
    StreamRing* rings_ = nullptr;
    const StreamSource* streams_[kSampleInstrumentCount] = {};
    const Instrument* instruments_ = nullptr;
    MixerSettings mix_;
    Voice voices_[kVoices + 1];
    // A note-off for a note that started in the same block waits one block,
    // so every note sounds for at least one block (the sequencer runs all of
    // a block's micro-steps before it renders; Gate Length 0 put note-on and
    // note-off into one block and the note never sounded; user report).
    volatile bool startedInBlock_[kVoices] = {};
    volatile int8_t pendingOff_[kVoices] = {};   // 0 none, else the NoteOffMode
    VoiceControl vc_[kVoices + 1];
    VoiceOut vout_[kVoices + 1];
    Delay delay_;
    Reverb reverb_;
    Limiter limL_, limR_;
    LineDynamics dynL_, dynR_;   // DYN0/DYN1, line-in send path (spec 04 §5.6)
    bool dynActive_ = false;     // dynamics state not yet settled
    Ramp lineLvl_, master_, lineSendD_, lineSendR_, click_;

    // cached parameter state
    uint32_t cDelayT_ = 0;
    uint16_t cDelayHz_ = 0;
    float cLimA_ = -1.0f, cLimR_ = -1.0f;
    uint8_t cLimMode_ = 0xFF;

    // blocks
    float dsend_[kBlock], rsend_[kBlock];
    float dL_[kBlock], dR_[kBlock], rL_[kBlock], rR_[kBlock];
    float lineL_[kBlock], lineR_[kBlock];
    float mL_[kBlock], mR_[kBlock];

    struct RawPreview {
        volatile bool active = false;
        const int16_t* data = nullptr;
        const int16_t* right = nullptr;   // right lane, nullptr = mono
        uint32_t frames = 0, pos = 0;
        uint32_t step = 65536, frac = 0;  // 16.16 rate and position fraction
        volatile uint32_t avail = 0;   // frames decoded so far (streamed preview)
    } raw_;
    volatile float peak_[kVoices] = {};
    volatile float busPeak_[4][2] = {};
    volatile int32_t head_[kVoices] = {};   // voicePlayhead() snapshot (init() sets -1)

    // metronome click instruments (normal / accent finetune, spec 07 Q10)
    Instrument clickIns_[2];
    bool transportPlaying(uint32_t* tick, int16_t* step0) const;
    TransportFn transport_ = nullptr;
    void* transportCtx_ = nullptr;

    PreBlockFn hook_ = nullptr;
    StemBlock* exportScratch_ = nullptr;
    void* hookCtx_ = nullptr;
    IMidiPort* midi_ = nullptr;
    const int16_t* lineIn_ = nullptr;
    IResampleTap* volatile tap_ = nullptr;
    volatile uint8_t tapSource_ = 0;
    int8_t tapLast_ = -1;   // track tapped in the last block (audio context)
    bool lineMonitor_ = false;
    volatile float lineMonPeak_ = 0.0f;
    float tempo_ = 130.0f;
    uint64_t blocks_ = 0;
    CpuMeter cpu_;
    Governor gov_;
    BlockProfile prof_;          // the block being rendered (live, timed)
    ProfileWindow profWin_;
    bool profOn_ = false;        // stamp this block (live with a cycle source)
    bool inHook_ = false;        // inside the pre-block hook (note work is attributed)
    SynthBank synth_;
    SynthMixInputs synthIn_;
    bool playing_ = false;       // transport state of the last block (governor reset on Stop)
    bool govPlaying_ = false;
    bool callbackDisabled_ = false;
    bool offline_ = false;
    bool ready_ = false;
};

}  // namespace audio
}  // namespace ot
