// sy1_to_patch: converts .sy1 patch files (the text patches of the classic
// freeware softsynth 1-Type is styled after) to 1-Type patches, written with
// the App's own Save As path (App::saveSynthLibraryPatch), so the files are
// byte for byte what the firmware writes: <sd-root>/Patches/1TYPE/<name>.patch,
// and next to each a <name>.txt with what did not carry over.
//
//   sy1_to_patch <file.sy1 | folder>... -o <sd-root>
//
// The mapping (the original's knob laws, its old-version conversion, the
// default macros) is opentracker-synths modules/onetype/tools/sy1/sy1_map.h
// (MIT, derived from Oxo Synth2).
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "app/app.h"
#include "hal_api.h"
#include "host_storage.h"
#include "presets.h"
#include "onetype.h"
#include "tools/sy1/sy1_map.h"

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
    std::fprintf(stderr, "usage: sy1_to_patch <file.sy1 | folder>... -o <sd-root>\n");
    return 2;
}

bool endsWithSy1(const std::string& s) {
    if (s.size() < 4) return false;
    std::string e = s.substr(s.size() - 4);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e == ".sy1";
}
void collect(const std::string& path, std::vector<std::string>& out) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) {
        out.push_back(path);
        return;
    }
    DIR* d = opendir(path.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n[0] != '.' && endsWithSy1(n)) names.push_back(path + "/" + n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    out.insert(out.end(), names.begin(), names.end());
}

}  // namespace

int main(int argc, char** argv) {
    const char* root = nullptr;
    std::vector<std::string> inputs;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc) root = argv[++i];
        else if (argv[i][0] != '-') collect(argv[i], inputs);
        else return usage();
    }
    if (!root || inputs.empty()) return usage();

    sim::HostStorage fs;
    if (!fs.init(root)) {
        std::fprintf(stderr, "sy1_to_patch: cannot use SD root %s\n", root);
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
        std::fprintf(stderr, "sy1_to_patch: App init/boot failed\n");
        return 1;
    }

    namespace sy = onetype_sy1;
    namespace ty = ot::audio::onetype;
    int failures = 0, made = 0;
    for (const std::string& path : inputs) {
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        if (!f) {
            std::fprintf(stderr, "FAIL  %s: cannot read\n", path.c_str());
            ++failures;
            continue;
        }
        const sy::Patch src = sy::parse(ss.str());
        float o[ty::kParamCount], ext[2];
        sy::toOnetype(src.v, o, ext);
        const std::string name = sy::patchName(src.name);
        presets::Preset pr{ot::SynthModel::ACD, name.c_str(), "", 1, {}, {}, ty::kEngineId};
        for (int k = 0; k < ty::kParamCount; ++k) pr.edits.push_back({k, o[k]});
        if (ext[1] > 0.0f) pr.edits.push_back({ty::kParamCount + 1, ext[1]});   // Spread
        for (const sy::Macro& m : sy::macros(o)) {
            presets::Macro pm{m.name, m.bipolar, {}};
            for (const sy::Target& t : m.targets) pm.targets.push_back({t.param, t.amount});
            pr.macros.push_back(pm);
        }
        for (const sy::Target& t : sy::velocityExtra(src.v)) pr.velocity.push_back({t.param, t.amount});
        std::string err;
        if (!presets::build(*app, 0, pr, err)) {
            std::fprintf(stderr, "FAIL  %s: %s\n", path.c_str(), err.c_str());
            ++failures;
            continue;
        }
        // M7 -> Volume: as much as the patch's velocity sense.
        ot::PatchMacro& m7 = app->synthPatch(0).macros[ot::kPatchMacroVelocity];
        for (int t = 0; t < m7.count; ++t)
            if (m7.targets[t].param == ty::kVolume) m7.targets[t].amountBits = ot::patchFloatToBits(sy::velocityVolume(src.v));
        if (!app->saveSynthLibraryPatch(0, name.c_str())) {
            std::fprintf(stderr, "FAIL  %s: Save As failed\n", path.c_str());
            ++failures;
            continue;
        }
        const std::vector<std::string> notes = sy::report(src.v);
        const std::string txt = std::string(root) + "/Patches/1TYPE/" + name + ".txt";
        if (std::FILE* r = std::fopen(txt.c_str(), "w")) {
            std::fprintf(r, "%s (from %s%s)\n", name.c_str(), path.c_str(), src.legacy() ? ", an old version, converted" : "");
            for (const std::string& n : notes) std::fprintf(r, "  %s\n", n.c_str());
            std::fclose(r);
        }
        std::printf("ok    Patches/1TYPE/%s.patch\n", name.c_str());
        for (const std::string& n : notes) std::printf("        %s\n", n.c_str());
        ++made;
    }
    std::printf("sy1_to_patch: %d patch(es) in %s/Patches/1TYPE\n", made, root);
    return failures ? 1 : 0;
}
