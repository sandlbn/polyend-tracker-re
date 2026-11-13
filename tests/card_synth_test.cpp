// [I] Card synths (modules/module_files.h): the boot scan lists
// them, the slots' engines decide what's loaded. A model change loads after
// App::kCardSettleMs while stopped ("Loads at Stop" while playing) and gives
// the slot its Init patch; Open loads before the patches; a set no slot
// names leaves the pool.
#include "test.h"

#include <cstring>

#include "audio/synth_engine.h"
#include "formats/crc32.h"
#include "modules/module.h"
#include "modules/module_files.h"
#include "modules/synth_descriptor.h"
#include "modules/synth_models.h"
#include "modules/thumb_branch.h"
#include "ui_fakes.h"

using namespace ot;
using namespace ot::mod;

namespace {

constexpr int32_t kCardEng = 0xE5;   // test-only id
const PatchParamRange kParams[2] = {{0.5f, 0, 1}, {1, 0, 2}};
const synthui::ParamUi kUi[2] = {{"Tone", synthui::kFmtPct, nullptr, 0}, {"Volume", synthui::kFmtPct, nullptr, 0}};
const uint8_t kMain[] = {0, 1};
const synthui::Section kSections[] = {OT_SYNTH_SECTION("Main", "Main", kMain), OT_SYNTH_MACRO_SECTIONS};
const SynthDescriptor kDesc = {kCardEng, "CARD", 0, 1, 2, kParams, kUi, kSections, 8, 0, nullptr, "Card-1"};

// The module "in the pool" (the host can't run pool code: enter() hands it out).
class CardModule final : public IModule {
public:
    const ModuleInfo& info() const override {
        static const ModuleInfo i = {"CRD1", kModuleApi, 100, "Card synth", kCapSynthEngine | kCapSynthDescriptor,
                                     0, 0, 0, 0, 8};
        return i;
    }
    bool attach(ModuleHost&) override {
        ++attached;
        return true;
    }
    audio::ISynthEngine* synthEngine(int32_t id) override { return id == kCardEng ? audio::synthTestEngine() : nullptr; }
    const SynthDescriptor* synthDescriptor(int i) const override { return i == 0 ? &kDesc : nullptr; }
    int attached = 0;
};
CardModule g_card;

constexpr uint32_t kLayout = 0x12345678u, kAbi = 0x9ABCDEF0u;
const OtmCore kCore = {kModuleApi, kLayout, kAbi, nullptr, 0};
uint8_t g_pool[256];
IModule& enterCard(uint32_t) { return g_card; }

// A 16-byte module image (no fixups, 8 B bss).
std::vector<uint8_t> otmFile() {
    OtmHeader h = {kOtmMagic, kOtmHeaderBytes, kModuleApi, kLayout, kAbi, {'C', 'R', 'D', '1'}, 16, 8, 8, 1, 0, 0, 0, 0};
    std::vector<uint8_t> v(sizeof h);
    std::memcpy(v.data(), &h, sizeof h);
    v.resize(v.size() + 16, 0);
    const uint32_t c = crc32(v.data(), v.size());
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(c >> (8 * i)));
    return v;
}

const OtmPool kPool = {g_pool, 0x20030000u, sizeof(g_pool), kCore, nullptr, &enterCard, nullptr};

struct CardRig {
    CardRig() {
        Harness::beforeInit = [](Harness& h, HalRefs& r) {
            h.fs.dirs.insert("Modules");
            h.fs.files["Modules/card.otm"] = otmFile();
            r.modulePool = &kPool;
        };
    }
    ~CardRig() { Harness::beforeInit = nullptr; }
};

}  // namespace

TEST(card_synth_listed_not_loaded) {
    CardRig rig;
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    CHECK(cardEngine(kCardEng) != nullptr);
    CHECK(!cardEngineLoaded(kCardEng));
    CHECK_EQ(g_card.attached, 0);
    const int k = synthModelListIndex(kCardEng);
    CHECK(k >= 5);
    CHECK_STR(synthModelListName(k), "Card-1");
    CHECK_STR(synthEngineFolder(kCardEng), "CARD");
    CHECK_STR(synthEngineDisplayName(kCardEng), "Card-1");
}

TEST(card_synth_loads_after_the_choice_rests) {
    CardRig rig;
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.setSynthEngine(1, kCardEng);
    CHECK_EQ(a.slotEngineId(1), kCardEng);
    CHECK_STR(a.cardSlotState(1), "Loading");
    h.run(100);
    CHECK(!cardEngineLoaded(kCardEng));   // still settling
    h.run(300);
    CHECK(cardEngineLoaded(kCardEng));
    CHECK(a.cardSlotState(1) == nullptr);
    CHECK_EQ(a.synthPatch(1).engine, kCardEng);   // its Init patch, once loaded
    CHECK_EQ(int(a.synthPatch(1).paramCount), 2);
    h.run(20);
    CHECK(a.engine.synth().slotAvailable(1));
    // Away from it: it leaves the pool.
    a.setSynthModel(1, int(SynthModel::VAP));
    h.run(400);
    CHECK(!cardEngineLoaded(kCardEng));
    CHECK(!ModuleHost::instance().find("CRD1"));
    // While playing it waits for Stop.
    a.togglePlay(false);
    a.setSynthEngine(2, kCardEng);
    h.run(500);
    CHECK(!cardEngineLoaded(kCardEng));
    CHECK_STR(a.cardSlotState(2), "Loads at Stop");
    a.togglePlay(false);
    h.run(400);
    CHECK(cardEngineLoaded(kCardEng));
    CHECK_EQ(a.synthPatch(2).engine, kCardEng);
}

TEST(card_synth_loads_with_the_project) {
    CardRig rig;
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.setSynthEngine(0, kCardEng);
    h.run(400);
    CHECK(cardEngineLoaded(kCardEng));
    CHECK(a.saveProjectAs("Card"));
    CHECK(a.newProject());
    h.run(400);
    CHECK(!cardEngineLoaded(kCardEng));   // New Project names no card synth
    CHECK(a.openProject("Projects", "Card"));
    CHECK(cardEngineLoaded(kCardEng));    // before the patches: the slot's patch is its own
    CHECK_EQ(a.slotEngineId(0), kCardEng);
    CHECK_EQ(a.synthPatch(0).engine, kCardEng);
}

// ---- [I] built-in synths' hot rows share the fast block (modules/hot_rows.h) ----

namespace ot {
namespace mod {
IModule& acdModule();
IModule& vapModule();
IModule& fatModule();
}  // namespace mod
}  // namespace ot

namespace {

constexpr uint32_t kFastBase = 0x1FFF9000u;
uint8_t g_fast[64];

// A 16-byte row per synth (one cut, no fixups) with a 4-byte table pointing
// into it and a "vtable pointer" slot (a test word) pointing at the table.
struct FakeRow {
    uint8_t code[16] = {};
    uint8_t table[4] = {};
    uint32_t cuts[1] = {16};
    uint32_t entries[1] = {0};
    uint32_t vptr;
    HotTable tab;
    HotSlot slot;
    FakeRow(uint32_t addr) {
        for (int i = 0; i < 4; ++i) table[i] = uint8_t((addr + 1) >> (8 * i));
        vptr = addr + 0x1000u;
        tab = HotTable{table, addr + 0x1000u, 4, 1, entries};
        slot = HotSlot{&vptr, addr + 0x1000u};
    }
    HotRow row(ModuleFactory m, uint32_t addr) { return HotRow{m, code, addr, 16, 1, cuts, 0, nullptr, 1, &tab, 1, &slot}; }
};
FakeRow g_acdRow(0x00010000u), g_vapRow(0x00020000u);
HotRow g_rows[2];
HotRows g_hot = {2, g_rows};
OtmPool g_hotPool;

struct HotRig {
    explicit HotRig(uint32_t fastCap) {
        g_rows[0] = g_acdRow.row(&acdModule, 0x00010000u);
        g_rows[1] = g_vapRow.row(&vapModule, 0x00020000u);
        g_acdRow.vptr = 0x00011000u;
        g_vapRow.vptr = 0x00021000u;
        g_hotPool = OtmPool{g_pool, 0x20030000u, sizeof(g_pool), kCore, nullptr, &enterCard, nullptr};
        g_hotPool.fastMem = g_fast;
        g_hotPool.fastBase = kFastBase;
        g_hotPool.fastCap = fastCap;
        g_hotPool.hotRows = &g_hot;
        Harness::beforeInit = [](Harness&, HalRefs& r) { r.modulePool = &g_hotPool; };
    }
    ~HotRig() { Harness::beforeInit = nullptr; }
};

int32_t engineOf(IModule& m) { return m.synthDescriptor(0)->engineId; }

}  // namespace

TEST(hot_rows_share_the_fast_block_in_slot_order) {
    HotRig rig(sizeof(g_fast));
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.setSynthEngine(0, engineOf(fatModule()));   // no row: takes no turn
    a.setSynthEngine(1, engineOf(acdModule()));
    a.setSynthEngine(2, engineOf(vapModule()));
    h.run(400);
    // ACD first (16 B code + 4 B table, 24 with the 8-byte step), then VAP.
    CHECK_EQ(g_acdRow.vptr, kFastBase + 16);
    CHECK_EQ(g_vapRow.vptr, kFastBase + 24 + 16);
    CHECK_EQ(thumb::rd32(g_fast + 16), kFastBase + 1);   // the table's copy points at the moved code
    // VAP leaves: back to flash, ACD keeps its place.
    a.setSynthEngine(2, engineOf(fatModule()));
    h.run(400);
    CHECK_EQ(g_vapRow.vptr, 0x00021000u);
    CHECK_EQ(g_acdRow.vptr, kFastBase + 16);
}

TEST(hot_row_without_room_runs_from_flash) {
    HotRig rig(24);   // room for one
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.setSynthEngine(0, engineOf(vapModule()));
    a.setSynthEngine(1, engineOf(acdModule()));
    a.setSynthEngine(2, engineOf(fatModule()));
    h.run(400);
    CHECK_EQ(g_vapRow.vptr, kFastBase + 16);   // the first slot's synth got the block
    CHECK_EQ(g_acdRow.vptr, 0x00011000u);
}

TEST(boot_scan_again_puts_moved_rows_back_in_flash) {
    HotRig rig(sizeof(g_fast));
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.setSynthEngine(0, engineOf(acdModule()));
    h.run(400);
    CHECK_EQ(g_acdRow.vptr, kFastBase + 16);
    // A scan again (the pool's start is rewritten): the moved row goes back
    // to flash first, and the report shows nothing in the block.
    static uint8_t scratch[4096];
    OtmLoadReport r;
    r.hotCut[0] = 99;
    loadModuleFiles(h.fs, g_hotPool, scratch, sizeof(scratch), &r);
    CHECK_EQ(g_acdRow.vptr, 0x00011000u);
    CHECK_EQ(int(r.hotCut[0]), 0);
}

