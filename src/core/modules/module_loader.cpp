// [I] Loadable modules (modules/module_loader.h).
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
    if (h.nExports > core.nExports || h.nExports > kOtmMaxExports) return OtmError::Exports;
    // The entry is a Thumb function (odd offset): an even one would fault on every boot.
    if (h.align < 4 || (h.align & (h.align - 1)) || h.entryOffset >= h.imageBytes || !(h.entryOffset & 1u))
        return OtmError::BadMagic;
    if (h.fastBytes > h.imageBytes || (h.fastBytes & 3u)) return OtmError::BadMagic;
    if (hdr) *hdr = h;
    return OtmError::None;
}

namespace {

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
void wr16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

// Thumb-2 BL (T1) / B.W (T4): both keep S:I1:I2:imm10:imm11 the same way
// (ARMv7-M ARM A7.7.18 / A7.7.12); I1 = !(J1 ^ S), I2 = !(J2 ^ S).
bool isLongBranch(const uint8_t* p) {
    const uint16_t a = rd16(p), b = rd16(p + 2);
    return (a & 0xF800u) == 0xF000u && ((b & 0xD000u) == 0xD000u || (b & 0xD000u) == 0x9000u);
}

int32_t branchOffset(const uint8_t* p) {
    const uint32_t a = rd16(p), b = rd16(p + 2);
    const uint32_t s = (a >> 10) & 1u, j1 = (b >> 13) & 1u, j2 = (b >> 11) & 1u;
    const uint32_t i1 = ~(j1 ^ s) & 1u, i2 = ~(j2 ^ s) & 1u;
    uint32_t v = s << 24 | i1 << 23 | i2 << 22 | (a & 0x3FFu) << 12 | (b & 0x7FFu) << 1;
    if (s) v |= 0xFE000000u;   // sign-extend the 25 bits
    return int32_t(v);
}

bool setBranchOffset(uint8_t* p, int64_t off) {
    if (off < -(int64_t(1) << 24) || off >= (int64_t(1) << 24) || (off & 1)) return false;
    const uint32_t v = uint32_t(off);
    const uint32_t s = (v >> 24) & 1u, i1 = (v >> 23) & 1u, i2 = (v >> 22) & 1u;
    const uint32_t j1 = (~i1 ^ s) & 1u, j2 = (~i2 ^ s) & 1u;
    wr16(p, uint16_t((rd16(p) & 0xF800u) | s << 10 | ((v >> 12) & 0x3FFu)));
    wr16(p + 2, uint16_t((rd16(p + 2) & 0xD000u) | j1 << 13 | j2 << 11 | ((v >> 1) & 0x7FFu)));
    return true;
}

}  // namespace

OtmError otmLoad(const uint8_t* file, uint32_t len, const OtmCore& core, const OtmBlock& fast,
                 const OtmBlock& main, uint32_t* entry) {
    OtmHeader h;
    OtmError e = otmCheck(file, len, core, &h);
    if (e != OtmError::None) return e;
    const uint32_t mainBytes = h.imageBytes - h.fastBytes;
    if (uint64_t(mainBytes) + h.bssBytes > main.cap) return OtmError::TooBig;
    if (main.base & (h.align - 1)) return OtmError::Align;
    if (h.fastBytes) {
        if (h.fastBytes > fast.cap) return OtmError::TooBig;
        if (fast.base & (h.align - 1)) return OtmError::Align;
    }
    const uint32_t mainLink = h.fastBytes ? kOtmMainLink : 0u;
    const uint32_t fastShift = fast.base, mainShift = main.base - mainLink;
    const uint8_t* img = file + kOtmHeaderBytes;
    const uint8_t* fix = img + h.imageBytes;
    // Check every fixup before writing anything: a refused file leaves the blocks as they were.
    for (uint32_t i = 0; i < h.nFixups; ++i) {
        const uint32_t f = rd32(fix + 4u * i);
        const uint32_t off = f & 0xFFFFFFu, kind = f >> 24;
        if (uint64_t(off) + 4u > h.imageBytes) return OtmError::BadFixup;
        // A word never straddles the two parts.
        if (off < h.fastBytes && off + 4u > h.fastBytes) return OtmError::BadFixup;
        if (kind == kOtmFixBranch) {
            if (!h.fastBytes || !isLongBranch(img + off)) return OtmError::BadFixup;
            const int64_t moved = off < h.fastBytes ? int64_t(mainShift) - int64_t(fastShift)
                                                    : int64_t(fastShift) - int64_t(mainShift);
            const int64_t to = int64_t(branchOffset(img + off)) + int32_t(uint32_t(moved));
            if (to < -(int64_t(1) << 24) || to >= (int64_t(1) << 24)) return OtmError::BadFixup;
        } else if (kind == kOtmFixFast) {
            if (!h.fastBytes) return OtmError::BadFixup;
        } else if (kind > h.nExports) {
            return OtmError::BadFixup;
        }
    }
    if (h.fastBytes) std::memcpy(fast.mem, img, h.fastBytes);
    std::memcpy(main.mem, img + h.fastBytes, mainBytes);
    std::memset(main.mem + mainBytes, 0, h.bssBytes);
    for (uint32_t i = 0; i < h.nFixups; ++i) {
        const uint32_t f = rd32(fix + 4u * i);
        const uint32_t off = f & 0xFFFFFFu, kind = f >> 24;
        uint8_t* at = off < h.fastBytes ? fast.mem + off : main.mem + (off - h.fastBytes);
        if (kind == kOtmFixBranch) {
            const uint32_t moved = off < h.fastBytes ? mainShift - fastShift : fastShift - mainShift;
            setBranchOffset(at, int64_t(branchOffset(at)) + int32_t(moved));
        } else {
            const uint32_t add = kind == 0 ? mainShift : kind == kOtmFixFast ? fastShift : core.exports[kind - 1];
            wr32(at, rd32(at) + add);
        }
    }
    if (entry) {
        const uint32_t eo = h.entryOffset & ~1u;
        *entry = (eo < h.fastBytes ? fast.base + eo : main.base + (eo - h.fastBytes)) | (h.entryOffset & 1u);
    }
    return OtmError::None;
}

OtmError otmLoad(const uint8_t* file, uint32_t len, const OtmCore& core, uint8_t* dst, uint32_t cap,
                 uint32_t base, uint32_t* entry) {
    OtmHeader h;
    OtmError e = otmCheck(file, len, core, &h);
    if (e != OtmError::None) return e;
    const uint32_t span = h.fastBytes ? otmFastSpan(h) : 0u;
    if (span > cap) return OtmError::TooBig;
    const OtmBlock fast{dst, span, base};
    const OtmBlock main{dst + span, cap - span, base + span};
    return otmLoad(file, len, core, fast, main, entry);
}

}  // namespace mod
}  // namespace ot
