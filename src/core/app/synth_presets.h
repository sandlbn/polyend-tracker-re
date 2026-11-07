// The project's synth presets: kSynthPresetCount patches shared by the slots,
// played by the pattern command Z. In SDRAM, read when a project opens, so a
// switch during playback never touches the card. Engine 0 = an empty entry.
#pragma once

#include <cstdint>

#include "model/fx.h"
#include "model/synth_patch.h"

namespace ot {
namespace presets {

SynthPatch* table();   // [0] is preset 1
uint8_t& dirty();      // bit n - 1: preset n's file is to be written or removed

}  // namespace presets
}  // namespace ot
