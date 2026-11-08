// [I] Modules loaded from the card (modules/module_files.h).
#include "modules/module_files.h"

#include <cstdio>
#include <cstring>

#include "hal_api.h"
#include "modules/module.h"

namespace ot {
namespace mod {

namespace {

// The first kMax names in name order (kept sorted), whatever the directory order.
struct Names {
    char name[OtmLoadReport::kMax][40];
    int n = 0;
    int skipped = 0;   // .otm files past the first kMax
};

bool isOtm(const char* s) {
    const size_t n = std::strlen(s);
    if (n < 5 || n >= sizeof(Names::name[0])) return false;
    const char* e = s + n - 4;
    return (e[0] == '.') && (e[1] == 'o' || e[1] == 'O') && (e[2] == 't' || e[2] == 'T') &&
           (e[3] == 'm' || e[3] == 'M');
}

bool collect(const IStorage::DirEntry& e, void* ctx) {
    Names& n = *static_cast<Names*>(ctx);
    if (e.isDir || e.name[0] == '.' || !isOtm(e.name)) return true;
    if (n.n == OtmLoadReport::kMax) {
        ++n.skipped;
        if (std::strcmp(e.name, n.name[n.n - 1]) >= 0) return true;
        --n.n;   // the last name makes room
    }
    int i = n.n++;
    for (; i > 0 && std::strcmp(n.name[i - 1], e.name) > 0; --i) std::strcpy(n.name[i], n.name[i - 1]);
    std::strcpy(n.name[i], e.name);
    return true;
}

// Reads Modules/<name> into scratch.
OtmError readModuleFile(IStorage& fs, const char* name, uint8_t* scratch, uint32_t cap, uint32_t* len) {
    char path[64];
    std::snprintf(path, sizeof(path), "%s/%s", kModulesDir, name);
    const IStorage::Handle h = fs.open(path, IStorage::kRead);
    if (h < 0) return OtmError::Truncated;
    const int32_t n = fs.size(h);
    const bool ok = n > 0 && uint32_t(n) <= cap && fs.read(h, scratch, uint32_t(n)) == n;
    fs.close(h);
    if (!ok) return n > 0 && uint32_t(n) > cap ? OtmError::TooBig : OtmError::Truncated;
    *len = uint32_t(n);
    return OtmError::None;
}

}  // namespace

void shareFastBlock(const uint32_t (*cuts)[kOtmMaxShareCuts], const uint32_t* nCuts, int n, uint32_t cap,
                    uint32_t* share) {
    // Turns: each module in name order moves to its next cut while it fits;
    // a module whose next cut doesn't fit is done (its row is hottest first).
    uint32_t next[OtmLoadReport::kMax] = {}, used = 0;
    for (int i = 0; i < n; ++i) share[i] = 0;
    for (bool moved = true; moved;) {
        moved = false;
        for (int i = 0; i < n; ++i) {
            if (next[i] >= nCuts[i]) continue;
            const uint32_t grow = cuts[i][next[i]] - share[i];
            if (grow > cap - used) {
                next[i] = nCuts[i];
                continue;
            }
            used += grow;
            share[i] = cuts[i][next[i]++];
            moved = true;
        }
    }
}

int loadModuleFiles(IStorage& fs, const OtmPool& pool, uint8_t* scratch, uint32_t scratchCap,
                    OtmLoadReport* report) {
    OtmLoadReport local;
    OtmLoadReport& r = report ? *report : local;
    r.n = 0;
    r.used = 0;
    r.fastUsed = 0;
    r.noRoom = false;
    r.skipped = 0;
    if (!pool.mem || !pool.enter) return 0;
    // Name order, so the master inserts chain the same way on every boot.
    Names names;
    if (fs.listDir(kModulesDir, &collect, &names) < 0 || names.n == 0) return 0;
    r.skipped = names.skipped;
    // The whole block while loading, then only what the modules took: the
    // rest goes back to the stack. No block at all (a profile whose stack
    // leaves no room above the floor) is the same as no room.
    if (!pool.cap || (pool.resize && !pool.resize(pool.cap))) {
        r.noRoom = true;
        return 0;
    }
    // Pass 1: every file's row cuts, then the fast block shared out (shareFastBlock).
    uint32_t cuts[OtmLoadReport::kMax][kOtmMaxShareCuts];
    uint32_t nCuts[OtmLoadReport::kMax] = {};
    for (int i = 0; i < names.n; ++i) {
        OtmLoadReport::Item& it = r.items[r.n++];
        std::strcpy(it.file, names.name[i]);
        it.refused = false;
        it.fastBytes = 0;
        it.rowBytes = 0;
        uint32_t len = 0;
        it.error = readModuleFile(fs, names.name[i], scratch, scratchCap, &len);
        OtmHeader hdr;
        if (it.error == OtmError::None) it.error = otmCheck(scratch, len, pool.core, &hdr);
        if (it.error != OtmError::None) continue;
        it.rowBytes = hdr.rowBytes;
        nCuts[i] = hdr.nCuts < kOtmMaxShareCuts ? hdr.nCuts : kOtmMaxShareCuts;
        for (uint32_t k = 0; k < nCuts[i]; ++k) cuts[i][k] = otmCutBytes(scratch, hdr, k);
    }
    uint32_t share[OtmLoadReport::kMax] = {};
    if (pool.fastMem) shareFastBlock(cuts, nCuts, names.n, pool.fastCap, share);
    // Pass 2: load each with its share.
    int loaded = 0;
    for (int i = 0; i < names.n; ++i) {
        OtmLoadReport::Item& it = r.items[i];
        if (it.error != OtmError::None) continue;
        uint32_t len = 0;
        it.error = readModuleFile(fs, names.name[i], scratch, scratchCap, &len);
        OtmHeader hdr;
        if (it.error == OtmError::None) it.error = otmCheck(scratch, len, pool.core, &hdr);
        if (it.error != OtmError::None) continue;
        const uint32_t at = (pool.base + r.used + hdr.align - 1) & ~(hdr.align - 1);
        const uint32_t off = at - pool.base;
        if (off > pool.cap) {
            it.error = OtmError::TooBig;
            continue;
        }
        // The row up to its share in the fast block (back to back: every cut
        // is a multiple of 8), the rest of the row after its main part.
        const uint32_t cut = share[i];
        const uint32_t foff = r.fastUsed;
        const OtmBlock fast{cut ? pool.fastMem + foff : nullptr, cut ? pool.fastCap - foff : 0u,
                            cut ? pool.fastBase + foff : 0u};
        const OtmBlock main{pool.mem + off, pool.cap - off, at};
        uint32_t entry = 0;
        it.error = otmLoad(scratch, len, pool.core, fast, main, cut, &entry);
        if (it.error != OtmError::None) continue;
        const uint32_t bytes = otmMainSpan(hdr, cut);
        if (pool.sync) {
            pool.sync(pool.mem + off, bytes);
            if (cut) pool.sync(fast.mem, cut);
        }
        IModule& m = pool.enter(entry);
        if (!ModuleHost::instance().addLoaded(m)) {
            it.refused = true;   // its space goes to the next file
            continue;
        }
        r.used = off + bytes;
        if (cut) r.fastUsed = foff + cut;
        it.fastBytes = cut;
        ++loaded;
    }
    if (pool.resize) pool.resize(r.used);
    return loaded;
}

}  // namespace mod
}  // namespace ot
