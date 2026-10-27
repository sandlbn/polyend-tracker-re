// Tracker Mini `.patch` codec: a minimal protobuf wire reader/writer for the
// nanopb Patch message (spec 21 §7; docs/MINI_PLAN.md §1). Pure: no storage,
// no allocation.
//
// Writer rules (match the stock encoder byte for byte):
//  - fields in field-number order; unknown fields kept raw, in file order,
//    merged in front of the first known field with a higher number;
//  - proto3 omission for singular scalars (int 0, fixed32 bits 0, empty
//    string, false); presence-flagged ParamValue.value is written when set;
//  - fixed-count arrays (names[4], macros[8], targets[6]) always fully written;
//  - Extra (field 9) is regenerated as the constant block of §7.2.
#pragma once

#include <cstddef>
#include <cstdint>

#include "model/synth_patch.h"

namespace ot {

constexpr size_t kPatchMaxFileSize = 8192;  // larger files are refused
constexpr size_t kPatchExtraSize = 1293;    // Extra payload length (derived from §7.2)

enum class PatchStatus : uint8_t {
    kOk = 0,
    kTooLarge,        // input over kPatchMaxFileSize
    kTruncated,       // a field runs past the end of its buffer
    kBadVarint,       // varint longer than 10 bytes / overflows 64 bits
    kBadWireType,     // wire type 3, 4, 6 or 7
    kBadField,        // field number 0, or a known field with the wrong wire type
    kBadValue,        // int32 out of range
    kTooManyParams,   // more than kPatchMaxParams params
    kTooManyEntries,  // more than 4 names / 8 macros / 6 targets
    kNameTooLong,     // name slot > kPatchNameMax or macro name > kPatchMacroNameMax
    kOpaqueOverflow,  // unknown fields exceed kPatchOpaqueMax bytes
    kBufferTooSmall,  // encode: output capacity too small
};
const char* patchStatusText(PatchStatus s);

// Decode `len` bytes into *out (fully reset to proto3 zero values first).
// Unknown fields inside known sub-messages are skipped, as nanopb does.
PatchStatus decodePatch(const uint8_t* data, size_t len, SynthPatch* out);

// Encode into out[cap]; *written receives the byte count. Never exceeds
// kPatchMaxFileSize for a patch that decodePatch accepted or patchInit built.
PatchStatus encodePatch(const SynthPatch& p, uint8_t* out, size_t cap, size_t* written);

// The constant Extra payload (kPatchExtraSize bytes, without tag/length).
void patchExtraPayload(uint8_t* out);

// Byte span [*beg, *end) of the first top-level field `field` (tag included)
// of an encoded patch. False when absent or the walk fails.
bool patchFieldSpan(const uint8_t* data, size_t len, uint32_t field, size_t* beg, size_t* end);

// ---- engine tables and validation (spec 21 §8 + MINI_PLAN corrections) ----

// Parameter table of a known engine, or nullptr (count 0).
const PatchParamRange* patchEngineParams(int32_t engine, int* count);
// [OT] Extension parameter table of an engine (Width; FAT also Stereo), or
// nullptr (count 0). Extension parameter k has index (Mini count + k).
const PatchParamRange* patchEngineExtParams(int32_t engine, int* count);
const char* patchEngineName(int32_t engine);  // "ACD".."PERC", or nullptr
int patchVolumeParam(int32_t engine);         // Volume index, -1 for PERC/unknown

// Build the engine's Init patch: every parameter present at its default
// (WTFM 6/7 absent, as in every factory WTFM patch), M1-6 "Macro n" empty,
// M7 "Velocity" -> Volume +1 (none for PERC), M8 "Pressure" empty (U15).
// Unused targets are {0, 0} like the factory files. False for an unknown engine.
bool patchInit(int32_t engine, SynthPatch* out);

// True when v is finite and inside the stored range of parameter `index`.
bool patchParamInRange(int32_t engine, int index, float v);

struct PatchIssue {
    enum Kind : uint8_t {
        kEngine,        // unknown engine id
        kParamCount,    // paramCount != engine parameter count
        kParamRange,    // params[index] present and out of range / not finite
        kMacroCount,    // macros[index].count outside 0..6
        kTargetParam,   // macros[index].targets[sub].param not -1 / valid index
        kTargetAmount,  // macros[index].targets[sub] amount outside -2..2 / not finite
        kNameLen,       // macros[index] name over 32 bytes (constructed patches only)
    };
    Kind kind;
    int16_t index;
    int8_t sub;
};

// Collect up to `cap` issues into out (may be nullptr); returns the total count.
int validatePatch(const SynthPatch& p, PatchIssue* out, int cap);

}  // namespace ot
