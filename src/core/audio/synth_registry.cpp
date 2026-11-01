// Synth engine registry (synth_engine.h): Tracker Mini engine id -> engine.
//
// The engines are modules (modules/module.h): the build profile picks which
// ones are compiled in; an engine id no module serves is "not available".
#include <cmath>

#include "audio/synth_engine.h"
#include "modules/module.h"

namespace ot {
namespace audio {

ISynthEngine* synthTestEngine();   // synth_test_engine.cpp (internal)

namespace {
uint32_t gHidden = 0;   // tests: engine ids reported as "not available"
}

void synthEngineHide(int32_t engineId, bool hide) {
    if (engineId < 0 || engineId >= 32) return;
    gHidden = hide ? (gHidden | (1u << engineId)) : (gHidden & ~(1u << engineId));
}

ISynthEngine* synthEngineFor(int32_t engineId) {
    if (gHidden && engineId >= 0 && engineId < 32 && (gHidden & (1u << engineId))) return nullptr;
    if (engineId == kSynthEngineTest) return synthTestEngine();
    return mod::ModuleHost::instance().synthEngine(engineId);
}

float synthPitchHz(float pitch) { return 440.0f * std::exp2((pitch - 69.0f) * (1.0f / 12.0f)); }

}  // namespace audio
}  // namespace ot
