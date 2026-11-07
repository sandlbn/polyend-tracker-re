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

}  // namespace

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
    int loaded = 0;
    for (int i = 0; i < names.n; ++i) {
        OtmLoadReport::Item& it = r.items[r.n++];
        std::strcpy(it.file, names.name[i]);
        it.refused = false;
        it.fastBytes = 0;
        it.fastInBlock = false;
        char path[64];
        std::snprintf(path, sizeof(path), "%s/%s", kModulesDir, names.name[i]);
        const IStorage::Handle h = fs.open(path, IStorage::kRead);
        if (h < 0) {
            it.error = OtmError::Truncated;
            continue;
        }
        const int32_t len = fs.size(h);
        bool ok = len > 0 && uint32_t(len) <= scratchCap && fs.read(h, scratch, uint32_t(len)) == len;
        fs.close(h);
        if (!ok) {
            it.error = len > 0 && uint32_t(len) > scratchCap ? OtmError::TooBig : OtmError::Truncated;
            continue;
        }
        // The next free spot at the module's alignment.
        OtmHeader hdr;
        it.error = otmCheck(scratch, uint32_t(len), pool.core, &hdr);
        if (it.error != OtmError::None) continue;
        it.fastBytes = hdr.fastBytes;
        const uint32_t at = (pool.base + r.used + hdr.align - 1) & ~(hdr.align - 1);
        const uint32_t off = at - pool.base;
        if (off > pool.cap) {
            it.error = OtmError::TooBig;
            continue;
        }
        // The fast part in the fast block when it fits there, else in front of the main part.
        const uint32_t fat = (pool.fastBase + r.fastUsed + hdr.align - 1) & ~(hdr.align - 1);
        const uint32_t foff = fat - pool.fastBase;
        const bool inFast = hdr.fastBytes && pool.fastMem && foff <= pool.fastCap &&
                            hdr.fastBytes <= pool.fastCap - foff;
        const uint32_t span = hdr.fastBytes && !inFast ? otmFastSpan(hdr) : 0u;
        if (span > pool.cap - off) {
            it.error = OtmError::TooBig;
            continue;
        }
        const OtmBlock fast = inFast ? OtmBlock{pool.fastMem + foff, pool.fastCap - foff, fat}
                                     : OtmBlock{pool.mem + off, span, at};
        const OtmBlock main{pool.mem + off + span, pool.cap - off - span, at + span};
        uint32_t entry = 0;
        it.error = otmLoad(scratch, uint32_t(len), pool.core, fast, main, &entry);
        if (it.error != OtmError::None) continue;
        const uint32_t bytes = span + (hdr.imageBytes - hdr.fastBytes) + hdr.bssBytes;
        if (pool.sync) {
            pool.sync(pool.mem + off, bytes);
            if (inFast) pool.sync(fast.mem, hdr.fastBytes);
        }
        IModule& m = pool.enter(entry);
        if (!ModuleHost::instance().addLoaded(m)) {
            it.refused = true;   // its space goes to the next file
            continue;
        }
        r.used = off + bytes;
        if (inFast) r.fastUsed = foff + hdr.fastBytes;
        it.fastInBlock = inFast;
        ++loaded;
    }
    if (pool.resize) pool.resize(r.used);
    return loaded;
}

}  // namespace mod
}  // namespace ot
