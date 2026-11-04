// In-memory sample instrument (spec 02 §6.2) and MIDI instrument (spec 02 §3.3).
#pragma once

#include <cstdint>

#include "model/constants.h"

namespace ot {

enum class SampleType : uint8_t { WaveFile = 0, Wavetable = 1 };

enum class PlayMode : uint8_t {
    OneShot = 0,
    ForwardLoop = 1,
    BackwardLoop = 2,
    PingPongLoop = 3,
    Slice = 4,
    BeatSlice = 5,
    Wavetable = 6,
    Granular = 7,
};

enum class FilterType : uint8_t { LowPass = 0, HighPass = 1, BandPass = 2 };
enum class LfoShape : uint8_t { RevSaw = 0, Saw = 1, Triangle = 2, Square = 3, Random = 4 };
enum class GrainShape : uint8_t { Square = 0, Triangle = 1, Gauss = 2 };
enum class GranularLoop : uint8_t { Forward = 0, Backward = 1, PingPong = 2 };

// Automation targets; index into Instrument::env / Instrument::lfo.
enum AutomationTarget : uint8_t {
    kAutoVolume = 0,
    kAutoPanning = 1,
    kAutoCutoff = 2,
    kAutoWavetablePos = 3,
    kAutoGranularPos = 4,
    kAutoFinetune = 5,
    kAutomationCount = 6,
};

constexpr int kSliceCount = 48;
constexpr int kInstrumentNameLen = 31;  // max chars, spec 02 §6.2 body 0x05

struct Envelope {
    float amount = 1.0f;
    uint16_t delayMs = 0;
    uint16_t attackMs = 0;
    uint16_t holdMs = 0;
    uint16_t decayMs = 0;
    float sustain = 1.0f;
    uint16_t releaseMs = 1000;
    uint8_t loop = 0;      // "use LFO instead of envelope"
    uint8_t enabled = 0;
};

struct Lfo {
    uint8_t shape = uint8_t(LfoShape::Triangle);
    uint8_t speed = 0;     // 0..28 (128 bars .. 1/64)
    float amount = 0.5f;
};

// Sample instrument parameters (the .pti body). Field defaults follow the
// firmware new-instrument initialiser (spec 02 §6.2 "fw default").
struct Instrument {
    uint8_t active = 1;
    uint8_t sampleType = uint8_t(SampleType::WaveFile);
    char name[kInstrumentNameLen + 1] = {};  // NUL-terminated
    uint32_t lengthFrames = 0;
    uint16_t wavetableWindowSize = 2048;
    uint32_t wavetableWindowCount = 0;
    uint32_t reserved38 = 0;
    uint8_t playMode = uint8_t(PlayMode::OneShot);
    uint16_t startPoint = 0;
    uint16_t loopPoint1 = 1;
    uint16_t loopPoint2 = 65534;
    uint16_t endPoint = 65535;
    uint32_t wavetableCurrentWindow = 0;
    Envelope env[kAutomationCount];
    Lfo lfo[kAutomationCount];
    float cutoff = 1.0f;
    float resonance = 0.0f;
    uint8_t filterType = uint8_t(FilterType::LowPass);
    uint8_t filterEnabled = 0;
    int8_t tune = 0;
    int8_t finetune = 0;
    uint8_t volume = 50;
    int16_t panning = 50;
    uint8_t delaySend = 0;
    uint16_t slices[kSliceCount] = {};
    uint8_t sliceCount = 0;
    uint8_t selectedSlice = 0;
    uint16_t grainLength = 441;
    uint16_t granularPosition = 0;
    uint8_t grainShape = uint8_t(GrainShape::Square);
    uint8_t granularLoop = uint8_t(GranularLoop::Forward);
    uint8_t reverbSend = 0;
    uint8_t overdrive = 0;
    uint8_t bitDepth = 16;
    uint8_t reserved173 = 0;

    // Runtime sample state (not stored in the body).
    bool stereo = false;

    Instrument() { setDefault(); }

    void setDefault() {
        *this = Instrument(NoInit{});
        // env0 enabled, attack 0; env1..5 disabled, attack 3000 (spec 02 §6.2).
        for (int i = 0; i < kAutomationCount; ++i) {
            env[i] = Envelope{};
            env[i].attackMs = i == 0 ? 0 : 3000;
            env[i].enabled = i == 0 ? 1 : 0;
            lfo[i] = Lfo{};
        }
    }

private:
    struct NoInit {};
    explicit Instrument(NoInit) {}
};

// MIDI instrument record (16 bytes each in project.mt at 0x4FD, spec 02 §3.3).
struct MidiInstrument {
    uint8_t channel = 0;
    uint8_t velocity = 127;
    uint8_t cc[6] = {1, 5, 7, 11, 64, 128};  // CC numbers for MIDI CC FX a..f
    uint8_t extra[8] = {};
};

// A default Instrument (the fw new-instrument initialiser), built on first
// use; one shared copy (SDRAM on the device, main loop only).
const Instrument& defaultInstrument();

}  // namespace ot
