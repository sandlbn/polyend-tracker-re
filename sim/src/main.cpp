// OpenTracker desktop simulator: runs the platform-independent core (src/core)
// against host implementations of the HAL interfaces (src/core/hal_api.h).
//
//   opentracker-sim [--sd DIR] [--scale 1|2] [--line-in] [--no-audio] [--no-midi]
//                   [--demo] [--quit-after MS]
//   opentracker-sim --script FILE [--screenshot OUT.png] [--panel] [--wav OUT.wav] [--sd DIR]
//
// See sim/README.md for the key map and the script language.
#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "canvas.h"
#include "headless.h"
#include "machine.h"
#include "panel.h"
#include "remote/display_tap.h"
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
    std::string frame;    // draw a recorded frame (.otf) for --screenshot
    std::string frames;   // headless: save the frame at each "budget" as DIR/NAME.otf (tools/eve_emu)
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
        "  --frames DIR      at each 'budget' also save the frame (fragment cache off) as DIR/NAME.otf\n"
        "                    (tools/eve_emu replays them on the FT81x emulator)\n"
        "  --frame FILE.otf  draw a saved frame (with --screenshot)\n"
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
        if (a == "--frame" && next(o.frame)) continue;
        if (a == "--frames" && next(o.frames)) continue;
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

// --frame: a saved frame (core/remote/display_tap.h) into a PNG.
int drawFrameFile(Machine& m, const Options& o) {
    std::ifstream f(o.frame, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot read %s\n", o.frame.c_str());
        return 2;
    }
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    m.display.setRomFontsOnly(o.romFonts);
    const bool ok = ot::remote::replayFrame(data.data(), data.size(), m.display);
    if (!ok) std::fprintf(stderr, "%s: malformed or cut frame (drawn up to there)\n", o.frame.c_str());
    if (!o.screenshot.empty()) saveShot(m, o.screenshot, o.panelShot);
    return ok ? 0 : 1;
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
    if (!o.frame.empty()) return drawFrameFile(m, o);
    ot::setRomTextMode(o.romText ? ot::kRomText : ot::kRomGlyphs);
    MachineOptions mo;
    mo.sd = o.sd;
    mo.lineIn = o.lineIn;
    mo.noAudio = o.noAudio;
    mo.noMidi = o.noMidi;
    mo.demo = o.demo;
    mo.romFonts = o.romFonts;
    mo.busyDisplay = o.busyDisplay;
    mo.wav = o.wav;
    mo.lineInTone = o.lineInTone;
    mo.frames = !o.frames.empty();
    if (!m.start(mo, headless)) return 1;
    if (!headless) {
        if (m.audio) m.audio->start();   // App::init already started it; harmless
        return runInteractive(m, o);
    }
    Headless h{m, o.panelShot, o.frames};
    if (m.audio) m.audio->start();
    h.advance(300);   // boot and first frame
    bool ok = o.script.empty() || h.run(o.script);
    h.advance(50);
    if (!o.screenshot.empty()) saveShot(m, o.screenshot, o.panelShot);
    m.shutdown();
    return ok ? 0 : 1;
}
