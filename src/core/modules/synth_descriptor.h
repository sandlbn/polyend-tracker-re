// Synth descriptor: what a synth module tells the core about its engine
// (docs/PLAN_3X_M2.md M-A). The core keeps the Tracker Mini format knowledge
// (model index 0..4 <-> engine ids 1, 2, 4, 5, 6 <-> folder names) and the
// generic Synth pages; a module brings its parameter table (stored ranges,
// defaults), the Edit Patch labels, value formats, enum names and sections.
// Module-facing: only <cstdint> and model/synth_patch.h.
#pragma once

#include <cstdint>

#include "model/synth_patch.h"

namespace ot {
namespace synthui {

// Value display / step kinds of one engine parameter.
enum Fmt : uint8_t {
    kFmtPct,      // v * 100 "%"
    kFmtVal100,   // v * 100, no unit
    kFmtSec,      // seconds
    kFmtHz,       // log frequency (cutoff)
    kFmtLfoHz,    // log frequency, two decimals
    kFmtEnum,     // names[]
    kFmtOnOff,
    kFmtSemi,     // semitones
    kFmtCents,
    kFmtInt,      // plain integer (index without known labels)
    kFmtMs,
    kFmtDb,
    kFmtNote,     // MIDI note name (PERC pitch)
    kFmtPan,      // -1..1 -> L100..R100
    kFmtPctMax,   // v / max(|min|, |max|) * 100 % (Resonance 0..1.5, Pitch LFO / Mod Amt bipolar)
    kFmtMs10,     // stored 0..100 shown as 0..10 ms (PERC Body / Snap Attack)
    kFmtPercNote, // PERC pitch 12..119 as C0..B8 (manual p.166)
    kFmtBodyWave, // PERC Body Wave 0..175 = 16 waves x 0..100 % (manual p.166)
    kFmtRatio,    // WTFM Ratio index 0..47 shown as 0.25 .. 12.00 (spec 2x-08 §1)
};
struct ParamUi {
    const char* label;
    uint8_t fmt;
    const char* const* names;   // kFmtEnum
    uint8_t nameCount;
};
// A section of the Edit Patch page: parameter indices in display order, or
// macro m (0..6) when macro >= 0.
struct Section {
    const char* name;       // grid cell ("Osc")
    const char* longName;   // F6 label ("Oscillator")
    const uint8_t* params;
    uint8_t count;
    int8_t macro;
};
constexpr int kMaxSections = 16;   // 4 x 4 grid (pads 0-3, 12-15, 24-27, 36-39)

// Enum names shared by several engines (defined in modules/synth_names.cpp).
namespace names {
extern const char* const kFilter15[15];   // the 15 filter types (VAP, WTFM)
extern const char* const kWave[6];        // LFO waveforms
extern const char* const kRetrig[3];      // LFO retrigger
extern const char* const kGlide[4];       // glide modes
extern const char* const kModSrc[5];      // mod matrix sources
}  // namespace names

}  // namespace synthui

namespace mod {

enum : uint8_t {
    // The engine keeps state beyond a voice (PERC kit): a voice stolen in the
    // block it started is not freed at once.
    kSynthFlagKitState = 1u << 0,
    // [3.x] Poly 1 note priority (docs/PLAN_CLASSIC.md C1): a new note sounds
    // only when it wins (priorityParam: 0 Last, 1 Low, 2 High; Last without
    // it), a losing one is held silently, and the release of the sounding
    // note returns the voice to the winner of the notes still held. Without
    // the flag a Poly 1 slot plays last-note and releases at the note-off.
    kSynthFlagNotePriority = 1u << 1,
};

struct SynthDescriptor {
    int32_t engineId;                   // Tracker Mini id 1, 2, 4, 5, 6, or 0x80..0xFF (module engines)
    const char* name;                   // 1..8 chars [A-Z0-9]: Patches/<name>/, UI, telemetry
    uint8_t flags;
    int8_t volumeParam;                 // Volume parameter (macro 7 target of the Init patch), -1 none
    uint8_t paramCount;                 // <= kPatchMaxParams
    const PatchParamRange* params;      // paramCount entries
    const synthui::ParamUi* ui;         // paramCount + extCount entries; label nullptr = not shown
    const synthui::Section* sections;   // Edit Patch sections, macros M1..M7 last
    uint8_t sectionCount;               // <= synthui::kMaxSections
    // [OT] extension parameters (patch field 16, model/synth_patch.h): index
    // paramCount + k, not macro targets. The first one, when present, is the
    // slot's Width (SynthBank widens the slot bus); kParamFlagSpread marks the
    // slot's Spread (SynthBank, a place per note); the rest are the engine's.
    uint8_t extCount;                   // <= kPatchExtMax
    const PatchParamRange* ext;         // extCount entries
    // [3.x] The name on the Synth page (model list, header, Model field) when
    // it differs from `name` ("D-Type" for DTYPE); nullptr = `name`. The
    // folder, the telemetry and the patch files keep `name`.
    const char* displayName = nullptr;
    // [3.x] With kSynthFlagNotePriority: the parameter that picks the
    // priority (0 Last, 1 Low, 2 High; macros may move it); -1 = Last.
    int8_t priorityParam = -1;
};

}  // namespace mod
}  // namespace ot

// Helpers for descriptor tables (module sources).
#define OT_SYNTH_ENUM(n) ::ot::synthui::kFmtEnum, n, uint8_t(sizeof(n) / sizeof(n[0]))
#define OT_SYNTH_FMT(f) ::ot::synthui::f, nullptr, 0
#define OT_SYNTH_SECTION(name, longName, arr) {name, longName, arr, uint8_t(sizeof(arr)), -1}
#define OT_SYNTH_MACRO_SECTIONS                                                                     \
    {"M1", "Macro 1", nullptr, 0, 0}, {"M2", "Macro 2", nullptr, 0, 1}, {"M3", "Macro 3", nullptr, 0, 2}, \
        {"M4", "Macro 4", nullptr, 0, 3}, {"M5", "Macro 5", nullptr, 0, 4}, {"M6", "Macro 6", nullptr, 0, 5}, \
        {"M7", "Macro 7", nullptr, 0, 6}
