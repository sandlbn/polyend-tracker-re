// Module settings pages (modules/module_ui.h) after Master, Track Mixer and
// Global Mixer; F-keys pick a
// row, the encoder and Up / Down change it. Shift + Master opens them; Master
// keeps the stock three-page cycle.
#include "test.h"

#include <cstdio>

#include "modules/module.h"
#include "modules/module_ui.h"
#include "ui_fakes.h"

using namespace ot;

namespace {

class PageModule final : public mod::IModule, public mod::IModulePage {
public:
    int v[3] = {10, 20, 30};
    const mod::ModuleInfo& info() const override {
        static const mod::ModuleInfo i = {"XPG1", mod::kModuleApi, 100, "Test page", mod::kCapPage, 0, 0, 0, 0, 0};
        return i;
    }
    bool attach(mod::ModuleHost&) override { return true; }
    mod::IModulePage* page() override { return this; }
    const char* title() const override { return "Probe"; }
    int rows() const override { return 3; }
    void row(int i, mod::ModuleRow& out) override {
        std::snprintf(out.label, sizeof(out.label), "Knob %d", i + 1);
        std::snprintf(out.value, sizeof(out.value), "%d", v[i]);
        out.frac = float(v[i]) / 100.0f;
        out.bipolar = false;
    }
    void adjust(int i, int d) override {
        v[i] += d;
        if (v[i] < 0) v[i] = 0;
        if (v[i] > 100) v[i] = 100;
    }
};
PageModule g_page;

}  // namespace

TEST(module_page_in_master_cycle) {
    static bool reg = false;
    if (!reg) reg = mod::ModuleHost::instance().addForTest(g_page);
    CHECK(reg);
    Harness h;
    const int pages = 3 + mod::ModuleHost::instance().pageCount();
    // Master keeps the stock cycle (3 pages); Shift + Master steps through
    // the module pages.
    h.tap(btn::Master);
    for (int k = 0; k < 4; ++k) {
        h.run(5);
        CHECK(!h.disp.lastHasText("Master Probe"));
        h.tap(btn::Master);
    }
    bool found = false;
    for (int k = 0; k < pages && !found; ++k) {
        h.down(btn::Shift);
        h.tap(btn::Master);
        h.up(btn::Shift);
        h.run(5);
        found = h.disp.lastHasText("Master Probe");
    }
    CHECK(found);
    CHECK(h.disp.lastHasText("Knob 1"));
    h.tap(btn::F2);
    h.enc(5);
    h.run(5);
    CHECK_EQ(g_page.v[1], 25);
    CHECK(h.disp.lastHasText("25"));
    h.tap(btn::Down);
    CHECK_EQ(g_page.v[1], 24);
    h.tap(btn::Right);
    h.enc(-40);
    CHECK_EQ(g_page.v[2], 0);
    CHECK_EQ(g_page.v[0], 10);
}

TEST(module_page_master_returns_to_master) {
    Harness h;
    h.tap(btn::Master);
    h.down(btn::Shift);
    h.tap(btn::Master);
    h.up(btn::Shift);
    h.run(5);
    CHECK(mod::ModuleHost::instance().pageCount() == 0 || !h.disp.lastHasText("Master 1/3"));
    h.tap(btn::Master);
    h.run(5);
    CHECK(h.disp.lastHasText("Master 1/3"));
}
