// [I] Modules loaded from the card (modules/module_files.h, fork issue #21).
#include "modules/module_files.h"

#include <cstdio>
#include <cstring>

#include "hal_api.h"
#include "modules/module.h"

namespace ot {
namespace mod {

namespace {

struct Names {
    char name[OtmLoadReport::kMax][40];
    int n = 0;
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
    if (n.n >= OtmLoadReport::kMax) return false;
    std::strcpy(n.name[n.n++], e.name);
    return true;
}

}  // namespace

int loadModuleFiles(IStorage& fs, const OtmPool& pool, uint8_t* scratch, uint32_t scratchCap,
                    OtmLoadReport* report) {
    OtmLoadReport local;
    OtmLoadReport& r = report ? *report : local;
    r.n = 0;
    r.used = 0;
    r.noRoom = false;
    if (!pool.mem || !pool.cap || !pool.enter) return 0;
    Names names;
    if (fs.listDir(kModulesDir, &collect, &names) < 0 || names.n == 0) return 0;
    // The whole block while loading, then only what the modules took: the
    // rest goes back to the stack.
    if (pool.resize && !pool.resize(pool.cap)) {
        r.noRoom = true;
        return 0;
    }
    // Name order, so the master inserts chain the same way on every boot.
    for (int i = 1; i < names.n; ++i)
        for (int j = i; j > 0 && std::strcmp(names.name[j - 1], names.name[j]) > 0; --j) {
            char t[sizeof(names.name[0])];
            std::strcpy(t, names.name[j]);
            std::strcpy(names.name[j], names.name[j - 1]);
            std::strcpy(names.name[j - 1], t);
        }
    int loaded = 0;
    for (int i = 0; i < names.n; ++i) {
        OtmLoadReport::Item& it = r.items[r.n++];
        std::strcpy(it.file, names.name[i]);
        it.refused = false;
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
        const uint32_t at = (pool.base + r.used + hdr.align - 1) & ~(hdr.align - 1);
        const uint32_t off = at - pool.base;
        if (off > pool.cap) {
            it.error = OtmError::TooBig;
            continue;
        }
        uint32_t entry = 0;
        it.error = otmLoad(scratch, uint32_t(len), pool.core, pool.mem + off, pool.cap - off, at, &entry);
        if (it.error != OtmError::None) continue;
        const uint32_t bytes = hdr.imageBytes + hdr.bssBytes;
        if (pool.sync) pool.sync(pool.mem + off, bytes);
        IModule& m = pool.enter(entry);
        if (!ModuleHost::instance().addLoaded(m)) {
            it.refused = true;   // its space goes to the next file
            continue;
        }
        r.used = off + bytes;
        ++loaded;
    }
    if (pool.resize) pool.resize(r.used);
    return loaded;
}

}  // namespace mod
}  // namespace ot
