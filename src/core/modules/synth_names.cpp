// Enum names shared by several synth descriptors (modules/synth_descriptor.h).
#include "modules/synth_descriptor.h"

namespace ot {
namespace synthui {

namespace names {
const char* const kFilter15[15] = {"LP MG 24dB",  "LP OB 24dB",  "LP OB 12dB",   "LP SVF 24dB",   "LP SVF 12dB",
                                   "HP OB 24dB",  "HP OB 12dB",  "HP SVF 24dB",  "HP SVF 12dB",   "BP OB 24dB",
                                   "BP OB 12dB",  "BP SVF 24dB", "BP SVF 12dB",  "Notch SVF 24dB", "Notch SVF 12dB"};
const char* const kWave[6] = {"Triangle", "Sine", "Ramp Up", "Ramp Down", "Square", "Random S&H"};
const char* const kRetrig[3] = {"Free Running", "One Shot", "Note"};
const char* const kGlide[4] = {"Always", "Overlap", "Legato", "Legato Overlap"};
const char* const kModSrc[5] = {"Amp Env", "Env 1 (Fltr)", "Env 2", "LFO 1", "LFO 2"};
}  // namespace names

}  // namespace synthui
}  // namespace ot
