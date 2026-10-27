// make_testsongs: writes the OpenTracker 2.x hardware-test projects onto an
// SD-card folder (reimpl/tools/make_testsongs/testsongs.h, TESTSONGS.md).
//
//   make_testsongs <sd-root> [--song N]... [--force] [--list] [--render] [--stems-seconds N]
//
// --stems-seconds N (or the environment variable STEMS_SECONDS): length of
// each stem of song 19 "OT Test 12 - Stems" (default 30, 10..600).
//
// --render also exports every generated song through the App's own offline
// render (spec 04 §6.1) to <sd-root>/Render/<name>.wav and the stems to
// <sd-root>/Render/<name>/ (level checks, listening without hardware).
//
// Drives a headless App (src/core, unchanged) over the simulator's host-folder
// IStorage, like make_demo. Writes <sd-root>/Projects/<name>/ per song and the
// source WAVs under <sd-root>/Samples/OT Test Songs/. Songs whose milestone is
// not in this build are skipped (features.h) unless --force is given.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "app/app.h"
#include "hal_api.h"
#include "host_storage.h"
#include "testsongs.h"

namespace {

class NullDisplay : public ot::IDisplay {
public:
    uint16_t width() const override { return 800; }
    uint16_t height() const override { return 480; }
    void dlStart() override {}
    void clearColorRGB(uint8_t, uint8_t, uint8_t) override {}
    void clear(bool, bool, bool) override {}
    void colorRGB(uint8_t, uint8_t, uint8_t) override {}
    void colorA(uint8_t) override {}
    void lineWidth(uint16_t) override {}
    void pointSize(uint16_t) override {}
    void scissorXY(uint16_t, uint16_t) override {}
    void scissorSize(uint16_t, uint16_t) override {}
    void begin(Prim) override {}
    void end() override {}
    void vertex2f(int32_t, int32_t) override {}
    void vertex2ii(uint16_t, uint16_t, uint8_t, uint8_t) override {}
    void text(int16_t, int16_t, uint8_t, uint16_t, const char*) override {}
    void number(int16_t, int16_t, uint8_t, uint16_t, int32_t) override {}
    void swap() override {}
};
class NullAudio : public ot::IAudioOut {
public:
    void setCallback(FillFn, void*) override {}
    bool start() override { return true; }
    void stop() override {}
};
class NullInput : public ot::IInput {
public:
    bool poll(ot::InputEvent&) override { return false; }
};
class NullLeds : public ot::ILeds {
public:
    uint8_t padCount() const override { return 48; }
    void setPad(uint8_t, uint8_t, uint8_t, uint8_t) override {}
    void clear() override {}
    void show() override {}
};
class NullMidi : public ot::IMidiPort {
public:
    bool send(ot::MidiPortId, const uint8_t*, uint32_t) override { return true; }
    uint32_t receive(ot::MidiPortId, uint8_t*, uint32_t) override { return 0; }
};
class FixedClock : public ot::IClock {
public:
    uint32_t ms = 1000;
    uint32_t micros() const override { return ms * 1000; }
    uint32_t millis() const override { return ms; }
};

int usage() {
    std::fprintf(stderr,
                 "usage: make_testsongs <sd-root> [--song N]... [--force] [--list] [--render] [--stems-seconds N]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<int> pick;
    bool force = false, listOnly = false, render = false;
    int stemSeconds = 0;
    if (const char* e = std::getenv("STEMS_SECONDS"))
        if (*e) stemSeconds = std::atoi(e);
    const char* root = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--song") && i + 1 < argc) pick.push_back(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--force")) force = true;
        else if (!std::strcmp(argv[i], "--list")) listOnly = true;
        else if (!std::strcmp(argv[i], "--render")) render = true;
        else if (!std::strcmp(argv[i], "--stems-seconds") && i + 1 < argc) stemSeconds = std::atoi(argv[++i]);
        else if (argv[i][0] != '-' && !root) root = argv[i];
        else return usage();
    }
    if (listOnly) {
        for (int i = 0; i < ts::kSongCount; ++i) {
            const ts::SongInfo& s = ts::songInfo(i);
            std::printf("%d  %-28s  %-28s  %s\n", int(s.id), s.name, s.milestone, s.available ? "yes" : "not in this build");
        }
        return 0;
    }
    if (!root) return usage();

    sim::HostStorage fs;
    if (!fs.init(root)) {
        std::fprintf(stderr, "make_testsongs: cannot use SD root %s\n", root);
        return 1;
    }
    NullDisplay disp;
    NullAudio audio;
    NullInput input;
    NullLeds leds;
    NullMidi midi;
    FixedClock clock;

    // Same AppMemory parts as app_main.cpp (sizes as on the device; the
    // full sample memory, spec 02 §8.2: the load-stress song fills ~80 %).
    std::vector<int16_t> bank(ot::kSampleBankBytes / 2), fx(ot::audio::AudioEngine::kFxMemorySamples);
    std::vector<ot::PatternStock> pats(ot::kPatternCount);
    std::vector<ot::Pattern> undo(20), fundo(10), seqBufs(2);
    std::vector<ot::Track> extTracks(ot::ExtTrackPool::kDefaultBlocks + 1);
    std::vector<uint8_t> extMeta(ot::ExtTrackPool::metaBytes(ot::ExtTrackPool::kDefaultBlocks));
    std::unique_ptr<ot::PerfPatternMemory> perf(new ot::PerfPatternMemory());
    ot::Pattern clip;
    std::vector<uint8_t> nums(64), scratch(0x10000);
    std::unique_ptr<ot::App> app(new ot::App());
    ot::HalRefs h;
    h.display = &disp;
    h.audio = &audio;
    h.storage = &fs;
    h.midi = &midi;
    h.input = &input;
    h.leds = &leds;
    h.clock = &clock;
    ot::AppMemory m;
    m.sampleBank = bank.data();
    m.sampleBankSamples = uint32_t(bank.size());
    m.fxMemory = fx.data();
    m.fxSamples = uint32_t(fx.size());
    m.patterns = pats.data();
    m.extTracks = extTracks.data();
    m.extTrackBlocks = int(extTracks.size());
    m.extMeta = extMeta.data();
    m.seqBuffers = seqBufs.data();
    m.undo = undo.data();
    m.undoNums = nums.data();
    m.undoSlots = 20;
    m.fileUndo = fundo.data();
    m.fileUndoNums = nums.data() + 32;
    m.fileUndoSlots = 10;
    m.clipboard = &clip;
    m.scratch = scratch.data();
    m.perfPatterns = perf.get();
    m.scratchBytes = uint32_t(scratch.size());
    if (!app->init(h, m) || !app->boot()) {
        std::fprintf(stderr, "make_testsongs: App init/boot failed\n");
        return 1;
    }

    ts::GenOptions opt;
    opt.force = force;
    if (stemSeconds > 0) opt.stemSeconds = stemSeconds;
    int failures = 0, made = 0;
    for (int i = 0; i < ts::kSongCount; ++i) {
        const ts::SongInfo& s = ts::songInfo(i);
        if (!pick.empty()) {
            bool want = false;
            for (int p : pick) want = want || p == int(s.id);
            if (!want) continue;
        }
        if (!s.available && !force) {
            std::printf("skip  %-28s  needs %s\n", s.name, s.milestone);
            continue;
        }
        std::string err;
        if (!ts::generateSong(*app, fs, s.id, opt, err)) {
            std::fprintf(stderr, "FAIL  %-28s  %s\n", s.name, err.c_str());
            ++failures;
            continue;
        }
        ++made;
        const std::string dir = std::string(ot::kProjectsDir) + "/" + s.name;
        // Every song is a Tracker Mini project (docs/specs/21) and must pass
        // the Mini format check.
        const std::string mini = ts::checkStockProject(fs, dir.c_str(), true);
        std::printf("ok    %-28s  Mini format: %s\n", s.name, mini.empty() ? "clean" : "differs (see below)");
        if (!mini.empty()) {
            std::fprintf(stderr, "%s", mini.c_str());
            ++failures;
        }
        if (render) {
            fs.mkdir("Render");
            const std::string wav = std::string("Render/") + s.name + ".wav";
            const std::string stems = std::string("Render/") + s.name;
            const bool okW = app->exportAudio(ot::App::ExportKind::Song, wav.c_str());
            const bool okS = app->exportAudio(ot::App::ExportKind::Song, stems.c_str(), nullptr, true);
            std::printf("%s  %-28s  render %s\n", okW && okS ? "ok  " : "FAIL", s.name, wav.c_str());
            if (!okW || !okS) ++failures;
        }
    }
    std::printf("make_testsongs: %d project(s) in %s/%s\n", made, root, ot::kProjectsDir);
    return failures ? 1 : 0;
}
