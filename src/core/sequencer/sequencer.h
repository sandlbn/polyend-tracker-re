// Sequencer engine: transport, block-driven tick clock, step and FX
// processing, MIDI clock/transport, live recording (spec 03).
//
// Deterministic and allocation-free. All state lives in the object; patterns
// are two caller-owned buffers (current / preload) that the sequencer swaps.
#pragma once

#include <atomic>
#include <cstdint>

#include "model/constants.h"
#include "model/pattern.h"
#include "model/project.h"
#include "sequencer/seq_playmodes.h"
#include "sequencer/seq_rng.h"
#include "sequencer/seq_sink.h"
#include "sequencer/seq_tables.h"

namespace ot {

// Settings the sequencer reads (spec 03 §4.1 settings block). Defaults are
// OpenTracker choices where the spec gives none.
struct SequencerSettings {
    uint8_t clockIn = 0;        // 0 internal, 1 USB, 2 MIDI jack
    uint8_t clockOut = 0;       // port mask 0..3
    uint8_t transportIn = 0;    // 0 off, 1 USB, 2 MIDI jack
    uint8_t transportOut = 0;   // port mask 0..3
    uint8_t midiOut = 3;        // port mask 0..3
    uint8_t middleC = 2;        // note sent = note + 12*(2 - middleC)
    uint8_t clockSyncCorrection = 6;  // index 0..12 -> -6..+6 pulses
    uint8_t recordingOptions = 0;     // 0 notes, 1 micro, 2 velocity, 3 micro+velocity
    uint8_t metronome = 0;      // on during recording
    uint8_t preRoll = 0;        // 1 = count-in before recording
    uint8_t tsNum = 3;          // beats per bar - 1
    uint8_t tsDen = 3;          // steps per beat - 1
};

// Caller-owned memory for per-track pattern overrides (spec 12 §C.1). Place
// it in SDRAM (AppMemory); host tests allocate it on the heap.
// Each voice track has two track slots: one plays (the active override), the
// other is staged by the main loop. A commit flips the slot index, so the
// audio ISR never copies track data. `scratch` receives the whole source
// pattern; it is separate from the preload buffer, so a song/queued preload
// is never clobbered (stock reuses the preload buffer, spec 12 §C.1).
// 2.x (docs/M1_PLAN.md §3): every track 0..15 has an override.
struct PerfPatternMemory {
    Track slot[kTrackCount][2];
    Pattern scratch;
};
static_assert(sizeof(Track) == 769, "track record is 769 bytes (spec 02 §4.2)");

class Sequencer {
public:
    // 2.x: 16 tracks (spec 2x-01 §6). Tracks 8..15 play MIDI and synths; synths
    // (instruments 64..66) play on all 16 tracks (Mini manual 6.13).
    static constexpr int kTracks = kTrackCount;   // 16
    static constexpr int kVoiceTracks = 8;        // tracks 0..7 own a sample voice

    Sequencer() { resetState(); }

    // --- wiring -------------------------------------------------------------
    // bufA becomes the current pattern, bufB the preload buffer. stage
    // (optional, a third buffer): switchPatternNow() assembles the new
    // pattern there in the main loop and the audio context swaps it in (no
    // pattern copy in the audio block). nullptr keeps the stage buffer of an
    // earlier attach() unless it is bufA or bufB; with none, the audio
    // context loads the pattern itself (one copy in that block).
    void attach(ISequencerSink* sink, ISequencerHost* host, Project* project, Pattern* bufA, Pattern* bufB,
                Pattern* stage = nullptr);
    SequencerSettings& settings() { return settings_; }
    void seedRandom(uint32_t s) { rng_.seed(s); }
    uint32_t randomState() const { return rng_.state(); }

    // --- time base (spec 03 §1.1) ------------------------------------------
    // Real-time: called once per audio block of `frames` samples.
    void processBlock(uint32_t frames = seq::kBlockFrames);
    // Advance by an explicit elapsed time (offline render uses
    // seq::kRenderBlockSeconds per block), then run the engine update.
    void processElapsed(float seconds);
    void setSuspended(bool s) { suspended_ = s; }
    void setRendering(bool r) { rendering_ = r; }

    // --- transport (spec 03 §2.1) -------------------------------------------
    void play(int startStep = 0);                 // pattern mode
    void playSong(int position);                  // song mode
    void playSelection(int stepStart, int stepEnd, int trackStart, int trackEnd, bool loop);
    void stop();
    void record();                                // record start (pre-roll / arm)
    // Toggle the recording flag (seq+0x2b) without touching the transport
    // (stock 0x111c0): Rec while recording -> false, Rec held + Play -> true.
    void setRecording(bool on) { recording_ = on; }
    void queuePattern(int delta);
    // spec 16 §A.2 (Pattern + Up/Down, FW:0x3d548): switch to pattern `num`
    // (1..255) at once, also while playing. The sequencer is re-pointed like
    // 0x11a34 (swap buffers, Tempo-FX override and roll/arp cleared); the step
    // positions are kept (wrapped when beyond the new length). Main loop:
    // with a stage buffer it loads the pattern there (host loadPattern, in
    // the main loop), then publishes the request; the audio context swaps
    // the stage buffer in at its next block (pointer swap, OP-1). Without one
    // the audio context loads it (one copy from the resident pattern store).
    // While stopped it commits now. A request not yet committed is replaced.
    // A queued Left/Right change stays queued.
    void switchPatternNow(uint8_t num);
    Pattern* stagePattern() { return stage_.load(std::memory_order_relaxed); }
    // Pattern number a not-yet-committed switchPatternNow() will play, else 0.
    uint8_t pendingSwitch() const { return pendSwitch_.load(std::memory_order_acquire); }
    // Song screen F1 deferred action (spec 06 §2.3): flips seq+0x1c without
    // restarting, so the next pattern end loops (off) or advances (on).
    void setSongMode(bool on) { songMode_ = on; }
    // UI play button: arms instead of playing when an external clock is selected.
    void userPlay(int startStep = 0);

    // --- MIDI input (spec 03 §4.2/§4.3); port = where the byte arrived ------
    void midiRealtimeIn(MidiPortId port, uint8_t byte);
    void onClock();
    void onStart(bool continueMsg = false);
    void onStop();

    // --- live input (spec 03 §5) -------------------------------------------
    // Returns the track the note went to (-1 if none). `instrumentVolume` is
    // the selected instrument's volume used by velocity recording.
    int liveNoteOn(uint8_t note, uint8_t velocity, uint8_t cursorTrack, uint8_t instrument,
                   uint8_t instrumentVolume = 127);
    // writeOff = false: release only, no OFF step (Instr / FX column recording,
    // manual p.95).
    void liveNoteOff(uint8_t note, bool writeOff = true);
    // Step entry while stopped: write `note` at (step, track) (spec 03 §5.1).
    void stepEntry(uint8_t note, uint8_t velocity, int step, int track, uint8_t instrument);
    void setRecordEnabled(int track, bool en) { if (validTrack(track)) tracks_[track].recEnabled = en; }

    // --- performance mode (spec 03 §2.6, spec 12 §C/§D) ---------------------
    // on: per-track lengths from the playing data. off (leaving the screen,
    // spec 12 §C.4): global length override off, overrides and queues
    // dropped, play modes reverted (step re-sync), repeat/velocity/transpose/
    // injection cleared. Voice FX reverts are the caller's job.
    void setPerformanceMode(bool on);
    bool performanceMode() const { return perfMode_; }
    void setPerfGlobalLength(int lastIndex) { perfGlobalLen_ = int16_t(lastIndex); }   // -1 = per track
    void setPerfTrackLength(int t, int lastIndex) { if (validTrack(t)) perfLen_[t] = int16_t(lastIndex); }
    // Play mode 0..22 (spec 12 §D); used on every track in performance mode
    // (2.x: tracks 8..15 too, docs/M1_PLAN.md §3).
    // Changing to 0 from another mode re-syncs the step to the master step.
    void setPerfPlayMode(int t, uint8_t mode);
    uint8_t perfPlayMode(int t) const { return validTrack(t) ? tracks_[t].playMode : 0; }
    void setPerfRepeat(int t, int8_t idx) { if (validTrack(t)) tracks_[t].repeatIdx = idx; }
    void setPerfVelocityOffset(int t, int8_t off) { if (validTrack(t)) tracks_[t].velOffset = off; }
    void setPerfTranspose(int t, bool on, int8_t semis) {
        if (validTrack(t)) { tracks_[t].transposeOn = on; tracks_[t].transpose = semis; }
    }
    void setPerfInjectedFx(int t, uint8_t type, uint8_t value) {
        if (validTrack(t)) { tracks_[t].inject.type = type; tracks_[t].inject.value = value; }
    }
    StepFx perfInjectedFx(int t) const { return validTrack(t) ? tracks_[t].inject : StepFx{}; }

    // --- per-track pattern override (spec 12 §C) ----------------------------
    // Main-loop API. Without memory the override calls fail (return false).
    void setPerfPatternMemory(PerfPatternMemory* mem) { perfMem_ = mem; }
    // Load pattern `number` (1..255) for track t (0..15) through the
    // host (synchronous) into that track's staging slot and enter performance
    // mode. immediate: the track's next step plays the new data (while
    // stopped: now). Otherwise it is queued and commits when that track wraps
    // (its own loop end); a later call for the same track replaces the queue.
    bool setTrackPattern(int t, uint8_t number, bool immediate);
    // Pattern number playing on track t (0 = the current pattern, no override).
    uint8_t trackPattern(int t) const { return (validTrack(t) && ovActive_[t]) ? ovNum_[t] : 0; }
    // Queued (not yet committed) pattern for track t, 0 = none.
    uint8_t queuedTrackPattern(int t) const { return validTrack(t) ? pendNum_[t].load() : 0; }
    // Drop every override and pending queue (tracks play the current pattern).
    void clearTrackPatterns();

    // --- status --------------------------------------------------------------
    bool isPlaying() const { return playing_; }
    bool isRecording() const { return recording_; }
    bool isArmed() const { return armed_; }
    bool isRendering() const { return rendering_; }
    bool isSongMode() const { return songMode_; }
    bool isSelectionPlaying() const { return playing_ && selection_; }
    bool isCountingIn() const { return countInSteps_ > 0; }
    // Ticks until the count-in ends and playback starts (0 when not counting in).
    uint32_t countInTicksLeft() const {
        if (countInSteps_ <= 0) return 0;
        const uint32_t total = uint32_t(countInSteps_) * uint32_t(seq::kTicksPerStep);
        return countInTicks_ < total ? total - countInTicks_ : 0;
    }
    float effectiveTempo() const;
    float tickPeriodUs() const;
    float externalTempo() const { return extBpm_; }
    bool externalClockLost() const { return extBpm_ == 0.0f; }  // "NO CLK"
    int step(int track = 0) const { return validTrack(track) ? tracks_[track].step : 0; }
    int microStep(int track = 0) const { return validTrack(track) ? tracks_[track].micro : 0; }
    int masterMicroStep() const { return masterMicro_; }
    int masterStep() const { return masterStep_; }
    // Pattern position that drives the pattern end: wraps at the base pattern
    // length (selection / global length override included), never at a
    // per-track length or play mode (docs/M1_PLAN.md §3).
    int patternPosition() const { return masterPos_; }
    int songPosition() const { return songPos_; }
    uint8_t currentPatternNumber() const { return curPatternNum_; }
    int queuedPattern() const { return queueActive_ ? queued_ : 0; }
    Pattern* currentPattern() { return cur_; }
    Pattern* preloadPattern() { return next_; }
    bool tempoOverrideActive() const { return tempoOverride_; }
    bool swingActive() const { return swingOn_; }
    float swingValue() const { return swing_; }
    bool swingPhase() const { return swingPhase_; }
    uint32_t countInMs() const;
    uint64_t tickCount() const { return tickCount_; }   // tick() calls while playing (diagnostic)
    // Tick counter seq+0x10 (1 at play start, 576 per step): the phase base of
    // tempo-locked LFOs (spec 07 Q5).
    uint32_t tickPosition() const { return tick_; }

private:
    struct NoteRec {
        int8_t note = kNoteEmpty;
        uint8_t instr = 0;
        StepFx fxA;   // step bytes +2/+3 = model fx[1]
        StepFx fxB;   // step bytes +4/+5 = model fx[0]
        int8_t volume = -1;
    };
    // Step context carried from the FX pass to note handling (possibly delayed).
    struct Pending {
        NoteRec rec;
        int16_t gateLimit = -1;
        bool arpSetup = false;
        uint8_t arpDir = 0;
        uint16_t arpPeriod = 0;
        uint16_t arpMask = 0;
        uint8_t arpCol = 0;
        bool chord = false;
        uint8_t chordValue = 0;
    };
    struct TrackState {
        NoteRec last;            // last-played record
        bool enabled = true;     // selection play
        bool recEnabled = true;
        bool sounding = false;
        uint8_t micro = 1;       // 1..48
        int16_t step = 0;
        int16_t stepsPlayed = 0;
        uint16_t sinceTrigger = 0;
        int16_t gate = 0;
        int16_t gateLimit = -1;
        // roll
        bool rollActive = false;
        bool rollThisStep = false;
        uint8_t rollType = 0;
        uint8_t rollRate = 0;
        uint8_t rollCol = 0;
        uint8_t rollDistance = 1;
        int16_t rollBaseVol = 100;
        // arp
        bool arpActive = false;
        uint8_t arpDir = 0;
        uint16_t arpPeriod = 0;
        uint16_t arpCounter = 0;
        uint16_t arpMask = 0;
        uint8_t arpBase = 0;
        uint8_t arpPos = 0;
        uint8_t arpCol = 0;
        // micro delay
        bool delayActive = false;
        uint8_t delay = 0;
        uint8_t delayCounter = 0;
        Pending delayed;
        // MIDI notes currently held by this track
        uint8_t midiCh = 0;
        uint8_t midiCount = 0;
        uint8_t midiNotes[4] = {};
        // perf
        int8_t repeatIdx = -1;
        uint8_t playMode = 0;
        int8_t velOffset = 0;
        bool transposeOn = false;
        int8_t transpose = 0;
        StepFx inject;
        // live input
        bool liveHeld = false;
        uint8_t liveNote = 0;
        uint8_t liveInstr = 0;   // the held live note's instrument (its recorded OFF gets it)
        int16_t liveStep = -1;
        int16_t skipStep = -1;
    };

    static bool validTrack(int t) { return t >= 0 && t < kTracks; }

    void resetState();
    void advance(float seconds);
    void updateExternalTempo(float seconds);
    void tick();
    void microStep();
    void processTrack(int t);
    void stepStart(int t);
    void handleNote(int t, Pending& p);
    void advanceStep(int t);
    void patternEnd();
    void swapBuffers();
    void afterSwap();
    void commitSwitch();
    void startPreload();
    void beginPreload(uint8_t num);
    void preloadPart(int maxTracks);
    void dropPreload() {
        preloadIssued_ = false;
        partNext_ = kTracks;
    }
    int preloadTracksPerBlock() const;
    void resetPositions(int startStep);
    void startPlayback(int startStep, bool keepSongMode, bool afterCountIn);

    void triggerNote(int t, const NoteRec& rec, bool chord, uint8_t chordValue);
    void noteOffTrack(int t, NoteOffMode mode);
    void releaseAllNotes();
    void clearRoll(TrackState& tr) { tr.rollActive = false; tr.rollThisStep = false; }
    void clearArp(TrackState& tr) { tr.arpActive = false; }
    void rollRetrigger(int t);
    void arpNext(int t);
    int rollVolume(const TrackState& tr) const;
    int distanceToNextNote(int t, int fromStep) const;

    int lastIndex(int t) const;
    // Last step index of the pattern itself (no per-track length): drives masterPos_.
    int baseLastIndex() const;
    void advanceMaster();
    // Track data the sequencer plays: the override slot or the current pattern.
    const Track& playTrack(int t) const {
        return (ovActive_[t] && perfMem_) ? perfMem_->slot[t][ovSlot_[t]] : cur_->tracks[t];
    }
    // Owner decision (M1-WP4): Performance FX apply to all 16 tracks.
    bool perfTrack(int t) const { (void)t; return perfMode_ && !selection_; }
    int syncPosition() const { return masterStep_ - syncOrigin_; }
    void resyncTrack(int t);
    void commitTrackPattern(int t);
    void discardTrackOverrides();
    void reloadPerfLengths();
    int loopStart() const { return selection_ ? selStart_ : 0; }
    bool isMuted(int t) const;
    const MidiInstrument midiInstr(uint8_t instr) const;

    // MIDI out helpers (spec 03 §4.1, §4.4).
    void sendRealtime(uint8_t ports, uint8_t byte);
    void sendMidi(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t len);
    uint8_t midiNoteNumber(int note) const;
    void sendCcFx(int t, uint8_t letter, uint8_t value, const NoteRec& rec);
    void sendPitchBendFx(uint8_t instr, uint8_t value);

    bool liveTargetStep(int t, int& target) const;
    static bool cellFree(const Step& s);

    ISequencerSink* sink_ = nullptr;
    ISequencerHost* host_ = nullptr;
    ISequencerHost defaultHost_;
    Project* project_ = nullptr;
    Pattern* cur_ = nullptr;
    Pattern* next_ = nullptr;
    // switchPatternNow staging buffer. Owned by the main loop while
    // pendSwitch_ == 0, by the audio context (which swaps it with cur_)
    // while non-zero.
    std::atomic<Pattern*> stage_{nullptr};
    SequencerSettings settings_;
    seq::ParkMiller rng_;

    TrackState tracks_[kTracks];

    // transport
    bool playing_ = false;
    bool recording_ = false;
    bool armed_ = false;
    bool rendering_ = false;
    bool suspended_ = false;
    bool songMode_ = false;
    bool perfMode_ = false;
    bool selection_ = false;
    bool selLoop_ = false;
    int16_t selStart_ = 0, selEnd_ = 0;
    uint8_t selTrackStart_ = 0, selTrackEnd_ = kTracks - 1;
    int songPos_ = 0;
    uint8_t curPatternNum_ = 1;
    uint8_t nextPatternNum_ = 0;
    bool preloadIssued_ = false;
    // Spread preload (host loadsPatternTracks(), CPU round 3): the next
    // track of next_ to copy; kTracks = nothing pending. A few tracks per
    // audio block during the preload step, the rest at the pattern end.
    uint8_t partNext_ = kTracks;
    float blockUs_ = 2901.0f;   // last advance() block length
    bool queueActive_ = false;
    bool queueReady_ = false;
    int queued_ = 0;
    bool breakPending_ = false;
    bool breakPreloadDone_ = false;

    // clock
    uint32_t tick_ = 1;
    uint64_t tickCount_ = 0;
    uint8_t masterMicro_ = 1;
    int32_t masterStep_ = 0;
    int16_t masterPos_ = 0;               // pattern position, wraps at baseLastIndex()
    float accUs_ = 0;

    // tempo
    bool tempoOverride_ = false;
    bool stopAtStepEnd_ = false;
    float overrideBpm_ = 120.0f;
    bool swingOn_ = false;
    bool swingPhase_ = false;
    float swing_ = 50.0f;
    float tempoHist_[4] = {};
    uint8_t tempoHistPos_ = 0;
    bool tempoHistInit_ = false;

    // external clock
    int32_t pendingPulses_ = 0;
    int32_t pendingCorrection_ = 0;
    int32_t leftover_ = 0;
    uint32_t pulsesThisBlock_ = 0;
    float sinceClockS_ = 0;
    float intervalAcc_ = 0;
    float intervals_[48] = {};
    uint8_t intervalPos_ = 0;
    uint8_t intervalCount_ = 0;
    float extBpm_ = 130.0f;

    // count-in
    int32_t countInSteps_ = 0;
    uint32_t countInTicks_ = 0;

    // performance
    int16_t perfGlobalLen_ = -1;
    int16_t perfLen_[kTracks] = {};
    int32_t syncOrigin_ = 0;               // masterStep_ at pattern step 0
    PerfPatternMemory* perfMem_ = nullptr;
    // Override state per track. pendNum_/pendImm_ are the main-loop ->
    // ISR handshake; the rest is written where noted in sequencer.cpp.
    std::atomic<uint8_t> pendNum_[kTracks] = {};
    std::atomic<bool> pendImm_[kTracks] = {};
    std::atomic<uint8_t> pendSwitch_{0};   // switchPatternNow main loop -> ISR
    uint8_t ovActive_[kTracks] = {};
    uint8_t ovSlot_[kTracks] = {};
    uint8_t ovNum_[kTracks] = {};
    uint16_t freshCommit_ = 0;             // tracks committed during this µstep (bit per track)
};

}  // namespace ot
