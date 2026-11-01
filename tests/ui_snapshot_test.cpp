// Display-list snapshot tool: renders every screen with some content and
// dumps the recorded FT81x command list as text for review, one file per
// screen, into $OT_SNAPSHOT_DIR (the Makefile sets build/<profile>/host-tests/snapshots).
// Also checks each frame stays inside the 2048-word RAM_DL budget.
#include <sys/stat.h>

#include <string>

#include "test.h"
#include "ui_fakes.h"

#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;

static std::string outDir() {
    const char* e = std::getenv("OT_SNAPSHOT_DIR");
    std::string d = e && *e ? e : "../build/full/host-tests/snapshots";
    ::mkdir(d.c_str(), 0755);
    return d;
}

static bool dump(Harness& h, const char* name) {
    h.app->ui.dirty();
    h.run(40);
    std::string path = outDir() + "/" + name + ".txt";
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "# %s: %zu display-list calls, ~%u DL words\n", name, h.disp.last.size(),
                 unsigned(h.app->ui.lastDlWords()));
    for (auto& l : h.disp.last) std::fprintf(f, "%s\n", l.c_str());
    std::fclose(f);
    return true;
}

TEST(snapshot_all_screens) {
    Harness h;
    App& a = *h.app;
    std::vector<int16_t> s(8000);
    for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t((int(i) % 200 - 100) * 250);
    std::vector<uint8_t> w(s.size() * 2 + 64);
    w.resize(wavEncode(s.data(), uint32_t(s.size()), 1, w.data(), w.size()));
    h.fs.mkdir("Samples");
    h.fs.files["Samples/saw.wav"] = w;
    CHECK(a.importSample("Samples/saw.wav", 0));
    // Some pattern content.
    h.tap(btn::Rec);
    for (int p = 36; p < 44; ++p) h.pad(uint8_t(p));
    h.tap(btn::Fx1);
    h.down(btn::Fx1);
    h.enc(3);
    h.up(btn::Fx1);
    struct Shot {
        uint8_t key;
        const char* name;
    } shots[] = {
        {btn::Pattern, "pattern"}, {btn::Song, "song"},       {btn::InstrParams, "instrument"},
        {btn::SampleLoader, "loader"}, {btn::Master, "master"}, {btn::File, "file"},
        {btn::Config, "config"},   {btn::SamplePlayback, "playback"}, {btn::Performance, "performance"},
        {btn::SampleEditor, "editor"}, {btn::SampleRecorder, "recorder"},
    };
    for (auto& sh : shots) {
        h.tap(sh.key);
        CHECK(dump(h, sh.name));
        CHECK(a.ui.lastDlWords() < 2048);
        CHECK(!h.disp.last.empty());
    }
    // A few overlays.
    h.tap(btn::Pattern);
    h.tap(btn::F8);
    h.tap(btn::F8);
    h.tap(btn::F1);
    CHECK(dump(h, "keyboard"));
    h.tap(btn::F7);
    h.down(btn::Instr);
    h.run(300);
    CHECK(static_cast<PatternScreen*>(a.ui.screen(kModPattern))->popup().isOpen());
    CHECK(dump(h, "instrument_picker"));
    h.up(btn::Instr);
    h.tap(btn::Master);
    h.tap(btn::Master);
    CHECK(dump(h, "track_mixer"));
}
