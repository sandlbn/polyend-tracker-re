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
