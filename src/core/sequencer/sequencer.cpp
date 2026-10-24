// Sequencer engine (spec 03). See sequencer.h.
#include "sequencer/sequencer.h"

namespace ot {

using namespace seq;

namespace {

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline bool isMidiInstr(uint8_t i) { return i >= 48 && i < 64; }
inline bool isMidiLike(uint8_t i) { return i >= 48; }          // 48..63 MIDI, 64..66 synths
inline bool isSynth(uint8_t i) { return isSynthInstrument(i); }  // 64..66 = Synth 1..3 (MINI_PLAN §3)
// Release the track's note before a new note (MIDI tracks are monophonic).
// A synth note following a synth note is left to the synth bank, which
// releases the old voice or reuses it (polyphony-1 legato / glide).
inline bool releaseBefore(uint8_t last, uint8_t next) {
    return (isMidiLike(last) || isMidiLike(next)) && !(isSynth(last) && isSynth(next));
}
inline bool isNoteOffSentinel(int8_t n) { return n == kNoteFade || n == kNoteCut || n == kNoteOff; }

// Engine slot for a column: FX-B (model fx[0], column 0) -> slot 1,
// FX-A (model fx[1], column 1) -> slot 0 (spec 03 §3.1).
inline uint8_t slotOfColumn(int c) { return c == 0 ? 1 : 0; }

// FX that the sequencer consumes at step start; on an empty step everything
// else is forwarded to the sounding voice with applyFx (spec 03 §3.3 step 9).
bool consumedBySequencer(uint8_t type) {
    switch (type) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6:
        case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 17:
        case 20: case 21: case 37:
            return true;
        default:
            return false;
    }
}

int ccLetter(uint8_t type) {
    if (type >= 8 && type <= 12) return type - 8;
    if (type == 37) return 5;
    return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// wiring / state
// ---------------------------------------------------------------------------

void Sequencer::resetState() {
    for (auto& t : tracks_) t = TrackState{};
    playing_ = recording_ = armed_ = false;
    songMode_ = perfMode_ = selection_ = false;
    tick_ = 1;
    masterMicro_ = 1;
    masterStep_ = 0;
    accUs_ = 0;
    tempoOverride_ = stopAtStepEnd_ = false;
    swingOn_ = swingPhase_ = false;
    swing_ = 50.0f;
    extBpm_ = kTempoDefault;  // boot default 130 (spec 03 §4.2)
    for (auto& p : perfLen_) p = kDefaultTrackLastStep;
}

void Sequencer::attach(ISequencerSink* sink, ISequencerHost* host, Project* project, Pattern* bufA,
                       Pattern* bufB, Pattern* stage) {
    sink_ = sink;
    host_ = host ? host : &defaultHost_;
    project_ = project;
    if (bufB != next_) dropPreload();   // a spread preload went into the old buffer
    cur_ = bufA;
    next_ = bufB;
    Pattern* st = stage ? stage : stage_.load(std::memory_order_relaxed);
    if (st == bufA || st == bufB) st = nullptr;
    stage_.store(st, std::memory_order_relaxed);
    if (project_) {
        curPatternNum_ = project_->currentPattern() ? project_->currentPattern() : 1;
        songPos_ = project_->playlistPos < kSongSlots ? project_->playlistPos : 0;
    }
}

void Sequencer::setPerformanceMode(bool on) {
    if (on) {
        if (perfMode_) return;
        perfMode_ = true;
        reloadPerfLengths();
        return;
    }
    if (!perfMode_) return;
    // spec 12 §C.4 leave the Performance screen.
    perfMode_ = false;
    perfGlobalLen_ = -1;
    clearTrackPatterns();
    for (int t = 0; t < kTracks; ++t) {
        TrackState& tr = tracks_[t];
        if (tr.playMode != 0) {
            tr.playMode = 0;
            resyncTrack(t);
        }
        tr.repeatIdx = -1;
        tr.velOffset = 0;
        tr.transposeOn = false;
        tr.transpose = 0;
        tr.inject = StepFx{};
    }
}

void Sequencer::reloadPerfLengths() {
    if (!cur_) return;
    for (int t = 0; t < kTracks; ++t) perfLen_[t] = playTrack(t).lastStep;
}

void Sequencer::setPerfPlayMode(int t, uint8_t mode) {
    if (!validTrack(t) || mode >= kPlayModeCount) return;
    TrackState& tr = tracks_[t];
    const uint8_t old = tr.playMode;
    tr.playMode = mode;
    // spec 12 §D revert: mode 0 and step := master step [DIS:0x44b9c].
    if (mode == 0 && old != 0) resyncTrack(t);
}

void Sequencer::resyncTrack(int t) {
    TrackState& tr = tracks_[t];
    if (!playing_) return;  // positions are reset at the next start
    const int len = lastIndex(t) + 1;
    int c = syncPosition() % len;
    if (c < 0) c += len;
    tr.step = int16_t(c);
    tr.stepsPlayed = int16_t(c);
}

// ---------------------------------------------------------------------------
// per-track pattern override (spec 12 §C)
// ---------------------------------------------------------------------------

bool Sequencer::setTrackPattern(int t, uint8_t number, bool immediate) {
    // Main loop. Handshake: clear the queue first so the ISR cannot flip to
    // the staging slot while it is being written; publish it last.
    if (!perfMem_ || !cur_ || !validTrack(t) || number == 0) return false;
    if (!perfMode_) setPerformanceMode(true);  // spec 12 §C.1
    pendImm_[t].store(false, std::memory_order_relaxed);
    pendNum_[t].store(0, std::memory_order_seq_cst);
    Pattern& src = perfMem_->scratch;
    host_->loadPattern(number, src, true);
    Track& dst = perfMem_->slot[t][ovSlot_[t] ^ 1];
    dst = src.tracks[t];
    dst.lastStep = src.tracks[0].lastStep;  // the pattern length, spec 12 §C.1
    pendImm_[t].store(immediate, std::memory_order_relaxed);
    pendNum_[t].store(number, std::memory_order_release);
    // Stopped: nothing is reading the track, commit now.
    if (immediate && !playing_) commitTrackPattern(t);
    return true;
}

void Sequencer::commitTrackPattern(int t) {
    // Audio ISR (or main loop while stopped). O(1): flips the slot index,
    // no track copy (spec 12 §C.1 "Commit" copies 769 bytes; OT does not).
    const uint8_t num = pendNum_[t].exchange(0, std::memory_order_acquire);
    pendImm_[t].store(false, std::memory_order_relaxed);
    if (num == 0 || !perfMem_) return;
    ovSlot_[t] ^= 1;
    ovActive_[t] = 1;
    ovNum_[t] = num;
    freshCommit_ = uint16_t(freshCommit_ | (1u << t));
    perfLen_[t] = perfMem_->slot[t][ovSlot_[t]].lastStep;
    // Position fix when beyond the new length (spec 12 §C.1).
    TrackState& tr = tracks_[t];
    const int li = lastIndex(t);
    const uint8_t mode = perfTrack(t) ? tr.playMode : 0;
    const bool beyond = (mode <= kPlayBack) ? tr.step > li : tr.stepsPlayed > li;
    if (!beyond) return;
    int c = syncPosition() % (li + 1);
    if (c < 0) c += li + 1;
    tr.stepsPlayed = int16_t(c);
    if (mode == kPlayFwd) tr.step = int16_t(c);
    else if (mode == kPlayBack) tr.step = int16_t(li - c);
}

void Sequencer::discardTrackOverrides() {
    // Song advance / queued pattern change (spec 12 §C.3). Pending queues stay.
    // OT: a queue that committed at this same boundary survives (stock commits
    // into the outgoing buffer and loses it).
    for (int t = 0; t < kTracks; ++t) {
        if (freshCommit_ & (1u << t)) continue;
        ovActive_[t] = 0;
        ovNum_[t] = 0;
    }
}

void Sequencer::clearTrackPatterns() {
    for (int t = 0; t < kTracks; ++t) {
        pendNum_[t].store(0);
        pendImm_[t].store(false);
        ovActive_[t] = 0;
        ovNum_[t] = 0;
    }
    if (perfMode_) reloadPerfLengths();
}

// ---------------------------------------------------------------------------
// tempo (spec 03 §1.2, §1.3)
// ---------------------------------------------------------------------------

static float projectTempo(const Project* p) {
    if (!p) return kTempoDefault;
    float bpm = p->tempo();
    if (!(bpm > 0.0f) || bpm > 1000.0f) bpm = kTempoDefault;  // load-time sanity, spec 03 §1.2
    return bpm;
}

float Sequencer::effectiveTempo() const {
    if (tempoOverride_) return overrideBpm_;
    if (settings_.clockIn != 0 && !rendering_ && !selection_) return extBpm_;
    return projectTempo(project_);
}

float Sequencer::tickPeriodUs() const {
    float bpm = effectiveTempo();
    if (!(bpm > 0.0f)) bpm = projectTempo(project_);  // NO CLK: avoid a zero period
    if (swingOn_) {
        const float s = swingPhase_ ? (100.0f - swing_) : swing_;
        return (s + 50.0f) * 3125.0f / bpm / 12.0f;
    }
    return 312500.0f / bpm / 12.0f;
}

uint32_t Sequencer::countInMs() const {
    // spec 03 §2.1 record start popup formula, saturating at 255.
    const float p = 312500.0f / projectTempo(project_) / 12.0f;
    const float v = p / 1000.0f * 576.0f * float(settings_.tsNum + 1) * float(settings_.tsDen + 1) / 1000.0f;
    return v > 255.0f ? 255u : uint32_t(v);
}

// ---------------------------------------------------------------------------
// time base (spec 03 §1.1, §4.2)
// ---------------------------------------------------------------------------

void Sequencer::processBlock(uint32_t frames) {
    if (suspended_) return;
    processElapsed(float(frames) / kRealtimeRate);
}

void Sequencer::processElapsed(float seconds) {
    if (suspended_) return;
    advance(seconds);
    // Engine block update with a 4-block moving average of the tempo (spec 03 §6).
    const float bpm = effectiveTempo();
    if (!tempoHistInit_) {
        for (auto& h : tempoHist_) h = bpm;
        tempoHistInit_ = true;
    }
    tempoHist_[tempoHistPos_] = bpm;
    tempoHistPos_ = uint8_t((tempoHistPos_ + 1) & 3);
    const float avg = (tempoHist_[0] + tempoHist_[1] + tempoHist_[2] + tempoHist_[3]) * 0.25f;
    if (sink_) sink_->engineBlockUpdate(avg);
}

void Sequencer::advance(float seconds) {
    if (pendSwitch_.load(std::memory_order_relaxed)) commitSwitch();
    blockUs_ = seconds * 1e6f;
    if (partNext_ < kTracks) preloadPart(preloadTracksPerBlock());
    updateExternalTempo(seconds);
    const bool internal = settings_.clockIn == 0 || rendering_ || selection_;
    if (internal) {
        accUs_ += seconds * 1e6f;
        const float p = tickPeriodUs();  // computed once per block
        while (accUs_ >= p) {
            tick();
            accUs_ -= p;
        }
    } else {
        const int32_t n = pendingCorrection_ + pendingPulses_ + leftover_;
        if (n > 0) {
            for (int32_t i = 0; i < n * kTicksPerExtClock; ++i) tick();
            leftover_ = 0;
        } else {
            leftover_ = n;
        }
        pendingCorrection_ = 0;
        pendingPulses_ = 0;
    }
}

void Sequencer::updateExternalTempo(float seconds) {
    // spec 03 §4.2 "Tempo measurement".
    intervalAcc_ += seconds;
    sinceClockS_ += seconds;
    if (pulsesThisBlock_ > 0) {
        intervals_[intervalPos_] = intervalAcc_;
        intervalPos_ = uint8_t((intervalPos_ + 1) % 48);
        if (intervalCount_ < 48) ++intervalCount_;
        intervalAcc_ = 0;
        if (intervalCount_ == 48) {
            float sum = 0;
            for (float v : intervals_) sum += v;
            if (sum > 0) {
                float bpm = float(int32_t(120.0f / sum + 0.5f));
                extBpm_ = clampf(bpm, 10.0f, 999.0f);
            }
        }
    }
    pulsesThisBlock_ = 0;
    if (sinceClockS_ > 1.0f) {
        extBpm_ = 0.0f;  // "NO CLK"
        intervalCount_ = 0;
        intervalPos_ = 0;
        sinceClockS_ = 1.0f + 1e-3f;  // keep bounded
    }
}

void Sequencer::tick() {
    if (countInSteps_ > 0 && !playing_) {
        // Count-in in 576-tick units (spec 03 §2.5).
        if (countInTicks_ % kTicksPerStep == 0) {
            const int32_t c = int32_t(countInTicks_ / kTicksPerStep);
            if (c >= countInSteps_) {
                countInSteps_ = 0;
                startPlayback(0, false, true);
                return;
            }
            const int bar = (settings_.tsNum + 1) * (settings_.tsDen + 1);
            if (sink_) {
                if (c % bar == 0) sink_->metronomeClick(true, 100);
                else if (c % (settings_.tsDen + 1) == 0) sink_->metronomeClick(false, 45);
            }
        }
        ++countInTicks_;
        return;
    }
    if (!playing_) return;
    ++tickCount_;
    const uint32_t old = tick_++;
    if (old % kTicksPerMicro == 0) microStep();  // spec 03 §1.4
}

void Sequencer::microStep() {
    // spec 03 §1.4 [LG:0xcb2e0].
    freshCommit_ = 0;
    if (++masterMicro_ > kMicrosPerStep) {
        masterMicro_ = 1;
        ++masterStep_;
        host_->onStepChanged();
        // Stock [LG:0xcb2e0] compares against sel.end itself, which replays
        // rows when the selection does not start at step 0 (spec 03 §2.1);
        // OpenTracker stops after exactly the selected rows (README).
        if (selection_ && !selLoop_ && masterStep_ >= (selEnd_ - selStart_ + 1)) {
            stop();
            return;
        }
    }
    bool stepEnd = false;
    for (int t = 0; t < kTracks; ++t) {
        processTrack(t);
        if (!playing_) return;
        TrackState& tr = tracks_[t];
        if (++tr.micro > kMicrosPerStep) {
            tr.micro = 1;
            advanceStep(t);
            if (!playing_) return;
            stepEnd = true;
        }
    }
    if (stepEnd) advanceMaster();
}

// ---------------------------------------------------------------------------
// step processing (spec 03 §3)
// ---------------------------------------------------------------------------

int Sequencer::lastIndex(int t) const {
    if (selection_) return clampi(selEnd_, 0, kStepCount - 1);
    int li = cur_ ? cur_->tracks[0].lastStep : kDefaultTrackLastStep;
    if (perfMode_) {
        if (perfGlobalLen_ >= 0) li = perfGlobalLen_;
        else li = perfLen_[t];
    }
    return clampi(li, 0, kStepCount - 1);
}

int Sequencer::baseLastIndex() const {
    // The length every track had before 2.x gave tracks 8..11 performance
    // lengths: what stock's pattern-end track (11) wrapped at.
    if (selection_) return clampi(selEnd_, 0, kStepCount - 1);
    int li = cur_ ? cur_->tracks[0].lastStep : kDefaultTrackLastStep;
    if (perfMode_ && perfGlobalLen_ >= 0) li = perfGlobalLen_;
    return clampi(li, 0, kStepCount - 1);
}

bool Sequencer::isMuted(int t) const {
    // spec 03 §2.4: project mute flags; RAM 0x200001cc+0x46e0 / +0x4cf6 are
    // values-block file offsets 0x12C (tracks 0..7) and 0x742 (tracks 8..11).
    // 2.x tracks 12..15: 0x742[4..7] as the Tracker Mini (MINI_PLAN U13).
    return project_ && project_->trackMuted(t);
}

const MidiInstrument Sequencer::midiInstr(uint8_t instr) const {
    if (project_ && isMidiInstr(instr)) return project_->midiInstrument(instr - 48);
    return MidiInstrument{};
}

void Sequencer::processTrack(int t) {
    TrackState& tr = tracks_[t];
    // spec 12 §C.2 immediate switch: the track's next step reads the new data.
    if (tr.micro == 1 && pendImm_[t].load(std::memory_order_relaxed)) commitTrackPattern(t);
    // spec 03 §3.2 (1) trigger timer.
    if (tr.sinceTrigger < 0xFFFF) ++tr.sinceTrigger;
    const bool muted = isMuted(t);
    // (2) gate; MIDI notes on muted tracks are released.
    if (tr.sounding) {
        ++tr.gate;
        if (tr.gateLimit != -1 && tr.gate >= tr.gateLimit) noteOffTrack(t, NoteOffMode::Off);
    }
    if (muted && (tr.midiCount || (tr.sounding && isSynth(tr.last.instr)))) noteOffTrack(t, NoteOffMode::Off);
    // (3) track 0 duties.
    if (t == 0 && playing_) {
        if ((tr.micro - 1) % kMicrosPerClock == 0 && !(selection_ && !selLoop_))
            sendRealtime(settings_.clockOut, 0xF8);
        if (tr.micro == 1) {
            // One step before the pattern end (masterPos_, not track 0's own
            // performance length or play mode).
            if (masterPos_ == baseLastIndex()) startPreload();
            if (recording_ && settings_.metronome && sink_) {
                const int bar = (settings_.tsNum + 1) * (settings_.tsDen + 1);
                if (tr.step % bar == 0) sink_->metronomeClick(true, 100);
                else if (tr.step % (settings_.tsDen + 1) == 0) sink_->metronomeClick(false, 45);
            }
        }
    }
    // (4) micro-delay and arp counters.
    if (tr.delayActive && tr.delayCounter < 0xFF) ++tr.delayCounter;
    if (tr.arpActive) ++tr.arpCounter;
    // (5) disabled or muted: counters only.
    if (!tr.enabled || muted) return;

    // Delayed note (spec 03 §3.4). Checked before the step start so a delay
    // that reaches into the next step fires before that step (spec 03 §7.4: OT choice).
    if (tr.delayActive && tr.delayCounter >= tr.delay) {
        tr.delayActive = false;
        handleNote(t, tr.delayed);
    }
    if (tr.micro == 1) stepStart(t);

    // spec 03 §3.4 continuous processing.
    if (tr.arpActive && tr.arpCounter >= tr.arpPeriod) {
        tr.arpCounter = 0;
        arpNext(t);
    }
    if (tr.rollActive && tr.rollRate && tr.sinceTrigger != 1 && tr.sinceTrigger % tr.rollRate == 1) {
        rollRetrigger(t);
    } else if (perfMode_ && tr.repeatIdx >= 0 && tr.repeatIdx < 15 && tr.last.note >= 0 &&
               tr.sinceTrigger != 1 && tr.sinceTrigger % kRepeatPeriod[tr.repeatIdx] == 1) {
        // Performance step repeater (spec 03 §2.6).
        NoteRec rec = tr.last;
        if (isMidiLike(rec.instr)) noteOffTrack(t, NoteOffMode::Off);
        triggerNote(t, rec, false, 0);
    }
}

void Sequencer::stepStart(int t) {
    TrackState& tr = tracks_[t];
    if (!cur_) return;
    const Step& s = playTrack(t).steps[clampi(tr.step, 0, kStepCount - 1)];
    // spec 03 §3.3 (1) work record.
    Pending p;
    p.rec.note = s.note;
    p.rec.instr = s.instrument;
    p.rec.fxA = s.fx[1];
    p.rec.fxB = s.fx[0];
    p.rec.volume = -1;
    tr.rollThisStep = false;
    if (tr.skipStep == tr.step) {  // note was just played live by recording (§5.2)
        if (p.rec.note >= 0) p.rec.note = kNoteEmpty;
        tr.skipStep = -1;
    }
    StepFx* col[2] = {&p.rec.fxB, &p.rec.fxA};  // evaluation order FX-B, FX-A

    // (2) performance FX injection into the first empty column.
    if (tr.inject.type != 0) {
        for (int c = 0; c < 2; ++c) {
            if (col[c]->type == 0) { *col[c] = tr.inject; break; }
        }
        // Table modes set or clear the injection every step (spec 12 §D).
        if (!(perfTrack(t) && isTableMode(tr.playMode))) tr.inject = StepFx{};
    }

    // (3) Random FX value pre-pass (id 16).
    for (int c = 0; c < 2; ++c) {
        if (col[c]->type != 16) continue;
        StepFx* o = col[1 - c];
        if (o->type == 0 || o->type == 16) break;
        const int r = col[c]->value;
        const int x = o->value;
        int lo = fxMin(o->type), hi = fxMax(o->type);
        if (o->type == 15) lo = 5;  // never choose STOP
        const int v = clampi(rng_.rangeIncl(x - r, x + r), lo, hi);
        o->value = uint8_t(v);
        col[c]->value = uint8_t(v);
        break;
    }

    // (4) MIDI-instrument delay byte (spec 03 §3.3 step 4, §7 item 3).
    bool hasNote = p.rec.note != kNoteEmpty && (p.rec.note >= 0 || isNoteOffSentinel(p.rec.note));
    int midiDelayBase = -1;
    if (hasNote && isMidiInstr(p.rec.instr)) {
        const uint8_t b = midiInstr(p.rec.instr).extra[0];
        if (b) midiDelayBase = (int(b) + 1) * 45 / 100 + 1;  // 13 §I.1
    }

    // (5) Chance pre-pass (id 4).
    bool failed = false;
    for (int c = 0; c < 2; ++c) {
        if (col[c]->type != 4) continue;
        const int r = rng_.range(0, 100);
        if (int(col[c]->value) < r) {
            failed = true;
            if (tr.rollActive && tr.rollCol == c) clearRoll(tr);
            if (tr.arpActive && tr.arpCol == c) clearArp(tr);
        }
    }
    if (failed) return;

    // (6) FX pass.
    bool hasMicro = false;
    int microVal = 0;
    for (int c = 0; c < 2; ++c) {
        const uint8_t type = col[c]->type;
        const uint8_t v = col[c]->value;
        switch (type) {
            case 1:  // Off
                if (tr.rollActive && tr.rollCol == c) clearRoll(tr);
                if (tr.arpActive && tr.arpCol == c) clearArp(tr);
                tr.delayActive = false;
                if (t < kVoiceTracks && sink_) sink_->voiceApplyFx(uint8_t(t), slotOfColumn(c), 0, 0);
                break;
            case 2:  // Micro-move
                hasMicro = true;
                microVal = v > 100 ? 100 : v;
                break;
            case 3: {  // Roll
                const int rv = v > 53 ? 53 : v;
                const int rateIdx = rv % 9;
                if (rateIdx == 0) {
                    clearRoll(tr);
                } else {
                    tr.rollActive = true;
                    tr.rollThisStep = true;
                    tr.rollType = uint8_t(rv / 9);
                    tr.rollRate = kRollPeriod[rateIdx - 1];
                    tr.rollCol = uint8_t(c);
                    tr.rollDistance = uint8_t(distanceToNextNote(t, tr.step));
                    tr.sinceTrigger = 1;  // counting starts at this step
                    if (p.rec.note < 0) {
                        tr.rollBaseVol = tr.last.volume >= 0 ? tr.last.volume
                                                             : host_->instrumentVolume(tr.last.instr);
                    }
                }
                break;
            }
            case 5:  // Random note
                if (p.rec.note >= 0) {
                    const int n = p.rec.note;
                    int cand = n;
                    for (int i = 0; i < 100; ++i) {
                        cand = clampi(rng_.rangeIncl(n - v, n + v), 0, 127);
                        if (host_->noteInScale(uint8_t(cand))) break;
                    }
                    p.rec.note = int8_t(cand);
                }
                break;
            case 6: {  // Random instrument, clamped inside its class
                const int i = p.rec.instr;
                int lo = 0, hi = 47;
                if (i >= 64) { lo = 64; hi = 66; }
                else if (i >= 48) { lo = 48; hi = 63; }
                p.rec.instr = uint8_t(clampi(rng_.rangeIncl(i - v, i + v), lo, hi));
                break;
            }
            case 7:  // Random volume: rand 0..v (with a note)
                if (p.rec.note >= 0) p.rec.volume = int8_t(rng_.rangeIncl(0, v > 100 ? 100 : v));
                break;
            case 8: case 9: case 10: case 11: case 12: case 37:
                if (isMidiInstr(p.rec.instr)) {
                    sendCcFx(t, uint8_t(ccLetter(type)), v, p.rec);
                } else if (sink_) {
                    // Synth step: macro 1..6 of the slot (Mini manual 7.38). An
                    // FX-only step uses the synth the track plays.
                    uint8_t in = p.rec.instr;
                    if (!isSynth(in) && p.rec.note < 0 && isSynth(tr.last.instr)) in = tr.last.instr;
                    if (isSynth(in)) sink_->synthMacroFx(uint8_t(synthSlotOf(in)), uint8_t(ccLetter(type)), v > 127 ? 127 : v);
                }
                break;
            case 13:  // Break
                breakPending_ = true;
                break;
            case 14:  // Chord
                p.chord = true;
                p.chordValue = uint8_t(v >= kChordCount ? kChordCount - 1 : v);
                break;
            case 15:  // Tempo
                if (v > 4) {
                    overrideBpm_ = clampf(2.0f * float(v), kTempoFxMin, kTempoFxMax);
                    tempoOverride_ = true;
                } else if (v == 4) {
                    stopAtStepEnd_ = true;
                }
                break;
            case 17:  // Swing
                swing_ = float(clampi(v, 25, 75));
                swingOn_ = true;
                break;
            case 18:  // Volume
                p.rec.volume = int8_t(v > 100 ? 100 : v);
                break;
            case 20:  // Gate
                p.gateLimit = int16_t((v > 100 ? 100 : v) * 47 / 100);
                break;
            case 21: {  // Arp (needs Chord in the other column)
                const int av = v > 33 ? 33 : v;
                const StepFx* o = col[1 - c];
                if (av >= 1 && o->type == 14) {
                    p.arpSetup = true;
                    p.arpDir = uint8_t((av - 1) / 11);
                    p.arpPeriod = kArpPeriod[(av - 1) % 11];
                    // spec 16 §A.9: Chord 0 (or > 29) arpeggiates chromatically.
                    p.arpMask = (o->value == 0 || o->value > 29) ? 0xFFFF : chordMask(o->value);
                    p.arpCol = uint8_t(c);
                }
                break;
            }
            case 34:  // Pitch bend for MIDI instruments at step start
                if (isMidiLike(p.rec.instr)) sendPitchBendFx(p.rec.instr, v);
                break;
            default:
                break;
        }
    }

    // (7) Break in song mode: preload the next song pattern now.
    if (breakPending_ && songMode_ && !breakPreloadDone_) {
        breakPreloadDone_ = true;
        startPreload();
    }

    // (8) Micro-move / MIDI delay.
    if (hasNote && (hasMicro || midiDelayBase >= 0)) {
        // spec 03 §1.5 / 13 §I.1: 1 + floor((m+1)·47/100) + (floor((v+1)·45/100) + 1).
        int d = (hasMicro ? 1 + (microVal + 1) * 47 / 100 : 0) + (midiDelayBase > 0 ? midiDelayBase : 0);
        if (d > 96) d = 96;
        tr.delayed = p;
        tr.delay = uint8_t(d);
        tr.delayCounter = 0;
        tr.delayActive = true;
        return;
    }
    // (9) note handling.
    handleNote(t, p);
}

void Sequencer::handleNote(int t, Pending& p) {
    TrackState& tr = tracks_[t];
    NoteRec rec = p.rec;
    if (rec.note >= 0) {
        // MIDI tracks are monophonic; sample voices are retriggered by the engine.
        if ((tr.sounding || tr.midiCount) && releaseBefore(tr.last.instr, rec.instr))
            noteOffTrack(t, NoteOffMode::Off);
        if (!tr.rollThisStep) clearRoll(tr);
        clearArp(tr);
        if (tr.rollActive) {
            tr.rollBaseVol = rec.volume >= 0 ? rec.volume : host_->instrumentVolume(rec.instr);
            tr.sinceTrigger = 1;
            if (tr.rollType == 1 || tr.rollType == 2) rec.volume = int8_t(rollVolume(tr));
        }
        triggerNote(t, rec, p.chord && !p.arpSetup, p.chordValue);
        tr.gateLimit = p.gateLimit;
        tr.gate = 0;
        tr.sinceTrigger = 1;
        tr.last = rec;
        if (p.arpSetup) {
            tr.arpActive = true;
            tr.arpDir = p.arpDir;
            tr.arpPeriod = p.arpPeriod;
            tr.arpMask = p.arpMask;
            tr.arpCol = p.arpCol;
            tr.arpBase = uint8_t(rec.note);
            tr.arpPos = 0;
            tr.arpCounter = 0;
        }
    } else if (isNoteOffSentinel(rec.note)) {
        noteOffTrack(t, NoteOffMode(rec.note));
        clearRoll(tr);
        clearArp(tr);
    } else {
        // Empty step: unconsumed FX modulate the sounding voice (tracks 0..7),
        // or the track's sounding synth note (any track).
        if (sink_ && tr.sounding && isSynth(tr.last.instr)) {
            const StepFx* col[2] = {&rec.fxB, &rec.fxA};
            for (int c = 0; c < 2; ++c) {
                const uint8_t type = col[c]->type;
                if (consumedBySequencer(type)) continue;
                sink_->synthApplyFx(uint8_t(t), type, col[c]->value);
            }
        } else if (t < kVoiceTracks && sink_) {
            const StepFx* col[2] = {&rec.fxB, &rec.fxA};
            for (int c = 0; c < 2; ++c) {
                const uint8_t type = col[c]->type;
                if (consumedBySequencer(type)) continue;
                if (type == 34 && isMidiLike(rec.instr)) continue;  // sent as pitch bend
                sink_->voiceApplyFx(uint8_t(t), slotOfColumn(c), type, col[c]->value);
            }
        }
    }
}

void Sequencer::triggerNote(int t, const NoteRec& rec, bool chord, uint8_t chordValue) {
    TrackState& tr = tracks_[t];
    tr.gate = 0;
    if (rec.note < 0) return;
    if (rec.instr < 48) {
        if (t >= kVoiceTracks) return;  // sample note-ons on tracks 9..12 are dropped
        if (sink_) {
            VoiceNoteOn n;
            n.instrument = rec.instr;
            n.note = uint8_t(rec.note);
            n.volume = rec.volume;
            n.fxA = rec.fxA;
            n.fxB = rec.fxB;
            n.tickPeriodUs = tickPeriodUs();
            sink_->voiceNoteOn(uint8_t(t), n);
        }
        tr.sounding = true;
        tr.midiCount = 0;
        return;
    }
    if (isSynth(rec.instr)) {  // Synth 1..3 on any track (MINI_PLAN §3)
        if (sink_) {
            SynthNoteOn n;
            n.slot = uint8_t(synthSlotOf(rec.instr));
            int note = rec.note;
            if (perfMode_ && tr.transposeOn) note = clampi(note + tr.transpose, 0, 127);
            n.note = uint8_t(note);
            int vol = rec.volume;
            if (perfMode_ && tr.velOffset) vol = clampi((vol < 0 ? 100 : vol) + tr.velOffset, 0, 100);
            n.volume = int8_t(vol);
            n.fxA = rec.fxA;
            n.fxB = rec.fxB;
            n.tickPeriodUs = tickPeriodUs();
            sink_->synthNoteOn(uint8_t(t), n);
        }
        tr.sounding = true;
        tr.midiCount = 0;
        return;
    }
    if (rec.instr >= 64) {  // 67..: no output (spec 03 §4.4)
        tr.sounding = false;
        return;
    }
    // MIDI instrument (spec 03 §4.4).
    const uint8_t ch = uint8_t(rec.instr - 47);
    int note = rec.note;
    if (perfMode_ && tr.transposeOn) note = clampi(note + tr.transpose, 0, 127);
    const int sent = midiNoteNumber(note);
    int vel;
    if (rec.volume >= 0) {
        vel = (rec.volume > 100 ? 100 : rec.volume) * 127 / 100;
    } else {
        const uint8_t dv = midiInstr(rec.instr).velocity;
        vel = (dv & 0x80) ? 127 : dv;
    }
    if (perfMode_) vel += tr.velOffset * 127 / 100;
    vel = clampi(vel, 0, 127);
    tr.midiCh = ch;
    tr.midiCount = 0;
    const uint8_t status = uint8_t(0x90 | (ch - 1));
    if (chord && chordValue > 0 && chordValue < kChordCount) {
        for (int i = 0; i < kChordSize[chordValue]; ++i) {
            const int n = sent + kChordIntervals[chordValue][i];
            if (n > 127) continue;
            sendMidi(status, uint8_t(n), uint8_t(vel), 3);
            tr.midiNotes[tr.midiCount++] = uint8_t(n);
        }
    } else {
        sendMidi(status, uint8_t(sent), uint8_t(vel), 3);
        tr.midiNotes[tr.midiCount++] = uint8_t(sent);
    }
    tr.sounding = true;
}

void Sequencer::noteOffTrack(int t, NoteOffMode mode) {
    TrackState& tr = tracks_[t];
    if (tr.midiCount) {
        const uint8_t status = uint8_t(0x80 | (tr.midiCh - 1));
        for (int i = 0; i < tr.midiCount; ++i) sendMidi(status, tr.midiNotes[i], 0, 3);
        tr.midiCount = 0;
    } else if (t < kVoiceTracks && tr.last.instr < 48 && tr.last.note >= 0 && sink_) {
        sink_->voiceNoteOff(uint8_t(t), mode);
    } else if (isSynth(tr.last.instr) && sink_) {
        sink_->synthNoteOff(uint8_t(t), mode);
    }
    tr.sounding = false;
}

void Sequencer::releaseAllNotes() {
    for (int t = 0; t < kTracks; ++t) {
        if (tracks_[t].sounding || tracks_[t].midiCount) noteOffTrack(t, NoteOffMode::Off);
    }
}

int Sequencer::rollVolume(const TrackState& tr) const {
    // spec 03 §3.5 volume ramp.
    const float f = clampf(float(tr.sinceTrigger) / (48.0f * float(tr.rollDistance)), 0.0f, 1.0f);
    const float base = float(tr.rollBaseVol);
    if (tr.rollType == 1) return int(base * (1.0f - f));
    if (tr.rollType == 2) return int(base * f);
    return tr.rollBaseVol;
}

int Sequencer::distanceToNextNote(int t, int fromStep) const {
    const int len = lastIndex(t) + 1;
    if (!cur_) return len;
    for (int k = 1; k <= len; ++k) {
        const int s = (fromStep + k) % len;
        const int8_t n = playTrack(t).steps[s].note;
        if (n >= 0 || isNoteOffSentinel(n)) return k;
    }
    return len < 1 ? 1 : len;
}

void Sequencer::rollRetrigger(int t) {
    TrackState& tr = tracks_[t];
    NoteRec rec = tr.last;
    if (rec.note < 0) return;
    // Cumulative note variation (spec 03 §3.4).
    switch (tr.rollType) {
        case 3: rec.note = int8_t(clampi(rec.note - 1, 0, 127)); break;
        case 4: rec.note = int8_t(clampi(rec.note + 1, 0, 127)); break;
        case 5: rec.note = int8_t(clampi(rng_.rangeIncl(rec.note - 12, rec.note + 12), 0, 127)); break;
        default: break;
    }
    if (tr.rollType == 1 || tr.rollType == 2) rec.volume = int8_t(rollVolume(tr));
    if (isMidiLike(rec.instr)) noteOffTrack(t, NoteOffMode::Off);
    triggerNote(t, rec, false, 0);
    tr.last = rec;
    tr.gateLimit = int16_t(tr.rollRate / 2);
    tr.gate = 0;
}

void Sequencer::arpNext(int t) {
    TrackState& tr = tracks_[t];
    if (!tr.arpMask) return;
    int np = tr.arpPos;
    if (tr.arpDir == 0) {
        for (int k = 1; k <= 12; ++k) {
            const int q = (tr.arpPos + k) % 12;
            if (tr.arpMask & (1u << q)) { np = q; break; }
        }
    } else if (tr.arpDir == 1) {
        for (int k = 1; k <= 12; ++k) {
            const int q = (tr.arpPos + 12 - k) % 12;
            if (tr.arpMask & (1u << q)) { np = q; break; }
        }
    } else {
        const int r = rng_.range(0, 12);
        for (int k = 0; k < 12; ++k) {
            const int q = (r + k) % 12;
            if (tr.arpMask & (1u << q)) { np = q; break; }
        }
    }
    tr.arpPos = uint8_t(np);
    NoteRec rec = tr.last;
    rec.note = int8_t(clampi(tr.arpBase + np, 0, 127));
    noteOffTrack(t, NoteOffMode::Off);
    triggerNote(t, rec, false, 0);
    tr.last = rec;
}

// ---------------------------------------------------------------------------
// step advance, pattern end, song mode (spec 03 §1.4, §2.2)
// ---------------------------------------------------------------------------

void Sequencer::advanceStep(int t) {
    // spec 03 §1.4, spec 12 §C.2/§D [FW:0x11db4].
    TrackState& tr = tracks_[t];
    int li = lastIndex(t);
    const int ls = loopStart();
    const bool perf = perfTrack(t);
    const uint8_t mode = perf ? tr.playMode : kPlayFwd;
    ++tr.stepsPlayed;
    bool wrapped;
    if (mode == kPlayBack) wrapped = tr.step - 1 < ls || breakPending_;
    else if (mode == kPlayFwd) wrapped = tr.step + 1 > li || breakPending_;
    else wrapped = tr.stepsPlayed > li || breakPending_;  // Rnd and tables count steps
    if (wrapped) {
        tr.stepsPlayed = 0;
        // Queued per-track pattern commits at this track's own loop end.
        if (perf && pendNum_[t].load(std::memory_order_relaxed)) {
            commitTrackPattern(t);
            li = lastIndex(t);
        }
    }
    if (mode == kPlayBack) {
        // OT: wraps to the effective last step (stock uses the track's own
        // length byte and ignores the pattern-length FX).
        tr.step = int16_t(wrapped ? li : tr.step - 1);
    } else if (mode == kPlayRnd) {
        tr.step = int16_t(rng_.rangeIncl(0, li));
    } else if (isTableMode(mode)) {
        uint8_t rollSel = 0;
        tr.step = int16_t(tableStep(mode, tr.stepsPlayed, li, rollSel));
        applyRollSelector(tr.inject, rollSel, rng_);
    } else {
        tr.step = int16_t(wrapped ? ls : tr.step + 1);
    }
}

void Sequencer::advanceMaster() {
    // Every track finished its step. Stock tests "track 11 wraps" here
    // (spec 03 §2.2); 2.x uses a master position that wraps at the base
    // pattern length, so a per-track length or play mode (now on every
    // track) never moves the song advance (docs/M1_PLAN.md §3).
    const int li = baseLastIndex();
    const bool wrapped = masterPos_ + 1 > li || breakPending_;
    masterPos_ = int16_t(wrapped ? loopStart() : masterPos_ + 1);
    swingPhase_ = !swingPhase_;  // spec 03 §1.3
    if (stopAtStepEnd_) {        // Tempo FX "STP" (spec 03 §2.2)
        stopAtStepEnd_ = false;
        tempoOverride_ = false;
        stop();
        return;
    }
    if (wrapped) {
        breakPending_ = false;
        breakPreloadDone_ = false;
        patternEnd();
    }
}

void Sequencer::startPreload() {
    // spec 03 §2.2 "One step before the end".
    if (!next_) return;
    if (queueActive_ && queued_ != 0) {
        beginPreload(uint8_t(queued_));
        queueReady_ = true;
    } else if (songMode_ && project_) {
        int np = songPos_ + 1;
        if (np >= kSongSlots || project_->playlist[np] == 0) np = 0;
        uint8_t num = project_->playlist[np];
        if (num == 0) num = curPatternNum_;
        beginPreload(num);
    }
}

void Sequencer::beginPreload(uint8_t num) {
    nextPatternNum_ = num;
    preloadIssued_ = true;
    if (host_->loadsPatternTracks()) {
        partNext_ = 0;   // copied by the next blocks (advance), finished at the pattern end
        return;
    }
    partNext_ = kTracks;
    host_->loadPattern(num, *next_, false);
}

void Sequencer::preloadPart(int maxTracks) {
    // Audio context: the next tracks of a spread preload into next_.
    if (!preloadIssued_ || !next_ || partNext_ >= kTracks) {
        partNext_ = kTracks;
        return;
    }
    const int n = maxTracks < kTracks - partNext_ ? maxTracks : kTracks - partNext_;
    host_->loadPatternTracks(nextPatternNum_, *next_, partNext_, n);
    partNext_ = uint8_t(partNext_ + n);
}

int Sequencer::preloadTracksPerBlock() const {
    // Spread the 16 tracks over 3/4 of the preload step (a swung step can be
    // 25 % shorter): 1 track per block up to ~210 BPM, 2 up to ~430 BPM.
    const float stepUs = tickPeriodUs() * float(kTicksPerStep);
    const float blocks = blockUs_ > 0.0f ? 0.75f * stepUs / blockUs_ : 1.0f;
    if (!(blocks >= 1.0f)) return kTracks;
    const int n = int(float(kTracks) / blocks) + 1;
    return n > kTracks ? kTracks : n;
}

void Sequencer::swapBuffers() {
    // [FW:0x11a34]: swap, clear Tempo-FX override and per-track roll/arp.
    Pattern* tmp = cur_;
    cur_ = next_;
    next_ = tmp;
    afterSwap();
}

void Sequencer::afterSwap() {
    tempoOverride_ = false;
    for (auto& tr : tracks_) {
        clearRoll(tr);
        clearArp(tr);
    }
    discardTrackOverrides();  // spec 12 §C.3
    if (perfMode_) reloadPerfLengths();
}

void Sequencer::resetPositions(int startStep) {
    startStep = clampi(startStep, 0, kStepCount - 1);
    syncOrigin_ = masterStep_ - startStep;
    masterPos_ = int16_t(startStep);
    for (int t = 0; t < kTracks; ++t) {
        TrackState& tr = tracks_[t];
        tr.micro = 1;
        tr.stepsPlayed = 0;
        const uint8_t mode = perfTrack(t) ? tr.playMode : kPlayFwd;
        if (mode == kPlayBack) {
            tr.step = int16_t(lastIndex(t));
        } else if (isTableMode(mode)) {
            uint8_t rollSel = 0;
            tr.step = int16_t(tableStep(mode, 0, lastIndex(t), rollSel));
        } else {
            tr.step = int16_t(startStep);
        }
    }
}

void Sequencer::patternEnd() {
    if (queueActive_ && queueReady_) {
        songMode_ = false;
        if (!preloadIssued_ || nextPatternNum_ != queued_) host_->loadPattern(uint8_t(queued_), *next_, true);
        else preloadPart(kTracks);   // the tracks a spread preload has not copied yet
        partNext_ = kTracks;
        swapBuffers();
        curPatternNum_ = uint8_t(queued_);
        if (project_) project_->setCurrentPattern(curPatternNum_);
        queueActive_ = queueReady_ = false;
        queued_ = 0;
        dropPreload();
        resetPositions(0);
        host_->onPatternEnd();
        host_->onPatternAdvanced();
        return;
    }
    if (songMode_ && project_) {
        bool songWrapped = false;
        int pos = songPos_ + 1;
        if (pos >= kSongSlots || project_->playlist[pos] == 0) {
            pos = 0;  // the song loops
            songWrapped = true;
        }
        uint8_t num = project_->playlist[pos];
        if (num == 0) num = curPatternNum_;
        if (!preloadIssued_ || nextPatternNum_ != num) host_->loadPattern(num, *next_, true);
        else preloadPart(kTracks);   // the tracks a spread preload has not copied yet
        dropPreload();
        swapBuffers();
        songPos_ = pos;
        project_->playlistPos = uint8_t(pos);
        curPatternNum_ = num;
        project_->setCurrentPattern(num);
        resetPositions(0);
        host_->onPatternEnd();
        if (!songWrapped) host_->onPatternAdvanced();
        return;
    }
    dropPreload();
    host_->onPatternEnd();
    host_->onPatternAdvanced();
}

// ---------------------------------------------------------------------------
// transport (spec 03 §2.1, §2.3)
// ---------------------------------------------------------------------------

void Sequencer::startPlayback(int startStep, bool keepSongMode, bool afterCountIn) {
    if (!keepSongMode) songMode_ = false;
    if (!(afterCountIn && recording_ && settings_.preRoll == 1)) releaseAllNotes();
    if (sink_) sink_->resetAllVoiceFx();
    selection_ = false;
    masterStep_ = 0;
    // Pending immediate per-track switches take effect from the first step.
    for (int t = 0; t < kTracks; ++t)
        if (pendImm_[t].load(std::memory_order_relaxed)) commitTrackPattern(t);
    resetPositions(startStep);
    // spec 12 §D: table-mode tracks are evaluated with k = 0 at playback start.
    for (int t = 0; t < kTracks; ++t) {
        TrackState& tr = tracks_[t];
        if (!perfTrack(t) || !isTableMode(tr.playMode)) continue;
        uint8_t rollSel = 0;
        tr.step = int16_t(tableStep(tr.playMode, 0, lastIndex(t), rollSel));
        applyRollSelector(tr.inject, rollSel, rng_);
    }
    for (auto& tr : tracks_) {
        tr.enabled = true;
        tr.sinceTrigger = 0;
        tr.gate = 0;
        tr.gateLimit = -1;
        tr.delayActive = false;
        // A note recorded at the end of the count-in already sounds: step 0
        // must not trigger it again.
        if (!afterCountIn) tr.skipStep = -1;
        clearRoll(tr);
        clearArp(tr);
    }
    playing_ = true;
    tick_ = 1;
    masterMicro_ = 1;
    accUs_ = 0;  // OT: deterministic start phase
    breakPending_ = breakPreloadDone_ = false;
    stopAtStepEnd_ = false;
    dropPreload();
    countInSteps_ = 0;
    sendRealtime(settings_.transportOut, 0xFA);
}

void Sequencer::play(int startStep) { startPlayback(startStep, false, false); }

void Sequencer::userPlay(int startStep) {
    if (settings_.clockIn != 0 && !rendering_) {
        armed_ = true;  // wait for external Start (spec 03 §4.3)
        return;
    }
    play(startStep);
}

void Sequencer::playSong(int position) {
    if (!project_) { play(0); return; }
    const int pos = clampi(position, 0, kSongSlots - 1);
    uint8_t num = project_->playlist[pos];
    if (num == 0) num = curPatternNum_;
    if (num != curPatternNum_ && cur_) host_->loadPattern(num, *cur_, true);
    // Song start reloads the play buffer: per-track overrides go (spec 12 §C.3).
    freshCommit_ = 0;
    discardTrackOverrides();
    if (perfMode_) reloadPerfLengths();
    curPatternNum_ = num;
    project_->setCurrentPattern(num);
    songPos_ = pos;
    project_->playlistPos = uint8_t(pos);
    songMode_ = true;
    startPlayback(0, true, false);
}

void Sequencer::playSelection(int stepStart, int stepEnd, int trackStart, int trackEnd, bool loop) {
    const uint8_t savedOut = settings_.transportOut;
    if (!loop) settings_.transportOut = 0;  // 0xFA only when looping
    startPlayback(stepStart, false, false);
    settings_.transportOut = savedOut;
    selection_ = true;
    selLoop_ = loop;
    selStart_ = int16_t(clampi(stepStart, 0, kStepCount - 1));
    selEnd_ = int16_t(clampi(stepEnd, selStart_, kStepCount - 1));
    selTrackStart_ = uint8_t(clampi(trackStart, 0, kTracks - 1));
    selTrackEnd_ = uint8_t(clampi(trackEnd, selTrackStart_, kTracks - 1));
    for (int t = 0; t < kTracks; ++t) tracks_[t].enabled = t >= selTrackStart_ && t <= selTrackEnd_;
}

void Sequencer::stop() {
    // spec 03 §2.1 Stop [FW:0x11b4c]: always rewinds.
    for (int t = 0; t < kTracks; ++t) {
        TrackState& tr = tracks_[t];
        if (tr.midiCount) noteOffTrack(t, NoteOffMode::Off);
        else if (isSynth(tr.last.instr) && sink_) sink_->synthNoteOff(uint8_t(t), NoteOffMode::Off);
        else if (t < kVoiceTracks && sink_) sink_->voiceNoteOff(uint8_t(t), NoteOffMode::Off);
        tr.sounding = false;
        clearRoll(tr);
        clearArp(tr);
        tr.delayActive = false;
        tr.liveHeld = false;
        tr.skipStep = -1;
        tr.enabled = true;
    }
    tempoOverride_ = false;
    stopAtStepEnd_ = false;
    swingOn_ = false;
    swingPhase_ = false;
    swing_ = 50.0f;
    countInSteps_ = 0;
    playing_ = false;
    recording_ = false;
    armed_ = false;
    breakPending_ = breakPreloadDone_ = false;
    selection_ = false;
    dropPreload();
    resetPositions(0);
    if (sink_) sink_->synthTransportStop();   // macro overrides -> page values
    sendRealtime(settings_.transportOut, 0xFC);
    if (host_->exportActive()) host_->finaliseExport();
}

void Sequencer::record() {
    // spec 03 §2.1 Record start [FW:0x113dc].
    recording_ = true;
    releaseAllNotes();
    if (settings_.clockIn != 0 && !rendering_) {
        armed_ = true;
        return;
    }
    if (settings_.preRoll == 1) {
        countInSteps_ = (settings_.tsNum + 1) * (settings_.tsDen + 1);
        countInTicks_ = 0;
        accUs_ = 0;
        return;
    }
    play(0);
}

void Sequencer::switchPatternNow(uint8_t num) {
    if (num < 1 || num > kPatternCount || !cur_ || !next_) return;
    // Take the stage buffer back first (a request the audio context has not
    // committed yet is dropped): from here on an audio block reads 0 and
    // leaves stage_ alone, so it may run in the middle of the load below.
    pendSwitch_.exchange(0, std::memory_order_acq_rel);
    if (Pattern* st = stage_.load(std::memory_order_relaxed)) host_->loadPattern(num, *st, true);
    pendSwitch_.store(num, std::memory_order_release);
    if (!playing_) commitSwitch();
}

void Sequencer::commitSwitch() {
    // Audio context (or main loop while stopped). The new pattern goes in
    // through a buffer that is not being played (spec 03 §2.2 0x11a34): the
    // stage buffer the main loop filled (pointer swap), else the preload
    // buffer, loaded here.
    const uint8_t num = pendSwitch_.exchange(0, std::memory_order_acquire);
    if (num == 0 || !cur_ || !next_) return;
    if (Pattern* st = stage_.load(std::memory_order_relaxed)) {
        stage_.store(cur_, std::memory_order_relaxed);
        cur_ = st;
        afterSwap();
    } else {
        host_->loadPattern(num, *next_, true);
        swapBuffers();
    }
    curPatternNum_ = num;
    if (project_) project_->setCurrentPattern(num);
    // A preload is for the pattern before the switch (or, without a stage
    // buffer, went into the buffer that is now the old pattern).
    dropPreload();   // patternEnd reloads a queued pattern (sync)
    // SPEC GAP (spec 16 §A.2 / 06 §1.7 do not say): 0x3d548 re-points the
    // sequencer without the "reset positions" of a pattern end, so playback
    // keeps its step; a step beyond the new length wraps into it.
    for (int t = 0; t < kTracks; ++t) {
        TrackState& tr = tracks_[t];
        const int li = lastIndex(t);
        if (tr.step > li) tr.step = int16_t(tr.step % (li + 1));
        if (tr.stepsPlayed > li) tr.stepsPlayed = int16_t(tr.stepsPlayed % (li + 1));
    }
    const int bli = baseLastIndex();
    if (masterPos_ > bli) masterPos_ = int16_t(masterPos_ % (bli + 1));
}

void Sequencer::queuePattern(int delta) {
    // spec 03 §2.2 [FW:0x121a4].
    const int base = queueActive_ && queued_ ? queued_ : curPatternNum_;
    queued_ = clampi(base + delta, 1, kPatternCount);
    queueActive_ = true;
    queueReady_ = false;
}

// ---------------------------------------------------------------------------
// MIDI input (spec 03 §4.2, §4.3)
// ---------------------------------------------------------------------------

static bool portSelected(uint8_t setting, MidiPortId port) {
    return (setting == 1 && port == MidiPortId::kUsb) || (setting == 2 && port == MidiPortId::kDin);
}

void Sequencer::midiRealtimeIn(MidiPortId port, uint8_t byte) {
    if (byte == 0xF8) {
        if (portSelected(settings_.clockIn, port)) onClock();
        return;
    }
    if (!portSelected(settings_.transportIn, port)) return;
    if (byte == 0xFA) onStart(false);
    else if (byte == 0xFB) onStart(true);
    else if (byte == 0xFC) onStop();
}

void Sequencer::onClock() {
    ++pendingPulses_;
    ++pulsesThisBlock_;
    sinceClockS_ = 0;
}

void Sequencer::onStart(bool continueMsg) {
    pendingPulses_ = -1;  // the first pulse only aligns
    leftover_ = 0;
    pendingCorrection_ = int32_t(clampi(settings_.clockSyncCorrection, 0, 12)) - 6;
    intervalAcc_ = 0;
    intervalCount_ = 0;
    intervalPos_ = 0;
    const bool rec = recording_ || armed_;
    if (!continueMsg && host_->songScreenActive() && project_) {
        int pos = songPos_;
        for (int i = 0; i < kSongSlots && project_->playlist[i]; ++i) {
            if (project_->playlist[i] == curPatternNum_) { pos = i; break; }
        }
        playSong(pos);
    } else {
        play(0);
    }
    recording_ = rec;
    armed_ = false;
}

void Sequencer::onStop() {
    pendingPulses_ = 0;
    pendingCorrection_ = 0;
    leftover_ = 0;
    stop();
}

// ---------------------------------------------------------------------------
// MIDI output helpers (spec 03 §4.1, §4.4)
// ---------------------------------------------------------------------------

void Sequencer::sendRealtime(uint8_t ports, uint8_t byte) {
    if (rendering_ || !sink_ || !(ports & 3)) return;
    sink_->midiSend(uint8_t(ports & 3), &byte, 1);
}

void Sequencer::sendMidi(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t len) {
    if (rendering_ || !sink_ || !(settings_.midiOut & 3)) return;
    const uint8_t buf[3] = {b0, b1, b2};
    sink_->midiSend(uint8_t(settings_.midiOut & 3), buf, len);
}

uint8_t Sequencer::midiNoteNumber(int note) const {
    return uint8_t(clampi(note + 12 * (2 - int(settings_.middleC)), 0, 127));
}

void Sequencer::sendCcFx(int t, uint8_t letter, uint8_t value, const NoteRec& rec) {
    const MidiInstrument mi = midiInstr(rec.instr);
    const uint8_t cc = mi.cc[letter < 6 ? letter : 0];
    const uint8_t ch = uint8_t(rec.instr - 48);  // status nibble = channel - 1
    const uint8_t v = value > 127 ? 127 : value;
    if (cc <= 127) {
        sendMidi(uint8_t(0xB0 | ch), cc, v, 3);
    } else if (cc == 0x80) {
        sendMidi(uint8_t(0xC0 | ch), v, 0, 2);  // Program Change (I)
    } else if (cc == 0x81) {
        sendMidi(uint8_t(0xD0 | ch), v, 0, 2);  // Channel Pressure (I)
    } else if (cc == 0x82 && rec.note >= 0) {
        int note = rec.note;
        const TrackState& tr = tracks_[t];
        if (perfMode_ && tr.transposeOn) note = clampi(note + tr.transpose, 0, 127);
        sendMidi(uint8_t(0xA0 | ch), midiNoteNumber(note), v, 3);  // Poly Pressure (I)
    }
}

void Sequencer::sendPitchBendFx(uint8_t instr, uint8_t value) {
    const int v = value > 198 ? 198 : value;
    if (!isMidiInstr(instr)) return;  // 64..66: no output channel defined (spec 03 §7 item 5)
    const int bend = -4000 + (v * 8000) / 198;
    const int raw = clampi(bend + 8192, 0, 16383);
    sendMidi(uint8_t(0xE0 | (instr - 48)), uint8_t(raw & 0x7F), uint8_t(raw >> 7), 3);
}

// ---------------------------------------------------------------------------
// live input / recording (spec 03 §5)
// ---------------------------------------------------------------------------

bool Sequencer::cellFree(const Step& s) {
    return (s.note == kNoteEmpty || s.note <= -5) && s.fx[0].type == 0 && s.fx[1].type == 0;
}

bool Sequencer::liveTargetStep(int t, int& target) const {
    const TrackState& tr = tracks_[t];
    const uint8_t opt = settings_.recordingOptions;
    const bool micro = (opt & ~2) == 1;
    target = tr.step;
    if (!micro && tr.micro > 24) {
        target = tr.step + 1;
        if (target > lastIndex(t)) target = loopStart();
    }
    return micro;
}

int Sequencer::liveNoteOn(uint8_t note, uint8_t velocity, uint8_t cursorTrack, uint8_t instrument,
                          uint8_t instrumentVolume) {
    if (note > 127 || cursorTrack >= kTracks) return -1;
    const int8_t vol = int8_t(velocity > 127 ? 100 : velocity * 100 / 127);
    int t = cursorTrack;
    int target = 0;
    // The last half step of a Pre-roll count-in rounds to step 0, like a note
    // late in a step rounds to the next one: a note meant for the first beat
    // that comes a little early is recorded, not just played (OpenTracker
    // rule, the specs do not say; USER_REPORTS_R3 R3-1).
    const bool countInEnd = !playing_ && recording_ && cur_ && countInSteps_ > 0 &&
                            countInTicksLeft() <= uint32_t(kTicksPerStep / 2);
    if ((playing_ && recording_ && cur_) || countInEnd) {
        const uint8_t opt = settings_.recordingOptions;
        int found = -1;
        bool micro = false;
        for (int k = cursorTrack; k < kTracks; ++k) {
            micro = countInEnd ? false : liveTargetStep(k, target);
            if (tracks_[k].recEnabled && cellFree(cur_->tracks[k].steps[target])) { found = k; break; }
        }
        if (found < 0) return -1;
        t = found;
        uint8_t instr = instrument;
        if (t >= 8 && instr < 48) instr = 48;
        Step& cell = cur_->tracks[t].steps[target];
        cell.note = int8_t(note);
        cell.instrument = instr;
        if (micro) {
            cell.fx[1].type = 2;  // FX-A = Micro-move
            cell.fx[1].value = uint8_t(clampi((masterMicro_ - 1) * 100 / 47, 0, 100));
        }
        if (opt == 2 || opt == 3) {
            const int v = velocity * instrumentVolume / 127;
            cell.fx[0].type = 18;  // FX-B = Volume
            cell.fx[0].value = uint8_t(clampi((v * 100 + 63) / 127, 0, 100));
        }
        host_->markPatternDirty();
        TrackState& tr = tracks_[t];
        tr.liveHeld = true;
        tr.liveNote = note;
        tr.liveInstr = instr;
        tr.liveStep = int16_t(target);
        if (target != tr.step || countInEnd) tr.skipStep = int16_t(target);
        instrument = instr;
    } else {
        if (t >= 8 && instrument < 48) instrument = 48;
        tracks_[t].liveHeld = true;
        tracks_[t].liveNote = note;
        tracks_[t].liveStep = -1;
    }
    // Play it live.
    TrackState& tr = tracks_[t];
    if (tr.sounding || tr.midiCount) {
        if (isMidiLike(tr.last.instr) || isMidiLike(instrument)) noteOffTrack(t, NoteOffMode::Off);
    }
    NoteRec rec;
    rec.note = int8_t(note);
    rec.instr = instrument;
    rec.volume = vol;
    triggerNote(t, rec, false, 0);
    tr.last = rec;
    tr.gateLimit = -1;
    tr.sinceTrigger = 1;
    return t;
}

void Sequencer::liveNoteOff(uint8_t note, bool writeOff) {
    int t = -1;
    for (int k = 0; k < kTracks; ++k) {
        if (tracks_[k].liveHeld && tracks_[k].liveNote == note) { t = k; break; }
    }
    if (t < 0) return;
    TrackState& tr = tracks_[t];
    noteOffTrack(t, NoteOffMode::Off);
    tr.liveHeld = false;
    if (!writeOff) {
        tr.liveStep = -1;
        return;
    }
    if (!(playing_ && recording_ && cur_) || tr.liveStep < 0) return;
    int target = 0;
    const bool micro = liveTargetStep(t, target);
    if (target == tr.liveStep) {
        target = target + 1;
        if (target > lastIndex(t)) target = loopStart();
    }
    Step& cell = cur_->tracks[t].steps[target];
    if (cell.note == kNoteEmpty) {
        cell.note = kNoteOff;
        cell.instrument = tr.liveInstr;   // not 0 (USER_REPORTS_R3 R3-2)
        if (micro && target == tr.step) {
            cell.fx[1].type = 2;
            cell.fx[1].value = uint8_t(clampi((masterMicro_ - 1) * 100 / 47, 0, 100));
        }
        host_->markPatternDirty();
    }
    tr.liveStep = -1;
}

void Sequencer::stepEntry(uint8_t note, uint8_t velocity, int step, int track, uint8_t instrument) {
    if (!cur_ || !validTrack(track) || step < 0 || step >= kStepCount || note > 127) return;
    Step& cell = cur_->tracks[track].steps[step];
    const bool wasEmpty = cell.note == kNoteEmpty;
    cell.note = int8_t(note);
    if (wasEmpty) {
        if (track >= 8 && instrument < 48) instrument = 48;
        cell.instrument = instrument;
    }
    host_->markPatternDirty();
    // Preview (spec 03 §5.1).
    TrackState& tr = tracks_[track];
    if ((tr.sounding || tr.midiCount) && (isMidiLike(tr.last.instr) || isMidiLike(cell.instrument)))
        noteOffTrack(track, NoteOffMode::Off);
    NoteRec rec;
    rec.note = int8_t(note);
    rec.instr = cell.instrument;
    rec.volume = int8_t(velocity > 127 ? 100 : velocity * 100 / 127);
    triggerNote(track, rec, false, 0);
    tr.last = rec;
}

}  // namespace ot
