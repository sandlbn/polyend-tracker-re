// The synth model list of the Synth page (F1 Model): the five Tracker Mini
// models in Mini order (index 0..4 = the Mini record's model, whether or not
// this build has the engine), then the module engines with ids 0x80..0xFF in
// module order (docs/PLAN_3X_M2.md M-A). A slot on a module engine keeps a
// Mini model in its record (the fallback) and names the engine in the project
// sidecar (App::setSlotEngine).
#pragma once

#include <cstdint>

namespace ot {

constexpr int kMaxSynthModels = 5 + 16;

int synthModelListCount();
int32_t synthModelListEngine(int i);       // engine id of list entry i (0 past the end)
const char* synthModelListName(int i);     // "ACD" .. "PERC", a module engine's display name
int synthModelListIndex(int32_t engineId); // -1 when not in the list
// Folder name of an engine: Patches/<name>/ ("?" when unknown).
const char* synthEngineFolder(int32_t engineId);
// Name of an engine on screen: the descriptor's displayName when it has one,
// else the folder name ("?" when unknown).
const char* synthEngineDisplayName(int32_t engineId);

}  // namespace ot
