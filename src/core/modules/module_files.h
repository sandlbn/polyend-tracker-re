// [I] Modules loaded from the card (fork issue #21; modules/module_loader.h has
// the file format). At boot every Modules/*.otm is looked at: a module without
// synth engines (master inserts, effects) loads into the device's module pool
// for good; a synth module is only listed (its engines and their names), and
// loads when the project names one of its engines (loadCardSynths). Both
// register after the compiled-in modules (ModuleHost::addLoaded).
#pragma once

#include <cstdint>

#include "modules/module_loader.h"

namespace ot {
class IStorage;
namespace mod {

class IModule;

// The block loaded modules run from, given by the device glue (HalRefs).
struct OtmPool {
    uint8_t* mem;       // the block (the stack's bottom on the device, hal/stack_guard.h)
    uint32_t base;      // its address as the module code sees it (== mem on the device)
    uint32_t cap;       // the most it may grow to
    OtmCore core;       // what this core accepts, and its exports
    // After an image is written: make it executable (code cache, barriers).
    void (*sync)(const void* p, uint32_t len);
    // Calls the module's entry point (base + entryOffset, Thumb bit set).
    IModule& (*enter)(uint32_t entry);
    // Optional: makes the first `bytes` of the block usable (the device moves
    // the stack guard over them); false = no room. Null = always usable.
    bool (*resize)(uint32_t bytes);
    // Optional: the block fast parts run from (SRAM_L on the device); each
    // module's row is cut to fit the room left, the rest of the row goes
    // after its main part in mem. Null = none.
    uint8_t* fastMem = nullptr;
    uint32_t fastBase = 0;
    uint32_t fastCap = 0;
};

// What happened to each Modules/*.otm: filled at boot, updated by every
// loadCardSynths (serial counts the changes).
struct OtmLoadReport {
    static constexpr int kMax = 16;
    struct Item {
        char file[40];      // name in Modules/ (longer names are skipped)
        OtmError error;     // None = loaded and registered (a synth: when last wanted)
        bool refused;       // loaded, but ModuleHost refused it (id taken, api, ...)
        bool synth;         // a synth module: loads when a project names its engine
        bool loaded;        // in the pool now
        uint32_t rowBytes;  // its row: the code that may run from the fast block (0 = none)
        uint32_t fastBytes; // the part of the row in the fast block (the cut; 0 = none)
    };
    Item items[kMax];
    int n = 0;
    uint32_t used = 0;      // pool bytes in use
    uint32_t fastUsed = 0;  // fast block bytes in use
    bool noRoom = false;    // the pool could not grow (the stack floor): nothing loaded
    int skipped = 0;        // .otm files past the first kMax in name order: not loaded
    uint32_t serial = 0;    // +1 on every change
};

// A card synth engine (the boot scan lists them, loaded or not).
struct CardEngine {
    int32_t engineId;
    char name[12];          // SynthDescriptor::name (Patches/<name>/)
    char displayName[16];   // SynthDescriptor::displayName, else name
    uint8_t file;           // OtmLoadReport item of its module
};
constexpr int kMaxCardEngines = 32;

constexpr const char* kModulesDir = "Modules";

// Cuts per module the fast block is shared by (a longer row stops there).
constexpr uint32_t kOtmMaxShareCuts = 32;

// Shares a fast block of cap bytes between n modules' rows (cuts[i][0 ..
// nCuts[i]) = module i's cuts, module_loader.h): they take turns, in order,
// moving to their next cut while it fits. share[i] = module i's cut (0 = none).
void shareFastBlock(const uint32_t (*cuts)[kOtmMaxShareCuts], const uint32_t* nCuts, int n, uint32_t cap,
                    uint32_t* share);

// Boot: Modules/*.otm (name order, the first OtmLoadReport::kMax). Each file is loaded once to see what it
// is; a module without synth engines stays and is handed to
// ModuleHost::addLoaded(), a synth module's engines are listed
// (cardEngineAt) and its pool room goes back. The modules that stay share
// the fast block (shareFastBlock); each file is read twice (cuts, then the
// load). scratch holds one file at a time (the sample memory at boot:
// nothing is loaded yet). Main loop, after ModuleHost::init() and before the
// audio engine takes the master inserts. The pool and the report are kept
// (pointers) for loadCardSynths: they must outlive it. Returns the number of modules loaded for good.
int loadModuleFiles(IStorage& fs, const OtmPool& pool, uint8_t* scratch, uint32_t scratchCap,
                    OtmLoadReport* report);

// The card synth engines the boot scan listed (none on builds without a pool).
int cardEngineCount();
const CardEngine* cardEngineAt(int k);
const CardEngine* cardEngine(int32_t engineId);   // nullptr: not a card engine
bool cardEngineLoaded(int32_t engineId);

// The card synth modules serving engines[0 .. n) are loaded, every other
// card synth leaves (ids that are not card engines are skipped). The modules
// are loaded in the order their engines come, so the first ones get the room
// when not all fit; their rows share what the boot modules left of the fast
// block. Nothing changes when the same set is loaded already. The audio block
// must not run a card engine meanwhile (SynthBank::holdEngines). Main loop;
// scratch holds one file at a time. Returns false when a wanted module
// didn't load (no room, a damaged file: the report says).
bool loadCardSynths(IStorage& fs, const int32_t* engines, int n, uint8_t* scratch, uint32_t scratchCap);
// True when loadCardSynths(engines, n) would change what's loaded.
bool cardSynthsDiffer(const int32_t* engines, int n);

}  // namespace mod
}  // namespace ot
