// [I] Card master effects (app/master_fx.cpp): the boot scan lists
// them; Turn on loads one (while playing: at the next Stop), Off lets it
// leave at the next Stop; its settings wait in the sidecar while it is out;
// its SDRAM comes from the top of the sample memory while it is loaded;
// Open loads the ones the project has data for.
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

uint32_t g_sdramBytes = 4096;

// The effect "in the pool" (the host can't run pool code: enter() hands it out).
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
        return line != nullptr;
    }
    audio::IMasterInsert* masterInsert() override { return this; }
    IModulePage* page() override { return this; }
    IProjectSection* section() override { return this; }
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
    }
    // Section: on + level.
    uint16_t tag() const override { return 0x4F10; }
    uint16_t version() const override { return 1; }
    bool dirty() const override { return true; }
    uint32_t encode(uint8_t* b, uint32_t cap) override {
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
        g_sdramBytes = 4096;
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

bool inChain(App& a) {
    for (int i = 0; i < audio::AudioEngine::kMaxMasterInserts; ++i)
        if (a.engine.masterInsert(i) == &g_fx) return true;
    return false;
}

}  // namespace

TEST(card_fx_listed_not_loaded) {
    FxRig rig;
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    const int k = bankRow(a);
    CHECK(k >= 0);
    App::MasterFx fx;
    CHECK(a.masterFx(k, fx));
    CHECK(fx.module == nullptr);
    CHECK(fx.fromCard);
    CHECK(!fx.on);
    CHECK_STR(fx.name, "Card FX");
    CHECK_EQ(fx.sdramBytes, 4096u);
    CHECK(!ModuleHost::instance().find("CFX1"));
    CHECK_EQ(int(a.bank.moduleHold()), 0);
    CHECK(!inChain(a));
}

TEST(card_fx_turn_on_loads_off_leaves_at_stop) {
    FxRig rig;
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    const size_t cap = a.bank.capacitySamples();
    const int k = bankRow(a);
    CHECK(a.turnMasterFx(k, true) == nullptr);
    App::MasterFx fx;
    CHECK(a.masterFx(bankRow(a), fx));
    CHECK(fx.module == &g_fx);   // loaded at once while stopped
    CHECK(fx.on);
    CHECK(inChain(a));
    // Its SDRAM is the top of the sample memory, which the samples no longer see.
    CHECK_EQ(int(a.bank.moduleHold()), 2048);
    CHECK_EQ(int(a.bank.capacitySamples()), int(cap - 2048));
    CHECK(g_fx.line == reinterpret_cast<uint8_t*>(a.bank.moduleArea()));
    // Settings survive leaving: Level 53, Off, it leaves (stopped: after the settle time).
    g_fx.adjust(1, 3);
    CHECK(a.turnMasterFx(bankRow(a), false) == nullptr);
    CHECK(ModuleHost::instance().find("CFX1"));   // still in until the set settles
    h.run(400);
    CHECK(!ModuleHost::instance().find("CFX1"));
    CHECK(!inChain(a));
    CHECK_EQ(int(a.bank.moduleHold()), 0);
    CHECK_EQ(int(a.bank.capacitySamples()), int(cap));
    CHECK(a.turnMasterFx(bankRow(a), true) == nullptr);
    CHECK(ModuleHost::instance().find("CFX1"));
    CHECK_EQ(g_fx.level, 53);
    CHECK(g_fx.on);
}

TEST(card_fx_turn_on_while_playing_loads_at_stop) {
    FxRig rig;
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    a.togglePlay(false);
    CHECK(a.turnMasterFx(bankRow(a), true) == nullptr);
    App::MasterFx fx;
    CHECK(a.masterFx(bankRow(a), fx));
    CHECK(fx.pending);
    CHECK(fx.module == nullptr);
    h.run(500);
    CHECK(!ModuleHost::instance().find("CFX1"));
    a.togglePlay(false);
    h.run(400);
    CHECK(ModuleHost::instance().find("CFX1"));
    CHECK(g_fx.on);
    // Off while playing: it stays in until Stop.
    a.togglePlay(false);
    CHECK(a.turnMasterFx(bankRow(a), false) == nullptr);
    h.run(500);
    CHECK(ModuleHost::instance().find("CFX1"));
    a.togglePlay(false);
    h.run(400);
    CHECK(!ModuleHost::instance().find("CFX1"));
}

TEST(card_fx_without_sample_memory_does_not_load) {
    FxRig rig;
    g_sdramBytes = 0x7FFFFF00u;   // more than the sample memory (the boot scan reads it)
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    const char* t = a.turnMasterFx(bankRow(a), true);
    CHECK(t != nullptr);
    CHECK(t && std::strstr(t, "Not enough sample memory") != nullptr);
    App::MasterFx fx;
    CHECK(a.masterFx(bankRow(a), fx));
    CHECK(fx.module == nullptr);
    CHECK(fx.error == App::FxError::kSampleMemory);
    CHECK_EQ(int(a.bank.moduleHold()), 0);
    g_sdramBytes = 4096;
}

TEST(card_fx_loads_with_the_project) {
    FxRig rig;
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    CHECK(a.turnMasterFx(bankRow(a), true) == nullptr);
    g_fx.adjust(1, -7);
    CHECK(a.saveProjectAs("Fx"));
    CHECK(a.newProject());
    CHECK(!ModuleHost::instance().find("CFX1"));   // a new project: it leaves
    CHECK_EQ(int(a.bank.moduleHold()), 0);
    CHECK(a.openProject("Projects", "Fx"));
    CHECK(ModuleHost::instance().find("CFX1"));
    CHECK(g_fx.on);
    CHECK_EQ(g_fx.level, 43);
    CHECK(inChain(a));
    // Saved off with settings: Open loads it for its data, then it leaves;
    // the data is kept for the next save.
    CHECK(a.turnMasterFx(bankRow(a), false) == nullptr);
    CHECK(a.saveProjectAs("Fx"));
    CHECK(a.openProject("Projects", "Fx"));
    h.run(400);
    CHECK(!ModuleHost::instance().find("CFX1"));
    CHECK(a.turnMasterFx(bankRow(a), true) == nullptr);
    CHECK_EQ(g_fx.level, 43);
}
