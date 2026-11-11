// A module's settings page (docs/PLAN_3X_M2.md M-C): shown in the Master key
// cycle after the Global Mixer. Up to 8 rows, one above each F-key: F1-F8
// select a row, the encoder and Up / Down change it. Module-facing: no
// drawing code; the core draws value bars and the bottom-bar labels.
#pragma once

#include <cstdint>
#include <cstring>

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

// [I] An effect's page starts with its on / mode row: value "Off" = off
// ("On" / "Off"; BBD "I" / "II" / "Off"), the first step up turns it on
// (the FX bank, fork #21).
inline bool pageOn(IModulePage* p) {
    if (!p) return false;
    ModuleRow r{};
    p->row(0, r);
    return std::strcmp(r.value, "Off") != 0;
}
inline void pageTurn(IModulePage* p, bool on) {
    if (p && pageOn(p) != on) p->adjust(0, on ? 1 : -100);
}

}  // namespace mod
}  // namespace ot
