// Step FX ids, names and value ranges (spec 02 §4.3).
//
// Names/symbols/ranges originate from Polyend tracker-lib types/patterns.ts
// (MIT, see third_party/LICENSES/tracker-lib.txt) as tabulated in spec 02.
#pragma once

#include <cstdint>

namespace ot {

enum class FxId : uint8_t {
    None = 0,
    Off = 1,
    MicroMove = 2,
    Roll = 3,
    Chance = 4,
    RandomNote = 5,
    RandomInstrument = 6,
    RandomVolume = 7,
    MidiCcA = 8,
    MidiCcB = 9,
    MidiCcC = 10,
    MidiCcD = 11,
    MidiCcE = 12,
    BreakPattern = 13,
    MidiChord = 14,
    Tempo = 15,
    RandomFxValue = 16,
    Swing = 17,
    Volume = 18,
    Glide = 19,
    GateLength = 20,
    Arp = 21,
    Position = 22,
    VolumeLfo = 23,
    PanningLfo = 24,
    Slice = 25,
    ReversePlayback = 26,
    LowPass = 27,
    HighPass = 28,
    BandPass = 29,
    DelaySend = 30,
    Panning = 31,
    ReverbSend = 32,
    FinetuneLfo = 33,
    MicroTune = 34,
    FilterLfo = 35,
    PositionLfo = 36,
    MidiCcF = 37,
    Overdrive = 38,
    BitDepth = 39,
    Tune = 40,
    SlideUp = 41,
    SlideDown = 42,
};

constexpr int kFxCount = 43;

// [3.x] Z, the synth preset command. A step stores it as FX type 0 ("none" in
// every firmware) with value kFxPresetCmdBase + n, n = 0 (the slot's own patch)
// to kSynthPresetCount. kFxSynthPreset is its place in the FX lists and tables;
// it is never written as a stored type byte.
constexpr uint8_t kFxSynthPreset = 43;
constexpr int kFxListCount = kFxCount + 1;
constexpr uint8_t kFxPresetCmdBase = 0xD0;
constexpr int kSynthPresetCount = 8;
constexpr bool isPresetValue(uint8_t value) {
    return value >= kFxPresetCmdBase && value <= kFxPresetCmdBase + kSynthPresetCount;
}
constexpr bool isPresetCmd(uint8_t type, uint8_t value) { return type == 0 && isPresetValue(value); }
constexpr uint8_t presetCmdIndex(uint8_t value) { return uint8_t(value - kFxPresetCmdBase); }
constexpr uint8_t fxLogicalType(uint8_t type, uint8_t value) { return isPresetCmd(type, value) ? kFxSynthPreset : type; }
constexpr uint8_t fxLogicalValue(uint8_t type, uint8_t value) { return isPresetCmd(type, value) ? presetCmdIndex(value) : value; }

struct FxInfo {
    char symbol;          // single-character column symbol
    const char* name;     // menu name
    uint8_t minValue;     // stored-value range (inclusive)
    uint8_t maxValue;
    uint8_t defaultValue;
};

// Info for an FX id of the lists; ids >= kFxListCount return the "None" entry.
const FxInfo& fxInfo(uint8_t id);
inline const FxInfo& fxInfo(FxId id) { return fxInfo(static_cast<uint8_t>(id)); }
inline bool fxIdValid(uint8_t id) { return id < kFxCount; }

// Firmware menu (display) order of ids, spec 02 §4.3.
extern const uint8_t kFxMenuOrder[kFxListCount];

}  // namespace ot
