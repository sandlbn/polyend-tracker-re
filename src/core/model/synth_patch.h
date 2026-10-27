// Tracker Mini synth patch model (spec 21 §7, §8; docs/MINI_PLAN.md §1).
//
// One `.patch` file = one SynthPatch. Values are kept exactly as stored: each
// parameter is the raw f32 bit pattern plus its presence flag, macro amounts
// are raw f32 bits, and unknown top-level fields (2-5, 10, 11, ...) are kept
// as raw wire bytes in file order so a decode/encode round trip is
// byte-exact. The codec lives in formats/patch_pb.*; this header is plain
// data with no dependencies.
#pragma once

#include <cstdint>
#include <cstring>

namespace ot {

enum SynthEngineId : int32_t {
    kSynthEngineNone = 0,
    kSynthEngineAcd = 1,
    kSynthEngineFat = 2,
    kSynthEngineVap = 4,
    kSynthEngineWtfm = 5,
    kSynthEnginePerc = 6,
};

constexpr int kPatchMaxParams = 84;      // PERC has the most (spec 21 §8.1)
constexpr int kPatchNameSlots = 4;       // Patch.names[4]
constexpr int kPatchNameMax = 256;       // bytes per name slot (NameSlot.text <= 256, spec 21 §7.1)
constexpr int kPatchMacros = 8;          // Patch.macros[8]
constexpr int kPatchMacroNameMax = 32;   // Macro.name
constexpr int kPatchMacroTargets = 6;    // Macro.targets[6]
constexpr int kPatchOpaqueMax = 256;     // raw unknown top-level fields
constexpr int kPatchMacroVelocity = 6;   // M7 "Velocity", driven by Volume FX
constexpr int kPatchMacroPressure = 7;   // M8 "Pressure", stored only
// [OT] Extension parameters (2.x stereo: Width, FAT Stereo): settings the
// Mini does not have, indexed after the engine's Mini parameters and stored
// in their own top-level field 16 (`repeated ParamValue ot_ext`, written
// after Extra, only when one is set). The Mini skips the field; 2.0-2.2 keep
// it as an unknown field. Absent = default.
constexpr int kPatchExtMax = 8;
constexpr uint32_t kPatchExtField = 16;

struct PatchParam {
    uint32_t bits;   // IEEE-754 single, exactly as stored
    bool present;    // ParamValue.value presence flag (absent = parameter default)
};

struct PatchTarget {
    // Engine parameter index. Only targets[0..count) are live; the factory
    // writer leaves the rest {0, 0} (not -1 as spec 21 §7.2 says). -1 = none.
    int32_t param;
    uint32_t amountBits;  // f32 amount, -2..+2
};

struct PatchMacro {
    uint8_t nameLen;
    char name[kPatchMacroNameMax + 1];  // NUL-terminated copy; nameLen is authoritative
    bool bipolar;
    int32_t count;                      // target_count as stored (0..6 when valid)
    PatchTarget targets[kPatchMacroTargets];
};

struct PatchName {
    uint16_t len;
    char text[kPatchNameMax + 1];
};

struct SynthPatch {
    int32_t engine;
    uint16_t paramCount;                // number of Patch.params entries
    PatchParam params[kPatchMaxParams];
    PatchName names[kPatchNameSlots];
    PatchMacro macros[kPatchMacros];
    bool extraPresent;                  // field 9; always regenerated as the constant block
    uint8_t extCount;                   // number of ot_ext (field 16) entries
    PatchParam ext[kPatchExtMax];       // [OT] extension parameters (see kPatchExtField)
    uint16_t opaqueLen;                 // bytes used in opaque[]
    uint8_t opaque[kPatchOpaqueMax];    // unknown top-level fields, raw (tag + payload), file order
};

inline float patchBitsToFloat(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
inline uint32_t patchFloatToBits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

// Stored range and default of one engine parameter (spec 21 §8 with the
// MINI_PLAN corrections). Values outside [min, max] take `def` on load.
struct PatchParamRange {
    float def;
    float min;
    float max;
    uint8_t flags = 0;  // kParamFlag*
};
constexpr uint8_t kParamFlagNoRange = 1;     // range unknown: any finite value (WTFM 6, 7; U6)
constexpr uint8_t kParamFlagInitAbsent = 2;  // Init patch leaves the entry absent

}  // namespace ot
