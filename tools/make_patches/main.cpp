// make_patches: writes the OpenTracker synth preset pack (presets.h,
// PRESETS.md) onto an SD-card folder.
//
//   make_patches <sd-root>          -> <sd-root>/Patches/<MODEL>/<name>.patch
//   make_patches --list             the presets
//   make_patches --markdown         PRESETS.md on stdout
//
// Drives a headless App (src/core, unchanged) over the simulator's host-folder
// IStorage, like make_testsongs: each preset is built on synth slot 1 from the
// engine's Init patch and saved with the App's own Save As
// (App::saveSynthLibraryPatch), so the files are what the firmware writes.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "app/app.h"
#include "hal_api.h"
#include "host_storage.h"
#include "presets.h"

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
    std::fprintf(stderr, "usage: make_patches <sd-root> | --list | --markdown\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    const char* root = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--list")) {
            for (const presets::Preset& p : presets::all())
                std::printf("%-5s %-22s %s\n", presets::folderOf(p), p.name, p.desc);
            return 0;
        }
        if (!std::strcmp(argv[i], "--markdown")) {
            std::fputs(presets::markdown().c_str(), stdout);
            return 0;
        }
        if (argv[i][0] != '-' && !root) root = argv[i];
        else return usage();
    }
    if (!root) return usage();

    sim::HostStorage fs;
    if (!fs.init(root)) {
        std::fprintf(stderr, "make_patches: cannot use SD root %s\n", root);
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
        std::fprintf(stderr, "make_patches: App init/boot failed\n");
        return 1;
    }

    int failures = 0, made = 0;
    for (const presets::Preset& p : presets::all()) {
        std::string err;
        if (!presets::buildAndSave(*app, 0, p, err)) {
            std::fprintf(stderr, "FAIL  %s\n", err.c_str());
            ++failures;
            continue;
        }
        ++made;
        std::printf("ok    Patches/%s/%s.patch\n", presets::folderOf(p), p.name);
    }
    std::printf("make_patches: %d patch(es) in %s/Patches\n", made, root);
    return failures ? 1 : 0;
}
