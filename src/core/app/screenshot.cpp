// Screenshot to BMP (spec 05 §3.4, §2.12 "Snapshots"): Shift + Power writes
// the shown frame as an 800 x 480, 32-bpp BMP (54-byte header, 1,536,054
// bytes) to the first free Snapshots/Snapshot_NNN.bmp. The frame is read
// back through IDisplay::readPixels row by row (no frame-sized buffer).
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "sdram_bss.h"

namespace ot {

namespace {
constexpr uint32_t kW = 800, kH = 480;
constexpr uint32_t kHeader = 54;
constexpr uint32_t kFileSize = kHeader + kW * kH * 4;   // 1,536,054
static_assert(kFileSize == 1536054u, "spec 05 §3.4 file size");

void put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

// One row of pixels (static: no large stack frame; SDRAM on the device, OP-5),
// read as 0x00RRGGBB words and turned into BMP bytes in place: pixel x's
// bytes are the 4 bytes of word x, so a byte write never reaches a word that
// is still to be read.
OT_SDRAM_BSS uint32_t gRow[kW];
}  // namespace

// BITMAPFILEHEADER + BITMAPINFOHEADER, bottom-up rows, BI_RGB 32 bpp.
static void bmpHeader(uint8_t h[kHeader]) {
    std::memset(h, 0, kHeader);
    h[0] = 'B';
    h[1] = 'M';
    put32(h + 2, kFileSize);
    put32(h + 10, kHeader);
    put32(h + 14, 40);
    put32(h + 18, kW);
    put32(h + 22, kH);
    put16(h + 26, 1);
    put16(h + 28, 32);
    put32(h + 30, 0);
    put32(h + 34, kW * kH * 4);
    put32(h + 38, 2835);   // 72 dpi
    put32(h + 42, 2835);
}

bool App::screenshot(char* pathOut, int cap) {
    IDisplay* d = hal_.display;
    if (!d || !hal_.storage) return false;
    if (!d->readPixels(0, 0, 1, 1, gRow)) return false;
    IStorage& f = fs();
    f.mkdir("Snapshots");
    char path[48];
    bool found = false;
    // SPEC GAP: whether numbering starts at 000 or 001 is not recorded; 001.
    for (int n = 1; n <= 999 && !found; ++n) {
        std::snprintf(path, sizeof(path), "Snapshots/Snapshot_%03d.bmp", n);
        found = !f.exists(path);
    }
    if (!found) return false;
    IStorage::Handle h = f.open(path, IStorage::kWrite);
    if (h < 0) return false;
    uint8_t hdr[kHeader];
    bmpHeader(hdr);
    bool ok = f.write(h, hdr, kHeader) == int32_t(kHeader);
    for (int y = int(kH) - 1; y >= 0 && ok; --y) {
        if (!d->readPixels(0, uint16_t(y), uint16_t(kW), 1, gRow)) {
            ok = false;
            break;
        }
        uint8_t* const bytes = reinterpret_cast<uint8_t*>(gRow);
        for (uint32_t x = 0; x < kW; ++x) {
            const uint32_t c = gRow[x];
            bytes[4 * x + 0] = uint8_t(c);         // B
            bytes[4 * x + 1] = uint8_t(c >> 8);    // G
            bytes[4 * x + 2] = uint8_t(c >> 16);   // R
            bytes[4 * x + 3] = 0xFF;
        }
        ok = f.write(h, bytes, sizeof(gRow)) == int32_t(sizeof(gRow));
    }
    f.close(h);
    if (!ok) {
        f.remove(path);
        return false;
    }
    if (pathOut && cap > 0) std::snprintf(pathOut, size_t(cap), "%s", path);
    return true;
}

}  // namespace ot
