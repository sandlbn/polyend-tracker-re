// OpenTracker simulator -- the machine (see machine.h).
#include "machine.h"

#include <cstdio>

#include "canvas.h"

namespace sim {

bool Machine::start(const MachineOptions& o, bool headless) {
    demo = o.demo;
    display.setRomFontsOnly(o.romFonts);
    if (o.busyDisplay) display.setBusyClock(&clock);
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
        if (o.audioBuffer > 0) sdlAudio.setBufferFrames(o.audioBuffer);
        sdlAudio.setDeviceName(o.audioOut);
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
    if (!o.eepromPath.empty()) {
        nvstore.open(o.eepromPath);
    } else {
        std::string sdPath = o.sd;
        while (sdPath.size() > 1 && sdPath.back() == '/') sdPath.pop_back();
        nvstore.open(sdPath + ".eeprom");
    }

    ot::HalRefs h;
    h.display = &display;
    if (o.frames) {
        // --frames: the display recorder, with room for a frame the guard
        // would cut on the device.
        tap.reset(new ot::remote::DisplayTap(display));
        tapBuf.assign(64 * 1024, 0);
        tap->setBuffer(tapBuf.data(), uint32_t(tapBuf.size()));
        h.display = tap.get();
    }
    h.audio = audio;
    h.storage = &storage;
    h.streamReader = &streamReader;
    h.midi = &midi;
    h.input = &input;
    h.leds = &leds;
    h.clock = &clock;
    h.nvstore = &nvstore;
    // 2.x USB Storage mode (no real USB in the sim: the lock stub); null when
    // the embedder turns it off (MachineOptions::usbStorage).
    h.usbStorage = o.usbStorage ? &usbStorage : nullptr;
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

void Machine::step() {
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

void Machine::shutdown() {
    if (audio) audio->stop();
    if (app) app->autosave();
}

void Machine::demoStep() {
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

void Machine::demoFrame(uint32_t now) {
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

}  // namespace sim
