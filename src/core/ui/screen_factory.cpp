// Factory test mode (spec 05 §2.12). See screen_factory.h.
#include "ui/screen_factory.h"

#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "app/input_gain.h"
#include "ui/gfx.h"
#include "ui/screens.h"
#include "ui/ui.h"

namespace ot {

namespace {
const char* const kStepNames[FactoryTest::kStepCount] = {
    "Screen", "Inputs", "Pad backlight", "RAM", "MIDI loopback",
    "Radio", "Battery", "Audio loopback", "USB / SD", "Finished",
};
const char* const kResultNames[4] = {"...", "PASS", "FAIL", "SKIP"};

constexpr uint32_t kAutoAdvanceMs = 800;      // auto steps: result shown this long
constexpr uint32_t kRamChunkWords = 256u * 1024u;
constexpr uint32_t kAudioTimeoutMs = 3000;    // per recording
constexpr const char* kSdTestPath = "factory_test.tmp";

inline uint16_t ramPattern(uint32_t i, int pass) {
    // Address-dependent pattern, different every pass; odd passes inverted.
    const uint32_t v = (i * 2654435761u) ^ (uint32_t(pass) * 0x9E3779B9u);
    const uint16_t w = uint16_t(v >> 16);
    return (pass & 1) ? uint16_t(~w) : w;
}
}  // namespace

const char* FactoryTest::stepName(int s) { return s >= 0 && s < kStepCount ? kStepNames[s] : ""; }

void FactoryTest::start(uint32_t now) {
    active_ = true;
    now_ = now;
    for (auto& r : res_) r = kPending;
    enter(kScreen);
    ui_.dirty();
}

void FactoryTest::exit() {
    active_ = false;
    // spec 05 §2.12: leaving the factory test returns to Config.
    ui_.closeFactoryTest();
}

void FactoryTest::enter(int s) {
    step_ = s;
    stepAt_ = now_;
    done_ = false;
    doneAt_ = 0;
    detail_[0] = 0;
    switch (s) {
        case kInputs:
            btnSeen_ = padSeen_ = 0;
            encUp_ = encDown_ = false;
            shiftHeld_ = false;
            break;
        case kRam: {
            // The free tail of the sample pool: the project's samples are
            // never touched. Skipped while a background job may move samples.
            ram_ = nullptr;
            ramWords_ = 0;
            ramPos_ = 0;
            ramPass_ = 0;
            ramVerify_ = false;
            ramErr_ = 0;
            if (!app_.jobBusy()) {
                ram_ = reinterpret_cast<uint16_t*>(app_.bank.tail());
                ramWords_ = ram_ ? uint32_t(app_.bank.freeSamples()) : 0;
            }
            if (ramWords_ < 1024) {
                std::snprintf(detail_, sizeof(detail_), "no free sample memory to test");
                finish(kSkip);
            }
            break;
        }
        case kMidi: runMidi(); break;
        case kRadio: runRadio(); break;
        case kBattery:
            // No fuel-gauge readout in OpenTracker (README).
            std::snprintf(detail_, sizeof(detail_), "no battery readout");
            finish(kSkip);
            break;
        case kAudio:
            audioPhase_ = -1;
            audioOk_[0] = audioOk_[1] = false;
            savedBuf_ = app_.capture.buffer();
            savedCap_ = app_.capture.bufferSamples();
            if (app_.capture.active() || !app_.bank.tail() || app_.bank.freeSamples() < kRecordFrames) {
                std::snprintf(detail_, sizeof(detail_), "no free sample memory to record");
                finish(kSkip);
            }
            break;
        case kUsbSd: runSd(); break;
        case kFinished: res_[kFinished] = kPass; break;
        default: break;
    }
}

void FactoryTest::finish(Result r) {
    res_[step_] = r;
    done_ = true;
    doneAt_ = now_ ? now_ : 1;
    ui_.dirty();
}

void FactoryTest::next() {
    if (step_ + 1 < kStepCount) enter(step_ + 1);
    ui_.dirty();
}

// MIDI loopback: a note-on sent on the DIN out must come back on the DIN in
// (loop cable). App::serviceMidi drains the ports every pass, so the reply is
// polled right here, with the HAL serviced, for at most ~30 ms.
void FactoryTest::runMidi() {
    IMidiPort* m = app_.hal().midi;
    if (!m) {
        std::snprintf(detail_, sizeof(detail_), "no MIDI ports");
        finish(kSkip);
        return;
    }
    uint8_t drain[32];
    while (m->receive(MidiPortId::kDin, drain, sizeof(drain)) > 0) {
    }
    const uint8_t msg[3] = {0x90, 0x3C, 0x64};
    m->send(MidiPortId::kDin, msg, 3);
    uint8_t got[3] = {};
    int n = 0;
    const uint32_t t0 = app_.millis();
    for (uint32_t it = 0; it < 100000 && n < 3 && app_.millis() - t0 < 30; ++it) {
        if (app_.hal().service) app_.hal().service();
        uint8_t b;
        while (n < 3 && m->receive(MidiPortId::kDin, &b, 1) == 1) {
            if (b >= 0xF8) continue;   // real-time bytes
            got[n++] = b;
        }
    }
    const uint8_t off[3] = {0x80, 0x3C, 0x00};
    m->send(MidiPortId::kDin, off, 3);
    const bool ok = n == 3 && std::memcmp(got, msg, 3) == 0;
    std::snprintf(detail_, sizeof(detail_), ok ? "DIN out -> DIN in OK" : "no reply on DIN in (loop cable?)");
    finish(ok ? kPass : kFail);
}

// Radio: the tuner must accept a frequency inside every region's band.
void FactoryTest::runRadio() {
    IRadio* r = app_.hal().radio;
    if (!r) {
        std::snprintf(detail_, sizeof(detail_), "no tuner driver");
        finish(kSkip);
        return;
    }
    const bool ok = r->tune(90.0f);
    std::snprintf(detail_, sizeof(detail_), ok ? "tuned 90.0 MHz" : "tuner did not respond");
    if (app_.cfg.radioFreq10) r->tune(float(app_.cfg.radioFreq10) / 10.0f);
    finish(ok ? kPass : kFail);
}

// SD: a 16 KB file written, read back, compared and removed. The USB link
// has no check in OpenTracker.
void FactoryTest::runSd() {
    IStorage& fs = app_.fs();
    // [I] The staging block borrows the browsers' shared list buffer
    // (screens.h): no browser is open on this screen, and each rebuilds its
    // list when it opens.
    constexpr uint32_t kBlock = 4096;
    static_assert(kBlock <= kBrowserListBytes, "the SD test block fits the list buffer");
    uint8_t* const buf = static_cast<uint8_t*>(browserListMem());
    bool ok = true;
    IStorage::Handle h = fs.open(kSdTestPath, IStorage::kWrite);
    if (h < 0) ok = false;
    for (uint32_t blk = 0; ok && blk < 4; ++blk) {
        for (uint32_t i = 0; i < kBlock; ++i) buf[i] = uint8_t((i * 7u) ^ (blk * 31u) ^ (i >> 8));
        ok = fs.write(h, buf, kBlock) == int32_t(kBlock);
    }
    if (h >= 0) fs.close(h);
    if (ok) {
        h = fs.open(kSdTestPath, IStorage::kRead);
        ok = h >= 0;
        for (uint32_t blk = 0; ok && blk < 4; ++blk) {
            ok = fs.read(h, buf, kBlock) == int32_t(kBlock);
            for (uint32_t i = 0; ok && i < kBlock; ++i)
                ok = buf[i] == uint8_t((i * 7u) ^ (blk * 31u) ^ (i >> 8));
        }
        if (h >= 0) fs.close(h);
    }
    fs.remove(kSdTestPath);
    std::snprintf(detail_, sizeof(detail_), ok ? "SD write/read 16 KB OK (USB not checked)" : "SD write/read failed");
    finish(ok ? kPass : kFail);
}

// RAM: kRamPasses passes; each writes the whole region, then verifies it
// (so the reads do not come from the cache), a chunk per update.
void FactoryTest::ramStep() {
    const uint32_t end = ramPos_ + kRamChunkWords < ramWords_ ? ramPos_ + kRamChunkWords : ramWords_;
    if (!ramVerify_) {
        for (uint32_t i = ramPos_; i < end; ++i) ram_[i] = ramPattern(i, ramPass_);
    } else {
        for (uint32_t i = ramPos_; i < end; ++i)
            if (ram_[i] != ramPattern(i, ramPass_)) ++ramErr_;
    }
    ramPos_ = end;
    if (ramPos_ < ramWords_) return;
    ramPos_ = 0;
    if (!ramVerify_) {
        ramVerify_ = true;
        return;
    }
    ramVerify_ = false;
    ++ramPass_;
    ui_.dirty();
    if (ramPass_ < kRamPasses) return;
    std::snprintf(detail_, sizeof(detail_), "%lu KB x %d passes, %lu errors",
                  static_cast<unsigned long>(ramWords_ / 512u), kRamPasses, static_cast<unsigned long>(ramErr_));
    finish(ramErr_ ? kFail : kPass);
}

// Audio: 1 s from the line input -> line_in_test.wav, then 1 s from the
// microphone -> mic_test.wav, recorded into the free sample memory through
// the recorder's capture tap. Pass = both files written with a signal.
void FactoryTest::audioStep(uint32_t now) {
    App& a = app_;
    Capture& c = a.capture;
    static const char* const kFiles[2] = {"line_in_test.wav", "mic_test.wav"};
    static const uint8_t kSource[2] = {0, 2};   // line in, mic low gain
    if (audioPhase_ < 0 || (audioPhase_ < 2 && !c.active())) {
        if (audioPhase_ >= 0) {
            // Previous recording finished: write it.
            const uint32_t n = c.length();
            const bool wrote = n > 0 && a.writeWav(kFiles[audioPhase_], c.buffer(), n);
            bool signal = false;
            for (uint32_t i = 0; i < n && !signal; ++i) signal = c.buffer()[i] != 0;
            audioOk_[audioPhase_] = wrote && signal;
        }
        ++audioPhase_;
        if (audioPhase_ < 2) {
            // spec 01 §5.5 factory levels: line in at level 15, mic 20 dB.
            const bool routed = a.selectInput(kSource[audioPhase_], kSource[audioPhase_] == 0
                                                                        ? input_gain::kFactoryLineGain
                                                                        : input_gain::kFactoryMicGain);
            if (!routed) {
                audioOk_[audioPhase_] = false;
                audioAt_ = now;
                return;   // next update: not active -> skipped to the next file
            }
            c.setBuffer(a.bank.tail(), kRecordFrames);
            c.start(0, 0.0f, 1);
            audioAt_ = now;
        }
        ui_.dirty();
    } else if (audioPhase_ < 2 && c.active() && (c.full() || now - audioAt_ >= kAudioTimeoutMs)) {
        c.stop();
    }
    if (audioPhase_ < 2) return;
    // Restore the recorder routing and buffer.
    c.setBuffer(savedBuf_, savedCap_);
    a.selectInput(a.cfg.recSource, a.cfg.recGain[a.cfg.recSource > 3 ? 0 : a.cfg.recSource]);
    std::snprintf(detail_, sizeof(detail_), "line in %s, mic %s", audioOk_[0] ? "OK" : "FAIL",
                  audioOk_[1] ? "OK" : "FAIL");
    finish(audioOk_[0] && audioOk_[1] ? kPass : kFail);
}

void FactoryTest::update(uint32_t now) {
    if (!active_) return;
    now_ = now;
    if (step_ == kInputs && !done_) {
        if (shiftHeld_ && now - shiftAt_ >= kShiftSkipMs) {
            shiftHeld_ = false;
            std::snprintf(detail_, sizeof(detail_), "skipped (Shift held)");
            finish(kSkip);
        }
    }
    if (step_ == kRam && !done_) ramStep();
    if (step_ == kAudio && !done_) audioStep(now);
    if (step_ == kPadLeds) ui_.dirty();   // LED animation
    // Automatic steps advance on their own once the result was shown.
    const bool automatic = step_ == kInputs || step_ == kRam || step_ == kMidi || step_ == kRadio ||
                           step_ == kBattery || step_ == kAudio || step_ == kUsbSd;
    if (automatic && done_ && now - doneAt_ >= kAutoAdvanceMs) next();
}

int FactoryTest::inputsLeft() const {
    int n = (encUp_ ? 0 : 1) + (encDown_ ? 0 : 1);
    for (int i = 0; i < kButtons; ++i) n += ((btnSeen_ >> i) & 1) ? 0 : 1;
    for (int p = 0; p < 48; ++p) n += ((padSeen_ >> p) & 1) ? 0 : 1;
    return n;
}

void FactoryTest::onButton(uint8_t id, uint8_t state) {
    if (!active_) return;
    if (step_ == kInputs && !done_) {
        if (id == btn::Shift) {
            shiftHeld_ = state != kRelease;
            if (state == kPress) shiftAt_ = now_;
        }
        if (state == kPress && id < kButtons) btnSeen_ |= 1ull << id;
        if (!inputsLeft()) {
            std::snprintf(detail_, sizeof(detail_), "all buttons, pads and the encoder OK");
            finish(kPass);
        }
        ui_.dirty();
        return;
    }
    if (state != kPress) return;
    switch (id) {
        case btn::F1:   // abort
            exit();
            return;
        case btn::F7:
            if (step_ == kScreen || step_ == kPadLeds) {
                finish(kFail);
                next();
            }
            break;
        case btn::F8:
        case btn::EncPush:
            if (step_ == kFinished) {
                exit();
                return;
            }
            if (step_ == kScreen || step_ == kPadLeds) finish(kPass);
            if (done_) next();
            break;
        default: break;
    }
    ui_.dirty();
}

void FactoryTest::onPad(uint8_t pad, uint8_t state) {
    if (!active_ || state != kPress || pad >= 48) return;
    if (step_ == kInputs && !done_) {
        padSeen_ |= 1ull << pad;
        if (!inputsLeft()) {
            std::snprintf(detail_, sizeof(detail_), "all buttons, pads and the encoder OK");
            finish(kPass);
        }
        ui_.dirty();
    }
}

void FactoryTest::onEncoder(int d) {
    if (!active_ || step_ != kInputs || done_) return;
    if (d > 0) encUp_ = true;
    if (d < 0) encDown_ = true;
    if (!inputsLeft()) {
        std::snprintf(detail_, sizeof(detail_), "all buttons, pads and the encoder OK");
        finish(kPass);
    }
    ui_.dirty();
}

void FactoryTest::padLeds(uint8_t* lv) const {
    if (!active_) return;
    if (step_ == kInputs) {
        for (int p = 0; p < 48; ++p) lv[p] = ((padSeen_ >> p) & 1) ? 0 : 8;
    } else if (step_ == kPadLeds) {
        // Chaser over the 48 pads (50 ms each), then all on / off at 2 Hz.
        const uint32_t t = now_ - stepAt_;
        const uint32_t chase = t / 50;
        for (int p = 0; p < 48; ++p) {
            if (chase < 48) lv[p] = uint32_t(p) == chase ? 31 : 2;
            else lv[p] = ((t / 500) & 1) ? 0 : 31;
        }
    }
}

void FactoryTest::draw(Gfx& g) const {
    if (!active_) return;
    const Theme& th = ui_.th();
    // [I] no stock shot: the stock title bar (bold title, no status group)
    // and a results panel at x 601 in the column grid (spec 20 §1.2, §4).
    g.fill(0, 0, kScreenW, kScreenH, th.bg);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "Factory test %d/%d", step_ + 1, int(kStepCount));
    wg::titleBar(g, th, buf, stepName(step_), nullptr);
    wg::titleHairline(g, th);
    wg::panel(g, th, 601, wg::kPanelY, 197, wg::kPanelH);

    // Results column.
    for (int s = 0; s < kStepCount - 1; ++s) {
        const int y = 46 + s * 26;
        g.text(610, y, kFont1, s == step_ ? th.yellow : th.text, stepName(s), IDisplay::kOptCenterY);
        const uint32_t c = res_[s] == kPass ? th.good : res_[s] == kFail ? th.alert : th.dim;
        g.text(790, y, kFont1, c, kResultNames[res_[s]], IDisplay::kOptCenterY | IDisplay::kOptRightX);
    }

    const char* hint = "";
    switch (step_) {
        case kScreen: {
            // Colour bars + grey ramp.
            static const uint32_t kBars[8] = {0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
                                              0xFF00FF, 0xFF0000, 0x0000FF, 0x000000};
            for (int i = 0; i < 8; ++i) g.fill(10 + i * 72, 40, 72, 150, kBars[i]);
            for (int i = 0; i < 32; ++i) {
                const uint32_t v = uint32_t(i * 255 / 31);
                g.fill(10 + i * 18, 195, 18, 40, (v << 16) | (v << 8) | v);
            }
            int y = 250;
            FactoryCheck checks[12];
            const int n = ui_.platform.bootChecks ? ui_.platform.bootChecks(checks, 12) : 0;
            for (int i = 0; i < n; ++i, y += 20) {
                std::snprintf(buf, sizeof(buf), "%s: %s", checks[i].name, checks[i].ok ? "OK" : "FAIL");
                g.text(14, y, kFont1, checks[i].ok ? th.good : th.alert, buf, IDisplay::kOptCenterY);
            }
            hint = "Check the colours.  F8 Pass   F7 Fail   F1 Exit";
            break;
        }
        case kInputs: {
            for (int id = 0; id < kButtons; ++id) {
                const int x = 14 + (id % 11) * 52, y = 44 + (id / 11) * 40;
                const bool seen = (btnSeen_ >> id) & 1;
                g.fill(x, y, 48, 34, seen ? th.good : th.panel);
                std::snprintf(buf, sizeof(buf), "%d", id);
                g.text(x + 24, y + 17, kFont1, th.text, buf, IDisplay::kOptCenter);
            }
            for (int p = 0; p < 48; ++p) {
                const int x = 14 + (p % 12) * 48, y = 176 + (p / 12) * 40;
                g.fill(x, y, 44, 34, ((padSeen_ >> p) & 1) ? th.good : th.panelDark);
                std::snprintf(buf, sizeof(buf), "%d", p);
                g.text(x + 22, y + 17, kFont1, th.text, buf, IDisplay::kOptCenter);
            }
            std::snprintf(buf, sizeof(buf), "Encoder:  left %s   right %s   (%d inputs left)", encDown_ ? "OK" : "-",
                          encUp_ ? "OK" : "-", inputsLeft());
            g.text(14, 350, kFont1, th.text, buf, IDisplay::kOptCenterY);
            hint = "Press every button and pad, turn the encoder both ways. Hold Shift to skip.";
            break;
        }
        case kPadLeds: hint = "Watch the pad lights.  F8 Pass   F7 Fail   F1 Exit"; break;
        case kRam:
            std::snprintf(buf, sizeof(buf), "Pass %d / %d   errors %lu", ramPass_ + (done_ ? 0 : 1), kRamPasses,
                          static_cast<unsigned long>(ramErr_));
            g.text(14, 120, kFont4, th.text, buf, IDisplay::kOptCenterY);
            g.hbar(14, 150, 560, 24, float(ramPass_) / float(kRamPasses), th.good, th.dim);
            hint = "Testing the free sample memory...";
            break;
        case kAudio:
            hint = audioPhase_ <= 0 ? "Recording the line input..." : "Recording the microphone...";
            break;
        case kFinished: {
            int fails = 0;
            for (int s = 0; s < kFinished; ++s) fails += res_[s] == kFail ? 1 : 0;
            std::snprintf(buf, sizeof(buf), fails ? "%d step(s) FAILED" : "All steps passed or skipped", fails);
            g.text(290, 200, kFontTitle, fails ? th.alert : th.good, buf, IDisplay::kOptCenter);
            hint = "F8 Exit";
            break;
        }
        default: break;
    }
    if (detail_[0]) g.text(14, 390, kFont1, th.yellow, detail_, IDisplay::kOptCenterY);
    g.text(14, 455, kFont3, th.dim, hint, IDisplay::kOptCenterY);
}

}  // namespace ot
