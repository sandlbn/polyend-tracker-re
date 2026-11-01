// OpenTracker synth preset pack (tools/make_patches, PRESETS.md): our own
// ACD / FAT / VAP / WTFM / PERC patches and (3.x) JU6, D-Type, CS-Type, A-Type and T-Type module engine patches, each built from the engine's built-in Init patch
// through the App (no factory patch, no file from the card is read) and
// written with the App's own Save As path (App::saveSynthLibraryPatch), so
// the files are byte for byte what the firmware writes:
// Patches/<MODEL>/<name>.patch.
//
// Shared by the generator (main.cpp) and tests/patches_test.cpp.
#pragma once

#include <string>
#include <vector>

#include "app/app.h"

namespace presets {

// Parameter edit on top of the Init patch (engine parameter index, value in
// the stored range of formats/patch_pb.cpp).
struct Edit {
    int index;
    float value;
};
// Macro target: amount x stored range per unit of the macro (synth_params.h).
struct Target {
    int param;
    float amount;
};
struct Macro {
    const char* name;
    bool bipolar;   // bipolar: 63 = the patch as saved; unipolar: 0 = as saved
    std::vector<Target> targets;
};
// The JU6 module engine (opentracker-synths modules/ju6, module JU6S).
constexpr int32_t kJu6Engine = 0x81;
// The D-Type module engine (opentracker-synths modules/dtype, module DTY1, Patches/DTYPE/).
constexpr int32_t kDtypeEngine = 0x82;
// The CS-Type module engine (modules/cstype, module CST1, Patches/CSTYPE/).
constexpr int32_t kCstypeEngine = 0x83;
// The A-Type module engine (modules/atype, module ATY1, Patches/ATYPE/).
constexpr int32_t kAtypeEngine = 0x84;
// The T-Type drum module engine (modules/ttype, module TTY1, Patches/TTYPE/): one patch = one kit.
constexpr int32_t kTtypeEngine = 0x85;

struct Preset {
    ot::SynthModel model;   // the Mini model (a module engine: the slot record's fallback)
    const char* name;   // "OT ...", also the file name
    const char* desc;   // one line for PRESETS.md
    int playPoly;       // suggested slot polyphony (tests render with it)
    std::vector<Edit> edits;
    std::vector<Macro> macros;   // M1..M6
    int32_t engine = 0;          // a module engine (0x80..0xFF), 0 = the model's own engine
    std::vector<Target> velocity = {};   // more M7 (Velocity) targets after Init's Volume (synth_params.h law)
};

// Every preset of the pack, ACD first, then FAT, VAP, WTFM, PERC, JU6, D-Type, CS-Type, A-Type, T-Type:
// the models whose modules are in the build profile.
const std::vector<Preset>& all();

// The preset's engine id and its folder name (Patches/<folder>/).
int32_t engineOf(const Preset& p);
const char* folderOf(const Preset& p);

// Slot s on the preset's model with the Init patch, the edits and macros M1..M6
// applied; the slot record's patch name and macro page values follow. False
// (with err) when an edit or a macro is out of range.
bool build(ot::App& app, int s, const Preset& p, std::string& err);

// build() then Save As: Patches/<MODEL>/<name>.patch.
bool buildAndSave(ot::App& app, int s, const Preset& p, std::string& err);

// PRESETS.md text (the list of every preset with its macros).
std::string markdown();

}  // namespace presets
