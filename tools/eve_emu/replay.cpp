// Replays coprocessor streams (otf2cmd output) on Bridgetek's FT81x emulator
// (bt8xxemu.dll, FT813 like the Tracker; Windows only, runs under Wine) and
// reports what each frame built in RAM_DL, optionally comparing the pixels of
// two streams.
//
//   replay.exe FONTS.bin LIST.txt
//
// FONTS.bin goes to RAM_G 0x06CC00 (the driver's font image). LIST.txt has
// one "NAME FILE.cmd [REF.cmd]" per line; each stream goes through the
// command FIFO and prints "NAME SENT_WORDS REG_CMD_DL_BYTES ok|FAULT|TIMEOUT".
// With REF.cmd both frames are also shown (CMD_SWAP, the Tracker's panel
// timings) and their scanned-out pixels compared: the line ends with "same"
// or "DIFF N X0 Y0 X1 Y1" (pixels that differ, their bounding box). After a
// fault the coprocessor is reset as Ft81xDisplay::recoverCoprocessor() does
// and the next frame is replayed.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bt8xxemu.h"

namespace {

BT8XXEMU_Emulator* E;

constexpr uint32_t kW = 800, kH = 480;
std::mutex gShotLock;
std::vector<argb8888> gShot(kW * kH);   // the last frame scanned out
std::atomic<uint32_t> gFrames{0};

int graphics(BT8XXEMU_Emulator*, void*, int output, const argb8888* buf, uint32_t w, uint32_t h,
             BT8XXEMU_FrameFlags) {
    if (output && buf && w == kW && h == kH) {
        std::lock_guard<std::mutex> g(gShotLock);
        gShot.assign(buf, buf + kW * kH);
    }
    ++gFrames;
    return 1;
}
void logcb(BT8XXEMU_Emulator*, void*, BT8XXEMU_LogType t, const char* m) {
    if (t != BT8XXEMU_LogMessage) std::fprintf(stderr, "[emu %d] %s\n", int(t), m);
}

void addr(uint32_t a, bool wr) {
    BT8XXEMU_transfer(E, uint8_t(((a >> 16) & 0x3F) | (wr ? 0x80 : 0)));
    BT8XXEMU_transfer(E, uint8_t(a >> 8));
    BT8XXEMU_transfer(E, uint8_t(a));
}
uint32_t rd32(uint32_t a) {
    BT8XXEMU_chipSelect(E, 1);
    addr(a, false);
    BT8XXEMU_transfer(E, 0);
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(BT8XXEMU_transfer(E, 0)) << (8 * i);
    BT8XXEMU_chipSelect(E, 0);
    return v;
}
void wrBytes(uint32_t a, const uint8_t* p, size_t n) {
    BT8XXEMU_chipSelect(E, 1);
    addr(a, true);
    for (size_t i = 0; i < n; ++i) BT8XXEMU_transfer(E, p[i]);
    BT8XXEMU_chipSelect(E, 0);
}
void wr(uint32_t a, uint32_t v, int n) {
    const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    wrBytes(a, b, size_t(n));
}

constexpr uint32_t kRamDl = 0x300000, kRamCmd = 0x308000, kRegCpuReset = 0x302020, kRegDlSwap = 0x302054,
                   kRegPclk = 0x302070, kRegCmdRead = 0x3020F8, kRegCmdWrite = 0x3020FC, kRegCmdDl = 0x302100,
                   kFontBase = 0x06CC00, kCmdSwap = 0xFFFFFF01u;

enum Result { kOk, kFault, kTimeout };

Result waitIdle() {
    for (int i = 0; i < 2000000; ++i) {
        const uint32_t r = rd32(kRegCmdRead) & 0xFFF;
        if (r == 0xFFF) return kFault;
        if (r == (rd32(kRegCmdWrite) & 0xFFF)) return kOk;
    }
    return kTimeout;
}

Result send(const std::vector<uint32_t>& w) {
    size_t i = 0;
    while (i < w.size()) {
        const uint32_t rp = rd32(kRegCmdRead) & 0xFFF, wp = rd32(kRegCmdWrite) & 0xFFF;
        if (rp == 0xFFF) return kFault;
        uint32_t room = 4092 - ((wp - rp) & 0xFFF), pos = wp;
        for (int k = 0; room >= 4 && i < w.size() && k < 256; ++k) {
            wr(kRamCmd + pos, w[i++], 4);
            pos = (pos + 4) & 0xFFF;
            room -= 4;
        }
        wr(kRegCmdWrite, pos, 4);
    }
    return waitIdle();
}

void recover() {   // Ft81xDisplay::recoverCoprocessor (programmer guide §5.6)
    wr(kRegCpuReset, 1, 1);
    wr(kRegCmdRead, 0, 4);
    wr(kRegCmdWrite, 0, 4);
    wr(kRegCpuReset, 0, 1);
}

// Ft81xDisplay::init's 800 x 480 panel timings, so frames are scanned out.
void panel() {
    wr(0x302034, 800, 2); wr(0x30202C, 1056, 2); wr(0x302030, 46, 2); wr(0x302038, 0, 2); wr(0x30203C, 10, 2);
    wr(0x302048, 480, 2); wr(0x302040, 525, 2); wr(0x302044, 23, 2); wr(0x30204C, 0, 2); wr(0x302050, 10, 2);
    wr(0x302064, 0, 1); wr(0x30206C, 1, 1); wr(0x302068, 0, 1); wr(0x302060, 1, 1); wr(0x30205C, 0x1B6, 2);
    wr(kRamDl + 0, 0x02000000, 4); wr(kRamDl + 4, 0x26000007, 4); wr(kRamDl + 8, 0, 4); wr(kRegDlSwap, 2, 4);
    wr(kRegPclk, 2, 1);
}

// The frame on screen once a few more have been scanned out after a swap.
bool shot(std::vector<argb8888>& out) {
    const uint32_t f0 = gFrames;
    for (int i = 0; i < 500 && gFrames < f0 + 4; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (gFrames < f0 + 4) return false;
    std::lock_guard<std::mutex> g(gShotLock);
    out = gShot;
    return true;
}

std::vector<uint32_t> loadWords(const std::string& p) {
    std::vector<uint32_t> w;
    if (FILE* f = std::fopen(p.c_str(), "rb")) {
        uint8_t b[4];
        while (std::fread(b, 1, 4, f) == 4)
            w.push_back(uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24);
        std::fclose(f);
    }
    return w;
}

std::vector<uint8_t> loadBytes(const std::string& p) {
    std::vector<uint8_t> b;
    if (FILE* f = std::fopen(p.c_str(), "rb")) {
        int c;
        while ((c = std::fgetc(f)) != EOF) b.push_back(uint8_t(c));
        std::fclose(f);
    }
    return b;
}

// Sends a frame (and, to show it, CMD_SWAP); REG_CMD_DL is read before the swap.
Result frame(const std::vector<uint32_t>& w, bool show, uint32_t& dl) {
    Result r = w.empty() ? kTimeout : send(w);
    dl = rd32(kRegCmdDl) & 0x1FFF;
    if (r == kOk && show) r = send({kCmdSwap});
    if (r != kOk) recover();
    return r;
}

const char* name(Result r) { return r == kOk ? "ok" : (r == kFault ? "FAULT" : "TIMEOUT"); }

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: replay.exe FONTS.bin LIST.txt\n");
        return 2;
    }
    BT8XXEMU_EmulatorParameters p;
    BT8XXEMU_defaults(BT8XXEMU_VERSION_API, &p, BT8XXEMU_EmulatorFT813);
    // Only the coprocessor: no audio (a CI runner has no audio device and the
    // emulator crashes looking for one), keyboard, mouse or frame skipping.
    p.Flags = BT8XXEMU_EmulatorEnableCoprocessor;
    p.Graphics = graphics;
    p.Log = logcb;
    BT8XXEMU_run(BT8XXEMU_VERSION_API, &E, &p);
    panel();
    const std::vector<uint8_t> fonts = loadBytes(argv[1]);
    if (fonts.empty()) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    for (size_t o = 0; o < fonts.size(); o += 4096)
        wrBytes(kFontBase + uint32_t(o), fonts.data() + o, fonts.size() - o < 4096 ? fonts.size() - o : 4096);
    FILE* list = std::fopen(argv[2], "r");
    if (!list) {
        std::fprintf(stderr, "cannot read %s\n", argv[2]);
        return 2;
    }
    char line[2048];
    int frames = 0;
    std::vector<argb8888> a, b;
    while (std::fgets(line, sizeof line, list)) {
        char nm[256], fa[900], fb[900];
        const int n = std::sscanf(line, "%255s %899s %899s", nm, fa, fb);
        if (n < 2) continue;
        const bool compare = n == 3;
        uint32_t dl = 0, dlRef = 0;
        const Result r = frame(loadWords(fa), compare, dl);
        std::printf("%s %zu %u %s", nm, loadWords(fa).size(), unsigned(dl), name(r));
        if (compare) {
            const bool gotA = r == kOk && shot(a);
            const Result rr = frame(loadWords(fb), true, dlRef);
            const bool gotB = rr == kOk && shot(b);
            if (!gotA || !gotB) {
                std::printf(" NOSHOT %s", name(rr));
            } else {
                uint32_t diff = 0, x0 = kW, y0 = kH, x1 = 0, y1 = 0;
                for (uint32_t i = 0; i < kW * kH; ++i)
                    if ((a[i] ^ b[i]) & 0xFFFFFFu) {
                        ++diff;
                        const uint32_t x = i % kW, y = i / kW;
                        if (x < x0) x0 = x;
                        if (y < y0) y0 = y;
                        if (x > x1) x1 = x;
                        if (y > y1) y1 = y;
                    }
                if (diff && std::getenv("REPLAY_DUMP")) {   // NAME.a.rgb / NAME.b.rgb: 800 x 480 x 4 bytes
                    for (int k = 0; k < 2; ++k) {
                        const std::string out = std::string(nm) + (k ? ".b.rgb" : ".a.rgb");
                        if (FILE* f = std::fopen(out.c_str(), "wb")) {
                            std::fwrite((k ? b : a).data(), 4, kW * kH, f);
                            std::fclose(f);
                        }
                    }
                }
                if (diff) std::printf(" DIFF %u %u %u %u %u", unsigned(diff), unsigned(x0), unsigned(y0), unsigned(x1), unsigned(y1));
                else std::printf(" same");
            }
        }
        std::printf("\n");
        std::fflush(stdout);
        ++frames;
    }
    std::fclose(list);
    BT8XXEMU_destroy(E);
    return frames ? 0 : 2;
}
