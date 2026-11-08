// [I] Modules loaded from the card (modules/module_files.h).
#include "modules/module_files.h"

#include <cstdio>
#include <cstring>

#include "hal_api.h"
#include "modules/module.h"
#include "modules/synth_descriptor.h"
#include "sdram_bss.h"

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

void shareFastBlock(const OtmShareCut (*cuts)[kOtmMaxShareCuts], const uint32_t* nCuts, int n, uint32_t cap,
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

namespace {

// [I] What the boot scan found and which card synths are in the pool (main
// loop only; SDRAM).
struct CardState {
    const OtmPool* pool;                 // the boot scan's (it outlives the scan)
    OtmLoadReport* r;
    IModule* mod[OtmLoadReport::kMax];   // card synths now loaded (by report item)
    CardEngine eng[kMaxCardEngines];
    int nEng;
    uint32_t keepUsed, keepFast;         // what the modules loaded for good take
    // Row cuts of the files being loaded (here, not on the stack).
    OtmShareCut cuts[OtmLoadReport::kMax][kOtmMaxShareCuts];
    uint32_t nCuts[OtmLoadReport::kMax];
};
OT_SDRAM_BSS CardState g_card;

void copyName(char* out, size_t cap, const char* s) { std::snprintf(out, cap, "%s", s ? s : ""); }

// Reads and checks Modules/<item's file>, and its row cuts.
OtmError readChecked(IStorage& fs, const OtmPool& pool, const char* name, uint8_t* scratch, uint32_t cap,
                     uint32_t* len, OtmHeader* hdr, OtmShareCut* cuts, uint32_t* nCuts) {
    OtmError e = readModuleFile(fs, name, scratch, cap, len);
    if (e == OtmError::None) e = otmCheck(scratch, *len, pool.core, hdr);
    if (e != OtmError::None) return e;
    if (cuts && nCuts) {
        const uint32_t n = hdr->nCuts < kOtmMaxShareCuts ? hdr->nCuts : kOtmMaxShareCuts;
        *nCuts = 0;
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t c = otmCutBytes(scratch, *hdr, k);
            if (c > 0xFFFFu) break;   // past 16 bits (never: a row is inside the pool)
            cuts[(*nCuts)++] = OtmShareCut(c);
        }
    }
    return OtmError::None;
}

// Loads the file in scratch at pool offset *used (the row's first `cut`
// bytes at fast offset *fastUsed) and enters it; *used / *fastUsed move past
// it. nullptr = no room or a bad file (it.error says).
IModule* place(const OtmPool& pool, OtmLoadReport::Item& it, const uint8_t* scratch, uint32_t len,
               const OtmHeader& hdr, uint32_t cut, uint32_t* used, uint32_t* fastUsed) {
    const uint32_t at = (pool.base + *used + hdr.align - 1) & ~(hdr.align - 1);
    const uint32_t off = at - pool.base;
    if (off > pool.cap) {
        it.error = OtmError::TooBig;
        return nullptr;
    }
    // The row up to its share in the fast block (back to back: every cut is
    // a multiple of 8), the rest of the row after its main part.
    const uint32_t foff = *fastUsed;
    const OtmBlock fast{cut ? pool.fastMem + foff : nullptr, cut ? pool.fastCap - foff : 0u,
                        cut ? pool.fastBase + foff : 0u};
    const OtmBlock main{pool.mem + off, pool.cap - off, at};
    uint32_t entry = 0;
    it.error = otmLoad(scratch, len, pool.core, fast, main, cut, &entry);
    if (it.error != OtmError::None) return nullptr;
    const uint32_t bytes = otmMainSpan(hdr, cut);
    if (pool.sync) {
        pool.sync(pool.mem + off, bytes);
        if (cut) pool.sync(fast.mem, cut);
    }
    *used = off + bytes;
    if (cut) *fastUsed = foff + cut;
    it.fastBytes = cut;
    return &pool.enter(entry);
}

// Lists m's engines as item i's; false = not a synth module.
bool listEngines(IModule& m, int i) {
    ModuleHost& h = ModuleHost::instance();
    bool synth = false;
    for (int k = 0; const SynthDescriptor* d = m.synthDescriptor(k); ++k) {
        synth = true;
        // A compiled-in engine or one an earlier file serves wins.
        if (h.synthDescriptor(d->engineId) || cardEngine(d->engineId) || g_card.nEng >= kMaxCardEngines) continue;
        CardEngine& e = g_card.eng[g_card.nEng++];
        e.engineId = d->engineId;
        copyName(e.name, sizeof(e.name), d->name);
        copyName(e.displayName, sizeof(e.displayName), d->displayName ? d->displayName : d->name);
        e.file = uint8_t(i);
    }
    return synth;
}

bool fileWanted(int i, const int* want, int nWant) {
    for (int k = 0; k < nWant; ++k)
        if (want[k] == i) return true;
    return false;
}

// The report items serving engines[0 .. n), in that order, once each.
int wantedFiles(const int32_t* engines, int n, int* want) {
    int nWant = 0;
    for (int k = 0; k < n; ++k)
        if (const CardEngine* e = cardEngine(engines[k]))
            if (!fileWanted(e->file, want, nWant)) want[nWant++] = e->file;
    return nWant;
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
    for (IModule* m : g_card.mod)   // a scan again: the card synths of the last one leave
        if (m) ModuleHost::instance().removeLoaded(*m);
    g_card = CardState{};
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
    // Pass 1: each file once at the pool's start (no fast part) to see what
    // it is, and its row cuts.
    OtmShareCut (*cuts)[kOtmMaxShareCuts] = g_card.cuts;
    uint32_t* nCuts = g_card.nCuts;
    int keep[OtmLoadReport::kMax], nKeep = 0;
    for (int i = 0; i < names.n; ++i) {
        OtmLoadReport::Item& it = r.items[r.n++];
        it = OtmLoadReport::Item{};
        std::strcpy(it.file, names.name[i]);
        uint32_t len = 0, used = 0, fastUsed = 0;
        OtmHeader hdr;
        it.error = readChecked(fs, pool, names.name[i], scratch, scratchCap, &len, &hdr, cuts[nKeep], &nCuts[nKeep]);
        if (it.error != OtmError::None) continue;
        it.rowBytes = hdr.rowBytes;
        IModule* m = place(pool, it, scratch, len, hdr, 0, &used, &fastUsed);
        if (!m) continue;
#if defined(OT_POOL_TEST) && OT_POOL_TEST
        // [I] measurement images: every module stays (a card synth replaces
        // the compiled-in one with its id, ModuleHost::addLoaded).
        (void)listEngines;
        keep[nKeep++] = i;
#else
        it.synth = listEngines(*m, i);
        if (!it.synth) keep[nKeep++] = i;
#endif
    }
    // Pass 2: the modules that stay, back to back, sharing the fast block.
    uint32_t share[OtmLoadReport::kMax] = {};
    if (pool.fastMem) shareFastBlock(cuts, nCuts, nKeep, pool.fastCap, share);
    int loaded = 0;
    for (int k = 0; k < nKeep; ++k) {
        OtmLoadReport::Item& it = r.items[keep[k]];
        uint32_t len = 0;
        OtmHeader hdr;
        it.error = readChecked(fs, pool, it.file, scratch, scratchCap, &len, &hdr, nullptr, nullptr);
        if (it.error != OtmError::None) continue;
        uint32_t used = r.used, fastUsed = r.fastUsed;
        IModule* m = place(pool, it, scratch, len, hdr, share[k], &used, &fastUsed);
        if (!m) continue;
        if (!ModuleHost::instance().addLoaded(*m)) {
            it.refused = true;   // its space goes to the next file
            it.fastBytes = 0;
            continue;
        }
        it.loaded = true;
        r.used = used;
        r.fastUsed = fastUsed;
        ++loaded;
    }
    if (pool.resize) pool.resize(r.used);
    g_card.pool = &pool;
    g_card.r = &r;
    g_card.keepUsed = r.used;
    g_card.keepFast = r.fastUsed;
    ++r.serial;
    return loaded;
}

int cardEngineCount() { return g_card.nEng; }

const CardEngine* cardEngineAt(int k) { return k >= 0 && k < g_card.nEng ? &g_card.eng[k] : nullptr; }

const CardEngine* cardEngine(int32_t engineId) {
    for (int k = 0; k < g_card.nEng; ++k)
        if (g_card.eng[k].engineId == engineId) return &g_card.eng[k];
    return nullptr;
}

bool cardEngineLoaded(int32_t engineId) {
    const CardEngine* e = cardEngine(engineId);
    return e && g_card.mod[e->file];
}

bool cardSynthsDiffer(const int32_t* engines, int n) {
    if (!g_card.pool) return false;
    int want[OtmLoadReport::kMax];
    const int nWant = wantedFiles(engines, n, want);
    for (int i = 0; i < g_card.r->n; ++i)
        if (g_card.r->items[i].synth && (g_card.mod[i] != nullptr) != fileWanted(i, want, nWant)) return true;
    return false;
}

bool loadCardSynths(IStorage& fs, const int32_t* engines, int n, uint8_t* scratch, uint32_t scratchCap) {
    if (!g_card.pool) return true;
    if (!cardSynthsDiffer(engines, n)) {
        for (int k = 0; k < n; ++k)
            if (cardEngine(engines[k]) && !cardEngineLoaded(engines[k])) return false;
        return true;
    }
    const OtmPool& pool = *g_card.pool;
    OtmLoadReport& r = *g_card.r;
    ++r.serial;
    // Every card synth leaves, then the wanted ones load again from the
    // first free byte (a new set gets the fast block shared out anew).
    for (int i = 0; i < r.n; ++i)
        if (g_card.mod[i]) {
            ModuleHost::instance().removeLoaded(*g_card.mod[i]);
            g_card.mod[i] = nullptr;
            r.items[i].loaded = false;
            r.items[i].fastBytes = 0;
        }
    r.used = g_card.keepUsed;
    r.fastUsed = g_card.keepFast;
    r.noRoom = false;
    int want[OtmLoadReport::kMax];
    const int nWant = wantedFiles(engines, n, want);
    bool ok = true;
    if (nWant > 0) {
        if (pool.resize && !pool.resize(pool.cap)) {
            r.noRoom = true;
            return false;
        }
        OtmShareCut (*cuts)[kOtmMaxShareCuts] = g_card.cuts;
        uint32_t* nCuts = g_card.nCuts;
        for (int k = 0; k < nWant; ++k) {
            nCuts[k] = 0;
            OtmLoadReport::Item& it = r.items[want[k]];
            uint32_t len = 0;
            OtmHeader hdr;
            it.error = readChecked(fs, pool, it.file, scratch, scratchCap, &len, &hdr, cuts[k], &nCuts[k]);
        }
        uint32_t share[OtmLoadReport::kMax] = {};
        if (pool.fastMem) shareFastBlock(cuts, nCuts, nWant, pool.fastCap - g_card.keepFast, share);
        for (int k = 0; k < nWant; ++k) {
            OtmLoadReport::Item& it = r.items[want[k]];
            it.refused = false;
            uint32_t len = 0;
            OtmHeader hdr;
            if (it.error == OtmError::None)
                it.error = readChecked(fs, pool, it.file, scratch, scratchCap, &len, &hdr, nullptr, nullptr);
            uint32_t used = r.used, fastUsed = r.fastUsed;
            IModule* m = it.error == OtmError::None ? place(pool, it, scratch, len, hdr, share[k], &used, &fastUsed)
                                                    : nullptr;
            if (m && !ModuleHost::instance().addLoaded(*m, true)) {
                it.refused = true;
                m = nullptr;
            }
            if (!m) {
                it.fastBytes = 0;
                ok = false;
                continue;
            }
            it.loaded = true;
            g_card.mod[want[k]] = m;
            r.used = used;
            r.fastUsed = fastUsed;
        }
    }
    if (pool.resize) pool.resize(r.used);
    return ok;
}

}  // namespace mod
}  // namespace ot
