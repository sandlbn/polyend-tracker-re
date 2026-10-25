// Fake HAL for driving the whole App on the host (ui_test, ui_snapshot_test).
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "app/app.h"
#include "hal_api.h"

// In-memory storage with real directories (listDir reports both files and
// folders; "" is the card root).
class FakeStorage : public ot::IStorage {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    std::set<std::string> dirs;
    // USB Storage mode (FakeUsbStorage): the computer owns the card; every
    // access is refused and counted (the app must not make any).
    bool locked = false;
    int lockedAccess = 0;
    bool refuse() {
        if (locked) ++lockedAccess;
        return locked;
    }
    // Like the device (IStorage::kMaxPath): a longer path fails, never cut.
    // Tests may fill files / dirs directly with longer paths (a computer can).
    uint32_t maxPath = ot::IStorage::kMaxPath;
    bool tooLong(const char* p) const { return maxPath && std::strlen(p) > maxPath; }

    static std::string parent(const std::string& p) {
        size_t s = p.rfind('/');
        return s == std::string::npos ? std::string() : p.substr(0, s);
    }
    bool parentOk(const std::string& p) const {
        std::string par = parent(p);
        return par.empty() || dirs.count(par);
    }
    Handle open(const char* path, OpenMode mode) override {
        if (refuse()) return -1;
        if (tooLong(path)) return -1;
        std::string p(path);
        auto it = files.find(p);
        if (mode == kRead && it == files.end()) return -1;
        if (mode != kRead && !parentOk(p)) return -1;
        if (mode == kWrite) files[p].clear();
        if (mode == kReadWrite && it == files.end()) files[p];
        Handle h = next_++;
        open_[h] = {p, 0};
        return h;
    }
    // Simulated SD latency (opt-in): every read advances this millisecond
    // clock by readLatencyMs, so background jobs span many main-loop passes.
    uint32_t* readLatencyClockMs = nullptr;
    uint32_t readLatencyMs = 1;
    int32_t read(Handle h, void* dst, uint32_t len) override {
        if (refuse()) return -1;
        if (readLatencyClockMs) *readLatencyClockMs += readLatencyMs;
        auto o = open_.find(h);
        if (o == open_.end()) return -1;
        auto& f = files[o->second.path];
        uint32_t avail = o->second.pos < f.size() ? uint32_t(f.size() - o->second.pos) : 0;
        uint32_t n = len < avail ? len : avail;
        if (n) std::memcpy(dst, f.data() + o->second.pos, n);
        o->second.pos += n;
        return int32_t(n);
    }
    int32_t write(Handle h, const void* src, uint32_t len) override {
        if (refuse()) return -1;
        auto o = open_.find(h);
        if (o == open_.end()) return -1;
        auto& f = files[o->second.path];
        if (f.size() < o->second.pos + len) f.resize(o->second.pos + len);
        if (len) std::memcpy(f.data() + o->second.pos, src, len);
        o->second.pos += len;
        return int32_t(len);
    }
    bool seek(Handle h, uint32_t pos) override {
        if (refuse()) return false;
        auto o = open_.find(h);
        if (o == open_.end()) return false;
        o->second.pos = pos;
        return true;
    }
    int32_t size(Handle h) override {
        if (refuse()) return -1;
        auto o = open_.find(h);
        return o == open_.end() ? -1 : int32_t(files[o->second.path].size());
    }
    void close(Handle h) override {
        refuse();
        open_.erase(h);
    }
    bool exists(const char* path) override {
        return !refuse() && !tooLong(path) && (files.count(path) || dirs.count(path));
    }
    bool mkdir(const char* path) override {
        if (refuse()) return false;
        if (tooLong(path)) return false;
        if (files.count(path)) return false;
        if (!parentOk(path)) return false;
        dirs.insert(path);
        return true;
    }
    bool remove(const char* path) override {
        if (refuse()) return false;
        if (tooLong(path)) return false;
        if (files.erase(path)) return true;
        std::string p(path);
        if (!dirs.count(p)) return false;
        std::string pre = p + "/";
        for (auto& f : files)
            if (f.first.compare(0, pre.size(), pre) == 0) return false;
        for (auto& d : dirs)
            if (d.compare(0, pre.size(), pre) == 0) return false;
        dirs.erase(p);
        return true;
    }
    bool rename(const char* from, const char* to) override {
        if (refuse()) return false;
        if (tooLong(from) || tooLong(to)) return false;
        auto it = files.find(from);
        if (it == files.end()) return false;
        files[to] = it->second;
        files.erase(it);
        return true;
    }
    int32_t listDir(const char* path, DirFn fn, void* ctx) override {
        if (refuse()) return -1;
        if (tooLong(path)) return -1;
        std::string p(path);
        if (!p.empty() && !dirs.count(p)) return -1;
        std::string pre = p.empty() ? "" : p + "/";
        std::vector<std::pair<std::string, std::pair<bool, uint32_t>>> ents;
        for (auto& d : dirs)
            if (d.compare(0, pre.size(), pre) == 0 && d.size() > pre.size() && d.find('/', pre.size()) == std::string::npos)
                ents.push_back({d.substr(pre.size()), {true, 0}});
        for (auto& f : files)
            if (f.first.compare(0, pre.size(), pre) == 0 && f.first.find('/', pre.size()) == std::string::npos)
                ents.push_back({f.first.substr(pre.size()), {false, uint32_t(f.second.size())}});
        int32_t n = 0;
        for (auto& e : ents) {
            DirEntry de{e.first.c_str(), e.second.first, e.second.second};
            ++n;
            if (!fn(de, ctx)) break;
        }
        return n;
    }

private:
    struct Open {
        std::string path;
        uint32_t pos;
    };
    std::map<Handle, Open> open_;
    Handle next_ = 1;
};

// USB Storage mode stand-in (IUsbStorage): begin() locks the fake card the
// way the device unmounts it, end() unlocks it; the host state is set by tests.
class FakeUsbStorage : public ot::IUsbStorage {
public:
    explicit FakeUsbStorage(FakeStorage* fs) : fs_(fs) {}
    bool failBegin = false;
    int begins = 0, ends = 0, services = 0;
    Host hostState = Host::kWaiting;
    bool begin() override {
        if (failBegin) return false;
        ++begins;
        fs_->locked = true;
        return true;
    }
    bool end() override {
        ++ends;
        fs_->locked = false;
        return true;
    }
    Host host() const override { return hostState; }
    void service() override { ++services; }
    bool active() const { return fs_->locked; }

private:
    FakeStorage* fs_;
};

// Records every display-list call of the last completed frame as text.
class RecordingDisplay : public ot::IDisplay {
public:
    std::vector<std::string> cur, last;
    int swaps = 0;
    // Device feedback a test can inject (IDisplay::frameFaultCount /
    // measuredDlBytes; 0 = unknown, like the host).
    uint32_t faultCount = 0, measuredBytes = 0;
    uint32_t frameFaultCount() const override { return faultCount; }
    uint32_t measuredDlBytes() const override { return measuredBytes; }
    uint16_t width() const override { return 800; }
    uint16_t height() const override { return 480; }
    void dlStart() override {
        if (busy.on) {
            // Ft81xDisplay::dlStart flushes (an SPI access: commits a lazy
            // tail) and blocks until the previous swap took effect.
            busyAccess();
            if (busy.pending) presentPending();
        }
        cur.clear();
    }
    void clearColorRGB(uint8_t r, uint8_t g, uint8_t b) override { add("CLEAR_COLOR %02X%02X%02X", r, g, b); }
    void clear(bool, bool, bool) override { add("CLEAR"); }
    void colorRGB(uint8_t r, uint8_t g, uint8_t b) override { add("COLOR %02X%02X%02X", r, g, b); }
    void colorA(uint8_t a) override { add("COLOR_A %u", a); }
    void lineWidth(uint16_t w) override { add("LINE_WIDTH %u", w); }
    void pointSize(uint16_t s) override { add("POINT_SIZE %u", s); }
    void scissorXY(uint16_t x, uint16_t y) override { add("SCISSOR_XY %u %u", x, y); }
    void scissorSize(uint16_t w, uint16_t h) override { add("SCISSOR_SIZE %u %u", w, h); }
    void begin(Prim p) override { add("BEGIN %d", int(p)); }
    void end() override { add("END"); }
    void vertex2f(int32_t x, int32_t y) override { add("VERTEX %d %d", int(x / 16), int(y / 16)); }
    void vertex2ii(uint16_t x, uint16_t y, uint8_t h, uint8_t c) override { add("VERTEX2II %u %u %u %u", x, y, h, c); }
    void text(int16_t x, int16_t y, uint8_t font, uint16_t opt, const char* s) override {
        add("TEXT %d %d f%u o%u \"%s\"", x, y, font, opt, s);
    }
    void number(int16_t x, int16_t y, uint8_t font, uint16_t opt, int32_t n) override {
        add("NUMBER %d %d f%u o%u %d", x, y, font, opt, int(n));
    }
    void swap() override {
        last = cur;
        ++swaps;
        if (busy.on) {
            busy.frame = cur;
            busy.pending = true;
            busy.committed = !busy.lazyCommit;
            busy.commitUs = nowUs() + uploadUs(cur);
        }
    }
    // Realistic busy panel (opt-in, busy.on; time from busy.clockMs): a
    // swapped frame is uploaded over SPI2 (DMA, ~16 B per recorded command
    // at 30 MHz), then shown at the next 54 Hz vsync (REG_DLSWAP clears);
    // frameReady() is false until then. `shown` is what the panel shows.
    // lazyCommit models the 2.0.0 driver: the frame's last DMA segment and
    // its REG_CMD_WRITE are only committed by the next display access, so
    // a frame nobody follows up on is never shown.
    struct Busy {
        bool on = false, lazyCommit = false;
        const uint32_t* clockMs = nullptr;
        bool pending = false, committed = false;
        uint64_t commitUs = 0;
        std::vector<std::string> frame;
        uint32_t polls = 0, shownFrames = 0;
    } busy;
    static constexpr uint64_t kVsyncUs = 18480;   // ft81xFrameMilliHz: 54.112 Hz
    std::vector<std::string> shown;
    bool frameReady() override {
        if (!busy.on) return true;
        ++busy.polls;
        busyAccess();
        busyTick();
        return !busy.pending;
    }
    const std::vector<std::string>& visible() {
        busyTick();
        return shown;
    }
    bool visibleHasText(const char* s) {
        for (auto& l : visible())
            if (l.compare(0, 5, "TEXT ") == 0 && l.find(s) != std::string::npos) return true;
        return false;
    }
    uint64_t nowUs() const { return busy.clockMs ? uint64_t(*busy.clockMs) * 1000u : 0; }
    static uint64_t uploadUs(const std::vector<std::string>& f) { return uint64_t(f.size()) * 16u * 8u / 30u; }
    void busyAccess() {
        if (busy.pending && !busy.committed) {
            busy.committed = true;
            busy.commitUs = nowUs() + uploadUs(busy.frame);
        }
    }
    void busyTick() {
        if (busy.pending && busy.committed && nowUs() >= (busy.commitUs / kVsyncUs + 1) * kVsyncUs) presentPending();
    }
    void presentPending() {
        shown = busy.frame;
        busy.pending = false;
        ++busy.shownFrames;
    }
    // Last IDisplay::setBacklight() level (-1 = never set) and the call count.
    int backlight = -1;
    int backlightCalls = 0;
    void setBacklight(uint8_t level) override {
        backlight = level;
        ++backlightCalls;
    }
    // Screenshot readback (off by default like the base IDisplay): pixel =
    // 0x00RRGGBB with R = x & 0xFF, G = y & 0xFF, B = 0x5A.
    bool readback = false;
    // Optional time cost per readPixels call (the device capture blocks for
    // seconds): advances *readbackClock by readbackMs.
    uint32_t* readbackClock = nullptr;
    uint32_t readbackMs = 0;
    bool readPixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t* rgb) override {
        if (!readback) return false;
        if (readbackClock) *readbackClock += readbackMs;
        for (uint16_t r = 0; r < h; ++r)
            for (uint16_t c = 0; c < w; ++c)
                rgb[size_t(r) * w + c] = (uint32_t((x + c) & 0xFF) << 16) | (uint32_t((y + r) & 0xFF) << 8) | 0x5Au;
        return true;
    }
    // Image path (spec 13 §G.6/§I.3): loadJpeg validates with the shared
    // IDisplay::probeJpeg (no decode) and logs "LOADJPEG <addr> <w>x<h> <path>"
    // (or "LOADJPEG <addr> FAIL <path>") to `loads`; set jpegSupport = false
    // to behave like a display without images. drawImage goes into the frame
    // as "IMAGE x y addr w h".
    bool jpegSupport = true;
    std::vector<std::string> loads;
    bool loadJpeg(uint32_t addr, ot::IStorage& fs, const char* path, uint16_t maxW, uint16_t maxH,
                  uint16_t& w, uint16_t& h) override {
        uint32_t size = 0;
        const bool ok = jpegSupport && probeJpeg(fs, path, addr, maxW, maxH, w, h, size);
        if (!ok) w = h = 0;
        char b[300];
        if (ok) snprintf(b, sizeof(b), "LOADJPEG %u %ux%u %s", unsigned(addr), unsigned(w), unsigned(h), path);
        else snprintf(b, sizeof(b), "LOADJPEG %u FAIL %s", unsigned(addr), path ? path : "");
        loads.push_back(b);
        return ok;
    }
    void drawImage(int16_t x, int16_t y, uint32_t addr, uint16_t w, uint16_t h) override {
        add("IMAGE %d %d %u %u %u", x, y, unsigned(addr), unsigned(w), unsigned(h));
    }
    // Games frames (spec 14 §3): a RAM_G model filled by writeRamG (bytes
    // counted in ramgBytes); drawRgb332 goes into the frame as
    // "RGB332 x y addr w h scale".
    std::vector<uint8_t> ramg = std::vector<uint8_t>(kRamGSize, 0);
    uint64_t ramgBytes = 0;
    bool writeRamG(uint32_t addr, const void* src, uint32_t len) override {
        if (addr >= kRamGSize || len > kRamGSize - addr) return false;
        std::memcpy(ramg.data() + addr, src, len);
        ramgBytes += len;
        return true;
    }
    void drawRgb332(int16_t x, int16_t y, uint32_t addr, uint16_t w, uint16_t h, uint8_t s) override {
        add("RGB332 %d %d %u %u %u %u", x, y, unsigned(addr), unsigned(w), unsigned(h), unsigned(s));
    }
    // Visual-parity primitives: frame lines "GRADIENT x0 y0 RRGGBB x1 y1
    // RRGGBB", "TRANSLATE_X <x16>", "SAVE_CONTEXT", "RESTORE_CONTEXT".
    // bindFontSet (outside a frame) is logged to `fontBinds` and returns
    // `customFonts` (default false = ROM fonts only, like the base IDisplay).
    bool customFonts = false;
    std::vector<int> fontBinds;
    bool bindFontSet(int set) override {
        fontBinds.push_back(set);
        return customFonts;
    }
    void gradient(int16_t x0, int16_t y0, uint32_t c0, int16_t x1, int16_t y1, uint32_t c1) override {
        add("GRADIENT %d %d %06X %d %d %06X", x0, y0, unsigned(c0 & 0xFFFFFFu), x1, y1, unsigned(c1 & 0xFFFFFFu));
    }
    void vertexTranslateX(int32_t x16) override { add("TRANSLATE_X %d", int(x16)); }
    void saveContext() override { add("SAVE_CONTEXT"); }
    void restoreContext() override { add("RESTORE_CONTEXT"); }
    bool lastHasText(const char* s) const {
        for (auto& l : last)
            if (l.compare(0, 5, "TEXT ") == 0 && l.find(s) != std::string::npos) return true;
        return false;
    }

private:
    void add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char b[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        cur.push_back(b);
    }
};

class ScriptInput : public ot::IInput {
public:
    std::deque<ot::InputEvent> q;
    bool poll(ot::InputEvent& ev) override {
        if (q.empty()) return false;
        ev = q.front();
        q.pop_front();
        return true;
    }
};

class FakeLeds : public ot::ILeds {
public:
    uint8_t level[48] = {};
    int shows = 0;
    uint8_t padCount() const override { return 48; }
    void setPad(uint8_t p, uint8_t r, uint8_t, uint8_t) override { if (p < 48) level[p] = r; }
    void clear() override { std::memset(level, 0, sizeof(level)); }
    void show() override { ++shows; }
};

class FakeClock : public ot::IClock {
public:
    uint32_t ms = 1000;
    uint32_t micros() const override { return ms * 1000; }
    uint32_t millis() const override { return ms; }
};

class FakeMidi : public ot::IMidiPort {
public:
    std::vector<uint8_t> sent;
    std::deque<uint8_t> in;
    size_t sentUsb = 0, sentDin = 0;   // bytes sent per port
    bool send(ot::MidiPortId p, const uint8_t* d, uint32_t n) override {
        sent.insert(sent.end(), d, d + n);
        (p == ot::MidiPortId::kUsb ? sentUsb : sentDin) += n;
        return true;
    }
    uint32_t receive(ot::MidiPortId p, uint8_t* dst, uint32_t cap) override {
        if (p != ot::MidiPortId::kUsb) return 0;
        uint32_t n = 0;
        while (n < cap && !in.empty()) {
            dst[n++] = in.front();
            in.pop_front();
        }
        return n;
    }
};

class FakeAudio : public ot::IAudioOut {
public:
    FillFn fn = nullptr;
    void* ctx = nullptr;
    bool started = false;
    void setCallback(FillFn f, void* c) override {
        fn = f;
        ctx = c;
    }
    bool start() override { return started = true; }
    void stop() override { started = false; }
    // Output mute line (pin 21): history of setOutputEnabled calls, 1 = on.
    std::vector<int> outputLog;
    bool outputOn() const { return !outputLog.empty() && outputLog.back() == 1; }
    void setOutputEnabled(bool on) override { outputLog.push_back(on ? 1 : 0); }
    // Last Master EQ / Bass Boost / Space push (spec 18 §1) and the call count.
    float dspEq[5] = {0, 0, 0, 0, 0};
    uint8_t dspBass = 0, dspSpace = 0;
    int dspCalls = 0;
    void applyMasterDsp(const float eq[5], uint8_t bass, uint8_t space) override {
        for (int b = 0; b < 5; ++b) dspEq[b] = eq[b];
        dspBass = bass;
        dspSpace = space;
        ++dspCalls;
    }
    // Run `blocks` audio callbacks; returns the peak absolute sample.
    int pump(int blocks) {
        int16_t buf[256];
        int peak = 0;
        for (int i = 0; i < blocks && fn; ++i) {
            fn(buf, 128, ctx);
            for (int k = 0; k < 256; ++k) peak = std::abs(buf[k]) > peak ? std::abs(buf[k]) : peak;
        }
        return peak;
    }
};

// Whole-app harness.
struct Harness {
    FakeStorage fs;
    FakeUsbStorage usb{&fs};
    RecordingDisplay disp;
    ScriptInput input;
    FakeLeds leds;
    FakeClock clock;
    FakeMidi midi;
    FakeAudio audio;
    std::vector<int16_t> bank, fx;
    std::vector<ot::PatternStock> pats;
    std::vector<ot::Pattern> undo, fundo, seqBufs;
    std::vector<ot::Track> extTracks;
    std::vector<uint8_t> extMeta;
    ot::Pattern clip;
    std::vector<uint8_t> nums, scratch;
    std::unique_ptr<ot::App> app;
    static std::string fwRequested;
    // Line input: when lineGen is set it fills each 128-frame block
    // (interleaved stereo) from the running block counter.
    std::function<void(int16_t* block, uint64_t blockIndex)> lineGen;
    int16_t lineBlock[256] = {};
    uint64_t lineBlocks = 0;
    int routedInput = -1;   // the last App::selectInput source (-1: none)

    explicit Harness(bool bootNow = true, uint32_t bankSamples = 4 * 1024 * 1024) {
        // ROM-font runs as whole CMD_TEXT strings (the checks read them);
        // display_budget_test / gfx_api_test turn the VERTEX2II path on.
        ot::setRomTextMode(ot::kRomWholeText);
        bank.resize(bankSamples);
        fx.resize(ot::audio::AudioEngine::kFxMemorySamples);
        pats.resize(ot::kPatternCount);
        seqBufs.resize(3);   // current, preload, switch stage
        extTracks.resize(ot::ExtTrackPool::kDefaultBlocks + 1);
        extMeta.resize(ot::ExtTrackPool::metaBytes(ot::ExtTrackPool::kDefaultBlocks));
        undo.resize(20);
        fundo.resize(10);
        nums.resize(64);
        scratch.resize(0x10000);
        app.reset(new ot::App());
        ot::HalRefs h;
        h.display = &disp;
        h.audio = &audio;
        h.storage = &fs;
        h.midi = &midi;
        h.input = &input;
        h.leds = &leds;
        h.clock = &clock;
        h.usbStorage = &usb;
        h.isUpdateImageName = [](const char* n) { return std::strlen(n) >= 15 && std::strstr(n, ".ptf") != nullptr; };
        h.requestFirmwareUpdate = [](ot::IStorage&, const char* n) {
            fwRequested = n;
            return true;
        };
        h.lineIn = [](void* c) -> const int16_t* {
            Harness* self = static_cast<Harness*>(c);
            if (!self->lineGen) return nullptr;
            self->lineGen(self->lineBlock, self->lineBlocks++);
            return self->lineBlock;
        };
        h.lineInCtx = this;
        h.selectInput = [](void* c, uint8_t source, uint8_t) {
            static_cast<Harness*>(c)->routedInput = source;
            return true;
        };
        h.inputCtx = this;
        ot::AppMemory m;
        m.sampleBank = bank.data();
        m.sampleBankSamples = uint32_t(bank.size());
        m.fxMemory = fx.data();
        m.fxSamples = uint32_t(fx.size());
        m.patterns = pats.data();
        m.seqBuffers = seqBufs.data();
        m.seqStage = seqBufs.data() + 2;
        m.extTracks = extTracks.data();
        m.extTrackBlocks = int(extTracks.size());
        m.extMeta = extMeta.data();
        m.undo = undo.data();
        m.undoNums = nums.data();
        m.undoSlots = 20;
        m.fileUndo = fundo.data();
        m.fileUndoNums = nums.data() + 32;
        m.fileUndoSlots = 10;
        m.clipboard = &clip;
        m.scratch = scratch.data();
        m.scratchBytes = uint32_t(scratch.size());
        app->init(h, m);
        // RAM_DL guard (Ui::render, docs/VERIFY_1.9.5.md §6): a frame that
        // did not fit the FT81x display list is a hard failure in every host
        // test (the device would have faulted); budget audits that expect it
        // clear dlOverflowFatal.
        app->ui.platform.dlOverflow = [](const char* screen, uint32_t words, uint32_t dropped, bool low) {
            std::fprintf(stderr, "DL OVERFLOW: %s frame cut at %u words (%u primitives dropped%s)\n", screen,
                         unsigned(words), unsigned(dropped), low ? ", low detail" : "");
            if (dlOverflowFatal) std::abort();
        };
        if (bootNow) app->boot();
    }
    static inline bool dlOverflowFatal = true;
    void run(uint32_t ms = 20) {
        // Advance time in 10 ms slices so timers fire in order.
        for (uint32_t t = 0; t < ms; t += 10) {
            clock.ms += 10;
            app->loop();
        }
        app->loop();
    }
    void push(ot::InputEvent::Type t, uint8_t id, int8_t d = 0) {
        ot::InputEvent e;
        e.type = t;
        e.id = id;
        e.delta = d;
        e.velocity = t == ot::InputEvent::kPadDown ? 127 : 0;
        e.timeMs = clock.ms;
        input.q.push_back(e);
        run(20);
    }
    void down(uint8_t id) { push(ot::InputEvent::kButtonDown, id); }
    void up(uint8_t id) { push(ot::InputEvent::kButtonUp, id); }
    void tap(uint8_t id) {
        down(id);
        up(id);
    }
    void pad(uint8_t p) {
        push(ot::InputEvent::kPadDown, p);
        push(ot::InputEvent::kPadUp, p);
    }
    void enc(int8_t d) { push(ot::InputEvent::kEncoder, 0, d); }
    void type(const char* s);   // on-screen keyboard: pads for letters/digits
};
inline std::string Harness::fwRequested;

// Keyboard pad for a lower-case letter/digit (layout in ui.cpp).
inline int keyPad(char c) {
    const char* rows[4] = {"1234567890", "qwertyuiop-+", "asdfghjkl@", "zxcvbnm"};
    for (int r = 0; r < 4; ++r) {
        const char* p = std::strchr(rows[r], c);
        if (p) return r * 12 + int(p - rows[r]);
    }
    return 47;   // space
}
inline void Harness::type(const char* s) {
    for (; *s; ++s) pad(uint8_t(keyPad(*s)));
}
