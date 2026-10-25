// 2.x CPU meter option and cell in the app (spec 2x-02 §2.2, M1 plan §5):
// the extension record after the 0xF4 stock config image, the Config Info
// lines, the 5 Hz poll that marks the frame dirty only on a change, and the
// title clip / display-list budget while the cell is shown.
#include "test.h"
#include "ui_fakes.h"
#include "nvstore_fake.h"

#include "audio/cpu_meter.h"
#include "ui/screens.h"

using namespace ot;

namespace {
uint32_t gNow = 0, gStep = 0, gReads = 0;
uint32_t fakeCycles() {
    if (gReads++ & 1) gNow += gStep;
    return gNow;
}
void setLoad(uint32_t permille) {
    gStep = uint32_t(uint64_t(audio::CpuMeter::kBudgetCycles) * permille / 1000u) + 1;
}
ConfigScreen& cfgScreen(Harness& h) { return *static_cast<ConfigScreen*>(h.app->ui.screen(kModConfig)); }
}  // namespace

TEST(cpu_config_extension_record) {
    // Default (Auto): nothing written past the stock image.
    FakeNvStore nv;
    Config c;
    CHECK(c.save(nv));
    CHECK_EQ(nv.mem[0xF4], 0xFF);
    std::vector<uint8_t> stock(nv.mem.begin(), nv.mem.begin() + Config::kImageSize);
    // Always: the record follows the image, stock bytes unchanged.
    c.cpuMeter = 1;
    CHECK(c.save(nv));
    CHECK(std::equal(stock.begin(), stock.end(), nv.mem.begin()));
    // Payload: cpuMeter, synthVoices (2.x M3, OP-8), then the 8-bit sum.
    CHECK_BYTES(nv.mem.data() + 0xF4, 'O', 'T', 'X', 4, 6, 1, 4, 0, 1, 0, 1, 7);
    Config r;
    CHECK(r.load(nv));
    CHECK_EQ(r.cpuMeter, 1);
    CHECK_EQ(r.synthVoices, 4);
    // Back to Auto: the record is rewritten (not left at Always).
    c.cpuMeter = 0;
    CHECK(c.save(nv));
    CHECK(std::equal(stock.begin(), stock.end(), nv.mem.begin()));
    r.cpuMeter = 2;
    CHECK(r.load(nv));
    CHECK_EQ(r.cpuMeter, 0);
    // Bad checksum / no record (stock-written EEPROM): default.
    c.cpuMeter = 2;
    CHECK(c.save(nv));
    nv.mem[0xF4 + 6] ^= 0xFF;
    CHECK(r.load(nv));
    CHECK_EQ(r.cpuMeter, 0);
    // The image itself never carries the option.
    Config i;
    i.cpuMeter = 2;
    i.toImage();
    Config j;
    j.toImage();
    CHECK(std::memcmp(i.image, j.image, Config::kImageSize) == 0);
    // File fallback (no EEPROM): record appended after the image.
    FakeStorage fs;
    i.cpuMeter = 1;
    CHECK(i.save(fs));
    Config k;
    CHECK(k.load(fs));
    CHECK_EQ(k.cpuMeter, 1);
}

TEST(synth_voices_config) {
    // OPTIMIZATION_2X OP-8: the synth voice budget is a Config value (1..8,
    // default 4) in the extension record, so 4 -> 6 needs no rebuild.
    FakeNvStore nv;
    Config c;
    CHECK_EQ(c.synthVoices, 4);
    c.synthVoices = 6;   // a non-default budget alone writes the record
    CHECK(c.save(nv));
    CHECK_BYTES(nv.mem.data() + 0xF4, 'O', 'T', 'X', 4, 6, 0, 6, 0, 1, 0, 1, 8);
    Config r;
    CHECK(r.load(nv));
    CHECK_EQ(r.synthVoices, 6);
    // A 1-byte payload record (first 2.x builds) keeps the default budget.
    const uint8_t old[] = {'O', 'T', 'X', 1, 1, 2, 2};
    for (size_t i = 0; i < sizeof(old); ++i) nv.mem[0xF4 + i] = old[i];
    nv.mem[0xF4 + sizeof(old)] = 0xFF;
    CHECK(r.load(nv));
    CHECK_EQ(r.cpuMeter, 2);
    CHECK_EQ(r.synthVoices, 4);
    // Out of range -> default.
    c.synthVoices = 9;
    c.sanitize();
    CHECK_EQ(c.synthVoices, 4);
    // The app hands it to the governor budget.
    Harness h;
    h.app->cfg.synthVoices = 6;
    h.app->markConfigDirty();
    h.app->preBlock(0.0f);
    CHECK_EQ(h.app->engine.governor().budget(), 6);
    CHECK_EQ(h.app->engine.governor().cap(), 6);
}

TEST(cpu_config_info_lines) {
    Harness h;
    h.tap(btn::Config);
    ConfigScreen& c = cfgScreen(h);
    const int info = ConfigScreen::categoryCount() - 1;
    CHECK_STR(ConfigScreen::categoryName(info), "Info");
    char t[48];
    c.optionText(info, 1, t, sizeof t);
    CHECK_STR(t, "0");
    c.optionText(info, 2, t, sizeof t);
    CHECK_STR(t, "0");
    c.optionText(info, 3, t, sizeof t);
    CHECK_STR(t, "4/4");
    c.optionText(1, 11, t, sizeof t);
    CHECK_STR(t, "Auto");
}

TEST(cpu_cell_poll_and_frame) {
    Harness h;
    App& a = *h.app;
    h.run(500);
    // No cycle source (host / sim): no data, hidden even when Always.
    a.cfg.cpuMeter = 1;
    h.audio.pump(8);
    h.run(300);
    CHECK(!a.ui.cpuCell().shown());
    CHECK(h.disp.last.size() > 0);
    bool stockClip = false;
    for (const std::string& l : h.disp.last) stockClip |= l == "SCISSOR_SIZE 270 27";
    CHECK(stockClip);
    const uint32_t stockWords = a.ui.lastDlWords();
    // 40 % load, Always: dim cell after the next 5 Hz poll.
    audio::CpuMeter::setCycleSource(&fakeCycles);
    audio::CpuMeter::setStampSource(nullptr);   // two reads per block: no profile stamps
    setLoad(400);
    h.audio.pump(8);
    h.run(300);
    CHECK(a.ui.cpuCell().shown());
    CHECK_EQ(a.ui.cpuCell().percent, 40);
    bool narrow = false;
    for (const std::string& l : h.disp.last) {
        unsigned w = 0, hh = 0;
        if (std::sscanf(l.c_str(), "SCISSOR_SIZE %u %u", &w, &hh) == 2 && hh == 27 && w < 270) narrow = true;
    }
    CHECK(narrow);
    CHECK(a.ui.lastDlWords() <= 1900u);
    CHECK(a.ui.lastDlWords() >= stockWords);
    // Same state: no new frame from the poll.
    const int swaps = h.disp.swaps;
    h.audio.pump(8);
    h.run(1000);
    CHECK_EQ(h.disp.swaps, swaps);
    // Auto: hidden below 60 %, shown (and redrawn) at 65 %.
    a.cfg.cpuMeter = 0;
    h.run(300);
    CHECK(!a.ui.cpuCell().shown());
    CHECK(h.disp.swaps > swaps);
    setLoad(650);
    h.audio.pump(8);
    h.run(300);
    CHECK(a.ui.cpuCell().level == wg::CpuLevel::kWarn);
    // Off: hidden whatever the load.
    a.cfg.cpuMeter = 2;
    setLoad(990);
    h.audio.pump(8);
    h.run(300);
    CHECK(!a.ui.cpuCell().shown());
    audio::CpuMeter::setCycleSource(nullptr);
}

TEST(export_rate_config_record) {
    // Config > General "Export sample rate" (record v4, payload byte 6):
    // 1 = 44.1 kHz (converted, default), 0 = the device rate with 44117 in
    // the header, as stock. Payload bytes 3..5 belong to later 2.x options:
    // a build that doesn't know them keeps them as they are.
    FakeNvStore nv;
    Config c;
    CHECK_EQ(c.exportRate, 1);
    c.exportRate = 0;   // alone writes the record
    CHECK(c.save(nv));
    CHECK_BYTES(nv.mem.data() + 0xF4, 'O', 'T', 'X', 4, 6, 0, 4, 0, 1, 0, 0, 5);
    Config r;
    CHECK(r.load(nv));
    CHECK_EQ(r.exportRate, 0);
    // Bytes 3..5 written by another 2.x build survive a load and a save.
    const uint8_t other[] = {'O', 'T', 'X', 4, 6, 0, 4, 2, 3, 1, 1, 11};
    for (size_t i = 0; i < sizeof(other); ++i) nv.mem[0xF4 + i] = other[i];
    CHECK(r.load(nv));
    CHECK_EQ(r.exportRate, 1);
    r.cpuMeter = 1;
    CHECK(r.save(nv));
    CHECK_BYTES(nv.mem.data() + 0xF4, 'O', 'T', 'X', 4, 6, 1, 4, 2, 3, 1, 1, 12);
    // An older record (payload 2) loads with the default 44.1 kHz.
    const uint8_t v1[] = {'O', 'T', 'X', 1, 2, 2, 4, 6};
    for (size_t i = 0; i < sizeof(v1); ++i) nv.mem[0xF4 + i] = v1[i];
    for (size_t i = sizeof(v1); i < 12; ++i) nv.mem[0xF4 + i] = 0xFF;
    r.exportRate = 0;
    CHECK(r.load(nv));
    CHECK_EQ(r.cpuMeter, 2);
    CHECK_EQ(r.exportRate, 1);
    c.exportRate = 7;
    c.sanitize();
    CHECK_EQ(c.exportRate, 1);
}
