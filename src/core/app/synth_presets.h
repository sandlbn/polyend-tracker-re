// The project's synth presets: kSynthPresetCount patches shared by the slots,
// played by the pattern command Z. In SDRAM, read when a project opens, so a
// switch during playback never touches the card. Engine 0 = an empty entry.
// The device passes its place in the SDRAM layout (app_main.cpp) to bind() before
// App::init(); elsewhere (tests, the simulator) the table is a static one.
#pragma once

#include <cstdint>

#include "model/fx.h"
#include "model/synth_patch.h"

namespace ot {
namespace presets {

void bind(SynthPatch* storage);   // kSynthPresetCount entries
SynthPatch* table();   // [0] is preset 1
uint8_t& dirty();      // bit n - 1: preset n's file is to be written or removed

}  // namespace presets
}  // namespace ot
