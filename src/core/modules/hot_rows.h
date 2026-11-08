// [I] Built-in synths' hot code in the shared fast pool.
//
// A compiled-in synth's hottest functions (its row, tools/hot_rows/<name>.hot)
// are linked in flash, back to back in row order, and run there by default.
// When the project uses the synth, the row up to a cut moves into the fast
// pool in SRAM_L, shared with the card synths' fast parts by the same turns
// (shareFastBlock, modules/module_files.h). Moving it out again only points
// the engine back at flash: the flash copy never changes.
//
// The build (tools/hot_pool.py) records what depends on where the row runs:
//   - fixups in the row (row offset in bits 0..23, kind in bits 24..31):
//       kHotFixBranch: a BL / B.W. A target inside the moved part moves with
//         it; any other target (flash) is out of branch range from SRAM_L, so
//         the branch goes through a stub after the moved code
//         (`ldr.w pc, [pc, #0]` + the target, 8 bytes per target);
//       kHotFixWord: a 32-bit word (a literal). One that points into the
//         moved part, or into one of the row's tables, is changed to the copy.
//   - tables: constant data in flash holding pointers into the row (the
//     engine's vtable, kernel tables). Moving copies each one after the
//     stubs, its entries pointing at the moved code where they can.
//   - slots: RAM words pointing into a table (the engine object's vtable
//     pointer). They point at the table's copy while the row is moved.
// Code outside the row that calls into it, or reads a table, keeps using
// flash: slower, never wrong.
//
// Pool layout of a moved row: the code up to the cut (8-aligned base), the
// stubs, the tables. Callers move rows only while no engine runs
// (SynthBank::holdEngines) and sync the code cache before letting go:
// voices keep no pointers into the pool across that.
//
// Platform-independent: host tests move fake rows between buffers.
#pragma once

#include <cstdint>

#include "modules/module.h"
#include "modules/module_loader.h"

namespace ot {
namespace mod {

constexpr uint32_t kHotFixBranch = 1, kHotFixWord = 2;
constexpr uint32_t kHotMaxStubs = 64;   // distinct branch targets outside the moved part
constexpr uint32_t kHotStubBytes = 8;

struct HotTable {
    const uint8_t* mem;        // the table (== addr on the device)
    uint32_t addr;
    uint32_t bytes;            // a multiple of 4
    uint32_t nEntries;
    const uint32_t* entries;   // byte offsets of the words that point into the row
};

struct HotSlot {
    uint32_t* word;            // RAM word
    uint32_t value;            // its value while the row runs from flash (inside a table)
};

struct HotRow {
    ModuleFactory module;      // the compiled-in module the row belongs to
    const uint8_t* mem;        // the row in flash (== addr on the device)
    uint32_t addr;
    uint32_t bytes;
    uint32_t nCuts;
    const uint32_t* cuts;      // row offsets: ascending multiples of 8, the last = bytes
    uint32_t nFixups;
    const uint32_t* fixups;
    uint32_t nTables;
    const HotTable* tables;
    uint32_t nSlots;
    const HotSlot* slots;
};

// The build's table of the image's rows (tools/hot_pool.py; none on host builds).
struct HotRows {
    uint32_t n;
    const HotRow* rows;
};

// True when the row's cuts, fixups, tables and slots are consistent.
bool hotRowCheck(const HotRow& r);

// Pool bytes the row needs moved up to cut (0 or one of r.cuts): the code,
// its stubs and its tables; 0 for cut 0, UINT32_MAX for a cut that isn't one
// or more stubs than kHotMaxStubs.
uint32_t hotRowSpan(const HotRow& r, uint32_t cut);

// Moves the row up to cut into dst (base 8-aligned, cap >= hotRowSpan) and
// points the slots at the copies; cut 0 = hotRowRestore. False: nothing
// written (a bad cut, no room, a branch out of range).
bool hotRowMove(const HotRow& r, uint32_t cut, const OtmBlock& dst);

// The slots point at flash again (the row runs from there).
void hotRowRestore(const HotRow& r);

}  // namespace mod
}  // namespace ot
