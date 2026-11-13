// [I] Loadable modules (modules/module_loader.h).
#include "modules/module_loader.h"

#include <cstring>

#include "formats/crc32.h"
#include "modules/thumb_branch.h"

namespace ot {
namespace mod {

using thumb::rd32;
using thumb::wr32;

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
    const uint64_t body = uint64_t(kOtmHeaderBytes) + h.imageBytes + uint64_t(h.nFixups) * 4u + uint64_t(h.nCuts) * 8u;
    if (body + 4u != len) return OtmError::Truncated;
    if (crc32(file, uint32_t(body)) != rd32(file + body)) return OtmError::BadCrc;
    if (h.api != core.api) return OtmError::Api;
    if (h.layoutHash != core.layoutHash) return OtmError::Layout;
    if (h.abiFlags != core.abiFlags) return OtmError::Abi;
    if (h.nExports > core.nExports || h.nExports > kOtmMaxExports) return OtmError::Exports;
    // The entry is a Thumb function (odd offset): an even one would fault on every boot.
    if (h.align < 4 || (h.align & (h.align - 1)) || h.entryOffset >= h.imageBytes || !(h.entryOffset & 1u))
        return OtmError::BadMagic;
    if (h.rowBytes > h.imageBytes || (h.rowBytes & 7u) || h.rowBytes >= kOtmMainLink) return OtmError::BadMagic;
    if ((h.rowBytes == 0) != (h.nCuts == 0)) return OtmError::BadMagic;
    // The cuts: ascending multiples of 8, the last at the row's end.
    const uint8_t* cuts = file + kOtmHeaderBytes + h.imageBytes + 4u * h.nFixups;
    uint32_t prev = 0;
    for (uint32_t k = 0; k < h.nCuts; ++k) {
        const uint32_t c = rd32(cuts + 8u * k);
        if (c <= prev || (c & 7u) || c > h.rowBytes) return OtmError::BadMagic;
        prev = c;
    }
    if (h.nCuts && prev != h.rowBytes) return OtmError::BadMagic;
    if (hdr) *hdr = h;
    return OtmError::None;
}

uint32_t otmCutBytes(const uint8_t* file, const OtmHeader& h, uint32_t k) {
    return rd32(file + kOtmHeaderBytes + h.imageBytes + 4u * h.nFixups + 8u * k);
}

uint32_t otmCutShare(const uint8_t* file, const OtmHeader& h, uint32_t k) {
    return rd32(file + kOtmHeaderBytes + h.imageBytes + 4u * h.nFixups + 8u * k + 4u);
}

uint32_t otmCutFor(const uint8_t* file, const OtmHeader& h, uint32_t room) {
    uint32_t best = 0;
    for (uint32_t k = 0; k < h.nCuts; ++k) {
        const uint32_t c = otmCutBytes(file, h, k);
        if (c > room) break;
        best = c;
    }
    return best;
}

using thumb::branchOffset;
using thumb::isLongBranch;
using thumb::setBranchOffset;

namespace {

// Where the image lands for one cut: image offsets and link addresses to
// memory and to run-time addresses.
struct Placement {
    const OtmHeader& h;
    const OtmBlock& fast;
    const OtmBlock& main;
    uint32_t cut, mainLink, tail;
    // Link address of image offset off.
    uint32_t link(uint32_t off) const { return off < h.rowBytes ? off : mainLink + (off - h.rowBytes); }
    // Run-time address of a link address (row: up to one past its end).
    uint32_t addr(uint32_t a) const {
        if (h.rowBytes && a <= h.rowBytes) return a < cut ? fast.base + a : main.base + tail + (a - cut);
        return main.base + (a - mainLink);
    }
    uint8_t* mem(uint32_t off) const {
        if (off < cut) return fast.mem + off;
        if (off < h.rowBytes) return main.mem + tail + (off - cut);
        return main.mem + (off - h.rowBytes);
    }
    // Whether a link address is in the image (row, or main part + bss).
    bool inImage(uint32_t a) const {
        if (h.rowBytes && a <= h.rowBytes) return true;
        return a >= mainLink && a - mainLink <= h.imageBytes - h.rowBytes + h.bssBytes;
    }
};

}  // namespace

OtmError otmLoad(const uint8_t* file, uint32_t len, const OtmCore& core, const OtmBlock& fast,
                 const OtmBlock& main, uint32_t cut, uint32_t* entry) {
    OtmHeader h;
    OtmError e = otmCheck(file, len, core, &h);
    if (e != OtmError::None) return e;
    bool cutOk = cut == 0;
    for (uint32_t k = 0; k < h.nCuts && !cutOk; ++k) cutOk = otmCutBytes(file, h, k) == cut;
    if (!cutOk) return OtmError::BadFixup;
    const uint32_t mainBytes = h.imageBytes - h.rowBytes;
    if (otmMainSpan(h, cut) > main.cap) return OtmError::TooBig;
    if (main.base & (h.align - 1)) return OtmError::Align;
    if (cut) {
        if (cut > fast.cap) return OtmError::TooBig;
        if (fast.base & 7u) return OtmError::Align;   // the row's functions need 8 at most
    }
    const Placement pl{h, fast, main, cut, h.rowBytes ? kOtmMainLink : 0u, otmTailOffset(h)};
    const uint32_t mainShift = main.base - pl.mainLink;
    const uint8_t* img = file + kOtmHeaderBytes;
    const uint8_t* fix = img + h.imageBytes;
    // Check every fixup before writing anything: a refused file leaves the blocks as they were.
    for (uint32_t i = 0; i < h.nFixups; ++i) {
        const uint32_t f = rd32(fix + 4u * i);
        const uint32_t off = f & 0xFFFFFFu, kind = f >> 24;
        if (uint64_t(off) + 4u > h.imageBytes) return OtmError::BadFixup;
        // A word never straddles the row and the main part, or the cut.
        if (off < h.rowBytes && off + 4u > h.rowBytes) return OtmError::BadFixup;
        if (off < cut && off + 4u > cut) return OtmError::BadFixup;
        if (kind == kOtmFixBranch) {
            if (!isLongBranch(img + off)) return OtmError::BadFixup;
            const uint32_t from = pl.link(off), to = from + 4u + uint32_t(branchOffset(img + off));
            if (!pl.inImage(to)) return OtmError::BadFixup;
            const int64_t d = int64_t(pl.addr(to)) - (int64_t(pl.addr(from)) + 4);
            if (d < -(int64_t(1) << 24) || d >= (int64_t(1) << 24)) return OtmError::BadFixup;
        } else if (kind == kOtmFixFast) {
            if (!h.rowBytes || rd32(img + off) > h.rowBytes) return OtmError::BadFixup;
        } else if (kind > h.nExports) {
            return OtmError::BadFixup;
        }
    }
    if (cut) std::memcpy(fast.mem, img, cut);
    std::memcpy(main.mem, img + h.rowBytes, mainBytes);
    std::memset(main.mem + mainBytes, 0, h.bssBytes);
    if (h.rowBytes > cut) {
        std::memset(main.mem + mainBytes + h.bssBytes, 0, pl.tail - mainBytes - h.bssBytes);
        std::memcpy(main.mem + pl.tail, img + cut, h.rowBytes - cut);
    }
    for (uint32_t i = 0; i < h.nFixups; ++i) {
        const uint32_t f = rd32(fix + 4u * i);
        const uint32_t off = f & 0xFFFFFFu, kind = f >> 24;
        uint8_t* at = pl.mem(off);
        if (kind == kOtmFixBranch) {
            const uint32_t from = pl.link(off), to = from + 4u + uint32_t(branchOffset(img + off));
            setBranchOffset(at, int64_t(pl.addr(to)) - (int64_t(pl.addr(from)) + 4));
        } else if (kind == kOtmFixFast) {
            wr32(at, pl.addr(rd32(at)));   // a Thumb bit stays: a function's address + 1 is still inside it
        } else {
            wr32(at, rd32(at) + (kind == 0 ? mainShift : core.exports[kind - 1]));
        }
    }
    if (entry) {
        const uint32_t eo = h.entryOffset & ~1u;
        *entry = pl.addr(pl.link(eo)) | (h.entryOffset & 1u);
    }
    return OtmError::None;
}

OtmError otmLoad(const uint8_t* file, uint32_t len, const OtmCore& core, uint8_t* dst, uint32_t cap,
                 uint32_t base, uint32_t* entry) {
    return otmLoad(file, len, core, OtmBlock{nullptr, 0, 0}, OtmBlock{dst, cap, base}, 0u, entry);
}

}  // namespace mod
}  // namespace ot
