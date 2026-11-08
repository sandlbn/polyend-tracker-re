// [I] Built-in synths' hot code in the shared fast pool (modules/hot_rows.h, fork issue #21).
#include "modules/hot_rows.h"

#include <cstring>

#include "modules/thumb_branch.h"

namespace ot {
namespace mod {

using thumb::rd32;
using thumb::wr32;

namespace {

bool isCut(const HotRow& r, uint32_t cut) {
    for (uint32_t k = 0; k < r.nCuts; ++k)
        if (r.cuts[k] == cut) return true;
    return false;
}

uint32_t fixOff(uint32_t f) { return f & 0xFFFFFFu; }
uint32_t fixKind(uint32_t f) { return f >> 24; }

// The address a branch fixup at row offset off goes to (while in flash).
uint32_t branchTarget(const HotRow& r, uint32_t off) {
    return r.addr + off + 4u + uint32_t(thumb::branchOffset(r.mem + off));
}

bool insideCut(const HotRow& r, uint32_t cut, uint32_t a) { return a >= r.addr && a - r.addr < cut; }

// Distinct targets of the moved part's branches that stay in flash, in
// first-use order: one stub each. False: more than kHotMaxStubs.
bool stubTargets(const HotRow& r, uint32_t cut, uint32_t* out, uint32_t* n) {
    *n = 0;
    for (uint32_t i = 0; i < r.nFixups; ++i) {
        const uint32_t off = fixOff(r.fixups[i]);
        if (fixKind(r.fixups[i]) != kHotFixBranch || off >= cut) continue;
        const uint32_t t = branchTarget(r, off);
        if (insideCut(r, cut, t)) continue;
        uint32_t k = 0;
        while (k < *n && out[k] != t) ++k;
        if (k < *n) continue;
        if (*n >= kHotMaxStubs) return false;
        out[(*n)++] = t;
    }
    return true;
}

uint32_t tableBytes(const HotRow& r) {
    uint32_t n = 0;
    for (uint32_t t = 0; t < r.nTables; ++t) n += r.tables[t].bytes;
    return n;
}

// Where things land for one cut.
struct Layout {
    const HotRow& r;
    uint32_t cut, base, nStubs;
    uint32_t stubAt(uint32_t k) const { return base + cut + kHotStubBytes * k; }
    uint32_t tableAt(uint32_t t) const {
        uint32_t a = base + cut + kHotStubBytes * nStubs;
        for (uint32_t i = 0; i < t; ++i) a += r.tables[i].bytes;
        return a;
    }
    // A pointer's value after the move: into the moved code, into a table's
    // copy, or unchanged. A Thumb bit stays (inside the same function).
    uint32_t moved(uint32_t v) const {
        if (insideCut(r, cut, v & ~1u)) return v - r.addr + base;
        for (uint32_t t = 0; t < r.nTables; ++t) {
            const HotTable& tb = r.tables[t];
            if (v >= tb.addr && v - tb.addr < tb.bytes) return tableAt(t) + (v - tb.addr);
        }
        return v;
    }
};

}  // namespace

bool hotRowCheck(const HotRow& r) {
    if (!r.mem || (r.addr & 7u) || !r.nCuts || r.cuts[r.nCuts - 1] != r.bytes) return false;
    uint32_t prev = 0;
    for (uint32_t k = 0; k < r.nCuts; ++k) {
        if (r.cuts[k] <= prev || (r.cuts[k] & 7u)) return false;
        prev = r.cuts[k];
    }
    for (uint32_t i = 0; i < r.nFixups; ++i) {
        const uint32_t off = fixOff(r.fixups[i]), kind = fixKind(r.fixups[i]);
        if (uint64_t(off) + 4u > r.bytes || (kind != kHotFixBranch && kind != kHotFixWord)) return false;
        for (uint32_t k = 0; k < r.nCuts; ++k)   // nothing straddles a cut
            if (off < r.cuts[k] && off + 4u > r.cuts[k]) return false;
        if (kind == kHotFixBranch && !thumb::isLongBranch(r.mem + off)) return false;
    }
    for (uint32_t t = 0; t < r.nTables; ++t) {
        const HotTable& tb = r.tables[t];
        if (!tb.mem || (tb.addr & 3u) || (tb.bytes & 3u)) return false;
        for (uint32_t e = 0; e < tb.nEntries; ++e)
            if ((tb.entries[e] & 3u) || tb.entries[e] + 4u > tb.bytes) return false;
    }
    for (uint32_t s = 0; s < r.nSlots; ++s) {
        bool in = false;
        for (uint32_t t = 0; t < r.nTables && !in; ++t)
            in = r.slots[s].value >= r.tables[t].addr && r.slots[s].value - r.tables[t].addr < r.tables[t].bytes;
        if (!r.slots[s].word || !in) return false;
    }
    return true;
}

uint32_t hotRowSpan(const HotRow& r, uint32_t cut) {
    if (!cut) return 0;
    uint32_t targets[kHotMaxStubs], n = 0;
    if (!isCut(r, cut) || !stubTargets(r, cut, targets, &n)) return UINT32_MAX;
    return cut + kHotStubBytes * n + tableBytes(r);
}

bool hotRowMove(const HotRow& r, uint32_t cut, const OtmBlock& dst) {
    if (!cut) {
        hotRowRestore(r);
        return true;
    }
    uint32_t targets[kHotMaxStubs], n = 0;
    if (!isCut(r, cut) || !stubTargets(r, cut, targets, &n) || (dst.base & 7u)) return false;
    if (cut + kHotStubBytes * n + tableBytes(r) > dst.cap) return false;
    const Layout L{r, cut, dst.base, n};
    // Every branch to flash must reach its stub (the pool is far smaller
    // than the range; checked so a bad block is refused before any write).
    for (uint32_t i = 0; i < r.nFixups; ++i) {
        const uint32_t off = fixOff(r.fixups[i]);
        if (fixKind(r.fixups[i]) != kHotFixBranch || off >= cut) continue;
        const uint32_t t = branchTarget(r, off);
        if (insideCut(r, cut, t)) continue;
        uint32_t k = 0;
        while (targets[k] != t) ++k;
        if (!thumb::inBranchRange(int64_t(L.stubAt(k)) - (int64_t(dst.base) + off + 4))) return false;
    }
    std::memcpy(dst.mem, r.mem, cut);
    for (uint32_t i = 0; i < r.nFixups; ++i) {
        const uint32_t off = fixOff(r.fixups[i]);
        if (off >= cut) continue;
        uint8_t* at = dst.mem + off;
        if (fixKind(r.fixups[i]) == kHotFixWord) {
            wr32(at, L.moved(rd32(at)));
            continue;
        }
        const uint32_t t = branchTarget(r, off);
        if (insideCut(r, cut, t)) continue;   // moved with it: the displacement holds
        uint32_t k = 0;
        while (targets[k] != t) ++k;
        thumb::setBranchOffset(at, int64_t(L.stubAt(k)) - (int64_t(dst.base) + off + 4));
    }
    uint8_t* p = dst.mem + cut;
    for (uint32_t k = 0; k < n; ++k, p += kHotStubBytes) {
        // ldr.w pc, [pc, #0]: pc reads as this + 4, the word right after.
        thumb::wr16(p, 0xF8DFu);
        thumb::wr16(p + 2, 0xF000u);
        wr32(p + 4, targets[k] | 1u);
    }
    for (uint32_t t = 0; t < r.nTables; ++t) {
        const HotTable& tb = r.tables[t];
        std::memcpy(p, tb.mem, tb.bytes);
        for (uint32_t e = 0; e < tb.nEntries; ++e) wr32(p + tb.entries[e], L.moved(rd32(p + tb.entries[e])));
        p += tb.bytes;
    }
    for (uint32_t s = 0; s < r.nSlots; ++s) *r.slots[s].word = L.moved(r.slots[s].value);
    return true;
}

void hotRowRestore(const HotRow& r) {
    for (uint32_t s = 0; s < r.nSlots; ++s) *r.slots[s].word = r.slots[s].value;
}

}  // namespace mod
}  // namespace ot
