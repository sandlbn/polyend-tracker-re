// [I] Loadable modules: a module built into a .otm file by
// tools/make_otm.py, loaded from the card into a block of memory at run time.
//
// An image has up to two parts: an optional row (the module's hot functions,
// hottest first, tools/synth_hot.py) and the main part (the rest of the code,
// constants and data, followed by the bss). The loader picks a cut in the row
// when it loads the module: the row up to the cut is the fast part (SRAM_L on
// the device), the rest of the row (the tail) goes after the main part's bss.
// The cuts are the row's function boundaries, each with the share of the
// module's work the row holds up to it, so a cut can fit whatever room the
// fast block has. The build tool links the row at 0 and the main part at
// kOtmMainLink (at 0 when there is no row), and keeps a list of the 32-bit
// words that depend on where they land ("fixups"). Loading is a copy per part
// plus one change per fixup:
//   - kind 0: the word holds a main-part address; add (main base - main link);
//   - kind k (1..kOtmMaxExports): the word holds an addend; add core export
//     k - 1 (module_exports.txt, append-only). Calls into the core go through
//     a stub per imported function in the main part (`ldr.w pc, [pc]` + that
//     word): flash is out of Thumb branch range from the SRAM the image runs in;
//   - kOtmFixFast: the word holds a row address; it becomes that address in
//     the fast part (before the cut) or in the tail (from the cut on);
//   - kOtmFixBranch: a Thumb BL / B.W (T4) out of its function in the row, or
//     between the row and the main part; its displacement is worked out again
//     from where both ends landed.
// Relative branches inside a row function, or inside the main part, need nothing.
//
// File layout (little-endian): OtmHeader, the image (imageBytes: rowBytes of
// row, then the main part), the fixups (nFixups x uint32: image offset in bits
// 0..23, kind in bits 24..31), the cuts (nCuts x 2 uint32: row offset, the
// share of the module's work up to it in millionths; ascending, multiples of
// 4, the last = rowBytes), then a CRC-32 (formats/crc32.h) of everything
// before it. The zero-initialised part (bssBytes) follows the main part in
// memory but is not in the file.
//
// [I] Growing later (#21 "option 2"): the row's functions are separate
// pieces already, so a loader could pick any of them (by the patch's modes)
// instead of a prefix; only the address mapping changes.
//
// Platform-independent: the caller passes the load address as a 32-bit value
// and the export addresses, so host tests relocate images for the device.
// Main loop only (never the audio path).
#pragma once

#include <cstdint>

namespace ot {
namespace mod {

constexpr uint32_t kOtmMagic = 0x334D544Fu;   // "OTM3" (OTM2: a fixed fast part, OTM1: none)
constexpr uint16_t kOtmHeaderBytes = 56;
constexpr uint32_t kOtmMainLink = 0x00400000u;   // main part's link address when there is a row
constexpr uint32_t kOtmMaxExports = 253;
constexpr uint32_t kOtmFixBranch = 254, kOtmFixFast = 255;

struct OtmHeader {
    uint32_t magic;          // kOtmMagic
    uint16_t headerBytes;    // kOtmHeaderBytes
    uint16_t api;            // kModuleApi the module was built against
    uint32_t layoutHash;     // hash of the module-facing core headers (tools/make_otm.py)
    uint32_t abiFlags;       // hash of the compiler's target flags
    char id[8];              // module id ("TAPE"), NUL-padded
    uint32_t imageBytes;     // image in the file (code, constants, initialised data)
    uint32_t bssBytes;       // zeroed after the image
    uint32_t align;          // load address alignment (power of two, >= 4)
    uint32_t entryOffset;    // IModule& entry() in the image, Thumb bit set
    uint32_t nFixups;
    uint32_t nExports;       // exports the module needs (highest kind used)
    uint32_t rowBytes;       // the row at the start of the image (0 = none)
    uint32_t nCuts;          // cuts in the row (0 without one)
};
static_assert(sizeof(OtmHeader) == kOtmHeaderBytes, "OtmHeader is the file layout");

enum class OtmError : uint8_t {
    None,
    Truncated,     // file shorter than its header says
    BadMagic,
    BadCrc,
    Api,           // built for another kModuleApi
    Layout,        // built against other core headers
    Abi,           // built with other target flags
    Exports,       // needs exports this core doesn't have
    TooBig,        // image + bss does not fit the block
    Align,         // block not aligned as the module needs
    BadFixup,      // a fixup outside the image
};

const char* otmErrorText(OtmError e);

// What this core accepts (the device passes its generated values).
struct OtmCore {
    uint16_t api;
    uint32_t layoutHash;
    uint32_t abiFlags;
    const uint32_t* exports;   // export addresses, module_exports.txt order
    uint32_t nExports;
};

// Checks the file against the core (no copy): header, CRC, versions, exports.
// On success *hdr is the header.
OtmError otmCheck(const uint8_t* file, uint32_t len, const OtmCore& core, OtmHeader* hdr);

// A block a part is loaded into: mem (cap bytes) is addressed as `base` by
// the code, which is the same as mem on the device.
struct OtmBlock {
    uint8_t* mem;
    uint32_t cap;
    uint32_t base;
};

// Cut k (0 <= k < h.nCuts) of a checked file: its row offset, and the share
// of the module's work the row holds up to it (millionths).
uint32_t otmCutBytes(const uint8_t* file, const OtmHeader& h, uint32_t k);
uint32_t otmCutShare(const uint8_t* file, const OtmHeader& h, uint32_t k);

// The longest cut that fits `room` bytes of fast block (0 = none).
uint32_t otmCutFor(const uint8_t* file, const OtmHeader& h, uint32_t room);

// Where the tail starts in the main block (after the main part and its bss),
// and the main block's bytes for a cut.
inline uint32_t otmTailOffset(const OtmHeader& h) { return (h.imageBytes - h.rowBytes + h.bssBytes + 3u) & ~3u; }
inline uint32_t otmMainSpan(const OtmHeader& h, uint32_t cut) {
    return h.rowBytes > cut ? otmTailOffset(h) + (h.rowBytes - cut) : h.imageBytes - h.rowBytes + h.bssBytes;
}

// Loads a checked file with the row cut at `cut` (0 or a cut's offset): the
// fast part into fast (ignored when cut is 0), the main part + bss + the
// row's tail into main; applies the fixups. On success *entry is the entry
// point's address. A refused file leaves both blocks as they were.
OtmError otmLoad(const uint8_t* file, uint32_t len, const OtmCore& core, const OtmBlock& fast,
                 const OtmBlock& main, uint32_t cut, uint32_t* entry);

// One block for everything (cut 0: the whole row in the tail).
OtmError otmLoad(const uint8_t* file, uint32_t len, const OtmCore& core, uint8_t* dst, uint32_t cap,
                 uint32_t base, uint32_t* entry);

}  // namespace mod
}  // namespace ot
