// Compiled-in modules (docs/PLAN_3X.md §2, §7).
//
// A module is code from a module repository (e.g. opentracker-synths) that the
// build profile (reimpl/profiles/<name>.toml) compiles into the image. There is
// no runtime loading: tools/profile.py generates module_registry.cpp, a
// constant table of factories, and ModuleHost walks it once at App::init().
//
// Rules for module code:
//  - no static constructors (namespace-scope objects must be constant-initialised);
//  - no heap; state lives in the host's structures (SynthVoice::state) or in
//    fixed static buffers declared in ModuleInfo;
//  - only the core headers a module needs: modules/module.h, audio/synth_engine.h,
//    audio/dsp.h, audio/bbd_chorus.h (the DSP BBDC and JU6 share), audio/va_dsp.h,
//    audio/chip_dsp.h and audio/fm_dsp.h (the retro engines' and LoFi's DSP),
//    audio/ring_io.h (block spans to and from the SDRAM), fastrun.h (API major
//    kModuleApi);
//  - [3.x] SDRAM (ModuleHost::sdram(), from attach()): the audio block may use it
//    only as contiguous spans moved once per block with audio/ring_io.h (ringIn /
//    ringOut / spanIn / spanOut), never one access per sample (an uncached miss
//    costs tens of cycles).
#pragma once

#include <cstdint>

namespace ot {
namespace audio {
class ISynthEngine;
class IMasterInsert;
class IEffect;
}
namespace mod {
struct SynthDescriptor;   // modules/synth_descriptor.h
class IModulePage;        // modules/module_ui.h

// Bump on any incompatible change of this header or the hook interfaces it names
// (modules compile with the core: a defaulted virtual appended to a hook
// interface, such as ISynthEngine::slotPost, is not one).
constexpr uint16_t kModuleApi = 1;

enum : uint32_t {
    kCapSynthEngine = 1u << 0,   // synthEngine() serves one or more SynthEngineIds
    kCapSynthDescriptor = 1u << 1,   // synthDescriptor(): the engine's tables (synth_descriptor.h)
    kCapSection = 1u << 2,           // section(): data saved with the project (project.ot3x)
    kCapMasterInsert = 1u << 3,      // masterInsert(): an effect on the master bus (audio/inserts.h)
    kCapPage = 1u << 4,              // page(): a settings page in the Master cycle (modules/module_ui.h)
    kCapEffect = 1u << 5,            // effect(i): per-instance effects (audio/inserts.h IEffect)
};

// Module data saved with the project: one chunk of the project sidecar
// project.ot3x (docs/specs/3x-01-ot3-format.md), main loop only. A build
// without the module keeps the chunk's bytes and writes them back unchanged.
class IProjectSection {
public:
    virtual uint16_t tag() const = 0;          // 0x4000..0x7FFF, unique in a profile
    virtual uint16_t version() const = 0;      // payload layout version
    virtual bool dirty() const = 0;            // changed since the last encode
    // Payload into buf (<= 4096 bytes); 0 = nothing to save (defaults).
    virtual uint32_t encode(uint8_t* buf, uint32_t cap) = 0;
    // Take the leading fields this version knows; false = refused (defaults kept).
    virtual bool decode(const uint8_t* p, uint32_t len, uint16_t writerVersion) = 0;
    virtual void reset() = 0;                  // New Project / no chunk in the file
};

struct ModuleInfo {
    char id[5];                // "FAT1" + NUL; unique in a profile
    uint16_t api;              // == kModuleApi, or the host refuses the module
    uint16_t version;          // major * 100 + minor
    const char* name;          // "FAT synth"
    uint32_t caps;
    // Declared budgets (docs/PLAN_3X.md §3.1); tools/profile.py checks the sums.
    uint16_t sramAudioBytes;
    uint32_t sdramBytes;
    uint16_t fastrunBytes;
    uint16_t cpuInsnPerUnit;   // bench-m4 worst case per unit (voice; PERC: slot)
    uint8_t maxUnits;
};

class ModuleHost;

class IModule {
public:
    virtual const ModuleInfo& info() const = 0;
    // Once, main loop, before audio starts. false = the module is left out.
    virtual bool attach(ModuleHost& host) = 0;
    // The engine for a Tracker Mini engine id, or nullptr.
    virtual audio::ISynthEngine* synthEngine(int32_t engineId) {
        (void)engineId;
        return nullptr;
    }
    // The i-th engine descriptor of the module (i = 0, 1, ...), nullptr past the end.
    virtual const SynthDescriptor* synthDescriptor(int i) const {
        (void)i;
        return nullptr;
    }
    // Project data of the module (kCapSection), or nullptr.
    virtual IProjectSection* section() { return nullptr; }
    // Master bus effect (kCapMasterInsert), or nullptr.
    virtual audio::IMasterInsert* masterInsert() { return nullptr; }
    // Settings page (kCapPage), or nullptr.
    virtual IModulePage* page() { return nullptr; }
    // [3.x] The i-th per-instance effect (kCapEffect; i = 0, 1, ...), nullptr
    // past the end. Singletons: the caller owns each instance's memory.
    virtual audio::IEffect* effect(int i) {
        (void)i;
        return nullptr;
    }
};

using ModuleFactory = IModule& (*)();
// Generated (module_registry.cpp): nullptr-terminated, constant-initialised.
extern const ModuleFactory kModuleFactories[];

class ModuleHost {
public:
    static constexpr int kMaxModules = 16;
    static ModuleHost& instance();
    // Walks kModuleFactories: API check, duplicate-id check, attach().
    // Idempotent; main loop only (not audio-safe on the first call).
    void init();
    int count() const { return n_; }
    IModule* at(int i) const { return i >= 0 && i < n_ ? mods_[i] : nullptr; }
    const IModule* find(const char* id) const;
    // First module serving engineId; runs init() if it never ran (host tests).
    audio::ISynthEngine* synthEngine(int32_t engineId);
    // Engine descriptors of the profile's modules (registered at init; a module
    // whose descriptor repeats an engine id or has too many parameters is refused).
    static constexpr int kMaxEngines = 16;
    const SynthDescriptor* synthDescriptor(int32_t engineId);
    int engineCount();
    const SynthDescriptor* engineAt(int k);
    // Module settings pages in module order.
    int pageCount();
    IModulePage* pageAt(int k);
    // Master inserts of the profile's modules, in module order (a chain).
    int masterInserts(audio::IMasterInsert** out, int cap);
    audio::IMasterInsert* masterInsert();   // the first, or nullptr
    // [3.x] Per-instance effects of the profile's modules, in module order (no
    // table: walked per call, main loop). A module repeating an effect id or
    // over the IEffect limits is refused at init.
    int effectCount();
    audio::IEffect* effectAt(int k);
    audio::IEffect* effectById(const char* id);
    // Host builds (tests): register one more module after init().
    bool addForTest(IModule& m);
    // Id of the last module refused at init ("" = none).
    const char* refused() const { return refused_; }
    // [3.x] Module memory in the SDRAM (docs/PLAN_TECHNO.md E1): `bytes` from
    // the profile's block (the sum of the modules' declared sdram_bytes,
    // OT_PROFILE_SDRAM_BYTES, in .sdram_bss on the device, zeroed at boot),
    // 16-byte aligned; nullptr when it does not fit (or bytes is 0). From
    // attach() only (main loop); never freed. Audio use: block spans only
    // (the rule above).
    void* sdram(uint32_t bytes);
    // [3.x] The core's synth slot insert lines (one kEffectWorkMax line per
    // slot), reserved once at init after the modules (images with slot
    // inserts; else nullptr).
    void* slotInsertLines() const { return insertLines_; }
    uint32_t sdramUsed() const { return sdram_.used; }
    uint32_t sdramSize() const { return sdram_.size; }
    struct SdramArena {
        uint8_t* base;
        uint32_t size, used;
    };
    // Host tests: hand out another block; returns the one in use.
    SdramArena sdramArenaForTest(SdramArena a) {
        const SdramArena old = sdram_;
        sdram_ = a;
        return old;
    }

private:
    bool descriptorsOk(const IModule& m) const;
    bool effectsOk(IModule& m);
    IModule* mods_[kMaxModules] = {};
    const SynthDescriptor* desc_[kMaxEngines] = {};
    int n_ = 0;
    int nDesc_ = 0;
    bool inited_ = false;
    char refused_[5] = {};
    SdramArena sdram_ = {nullptr, 0, 0};   // the profile's block from init() (constant-initialised)
    void* insertLines_ = nullptr;
    static SdramArena profileSdram();
};

}  // namespace mod
}  // namespace ot
