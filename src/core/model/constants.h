// Data-model constants (spec 02 §4.3, §8.2).
#pragma once

#include <cstdint>

#include "model/fx.h"

namespace ot {

// 2.x (docs/M1_PLAN.md §1): a pattern has 16 tracks. Tracks 0..11 are the
// stock tracks (pattern_NN.mtp image, spec 02 §4.3); tracks 12..15 are the
// extension tracks kept in the ext-track pool (model/pattern_store.h). Tracks
// 8..15 are MIDI tracks.
constexpr int kTrackCount = 16;            // tracks per (working) pattern, 2.x
constexpr int kStockTrackCount = 12;       // tracks per stock 1.9.x .mtp / stock pattern (older files: 8), spec 02 §4.3
constexpr int kMiniTrackCount = 16;        // tracks per Tracker Mini .mtp (0x3030), spec 21 §4
constexpr int kExtTrackCount = kTrackCount - kStockTrackCount;   // 4 extension tracks (13..16)
constexpr int kMidiTrackStart = 8;         // first MIDI-only track (no sample voice)
constexpr int kLegacyTrackCount = 8;       // pre-v5 / 6184-byte patterns, spec 02 §4.4
constexpr int kStepCount = 128;            // max steps per track, spec 02 §4.3
constexpr int kFxPerStep = 2;              // FX columns per step, spec 02 §4.3
constexpr int kDefaultTrackLastStep = 31;  // 32 steps, spec 02 §4.2

constexpr int kPatternCount = 255;         // pattern numbers 1..255, 0 = empty slot, spec 02 §4.3
constexpr int kSongSlots = 255;            // playlist length, spec 02 §3.2

constexpr int kSampleInstrumentCount = 48; // instruments 0..47 (files 01..48), spec 02 §4.3
constexpr int kMidiInstrumentCount = 16;   // instruments 48..63, spec 02 §4.3
constexpr int kInstrumentCount = kSampleInstrumentCount + kMidiInstrumentCount;
constexpr int kFirstMidiInstrument = kSampleInstrumentCount;
// Tracker Mini 2.2.1 (spec 21 §4.1): instrument bytes 64, 65, 66 are synth
// slots 1..3. Values above 66 are not valid instruments.
constexpr int kFirstSynthInstrument = kInstrumentCount;   // 64
constexpr int kSynthSlotCount = 3;
constexpr int kLastSynthInstrument = kFirstSynthInstrument + kSynthSlotCount - 1;   // 66
// [3.x] Instrument bytes 67..74 play the synth presets P1..P8 (OpenTracker v3;
// other firmware reads them as invalid instruments: silent). A note of one
// borrows a synth slot for that preset (audio/synth_bank.h).
constexpr int kFirstPresetInstrument = kLastSynthInstrument + 1;   // 67
constexpr int kPresetInstrumentCount = 8;
constexpr int kLastPresetInstrument = kFirstPresetInstrument + kPresetInstrumentCount - 1;   // 74
static_assert(kPresetInstrumentCount == kSynthPresetCount, "one preset instrument per synth preset");

// Step note sentinels (spec 02 §4.2). 0..127 are note numbers.
constexpr int8_t kNoteEmpty = -1;
constexpr int8_t kNoteFade = -2;
constexpr int8_t kNoteCut = -3;
constexpr int8_t kNoteOff = -4;
constexpr int8_t kNoteMax = 127;

inline bool isSampleInstrument(uint8_t i) { return i < kSampleInstrumentCount; }
inline bool isMidiInstrument(uint8_t i) { return i >= kSampleInstrumentCount && i < kInstrumentCount; }
// [3.x] Synth instruments: the slots S1..S3 (64..66) and the presets P1..P8
// (67..74). Only a slot instrument names a slot (synthSlotOf, -1 otherwise).
inline bool isSynthSlotInstrument(uint8_t i) { return unsigned(i) - kFirstSynthInstrument < unsigned(kSynthSlotCount); }
inline bool isPresetInstrument(uint8_t i) { return unsigned(i) - kFirstPresetInstrument < unsigned(kPresetInstrumentCount); }
inline bool isSynthInstrument(uint8_t i) {
    return unsigned(i) - kFirstSynthInstrument < unsigned(kSynthSlotCount + kPresetInstrumentCount);
}
inline int synthSlotOf(uint8_t i) { return isSynthSlotInstrument(i) ? int(i) - kFirstSynthInstrument : -1; }
inline uint8_t presetOfInstrument(uint8_t i) { return isPresetInstrument(i) ? uint8_t(i - kFirstPresetInstrument + 1) : 0; }

// Sample bank (spec 02 §8.2).
constexpr uint32_t kSampleBankBytes = 0xB40000;   // 11 796 480 bytes
constexpr uint32_t kSampleRate = 44100;

}  // namespace ot
