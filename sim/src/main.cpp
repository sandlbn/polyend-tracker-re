// OpenTracker desktop simulator: runs the platform-independent core (src/core)
// against host implementations of the HAL interfaces (src/core/hal_api.h).
//
//   opentracker-sim [--sd DIR] [--scale 1|2] [--line-in] [--no-audio] [--no-midi]
//                   [--demo] [--quit-after MS]
//   opentracker-sim --script FILE [--screenshot OUT.png] [--panel] [--wav OUT.wav] [--sd DIR]
//
// See sim/README.md for the key map and the script language.
#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "app/storage_stream_reader.h"
#include "app/app.h"
#include "formats/wav.h"
#include "canvas.h"
#include "host_hal.h"
#include "host_midi.h"
#include "host_storage.h"
#include "sim_usb_storage.h"
#include "panel.h"
#include "png.h"
#include "sdl_audio.h"
#include "sim_nvstore.h"
#include "sw_display.h"
#include "ui/buttons.h"
#include "ui/screens.h"

namespace b = ot::btn;
using namespace sim;

namespace {

struct Options {
    std::string sd;
    int scale = 1;
    bool lineIn = false, noAudio = false, noMidi = false, demo = false, panelShot = false;
    bool romFonts = false, romText = false;   // OP-6 checks (also OT_SIM_ROM_FONTS / OT_SIM_ROM_TEXT = 1)
    bool busyDisplay = true;                  // FT81x pacing model (off: --instant-display / OT_SIM_BUSY = 0)
    std::string script, screenshot, wav;
    float lineInTone = 0.0f;   // headless: synthetic line input (Hz), 0 = none
    long quitAfterMs = -1;
};

void usage() {
    std::printf(
        "usage: opentracker-sim [options]\n"
        "  --sd DIR          SD card root (default ~/.opentracker-sd)\n"
        "  --scale N         window scale 1 or 2 (default 1)\n"
        "  --line-in         open the default capture device as line-in\n"
        "  --no-audio        do not open an audio device\n"
        "  --no-midi         do not create ALSA MIDI ports\n"
        "  --demo            renderer/input demo instead of the app\n"
        "  --quit-after MS   exit after MS milliseconds (interactive mode)\n"
        "headless (no window, virtual time):\n"
        "  --script FILE     replay scripted input (see README)\n"
        "  --screenshot PNG  save the framebuffer at the end\n"
        "  --panel           screenshots include the whole front panel\n"
        "  --wav OUT.wav     record the rendered audio\n"
        "  --line-in-tone HZ synthetic line input: tone bursts (recorder tests)\n"
        "  --rom-fonts       never bind the custom fonts (FT81x ROM font fallback)\n"
        "  --rom-text        ROM fallback text as CMD_TEXT (per glyph at the grid pitch), not VERTEX2II\n"
        "  --instant-display show every frame at once, frameReady() always true (default: the FT81x\n"
        "                    frame pacing model -- upload time, shown at the 54 Hz vsync)\n");
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--sd" && next(o.sd)) continue;
        if (a == "--scale" && next(v)) { o.scale = std::atoi(v.c_str()) >= 2 ? 2 : 1; continue; }
        if (a == "--line-in") { o.lineIn = true; continue; }
        if (a == "--no-audio") { o.noAudio = true; continue; }
        if (a == "--no-midi") { o.noMidi = true; continue; }
        if (a == "--demo") { o.demo = true; continue; }
        if (a == "--panel") { o.panelShot = true; continue; }
        if (a == "--rom-fonts") { o.romFonts = true; continue; }
        if (a == "--rom-text") { o.romText = true; continue; }
        if (a == "--instant-display") { o.busyDisplay = false; continue; }
        if (a == "--script" && next(o.script)) continue;
        if (a == "--screenshot" && next(o.screenshot)) continue;
        if (a == "--wav" && next(o.wav)) continue;
        if (a == "--line-in-tone" && next(v)) { o.lineInTone = float(std::atof(v.c_str())); continue; }
        if (a == "--quit-after" && next(v)) { o.quitAfterMs = std::atol(v.c_str()); continue; }
        if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        std::fprintf(stderr, "unknown or incomplete option: %s\n", a.c_str());
        usage();
        return false;
    }
    // The visual-diff tool starts the simulator itself: the environment
    // selects the ROM font modes for all its scenes.
    if (const char* e = std::getenv("OT_SIM_ROM_FONTS")) o.romFonts = o.romFonts || std::atoi(e) == 1;
    if (const char* e = std::getenv("OT_SIM_BUSY")) o.busyDisplay = o.busyDisplay && std::atoi(e) != 0;
    if (const char* e = std::getenv("OT_SIM_ROM_TEXT")) o.romText = o.romText || std::atoi(e) == 1;
    if (o.sd.empty()) {
        const char* home = std::getenv("HOME");
        o.sd = std::string(home ? home : ".") + "/.opentracker-sd";
    }
    return true;
}

// ---------------------------------------------------------------------------
// The machine: HAL objects + App (or the demo).

struct Machine {
    SimClock clock;
    SwDisplay display;
    SimInput input{clock};
    SimLeds leds;
    HostStorage storage;
    ot::StorageStreamReader streamReader{storage};   // SD streaming: host reader over the card folder
    SimUsbStorage usbStorage{storage};
    HostMidi midi;
    SdlAudio sdlAudio;
    ManualAudio manualAudio;
    // Synthetic line input: 0.25 s silence, then 0.4 s tone bursts every 0.6 s
    // (L at hz, R a fifth above: distinct stereo lanes).
    struct ToneIn {
        float hz = 440.0f;
        uint64_t n = 0;
        int16_t buf[128 * 2] = {};
        const int16_t* block() {
            for (int i = 0; i < 128; ++i, ++n) {
                const double t = double(n) / 44100.0;
                const double ph = std::fmod(t - 0.25, 0.6);
                const double env = t < 0.25 || ph > 0.4 ? 0.0 : std::sin(3.14159265 * ph / 0.4);
                // Distinct lanes for the 2.x stereo recorder: R a fifth above, softer.
                buf[2 * i] = int16_t(12000.0 * env * std::sin(2.0 * 3.14159265 * hz * t));
                buf[2 * i + 1] = int16_t(9000.0 * env * std::sin(2.0 * 3.14159265 * 1.5 * hz * t));
            }
            return buf;
        }
    } tone;
    ot::IAudioOut* audio = nullptr;
    bool demo = false;

    // App and its large buffers (SDRAM on the device).
    std::unique_ptr<ot::App> app;
    std::vector<int16_t> sampleBank, fxMemory;
    std::unique_ptr<ot::PatternStock[]> patterns;
    std::unique_ptr<ot::Pattern[]> undo, fileUndo, clipboard, seqBuffers;
    std::vector<ot::Track> extTracks;
    std::vector<uint8_t> extMeta;
    uint8_t undoNums[20] = {}, fileUndoNums[10] = {};
    std::vector<uint8_t> scratch;
    std::unique_ptr<ot::PerfPatternMemory> perfPatterns;
    SimNvStore nvstore;   // config EEPROM stand-in: "<sd>.eeprom" next to the SD folder

    // demo state
    std::string demoLog[8];
    int demoEnc = 0;
    uint32_t demoLastFrame = 0;

    bool start(const Options& o, bool headless) {
        demo = o.demo;
        clock.setVirtual(headless);
        if (!storage.init(o.sd)) {
            std::fprintf(stderr, "cannot use SD folder %s\n", o.sd.c_str());
            return false;
        }
        if (!o.noMidi && !headless) midi.open();
        if (headless) {
            audio = &manualAudio;
            if (!o.wav.empty() && !manualAudio.recordWav(o.wav.c_str()))
                std::fprintf(stderr, "cannot write %s\n", o.wav.c_str());
        } else if (!o.noAudio) {
            sdlAudio.enableLineIn(o.lineIn);
            audio = &sdlAudio;
        }
        if (demo) return true;

        sampleBank.assign(ot::kSampleBankBytes / 2, 0);
        fxMemory.assign(ot::audio::AudioEngine::kFxMemorySamples, 0);
        patterns.reset(new ot::PatternStock[ot::kPatternCount]);
        seqBuffers.reset(new ot::Pattern[3]);   // current, preload, switch stage
        extTracks.assign(ot::ExtTrackPool::kDefaultBlocks + 1, ot::Track{});
        extMeta.assign(ot::ExtTrackPool::metaBytes(ot::ExtTrackPool::kDefaultBlocks), 0);
        undo.reset(new ot::Pattern[20]);
        fileUndo.reset(new ot::Pattern[10]);
        clipboard.reset(new ot::Pattern[1]);
        scratch.assign(64 * 1024, 0);
        perfPatterns.reset(new ot::PerfPatternMemory);
        std::string sdPath = o.sd;
        while (sdPath.size() > 1 && sdPath.back() == '/') sdPath.pop_back();
        nvstore.open(sdPath + ".eeprom");

        ot::HalRefs h;
        h.display = &display;
        h.audio = audio;
        h.storage = &storage;
        h.streamReader = &streamReader;
        h.midi = &midi;
        h.input = &input;
        h.leds = &leds;
        h.clock = &clock;
        h.nvstore = &nvstore;
        h.usbStorage = &usbStorage;   // 2.x USB Storage mode (no real USB in the sim)
        if (audio == &sdlAudio) {
            // Line-in goes through the engine (spec 04 §1.3 IG/CHMODE).
            h.lineIn = [](void* c) { return static_cast<SdlAudio*>(c)->lineIn(); };
            h.lineInCtx = &sdlAudio;
        }
        if (o.lineInTone > 0.0f) {
            tone.hz = o.lineInTone;
            h.lineIn = [](void* c) { return static_cast<ToneIn*>(c)->block(); };
            h.lineInCtx = &tone;
        }
        // The capture device stands in for line in and both mic gains; there
        // is no FM tuner (HalRefs::radio stays null -> "not available").
        h.selectInput = [](void*, uint8_t source, uint8_t) { return source != 1; };
        ot::AppMemory m;
        m.sampleBank = sampleBank.data();
        m.sampleBankSamples = uint32_t(sampleBank.size());
        m.fxMemory = fxMemory.data();
        m.fxSamples = uint32_t(fxMemory.size());
        m.patterns = patterns.get();
        m.seqBuffers = seqBuffers.get();
        m.seqStage = seqBuffers.get() + 2;
        m.extTracks = extTracks.data();
        m.extTrackBlocks = int(extTracks.size());
        m.extMeta = extMeta.data();
        m.undo = undo.get();
        m.undoNums = undoNums;
        m.undoSlots = 20;      // spec 05 §5.1
        m.fileUndo = fileUndo.get();
        m.fileUndoNums = fileUndoNums;
        m.fileUndoSlots = 10;  // spec 05 §5.1 (b)
        m.clipboard = clipboard.get();
        m.scratch = scratch.data();
        m.scratchBytes = uint32_t(scratch.size());
        m.perfPatterns = perfPatterns.get();
        app.reset(new ot::App());
        if (!app->init(h, m)) {
            std::fprintf(stderr, "App::init failed\n");
            return false;
        }
        // The Ui RAM_DL guard runs here too (the frame is cut as on the
        // device); report it like the device's diag log does.
        app->ui.platform.dlOverflow = [](const char* screen, uint32_t words, uint32_t dropped, bool low) {
            std::fprintf(stderr, "ui: display list over budget on %s: cut at %u words, %u primitives dropped%s\n",
                         screen, unsigned(words), unsigned(dropped), low ? " (low detail)" : "");
        };
        return true;
    }

    void step() {
        if (app) {
            app->loop();
            // Display brightness follows the config like the device glue
            // (src/app_main.cpp applyBrightness, spec 05 §1.8).
            const uint8_t level = app->cfg.brightness > 2 ? 2 : app->cfg.brightness;
            if (level != appliedBrightness) {
                appliedBrightness = level;
                display.setBacklight(ot::IDisplay::brightnessDuty(level));
            }
        } else {
            demoStep();
        }
    }
    uint8_t appliedBrightness = 0xFF;

    void shutdown() {
        if (audio) audio->stop();
        if (app) app->autosave();
    }

    // --- demo: exercise the renderer and echo inputs -----------------------
    void demoStep() {
        ot::InputEvent ev;
        while (input.poll(ev)) {
            char buf[64];
            const char* t[] = {"none", "button down", "button up", "pad down", "pad up", "encoder"};
            if (ev.type == ot::InputEvent::kEncoder) demoEnc += ev.delta;
            std::snprintf(buf, sizeof(buf), "%6u ms  %s id=%u vel=%u delta=%d", ev.timeMs, t[ev.type], ev.id,
                          ev.velocity, ev.delta);
            for (int i = 7; i > 0; --i) demoLog[i] = demoLog[i - 1];
            demoLog[0] = buf;
            if (ev.type == ot::InputEvent::kPadDown) leds.setPad(ev.id, 0, 160, 255);
            if (ev.type == ot::InputEvent::kPadUp) leds.setPad(ev.id, 0, 0, 0);
            leds.show();
        }
        const uint32_t now = clock.millis();
        if (now - demoLastFrame < 16) return;
        demoLastFrame = now;
        demoFrame(now);
    }

    void demoFrame(uint32_t now) {
        ot::IDisplay& d = display;
        d.dlStart();
        d.clearColorRGB(16, 18, 24);
        d.clear();
        d.colorRGB(50, 49, 50);
        d.rect(0, 0, 799, 27);
        d.colorRGB(255, 255, 255);
        d.text(8, 14, 28, ot::IDisplay::kOptCenterY, "OpenTracker simulator - renderer demo");
        d.number(792, 14, 26, ot::IDisplay::kOptCenterY | ot::IDisplay::kOptRightX, int32_t(now));
        int y = 34;
        for (int f = 16; f <= 31; ++f) {
            if (f == 24) y = 34;
            const int x = f < 24 ? 8 : 300;
            d.colorRGB(f >= 26 ? 255 : 160, 220, f >= 26 ? 160 : 255);
            char s[32];
            std::snprintf(s, sizeof(s), "%d: Ag 0123", f);
            d.text(int16_t(x), int16_t(y), uint8_t(f), 0, s);
            const ot::IDisplay::Opt none = ot::IDisplay::kOptNone;
            (void)none;
            y += romFont(uint8_t(f))->height + 2;
        }
        // Primitives.
        d.colorRGB(255, 80, 60);
        d.lineWidth(48);
        d.begin(ot::IDisplay::kRects);
        d.vertex2f(600 * 16, 40 * 16);
        d.vertex2f(780 * 16, 90 * 16);
        d.end();
        d.lineWidth(16);
        d.colorRGB(60, 200, 90);
        d.colorA(128);
        d.begin(ot::IDisplay::kPoints);
        d.pointSize(30 * 16);
        d.vertex2f(650 * 16, 140 * 16);
        d.vertex2f(690 * 16, 150 * 16);
        d.end();
        d.colorA(255);
        d.colorRGB(255, 220, 0);
        d.lineWidth(24);
        d.begin(ot::IDisplay::kLineStrip);
        for (int i = 0; i <= 18; ++i) {
            const int x = 590 + i * 11;
            const int yy = 210 + int(30 * ((i * 7 + int(now / 50)) % 13 - 6) / 6);
            d.vertex2f(x * 16, yy * 16);
        }
        d.end();
        d.colorRGB(40, 90, 200);
        d.begin(ot::IDisplay::kEdgeStripB);
        for (int i = 0; i <= 10; ++i) d.vertex2f((590 + i * 20) * 16, (300 - (i % 3) * 12) * 16);
        d.end();
        // Scissored bar.
        d.scissorXY(590, 330);
        d.scissorSize(200, 14);
        d.colorRGB(255, 255, 255);
        d.text(590 + int16_t((now / 20) % 260) - 60, 330, 26, 0, "scissor clip test");
        d.scissorXY(0, 0);
        d.scissorSize(2048, 2048);
        d.colorRGB(200, 200, 200);
        d.text(590, 360, 27, 0, "encoder:");
        d.number(700, 360, 27, ot::IDisplay::kOptSigned, demoEnc);
        // Input echo.
        d.colorRGB(180, 230, 180);
        for (int i = 0; i < 8; ++i) d.text(8, int16_t(330 + i * 16), 18, 0, demoLog[i].c_str());
        // Bottom labels like the app's bar (spec 06 §0.3).
        for (int i = 0; i < 8; ++i) {
            d.colorRGB(input.buttonDown(uint8_t(i)) ? 120 : 50, 49, 50);
            d.rect(int16_t(50 + 100 * i - 47), 425, int16_t(50 + 100 * i + 46), 478);
            d.colorRGB(255, 255, 255);
            char s[8];
            std::snprintf(s, sizeof(s), "F%d", i + 1);
            d.text(int16_t(50 + 100 * i), 452, 27, ot::IDisplay::kOptCenter, s);
        }
        d.swap();
    }
};

void saveShot(Machine& m, const std::string& path, bool panel) {
    std::vector<uint32_t> fb(800 * 480);
    m.display.frontCopy(fb.data());
    bool ok;
    if (panel) {
        Canvas c(Panel::kW, Panel::kH);
        Panel().draw(c, fb.data(), m.input, m.leds, 0, "headless screenshot");
        ok = writePng(path.c_str(), c.pixels(), c.width(), c.height());
    } else {
        ok = writePng(path.c_str(), fb.data(), 800, 480);
    }
    std::fprintf(stderr, "%s %s\n", ok ? "saved" : "FAILED to save", path.c_str());
}

// ---------------------------------------------------------------------------
// Headless script runner (virtual time; audio blocks pulled every 128 frames).

// Every Ui::notify() text since the last check (expect-open-ok).
std::vector<std::string> gNotices;
void recordNotice(const char* text) { gNotices.push_back(text); }

struct Headless {
    Machine& m;
    bool panel;
    uint64_t frameAcc = 0;   // frames * 1000
    // Display budget (2.x performance workstream): the largest per-frame
    // command-FIFO upload since the last "budget" / "budget-reset".
    uint32_t seenFrames = 0, maxUpload = 0, maxDl = 0, budgetFrames = 0;

    void sampleFrame() {
        if (!m.app || m.app->ui.frames() == seenFrames) return;
        seenFrames = m.app->ui.frames();
        ++budgetFrames;
        maxUpload = std::max(maxUpload, m.app->ui.lastUploadWords());
        maxDl = std::max(maxDl, m.app->ui.lastDlWords());
    }

    // budget NAME MAXWORDS: fail when a frame since the last reset uploaded
    // more than MAXWORDS display-list words (or the RAM_DL total passed 2000).
    bool budget(const std::string& name, long maxWords, int line) {
        // A screen at rest draws nothing: redraw it until its cached blocks
        // are captured, then measure one more redraw (the steady state of a
        // change that does not touch the static blocks).
        if (m.app && budgetFrames < 4) {
            for (int i = 0; i < 4; ++i) {
                m.app->ui.dirty();
                advance(20);
            }
            maxUpload = maxDl = budgetFrames = 0;
            m.app->ui.dirty();
            advance(20);
        }
        const bool ok = budgetFrames > 0 && long(maxUpload) <= maxWords && maxDl <= 2000;
        std::fprintf(stderr, "budget %-18s %4u words max (budget %4ld), RAM_DL %4u, %u frames%s\n", name.c_str(),
                     unsigned(maxUpload), maxWords, unsigned(maxDl), unsigned(budgetFrames),
                     ok ? "" : "  <-- OVER BUDGET");
        if (!ok) std::fprintf(stderr, "script:%d: display budget exceeded for %s\n", line, name.c_str());
        maxUpload = maxDl = budgetFrames = 0;
        return ok;
    }

    // fragcheck NAME: the current screen drawn inline and through cached
    // fragments (probe, capture, append) must give identical pixels.
    bool fragCheck(const std::string& name, int line) {
        if (!m.app) return true;
        ot::Ui& ui = m.app->ui;
        std::vector<uint32_t> a(800 * 480), b(800 * 480);
        m.display.setFragments(false);
        ui.render();
        m.display.frontCopy(a.data());
        const uint32_t inlineWords = ui.lastUploadWords();
        m.display.setFragments(true);
        ui.fragments.reset();
        for (int i = 0; i < 4; ++i) ui.render();   // probes until stable, capture, append
        m.display.frontCopy(b.data());
        size_t bad = 0;
        for (size_t i = 0; i < a.size(); ++i) bad += a[i] != b[i];
        std::fprintf(stderr, "fragcheck %-15s inline %4u words, cached %4u words (%u appends)%s\n", name.c_str(),
                     unsigned(inlineWords), unsigned(ui.lastUploadWords()), unsigned(m.display.lastFrameAppends()),
                     bad ? "  <-- PIXELS DIFFER" : "");
        if (bad) {
            int x0 = 800, y0 = 480, x1 = -1, y1 = -1;
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i] != b[i]) {
                    const int x = int(i % 800), y = int(i / 800);
                    x0 = std::min(x0, x); y0 = std::min(y0, y);
                    x1 = std::max(x1, x); y1 = std::max(y1, y);
                }
            std::fprintf(stderr, "script:%d: %zu pixels differ with fragments on %s (box %d,%d..%d,%d)\n", line, bad,
                         name.c_str(), x0, y0, x1, y1);
        }
        seenFrames = ui.frames();
        return bad == 0;
    }

    // romcheck NAME (ROM fonts active): the current screen with the ROM-font
    // runs as CMD_TEXT and as VERTEX2II glyphs (OP-6) must give identical
    // pixels; prints both upload sizes.
    bool romCheck(const std::string& name, int line) {
        if (!m.app) return true;
        ot::Ui& ui = m.app->ui;
        if (ot::customFontsBound()) {
            std::fprintf(stderr, "script:%d: romcheck needs 'romfonts on'\n", line);
            return false;
        }
        std::vector<uint32_t> a(800 * 480), b(800 * 480);
        m.display.setFragments(false);
        ot::setRomTextMode(ot::kRomText);
        ui.render();
        m.display.frontCopy(a.data());
        const uint32_t textWords = ui.lastUploadWords();
        ot::setRomTextMode(ot::kRomGlyphs);
        ui.render();
        m.display.frontCopy(b.data());
        const uint32_t glyphWords = ui.lastUploadWords();
        m.display.setFragments(true);
        size_t bad = 0;
        for (size_t i = 0; i < a.size(); ++i) bad += a[i] != b[i];
        std::fprintf(stderr, "romcheck %-16s CMD_TEXT %4u words, VERTEX2II %4u words%s\n", name.c_str(),
                     unsigned(textWords), unsigned(glyphWords), bad ? "  <-- PIXELS DIFFER" : "");
        if (bad) {
            int x0 = 800, y0 = 480, x1 = -1, y1 = -1;
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i] != b[i]) {
                    const int x = int(i % 800), y = int(i / 800);
                    x0 = std::min(x0, x); y0 = std::min(y0, y);
                    x1 = std::max(x1, x); y1 = std::max(y1, y);
                }
            std::fprintf(stderr, "script:%d: %zu pixels differ between ROM CMD_TEXT and VERTEX2II on %s (box %d,%d..%d,%d)\n",
                         line, bad, name.c_str(), x0, y0, x1, y1);
        }
        seenFrames = ui.frames();
        return bad == 0;
    }

    void advance(long ms) {
        for (long i = 0; i < ms; ++i) {
            m.clock.advanceUs(1000);
            frameAcc += uint64_t(ot::IAudioOut::kSampleRate);
            while (frameAcc >= uint64_t(ot::IAudioOut::kBlockFrames) * 1000) {
                frameAcc -= uint64_t(ot::IAudioOut::kBlockFrames) * 1000;
                m.manualAudio.pull();
            }
            m.step();
            sampleFrame();
        }
    }

    bool button(const std::string& name, bool down, int line) {
        const int id = buttonByName(name.c_str());
        if (id < 0) {
            std::fprintf(stderr, "script:%d: unknown button '%s'\n", line, name.c_str());
            return false;
        }
        m.input.button(uint8_t(id), down);
        return true;
    }

    bool run(const std::string& path) {
        if (m.app) m.app->ui.platform.notified = &recordNotice;
        std::ifstream f(path);
        if (!f) {
            std::fprintf(stderr, "cannot read script %s\n", path.c_str());
            return false;
        }
        std::string lineStr;
        int line = 0;
        while (std::getline(f, lineStr)) {
            ++line;
            const size_t hash = lineStr.find('#');
            if (hash != std::string::npos) lineStr.resize(hash);
            std::istringstream ss(lineStr);
            std::string cmd, a1, a2, a3;
            ss >> cmd >> a1 >> a2 >> a3;
            if (cmd.empty()) continue;
            for (auto& ch : cmd) ch = char(std::tolower(uint8_t(ch)));
            const long n1 = std::atol(a1.c_str());
            if (cmd == "wait") advance(n1);
            else if (cmd == "press" || cmd == "down") { if (!button(a1, true, line)) return false; advance(1); }
            else if (cmd == "release" || cmd == "up") { if (!button(a1, false, line)) return false; advance(1); }
            else if (cmd == "tap" || cmd == "hold") {
                const long t = a2.empty() ? (cmd == "tap" ? 60 : 600) : std::atol(a2.c_str());
                if (!button(a1, true, line)) return false;
                advance(t);
                button(a1, false, line);
                advance(cmd == "tap" ? 60 : 1);
            } else if (cmd == "pad") {
                const int p = int(n1);
                const int vel = a3.empty() ? 100 : std::atoi(a3.c_str());
                if (a2 == "down") m.input.pad(uint8_t(p), true, uint8_t(vel));
                else if (a2 == "up") m.input.pad(uint8_t(p), false);
                else {   // tap
                    m.input.pad(uint8_t(p), true, uint8_t(vel));
                    advance(60);
                    m.input.pad(uint8_t(p), false);
                }
                advance(1);
            } else if (cmd == "enc") {
                m.input.encoder(int(n1));
                advance(1);
            } else if (cmd == "screenshot") {
                saveShot(m, a1, panel);
            } else if (cmd == "panelshot") {
                saveShot(m, a1, true);
            } else if (cmd == "budget") {
                if (!budget(a1, std::atol(a2.c_str()), line)) return false;
            } else if (cmd == "budget-reset") {
                maxUpload = maxDl = budgetFrames = 0;
            } else if (cmd == "fragcheck") {
                if (!fragCheck(a1, line)) return false;
            } else if (cmd == "romfonts") {
                // romfonts on|off: ROM font fallback (custom fonts unbound) or not.
                if (m.app) {
                    m.display.setRomFontsOnly(a1 != "off");
                    m.app->applySequencerSettings();   // applyFontSet()
                    m.app->ui.dirty();
                }
                advance(1);
            } else if (cmd == "romcheck") {
                if (!romCheck(a1, line)) return false;
            } else if (cmd == "usbstorage") {
                // Interface event 10 (spec 01 §6.1); 2.x: the USB Storage mode
                // (Config > USB > USB Storage), the card locked while it is on.
                if (m.app) m.app->ui.setUsbStorageMode(a1 != "off");
                advance(1);
            } else if (cmd == "usbhost") {
                // usbhost none|waiting|inuse|ejected: what the "computer" does.
                using H = ot::IUsbStorage::Host;
                m.usbStorage.setHost(a1 == "waiting" ? H::kWaiting
                                     : a1 == "inuse" ? H::kInUse
                                     : a1 == "ejected" ? H::kEjected
                                                       : H::kNoUsb);
                advance(1);
            } else if (cmd == "expect-usbstorage") {
                // expect-usbstorage on|off: mode, page and card lock agree; no
                // app access reached the card while it was locked.
                const bool want = a1 != "off";
                const bool mode = m.app && m.app->usbStorageActive();
                const bool page = m.app && m.app->ui.usbStorageMode();
                if (mode != want || page != want || m.storage.locked() != want || m.storage.lockedAccess() != 0) {
                    std::fprintf(stderr, "script:%d: usb storage mode %d page %d card locked %d (%d refused accesses), want %d\n",
                                 line, int(mode), int(page), int(m.storage.locked()), m.storage.lockedAccess(), int(want));
                    return false;
                }
            } else if (cmd == "expect-usbwarn") {
                // expect-usbwarn on|off: the mode page shows the eject reminder.
                const bool want = a1 != "off";
                const bool got = m.app && m.app->ui.usb.ejectWarningShown();
                if (got != want) {
                    std::fprintf(stderr, "script:%d: eject reminder %d, want %d\n", line, int(got), int(want));
                    return false;
                }
            } else if (cmd == "theme" || cmd == "font") {
                // Display theme 0..2 / font set 0..1 (config "Display theme" /
                // "Display font") without the Config key flow; visual scenes.
                if (m.app) {
                    if (cmd == "theme") m.app->cfg.theme = uint8_t(n1);
                    else m.app->cfg.font = uint8_t(n1);
                    m.app->applySequencerSettings();
                    m.app->ui.dirty();
                }
                advance(1);
            } else if (cmd == "config" && a1 == "stream") {
                // config stream all|auto|off: Config > Stream from card, applied
                // like the Config screen does (autosave, then the Workspace
                // reloads with the new choice; without a saved Workspace it
                // applies at the next project load). SD streaming test songs.
                const uint8_t mode = a2 == "all" ? 2 : (a2 == "auto" ? 1 : (a2 == "off" ? 0 : 0xFF));
                if (!m.app || mode == 0xFF || (mode != 0 && !m.app->hal().streamReader)) {
                    std::fprintf(stderr, "script:%d: config stream all|auto|off (streaming in this build)\n", line);
                    return false;
                }
                if (m.app->cfg.streamMode != mode) {
                    m.app->cfg.streamMode = mode;
                    m.app->markConfigDirty();
                    m.app->autosave();
                    m.app->startLoadWorkspace();
                }
                advance(1);
            } else if (cmd == "expect-streams") {
                // expect-streams N: instruments bound to a card stream.
                const int got = m.app ? m.app->streamedCount() : -1;
                if (got != n1) {
                    std::fprintf(stderr, "script:%d: %d streamed instruments, want %ld\n", line, got, n1);
                    return false;
                }
            } else if (cmd == "expect-underruns") {
                // expect-underruns N: at most N stream underruns (all voices, running totals).
                long got = 0;
                if (m.app)
                    for (int v = 0; v < ot::audio::kVoices; ++v) got += long(m.app->streamRings()[v].underruns);
                if (!m.app || got > n1) {
                    std::fprintf(stderr, "script:%d: %ld stream underruns, want at most %ld\n", line, got, n1);
                    return false;
                }
            } else if (cmd == "expect-notice") {
                // expect-notice TEXT: a notice containing TEXT was shown since
                // the last expect-open-ok; it is taken off the list
                // expect-open-ok checks.
                std::string want = lineStr.substr(lineStr.find(a1));
                while (!want.empty() && (want.back() == ' ' || want.back() == '\r')) want.pop_back();
                const auto it = std::find_if(gNotices.begin(), gNotices.end(),
                                             [&](const std::string& n) { return n.find(want) != std::string::npos; });
                if (it == gNotices.end()) {
                    std::fprintf(stderr, "script:%d: no notice \"%s\"\n", line, want.c_str());
                    return false;
                }
                gNotices.erase(it);
            } else if (cmd == "expect-open-ok") {
                // The last project job finished without errors, no popup is
                // open, and no notice other than "Project opened" was shown
                // since the script start / the last expect-open-ok (load
                // notices: unreadable files, damaged or too large synth patches,
                // track memory full). Test songs, TESTSONGS.md.
                std::string extra;
                for (const std::string& n : gNotices)
                    if (n != "Project opened") extra += (extra.empty() ? "" : "; ") + n;
                gNotices.clear();
                if (!m.app || m.app->jobBusy() || !m.app->lastJobOk() || m.app->lastJobErrors() ||
                    m.app->ui.askOpen() || m.app->extPoolFullEvents() || !extra.empty()) {
                    std::fprintf(stderr, "script:%d: project job failed or left a notice (ok %d, errors %d) %s\n", line,
                                 m.app ? int(m.app->lastJobOk()) : 0, m.app ? m.app->lastJobErrors() : -1,
                                 extra.c_str());
                    return false;
                }
            } else if (cmd == "expect-project") {
                const std::string want = lineStr.substr(lineStr.find(a1));
                std::string w = want;
                while (!w.empty() && (w.back() == ' ' || w.back() == '\r')) w.pop_back();
                if (!m.app || w != m.app->project.name) {
                    std::fprintf(stderr, "script:%d: project is \"%s\", want \"%s\"\n", line,
                                 m.app ? m.app->project.name : "", w.c_str());
                    return false;
                }
            } else if (cmd == "expect-playing") {
                if (!m.app || !m.app->seq.isPlaying()) {
                    std::fprintf(stderr, "script:%d: not playing\n", line);
                    return false;
                }
            } else if (cmd == "expect-sound") {
                // Peak output since the last expect-sound / sound-reset >= N.
                const int got = m.manualAudio.peak;
                m.manualAudio.peak = 0;
                if (got < n1) {
                    std::fprintf(stderr, "script:%d: output peak %d < %ld\n", line, got, n1);
                    return false;
                }
            } else if (cmd == "sound-reset") {
                m.manualAudio.peak = 0;
            } else if (cmd == "echo") {
                std::fprintf(stderr, "script: %s\n", lineStr.c_str() + lineStr.find(a1));
            } else if (cmd == "expect-screen") {
                // expect-screen NAME: the active screen's name() (e.g. "Performance",
                // "Sample Recorder"); key-flow regression scripts.
                std::string want = lineStr.substr(lineStr.find(a1));
                while (!want.empty() && (want.back() == ' ' || want.back() == '\r')) want.pop_back();
                const ot::Screen* s = m.app ? m.app->ui.current() : nullptr;
                if (!s || want != s->name()) {
                    std::fprintf(stderr, "script:%d: screen is \"%s\", want \"%s\"\n", line, s ? s->name() : "",
                                 want.c_str());
                    return false;
                }
            } else if (cmd == "tone-sample") {
                // tone-sample SLOT FRAMES [stereo]: write a 220 Hz decaying-saw WAV to the
                // SD card and import it into instrument SLOT (1-based), which
                // becomes the current instrument; playhead / slice scripts.
                bool ok = false;
                if (m.app && n1 >= 1 && n1 <= ot::kSampleInstrumentCount) {
                    const uint32_t frames = uint32_t(std::atol(a2.c_str()) > 0 ? std::atol(a2.c_str()) : 44100);
                    // "stereo": a drum-like loop instead (decaying noise hits every
                    // 1/8 s, different in L and R) for the stereo waveform views.
                    const bool st = a3 == "stereo";
                    const uint16_t ch = st ? 2 : 1;
                    std::vector<int16_t> pcm(size_t(frames) * ch);
                    uint32_t x = 1;
                    for (uint32_t i = 0; i < frames; ++i) {
                        if (!st) {
                            pcm[i] = int16_t(20000.0f * (1.0f - float(i) / float(frames)) * (float(i % 200) / 100.0f - 1.0f));
                            continue;
                        }
                        const uint32_t t = i % 5512, hit = (i / 5512) % 4;
                        const float env = std::exp(-float(t) / (hit == 0 ? 1800.0f : 500.0f));
                        for (uint16_t c = 0; c < 2; ++c) {
                            x = x * 1664525u + 1013904223u;
                            const float n = float(int32_t(x >> 16) - 32768) / 32768.0f;
                            const float g = c == 0 ? (hit == 2 ? 0.4f : 0.9f) : (hit == 1 ? 0.4f : 0.8f);
                            pcm[size_t(i) * 2 + c] = int16_t(26000.0f * g * env * n);
                        }
                    }
                    std::vector<uint8_t> wav(size_t(frames) * 2 * ch + 64);
                    wav.resize(ot::wavEncode(pcm.data(), frames, ch, wav.data(), wav.size()));
                    const ot::IStorage::Handle h = m.storage.open("simtone.wav", ot::IStorage::kWrite);
                    if (h >= 0) {
                        ok = m.storage.write(h, wav.data(), uint32_t(wav.size())) == int32_t(wav.size());
                        m.storage.close(h);
                    }
                    ok = ok && m.app->importSample("simtone.wav", int(n1 - 1));
                    if (ok) m.app->setCurrentInstrument(uint8_t(n1 - 1));
                }
                if (!ok) {
                    std::fprintf(stderr, "script:%d: tone-sample failed\n", line);
                    return false;
                }
                advance(1);
            } else if (cmd == "expect-playhead") {
                // expect-playhead on|off: Sample Playback shows (or not) the
                // play-position line (USER_REPORTS_R2 §1c).
                const auto* pb = m.app && m.app->ui.active() == ot::kModPlayback
                                     ? static_cast<const ot::PlaybackScreen*>(m.app->ui.current())
                                     : nullptr;
                const int x = pb ? pb->drawnHeadX() : -1;
                std::fprintf(stderr, "playhead x %d\n", x);
                if (!pb || (a1 == "on") != (x >= 0)) {
                    std::fprintf(stderr, "script:%d: playhead x %d, want %s\n", line, x, a1.c_str());
                    return false;
                }
            } else if (cmd == "expect-master") {
                // expect-master PAGE SUB: Master screen page (0 Master, 1 Track
                // Mixer, 2 Global Mixer) and Master sub-page (0 main, 1 Reverb,
                // 2 Delay, 3 Limiter, 4 EQ); it must be the active screen.
                const auto* mx = m.app && m.app->ui.active() == ot::kModMaster
                                     ? static_cast<const ot::MixerScreen*>(m.app->ui.current())
                                     : nullptr;
                const long sub = std::atol(a2.c_str());
                if (!mx || mx->page != n1 || mx->sub != sub) {
                    std::fprintf(stderr, "script:%d: master page %d sub %d (active %d), want %ld %ld\n", line,
                                 mx ? mx->page : -1, mx ? mx->sub : -1, m.app ? m.app->ui.active() : -1, n1, sub);
                    return false;
                }
            } else if (cmd == "fps-reset" || cmd == "expect-fps") {
                // fps-reset, then expect-fps N: at least N rendered frames per
                // second (virtual time) since the reset. A screen at rest renders
                // nothing; a live meter must keep drawing.
                static uint32_t frames0 = 0, ms0 = 0;
                const uint32_t frames = m.app ? m.app->ui.frames() : 0, ms = m.clock.millis();
                if (cmd == "fps-reset") {
                    frames0 = frames;
                    ms0 = ms;
                } else {
                    const double fps = ms != ms0 ? double(frames - frames0) * 1000.0 / double(ms - ms0) : 0.0;
                    std::fprintf(stderr, "fps %.1f (%u frames in %u ms)\n", fps, unsigned(frames - frames0),
                                 unsigned(ms - ms0));
                    if (fps < double(n1)) {
                        std::fprintf(stderr, "script:%d: %.1f frames/s < %ld\n", line, fps, n1);
                        return false;
                    }
                }
            } else {
                std::fprintf(stderr, "script:%d: unknown command '%s'\n", line, cmd.c_str());
                return false;
            }
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Interactive front end.

int keyToButton(SDL_Scancode sc, bool padMode) {
    switch (sc) {
        case SDL_SCANCODE_F1: return b::F1;
        case SDL_SCANCODE_F2: return b::F2;
        case SDL_SCANCODE_F3: return b::F3;
        case SDL_SCANCODE_F4: return b::F4;
        case SDL_SCANCODE_F5: return b::F5;
        case SDL_SCANCODE_F6: return b::F6;
        case SDL_SCANCODE_F7: return b::F7;
        case SDL_SCANCODE_F8: return b::F8;
        case SDL_SCANCODE_F9: return b::Pattern;
        case SDL_SCANCODE_F10: return b::Song;
        case SDL_SCANCODE_F11: return b::Aux;
        case SDL_SCANCODE_F12: return b::Help;
        case SDL_SCANCODE_PRINTSCREEN:
        case SDL_SCANCODE_PAUSE: return b::Power;   // Shift + Power = screenshot (spec 05 §3.4)
        case SDL_SCANCODE_UP: return b::Up;
        case SDL_SCANCODE_DOWN: return b::Down;
        case SDL_SCANCODE_LEFT: return b::Left;
        case SDL_SCANCODE_RIGHT: return b::Right;
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT: return b::Shift;
        case SDL_SCANCODE_SPACE: return b::Play;
        case SDL_SCANCODE_INSERT: return b::Insert;
        case SDL_SCANCODE_DELETE:
        case SDL_SCANCODE_BACKSPACE: return b::Delete;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: return b::EncPush;
        default: break;
    }
    if (padMode) return -1;
    switch (sc) {
        case SDL_SCANCODE_R: return b::Rec;
        case SDL_SCANCODE_C: return b::Copy;
        case SDL_SCANCODE_N: return b::Note;
        case SDL_SCANCODE_I: return b::Instr;
        case SDL_SCANCODE_X: return b::Fx1;
        case SDL_SCANCODE_V: return b::Fx2;
        case SDL_SCANCODE_P: return b::Pattern;
        case SDL_SCANCODE_A: return b::Aux;
        case SDL_SCANCODE_H: return b::Help;
        default: break;
    }
    // 1..0 -> module keys 0x08..0x11 (the top two rows of the key block).
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) return 0x08 + (sc - SDL_SCANCODE_1);
    return -1;
}

int keyToPad(SDL_Scancode sc) {
    static const SDL_Scancode rows[4][12] = {
        {SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6,
         SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0, SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS},
        {SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_T, SDL_SCANCODE_Y,
         SDL_SCANCODE_U, SDL_SCANCODE_I, SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_LEFTBRACKET,
         SDL_SCANCODE_RIGHTBRACKET},
        {SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H,
         SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_SEMICOLON, SDL_SCANCODE_APOSTROPHE,
         SDL_SCANCODE_BACKSLASH},
        {SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_B, SDL_SCANCODE_N,
         SDL_SCANCODE_M, SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH, SDL_SCANCODE_UNKNOWN,
         SDL_SCANCODE_UNKNOWN},
    };
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 12; ++c)
            if (rows[r][c] == sc && sc != SDL_SCANCODE_UNKNOWN) return r * 12 + c;
    return -1;
}

int runInteractive(Machine& m, const Options& o) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("OpenTracker simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       Panel::kW * o.scale, Panel::kH * o.scale, SDL_WINDOW_RESIZABLE);
    if (!win) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_RenderSetLogicalSize(ren, Panel::kW, Panel::kH);
    SDL_Texture* tex =
        SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, Panel::kW, Panel::kH);

    Panel panel;
    Canvas canvas(Panel::kW, Panel::kH);
    std::vector<uint32_t> fb(800 * 480);
    bool padMode = false;
    uint64_t latched = 0;
    Control mouseCtl;
    bool quit = false;
    uint32_t lastPresent = 0;

    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT: quit = true; break;
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    if (ev.key.repeat) break;   // the sim input does its own auto-repeat
                    const bool down = ev.type == SDL_KEYDOWN;
                    const SDL_Scancode sc = ev.key.keysym.scancode;
                    if (down && sc == SDL_SCANCODE_Q && (ev.key.keysym.mod & KMOD_CTRL)) { quit = true; break; }
                    if (sc == SDL_SCANCODE_TAB || sc == SDL_SCANCODE_CAPSLOCK) {
                        if (down) padMode = !padMode;
                        break;
                    }
                    if (sc == SDL_SCANCODE_PAGEUP || sc == SDL_SCANCODE_PAGEDOWN) {
                        if (down) m.input.encoder(sc == SDL_SCANCODE_PAGEUP ? 1 : -1);
                        break;
                    }
                    if (!padMode && (sc == SDL_SCANCODE_LEFTBRACKET || sc == SDL_SCANCODE_RIGHTBRACKET)) {
                        if (down) m.input.encoder(sc == SDL_SCANCODE_RIGHTBRACKET ? 1 : -1);
                        break;
                    }
                    const int id = keyToButton(sc, padMode);
                    if (id >= 0) {
                        if (!((latched >> id) & 1)) m.input.button(uint8_t(id), down);
                        break;
                    }
                    if (padMode) {
                        const int p = keyToPad(sc);
                        if (p >= 0) m.input.pad(uint8_t(p), down, 100);
                    }
                    break;
                }
                case SDL_MOUSEWHEEL: {
                    int d = ev.wheel.y;
                    if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) d = -d;
                    // Wheel up = clockwise (values go up). On a list (names,
                    // options, rows) clockwise moves down, so the wheel is
                    // flipped there and scrolls with the list (user report).
                    if (m.app && m.app->ui.encoderOnList()) d = -d;
                    if (d) m.input.encoder(d);
                    break;
                }
                case SDL_MOUSEBUTTONDOWN: {
                    const Control c = panel.hit(ev.button.x, ev.button.y);
                    if (ev.button.button == SDL_BUTTON_RIGHT) {
                        if (c.kind == Control::kButton) {   // latch toggle
                            latched ^= 1ull << c.id;
                            m.input.button(c.id, (latched >> c.id) & 1);
                        }
                        break;
                    }
                    if (ev.button.button != SDL_BUTTON_LEFT) break;
                    mouseCtl = c;
                    switch (c.kind) {
                        case Control::kButton:
                            if (!((latched >> c.id) & 1)) m.input.button(c.id, true);
                            break;
                        case Control::kPad: m.input.pad(c.id, true, 100); break;
                        case Control::kEncoderCcw: m.input.encoder(-1); break;
                        case Control::kEncoderCw: m.input.encoder(1); break;
                        case Control::kEncoderPush: m.input.button(b::EncPush, true); break;
                        default: break;
                    }
                    break;
                }
                case SDL_MOUSEBUTTONUP: {
                    if (ev.button.button != SDL_BUTTON_LEFT) break;
                    const Control c = mouseCtl;
                    mouseCtl = Control();
                    if (c.kind == Control::kButton && !((latched >> c.id) & 1)) m.input.button(c.id, false);
                    if (c.kind == Control::kPad) m.input.pad(c.id, false);
                    if (c.kind == Control::kEncoderPush) m.input.button(b::EncPush, false);
                    break;
                }
                default: break;
            }
        }
        m.step();
        const uint32_t now = SDL_GetTicks();
        if (o.quitAfterMs >= 0 && long(now) >= o.quitAfterMs) quit = true;
        if (now - lastPresent >= 16) {
            lastPresent = now;
            m.display.frontCopy(fb.data());
            SwDisplay::dimFrame(fb.data(), fb.size(), m.display.backlight());
            std::string status = std::string("Keys: ") + (padMode ? "PAD mode (letters/digits play pads)"
                                                                  : "COMMAND mode (digits = module keys)") +
                                 "\nTab toggles key mode; right-click latches a button\nSD card: " +
                                 m.storage.root() + "\nMIDI: " +
                                 (m.midi.active() ? "ALSA ports OpenTracker DIN/USB" : "off") +
                                 "\nAudio: " + (m.audio ? "44100 Hz (device: 44117.6 Hz)" : "off") +
                                 "\nCtrl+Q quits";
            panel.draw(canvas, fb.data(), m.input, m.leds, latched, status);
            SDL_UpdateTexture(tex, nullptr, canvas.pixels(), Panel::kW * 4);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, nullptr, nullptr);
            SDL_RenderPresent(ren);
        } else {
            SDL_Delay(1);
        }
    }
    m.shutdown();
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parseArgs(argc, argv, o)) return 2;
    const bool headless = !o.script.empty() || !o.screenshot.empty();
    Machine m;
    m.display.setRomFontsOnly(o.romFonts);
    if (o.busyDisplay) m.display.setBusyClock(&m.clock);
    ot::setRomTextMode(o.romText ? ot::kRomText : ot::kRomGlyphs);
    if (!m.start(o, headless)) return 1;
    if (!headless) {
        if (m.audio) m.audio->start();   // App::init already started it; harmless
        return runInteractive(m, o);
    }
    Headless h{m, o.panelShot};
    if (m.audio) m.audio->start();
    h.advance(300);   // boot and first frame
    bool ok = o.script.empty() || h.run(o.script);
    h.advance(50);
    if (!o.screenshot.empty()) saveShot(m, o.screenshot, o.panelShot);
    m.shutdown();
    return ok ? 0 : 1;
}
