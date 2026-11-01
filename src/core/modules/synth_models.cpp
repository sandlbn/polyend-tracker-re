// Synth model list (modules/synth_models.h).
#include "modules/synth_models.h"

#include "formats/patch_pb.h"
#include "model/project.h"
#include "modules/module.h"
#include "modules/synth_descriptor.h"

namespace ot {

namespace {
// The k-th module engine with an id >= 0x80, or nullptr.
const mod::SynthDescriptor* moduleEngine(int k) {
    mod::ModuleHost& h = mod::ModuleHost::instance();
    for (int i = 0; i < h.engineCount(); ++i) {
        const mod::SynthDescriptor* d = h.engineAt(i);
        if (d->engineId < 0x80) continue;
        if (k == 0) return d;
        --k;
    }
    return nullptr;
}
}  // namespace

int synthModelListCount() {
    int n = kSynthModelCount;
    while (moduleEngine(n - kSynthModelCount)) ++n;
    return n;
}

int32_t synthModelListEngine(int i) {
    if (i < 0) return 0;
    if (i < kSynthModelCount) return synthEngineId(SynthModel(i));
    const mod::SynthDescriptor* d = moduleEngine(i - kSynthModelCount);
    return d ? d->engineId : 0;
}

const char* synthModelListName(int i) {
    if (i >= 0 && i < kSynthModelCount) return synthModelName(SynthModel(i));
    const mod::SynthDescriptor* d = moduleEngine(i - kSynthModelCount);
    return d ? (d->displayName ? d->displayName : d->name) : "?";
}

int synthModelListIndex(int32_t engineId) {
    const int n = synthModelListCount();
    for (int i = 0; i < n; ++i)
        if (synthModelListEngine(i) == engineId) return i;
    return -1;
}

const char* synthEngineFolder(int32_t engineId) {
    const char* n = patchEngineName(engineId);
    return n ? n : "?";
}

const char* synthEngineDisplayName(int32_t engineId) {
    const mod::SynthDescriptor* d = mod::ModuleHost::instance().synthDescriptor(engineId);
    return d && d->displayName ? d->displayName : synthEngineFolder(engineId);
}

}  // namespace ot
