// [I] A card module with a master effect that is also a synth slot insert
// (Dirt, BBD): it loads at boot for good, as the modules without a master
// effect, so a slot insert never points into a module that left, and a
// project opened plays its slot inserts.
#include "test.h"

#include <cstdio>
#include <cstring>

#include "audio/inserts.h"
#include "formats/crc32.h"
#include "modules/module.h"
#include "modules/module_files.h"
#include "modules/module_ui.h"
#include "ui_fakes.h"

using namespace ot;
using namespace ot::mod;

namespace {

uint32_t g_sdramBytes = 0;   // as Dirt: no SDRAM (a boot module's comes from the profile block)

// The effect "in the pool" (the host can't run pool code: enter() hands it out).
class CardDrive final : public audio::IEffect {
public:
    const char* id() const override { return "CDRV"; }
    const char* name() const override { return "CDrive"; }
    uint32_t stateBytes() const override { return 8; }
    int paramCount() const override { return 1; }
    const audio::EffectParam& param(int) const override {
        static const audio::EffectParam p = {"Amount", 100, 50, nullptr};
        return p;
    }
    bool process(void*, void*, const audio::EffectSettings&, bool on, float*, float*, uint32_t) override { return on; }
};
CardDrive g_drive;
class CardFxModule final : public IModule, public IModulePage, public IProjectSection, public audio::IMasterInsert {
public:
    const ModuleInfo& info() const override {
        static ModuleInfo i = {"CFX1", kModuleApi, 100, "Card FX", kCapMasterInsert | kCapPage | kCapSection, 0, 0, 0, 0, 1};
        i.sdramBytes = g_sdramBytes;
        return i;
    }
    bool attach(ModuleHost& h) override {
        ++attached;
        line = static_cast<uint8_t*>(h.sdram(g_sdramBytes));
        on = false;
        level = 50;
        return !g_sdramBytes || line != nullptr;
    }
    audio::IMasterInsert* masterInsert() override { return this; }
    IModulePage* page() override { return this; }
    IProjectSection* section() override { return this; }
    audio::IEffect* effect(int i) override { return i == 0 ? &g_drive : nullptr; }
    // Page: On, Level.
    const char* title() const override { return "CardFx"; }
    int rows() const override { return 2; }
    void row(int i, ModuleRow& out) override {
        std::snprintf(out.label, sizeof out.label, "%s", i ? "Level" : "On");
        if (i) std::snprintf(out.value, sizeof out.value, "%d", level);
        else std::snprintf(out.value, sizeof out.value, "%s", on ? "On" : "Off");
        out.frac = 0;
        out.bipolar = false;
    }
    void adjust(int i, int d) override {
        if (i == 0) on = d > 0;
        else level += d;
        dirt = true;
    }
    // Section: on + level.
    uint16_t tag() const override { return 0x4F10; }
    uint16_t version() const override { return 1; }
    bool dirty() const override { return dirt; }
    bool dirt = false;
    uint32_t encode(uint8_t* b, uint32_t cap) override {
        dirt = false;
        if (cap < 2 || (!on && level == 50)) return 0;
        b[0] = on;
        b[1] = uint8_t(level);
        return 2;
    }
    bool decode(const uint8_t* p, uint32_t len, uint16_t) override {
        if (len < 2) return false;
        on = p[0] != 0;
        level = p[1];
        return true;
    }
    void reset() override {
        on = false;
        level = 50;
    }
    bool process(float*, float*, uint32_t) override { return false; }
    int attached = 0;
    uint8_t* line = nullptr;
    bool on = false;
    int level = 50;
};
CardFxModule g_fx;


constexpr uint32_t kLayout = 0x12345678u, kAbi = 0x9ABCDEF0u;
const OtmCore kCore = {kModuleApi, kLayout, kAbi, nullptr, 0};
uint8_t g_pool[256];
IModule& enterFx(uint32_t) { return g_fx; }

std::vector<uint8_t> otmFile() {
    OtmHeader h = {kOtmMagic, kOtmHeaderBytes, kModuleApi, kLayout, kAbi, {'C', 'F', 'X', '1'}, 16, 8, 8, 1, 0, 0, 0, 0};
    std::vector<uint8_t> v(sizeof h);
    std::memcpy(v.data(), &h, sizeof h);
    v.resize(v.size() + 16, 0);
    const uint32_t c = crc32(v.data(), v.size());
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(c >> (8 * i)));
    return v;
}

const OtmPool kPool = {g_pool, 0x20030000u, sizeof(g_pool), kCore, nullptr, &enterFx, nullptr};

struct FxRig {
    FxRig() {
        g_sdramBytes = 0;
        Harness::beforeInit = [](Harness& h, HalRefs& r) {
            h.fs.dirs.insert("Modules");
            h.fs.files["Modules/cfx.otm"] = otmFile();
            r.modulePool = &kPool;
        };
    }
    ~FxRig() { Harness::beforeInit = nullptr; }
};

int bankRow(App& a) {
    for (int k = 0; k < a.masterFxCount(); ++k) {
        App::MasterFx fx;
        if (a.masterFx(k, fx) && std::strcmp(fx.id, "CFX1") == 0) return k;
    }
    return -1;
}

[[maybe_unused]] bool inChain(App& a) {
    for (int i = 0; i < audio::AudioEngine::kMaxMasterInserts; ++i)
        if (a.engine.masterInsert(i) == &g_fx) return true;
    return false;
}

}  // namespace

TEST(card_fx_with_a_slot_effect_loads_at_boot) {
    FxRig rig;
    Harness h;
    App& a = *h.app;
    CHECK(ModuleHost::instance().find("CFX1"));
    CHECK(ModuleHost::instance().effectById("CDRV") == &g_drive);
    CHECK_EQ(mod::cardFxIndex("CFX1"), -1);
    CHECK(bankRow(a) >= 0);   // still in the FX bank, as a built-in one
}

TEST(card_fx_slot_insert_stays_bound_after_off) {
    FxRig rig;
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    CHECK(a.turnMasterFx(bankRow(a), true) == nullptr);
    a.setSlotInsertKind(0, "CDRV");
    h.audio.pump(2);
    CHECK(a.engine.synth().slotInsert(0) == &g_drive);
    CHECK(a.turnMasterFx(bankRow(a), false) == nullptr);
    h.run(400);
    CHECK(ModuleHost::instance().find("CFX1"));
    h.audio.pump(2);
    CHECK(a.engine.synth().slotInsert(0) == &g_drive);
}

TEST(card_fx_slot_insert_bound_after_open) {
    FxRig rig;
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    CHECK(a.turnMasterFx(bankRow(a), true) == nullptr);
    a.setSlotInsertKind(0, "CDRV");
    CHECK(a.saveProjectAs("Fx2"));
    CHECK(a.newProject());
    CHECK(a.openProject("Projects", "Fx2"));
    h.run(400);
    CHECK(g_fx.on);
    CHECK(a.slotInsertId(0) != nullptr);
    h.audio.pump(2);
    CHECK(a.engine.synth().slotInsert(0) == &g_drive);
    CHECK(std::strstr(a.ui.notice(), "not in this build") == nullptr);
}
