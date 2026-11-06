// [I] Loadable modules (modules/module_loader.h, fork issue #21).
#include "modules/module_loader.h"

#include <cstring>

#include "formats/crc32.h"

namespace ot {
namespace mod {

namespace {
uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void wr32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}
}  // namespace

const char* otmErrorText(OtmError e) {
    switch (e) {
        case OtmError::None: return "ok";
        case OtmError::Truncated: return "file truncated";
        case OtmError::BadMagic: return "not a module";
        case OtmError::BadCrc: return "file damaged";
        case OtmError::Api:
        case OtmError::Layout:
        case OtmError::Abi:
        case OtmError::Exports: return "needs updating";
        case OtmError::TooBig: return "doesn't fit";
        case OtmError::Align: return "bad alignment";
        case OtmError::BadFixup: return "file damaged";
    }
    return "?";
}

OtmError otmCheck(const uint8_t* file, uint32_t len, const OtmCore& core, OtmHeader* hdr) {
    if (len < kOtmHeaderBytes + 4) return OtmError::Truncated;
    OtmHeader h;
    std::memcpy(&h, file, sizeof(h));   // little-endian on every target we build
    if (h.magic != kOtmMagic || h.headerBytes != kOtmHeaderBytes) return OtmError::BadMagic;
    const uint64_t body = uint64_t(kOtmHeaderBytes) + h.imageBytes + uint64_t(h.nFixups) * 4u;
    if (body + 4u != len) return OtmError::Truncated;
    if (crc32(file, uint32_t(body)) != rd32(file + body)) return OtmError::BadCrc;
    if (h.api != core.api) return OtmError::Api;
    if (h.layoutHash != core.layoutHash) return OtmError::Layout;
    if (h.abiFlags != core.abiFlags) return OtmError::Abi;
    if (h.nExports > core.nExports) return OtmError::Exports;
    if (h.align < 4 || (h.align & (h.align - 1)) || h.entryOffset >= h.imageBytes) return OtmError::BadMagic;
    if (hdr) *hdr = h;
    return OtmError::None;
}

OtmError otmLoad(const uint8_t* file, uint32_t len, const OtmCore& core, uint8_t* dst, uint32_t cap,
                 uint32_t base, uint32_t* entry) {
    OtmHeader h;
    OtmError e = otmCheck(file, len, core, &h);
    if (e != OtmError::None) return e;
    if (uint64_t(h.imageBytes) + h.bssBytes > cap) return OtmError::TooBig;
    if (base & (h.align - 1)) return OtmError::Align;
    const uint8_t* img = file + kOtmHeaderBytes;
    const uint8_t* fix = img + h.imageBytes;
    // Check every fixup before writing anything: a refused file leaves dst as it was.
    for (uint32_t i = 0; i < h.nFixups; ++i) {
        const uint32_t f = rd32(fix + 4u * i);
        const uint32_t off = f & 0xFFFFFFu, kind = f >> 24;
        if (uint64_t(off) + 4u > h.imageBytes || kind > h.nExports) return OtmError::BadFixup;
    }
    std::memcpy(dst, img, h.imageBytes);
    std::memset(dst + h.imageBytes, 0, h.bssBytes);
    for (uint32_t i = 0; i < h.nFixups; ++i) {
        const uint32_t f = rd32(fix + 4u * i);
        const uint32_t off = f & 0xFFFFFFu, kind = f >> 24;
        wr32(dst + off, rd32(dst + off) + (kind ? core.exports[kind - 1] : base));
    }
    if (entry) *entry = base + h.entryOffset;
    return OtmError::None;
}

}  // namespace mod
}  // namespace ot
