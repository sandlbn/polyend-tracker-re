// [I] Loadable modules: a module built into a .otm file by
// tools/make_otm.py, loaded from the card into a block of memory at run time.
//
// An image has up to two parts: an optional fast part (the module's hot
// functions, tools/synth_hot.py, for SRAM_L) and the main part (the rest of
// the code, constants and data, followed by the bss). The build tool links the
// fast part at 0 and the main part at kOtmMainLink (at 0 when there is no fast
// part), and keeps a list of the 32-bit words that depend on where they land
// ("fixups"). Loading is a copy per part plus one change per fixup:
//   - kind 0: the word holds a main-part address; add (main base - main link);
//   - kind k (1..kOtmMaxExports): the word holds an addend; add core export
//     k - 1 (module_exports.txt, append-only). Calls into the core go through
//     a stub per imported function in the main part (`ldr.w pc, [pc]` + that
//     word): flash is out of Thumb branch range from the SRAM the image runs in;
//   - kOtmFixFast: the word holds a fast-part address; add the fast base;
//   - kOtmFixBranch: a Thumb BL / B.W (T4) from one part into the other; its
//     displacement moves by how far the two parts moved apart.
// Relative branches inside a part need nothing.
//
// File layout (little-endian): OtmHeader, the image (imageBytes: fastBytes of
// fast part, then the main part), the fixups (nFixups x uint32: image offset in
// bits 0..23, kind in bits 24..31), then a CRC-32 (formats/crc32.h) of
// everything before it. The zero-initialised part (bssBytes) follows the main
// part in memory but is not in the file.
//
// Platform-independent: the caller passes the load address as a 32-bit value
// and the export addresses, so host tests relocate images for the device.
// Main loop only (never the audio path).
#pragma once

#include <cstdint>

namespace ot {
namespace mod {

constexpr uint32_t kOtmMagic = 0x324D544Fu;   // "OTM2" (OTM1 had no fast part)
constexpr uint16_t kOtmHeaderBytes = 52;
constexpr uint32_t kOtmMainLink = 0x00400000u;   // main part's link address when there is a fast part
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
    uint32_t fastBytes;      // the fast part at the start of the image (0 = none)
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

// Loads a checked file: the fast part into fast (ignored without one), the
// main part + bss into main; applies the fixups. On success *entry is the
// entry point's address. A refused file leaves both blocks as they were.
OtmError otmLoad(const uint8_t* file, uint32_t len, const OtmCore& core, const OtmBlock& fast,
                 const OtmBlock& main, uint32_t* entry);

// One block for everything (a file without a fast part, or both parts back
// to back in dst: the fast part first, the main part at the next multiple of
// the module's alignment).
OtmError otmLoad(const uint8_t* file, uint32_t len, const OtmCore& core, uint8_t* dst, uint32_t cap,
                 uint32_t base, uint32_t* entry);

// Bytes the fast part takes in front of the main part when both share one block.
inline uint32_t otmFastSpan(const OtmHeader& h) { return (h.fastBytes + h.align - 1) & ~(h.align - 1); }

}  // namespace mod
}  // namespace ot
