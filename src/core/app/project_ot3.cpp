// 3.x project sidecar `project.ot3x` (docs/specs/3x-01-ot3-format.md): what
// the Tracker Mini project files cannot hold -- a synth slot on a module
// engine, the modules a project uses, module data -- in one OT3 container
// next to them. Absent file = the Mini mapping for every slot.
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "formats/file_io.h"
#include "formats/ot3.h"
#include "model/project.h"
#include "modules/module.h"
#include "ot_profile.h"
#include "sdram_bss.h"

namespace ot {

namespace {

// Module chunks of modules not in this build, written back unchanged (in
// file order). 1 KiB in 3.0: the room left in .sdram_bss (spec 3x-01).
constexpr uint32_t kOpaqueMax = 1024;
OT_SDRAM_BSS uint8_t g_opaque[kOpaqueMax];
uint32_t g_opaqueLen = 0;
bool g_opaqueLost = false;   // a foreign chunk did not fit: not kept

#if OT_SLOT_INSERTS
// Each slot's settings and Off of the inserts it had before, so picking one
// again brings them back (not saved; a new or loaded project starts clean).
// A ninth kind takes the last entry.
struct InsertMemo {
    char id[4];   // all zero = free
    audio::EffectSettings set;
    bool off;
};
constexpr int kInsertMemoMax = 8;
OT_SDRAM_BSS InsertMemo g_insertMemo[kSynthSlotCount][kInsertMemoMax];

InsertMemo* insertMemo(int s, const char* id, bool make) {
    static const char kFree[4] = {};
    for (InsertMemo& m : g_insertMemo[s])
        if (std::memcmp(m.id, id, 4) == 0) return &m;
    if (!make) return nullptr;
    for (InsertMemo& m : g_insertMemo[s])
        if (std::memcmp(m.id, kFree, 4) == 0) return &m;
    return &g_insertMemo[s][kInsertMemoMax - 1];
}
#endif

constexpr uint8_t kWriter[4] = {3, 0, 0, 0};

bool isModuleTag(uint16_t t) { return t >= ot3::kTagModuleFirst && t <= ot3::kTagModuleLast; }

void idText(const char* id, char out[5]) {
    for (int i = 0; i < 4; ++i) out[i] = id[i] >= 32 && id[i] < 127 ? id[i] : '?';
    out[4] = 0;
}

}  // namespace

int32_t App::slotEngineId(int s) const {
    if (s < 0 || s >= kSynthSlotCount) return 0;
    if (slotEngine_[s]) return slotEngine_[s];
    const SynthSlotRecord r = project.synthSlot(s);
    return r.model < uint32_t(kSynthModelCount) ? synthEngineId(SynthModel(r.model)) : 0;
}

void App::setSlotEngine(int s, int32_t engineId) {
    if (s < 0 || s >= kSynthSlotCount) return;
    const int32_t v = engineId >= 0x80 ? engineId : 0;
    if (slotEngine_[s] != v) {
        slotEngine_[s] = v;
        sidecarDirty_ = true;
    }
    engine.synth().setSlotEngine(s, v);
}

void App::setSlotArpGate(int s, int gate) {
    if (s < 0 || s >= kSynthSlotCount) return;
    const uint8_t g = uint8_t(gate < 1 ? 1 : (gate > 100 ? 100 : gate));
    if (slotArpGate_[s] == g) return;
    slotArpGate_[s] = g;
    sidecarDirty_ = true;
}

#if OT_SLOT_INSERTS
audio::IEffect* App::slotInsertEffect(int s) const {
    const char* id = slotInsertId(s);
    return id ? mod::ModuleHost::instance().effectById(id) : nullptr;
}

const char* App::slotInsertId(int s) const {
    if (s < 0 || s >= kSynthSlotCount) return nullptr;
    const char* id = slotInsert_[s].id;
    return id[0] || id[1] || id[2] || id[3] ? id : nullptr;
}

audio::EffectSettings App::slotInsertSettings(int s) const {
    return s >= 0 && s < kSynthSlotCount ? slotInsert_[s].set : audio::EffectSettings{};
}

void App::applySlotInsert(int s) {
    engine.synth().setSlotInsert(s, slotInsertEffect(s), slotInsert_[s].set, slotInsert_[s].off);
}

bool App::slotInsertOff(int s) const { return s >= 0 && s < kSynthSlotCount && slotInsertId(s) && slotInsert_[s].off; }

void App::setSlotInsertOff(int s, bool off) {
    if (!slotInsertId(s) || slotInsert_[s].off == off) return;
    slotInsert_[s].off = off;
    sidecarDirty_ = true;
    applySlotInsert(s);
}

void App::setSlotInsertKind(int s, const char* effectId) {
    if (s < 0 || s >= kSynthSlotCount) return;
    if (const char* cur = slotInsertId(s)) {   // keep the one we leave
        InsertMemo* m = insertMemo(s, cur, true);
        std::memcpy(m->id, cur, 4);
        m->set = slotInsert_[s].set;
        m->off = slotInsert_[s].off;
    }
    SlotInsertCfg c = {};
    if (effectId && effectId[0]) {
        std::memcpy(c.id, effectId, 4);
        if (const InsertMemo* m = insertMemo(s, c.id, false)) {
            c.set = m->set;
            c.off = m->off;
        } else if (audio::IEffect* fx = mod::ModuleHost::instance().effectById(c.id)) {
            c.set = audio::effectDefaults(*fx);
        }
    }
    if (std::memcmp(&c, &slotInsert_[s], sizeof c) != 0) sidecarDirty_ = true;
    slotInsert_[s] = c;
    applySlotInsert(s);
}

void App::setSlotInsertValue(int s, int i, uint8_t v) {
    audio::IEffect* fx = slotInsertEffect(s);
    if (!fx || i < 0 || i >= fx->paramCount() || i >= audio::kEffectMaxParams) return;
    const uint8_t max = fx->param(i).max;
    const uint8_t c = v > max ? max : v;
    if (slotInsert_[s].set.v[i] == c) return;
    slotInsert_[s].set.v[i] = c;
    sidecarDirty_ = true;
    applySlotInsert(s);
}
#endif

bool App::sidecarDirty() const {
    if (sidecarReadOnly_) return false;
    if (sidecarDirty_) return true;
    mod::ModuleHost& h = mod::ModuleHost::instance();
    for (int i = 0; i < h.count(); ++i)
        if (mod::IProjectSection* sec = h.at(i)->section())
            if (sec->dirty()) return true;
    return false;
}

void App::resetSidecar() {
    for (int s = 0; s < kSynthSlotCount; ++s) {
        slotEngine_[s] = 0;
        engine.synth().setSlotEngine(s, 0);
        slotArpGate_[s] = 100;
#if OT_SLOT_INSERTS
        slotInsert_[s] = SlotInsertCfg{};
        applySlotInsert(s);
#endif
    }
#if OT_SLOT_INSERTS
    std::memset(g_insertMemo, 0, sizeof(g_insertMemo));
#endif
    mod::ModuleHost& h = mod::ModuleHost::instance();
    for (int i = 0; i < h.count(); ++i)
        if (mod::IProjectSection* sec = h.at(i)->section()) sec->reset();
    g_opaqueLen = 0;
    g_opaqueLost = false;
    sidecarDirty_ = false;
    sidecarReadOnly_ = false;
}

bool App::encodeSidecar(uint8_t* out, uint32_t cap, uint32_t* len) {
    *len = 0;
    mod::ModuleHost& h = mod::ModuleHost::instance();
    // The first half of out[] is the file, the second half holds the module
    // payloads while the MODS list is built.
    const uint32_t half = cap / 2;
    uint8_t* tmp = out + half;
    const uint32_t tcap = half < 4096 ? half : 4096;
    ot3::Writer w;
    w.begin(out, half, ot3::kKindProjectSidecar, kWriter);
    const uint8_t info[4] = {uint8_t(kSynthSlotCount), 0, 0, 0};
    w.chunk(ot3::kTagInfo, ot3::kFlagCritical, info, sizeof info);
    bool any = false;
    for (int s = 0; s < kSynthSlotCount; ++s) {
        if (!slotEngine_[s]) continue;
        ot3::SlotEngine se{};
        se.slot = uint8_t(s);
        const SynthSlotRecord r = project.synthSlot(s);
        se.miniFallback = uint8_t(r.model < uint32_t(kSynthModelCount) ? r.model : 0);
        se.engineId = slotEngine_[s];
        for (int i = 0; i < h.count(); ++i)
            if (h.at(i)->synthEngine(slotEngine_[s])) {
                std::memcpy(se.moduleId, h.at(i)->info().id, 4);
                se.moduleVersion = h.at(i)->info().version;
            }
        w.chunk(ot3::kTagSlotEngine, 0, &se, sizeof se);
        any = true;
    }
    for (int s = 0; s < kSynthSlotCount; ++s) {
        if (slotArpGate_[s] >= 100) continue;
        const ot3::SlotArp sa = {uint8_t(s), slotArpGate_[s], {0, 0}};
        w.chunk(ot3::kTagSlotArp, 0, &sa, sizeof sa);
        any = true;
    }
#if OT_SLOT_INSERTS
    for (int s = 0; s < kSynthSlotCount; ++s) {
        if (!slotInsertId(s)) continue;
        ot3::SlotInsert si{};
        si.slot = uint8_t(s);
        si.rsv = slotInsert_[s].off ? ot3::kSlotInsertOff : 0;
        std::memcpy(si.effectId, slotInsert_[s].id, 4);
        std::memcpy(si.set, slotInsert_[s].set.v, sizeof si.set);
        w.chunk(ot3::kTagSlotInsert, 0, &si, sizeof si);
        any = true;
    }
#endif
    // Module sections with data: MODS lists them, then one chunk each.
    struct Pending {
        uint16_t tag, version;
        char id[4];
        uint32_t len, off;
    } pend[mod::ModuleHost::kMaxModules];
    ot3::ModEntry mods[mod::ModuleHost::kMaxModules];
    int nm = 0;
    uint32_t used = 0;
    for (int i = 0; i < h.count(); ++i) {
        mod::IProjectSection* sec = h.at(i)->section();
        if (!sec || used >= tcap) continue;
        const uint32_t n = sec->encode(tmp + used, tcap - used);
        if (!n) continue;
        Pending& p = pend[nm];
        p.tag = sec->tag();
        p.version = sec->version();
        std::memcpy(p.id, h.at(i)->info().id, 4);
        p.len = n;
        p.off = used;
        used += (n + 3u) & ~3u;
        ot3::ModEntry& e = mods[nm++];
        std::memcpy(e.id, h.at(i)->info().id, 4);
        e.version = h.at(i)->info().version;
        e.enabled = 1;
        e.rsv = 0;
    }
    if (nm) w.chunk(ot3::kTagMods, ot3::kFlagCritical, mods, uint32_t(nm) * sizeof(ot3::ModEntry));
    for (int k = 0; k < nm; ++k) {
        ot3::ModuleChunkHeader mh{};
        std::memcpy(mh.id, pend[k].id, 4);
        mh.version = pend[k].version;
        w.chunk2(pend[k].tag, ot3::kFlagOpaque, &mh, sizeof mh, tmp + pend[k].off, pend[k].len);
    }
    // Chunks of modules not in this build, unchanged and in their file order.
    for (uint32_t pos = 0; pos + ot3::kChunkHeaderSize <= g_opaqueLen;) {
        const uint8_t* c = g_opaque + pos;
        const uint16_t tag = uint16_t(c[0] | (c[1] << 8));
        const uint16_t fl = uint16_t(c[2] | (c[3] << 8));
        uint32_t l = 0;
        std::memcpy(&l, c + 4, 4);
        w.chunk(tag, fl, c + ot3::kChunkHeaderSize, l);
        pos += ot3::kChunkHeaderSize + ((l + 3u) & ~3u);
    }
    any = any || nm > 0 || g_opaqueLen > 0;
    const uint32_t size = w.end();
    if (!size) return false;
    *len = any ? size : 0;
    return true;
}

bool App::decodeSidecar(const uint8_t* p, uint32_t len) {
    resetSidecar();
    ot3::Reader r;
    const ot3::Error e = r.open(p, len, ot3::kKindProjectSidecar);
    if (e != ot3::Error::kOk && e != ot3::Error::kNewer) return false;
    if (e == ot3::Error::kNewer) sidecarReadOnly_ = true;
    mod::ModuleHost& h = mod::ModuleHost::instance();
    ot3::Chunk c;
    while (r.next(c)) {
        if (c.tag == ot3::kTagInfo) continue;
        if (c.tag == ot3::kTagSlotEngine && c.len >= 8) {
            ot3::SlotEngine se{};
            std::memcpy(&se, c.payload, c.len < sizeof se ? c.len : sizeof se);
            if (se.slot < kSynthSlotCount && se.engineId >= 0x80) {
                slotEngine_[se.slot] = se.engineId;
                engine.synth().setSlotEngine(se.slot, se.engineId);
            }
            continue;
        }
        if (c.tag == ot3::kTagSlotArp && c.len >= 2) {
            if (c.payload[0] < kSynthSlotCount && c.payload[1] >= 1 && c.payload[1] <= 100)
                slotArpGate_[c.payload[0]] = c.payload[1];
            continue;
        }
#if OT_SLOT_INSERTS
        if (c.tag == ot3::kTagSlotInsert && c.len >= 6) {
            ot3::SlotInsert si{};
            std::memcpy(&si, c.payload, c.len < sizeof si ? c.len : sizeof si);
            if (si.slot < kSynthSlotCount && (si.effectId[0] || si.effectId[1] || si.effectId[2] || si.effectId[3])) {
                SlotInsertCfg& cfg = slotInsert_[si.slot];
                std::memcpy(cfg.id, si.effectId, 4);
                std::memcpy(cfg.set.v, si.set, sizeof si.set);
                cfg.off = (si.rsv & ot3::kSlotInsertOff) != 0;
                if (audio::IEffect* fx = mod::ModuleHost::instance().effectById(cfg.id))
                    for (int i = 0; i < audio::kEffectMaxParams; ++i) {
                        const uint8_t max = i < fx->paramCount() ? fx->param(i).max : 0;
                        if (cfg.set.v[i] > max) cfg.set.v[i] = max;
                    }
                applySlotInsert(si.slot);
            }
            continue;
        }
#endif
        if (c.tag == ot3::kTagMods) {
            for (uint32_t o = 0; o + sizeof(ot3::ModEntry) <= c.len; o += sizeof(ot3::ModEntry)) {
                const char* id = reinterpret_cast<const char*>(c.payload + o);
                if (!h.find(id) && !loadNotice_[0]) {
                    char t[5];
                    idText(id, t);
                    std::snprintf(loadNotice_, sizeof(loadNotice_), "Project uses module %s (not in this build)", t);
                }
            }
            continue;
        }
        if (isModuleTag(c.tag) && c.len >= sizeof(ot3::ModuleChunkHeader)) {
            ot3::ModuleChunkHeader mh;
            std::memcpy(&mh, c.payload, sizeof mh);
            const mod::IModule* m = h.find(mh.id);
            mod::IProjectSection* sec = m ? const_cast<mod::IModule*>(m)->section() : nullptr;
            if (sec && sec->tag() == c.tag) {
                sec->decode(c.payload + sizeof mh, c.len - uint32_t(sizeof mh), mh.version);
                continue;
            }
        } else if (!(c.flags & ot3::kFlagCritical)) {
            continue;   // an unknown non-critical core chunk of a newer writer: dropped
        } else {
            sidecarReadOnly_ = true;   // unknown critical chunk: never re-save over it
        }
        // Keep the chunk as it was (a module not in this build).
        const uint32_t need = ot3::kChunkHeaderSize + ((c.len + 3u) & ~3u);
        if (g_opaqueLen + need > kOpaqueMax) {
            g_opaqueLost = true;
            continue;
        }
        uint8_t* d = g_opaque + g_opaqueLen;
        d[0] = uint8_t(c.tag);
        d[1] = uint8_t(c.tag >> 8);
        d[2] = uint8_t(c.flags);
        d[3] = uint8_t(c.flags >> 8);
        std::memcpy(d + 4, &c.len, 4);
        std::memcpy(d + ot3::kChunkHeaderSize, c.payload, c.len);
        for (uint32_t i = c.len; i < ((c.len + 3u) & ~3u); ++i) d[ot3::kChunkHeaderSize + i] = 0;
        g_opaqueLen += need;
    }
    if (r.error() != ot3::Error::kOk && r.error() != ot3::Error::kNewer) {
        sidecarReadOnly_ = true;
        return false;
    }
    if (g_opaqueLost && !loadNotice_[0])
        std::snprintf(loadNotice_, sizeof(loadNotice_), "Module data too large: not kept");
    return true;
}

void App::loadSidecarFile(const char* path) {
    IStorage& f = fs();
    resetSidecar();
    if (!f.exists(path)) return;
    uint32_t got = 0;
    const bool read = mem_.scratch && mem_.scratchBytes >= ot3::kMaxFile + 4 &&
                      readWholeFile(f, path, mem_.scratch, ot3::kMaxFile + 1, &got) == FmtStatus::Ok && got <= ot3::kMaxFile;
    if (read && decodeSidecar(mem_.scratch, got)) return;
    // Damaged: keep the file aside, open with the Mini mapping.
    resetSidecar();
    char bad[120];
    std::snprintf(bad, sizeof(bad), "%s.bad", path);
    if (f.exists(bad)) f.remove(bad);
    f.rename(path, bad);
    if (!loadNotice_[0]) std::snprintf(loadNotice_, sizeof(loadNotice_), "OpenTracker data damaged, skipped");
}

bool App::saveSidecarFile(const char* path) {
    if (sidecarReadOnly_) return true;
    IStorage& f = fs();
    uint32_t len = 0;
    if (!mem_.scratch || mem_.scratchBytes < 2 * ot3::kMaxFile) return false;
    if (!encodeSidecar(mem_.scratch, 2 * ot3::kMaxFile, &len)) return false;
    sidecarDirty_ = false;
    if (!len) return !f.exists(path) || f.remove(path);
    char tmp[120];
    std::snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (writeWholeFile(f, tmp, mem_.scratch, len) != FmtStatus::Ok) {
        f.remove(tmp);
        sidecarDirty_ = true;
        return false;
    }
    if (f.exists(path) && !f.remove(path)) return false;
    return f.rename(tmp, path);
}

}  // namespace ot
