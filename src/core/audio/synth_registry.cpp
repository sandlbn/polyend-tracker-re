// Synth engine registry (synth_engine.h): Tracker Mini engine id -> engine.
//
// WP-F: add each engine here (one line per id) as it lands.
#include <cmath>

#include "audio/engines/acd.h"
#include "audio/engines/fat.h"
#include "audio/engines/perc.h"
#include "audio/engines/vap.h"
#include "audio/engines/wtfm.h"
#include "audio/synth_engine.h"

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
    switch (engineId) {
        case kSynthEngineAcd:
            return synthAcdEngine();
        case kSynthEngineFat:
            return fat::engine();
        case kSynthEngineVap:
            return vap::engine();
        case kSynthEnginePerc:
            return percEngine();
        case kSynthEngineWtfm:
            return wtfm::engine();
        case kSynthEngineTest:
            return synthTestEngine();
        default:
            return nullptr;
    }
}

float synthPitchHz(float pitch) { return 440.0f * std::exp2((pitch - 69.0f) * (1.0f / 12.0f)); }

}  // namespace audio
}  // namespace ot
