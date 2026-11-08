// [I] Modules loaded from the card: every Modules/*.otm at
// boot, into the device's module pool, registered after the compiled-in ones
// (ModuleHost::addLoaded; modules/module_loader.h has the file format).
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

struct OtmLoadReport {
    static constexpr int kMax = 8;
    struct Item {
        char file[40];      // name in Modules/ (longer names are skipped)
        OtmError error;     // None = loaded and registered
        bool refused;       // loaded, but ModuleHost refused it (id taken, api, ...)
        uint32_t rowBytes;  // its row: the code that may run from the fast block (0 = none)
        uint32_t fastBytes; // the part of the row in the fast block (the cut; 0 = none)
    };
    Item items[kMax];
    int n = 0;
    uint32_t used = 0;      // pool bytes in use after loading
    uint32_t fastUsed = 0;  // fast block bytes in use
    bool noRoom = false;    // the pool could not grow (the stack floor): nothing loaded
    int skipped = 0;        // .otm files past the first kMax in name order: not loaded
};

constexpr const char* kModulesDir = "Modules";

// Cuts per module the fast block is shared by (a longer row stops there).
constexpr uint32_t kOtmMaxShareCuts = 32;

// Shares a fast block of cap bytes between n modules' rows (cuts[i][0 ..
// nCuts[i]) = module i's cuts, module_loader.h): they take turns, in order,
// moving to their next cut while it fits. share[i] = module i's cut (0 = none).
void shareFastBlock(const uint32_t (*cuts)[kOtmMaxShareCuts], const uint32_t* nCuts, int n, uint32_t cap,
                    uint32_t* share);

// Loads Modules/*.otm (name order, the first OtmLoadReport::kMax) into the pool and hands each to
// ModuleHost::addLoaded(); their rows share the fast block (shareFastBlock).
// Each file is read twice: once for its cuts, once to load it. scratch holds one file at a time (the sample
// memory at boot: nothing is loaded yet). Main loop, after ModuleHost::init()
// and before the audio engine takes the master inserts.
// Returns the number of modules loaded.
int loadModuleFiles(IStorage& fs, const OtmPool& pool, uint8_t* scratch, uint32_t scratchCap,
                    OtmLoadReport* report);

}  // namespace mod
}  // namespace ot
