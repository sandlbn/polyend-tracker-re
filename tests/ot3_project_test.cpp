// The 3.x project sidecar project.ot3x (app/project_ot3.cpp, docs/specs/
// 3x-01-ot3-format.md) through the App: nothing is written while there is
// nothing to keep; a module section and a slot's module engine survive
// autosave, load, Save As and Open; chunks of modules not in this build are
// written back unchanged; an unknown module is named in a notice; a damaged
// file is set aside.
#include "test.h"

#include <cstring>
#include <string>
#include <vector>

#include "formats/crc32.h"
#include "formats/ot3.h"
#include "modules/module.h"
#include "modules/module_ui.h"
#include "ui_fakes.h"

using namespace ot;

namespace {

constexpr const char* kWsSidecar = "Workspace/project.ot3x";

// A test module with project data: 3 bytes, tag 0x4005.
class TestSection final : public mod::IProjectSection {
public:
    uint8_t v[3] = {};
    bool d = false;
    uint16_t tag() const override { return 0x4005; }
    uint16_t version() const override { return 1; }
    bool dirty() const override { return d; }
    uint32_t encode(uint8_t* buf, uint32_t cap) override {
        d = false;
        if (!v[0] && !v[1] && !v[2]) return 0;
        if (cap < 3) return 0;
        std::memcpy(buf, v, 3);
        return 3;
    }
    bool decode(const uint8_t* p, uint32_t len, uint16_t) override {
        std::memset(v, 0, 3);
        std::memcpy(v, p, len < 3 ? len : 3);
        return true;
    }
    void reset() override {
        std::memset(v, 0, 3);
        d = false;
    }
};
TestSection g_sec;

class TestModule final : public mod::IModule {
public:
    const mod::ModuleInfo& info() const override {
        static const mod::ModuleInfo i = {"XFX1", mod::kModuleApi, 100, "Test FX", mod::kCapSection, 0, 0, 0, 0, 0};
        return i;
    }
    bool attach(mod::ModuleHost&) override { return true; }
    mod::IProjectSection* section() override { return &g_sec; }
};
TestModule g_mod;

void registerTestModule() {
    static bool done = false;
    if (!done) done = mod::ModuleHost::instance().addForTest(g_mod);
}

std::string g_note;
void onNote(const char* t) { g_note = t; }

std::vector<ot3::Chunk> chunks(const std::vector<uint8_t>& f) {
    std::vector<ot3::Chunk> out;
    ot3::Reader r;
    if (r.open(f.data(), uint32_t(f.size()), ot3::kKindProjectSidecar) != ot3::Error::kOk) return out;
    ot3::Chunk c;
    while (r.next(c)) out.push_back(c);
    return out;
}

}  // namespace

TEST(sidecar_absent_until_needed) {
    registerTestModule();
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.autosave();
    CHECK(h.fs.files.count(kWsSidecar) == 0);
    CHECK(a.saveProjectAs("Plain"));
    CHECK(h.fs.files.count("Projects/Plain/project.ot3x") == 0);
    CHECK_EQ(a.slotEngineId(0), int32_t(kSynthEngineAcd));   // the Mini record
}

TEST(sidecar_section_and_slot_engine_round_trip) {
    registerTestModule();
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    g_sec.v[0] = 7;
    g_sec.v[2] = 9;
    g_sec.d = true;
    a.setSlotEngine(1, 0x81);
    CHECK(a.sidecarDirty());
    a.autosave();
    CHECK(!a.sidecarDirty());
    CHECK(h.fs.files.count(kWsSidecar) == 1);
    const std::vector<ot3::Chunk> cs = chunks(h.fs.files[kWsSidecar]);
    CHECK_EQ(int(cs.size()), 4);   // INFO, SLOT_ENGINE, MODS, the module chunk
    CHECK(cs.size() == 4 && cs[1].tag == ot3::kTagSlotEngine && cs[2].tag == ot3::kTagMods && cs[3].tag == 0x4005);
    // Load: both come back.
    g_sec.reset();
    a.setSlotEngine(1, 0);
    CHECK(a.loadWorkspace());
    CHECK_EQ(a.slotEngineId(1), int32_t(0x81));
    CHECK(g_sec.v[0] == 7 && g_sec.v[1] == 0 && g_sec.v[2] == 9);
    // Save As carries the file; New Project resets; Open restores.
    CHECK(a.saveProjectAs("Side"));
    CHECK(h.fs.files.count("Projects/Side/project.ot3x") == 1);
    CHECK(h.fs.files["Projects/Side/project.ot3x"] == h.fs.files[kWsSidecar]);
    CHECK(a.newProject());
    CHECK(g_sec.v[0] == 0 && g_sec.v[2] == 0);
    CHECK_EQ(a.slotEngineId(1), int32_t(kSynthEngineAcd));
    CHECK(h.fs.files.count(kWsSidecar) == 0);
    CHECK(a.openProject("Projects", "Side"));
    CHECK_EQ(a.slotEngineId(1), int32_t(0x81));
    CHECK(g_sec.v[0] == 7 && g_sec.v[2] == 9);
    // Back to defaults: the file goes away on the next autosave and save.
    g_sec.reset();
    g_sec.d = true;
    a.setSlotEngine(1, 0);
    a.autosave();
    CHECK(h.fs.files.count(kWsSidecar) == 0);
    CHECK(a.saveProject());
    CHECK(h.fs.files.count("Projects/Side/project.ot3x") == 0);
}

TEST(sidecar_keeps_chunks_of_absent_modules) {
    registerTestModule();
    Harness h;
    App& a = *h.app;
    a.ui.platform.notified = &onNote;
    CHECK(a.newProject());
    // A file from a build with module ZZZZ (chunk 0x4321) and module NONE in MODS.
    std::vector<uint8_t> f(512);
    ot3::Writer w;
    const uint8_t wr[4] = {3, 1, 0, 0};
    w.begin(f.data(), uint32_t(f.size()), ot3::kKindProjectSidecar, wr);
    const uint8_t info[4] = {3, 0, 0, 0};
    w.chunk(ot3::kTagInfo, ot3::kFlagCritical, info, 4);
    ot3::ModEntry me[2] = {};
    std::memcpy(me[0].id, "ZZZZ", 4);
    std::memcpy(me[1].id, "NONE", 4);
    w.chunk(ot3::kTagMods, ot3::kFlagCritical, me, sizeof me);
    ot3::ModuleChunkHeader mh{};
    std::memcpy(mh.id, "ZZZZ", 4);
    mh.version = 3;
    const uint8_t pay[6] = {1, 2, 3, 4, 5, 6};
    w.chunk2(0x4321, ot3::kFlagOpaque, &mh, sizeof mh, pay, sizeof pay);
    f.resize(w.end());
    h.fs.files[kWsSidecar] = f;
    g_note.clear();
    CHECK(a.loadWorkspace());
    CHECK(g_note.find("Project uses module ZZZZ") != std::string::npos);
    // A later save writes the foreign chunk back byte for byte.
    a.setSlotEngine(2, 0x90);
    a.autosave();
    const std::vector<uint8_t>& g = h.fs.files[kWsSidecar];
    bool found = false;
    for (const ot3::Chunk& c : chunks(g))
        if (c.tag == 0x4321) {
            found = c.len == 14 && std::memcmp(c.payload, "ZZZZ", 4) == 0 && std::memcmp(c.payload + 8, pay, 6) == 0 &&
                    (c.flags & ot3::kFlagOpaque);
        }
    CHECK(found);
}

TEST(sidecar_damaged_file_is_set_aside) {
    registerTestModule();
    Harness h;
    App& a = *h.app;
    a.ui.platform.notified = &onNote;
    CHECK(a.newProject());
    g_sec.v[1] = 5;
    g_sec.d = true;
    a.autosave();
    std::vector<uint8_t> f = h.fs.files[kWsSidecar];
    f[20] ^= 0x01;   // breaks the CRC
    h.fs.files[kWsSidecar] = f;
    g_sec.reset();
    g_note.clear();
    CHECK(a.loadWorkspace());
    CHECK(g_note.find("OpenTracker data damaged") != std::string::npos);
    CHECK(h.fs.files.count(kWsSidecar) == 0);
    CHECK(h.fs.files.count("Workspace/project.ot3x.bad") == 1);
    CHECK_EQ(int(g_sec.v[1]), 0);
}

#ifdef OT_MOD_BBDC
// The BBD chorus insert was saved as module JUNO before its rename: such a
// project loads its settings into BBDC without a "not in this build" notice
// and is saved back under the new id.
TEST(sidecar_loads_the_bbd_chorus_under_its_old_id) {
    Harness h;
    App& a = *h.app;
    a.ui.platform.notified = &onNote;
    CHECK(a.newProject());
    mod::IModule* m = const_cast<mod::IModule*>(mod::ModuleHost::instance().find("BBDC"));
    CHECK(m && m->section() && m->page());
    if (!m || !m->section() || !m->page()) return;
    mod::IProjectSection* sec = m->section();
    sec->reset();
    m->page()->adjust(0, 2);   // Chorus II
    uint8_t pay[16];
    const uint32_t n = sec->encode(pay, sizeof pay);
    CHECK(n > 0);
    sec->reset();
    std::vector<uint8_t> f(512);
    ot3::Writer w;
    const uint8_t wr[4] = {3, 0, 0, 0};
    w.begin(f.data(), uint32_t(f.size()), ot3::kKindProjectSidecar, wr);
    const uint8_t info[4] = {3, 0, 0, 0};
    w.chunk(ot3::kTagInfo, ot3::kFlagCritical, info, 4);
    ot3::ModEntry me{};
    std::memcpy(me.id, "JUNO", 4);
    w.chunk(ot3::kTagMods, ot3::kFlagCritical, &me, sizeof me);
    ot3::ModuleChunkHeader mh{};
    std::memcpy(mh.id, "JUNO", 4);
    mh.version = sec->version();
    w.chunk2(sec->tag(), 0, &mh, sizeof mh, pay, n);
    f.resize(w.end());
    h.fs.files[kWsSidecar] = f;
    g_note.clear();
    CHECK(a.loadWorkspace());
    CHECK(g_note.find("JUNO") == std::string::npos);
    mod::ModuleRow row{};
    m->page()->row(0, row);
    CHECK_STR(row.value, "II");
    m->page()->adjust(1, -10);   // a change: the sidecar is written again
    a.autosave();
    bool renamed = false;
    for (const ot3::Chunk& c : chunks(h.fs.files[kWsSidecar]))
        if (c.tag == sec->tag()) renamed = std::memcmp(c.payload, "BBDC", 4) == 0;
    CHECK(renamed);
    sec->reset();
}
#endif

#if OT_SLOT_INSERTS
// Synth slot inserts (chunk 0x0008 SLOT_INSERT, one per slot with an insert):
// the effect by its id and its settings survive autosave and load; an effect
// not in this build keeps its record and is written back; values above a
// setting's max are clamped; New Project clears them; the audio bank gets the
// bound effect.
namespace {
class InsFx final : public audio::IEffect {
public:
    const char* id() const override { return "XINS"; }
    const char* name() const override { return "Ins"; }
    uint32_t stateBytes() const override { return 4; }
    int paramCount() const override { return 2; }
    const audio::EffectParam& param(int i) const override {
        static const audio::EffectParam p[2] = {{"Amount", 100, 40, nullptr}, {"Mode", 2, 1, nullptr}};
        return p[i];
    }
    bool process(void*, void*, const audio::EffectSettings&, bool on, float*, float*, uint32_t) override { return on; }
};
InsFx g_insFx;
class InsModule final : public mod::IModule {
public:
    const mod::ModuleInfo& info() const override {
        static const mod::ModuleInfo i = {"XINM", mod::kModuleApi, 100, "Test insert", mod::kCapEffect, 0, 0, 0, 0, 0};
        return i;
    }
    bool attach(mod::ModuleHost&) override { return true; }
    audio::IEffect* effect(int i) override { return i == 0 ? &g_insFx : nullptr; }
};
InsModule g_insMod;
}  // namespace

TEST(sidecar_slot_insert_round_trip) {
    static bool done = false;
    if (!done) done = mod::ModuleHost::instance().addForTest(g_insMod);
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    CHECK(a.slotInsertId(1) == nullptr);
    a.setSlotInsertKind(1, "XINS");
    CHECK(a.slotInsertEffect(1) == &g_insFx);
    CHECK_EQ(int(a.slotInsertSettings(1).v[0]), 40);   // the defaults
    CHECK_EQ(int(a.slotInsertSettings(1).v[1]), 1);
    a.setSlotInsertValue(1, 0, 77);
    a.setSlotInsertValue(1, 1, 9);                     // clamped to 2
    CHECK_EQ(int(a.slotInsertSettings(1).v[1]), 2);
    CHECK(a.sidecarDirty());
    a.autosave();
    std::vector<ot3::Chunk> cs = chunks(h.fs.files[kWsSidecar]);
    int found = 0;
    for (const ot3::Chunk& c : cs)
        if (c.tag == ot3::kTagSlotInsert) {
            ++found;
            CHECK(c.len >= 12 && c.payload[0] == 1 && std::memcmp(c.payload + 2, "XINS", 4) == 0);
            CHECK(c.payload[6] == 77 && c.payload[7] == 2);
            CHECK((c.flags & ot3::kFlagCritical) == 0);
        }
    CHECK_EQ(found, 1);
    // Off: kept with the settings, saved in the reserved byte (bit 0).
    CHECK(!a.slotInsertOff(1));
    a.setSlotInsertOff(1, true);
    CHECK(a.slotInsertOff(1));
    a.autosave();
    for (const ot3::Chunk& c : chunks(h.fs.files[kWsSidecar]))
        if (c.tag == ot3::kTagSlotInsert) CHECK_EQ(int(c.payload[1]), 1);
    CHECK(a.loadWorkspace());
    CHECK(a.slotInsertOff(1));
    CHECK_EQ(int(a.slotInsertSettings(1).v[0]), 77);
    a.setSlotInsertOff(1, false);
    a.autosave();
    // The audio bank has it bound after a block.
    h.audio.pump(1);
    CHECK(a.engine.synth().slotInsert(1) == &g_insFx);
    // Load: back.
    a.setSlotInsertKind(1, nullptr);
    CHECK(a.loadWorkspace());
    CHECK(a.slotInsertEffect(1) == &g_insFx);
    CHECK_EQ(int(a.slotInsertSettings(1).v[0]), 77);
    // An effect not in this build (ZZZZ on slot 2): kept, not bound, written back.
    std::vector<uint8_t> f(512);
    ot3::Writer w;
    const uint8_t wr[4] = {3, 1, 0, 0};
    w.begin(f.data(), uint32_t(f.size()), ot3::kKindProjectSidecar, wr);
    const uint8_t info[4] = {3, 0, 0, 0};
    w.chunk(ot3::kTagInfo, ot3::kFlagCritical, info, 4);
    ot3::SlotInsert si{};
    si.slot = 2;
    std::memcpy(si.effectId, "ZZZZ", 4);
    si.set[0] = 5;
    w.chunk(ot3::kTagSlotInsert, 0, &si, sizeof si);
    f.resize(w.end());
    h.fs.files[kWsSidecar] = f;
    CHECK(a.loadWorkspace());
    CHECK(a.slotInsertId(1) == nullptr);
    CHECK(a.slotInsertId(2) != nullptr && std::memcmp(a.slotInsertId(2), "ZZZZ", 4) == 0);
    CHECK(a.slotInsertEffect(2) == nullptr);
    a.setSlotEngine(0, 0x90);   // any edit: the file is written again
    a.autosave();
    found = 0;
    for (const ot3::Chunk& c : chunks(h.fs.files[kWsSidecar]))
        if (c.tag == ot3::kTagSlotInsert && c.payload[0] == 2 && std::memcmp(c.payload + 2, "ZZZZ", 4) == 0 && c.payload[6] == 5)
            ++found;
    CHECK_EQ(found, 1);
    // New Project: none.
    CHECK(a.newProject());
    CHECK(a.slotInsertId(1) == nullptr && a.slotInsertId(2) == nullptr);
}
#endif
