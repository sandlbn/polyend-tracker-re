#include "app/app.h"

#include <cstdio>
#include <cstring>
#include <new>

#include "app/project_fields.h"
#include "app/project_io.h"
#include "formats/file_io.h"
#include "sdram_bss.h"
#include "ui/screens.h"

namespace ot {

namespace {
constexpr uint32_t kAutosaveMs = 10000;   // OpenTracker: workspace autosave period
constexpr uint32_t kConfigSaveMs = 5000;  // spec 05 §1.7 settings write-back
}  // namespace

#if defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE
namespace {
// Storage of the OT_APP_SDRAM_MEMBER members (app.h). Raw bytes, so nothing
// runs on it at static initialisation (the SDRAM is not up yet).
struct AppSdram {
    alignas(PatternNames) unsigned char names[sizeof(PatternNames)];
    alignas(ProjectJob) unsigned char pj[sizeof(ProjectJob)];
    alignas(SynthPatch) unsigned char patches[sizeof(SynthPatch) * kSynthSlotCount];
    unsigned char extKeep[kPatternCount + 1];
};
OT_SDRAM_BSS AppSdram gAppSdram;
}  // namespace

App::App()
    : names(*reinterpret_cast<PatternNames*>(gAppSdram.names)), bank(engine.bank()), ui(*this),
      extKeep_(*reinterpret_cast<ExtKeep*>(gAppSdram.extKeep)),
      patches_(*reinterpret_cast<SynthPatches*>(gAppSdram.patches)), pj_(*reinterpret_cast<ProjectJob*>(gAppSdram.pj)) {}
#else
App::App() : bank(engine.bank()), ui(*this) {}
#endif

bool App::init(const HalRefs& hal, const AppMemory& mem) {
#if defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE
    // The SDRAM members (app.h OT_APP_SDRAM_MEMBER): constructed now, after
    // otAppMain() zeroed `.sdram_bss`.
    new (&names) PatternNames();
    new (&pj_) ProjectJob();
    for (SynthPatch& p : patches_) new (&p) SynthPatch();
#else
    for (SynthPatch& p : patches_) p = SynthPatch{};   // plain member without an initialiser
#endif
    // Every build: on the host extKeep_ is a plain member with no initialiser
    // (on the device `.sdram_bss` is already zero).
    std::memset(extKeep_, 0, sizeof(extKeep_));
    hal_ = hal;
    mem_ = mem;
    if (!hal_.storage || !hal_.clock || !mem_.patterns || !mem_.clipboard || !mem_.seqBuffers) return false;
    // 2.x ext-track pool (docs/M1_PLAN.md §1); the last record is the
    // pool-full write target of storedTrack().
    if (!mem_.extTracks || mem_.extTrackBlocks < 2 || !mem_.extMeta) return false;
    extDiscard_ = &mem_.extTracks[mem_.extTrackBlocks - 1];
    ext_.attach(mem_.extTracks, mem_.extTrackBlocks - 1, mem_.extMeta);
    // spec 04: delay/reverb memory and the sample pool are caller-provided (SDRAM).
    if (!engine.init(mem_.fxMemory, mem_.fxSamples, mem_.sampleBank, mem_.sampleBankSamples, instruments))
        return false;
    engine.setMidiOut(hal_.midi);
    engine.setPreBlockHook(&App::preBlockThunk, this);
    // Tempo-locked LFO phase (spec 07 Q5).
    engine.setTransportSource(
        [](void* ctx, uint32_t* tick, int16_t* step0) {
            const Sequencer& s = static_cast<App*>(ctx)->seq;
            if (!s.isPlaying()) return false;
            *tick = s.tickPosition();
            *step0 = int16_t(s.step(0));
            return true;
        },
        this);
    seq.setPerfPatternMemory(mem_.perfPatterns);   // spec 12 §C.1 staging (SDRAM)
    engine.setExportScratch(mem_.exportStems);
    // Synth slots play the App's patches (2.x M3, audio/synth_bank.h).
    for (int s = 0; s < kSynthSlotCount; ++s) engine.synth().bindPatch(s, &patches_[s]);
    undo.attach(mem_.undo, mem_.undoNums, mem_.undoSlots);
    fileUndo.attach(mem_.fileUndo, mem_.fileUndoNums, mem_.fileUndoSlots);
    for (auto& in : instruments) in.setDefault();
    project.setDefault();
    names.clear();
    resetPatternStore();
    defaultPattern(mem_.seqBuffers[0]);
    defaultPattern(mem_.seqBuffers[1]);
    if (mem_.seqStage) defaultPattern(*mem_.seqStage);
    attachSequencer();
    applyEngineSettings();
    ui.attach(hal_.display, hal_.leds);
    if (hal_.audio) {
        engine.attachOutput(*hal_.audio);
        hal_.audio->start();
    }
    return true;
}

void App::attachSequencer() {
    Pattern* a = seq.currentPattern() ? seq.currentPattern() : &mem_.seqBuffers[0];
    Pattern* b = seq.preloadPattern() ? seq.preloadPattern() : &mem_.seqBuffers[1];
    Pattern* c = seq.stagePattern() ? seq.stagePattern() : mem_.seqStage;
    seq.attach(&engine, this, &project, a, b, c);
}

void App::defaultPattern(Pattern& p) const {
    p.setDefault();
    uint16_t len = project.defaultPatternLength();
    if (len < 1 || len > kStepCount) len = 32;
    pops::setLength(p, len);
}

void App::defaultPattern(PatternStock& p) const {
    // Same as the Pattern overload (pops::setLength: every track one length).
    p.setDefault();
    uint16_t len = project.defaultPatternLength();
    if (len < 1 || len > kStepCount) len = 32;
    for (auto& t : p.tracks) t.lastStep = uint8_t(len - 1);
}

bool App::boot() {
    IStorage& f = fs();
    // spec 02 §1 directory-creation rule.
    if (!f.mkdir(kWorkspace)) return false;
    f.mkdir("Workspace/instruments");
    f.mkdir("Workspace/patterns");
    f.mkdir("Workspace/samples");
    f.mkdir(kProjectsDir);
    loadConfig();
    applySequencerSettings();
    // spec 02 §1.1 boot / resume.
    bool ok = false;
    if (f.exists("Workspace/project.mt")) ok = loadWorkspace();
    if (!ok) ok = newProject();
    booted_ = true;
    lastAutosave_ = millis();
    ui.start();
    return ok;
}

void App::applySequencerSettings() {
    // Display font set (spec 05 §3.6, stock 0x5ffc8). Applied here because this
    // runs at boot after loadConfig and after every config Apply / reset.
    applyFontSet(hal_.display, cfg.font);
    SequencerSettings& s = seq.settings();
    s.clockIn = cfg.clockIn;
    s.clockOut = cfg.clockOut;
    s.transportIn = cfg.transportIn;
    s.transportOut = cfg.transportOut;
    s.midiOut = cfg.midiOut;
    s.middleC = cfg.middleC;
    s.clockSyncCorrection = cfg.clockCorrection;
    s.recordingOptions = cfg.recordingOptions;
    s.metronome = cfg.metronome;
    s.preRoll = cfg.preRoll;
    s.tsNum = cfg.tsNum;
    s.tsDen = cfg.tsDen;
}

void App::preBlockThunk(void* ctx, float seconds) { static_cast<App*>(ctx)->preBlock(seconds); }

void App::preBlock(float seconds) {
    if (engineDirty_) {
        applyEngineSettings();
        engine.noteSeqWork(audio::kSeqEngineApply);
    }
    const int16_t* in = hal_.lineIn && !rendering_ ? hal_.lineIn(hal_.lineInCtx) : nullptr;
    // Recorder input tap (spec 05 appendix C §2): meter / count-in / record.
    if (!rendering_) {
        capture.process(in, audio::kBlock, &engine);
        if (capture.recording()) engine.noteSeqWork(audio::kSeqRecorder);
    }
    // Monitor off (appendix C §2.7) keeps the input out of the mix. Monitor
    // On in the Recorder mixes it at unity, not at the Global Mixer line
    // level / mute (spec 22 §5).
    if (in && !inputMonitorMuted) engine.setLineInput(in);
    engine.setLineMonitor(capture.metering() && !inputMonitorMuted && !rendering_);
    // spec 05 §1.3 / spec 04 §1.1 step 1: the audio block drives the sequencer
    // clock (live 128/44122.65 s, offline kRenderBlockSeconds).
    seq.processElapsed(seconds);
    // [I] Live resampling: did a bar start in this block (its notes are
    // rendered in it)? Bars follow the pattern as the metronome counts them
    // (track 1's step % bar): the pattern's first step always starts one, so
    // a pattern shorter or longer than whole bars stays in step with them.
    if (!rendering_) {
        const bool playing = seq.isPlaying();
        // A step counts once its first micro-step has run (its notes fired):
        // the step number advances one micro-step (12 ticks) earlier, which
        // started takes up to a block before the downbeat (USER_REPORTS_R3 R3-8).
        int32_t st = seq.step(0);
        if (playing && resampleWasPlaying_ && seq.microStep(0) <= 1) st = resampleLastStep_;
        const int32_t bar = resampleBarSteps();
        bool barStart = false;
        if (playing) {
            if (!resampleWasPlaying_) {
                barStart = st % bar == 0;   // play start
            } else if (st < resampleLastStep_) {
                barStart = true;            // the pattern wrapped (or restarted): step 0
            } else {
                for (int32_t k = resampleLastStep_ + 1; k <= st; ++k) barStart = barStart || k % bar == 0;
            }
        }
        resampleWasPlaying_ = playing;
        resampleLastStep_ = st;
        if (resampler.busy()) {
            resampler.onBlock(playing, barStart);
            // Line in takes record the input as it comes in, whatever the
            // monitor (Monitor Off keeps it out of the engine's mix).
            if (resampler.source() == audio::AudioEngine::kTapLineIn)
                resampler.tapLine(in, engine.settings().lineInChannel, audio::kBlock);
        }
    }
}

void App::applyEngineSettings() {
    engineDirty_ = false;
    audio::MixerSettings& m = engine.settings();
    m.loadFromProject(project);
    // Track mute comes from the project bytes 0x12C+t (spec 03 §2.4); the
    // Track Mixer implements solo by muting.
    m.masterVolume = cfg.masterVolume > 100 ? 100 : cfg.masterVolume;   // spec 05 §5.4
    m.metronomeVolume = cfg.metronomeVolume;
    m.previewVolume = cfg.previewVolume > 100 ? 100 : cfg.previewVolume;
    // Config "Line in channel" (spec 15 §2.2 #11): clamp > 2 -> 2 and push it
    // to the line input, both the Global-mixer pass-through and the recorder.
    const uint8_t lineCh = cfg.lineInChannel > 2 ? 2 : cfg.lineInChannel;
    m.lineInChannel = lineCh;
    capture.setChannelMode(lineCh);
    // 2.x synth slots: the project records (spec 21 §3.3) and the voice
    // budget from Config (OPTIMIZATION_2X item 10 / OP-8).
    for (int s = 0; s < kSynthSlotCount; ++s) engine.synth().setSlotRecord(s, project.synthSlot(s));
    if (engine.governor().budget() != cfg.synthVoices) engine.governor().setBudget(cfg.synthVoices);
}

// ---------------------------------------------------------------------------
// Audition on the track voices (spec 07 Q7)

int App::auditionTrack(uint16_t voiceId, int from) const {
    // Scan from the cursor track (FW:0x114B8).
    const int start = from >= 0 && from < kAuditionTracks ? from : editorTrack;
    for (int k = 0; k < kAuditionTracks; ++k) {
        const int t = (start + k) % kAuditionTracks;
        if (aud_[t].busy && aud_[t].id == voiceId) return t;
    }
    return -1;
}

void App::releaseAudition(int t) {
    Audition& r = aud_[t];
    if (!r.busy) return;
    if (r.midi) {
        if (hal_.midi) {
            // Config "MIDI out" port mask, as for the sequencer (spec 17 §1.4 step 7).
            const uint8_t msg[3] = {uint8_t(0x80 | r.ch), r.note, 0};
            routeMidi(*hal_.midi, uint8_t(cfg.midiOut & 3), msg, 3);
        }
    } else if (t < audio::kVoices) {
        engine.voiceNoteOff(uint8_t(t), NoteOffMode::Off);   // normal envelope release
    }
    r = Audition{};
}

uint8_t App::liveVelocity(uint8_t velocity) const {
    // spec 17 §1.4 step 1: 127 unless Recording options is a Velocity mode.
    if (cfg.recordingOptions == 2 || cfg.recordingOptions == 3) return velocity > 127 ? 127 : velocity;
    return 127;
}

uint8_t App::liveVolume(uint8_t instrument, uint8_t vel) const {
    // spec 17 §1.4 step 7: vol = vel * IV / 127 (FW:0x1191c).
    int iv = 100;
    if (instrument < kSampleInstrumentCount)
        iv = (instruments[instrument].volume * 127 + 50) / 100;   // round(V * 127 / 100)
    else if (isMidiInstrument(instrument))
        iv = project.midiInstrument(instrument - kFirstMidiInstrument).velocity;
    if (iv > 127) iv = 127;
    return uint8_t((vel > 127 ? 127 : vel) * iv / 127);
}

int App::previewOn(uint16_t voiceId, uint8_t instrument, uint8_t note, uint8_t velocity, int forceTrack,
                   int startTrack) {
    // e.g. a running 0x60 loader preview is released first. MIDI input keys
    // are not de-duplicated: the same note twice takes two tracks (spec 17
    // §1.4 step 5); each note-off releases one.
    if (voiceId < kAuditionMidiIn || voiceId >= kAuditionMidiIn + 128) previewOff(voiceId);
    const bool midi = isMidiInstrument(instrument);
    if (isSynthInstrument(instrument)) {
        if (voiceId >= kAuditionMidiIn && voiceId < kAuditionMidiIn + 128) {
            // MIDI input: one live track per held note (polyphonic, up to the
            // slot polyphony and the voice cap); all held: the tracks are
            // reused in turn.
            // Velocity as for samples (spec 17 §1.4 step 1: 127 unless
            // Recording options = velocity).
            static_assert(sizeof(synthLive_) / sizeof(synthLive_[0]) == audio::kSynthLiveTracks, "live tracks");
            int k = -1;
            for (int i = 0; i < audio::kSynthLiveTracks && k < 0; ++i)
                if (!synthLive_[i]) k = i;
            if (k < 0) k = synthLiveNext_;
            synthLiveNext_ = uint8_t((k + 1) % audio::kSynthLiveTracks);
            synthLive_[k] = voiceId;
            const uint8_t vel = liveVelocity(uint8_t(velocity & 127));
            engine.synth().liveOn(k, synthSlotOf(instrument), uint8_t(note & 127),
                                  vel >= 127 ? int8_t(-1) : int8_t((vel * 100 + 63) / 127));
            return -1;
        }
        // 2.x synth slots: the framework's audition track (one note at a time).
        engine.synth().auditionOn(synthSlotOf(instrument), uint8_t(note & 127));
        synthAudVoice_ = voiceId;
        return -1;
    }
    if (!midi && instrument >= kSampleInstrumentCount) return -1;
    // spec 17 §1.4 step 6: the track is claimed even when nothing can sound
    // (empty sample slot, no MIDI port), so step entry still applies.
    const bool sound = midi ? hal_.midi != nullptr : bank.loaded(instrument);
    int track = -1;
    if (forceTrack >= 0) {
        if (forceTrack < kAuditionTracks && (midi || forceTrack < audio::kVoices)) {
            track = forceTrack;
            releaseAudition(track);
        }
    } else {
        const int start = startTrack >= 0 && startTrack < kAuditionTracks ? startTrack : editorTrack;
        for (int k = 0; k < kAuditionTracks && track < 0; ++k) {
            const int t = (start + k) % kAuditionTracks;
            if (!midi && t >= audio::kVoices) continue;
            if (trackMuted(t)) continue;
            // The pattern editor's note-encoder audition has no release
            // event (stock re-triggers the row instead): its track counts as
            // free, else MIDI keys stopped writing at the cursor after a jog
            // turn until a pad press / project load (user report).
            if (aud_[t].busy && aud_[t].id != kAuditionEncoder) continue;
            if (aud_[t].busy) releaseAudition(t);
            track = t;   // tracks playing pattern notes are not skipped
        }
    }
    if (track < 0) return -1;
    // Velocity is 127 unless Recording options = velocity (spec 03 §5.2,
    // spec 17 §1.4 step 1); the sounding volume is vel * IV / 127 (step 7).
    const uint8_t vel = liveVelocity(uint8_t(velocity & 127));
    const uint8_t vol = liveVolume(instrument, vel);
    Audition& r = aud_[track];
    if (midi) {
        // MIDI instrument Mn plays on channel n, like the sequencer (spec 03
        // §4.4); the record's channel byte is 0-based and not used for output
        // (reading it as 1-based shifted M02..M16 down one, USER_REPORTS_R3 R3-6).
        const uint8_t ch = uint8_t((instrument - kFirstMidiInstrument) & 15);
        const uint8_t msg[3] = {uint8_t(0x90 | ch), uint8_t(note & 127), uint8_t(vol ? vol : 1)};
        // Routed by Config "MIDI out" like the sequencer (spec 17 §1.4 step 7).
        if (sound) routeMidi(*hal_.midi, uint8_t(cfg.midiOut & 3), msg, 3);
        r.ch = ch;
    } else if (sound) {
        // Velocity-scaled volume as in step entry (spec 03 §5.1).
        VoiceNoteOn n;
        n.instrument = instrument;
        n.note = uint8_t(note & 127);
        // Full velocity keeps the instrument volume; otherwise round(vol * 100 / 127).
        n.volume = vel >= 127 ? int8_t(-1) : int8_t((vol * 100 + 63) / 127);
        engine.voiceNoteOff(uint8_t(track), NoteOffMode::Off);
        engine.voiceNoteOn(uint8_t(track), n);
    }
    r.busy = true;
    r.midi = midi;
    r.id = voiceId;
    r.instrument = instrument;
    r.note = uint8_t(note & 127);
    return track;
}

void App::previewOff(uint16_t voiceId, int startTrack) {
    if (voiceId == synthAudVoice_) {
        engine.synth().auditionOff();
        synthAudVoice_ = kNoSynthAudition;
    }
    // A MIDI input note on a synth live track: one note-off releases one.
    for (int k = 0; k < audio::kSynthLiveTracks; ++k)
        if (synthLive_[k] && synthLive_[k] == voiceId) {
            engine.synth().liveOff(k);
            synthLive_[k] = 0;
            return;
        }
    const int t = auditionTrack(voiceId, startTrack);
    if (t >= 0) releaseAudition(t);
}

void App::previewAllOff() {
    for (int t = 0; t < kTrackCount; ++t) releaseAudition(t);
    if (synthAudVoice_ != kNoSynthAudition) engine.synth().auditionOff();
    synthAudVoice_ = kNoSynthAudition;
    for (int k = 0; k < audio::kSynthLiveTracks; ++k)
        if (synthLive_[k]) {
            engine.synth().liveOff(k);
            synthLive_[k] = 0;
        }
    engine.previewStop();   // SD file preview
}

void App::allVoicesOff() {
    previewAllOff();
    engine.stopAll();
}

void App::loop() {
    const uint32_t now = millis();
    if (hal_.service) hal_.service();
    if (!booted_) {
        // spec 05 §1.5 boot state 2: wait for a card.
        if (now - lastNoCardTry_ >= 1000 || lastNoCardTry_ == 0) {
            lastNoCardTry_ = now ? now : 1;
            if (!boot() && hal_.display) {
                Gfx g(*hal_.display);
                g.beginFrame(0);
                g.text(400, 240, kFontLarge, 0xFFFFFF, "Insert SD card", IDisplay::kOptCenter);
                g.endFrame();
            }
        }
        return;
    }
    // Audio output mute line (spec 01 §5.4, spec 05 §1.9): unmuted only once
    // the start-up project has loaded, and muted while a project loads, as on
    // stock; unmuting at audio start gave a crack at power-on (R3-4).
    if (hal_.audio) {
        const bool loading = jobBusy() && (jobOp() == JobOp::LoadWorkspace || jobOp() == JobOp::Open);
        if (outputOn_ == loading) {
            outputOn_ = !loading;
            hal_.audio->setOutputEnabled(outputOn_);
        }
    }
    // 2.x USB Storage mode: the computer owns the card. Only the UI (the mode
    // page) and the USB block I/O run; nothing here may touch the card.
    if (usbActive_) {
        loopUsbStorage(now);
        return;
    }
    // Card pulled while running: "Please insert SD card" screen until it returns.
    if (serviceCardRemoved(*this, now)) return;
    // Input.
    if (hal_.input) {
        InputEvent ev;
        int guard = 0;
        while (guard++ < 32 && hal_.input->poll(ev)) ui.handle(ev, now);
    }
    // spec 05 appendix E3: while a project is opened / saved (progress
    // popup) MIDI input waits; an autosave runs in the background.
    const bool modalJob = jobBusy() && jobOp() != JobOp::Autosave;
    if (!modalJob) serviceMidi();
    // Transport bookkeeping.
    const bool playing = seq.isPlaying();
    if (playing && !wasPlaying_) playStart_ = now;
    wasPlaying_ = playing;
    if (stepChanged_) {
        stepChanged_ = false;
        ui.dirty();
    }
    if (patternEditedFromSeq_) {
        patternEditedFromSeq_ = false;
        patternEdited();
    }
    serviceResample();
    // Project manager (spec 05 §1.3 step 5, appendix E3/E4): one bounded
    // step of the running job, else the periodic autosave check.
    if (jobBusy()) serviceProjectJob();
    else if (!sampleBankLent && now - lastAutosave_ >= kAutosaveMs) startAutosave();
    ui.update(now);
    if (hal_.audio) {   // spec 18 §1: Master EQ / Bass Boost / Space on the codec DAP
        float eq[5];
        for (int b = 0; b < 5; ++b) eq[b] = project.f32(pf::kEqGain + 4 * uint32_t(b));
        hal_.audio->applyMasterDsp(eq, project.u8(pf::kBassBoost), project.u8(pf::kSpace));
    }
    if (perfCc && ui.active() != kModPerformance) perfCc->perfBackground(now);   // spec 16 §A.15
    ui.renderIfDirty(now);
    syncRecorderSettings();
    // spec 05 §1.5 state 4 / 0xf814: dirty config written every >= 5 s, or
    // on the next pass when forced.
    if (configDirty_ && (configForce_ || now - lastConfigSave_ >= kConfigSaveMs)) {
        configDirty_ = configForce_ = false;
        lastConfigSave_ = now;
        saveConfig();
    }
}

void App::run() {
    for (;;) loop();
}

// ---------------------------------------------------------------------------
// MIDI input (spec 03 §4; notes audition / live record / step entry §5.1)

void App::serviceMidi() {
    if (!hal_.midi) return;
    uint8_t buf[32];
    for (int p = 0; p < 2; ++p) {
        const MidiPortId port = p == 0 ? MidiPortId::kDin : MidiPortId::kUsb;
        uint32_t n;
        int guard = 0;
        while (guard++ < 8 && (n = hal_.midi->receive(port, buf, sizeof(buf))) > 0)
            for (uint32_t i = 0; i < n; ++i) midiByte(port, buf[i]);
    }
}

void App::midiByte(MidiPortId port, uint8_t b) {
    if (b >= 0xF8) {
        // STRINGS_DIFF 1.9.3 item 2 (0x1223c): MIDI Start while the Song
        // screen is shown and the current pattern is not in the song: notify
        // and do not start.
        if (b == 0xFA && songScreenActive() && cfg.transportIn == (port == MidiPortId::kUsb ? 1 : 2) &&
            !patternInSong(patternNumber())) {
            ui.notify("Load pattern from song first");
            return;
        }
        seq.midiRealtimeIn(port, b);
        return;
    }
    MidiIn& m = midiIn_[port == MidiPortId::kDin ? 0 : 1];
    if (b & 0x80) {
        m.status = b >= 0xF0 ? 0 : b;
        m.count = 0;
        return;
    }
    if (!m.status) return;
    m.data[m.count++] = b;
    const uint8_t type = m.status & 0xF0;
    const uint8_t need = (type == 0xC0 || type == 0xD0) ? 1 : 2;
    if (m.count < need) return;
    m.count = 0;
    // Control Change: config "CC in" port mask and "CC in channel" (spec 12 §B.1).
    if (type == 0xB0) {
        if (midicc::portAccepted(cfg.ccIn, port == MidiPortId::kUsb) &&
            midicc::channelAccepted(cfg.ccInChannel, m.status & 15))
            handleControlChange(m.data[0], m.data[1]);
        return;
    }
    // Notes in: config "Notes in" port mask (1 USB, 2 DIN) and channel.
    const uint8_t mask = port == MidiPortId::kUsb ? 1 : 2;
    if (!(cfg.notesIn & mask)) return;
    if (cfg.notesInChannel && (m.status & 15) != cfg.notesInChannel - 1) return;
    handleMidiMessage(m.status, m.data[0], m.data[1]);
}

bool App::patternInSong(int num) const {
    for (int i = 0; i < kSongSlots && project.playlist[i]; ++i)
        if (project.playlist[i] == num) return true;
    return false;
}

void App::handleMidiMessage(uint8_t st, uint8_t d1, uint8_t d2) {
    const uint8_t type = st & 0xF0;
    // Only note on / off have handlers; pitch bend, aftertouch, program
    // change and SysEx are ignored (spec 17 §1.2, §1.5).
    if (type != 0x80 && type != 0x90) return;
    // Middle C transposition: n' = clamp(n + 12 * (MiddleC - 2), 0, 127).
    int n = int(d1 & 127) + 12 * (int(cfg.middleC > 3 ? 2 : cfg.middleC) - 2);
    const uint8_t note = uint8_t(n < 0 ? 0 : (n > 127 ? 127 : n));
    const bool noteOn = type == 0x90 && d2 > 0;   // velocity-0 note-on = note-off (spec 17 §1.2)
    // spec 17 §1.3: Sample Playback, Sample Editor and Sample Recorder own the
    // notes (their preview players); every other screen uses the track voices.
    const int act = ui.active();
    if (act == kModPlayback || act == kModEditor || act == kModRecorder)
        if (Screen* sc = ui.screen(act); sc && sc->onMidiNote(note, d2, noteOn)) return;
    const uint16_t voice = uint16_t(kAuditionMidiIn + note);   // key = note + 100 (own id space)
    // Target rectangle (spec 17 §1.3, FW:0x3c670): the pattern editor
    // selection or cursor, also while another screen is shown.
    auto* pe = static_cast<PatternScreen*>(ui.screen(kModPattern));
    SelRect rect;
    rect.s0 = rect.s1 = 0;
    rect.t0 = rect.t1 = editorTrack;
    if (pe) rect = pe->target();
    if (noteOn) {
        if (seq.isRecording()) {
            // spec 17 §1.4 step 9: the live-record path (spec 03 §5.2).
            seq.liveNoteOn(note, d2, 0, currentInstrument(), recordVolume(currentInstrument()));
            previewOn(voice, currentInstrument(), note, d2, -1, rect.t0);
            return;
        }
        // spec 17 §1.4 step 3: reset the voice FX while stopped.
        if (!seq.isPlaying()) engine.resetAllVoiceFx();
        // Steps 4-7: first free unmuted track from startTrack; no stealing.
        int t = previewOn(voice, currentInstrument(), note, d2, -1, rect.t0);
        // 2.x synth slots sound on the synth live tracks, not a pattern
        // track: step entry writes the n-th held note to the n-th track of
        // the target (user report: MIDI keys recorded nothing on S1..S3).
        if (t < 0 && isSynthInstrument(currentInstrument())) {
            int held = 0;
            for (int k = 0; k < audio::kSynthLiveTracks; ++k)
                if (synthLive_[k] && synthLive_[k] != voice) ++held;
            t = rect.t0 + held;
        }
        // Step 8: edit-mode chord entry across the selected tracks.
        if (t >= 0 && pe && ui.active() == kModPattern) pe->midiChordEntry(t, note, liveVelocity(d2));
    } else {
        // Velocity-0 note-on = note-off (spec 17 §1.2).
        if (seq.isRecording()) seq.liveNoteOff(note);
        previewOff(voice, rect.t0);
    }
}

uint8_t App::ccTargetInstrument() const {
    const uint8_t ci = currentInstrument();
    if (ci < kSampleInstrumentCount) return ci;
    return ccMapInstrument < kSampleInstrumentCount ? ccMapInstrument : 0;
}

void App::handleControlChange(uint8_t cc, uint8_t value) {
    cc &= 127;
    value &= 127;
    // (1) Performance, only while its screen is active (spec 12 §B.1-§B.3).
    if (ui.active() == kModPerformance && perfCc) {
        if (midicc::isPerfTrackCc(cc)) perfCc->perfTrackPatternCc(uint8_t(cc - 41), value);
        else if (midicc::isPerfSlotCc(cc)) perfCc->perfSlotCc(uint8_t(cc - 51), value);
    }
    // (2) Master levels on any screen, playing or not (§B.4).
    if (const uint32_t off = midicc::masterOffset(cc)) {
        const uint8_t lv = midicc::masterLevel(value);
        if (project.u8(off) != lv) {
            project.setU8(off, lv);
            // Stock sets no dirty flag, so the level is lost unless something
            // else is saved; OpenTracker marks the project changed (README).
            markProjectDirty();
            if (ui.active() == kModMaster) ui.dirty();
        }
    }
    // (3) Instrument map, in any play state (spec 17 §1.6: no play-state
    // test; sequenced notes pick the change up at once).
    if (midicc::findRow(cc)) {
        const uint8_t t = ccTargetInstrument();
        // SPEC GAP: an empty slot is not edited (it would create an instrument file).
        if (instrPresent[t] && midicc::applyInstrumentCc(instruments[t], cc, value)) {
            markInstrumentDirty(t);   // saved with the project (spec 17 §1.10)
            if (ui.active() == kModInstrument) ui.dirty();
        }
    }
    // (4) Sample Playback map CC 16-19 (spec 17 §1.7): always the current
    // instrument; no effect while a MIDI instrument is current.
    if (midicc::isPlaybackCc(cc)) {
        const uint8_t ci = currentInstrument();
        if (ci < kSampleInstrumentCount && instrPresent[ci] &&
            midicc::applyPlaybackCc(instruments[ci], bank.frames(ci), cc, value)) {
            markInstrumentDirty(ci);
            if (ui.active() == kModPlayback) ui.dirty();
        }
    }
}

// ---------------------------------------------------------------------------
// Patterns

void App::setPresent(int num, bool on) {
    if (num < 1 || num > kPatternCount) return;
    if (on) present_[num >> 3] |= uint8_t(1u << (num & 7));
    else present_[num >> 3] &= uint8_t(~(1u << (num & 7)));
}

bool App::patternHasData(int num) {
    if (num < 1 || num > kPatternCount) return false;
    auto hasData = [](const Track& t) {
        for (int s = 0; s < kStepCount; ++s) {
            const Step& st = t.steps[s];
            if (st.note != kNoteEmpty || st.fx[0].type || st.fx[1].type) return true;
        }
        return false;
    };
    if (num == patternNumber()) {
        for (const auto& t : pattern().tracks)
            if (hasData(t)) return true;
        return false;
    }
    if (!patternPresent(num)) return false;
    for (const auto& t : storedStock(num).tracks)
        if (hasData(t)) return true;
    for (int e = 0; e < kExtTrackCount; ++e)
        if (const Track* t = ext_.find(num, e); t && hasData(*t)) return true;
    return false;
}

int App::firstEmptyPattern() {
    for (int n = 1; n <= kPatternCount; ++n)
        if (!patternHasData(n)) return n;
    return kPatternCount;
}

void App::markPatternDirty(int num) {
    if (num < 1 || num > kPatternCount) return;
    patDirty_[num >> 3] |= uint8_t(1u << (num & 7));
    setPresent(num, true);
    unsaved_ = true;
}

void App::patternEdited() {
    const int num = patternNumber();
    // VERIFY_M1 R2: an edit of tracks 13..16 that does not fit the full
    // ext-track pool is refused: the working pattern is re-assembled from
    // the store, so what is shown (and played) is what will be saved. The
    // pool-full notice comes from storePattern().
    if (!storePattern(num, pattern())) assemblePattern(num, pattern());
    markPatternDirty(num);
    ui.dirty();
}

void App::reloadCurrentFromStore() {
    const int num = patternNumber();
    assemblePattern(num, pattern());
    ui.dirty();
}

// ---------------------------------------------------------------------------
// Resident pattern store: stock tracks + ext-track pool (docs/M1_PLAN.md §1)

namespace {
bool validNum(int num) { return num >= 1 && num <= kPatternCount; }
}  // namespace

void App::reportExtPoolFull() {
    // One notice per burst of refused allocations.
    if (ext_.fullEvents() == extFullReported_) return;
    extFullReported_ = ext_.fullEvents();
    ui.notify("Track memory full");
}

Track& App::storedTrack(int num, int t) {
    if (!validNum(num) || t < 0 || t >= kTrackCount) return *extDiscard_;
    if (t < kStockTrackCount) return storedStock(num).tracks[t];
    const int e = t - kStockTrackCount;
    if (Track* b = ext_.find(num, e)) return *b;
    // The discard record doubles as the initial image (no stack copy).
    defaultExtTrack(*extDiscard_, storedStock(num));
    if (Track* b = ext_.alloc(num, e, *extDiscard_)) return *b;
    reportExtPoolFull();
    return *extDiscard_;
}

bool App::storedTrackWritable(int num, int t) const {
    if (!validNum(num) || t < 0 || t >= kTrackCount) return false;
    if (t < kStockTrackCount) return true;
    return ext_.find(num, t - kStockTrackCount) != nullptr || ext_.freeBlocks() > 0;
}

const Track* App::findStoredTrack(int num, int t) const {
    if (!validNum(num) || t < 0 || t >= kTrackCount) return nullptr;
    if (t < kStockTrackCount) return &storedStock(num).tracks[t];
    return ext_.find(num, t - kStockTrackCount);
}

void App::readStoredTrack(int num, int t, Track& out) const {
    if (const Track* src = findStoredTrack(num, t)) out = *src;
    else if (validNum(num)) defaultExtTrack(out, storedStock(num));
    else out.clear(kDefaultTrackLastStep);
}

bool App::storePattern(int num, const Pattern& p) {
    if (!validNum(num)) return false;
    PatternStock& st = storedStock(num);
    p.copyStockTo(st);
    bool ok = true;
    for (int e = 0; e < kExtTrackCount; ++e) {
        const Track& src = p.tracks[kStockTrackCount + e];
        if (Track* b = ext_.find(num, e)) {
            *b = src;   // kept until compactExtTracks(), also when default again
        } else if (!extTrackIsDefault(src, st.tracks[0].lastStep)) {
            if (!ext_.alloc(num, e, src)) ok = false;
        }
    }
    if (!ok) reportExtPoolFull();
    return ok;
}

void App::assemblePattern(int num, Pattern& dst) const {
    // Audio context too (loadPattern): reads only, no allocation, O(1) per
    // track (16 x 769-byte copies).
    if (!validNum(num)) num = 1;
    const PatternStock& st = storedStock(num);
    dst.copyStockFrom(st);
    for (int e = 0; e < kExtTrackCount; ++e) {
        Track& d = dst.tracks[kStockTrackCount + e];
        if (const Track* b = ext_.find(num, e)) d = *b;
        else defaultExtTrack(d, st);
    }
}

int App::compactExtTracks(int num) {
    int freed = 0;
    const int n0 = num ? num : 1, n1 = num ? num : kPatternCount;
    if (!validNum(n0) || !validNum(n1)) return 0;
    for (int n = n0; n <= n1; ++n) {
        const uint8_t base = storedStock(n).tracks[0].lastStep;
        for (int e = 0; e < kExtTrackCount; ++e) {
            const Track* b = ext_.find(n, e);
            if (b && extTrackIsDefault(*b, base)) {
                ext_.release(n, e);
                ++freed;
            }
        }
    }
    return freed;
}

void App::clearExtTracks(int num) {
    ext_.releasePattern(num);
    if (validNum(num)) extKeep_[num] = 0;   // dropped on purpose: nothing to keep from the file
}

void App::defaultStored(int num) {
    if (!validNum(num)) return;
    clearExtTracks(num);
    defaultPattern(storedStock(num));
}

void App::resetPatternStore() {
    ext_.clear();
    std::memset(extKeep_, 0, sizeof(extKeep_));
    extFullReported_ = 0;
    for (int n = 1; n <= kPatternCount; ++n) defaultPattern(storedStock(n));
}

void App::pushFileUndo(int num) {
    if (!validNum(num)) return;
    if (Pattern* slot = fileUndo.pushSlot(uint8_t(num))) assemblePattern(num, *slot);
}

bool App::popFileUndo(uint8_t* num) {
    uint8_t n = 0;
    const Pattern* p = fileUndo.popSlot(&n);
    if (!p) return false;
    if (num) *num = n;
    storePattern(n, *p);
    return true;
}

void App::selectPattern(int num) {
    if (num < 1) num = 1;
    if (num > kPatternCount) num = kPatternCount;
    if (seq.isPlaying()) {
        // spec 06 §1.7: while playing the change is queued.
        const int base = seq.queuedPattern() ? seq.queuedPattern() : seq.currentPatternNumber();
        if (num != base) seq.queuePattern(num - base);
        ui.dirty();
        return;
    }
    if (num == patternNumber()) return;
    commitCurrentPattern();
    loadPattern(uint8_t(num), pattern(), true);
    project.setCurrentPattern(uint8_t(num));
    attachSequencer();   // re-point the sequencer (spec 06 §1.7 "0x11a34")
    markProjectDirty();
    ui.dirty();
}

void App::switchPatternNow(int num) {
    if (!seq.isPlaying()) {
        selectPattern(num);
        return;
    }
    if (num < 1) num = 1;
    if (num > kPatternCount) num = kPatternCount;
    if (num == effectivePatternNumber()) return;
    // Main loop: the store already holds every edit (patternEdited); copy
    // once more so the outgoing buffer's state is kept. The sequencer then
    // assembles the new pattern into its stage buffer (here, in the main
    // loop) and the audio context swaps it in.
    if (!seq.pendingSwitch()) commitCurrentPattern();
    seq.switchPatternNow(uint8_t(num));
    markProjectDirty();
    ui.dirty();
}

void App::loadPattern(uint8_t number, Pattern& dst, bool sync) {
    (void)sync;
    // The whole pattern set lives in memory, so loads are a copy (safe in the
    // audio-block context): stock tracks + ext blocks, no allocation.
    if (number < 1 || number > kPatternCount) number = 1;
    if (patternPresent(number)) assemblePattern(number, dst);
    else defaultPattern(dst);
    engine.noteSeqWork(audio::kSeqWholeLoad, kTrackCount);   // only counted in the audio block
}

void App::loadPatternTracks(uint8_t number, Pattern& dst, int first, int count) {
    // Audio context: tracks [first, first + count) of what loadPattern
    // writes (assemblePattern / defaultPattern), one 769-byte copy each.
    if (number < 1 || number > kPatternCount) number = 1;
    if (first < 0) first = 0;
    const int end = first + count < kTrackCount ? first + count : kTrackCount;
    if (first >= end) return;
    if (patternPresent(number)) {
        const PatternStock& st = storedStock(number);
        for (int t = first; t < end; ++t) {
            Track& d = dst.tracks[t];
            if (t < kStockTrackCount) d = st.tracks[t];
            else if (const Track* b = ext_.find(number, t - kStockTrackCount)) d = *b;
            else defaultExtTrack(d, st);
        }
    } else {
        uint16_t len = project.defaultPatternLength();
        if (len < 1 || len > kStepCount) len = 32;
        for (int t = first; t < end; ++t) {
            Track& d = dst.tracks[t];
            d.lastStep = uint8_t(len - 1);
            for (auto& s : d.steps) s = Step{};
        }
    }
    engine.noteSeqWork(audio::kSeqPreloadPart, uint32_t(end - first));   // only counted in the audio block
}

void App::pushUndo() { undo.push(pattern(), patternNumber()); }

bool App::undoEdit() {
    uint8_t num = 0;
    Pattern& cur = pattern();
    if (!undo.undo(cur, patternNumber(), &num)) {
        ui.notify("No undo data.");
        return false;
    }
    if (num != patternNumber() && !seq.isPlaying()) {
        // The snapshot belongs to another pattern: make it current (the
        // store already holds the committed state of the old one).
        project.setCurrentPattern(num);
        attachSequencer();
    }
    patternEdited();
    return true;
}

bool App::redoEdit() {
    uint8_t num = 0;
    if (!undo.redo(pattern(), patternNumber(), &num)) {
        ui.notify("No redo data.");
        return false;
    }
    patternEdited();
    return true;
}

void App::copyToClipboard(const SelRect& r) {
    *mem_.clipboard = pattern();
    clipRect_ = r;
    clipValid_ = true;
}

// ---------------------------------------------------------------------------
// Settings shortcuts

void App::setCurrentInstrument(int i) {
    if (i < 0) i = 0;
    // 2.x: synth slots 64..66 are instruments too (Tracker Mini).
    if (i >= kFirstSynthInstrument + kSynthSlotCount) i = kFirstSynthInstrument + kSynthSlotCount - 1;
    if (project.u8(pf::kCurInstrument) != i) {
        project.setU8(pf::kCurInstrument, uint8_t(i));
        markProjectDirty();
    }
}

uint8_t App::stepJump() const {
    const uint8_t v = project.u8(pf::kStepJump);
    return v > 32 ? 1 : v;
}

void App::setStepJump(int v) {
    project.setU8(pf::kStepJump, uint8_t(v < 0 ? 0 : (v > 32 ? 32 : v)));
    markProjectDirty();
}

bool App::trackMuted(int t) const {
    // project.mt: 0x12C (tracks 0..7), 0x742[0..7] (tracks 8..15; 12..15
    // as the Tracker Mini, MINI_PLAN U13).
    return project.trackMuted(t);
}

void App::setTrackMute(int t, bool m) {
    if (!project.setTrackMuted(t, m)) return;
    // Sample voices are not cut (spec 03 §2.4): the engine zeroes the dry
    // path and post-fader sends of a muted track (spec 07 Q3). Tracks 8..15
    // are MIDI only; the sequencer reads the flag at the next step.
    markProjectDirty();   // engine mute follows via applyEngineSettings
}

void App::refreshPadNotes() {
    uint8_t sc = project.u8(pf::kScale), lay = project.u8(pf::kNotesLayout), root = project.u8(pf::kRootNote);
    if (sc >= kScaleCount) sc = 0;
    if (lay > 11) lay = 11;
    if (root > 36) root = 24;
    padNotes.build(sc, lay, root);
}

uint8_t App::padNote(uint8_t pad) {
    // spec 05 §4.1, spec 06 §1.6: Beat Slice (play mode 5) maps pads to
    // 24 + pad (pad = slice); Slice mode plays the selected slice through the
    // pad layout like any instrument (manual p.126, MANUAL_AUDIT #2).
    const uint8_t ci = currentInstrument();
    if (ci < kSampleInstrumentCount && instruments[ci].playMode == uint8_t(PlayMode::BeatSlice))
        return uint8_t(24 + (pad > 47 ? 47 : pad));
    refreshPadNotes();
    return padNotes.note(pad);
}

bool App::noteInScale(uint8_t note) { return isInScale(note, project.u8(pf::kRootNote), project.u8(pf::kScale)); }

uint8_t App::instrumentVolume(uint8_t instrument) {
    return instrument < kSampleInstrumentCount ? instruments[instrument].volume : 100;
}

uint8_t App::recordVolume(uint8_t instrument) const {
    // IV for the live-record Volume FX (spec 03 §5.2, spec 17 §1.4 step 7):
    // 0..127 like liveVolume(); instrumentVolume()'s 0..100 (the sequencer's
    // roll volume) recorded a full-velocity note on a volume-100 instrument
    // as V79 (MANUAL_AUDIT_2026-10-23 #1).
    return liveVolume(instrument, 127);
}

void App::markInstrumentDirty(int i, bool sample) {
    if (i < 0 || i >= kSampleInstrumentCount) return;
    instrHeaderDirty_[i] = true;
    if (sample) instrSampleDirty_[i] = true;
    unsaved_ = true;
}

// ---------------------------------------------------------------------------
// Transport

void App::togglePlay(bool shift) {
    if (seq.isPlaying() || seq.isArmed()) {
        seq.stop();
        allVoicesOff();
    } else if (shift) {
        seq.playSong(project.playlistPos);
    } else {
        seq.userPlay(0);
    }
    ui.dirty();
}

// ---------------------------------------------------------------------------
// Paths

void App::workspacePath(char* out, int cap, const char* rel) const {
    std::snprintf(out, size_t(cap), "%s/%s", kWorkspace, rel);
}

void App::projectPath(char* out, int cap) const {
    std::snprintf(out, size_t(cap), "%s/%s", project.folder[0] ? project.folder : kProjectsDir, project.name);
}

// ---------------------------------------------------------------------------
// Autosave (spec 02 §1.1)


// ---------------------------------------------------------------------------
// Config persistence (spec 05 §5.4, spec 01 §10) and the per-project
// recorder settings (spec 02 §3.3 0x134..0x13C).

bool App::saveConfig() {
    if (hal_.nvstore && cfg.save(*hal_.nvstore)) return true;
    // No store, or the store refused (EEPROM not ready): keep it on the card.
    return hal_.storage && cfg.save(fs());
}

void App::loadConfig() {
    if (hal_.nvstore && cfg.load(*hal_.nvstore)) return;
    // EEPROM empty / not ours: import an OpenTracker config file once (an
    // older build kept the settings there), else defaults.
    if (!hal_.storage || !cfg.load(fs())) cfg.setDefaults();
    saveConfig();
}

void App::syncRecorderSettings() {
    uint8_t* p = &project.values[pf::kRecSource - mt::kValuesOff];
    constexpr int kN = int(sizeof(recShadow_));   // 0x134..0x13F (incl. the f32 at 0x13C)
    auto toProject = [&]() {
        p[0] = cfg.recSource;
        p[1] = cfg.recGain[0];   // line
        p[2] = cfg.recGain[2];   // mic low gain
        p[3] = cfg.recGain[3];   // mic high gain
        p[4] = cfg.recGain[1];   // radio
        // spec 22 §5: the project byte is stock's recorder +0x7E, 0 = On,
        // 1 = Off (cfg.recMonitor is the UI's 1 = On).
        p[5] = cfg.recMonitor ? 0 : 1;
        project.setF32(pf::kRadioFreq, float(cfg.radioFreq10) / 10.0f);
    };
    if (!recShadowValid_ || std::memcmp(p, recShadow_, kN) != 0) {
        // Project loaded / created: project -> working copies. An all-zero
        // block with the 87.5 MHz default is a project that never stored
        // recorder settings: it takes OpenTracker's defaults (README).
        bool unset = p[1] == 0 && p[2] == 0 && p[3] == 0 && p[4] == 0 && p[5] == 0 && p[0] == 0;
        if (unset) {
            Config d;
            cfg.recSource = d.recSource;
            std::memcpy(cfg.recGain, d.recGain, sizeof(cfg.recGain));
            cfg.recMonitor = d.recMonitor;
        } else {
            cfg.recSource = p[0] > 3 ? 0 : p[0];
            cfg.recGain[0] = p[1];
            cfg.recGain[2] = p[2];
            cfg.recGain[3] = p[3];
            cfg.recGain[1] = p[4];
            cfg.recMonitor = p[5] == 0 ? 1 : 0;   // stock: any non-zero = Off
        }
        const float f = project.f32(pf::kRadioFreq);
        cfg.radioFreq10 = (f >= 76.0f && f <= 108.0f) ? uint16_t(f * 10.0f + 0.5f) : 875;
        cfg.sanitize();
        if (unset) toProject();
        std::memcpy(recShadow_, p, kN);
        recShadowValid_ = true;
        return;
    }
    // Working copies edited (recorder screen): -> project, project changed.
    uint8_t want[kN];
    std::memcpy(want, p, kN);
    toProject();
    if (std::memcmp(want, p, kN) != 0) {
        std::memcpy(recShadow_, p, kN);
        projectDirty_ = unsaved_ = true;
    }
}

}  // namespace ot
