// A module's settings page (docs/PLAN_3X_M2.md M-C): shown in the Master key
// cycle after the Global Mixer. Up to 8 rows, one above each F-key: F1-F8
// select a row, the encoder and Up / Down change it. Module-facing: no
// drawing code; the core draws value bars and the bottom-bar labels.
#pragma once

#include <cstdint>

namespace ot {
namespace mod {

struct ModuleRow {
    char label[16];    // bottom-bar label ("Drive")
    char value[16];    // bottom-bar value ("30", "On")
    float frac;        // bar position 0..1 (bipolar: -1..1)
    bool bipolar;
};

class IModulePage {
public:
    virtual const char* title() const = 0;   // "Tape" (title bar: "Master Tape")
    virtual int rows() const = 0;            // 1..8
    virtual void row(int i, ModuleRow& out) = 0;
    virtual void adjust(int i, int detents) = 0;   // main loop; mark the module's data dirty
};

}  // namespace mod
}  // namespace ot
