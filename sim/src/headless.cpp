// OpenTracker simulator -- headless script runner (see headless.h).
#include "headless.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "formats/wav.h"
#include "canvas.h"
#include "panel.h"
#include "png.h"
#include "ui/gfx.h"
#include "ui/screens.h"

namespace sim {

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

// Every Ui::notify() text since the last check (expect-open-ok).
std::vector<std::string> gNotices;
void recordNotice(const char* text) { gNotices.push_back(text); }

void Headless::sampleFrame() {
    if (!m.app || m.app->ui.frames() == seenFrames) return;
    seenFrames = m.app->ui.frames();
    ++budgetFrames;
    maxUpload = std::max(maxUpload, m.app->ui.lastUploadWords());
    maxDl = std::max(maxDl, m.app->ui.lastDlWords());
}

// budget NAME MAXWORDS: fail when a frame since the last reset uploaded
// more than MAXWORDS display-list words (or the RAM_DL total passed 2000).
bool Headless::budget(const std::string& name, long maxWords, int line) {
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
    if (!framesDir.empty() && !saveFrame(name, line)) return false;
    return ok;
}

// --frames: the current screen drawn once more with the fragment cache off
// (the recorded stream has no RAM_G copies) into DIR/NAME.otf; DIR/index.txt
// gets "NAME words" with the UI's RAM_DL count of that frame (driver font
// setup not included). With custom fonts also DIR/NAME.text.otf: the same
// screen with its text as CMD_TEXT (kCustomText, the guard lifted), the
// pixel reference for the glyph runs.
bool Headless::captureFrame(const std::string& path, int line) {
    ot::remote::DisplayTap* t = m.tap.get();
    ot::Ui& ui = m.app->ui;
    const bool was = ui.fragments.enabled;
    ui.fragments.enabled = false;
    t->request();
    t->arm();
    ui.render();
    ui.fragments.enabled = was;
    bool got = t->ready() && !t->truncated();
    if (got) {
        FILE* f = std::fopen(path.c_str(), "wb");
        got = f && std::fwrite(t->frame(), 1, t->frameBytes(), f) == t->frameBytes();
        if (f) std::fclose(f);
        if (!got) std::fprintf(stderr, "script:%d: cannot write %s\n", line, path.c_str());
    } else {
        std::fprintf(stderr, "script:%d: frame %s not recorded%s\n", line, path.c_str(),
                     t->truncated() ? " (cut)" : "");
    }
    t->release();
    return got;
}

bool Headless::saveFrame(const std::string& name, int line) {
    if (!m.app || !m.tap) return true;
    ot::Ui& ui = m.app->ui;
    bool ok = captureFrame(framesDir + "/" + name + ".otf", line);
    const uint32_t words = ui.lastDlWords();
    if (ok && ot::customFontsBound() && ot::customTextMode() == ot::kCustomGlyphs) {
        const uint32_t limit = ui.dlFrameLimit();
        ui.setDlFrameLimit(0);
        ot::setCustomTextMode(ot::kCustomText);
        ok = captureFrame(framesDir + "/" + name + ".text.otf", line);
        ot::setCustomTextMode(ot::kCustomGlyphs);
        ui.setDlFrameLimit(limit);
        // The display shows the last frame sent: leave it showing the
        // screen as drawn (a following fragcheck reads the front buffer).
        ui.render();
        advance(20);
    }
    if (ok) {
        if (FILE* ix = std::fopen((framesDir + "/index.txt").c_str(), "a")) {
            std::fprintf(ix, "%s %u\n", name.c_str(), unsigned(words));
            std::fclose(ix);
        }
    }
    seenFrames = ui.frames();
    return ok;
}

// fragcheck NAME: the current screen drawn inline and through cached
// fragments (probe, capture, append) must give identical pixels.
bool Headless::fragCheck(const std::string& name, int line) {
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
bool Headless::romCheck(const std::string& name, int line) {
    if (!m.app) return true;
    ot::Ui& ui = m.app->ui;
    if (ot::customFontsBound()) {
        std::fprintf(stderr, "script:%d: romcheck needs 'romfonts on'\n", line);
        return false;
    }
    std::vector<uint32_t> a(800 * 480), b(800 * 480);
    m.display.setFragments(false);
    // The CMD_TEXT reference is a pixel reference only (the device draws
    // ROM text as glyphs): at its RAM_DL cost a text-heavy screen passes
    // the guard, so lift it for this frame.
    const uint32_t limit = ui.dlFrameLimit();
    ui.setDlFrameLimit(0);
    ot::setRomTextMode(ot::kRomText);
    ui.render();
    ui.setDlFrameLimit(limit);
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

void Headless::advance(long ms) {
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

bool Headless::button(const std::string& name, bool down, int line) {
    const int id = buttonByName(name.c_str());
    if (id < 0) {
        std::fprintf(stderr, "script:%d: unknown button '%s'\n", line, name.c_str());
        return false;
    }
    m.input.button(uint8_t(id), down);
    return true;
}

bool Headless::run(const std::string& path) {
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
            // expect-open-ok checks. With no match here the embedder's
            // extension is asked (notices of its own are not in gNotices).
            std::string want = lineStr.substr(lineStr.find(a1));
            while (!want.empty() && (want.back() == ' ' || want.back() == '\r')) want.pop_back();
            const auto it = std::find_if(gNotices.begin(), gNotices.end(),
                                         [&](const std::string& n) { return n.find(want) != std::string::npos; });
            if (it != gNotices.end()) {
                gNotices.erase(it);
            } else if (!extension || extension(lineStr, line) <= 0) {
                std::fprintf(stderr, "script:%d: no notice \"%s\"\n", line, want.c_str());
                return false;
            }
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
            // An embedder's own commands (Headless::extension).
            const int r = extension ? extension(lineStr, line) : -1;
            if (r == 0) return false;
            if (r < 0) {
                std::fprintf(stderr, "script:%d: unknown command '%s'\n", line, cmd.c_str());
                return false;
            }
        }
    }
    return true;
}

}  // namespace sim
