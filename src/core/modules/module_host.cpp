// Module host (modules/module.h).
#include "modules/module.h"

#include "audio/inserts.h"
#include "audio/synth_engine.h"
#include "model/synth_patch.h"
#include "modules/synth_descriptor.h"

#include <cstring>

#include "ot_profile.h"
#include "sdram_bss.h"

#ifndef OT_PROFILE_SDRAM_BYTES
#define OT_PROFILE_SDRAM_BYTES 0
#endif

namespace ot {
namespace mod {

namespace {
#if OT_PROFILE_SDRAM_BYTES > 0
// [3.x] E1: the profile modules' SDRAM block (NOLOAD, zeroed by otAppMain()).
OT_SDRAM_BSS alignas(16) uint8_t gModuleSdram[OT_PROFILE_SDRAM_BYTES];
#endif
ModuleHost gHost;   // constant-initialised
}  // namespace

ModuleHost::SdramArena ModuleHost::profileSdram() {
#if OT_PROFILE_SDRAM_BYTES > 0
    return SdramArena{gModuleSdram, uint32_t(OT_PROFILE_SDRAM_BYTES), 0};
#else
    return SdramArena{nullptr, 0, 0};
#endif
}

void* ModuleHost::sdram(uint32_t bytes) {
    if (!bytes || !sdram_.base || noSdram_) return nullptr;
    const uintptr_t base = reinterpret_cast<uintptr_t>(sdram_.base);
    const uintptr_t at = (base + sdram_.used + 15u) & ~uintptr_t(15u);
    const uint64_t end = uint64_t(at - base) + bytes;
    if (end > sdram_.size) return nullptr;
    sdram_.used = uint32_t(end);
    return reinterpret_cast<void*>(at);
}

ModuleHost& ModuleHost::instance() { return gHost; }

void ModuleHost::init() {
    if (inited_) return;
    inited_ = true;
    if (!sdram_.base) sdram_ = profileSdram();
    for (int i = 0; kModuleFactories[i]; ++i) {
        IModule& m = kModuleFactories[i]();
        const ModuleInfo& in = m.info();
        bool ok = in.api == kModuleApi && n_ < kMaxModules && !find(in.id) && descriptorsOk(m) && effectsOk(m) &&
                  m.attach(*this);
        if (!ok) {
            std::memcpy(refused_, in.id, sizeof(refused_));
            refused_[4] = '\0';
            continue;
        }
        mods_[n_++] = &m;
        for (int i = 0; const SynthDescriptor* d = m.synthDescriptor(i); ++i)
            if (nDesc_ < kMaxEngines) desc_[nDesc_++] = d;
    }
#if defined(OT_SLOT_INSERTS) && OT_SLOT_INSERTS
    // The core's reservation in the same block (tools/profile.py sizes it).
    insertLines_ = sdram(uint32_t(audio::kSynthSlots) * audio::kEffectWorkMax);
#endif
}

bool ModuleHost::descriptorsOk(const IModule& m) const {
    int add = 0;
    for (int i = 0; const SynthDescriptor* d = m.synthDescriptor(i); ++i) {
        if (!d->name || !d->name[0] || d->paramCount > kPatchMaxParams || !d->params || !d->ui) return false;
        if (d->volumeParam >= int(d->paramCount)) return false;
        if (d->priorityParam >= int(d->paramCount)) return false;
        if (d->extCount > kPatchExtMax || (d->extCount && !d->ext)) return false;
        // SynthParams holds both: the Mini parameters, then the extension ones.
        if (int(d->paramCount) + int(d->extCount) > audio::kSynthParamMax) return false;
        for (int k = 0; k < nDesc_; ++k)
            if (desc_[k]->engineId == d->engineId) return false;
        ++add;
    }
    return nDesc_ + add <= kMaxEngines;
}

bool ModuleHost::effectsOk(IModule& m) {
    for (int i = 0; audio::IEffect* fx = m.effect(i); ++i) {
        const char* id = fx->id();
        if (!id || std::strlen(id) != 4 || !fx->name() || !fx->name()[0]) return false;
        if (fx->stateBytes() > audio::kEffectStateMax || fx->workBytes() > audio::kEffectWorkMax) return false;
        if (fx->paramCount() < 1 || fx->paramCount() > audio::kEffectMaxParams) return false;
        if (effectById(id)) return false;
        for (int j = 0; j < i; ++j)
            if (std::strncmp(m.effect(j)->id(), id, 4) == 0) return false;
    }
    return true;
}

int ModuleHost::effectCount() {
    if (!inited_) init();
    int n = 0;
    for (int i = 0; i < n_; ++i)
        for (int j = 0; mods_[i]->effect(j); ++j) ++n;
    return n;
}

audio::IEffect* ModuleHost::effectAt(int k) {
    if (!inited_) init();
    if (k < 0) return nullptr;
    for (int i = 0; i < n_; ++i)
        for (int j = 0; audio::IEffect* fx = mods_[i]->effect(j); ++j)
            if (k-- == 0) return fx;
    return nullptr;
}

audio::IEffect* ModuleHost::effectById(const char* id) {
    if (!inited_) init();
    if (!id) return nullptr;
    for (int i = 0; i < n_; ++i)
        for (int j = 0; audio::IEffect* fx = mods_[i]->effect(j); ++j)
            if (std::strncmp(fx->id(), id, 4) == 0) return fx;
    return nullptr;
}

int ModuleHost::pageCount() {
    if (!inited_) init();
    int n = 0;
    for (int i = 0; i < n_; ++i) n += mods_[i]->page() != nullptr;
    return n;
}

IModulePage* ModuleHost::pageAt(int k) {
    if (!inited_) init();
    for (int i = 0; i < n_; ++i)
        if (IModulePage* p = mods_[i]->page()) {
            if (k == 0) return p;
            --k;
        }
    return nullptr;
}

int ModuleHost::masterInserts(audio::IMasterInsert** out, int cap) {
    if (!inited_) init();
    int n = 0;
    for (int i = 0; i < n_ && n < cap; ++i)
        if (audio::IMasterInsert* ins = mods_[i]->masterInsert()) out[n++] = ins;
    return n;
}

audio::IMasterInsert* ModuleHost::masterInsert() {
    if (!inited_) init();
    for (int i = 0; i < n_; ++i)
        if (audio::IMasterInsert* ins = mods_[i]->masterInsert()) return ins;
    return nullptr;
}

bool ModuleHost::addForTest(IModule& m) { return addLoaded(m); }

void ModuleHost::removeLoaded(IModule& m) {
    int i = 0;
    while (i < n_ && mods_[i] != &m) ++i;
    if (i == n_) return;
    for (int k = i; k + 1 < n_; ++k) mods_[k] = mods_[k + 1];
    mods_[--n_] = nullptr;
    int w = 0;
    for (int k = 0; k < nDesc_; ++k) {
        bool mine = false;
        for (int j = 0; const SynthDescriptor* d = m.synthDescriptor(j); ++j) mine |= d == desc_[k];
        if (!mine) desc_[w++] = desc_[k];
    }
    for (int k = w; k < nDesc_; ++k) desc_[k] = nullptr;
    nDesc_ = w;
}

// [I] A module loaded from the card (modules/module_files.h): the
// checks init() makes, after the compiled-in modules.
bool ModuleHost::addLoaded(IModule& m, bool onDemand) {
    if (!inited_) init();
    const ModuleInfo& in = m.info();
#if defined(OT_POOL_TEST) && OT_POOL_TEST
    // [I] measurement images: a card module replaces the compiled-in one with
    // its id, so one image measures a synth from flash and from the pool.
    for (int i = 0; i < n_; ++i) {
        if (std::strncmp(mods_[i]->info().id, in.id, 4) != 0) continue;
        IModule* old = mods_[i];
        for (int k = i; k + 1 < n_; ++k) mods_[k] = mods_[k + 1];
        --n_;
        int w = 0;
        for (int k = 0; k < nDesc_; ++k) {
            bool mine = false;
            for (int j = 0; const SynthDescriptor* d = old->synthDescriptor(j); ++j) mine |= d == desc_[k];
            if (!mine) desc_[w++] = desc_[k];
        }
        nDesc_ = w;
        break;
    }
#endif
    bool ok = in.api == kModuleApi && n_ < kMaxModules && !find(in.id) && descriptorsOk(m) && effectsOk(m);
    if (ok) {
        noSdram_ = onDemand;
        ok = m.attach(*this);
        noSdram_ = false;
    }
    if (!ok) {
        std::memcpy(refused_, in.id, sizeof(refused_));
        refused_[4] = '\0';
        return false;
    }
    mods_[n_++] = &m;
    for (int i = 0; const SynthDescriptor* d = m.synthDescriptor(i); ++i)
        if (nDesc_ < kMaxEngines) desc_[nDesc_++] = d;
    return true;
}

const SynthDescriptor* ModuleHost::synthDescriptor(int32_t engineId) {
    if (!inited_) init();
    for (int k = 0; k < nDesc_; ++k)
        if (desc_[k]->engineId == engineId) return desc_[k];
    return nullptr;
}

int ModuleHost::engineCount() {
    if (!inited_) init();
    return nDesc_;
}

const SynthDescriptor* ModuleHost::engineAt(int k) {
    if (!inited_) init();
    return k >= 0 && k < nDesc_ ? desc_[k] : nullptr;
}

// Modules renamed after projects were saved with their old id (the
// sidecar's module list and project sections keep the id they were saved
// with): old id -> current id.
constexpr const char* kRenamed[][2] = {
    {"JUNO", "BBDC"},   // the BBD chorus insert, renamed before 3.0
};

const IModule* ModuleHost::find(const char* id) const {
    for (int i = 0; i < n_; ++i)
        if (std::strncmp(mods_[i]->info().id, id, 4) == 0) return mods_[i];
    for (const auto& r : kRenamed)
        if (std::strncmp(r[0], id, 4) == 0) return find(r[1]);
    return nullptr;
}

audio::ISynthEngine* ModuleHost::synthEngine(int32_t engineId) {
    if (!inited_) init();
    for (int i = 0; i < n_; ++i)
        if (audio::ISynthEngine* e = mods_[i]->synthEngine(engineId)) return e;
    return nullptr;
}

}  // namespace mod
}  // namespace ot
