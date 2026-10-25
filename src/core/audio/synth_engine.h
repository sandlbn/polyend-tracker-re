// Synth engine interface (2.x M3, docs/MINI_PLAN.md §3, §4 WP-E) [OT].
//
// The framework (synth_bank.h) owns slots, the voice pool, macros, pitch FX,
// gain and mixing. An engine is the DSP of one Tracker Mini model (spec 21
// §8: ACD, FAT, VAP, WTFM, PERC). It is a stateless singleton: every voice
// keeps its state in SynthVoice::state, every slot in SynthSlotScratch.
//
// Contract for engines (WP-F):
//  - All calls come from the audio context: no allocation, no I/O, no locks.
//  - Parameters are the spec 21 §8 stored values (engineering units, enum
//    indices as floats), already clamped to their range and with macros
//    M1..M6 applied (SynthParams). Read p.end[i] at block rate, or ramp
//    p.start[i] -> p.end[i] across the block for continuous parameters
//    (p.moving says whether any value changed this block).
//  - The engine's "Volume" parameter (patchVolumeParam) and the Volume FX /
//    Macro 7 are applied by the framework through the gain ramp passed to
//    render(): engines must NOT apply Volume themselves.
//  - Pitch: SynthNote::pitch is the note in semitones (MIDI numbering, 69 =
//    A 440 Hz; synthPitchHz). Patch glide (Glide Mode / Glide Time) is the
//    engine's job, using SynthNote::overlap; step-FX pitch modulation (Slide,
//    Glide FX, Tune, Micro-tune) arrives in SynthVoice::bend (semitones,
//    ramp bendStart -> bend over the block). Sounding pitch = the engine's
//    (glided) note pitch + bend. A note with a Glide FX (SynthNote::fxGlide)
//    skips the patch glide: one mechanism owns the start pitch.
//  - render() ADDS kBlock frames into L and R (the slot bus, before slot gain
//    / pan / sends), multiplied by g0 + i * gInc, and returns false once the
//    voice is silent for good (the framework frees it). It also stores the
//    amplitude-envelope level (0..1) in SynthVoice::level for voice stealing.
#pragma once

#include <cstddef>
#include <cstdint>

#include "audio/audio_tables.h"
#include "model/synth_patch.h"

namespace ot {
namespace audio {

constexpr int kSynthSlots = 3;                 // Synth 1..3 (instruments 64..66)
constexpr int kSynthMaxVoices = 8;             // pool records; Config budget 1..8
constexpr int kSynthDefaultVoices = 4;         // OPTIMIZATION_2X item 10 (6 after OP-3..5)
constexpr int kSynthParamMax = kPatchMaxParams;   // 84 (PERC)
constexpr int kSynthVoiceStateBytes = 128;     // engine state per voice (SRAM)
constexpr int kSynthSlotScratchBytes = 384;    // engine state per slot (SRAM)
// Internal test engine id (not a Tracker Mini id, never written to a patch).
constexpr int32_t kSynthEngineTest = 0x7E;

// Effective parameters of one slot for the current block.
struct SynthParams {
    int32_t engine = 0;
    int count = 0;                    // parameters of the engine
    bool moving = false;              // some start[i] != end[i] this block
    uint32_t serial = 0;              // changes whenever an end[] value changes
    float start[kSynthParamMax] = {}; // value at the block start
    float end[kSynthParamMax] = {};   // value at the block end (the target)
    // Ramp helper: start value and per-frame increment of parameter i.
    float ramp(int i, float& inc) const {
        inc = (end[i] - start[i]) * (1.0f / float(kBlock));
        return start[i];
    }
};

// One note start.
struct SynthNote {
    float pitch = 60.0f;   // semitones; the note (Tune FX not included: see bend)
    float velocity = 1.0f; // Macro 7 value 0..1 (Volume FX / 100; 1 without)
    uint8_t note = 60;     // 0..127
    // True when a still-gated voice of a polyphony-1 slot is reused for the
    // new note (legato / "Overlap" glide modes). A reused voice that was in
    // its release has overlap = false and reused = true.
    bool overlap = false;
    bool reused = false;
    // A Glide step FX (19) owns this note's pitch start: the framework's bend
    // starts at the track's previous pitch, so the engine must not add its
    // patch glide on top (it starts at `pitch`).
    bool fxGlide = false;
};

// Per-slot engine scratch (per-block coefficient caches, PERC drum state).
struct alignas(8) SynthSlotScratch {
    // Zeroed: engines may keep fields across slotReset() (PERC's noise seed)
    // and must never read what the memory held before.
    uint8_t bytes[kSynthSlotScratchBytes] = {};
    template <class T>
    T& as() {
        static_assert(sizeof(T) <= kSynthSlotScratchBytes, "slot state too large");
        return *reinterpret_cast<T*>(bytes);
    }
};

// One pool voice as an engine sees it. Fields above `state` are written by
// the framework (read-only for engines) except `level`.
struct alignas(8) SynthVoice {
    SynthSlotScratch* slotScratch = nullptr;   // the owning slot's scratch
    float bend = 0.0f;         // step-FX pitch offset at the block end (semitones)
    float bendStart = 0.0f;    // ... at the block start
    float level = 0.0f;        // engine: amplitude-envelope level 0..1
    uint8_t slot = 0;          // 0..2
    uint8_t note = 0;
    bool gate = false;         // key held (false after noteOff)
    // Framework: this block is the voice's last (stolen / cut: the gain ramp
    // falls to 0 and the record is freed after it). Engines that keep state
    // beyond the voice (PERC's kit) release it here.
    bool fading = false;
    alignas(8) uint8_t state[kSynthVoiceStateBytes];
    template <class T>
    T& as() {
        static_assert(sizeof(T) <= kSynthVoiceStateBytes, "voice state too large");
        return *reinterpret_cast<T*>(state);
    }
};

class ISynthEngine {
public:
    virtual ~ISynthEngine() = default;
    virtual int32_t id() const = 0;            // SynthEngineId
    virtual const char* name() const = 0;      // "ACD", ...
    // Once per block for every slot with sounding voices, before its voices
    // render (shared coefficients into the slot scratch). Optional.
    virtual void prepare(SynthSlotScratch& slot, const SynthParams& p) { (void)slot; (void)p; }
    // A slot starts using this engine (patch / model change, project load).
    virtual void slotReset(SynthSlotScratch& slot) { (void)slot; }
    // Song tempo in BPM (the sequencer's smoothed tempo), once per block
    // before prepare() for every slot with sounding voices: tempo-synced
    // LFOs / delays follow the song. Optional.
    virtual void setTempo(float bpm) { (void)bpm; }
    // Per-voice send outputs (PERC per-drum Delay / Reverb sends). Called
    // once per block before the slot's voices render, and with nullptr after
    // them. delay / reverb are the main mono send buses (kBlock floats,
    // nullptr = that send is off); gainD / gainR are the slot gain x the
    // Global Mixer delay / reverb level for that block. The engine adds its
    // own per-voice send amounts, pre slot pan. Returns false when the engine
    // has no per-voice sends (the framework then never calls it again for
    // this block). Optional.
    virtual bool setSendTap(int slot, float* delay, float* reverb, float gainD, float gainR) {
        (void)slot; (void)delay; (void)reverb; (void)gainD; (void)gainR;
        return false;
    }
    // Start a note. A fresh voice has zeroed state; a reused one (n.reused)
    // keeps its state (continue envelopes / glide from where they are).
    virtual void noteOn(SynthVoice& v, const SynthParams& p, const SynthNote& n) = 0;
    // Gate off: enter the release.
    virtual void noteOff(SynthVoice& v, const SynthParams& p) = 0;
    // Add one block into L / R (see the contract above). false = finished.
    virtual bool render(SynthVoice& v, const SynthParams& p, float g0, float gInc, float* L, float* R) = 0;
};

// Engine registry keyed by the Tracker Mini engine id (synth_registry.cpp).
// nullptr = the model is not available in this build (FAT until 2.1, WTFM until 2.2):
// such a slot loads and saves its patch but stays silent.
ISynthEngine* synthEngineFor(int32_t engineId);
// Tests: report an engine id as "not available" (every Mini model has an
// engine since 2.2; the read-only "not available" path stays testable).
void synthEngineHide(int32_t engineId, bool hide);

// INTERNAL sine + envelope engine (tests, benches, sim until WP-F).
ISynthEngine* synthTestEngine();

// Note pitch (semitones, 69 = 440 Hz) -> Hz.
float synthPitchHz(float pitch);

}  // namespace audio
}  // namespace ot
