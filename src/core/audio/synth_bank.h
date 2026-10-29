// Synth framework: 3 slots, the global voice pool, macros, pitch FX and the
// slot mixer (2.x M3, docs/MINI_PLAN.md §3, §4 WP-E; spec 21 §3.3, §7, §8;
// docs/specs/2x-05 where it still fits the Mini model) [OT].
//
// Slot s (0..2) = the project.mt record (gain, pan, sends, polyphony,
// model, macro page values at +0x38) plus its SynthPatch (owned by the App,
// bound by pointer). Instruments 64..66 play slots 0..2 on any track.
//
// Voices: one global pool of kSynthMaxVoices records (SRAM, see below). The
// usable number is the governor's cap C <= budget B (Config "synth voices",
// default 4, OPTIMIZATION_2X item 10 / OP-8). Each slot stores polyphony
// 0..8 and plays min(polyphony, C) (0 = slot off). A new note:
//   1. polyphony 1: reuse the slot's voice (legato when its gate is held);
//   2. else release the track's previous note and take a free voice when
//      the slot and the pool have room;
//   3. else steal inside the slot (same note, then the quietest voice in
//      release, then the oldest), else globally by the governor rule.
// A stolen voice fades out over one block (2.9 ms) and stops counting at
// once; the new note takes a free record at once, or the stolen record
// after its fade when the pool is exhausted. A victim that started in the
// same block (it has not rendered a frame yet) is freed at once instead:
// its fade would be a 2.9 ms blip of a note nobody hears and costs a whole
// voice render (CPU peak work, docs/OPTIMIZATION_2X.md "Peak attribution").
//
// Note-on spreading (CPU peaks): at most kStartsPerBlock note-ons per block
// start a new voice record (the mono reuse of a polyphony-1 slot and the
// PERC hits are cheap and never wait). Further note-ons of that block are
// queued, in order, and start before anything else in the next block: a
// queued note (with its note-off / FX of the same block) is late by exactly
// one block, 2.9 ms, never more. A chord on 16 tracks thus spreads its voice
// starts and the steal fades they cause over two blocks. (Corner case: a
// queued note whose own note-off follows within the block, started when every
// record is busy, is dropped like a pending note that gets its note-off.)
//
// Macros: M1..M6 = the slot's page values (record +0x38), overridden by FX
// CC a..f until Stop; M7 = the note's Volume FX; M8 stored only. Law and the
// M7 rule: synth_params.h. Evaluated at block rate and ramped across the
// block (SynthParams start -> end).
//
// Stop: the Tracker Mini manual does not say whether Stop releases or cuts
// synth notes, so 2.x keeps the stock transport: the sequencer sends its
// note-offs and the Play key's Stop then cuts every voice at once
// (App::togglePlay -> AudioEngine::stopAll -> stopAll() here), samples and
// synths alike; no release tail. (2x-05 §3.3's "release, cut on a double
// Stop" is not used.)
//
// Mixing: voices -> slot bus (stereo) -> slot gain (dB) -> pan as balance
// (linear law, spec 04 §4.3) -> main bus; post-gain sends (L+R)/2 * SEND law
// * Global Mixer delay / reverb level. The bus feeds the Synth k stem.
//
// Real time: no allocation, no I/O. Main-loop setters only store values
// (the audio context picks them up at the next block). Memory (SRAM .bss,
// inside AudioEngine): voice pool 8 x 192 B = 1.5 KB (hot: touched every
// sample by the engines; uncached SDRAM would cost ~34 cycles per access),
// slots 3 x ~1.1 KB (parameters + engine scratch), bus 1 KB.
#pragma once

// Marker for the 2.x test-song generator (tools/make_testsongs/features.h).
#define OT_FEATURE_SYNTHS 1

#include <cstdint>

#include "audio/block_profile.h"
#include "audio/dsp.h"
#include "audio/governor.h"
#include "audio/synth_engine.h"
#include "audio/synth_params.h"
#include "model/project.h"
#include "model/synth_patch.h"
#include "sequencer/seq_sink.h"

namespace ot {
namespace audio {

constexpr int kSynthTracks = 16;                  // sequencer tracks
constexpr int kSynthAuditionTrack = kSynthTracks; // UI pads / preview (WP-G)
// MIDI input notes on a synth instrument (spec 17 §1.4 for synths [OT]): one
// track per held note, so external notes play polyphonically up to the
// slot's polyphony and the voice cap.
constexpr int kSynthLiveTracks = 4;
constexpr int kSynthLiveTrack0 = kSynthAuditionTrack + 1;
constexpr int kSynthAllTracks = kSynthLiveTrack0 + kSynthLiveTracks;

// What the synth UI shows for one slot (SynthBank::slotView).
struct SynthSlotView {
    int32_t engine = 0;          // Tracker Mini engine id of the record's model
    const char* model = "";      // "ACD".."PERC" ("" for a bad record)
    bool available = false;      // false: "Synth k (WTFM) not available", silent
    bool patchBound = false;     // a patch is attached
    bool patchMismatch = false;  // patch engine != record model (silent)
    int polyphony = 0;           // stored 0..8
    int playable = 0;            // min(polyphony, cap): what plays now
    int sounding = 0;            // voices of this slot sounding
    bool limited = false;        // the governor cap is below the polyphony
    uint8_t macro[kSynthSeqMacros] = {};   // effective M1..M6 (override or page)
    bool overridden[kSynthSeqMacros] = {}; // FX CC override active
    float lastVelocity = 1.0f;   // M7 of the last note
};

// Per-block mixer inputs from AudioEngine::controlBlock.
struct SynthMixInputs {
    float delayLevel = 1.0f;    // Global Mixer delay level (0 when muted)
    float reverbLevel = 1.0f;
    // Track Mixer fader of each sequencer track's synth notes (TRK law; 0
    // when muted / soloed out). Tracks 9-16 have no fader (Tracker Mini): 1.
    float trackGain[16];
    SynthMixInputs() {
        for (float& g : trackGain) g = 1.0f;
    }
};

class SynthBank : public IVoicePool {
public:
    SynthBank();
    // Everything silent, overrides cleared, slots re-read at the next block.
    void reset();
    void setGovernor(const Governor* g) { gov_ = g; }
    // Voice starts per block before note-ons wait one block (see above);
    // 0 = no limit (tests). Default kStartsPerBlock.
    static constexpr int kStartsPerBlock = 3;
    void setStartsPerBlock(int n) { startCap_ = n < 0 ? 0 : n; }
    int queuedNotes() const { return nDefer_; }
    // Block profile of the live block being rendered (block_profile.h):
    // event counts and per-slot engine cycles; nullptr = off.
    void setProfile(BlockProfile* p) { prof_ = p; }
    // Tests: play every available model with this engine (nullptr = registry).
    void setEngineOverride(ISynthEngine* e) {
        override_ = e;
        for (auto& b : rebind_) b = true;
    }

    // --- main loop (App / UI) ---------------------------------------------
    // Attach slot s's patch (stable storage, e.g. App::synthPatch(s)).
    void bindPatch(int s, const SynthPatch* p);
    // The patch's values / macros / engine changed: re-read at the next block
    // (parameter edits apply live; a new engine restarts the slot).
    void patchChanged(int s);
    // The project record of slot s (spec 21 §3.3). Gain / pan / sends ramp,
    // polyphony applies to new notes, a model change silences the slot.
    void setSlotRecord(int s, const SynthSlotRecord& r);
    // A hand edit of macro m (Synth Macros page) takes over from its FX CC
    // override: the page value applies from the next block (slotView shows it
    // at once); the next FX CC on that macro overrides it again.
    void releaseMacro(int s, int m);
    const SynthSlotRecord& slotRecord(int s) const { return slots_[clampSlot(s)].rec; }
    SynthSlotView slotView(int s) const;
    bool slotAvailable(int s) const;
    // Pads / preview (WP-G): play slot s on the audition track.
    void auditionOn(int s, uint8_t note, int8_t volume = -1);
    void auditionOff();
    // MIDI input note k (0..kSynthLiveTracks-1) on slot s; volume -1 = full
    // velocity, else 0..100. Main loop; the audio block starts it.
    void liveOn(int k, int s, uint8_t note, int8_t volume);
    void liveOff(int k);
    // 2.x live MIDI input pitch bend in semitones for every note the live
    // tracks started (releases included). Main loop; the audio block applies it.
    void setLiveBend(float semis) { liveBend_ = semis; }

    // --- audio context (sequencer sink, AudioEngine) -------------------------
    void noteOn(int track, const SynthNoteOn& n);
    void noteOff(int track, NoteOffMode mode);
    void applyFx(int track, uint8_t type, uint8_t value);
    void macroFx(int slot, int macro, uint8_t value);
    void transportStop();     // macro overrides -> page values
    void resetFx();           // pitch FX state of every track (play start)
    void stopAll();           // every voice silent now (no fade)
    // True when a voice sounds or a slot ramp is still moving, a UI
    // audition request waits (it is taken in render's sync) or note-ons are
    // queued / counted for the block (note-on spreading).
    bool active() const {
        return activeVoices_ != 0 || aud_.req != 0 || nDefer_ != 0 || starts_ != 0 || livePending();
    }
    // Render one block: adds into mL/mR and the send buses. stems[s] (may be
    // nullptr) receives slot s's post-gain/pan bus as interleaved int16.
    // Returns true when anything was added.
    // [I] tapTrack >= 0: every note that track started (releases included)
    // is also added into tapL / tapR at its slot's gain / pan, dry (live
    // resampling, Track source; other tracks' notes on the slot are not).
    bool render(const SynthMixInputs& in, float* mL, float* mR, float* dsend, float* rsend, int16_t* const stems[kSynthSlots],
                int tapTrack = -1, float* tapL = nullptr, float* tapR = nullptr);

    // --- IVoicePool (governor, spec 2x-02 §3) --------------------------------
    int sounding() const override;
    bool stealOne() override;

    // Diagnostics / tests.
    const SynthParams& params(int s) const { return slots_[clampSlot(s)].p; }
    int voicesOf(int s) const;   // sounding (not fading) voices of slot s
    // The voice a track holds (nullptr: none); UI voice-activity row / tests.
    const SynthVoice* voiceOfTrack(int track) const {
        if (track < 0 || track >= kSynthAllTracks || trackVoice_[track] < 0) return nullptr;
        return &voices_[trackVoice_[track]].v;
    }
    int busyRecords() const;     // records in use (sounding + fading)
    uint32_t steals() const { return steals_; }
    // Peak of slot s's post gain / pan bus since the last call (main loop:
    // the Global Mixer 3/3 synth meters, Tracker Mini manual p.220).
    float slotPeak(int s);
    // Peak of the synth notes of sequencer track t since the last call (Track
    // Mixer meters: a track playing a synth meters its share of the slot bus,
    // split by the voices' envelope level x gain). Main loop.
    float trackPeak(int t);

private:
    enum VState : uint8_t { kFree = 0, kOn, kFading };
    struct Voice {
        SynthVoice v;            // engine-visible part
        uint32_t age = 0;        // note-on serial (oldest = smallest)
        float g = 0.0f;          // framework gain at the end of the last block
        float vel = 1.0f;        // M7 (0..1)
        float bendTarget = 0.0f; // step-FX pitch target (semitones)
        float bendStep = 0.0f;   // semitones per block towards bendTarget
        float liveBend = 0.0f;   // live pitch bend already added into bend / bendTarget
        bool live = false;       // started on a live track (follows setLiveBend)
        uint8_t state = kFree;
        int8_t track = -1;       // holder (-1 none)
        int8_t pending = -1;     // pend_[] entry started on this record after its fade
        uint8_t fresh = 0;       // started since the last render (no frame out yet)
        int8_t sendD = -1;       // step FX Delay / Reverb Send 0..100 for this note, -1 = the slot's
        int8_t sendR = -1;
        int8_t pan = -1;         // step FX Panning 0..100 (50 centre) for this note, -1 none
    };
    // A note-on queued to the next block (note-on spreading) with the
    // note-off / FX that followed it in its block.
    struct Deferred {
        int8_t track = -1;
        int8_t off = 0;          // 0 none, else the NoteOffMode that followed
        uint8_t nfx = 0;
        SynthNoteOn n;
        uint8_t fx[2][2] = {};   // type, value
    };
    // A note waiting for a fading record (only when every record is busy).
    struct Pending {
        int8_t record = -1;      // -1 = entry free
        int8_t track = -1;
        uint8_t slot = 0;
        SynthNote note;
        float bend = 0.0f, bendTarget = 0.0f, bendStep = 0.0f;
        int8_t sendD = -1, sendR = -1;   // the note step's Delay / Reverb Send FX
        int8_t pan = -1;                 // the note step's Panning FX
    };
    static constexpr int kPending = 2;
    struct Slot {
        SynthSlotRecord rec;
        const SynthPatch* patch = nullptr;
        ISynthEngine* eng = nullptr;   // nullptr: silent (unavailable / mismatch / poly 0 still loads)
        int32_t engineId = 0;
        bool mismatch = false;
        SynthParams p;
        // macro state
        uint8_t page[kSynthSeqMacros] = {};
        int16_t ovr[kSynthSeqMacros] = {-1, -1, -1, -1, -1, -1};   // FX override, -1 none
        uint8_t applied[kSynthSeqMacros] = {};   // values the end[] reflect
        float lastVel = 1.0f;
        bool macroDirty = true;
        // compiled targets of M1..M6 (+ M7 non-Volume targets at slot level)
        struct Tgt { uint8_t param; uint8_t macro; float k; };   // k = amount * (max - min) (M7: amount)
        Tgt tgt[kSynthSeqMacros * kPatchMacroTargets];
        uint8_t ntgt = 0;
        Tgt tgt7[kPatchMacroTargets];
        uint8_t ntgt7 = 0;
        float volK = 0.0f;        // sum of M7 amounts on the Volume parameter
        int volIdx = -1;          // Volume parameter (-1: PERC / unknown)
        uint8_t touched[(kSynthSeqMacros + 1) * kPatchMacroTargets];   // params moved last block
        uint8_t ntouched = 0;
        bool fullCopy = false;    // every start[] catches up (patch edit ramp)
        // end[] was moved since the last render and start[] already holds
        // the value at the next block start: render must not catch up first.
        bool fresh = false;
        float gainLin = 0.0f;     // dbToGain(rec.gainDb), 0 at -96 dB
        float mixL = 0.0f, mixR = 0.0f;   // gain x balance
        float mixD = 0.0f, mixE = 0.0f;   // 0.5 x SEND[delay / reverb send]
        float volMin = 0.0f, volMax = 2.0f;   // Volume parameter range
        // mixer ramps
        Ramp gl, gr, sd, sr;
        // [OT] Width (stereo S2): the patch value, the value the last block
        // ended on, and the side signal's allpass chain state (x1, y1).
        float widthEnd = 0.0f, widthCur = 0.0f;
        float ap[4][2] = {};
        SynthSlotScratch scratch;
    };

    static int clampSlot(int s) { return s < 0 ? 0 : (s >= kSynthSlots ? kSynthSlots - 1 : s); }
    int cap() const;
    int playable(int s) const;
    void sync();   // main-loop changes -> audio state (start of noteOn / render)
    void noteOnNow(int track, const SynthNoteOn& n, bool forced = true);
    void flushDeferred();
    Deferred* deferredOf(int track);
    void bindEngine(int s);
    void reloadParams(int s, bool jump);
    void applyMacros(int s);
    // [OT] Width on the slot bus (busL_ / busR_), before mixSlot; nothing
    // at all while the width is and stays 0.
    void widen(Slot& sl);
    void catchUp(Slot& sl);   // start[] = end[] of the moves of the last block
    float paramBase(const Slot& sl, int i) const;
    float voiceGain(const Slot& sl, const Voice& v) const;
    int freeRecord() const;
    int victimInSlot(int s, uint8_t note) const;
    int victimGlobal() const;
    void fade(int r, bool steal = false);
    void startOn(int r, int track, int s, const SynthNote& n, float bend, float bendTarget, float bendStep);
    void releaseTrack(int track);
    float mixSlot(Slot& sl, float* mL, float* mR, float* dsend, float* rsend, int16_t* stem);   // block peak
    void notePitchFx(const SynthNoteOn& n, float prevPitch, bool hasPrev, float& bend, float& target, float& step) const;

    Voice voices_[kSynthMaxVoices];
    Pending pend_[kPending];
    Deferred defer_[kSynthTracks + 1];   // MIDI input tracks never wait
    int nDefer_ = 0;
    int startCap_ = kStartsPerBlock;
    int starts_ = 0;              // voice starts in the current block
    bool flushDue_ = false;       // a render ended since the queue was filled
    bool recApplied_[kSynthSlots] = {};   // slots_[s].rec has been applied (sync)
    Slot slots_[kSynthSlots];
    int8_t trackVoice_[kSynthAllTracks];      // record held by each track, -1 none
    // [I] Track that started each record, kept through its release (the
    // resample tap); -1 free. Not in Voice: one more byte pads each record.
    int8_t owner_[kSynthMaxVoices];
    float trackPitch_[kSynthAllTracks];       // last note pitch (Glide FX source)
    uint32_t serial_ = 0;
    uint32_t steals_ = 0;
    int activeVoices_ = 0;                    // records not free
    const Governor* gov_ = nullptr;
    BlockProfile* prof_ = nullptr;
    ISynthEngine* override_ = nullptr;
    // Main loop -> audio handshake (one writer each side; a flag is cleared
    // before its data is read, so a later write is seen at the next block).
    volatile bool rebind_[kSynthSlots] = {};  // engine / patch must be re-read
    volatile bool recDirty_[kSynthSlots] = {};
    volatile bool ovrRelease_[kSynthSlots][kSynthSeqMacros] = {};   // releaseMacro()
    SynthSlotRecord pendRec_[kSynthSlots];
    struct Audition {
        volatile uint8_t req = 0;   // 1 note on, 2 note off
        uint8_t slot = 0, note = 60;
        int8_t vol = -1;
    } aud_;
    // MIDI input mailbox per live track, main loop -> audio block: one
    // 32-bit word (a single store / load, never torn) holding the latest
    // request: bits 0-7 serial, 8 on, 9-10 slot, 11-17 note, 24-31 volume.
    // Only the final state of a track counts, so no note-off can be lost;
    // render() alone reads it (never the main-loop noteOn path).
    volatile uint32_t live_[kSynthLiveTracks] = {};
    volatile float liveBend_ = 0.0f;            // setLiveBend (main loop -> audio)
    uint8_t liveSeen_[kSynthLiveTracks] = {};   // audio side: last serial
    uint8_t liveSerial_[kSynthLiveTracks] = {}; // main loop: last serial
    void liveDrain();
    bool livePending() const {
        for (int k = 0; k < kSynthLiveTracks; ++k)
            if (uint8_t(live_[k]) != liveSeen_[k]) return true;
        return false;
    }
    float busL_[kBlock] = {}, busR_[kBlock] = {};   // slot bus, zero between slots
    float voiceL_[kBlock] = {}, voiceR_[kBlock] = {};
    int8_t noteSendD_ = -1, noteSendR_ = -1;   // sends of the note being started (startOn)
    int8_t notePan_ = -1;                      // its Panning FX   // a note with its own sends (step FX)
    volatile float peak_[kSynthSlots] = {};          // bus peaks for slotPeak()
    volatile float trackPeak_[kSynthTracks] = {};    // per-track shares for trackPeak()
    float tempo_ = 130.0f;

public:
    void setTempo(float bpm) { if (bpm > 0.0f) tempo_ = bpm; }
};

}  // namespace audio
}  // namespace ot
