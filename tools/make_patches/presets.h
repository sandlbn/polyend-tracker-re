// OpenTracker synth preset pack (tools/make_patches, PRESETS.md): our own
// ACD / FAT / VAP / WTFM / PERC patches, each built from the engine's built-in Init patch
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
struct Preset {
    ot::SynthModel model;
    const char* name;   // "OT ...", also the file name
    const char* desc;   // one line for PRESETS.md
    int playPoly;       // suggested slot polyphony (tests render with it)
    std::vector<Edit> edits;
    std::vector<Macro> macros;   // M1..M6
};

// Every preset of the pack, ACD first, then FAT, VAP, WTFM, PERC.
const std::vector<Preset>& all();

// Slot s on the preset's model with the Init patch, the edits and macros M1..M6
// applied; the slot record's patch name and macro page values follow. False
// (with err) when an edit or a macro is out of range.
bool build(ot::App& app, int s, const Preset& p, std::string& err);

// build() then Save As: Patches/<MODEL>/<name>.patch.
bool buildAndSave(ot::App& app, int s, const Preset& p, std::string& err);

// PRESETS.md text (the list of every preset with its macros).
std::string markdown();

}  // namespace presets
