// Sequencer <-> audio engine / app interfaces (spec 03 §6).
//
// ISequencerSink is what the sequencer drives (sample voices, metronome,
// MIDI out). ISequencerHost is what the sequencer asks of the app (pattern
// loading, callbacks, instrument data). Both are plain virtual interfaces,
// called from the audio-block context: implementations must not block or
// allocate.
#pragma once

#include <cstdint>

#include "hal_api.h"
#include "model/pattern.h"

namespace ot {

// Note-off modes; values equal the step note sentinels (spec 02 §4.2).
enum class NoteOffMode : int8_t {
    Fade = -2,  // fast fixed fade
    Cut = -3,   // immediate stop
    Off = -4,   // normal envelope release
};

// Argument block for ISequencerSink::voiceNoteOn (spec 03 §6 voiceNoteOn).
struct VoiceNoteOn {
    uint8_t instrument = 0;   // 0..47
    uint8_t note = 0;         // 0..127
    int8_t volume = -1;       // -1 = instrument default, else 0..100 (apply as FX 18)
    StepFx fxA;               // step bytes +2/+3 -> engine slot 0
    StepFx fxB;               // step bytes +4/+5 -> engine slot 1
    float tickPeriodUs = 0;   // current tick period (Slide FX 41/42 rate)
};

// Argument block for ISequencerSink::synthNoteOn (2.x M3, docs/MINI_PLAN.md §3):
// a step with instrument 64..66 (Synth 1..3) on any of the 16 tracks.
struct SynthNoteOn {
    uint8_t slot = 0;         // 0..2 = instrument 64..66
    uint8_t note = 0;         // 0..127 (performance transpose applied)
    int8_t volume = -1;       // -1 = none (Macro 7 full), else 0..100 (Volume / Random Volume FX)
    StepFx fxA;               // step FX columns (Slide, Glide, Tune, Micro-tune, Volume)
    StepFx fxB;
    float tickPeriodUs = 0;
};

// MIDI output port mask; bit values follow the Clock/Transport/MIDI-out
// settings (spec 03 §4.1): 1 = port A, 2 = port B, 3 = both.
// Port A is taken to be USB and port B the DIN jack (spec 03 §7 item 1, I).
constexpr uint8_t kMidiPortA = 1;
constexpr uint8_t kMidiPortB = 2;

class ISequencerSink {
public:
    virtual ~ISequencerSink() = default;

    // One sample voice per track 0..7 (spec 03 §6).
    virtual void voiceNoteOn(uint8_t track, const VoiceNoteOn& n) = 0;
    virtual void voiceNoteOff(uint8_t track, NoteOffMode mode) = 0;
    // Undo the previous FX in `slot`, then apply (type, value); type 0/1 = undo only.
    virtual void voiceApplyFx(uint8_t track, uint8_t slot, uint8_t type, uint8_t value) = 0;
    virtual void resetAllVoiceFx() = 0;
    // Once per audio block; 4-block moving average of the effective tempo.
    virtual void engineBlockUpdate(float smoothedTempoBpm) = 0;
    // Built-in click; accent velocity 100, normal 45 (spec 03 §2.5).
    virtual void metronomeClick(bool accent, uint8_t velocity) = 0;
    // Complete MIDI message (1..3 bytes) for the ports in portMask.
    virtual void midiSend(uint8_t portMask, const uint8_t* bytes, uint8_t len) = 0;

    // 2.x synth slots (docs/MINI_PLAN.md §3; audio/synth_bank.h). Track 0..15.
    // A new note on a track releases that track's previous synth note (or
    // reuses the voice of a polyphony-1 slot: legato / glide).
    virtual void synthNoteOn(uint8_t track, const SynthNoteOn& n) { (void)track; (void)n; }
    virtual void synthNoteOff(uint8_t track, NoteOffMode mode) { (void)track; (void)mode; }
    // Unconsumed step FX on an empty step of a track whose synth note sounds.
    virtual void synthApplyFx(uint8_t track, uint8_t type, uint8_t value) { (void)track; (void)type; (void)value; }
    // FX CC a..f (macro 0..5) on a synth step: override of the slot's page
    // value until Stop.
    virtual void synthMacroFx(uint8_t slot, uint8_t macro, uint8_t value) { (void)slot; (void)macro; (void)value; }
    // [OT] Z: slot plays preset n (1..8) or its own patch (0) until the next
    // Z, a loop end or Stop. Runs before the same step's synthNoteOn. False:
    // refused (an empty preset, out of range), the slot plays on as it was;
    // a repeat of the slot's current preset counts as taken.
    virtual bool synthPreset(uint8_t slot, uint8_t n) { (void)slot; (void)n; return false; }
    // [OT] Whether synthPreset(slot, n) would take n (0 always): asked from
    // the audio context, before the Z that sends it.
    virtual bool synthPresetTakes(uint8_t n) { (void)n; return false; }
    // Transport stop: macro overrides return to the page values.
    virtual void synthTransportStop() {}
};

// Routes a sink MIDI message onto the HAL ports (A = USB, B = DIN).
inline void routeMidi(IMidiPort& port, uint8_t portMask, const uint8_t* bytes, uint8_t len) {
    if (portMask & kMidiPortA) port.send(MidiPortId::kUsb, bytes, len);
    if (portMask & kMidiPortB) port.send(MidiPortId::kDin, bytes, len);
}

class ISequencerHost {
public:
    virtual ~ISequencerHost() = default;

    // Load pattern `number` (1..255) into `dst`. Called one step before the
    // switch (preload, sync=false: may complete asynchronously within that
    // step) or synchronously for playSong (sync=true). Spec 03 §2.2.
    virtual void loadPattern(uint8_t number, Pattern& dst, bool sync) { (void)number; (void)dst; (void)sync; }
    // Spread preload (OpenTracker CPU, docs/OPTIMIZATION_2X.md round 3): a
    // host that returns true here gets the preload (sync = false) as
    // loadPatternTracks() calls instead of the one loadPattern call: tracks
    // [first, first + count) of pattern `number`, a few per audio block
    // during the preload step and the rest at the pattern end. Together they
    // must produce what loadPattern(number, dst, false) produces. (A whole
    // pattern copied SDRAM -> SDRAM in one audio block costs ~140k cycles on
    // the device.)
    virtual bool loadsPatternTracks() const { return false; }
    virtual void loadPatternTracks(uint8_t number, Pattern& dst, int first, int count) {
        (void)number; (void)dst; (void)first; (void)count;
    }
    // Master step changed (UI notification, spec 03 §1.4).
    virtual void onStepChanged() {}
    // Pattern-end callbacks (seq+0x540 / +0x544, spec 03 §2.2).
    virtual void onPatternEnd() {}
    virtual void onPatternAdvanced() {}
    // Export hooks: when an export is installed, Stop / Tempo-STP finalise it.
    virtual bool exportActive() { return false; }
    virtual void finaliseExport() {}
    // Project scale test used by Random Note (spec 03 §3.6 id 5).
    virtual bool noteInScale(uint8_t note) { (void)note; return true; }
    // Default instrument volume 0..100 (roll volume ramp base, spec 03 §3.5).
    virtual uint8_t instrumentVolume(uint8_t instrument) { (void)instrument; return 100; }
    // Pattern edited by step entry / live recording (spec 03 §5).
    virtual void markPatternDirty() {}
    // External Start chooses song play when the Song screen is active (spec 03 §4.3).
    virtual bool songScreenActive() { return false; }
    // [OT] Z chase, main loop only (never the audio block): what the Z
    // commands before a start position leave the synth slots on. Song mode:
    // playlist positions 0..songPos-1, then steps 0..lastStepInclusive of
    // `pattern`; pattern mode: those steps only (-1 = none). preset[s] comes
    // in as 0 (own patch) and lastInstr[t] as 0xFF (no note seen): the
    // default leaves both, the app fills in what the pattern data says.
    virtual void synthChase(bool songMode, int songPos, uint8_t pattern, int lastStepInclusive,
                            uint8_t preset[kSynthSlotCount], uint8_t lastInstr[kTrackCount]) {
        (void)songMode; (void)songPos; (void)pattern; (void)lastStepInclusive; (void)preset; (void)lastInstr;
    }
};

}  // namespace ot
