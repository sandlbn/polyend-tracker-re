// OpenTracker -- hardware abstraction interfaces.
//
// Everything in src/core talks to the hardware only through these interfaces
// (ARCHITECTURE.md "Rules"). src/hal implements them for the MK66 board; the
// host tests implement them with in-memory fakes.
//
// Conventions:
//  - No exceptions, no RTTI, no heap: callbacks are plain function pointers
//    plus a user context pointer.
//  - Return values: negative = error, >= 0 = success / byte count.
//  - Interfaces are deliberately minimal; extend them (append methods) as
//    subsystems need more, and keep the host fakes in tests/ in sync.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ot {

class IStorage;

// ---------------------------------------------------------------------------
// IDisplay -- FT81x-style display list / coprocessor command builder.
//
// Mirrors the public FT81x programming model (Bridgetek FT81x datasheet and
// programmer guide): a frame is built as a command list between dlStart()
// and swap(). Coordinates passed to vertex2f() are in 1/16 pixel units, like
// the FT81x VERTEX2F command; helper methods take whole pixels.
// ---------------------------------------------------------------------------
class IDisplay {
public:
    // Graphics primitives for begin(); values equal the FT81x BEGIN() codes.
    enum Prim : uint8_t {
        kBitmaps = 1,
        kPoints = 2,
        kLines = 3,
        kLineStrip = 4,
        kEdgeStripR = 5,
        kEdgeStripL = 6,
        kEdgeStripA = 7,
        kEdgeStripB = 8,
        kRects = 9,
    };
    // Option flags for text()/number(); values equal the FT81x OPT_* codes.
    enum Opt : uint16_t {
        kOptNone = 0,
        kOptSigned = 256,
        kOptCenterX = 512,
        kOptCenterY = 1024,
        kOptCenter = 1536,
        kOptRightX = 2048,
    };

    virtual ~IDisplay() = default;

    virtual uint16_t width() const = 0;
    virtual uint16_t height() const = 0;

    // --- display list -----------------------------------------------------
    virtual void dlStart() = 0;                                  // CMD_DLSTART
    virtual void clearColorRGB(uint8_t r, uint8_t g, uint8_t b) = 0;
    virtual void clear(bool color = true, bool stencil = true, bool tag = true) = 0;
    virtual void colorRGB(uint8_t r, uint8_t g, uint8_t b) = 0;
    virtual void colorA(uint8_t a) = 0;
    virtual void lineWidth(uint16_t width16) = 0;                // 1/16 px
    virtual void pointSize(uint16_t size16) = 0;                 // 1/16 px
    virtual void scissorXY(uint16_t x, uint16_t y) = 0;
    virtual void scissorSize(uint16_t w, uint16_t h) = 0;
    virtual void begin(Prim prim) = 0;
    virtual void end() = 0;
    virtual void vertex2f(int32_t x16, int32_t y16) = 0;         // 1/16 px
    virtual void vertex2ii(uint16_t x, uint16_t y, uint8_t handle = 0, uint8_t cell = 0) = 0;

    // --- coprocessor widgets ---------------------------------------------
    virtual void text(int16_t x, int16_t y, uint8_t font, uint16_t options, const char* s) = 0;
    virtual void number(int16_t x, int16_t y, uint8_t font, uint16_t options, int32_t n) = 0;

    // Finish the list (DISPLAY) and swap it in (CMD_SWAP). On return the
    // whole frame is submitted: it reaches the screen at the next vsync
    // without any further IDisplay call (the UI may not draw again for
    // seconds; tests/display_busy_test.cpp checks this contract).
    virtual void swap() = 0;

    // Read back a rectangle of the frame currently shown, as 0x00RRGGBB
    // words, row-major (screenshot, spec 05 §3.4: CMD_SNAPSHOT2 on the
    // FT81x). Returns false when the display cannot read back.
    // TODO(hal): implement for the FT81x driver (CMD_SNAPSHOT2 into RAM_G,
    // then read RAM_G over SPI); the simulator implements it. The device HAL
    // must also deliver the power switch as button btn::Power (0x2a) so
    // Shift + Power reaches Ui (spec 05 §2.1.1).
    virtual bool readPixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t* rgb) {
        (void)x; (void)y; (void)w; (void)h; (void)rgb;
        return false;
    }

    // Backlight PWM duty on the FT81x REG_PWM_DUTY scale, 0 (off) .. 128
    // (100 %); values above 128 clamp (spec 05 §1.8 HAL.setBacklight). The
    // FT81x driver turns the backlight on at the first draw (spec 01 §4.2);
    // a level set before that is the level used then. Default: no backlight
    // control (host fakes, displays without PWM).
    virtual void setBacklight(uint8_t level) { (void)level; }

    // Config "Display brightness" (0 Low, 1 Medium, 2 High; > 2 clamps to
    // High) to REG_PWM_DUTY: 20 / 50 / 128 (spec 05 §1.8, §3.1 notes D).
    static constexpr uint8_t brightnessDuty(uint8_t configLevel) {
        return configLevel == 0 ? 20 : (configLevel == 1 ? 50 : 128);
    }

    // --- images: JPEG decoded by the FT81x coprocessor into RAM_G ----------
    // spec 13 §G.6 / §I.3. The device streams the file through the
    // coprocessor FIFO with CMD_LOADIMAGE(dest, OPT_NODL); the decoded bitmap
    // is RGB565 (2 B/px, stride = 2 * w) at `ramgAddr`. Only baseline JPEG
    // (SOF0/SOF1) is accepted: CMD_LOADIMAGE does not decode progressive files.
    static constexpr uint32_t kRamGSize = 0x100000;          // FT81x RAM_G, 1 MiB
    static constexpr uint32_t kRamGImageLimit = 1000000;     // spec 13 §G.6: file <= limit - dest
    static constexpr uint32_t kRamGCover = 670000;           // spec 13 §I.3 project cover (0xA3930)
    static constexpr uint16_t kCoverMaxSide = 400;           // spec 13 §I.3: <= 400 x 400 px

    // Validates `path` without decoding: the file starts with SOI, its first
    // SOFn segment is baseline (SOF0/SOF1) with 0 < w <= maxW, 0 < h <= maxH,
    // the file is <= kRamGImageLimit - ramgAddr bytes (spec 13 §G.6) and the
    // RGB565 result fits RAM_G. ramgAddr must be 4-byte aligned. Defined below
    // IStorage. Shared by every loadJpeg implementation.
    static bool probeJpeg(IStorage& fs, const char* path, uint32_t ramgAddr, uint16_t maxW, uint16_t maxH,
                          uint16_t& w, uint16_t& h, uint32_t& fileSize);

    // Decode the JPEG file `path` into RAM_G at ramgAddr (probeJpeg rules) and
    // report its size. Blocking, main loop only, and never between dlStart()
    // and swap(). On failure RAM_G at ramgAddr is undefined and false is
    // returned. Default: no image support.
    virtual bool loadJpeg(uint32_t ramgAddr, IStorage& fs, const char* path, uint16_t maxW, uint16_t maxH,
                          uint16_t& w, uint16_t& h) {
        (void)ramgAddr; (void)fs; (void)path; (void)maxW; (void)maxH;
        w = h = 0;
        return false;
    }
    // spec 13 §G.6 loader naming: `base` + ".jpg", else `base` + ".jpeg"
    // (e.g. "<project>/cover"). Defined below IStorage.
    bool loadJpegBase(uint32_t ramgAddr, IStorage& fs, const char* base, uint16_t maxW, uint16_t maxH,
                      uint16_t& w, uint16_t& h);

    // Display-list draw of a w x h RGB565 bitmap at RAM_G ramgAddr with its
    // top-left at pixel (x, y) (BITMAP_SOURCE/LAYOUT/SIZE on a private bitmap
    // handle, BEGIN(BITMAPS), VERTEX2F). Like any FT81x bitmap it is
    // modulated by the current COLOR_RGB/COLOR_A and clipped by the scissor:
    // set colorRGB(255,255,255) for the image as decoded. Default: no-op.
    virtual void drawImage(int16_t x, int16_t y, uint32_t ramgAddr, uint16_t w, uint16_t h) {
        (void)x; (void)y; (void)ramgAddr; (void)w; (void)h;
    }

    // --- raw RAM_G frames: Games (spec 14 §3) -----------------------------
    // The NES frame (256 x 240 RGB332, 61,440 B) lives in the free RAM_G
    // band between the screenshot band and the cover image (stock uses
    // 840,000, which overlaps the cover; spec 14 §5.3).
    static constexpr uint32_t kRamGGameFrame = 384000;
    static constexpr uint32_t kGameFrameBytes = 256u * 240u;
    // Copy `len` bytes from `src` into RAM_G at ramgAddr (FT81x memory write).
    // Main loop only, never between dlStart() and swap(). False when the
    // display has no RAM_G (default) or the range leaves RAM_G.
    virtual bool writeRamG(uint32_t ramgAddr, const void* src, uint32_t len) {
        (void)ramgAddr; (void)src; (void)len;
        return false;
    }
    // Display-list draw of a w x h RGB332 bitmap (FT81x format 4, stride w)
    // at RAM_G ramgAddr, magnified `scale` times with nearest filtering
    // (BITMAP_TRANSFORM_A/E = 256 / scale), top-left at pixel (x, y). The
    // graphics context is saved/restored around it. Modulated by COLOR_RGB /
    // COLOR_A like any bitmap. Default: no-op.
    virtual void drawRgb332(int16_t x, int16_t y, uint32_t ramgAddr, uint16_t w, uint16_t h, uint8_t scale) {
        (void)x; (void)y; (void)ramgAddr; (void)w; (void)h; (void)scale;
    }

    // --- frame pacing (2.x non-blocking swap) ------------------------------
    // True when a new frame can start without waiting: the coprocessor has
    // consumed the command FIFO and the previous CMD_SWAP has taken effect
    // (REG_DLSWAP == 0 on the FT81x). The UI skips a frame instead of
    // spinning when this is false. Default: always ready.
    virtual bool frameReady() { return true; }
    // Device feedback (FT81x): frames the coprocessor faulted on after they
    // were sent (a running count), and the RAM_DL bytes the last completed
    // frame really used. 0 where unknown (host displays).
    virtual uint32_t frameFaultCount() const { return 0; }
    virtual uint32_t measuredDlBytes() const { return 0; }

    // --- cached display-list fragments (spec 05 §3.3 stock scheme) --------
    // The stock firmware keeps each control's display list in RAM_G and
    // composes frames with CMD_APPEND. Here the UI (Gfx::cached) captures a
    // run of commands that it draws inline anyway: fragmentBegin(slot) ...
    // fragmentEnd(slot) stores the display-list words those commands produce
    // in the slot (FT81x: REG_CMD_DL before/after, then CMD_MEMCPY from RAM_DL
    // into RAM_G); fragmentAppend(slot) later re-emits them (CMD_APPEND). Only
    // between dlStart() and swap(); a fragment inherits the graphics state at
    // the append point like CMD_APPEND does. Default: unsupported.
    // 16 slots: the FT81x keeps them in RAM_G after the 1.9.5 font image.
    static constexpr uint8_t kFragmentSlots = 16;
    virtual bool fragmentsSupported() const { return false; }
    virtual void fragmentBegin(uint8_t slot) { (void)slot; }
    // False when the fragment could not be stored (too large, fault); the
    // slot is then empty and must not be appended.
    virtual bool fragmentEnd(uint8_t slot) { (void)slot; return false; }
    virtual void fragmentAppend(uint8_t slot) { (void)slot; }
    // Display-list words held by a stored fragment (0 = empty slot).
    virtual uint32_t fragmentWords(uint8_t slot) const { (void)slot; return 0; }

    // --- visual-parity primitives (spec 20 §1.5, §2.1) ---------------------
    // Bitmap handles the custom UI fonts are bound to by bindFontSet(): the
    // five font roles of spec 20 §2.1 on kFontHandleBase + 0..4 and the icon
    // atlas on kIconHandle (handles 1 and 2 are the image / Games handles of
    // the FT81x driver; 15 is the coprocessor scratch handle).
    static constexpr uint8_t kFontHandleBase = 3;
    static constexpr uint8_t kFontHandleCount = 5;
    static constexpr uint8_t kIconHandle = 8;

    // Bind custom font set `set` (0 Original, 1 New; spec 20 §2.1) to
    // kFontHandleBase.. (and the icon atlas to kIconHandle), outside
    // dlStart()..swap(). Returns true when the display has custom fonts and
    // they are now bound; false (default) = ROM fonts only, the UI then
    // falls back to the ROM font roles.
    virtual bool bindFontSet(int set) {
        (void)set;
        return false;
    }

    // CMD_GRADIENT: fills the current scissor area with a linear gradient
    // from colour rgb0 (0x00RRGGBB) at pixel (x0, y0) to rgb1 at (x1, y1),
    // clamped beyond the end points. Leaves COLOR_RGB/COLOR_A unchanged on
    // the FT81x (the coprocessor saves/restores the context). Default
    // fallback: a solid fill in the mid colour over the scissor area (it
    // does change COLOR_RGB; callers re-set their colour afterwards).
    virtual void gradient(int16_t x0, int16_t y0, uint32_t rgb0, int16_t x1, int16_t y1, uint32_t rgb1) {
        (void)x0; (void)y0; (void)x1; (void)y1;
        const uint32_t r = (((rgb0 >> 16) & 255u) + ((rgb1 >> 16) & 255u)) / 2u;
        const uint32_t g = (((rgb0 >> 8) & 255u) + ((rgb1 >> 8) & 255u)) / 2u;
        const uint32_t b = ((rgb0 & 255u) + (rgb1 & 255u)) / 2u;
        colorRGB(uint8_t(r), uint8_t(g), uint8_t(b));
        rect(0, 0, int16_t(width() - 1), int16_t(height() - 1));
    }

    // VERTEX_TRANSLATE_X: added to the x of every following VERTEX2F /
    // VERTEX2II, in 1/16 px (signed, 17 bits on the FT81x). Part of the
    // graphics context (reset by dlStart, saved by saveContext). VERTEX2II
    // only reaches x <= 511, so glyph runs right of that translate first.
    // Default: no-op (displays that never see x >= 512 VERTEX2II).
    virtual void vertexTranslateX(int32_t x16) { (void)x16; }

    // SAVE_CONTEXT / RESTORE_CONTEXT: push / pop the graphics context
    // (colour, alpha, line width, point size, scissor, vertex translate;
    // FT81x stack depth 4). Default: no-op.
    virtual void saveContext() {}
    virtual void restoreContext() {}

    // BITMAP_TRANSFORM_A..F for the following bitmap draws: source (u, v) =
    // (a*x + b*y + c, d*x + e*y + f) of the drawn pixel (x, y) relative to
    // the vertex; a, b, d, e signed 8.8, c, f signed 1/256 px. Part of the
    // graphics context (identity after dlStart, saved by saveContext).
    // bitmapSize: BITMAP_HANDLE(handle) + BITMAP_SIZE(NEAREST, BORDER, BORDER,
    // w, h), the drawn rectangle of `handle`'s cells (Gfx::textRotated); per
    // handle, not part of the context. Defaults: no-op.
    virtual void bitmapTransform(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) {
        (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    }
    virtual void bitmapSize(uint8_t handle, uint16_t w, uint16_t h) { (void)handle; (void)w; (void)h; }

    // --- convenience helpers built on the primitives ---------------------
    // RECTS / LINES vertices go at pixel centres (+8/16): the FT81x draws a
    // RECTS (LINE_WIDTH 16) from 0.5 px before the first vertex to 0.5 px
    // after the second, anti-aliased, so corner vertices would leave every
    // edge half-covered (dim 1-px dashes and dividers on the device,
    // Snapshot_014 vs stock Snapshot_013). Pixels x0..x1 / y0..y1 inclusive.
    void rect(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
        begin(kRects);
        vertex2f(int32_t(x0) * 16 + 8, int32_t(y0) * 16 + 8);
        vertex2f(int32_t(x1) * 16 + 8, int32_t(y1) * 16 + 8);
        end();
    }
    void line(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
        begin(kLines);
        vertex2f(int32_t(x0) * 16 + 8, int32_t(y0) * 16 + 8);
        vertex2f(int32_t(x1) * 16 + 8, int32_t(y1) * 16 + 8);
        end();
    }
};

// ---------------------------------------------------------------------------
// IAudioOut -- pull-model stereo output, 44.1 kHz, int16, blocks of 128 frames.
// The driver calls the callback from its DMA/ISR context; the callback must
// fill exactly `frames` interleaved L/R frames and must not block or allocate.
// ---------------------------------------------------------------------------
class IAudioOut {
public:
    static constexpr uint32_t kSampleRate = 44100;
    static constexpr uint32_t kBlockFrames = 128;
    static constexpr uint32_t kChannels = 2;

    // out: interleaved L,R,L,R...; frames == kBlockFrames.
    using FillFn = void (*)(int16_t* out, uint32_t frames, void* ctx);

    virtual ~IAudioOut() = default;
    virtual void setCallback(FillFn fn, void* ctx) = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
    // Audio output mute line (spec 01 §5.4 pin 21, LOW = muted). start() runs
    // the audio with the output still muted; the app enables it once the
    // start-up project has loaded and mutes it around project loads, like
    // stock (spec 05 §1.9). Default: no mute line.
    virtual void setOutputEnabled(bool on) { (void)on; }
    // Codec post-processing for the Master EQ (5 band gains -1..1), Bass Boost
    // (0..100) and Space (0..8), spec 18 §1. Main-loop context, called every
    // pass with the project values; the driver writes only what changed. It
    // acts after the mix, so offline renders never contain it. Default: no DSP.
    virtual void applyMasterDsp(const float eq[5], uint8_t bassBoost, uint8_t space) {
        (void)eq;
        (void)bassBoost;
        (void)space;
    }
};

// ---------------------------------------------------------------------------
// IStorage -- FatFs-like file access on the SD card. Paths use '/', are
// relative to the card root, and may use long names (LFN) or 8.3 names.
// ---------------------------------------------------------------------------
class IStorage {
public:
    using Handle = int32_t;                 // < 0 = invalid
    enum OpenMode : uint8_t {
        kRead = 1,                          // existing file, read only
        kWrite = 2,                         // create or truncate, write only
        kReadWrite = 3,                     // create if missing, no truncation
    };
    struct DirEntry {
        const char* name;                   // entry name (no path), valid during callback only
        bool isDir;
        uint32_t size;
    };
    // Return false to stop the listing early.
    using DirFn = bool (*)(const DirEntry& e, void* ctx);

    virtual ~IStorage() = default;

    virtual Handle open(const char* path, OpenMode mode) = 0;
    virtual int32_t read(Handle h, void* dst, uint32_t len) = 0;        // bytes read, <0 error
    virtual int32_t write(Handle h, const void* src, uint32_t len) = 0; // bytes written, <0 error
    virtual bool seek(Handle h, uint32_t pos) = 0;
    virtual int32_t size(Handle h) = 0;
    virtual void close(Handle h) = 0;

    virtual bool exists(const char* path) = 0;
    virtual bool mkdir(const char* path) = 0;    // single level; true if it exists afterwards
    virtual bool remove(const char* path) = 0;   // file or empty directory
    virtual bool rename(const char* from, const char* to) = 0;
    virtual int32_t listDir(const char* path, DirFn fn, void* ctx) = 0; // entry count, <0 error
    // Longest path (without a leading '/') the card storage takes; a longer
    // one fails instead of being cut (a cut path names another file or the
    // folder itself, issue #55).
    static constexpr uint32_t kMaxPath = 257;
};

// ---------------------------------------------------------------------------
// IMidiPort -- raw MIDI byte streams on the DIN and USB-device ports.
// ---------------------------------------------------------------------------
enum class MidiPortId : uint8_t { kDin = 0, kUsb = 1 };

class IMidiPort {
public:
    virtual ~IMidiPort() = default;
    // Queue bytes for output. Returns false if they do not fit (nothing sent).
    virtual bool send(MidiPortId port, const uint8_t* data, uint32_t len) = 0;
    // Copy up to `cap` received bytes into dst; returns the count (0 = none).
    virtual uint32_t receive(MidiPortId port, uint8_t* dst, uint32_t cap) = 0;
};

// ---------------------------------------------------------------------------
// IInput -- front-panel events (buttons, the pad grid, the encoder).
// ---------------------------------------------------------------------------
struct InputEvent {
    enum Type : uint8_t {
        kNone = 0,
        kButtonDown,
        kButtonUp,
        kPadDown,
        kPadUp,
        kEncoder,
    };
    Type type = kNone;
    uint8_t id = 0;        // button id or pad index (0-based)
    uint8_t velocity = 0;  // pads only, 0..127
    int8_t delta = 0;      // encoder only, detents (+ clockwise)
    uint32_t timeMs = 0;   // IClock::millis() at capture
};

class IInput {
public:
    virtual ~IInput() = default;
    // Pop the oldest event; false when the queue is empty.
    virtual bool poll(InputEvent& ev) = 0;
};

// ---------------------------------------------------------------------------
// ILeds -- pad grid RGB LEDs. Changes become visible on show().
// ---------------------------------------------------------------------------
class ILeds {
public:
    virtual ~ILeds() = default;
    virtual uint8_t padCount() const = 0;
    virtual void setPad(uint8_t pad, uint8_t r, uint8_t g, uint8_t b) = 0;
    virtual void clear() = 0;
    virtual void show() = 0;
};

// ---------------------------------------------------------------------------
// IRadio -- optional FM tuner whose audio feeds the codec line input
// (spec 05 appendix C §2.8, spec 04 §5.7). Boards/hosts without a tuner
// driver pass no IRadio; the recorder then reports "not available".
// ---------------------------------------------------------------------------
class IRadio {
public:
    virtual ~IRadio() = default;
    // Band limits for the region (spec 05 appendix C §2.8 region table).
    virtual void setBand(float minMHz, float maxMHz, float stepMHz) = 0;
    virtual bool tune(float mhz) = 0;
    virtual bool seek(bool up) = 0;          // starts a seek; poll busy()
    virtual bool busy() const = 0;
    virtual float frequency() const = 0;     // MHz
};

// ---------------------------------------------------------------------------
// IClock -- monotonic time since boot (wraps like the Arduino counters).
// ---------------------------------------------------------------------------
class IClock {
public:
    virtual ~IClock() = default;
    virtual uint32_t micros() const = 0;
    virtual uint32_t millis() const = 0;
};

// ---------------------------------------------------------------------------
// IDisplay JPEG helpers (need IStorage).
// ---------------------------------------------------------------------------
inline bool IDisplay::probeJpeg(IStorage& fs, const char* path, uint32_t ramgAddr, uint16_t maxW,
                                uint16_t maxH, uint16_t& w, uint16_t& h, uint32_t& fileSize) {
    w = h = 0;
    fileSize = 0;
    if (!path || (ramgAddr & 3u) || ramgAddr >= kRamGImageLimit) return false;
    const IStorage::Handle f = fs.open(path, IStorage::kRead);
    if (f < 0) return false;
    const int32_t sz = fs.size(f);
    bool ok = sz > 4 && uint32_t(sz) <= kRamGImageLimit - ramgAddr;
    uint8_t b[7];
    ok = ok && fs.read(f, b, 2) == 2 && b[0] == 0xFF && b[1] == 0xD8;   // SOI
    uint32_t pos = 2;
    bool found = false;
    // Walk the marker segments up to the first SOFn (bounded; spec 13 §G.6
    // parses SOF from the head of the file). SPEC GAP: segments are skipped by
    // seeking, so large APPn blocks before SOF do not limit the probe.
    for (int seg = 0; ok && !found && seg < 64; ++seg) {
        ok = fs.seek(f, pos) && fs.read(f, b, 2) == 2 && b[0] == 0xFF;
        if (!ok) break;
        uint8_t m = b[1];
        pos += 2;
        while (ok && m == 0xFF) {             // fill bytes
            ok = fs.read(f, &m, 1) == 1;
            ++pos;
        }
        if (!ok) break;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;   // standalone markers
        if (m == 0xD8 || m == 0xD9 || m == 0xDA) { ok = false; break; }   // no SOF before data
        ok = fs.read(f, b, 2) == 2;
        const uint32_t len = ok ? (uint32_t(b[0]) << 8 | b[1]) : 0;
        ok = ok && len >= 2;
        if (!ok) break;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            // SOFn: only baseline / extended sequential Huffman decode.
            ok = (m == 0xC0 || m == 0xC1) && len >= 8 && fs.read(f, b, 5) == 5 && b[0] == 8;
            if (ok) {
                h = uint16_t(b[1] << 8 | b[2]);
                w = uint16_t(b[3] << 8 | b[4]);
                found = true;
            }
            break;
        }
        pos += len;
        ok = pos < uint32_t(sz);
    }
    fs.close(f);
    ok = ok && found && w && h && w <= maxW && h <= maxH &&
         ramgAddr + uint32_t(w) * h * 2u <= kRamGSize;
    if (!ok) {
        w = h = 0;
        return false;
    }
    fileSize = uint32_t(sz);
    return true;
}

inline bool IDisplay::loadJpegBase(uint32_t ramgAddr, IStorage& fs, const char* base, uint16_t maxW,
                                   uint16_t maxH, uint16_t& w, uint16_t& h) {
    w = h = 0;
    if (!base) return false;
    char path[256];
    uint32_t n = 0;
    while (base[n] && n < sizeof(path) - 6) { path[n] = base[n]; ++n; }
    if (base[n]) return false;                      // too long
    const char* ext[2] = {".jpg", ".jpeg"};
    for (const char* e : ext) {
        uint32_t k = n;
        for (const char* p = e; *p; ++p) path[k++] = *p;
        path[k] = 0;
        if (fs.exists(path)) return loadJpeg(ramgAddr, fs, path, maxW, maxH, w, h);
    }
    return false;
}

// ---------------------------------------------------------------------------
// INvStore -- non-volatile settings bytes: the global config image (spec 05
// §5.4, spec 01 §10). Offsets are app-area offsets: the device adds the stock
// +10 (spec 02 §9), so EEPROM bytes 0..9 (byte 0 = update/power flag) are
// never reachable through this interface. read/write return false when the
// store is not usable (e.g. the FlexRAM is not partitioned as EEPROM) or the
// range is outside size().
// ---------------------------------------------------------------------------
class INvStore {
public:
    virtual ~INvStore() = default;
    virtual uint32_t size() const = 0;
    virtual bool read(uint32_t offset, void* dst, uint32_t len) = 0;
    virtual bool write(uint32_t offset, const void* src, uint32_t len) = 0;
};

// ---------------------------------------------------------------------------
// IUsbStorage -- 2.x USB Storage mode (Tracker Mini manual §13.10): the SD
// card is handed to a computer as a USB drive. Device: src/hal/usb_storage.*
// (USB mass storage, Bulk-Only Transport); outside the mode the drive reports
// "medium not present". The app calls begin() only after it stopped playback
// and recording and wrote everything to the card; between begin() and end()
// the app must not touch IStorage (the implementation refuses it anyway).
// ---------------------------------------------------------------------------
class IUsbStorage {
public:
    enum class Host : uint8_t {
        kNoUsb = 0,   // no computer (cable unplugged, host asleep)
        kWaiting,     // connected; the computer has not used the drive yet
        kInUse,       // the computer read the drive (it is mounted there)
        kEjected,     // the computer ejected the drive: safe to exit
    };
    virtual ~IUsbStorage() = default;
    // Unmount the card from the app and present it to the computer (UNIT
    // ATTENTION "medium changed"). False: no usable card; the app keeps it.
    virtual bool begin() = 0;
    // Report the medium removed, let a command in flight finish, take the
    // card back and remount it. False: the card could not be mounted again.
    virtual bool end() = 0;
    virtual Host host() const = 0;
    // Main loop, every pass: the card reads / writes the computer asked for.
    virtual void service() = 0;
};

}  // namespace ot
