// [I] Loadable modules: a module built into a .otm file by
// tools/make_otm.py, loaded from the card into a block of memory at run time.
//
// The build tool links the module completely at address 0 and keeps a list of
// the 32-bit words that depend on where it lands ("fixups"). Loading is a copy
// plus one addition per fixup:
//   - kind 0: the word holds an address inside the image; add the load address;
//   - kind k (1..255): the word holds an addend; add core export k - 1
//     (module_exports.txt, append-only). Calls into the core go through a stub
//     per imported function inside the image (`ldr.w pc, [pc]` + that word):
//     flash is out of Thumb branch range from the SRAM the image runs in.
// Relative branches inside the image need nothing.
//
// File layout (little-endian): OtmHeader, the image (imageBytes), the fixups
// (nFixups x uint32: offset in bits 0..23, kind in bits 24..31), then a CRC-32
// (formats/crc32.h) of everything before it. The zero-initialised part
// (bssBytes) follows the image in memory but is not in the file.
//
// Platform-independent: the caller passes the load address as a 32-bit value
// and the export addresses, so host tests relocate images for the device.
// Main loop only (never the audio path).
#pragma once

#include <cstdint>

namespace ot {
namespace mod {

constexpr uint32_t kOtmMagic = 0x314D544Fu;   // "OTM1"
constexpr uint16_t kOtmHeaderBytes = 48;

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

// Loads a checked file into dst (cap bytes; dst is addressed as `base` by the
// code, which is the same as dst on the device): copies the image, zeroes the
// bss and applies the fixups. On success *entry is base + entryOffset.
OtmError otmLoad(const uint8_t* file, uint32_t len, const OtmCore& core, uint8_t* dst, uint32_t cap,
                 uint32_t base, uint32_t* entry);

}  // namespace mod
}  // namespace ot
