// Synth model list (modules/synth_models.h).
#include "modules/synth_models.h"

#include "formats/patch_pb.h"
#include "model/project.h"
#include "modules/module.h"
#include "modules/module_files.h"
#include "modules/synth_descriptor.h"

namespace ot {

namespace {
struct ListEngine {
    int32_t id;
    const char* name;
};

// The k-th module engine with an id >= 0x80: the compiled-in ones, then
// [I] the card's (modules/module_files.h, listed whether loaded or not; a
// loaded one keeps its place). {0, nullptr} past the end.
ListEngine moduleEngine(int k) {
    mod::ModuleHost& h = mod::ModuleHost::instance();
    for (int i = 0; i < h.engineCount(); ++i) {
        const mod::SynthDescriptor* d = h.engineAt(i);
        if (d->engineId < 0x80 || mod::cardEngine(d->engineId)) continue;
        if (k-- == 0) return {d->engineId, d->displayName ? d->displayName : d->name};
    }
    if (const mod::CardEngine* e = mod::cardEngineAt(k)) return {e->engineId, e->displayName};
    return {0, nullptr};
}
}  // namespace

int synthModelListCount() {
    int n = kSynthModelCount;
    while (moduleEngine(n - kSynthModelCount).id) ++n;
    return n;
}

int32_t synthModelListEngine(int i) {
    if (i < 0) return 0;
    if (i < kSynthModelCount) return synthEngineId(SynthModel(i));
    return moduleEngine(i - kSynthModelCount).id;
}

const char* synthModelListName(int i) {
    if (i >= 0 && i < kSynthModelCount) return synthModelName(SynthModel(i));
    const ListEngine e = moduleEngine(i - kSynthModelCount);
    return e.name ? e.name : "?";
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
    if (d) return d->displayName ? d->displayName : d->name;
    if (const mod::CardEngine* e = mod::cardEngine(engineId)) return e->displayName;   // [I] not loaded yet
    return synthEngineFolder(engineId);
}

}  // namespace ot
