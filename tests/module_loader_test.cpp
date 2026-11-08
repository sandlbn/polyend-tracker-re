// [I] Loadable modules (modules/module_loader.h): the .otm checks and
// fixups on built-in images, and a real module (tools/make_otm.py) against the
// image the linker makes for the same address (OT_OTM_DIR, skipped without it).
#include "test.h"

#include <cstdio>
#include <string>
#include <vector>

#include "formats/crc32.h"
#include "mem_storage.h"
#include "modules/module.h"
#include "modules/module_files.h"
#include "modules/module_loader.h"
#include "modules/synth_descriptor.h"

using namespace ot;
using namespace ot::mod;

namespace {

constexpr uint32_t kLayout = 0x12345678u, kAbi = 0x9ABCDEF0u;
const uint32_t kExports[3] = {0x00001001u, 0x00002000u, 0x00003001u};
const OtmCore kCore = {1, kLayout, kAbi, kExports, 3};

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

// A 16-byte image: word 0 = image offset 8 (kind 0), word 1 = addend 4 on
// export 2, word 2 = plain data, word 3 = export 3 (addend 0).
struct Built {
    OtmHeader h;
    std::vector<uint8_t> image{8, 0, 0, 0, 4, 0, 0, 0, 0xAA, 0xBB, 0xCC, 0xDD, 0, 0, 0, 0};
    std::vector<uint32_t> fixups{0u, 4u | 2u << 24, 12u | 3u << 24};
    Built() {
        h = OtmHeader{kOtmMagic, kOtmHeaderBytes, 1, kLayout, kAbi, {'T', 'E', 'S', 'T'}, 16, 8, 8, 1, 3, 3, 0, 0};
    }
    std::vector<uint8_t> file() const {
        OtmHeader hh = h;
        hh.imageBytes = uint32_t(image.size());
        hh.nFixups = uint32_t(fixups.size());
        std::vector<uint8_t> v(sizeof(hh));
        std::memcpy(v.data(), &hh, sizeof(hh));
        v.insert(v.end(), image.begin(), image.end());
        for (uint32_t f : fixups) put32(v, f);
        put32(v, crc32(v.data(), v.size()));
        return v;
    }
};

uint32_t word(const uint8_t* p) { return uint32_t(p[0]) | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

OtmError load(const std::vector<uint8_t>& f, uint8_t* dst, uint32_t cap, uint32_t base, uint32_t* entry = nullptr) {
    return otmLoad(f.data(), uint32_t(f.size()), kCore, dst, cap, base, entry);
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::vector<uint8_t> v;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        uint8_t b[4096];
        size_t n;
        while ((n = std::fread(b, 1, sizeof(b), f)) > 0) v.insert(v.end(), b, b + n);
        std::fclose(f);
    }
    return v;
}

}  // namespace

TEST(loads_and_applies_fixups) {
    const std::vector<uint8_t> f = Built().file();
    uint8_t dst[32];
    std::memset(dst, 0x55, sizeof(dst));
    uint32_t entry = 0;
    CHECK(load(f, dst, sizeof(dst), 0x1FFF8000u, &entry) == OtmError::None);
    CHECK_EQ(word(dst + 0), 0x1FFF8008u);          // inside the image: + base
    CHECK_EQ(word(dst + 4), 0x00002004u);          // export 2 + addend 4
    CHECK_EQ(word(dst + 8), 0xDDCCBBAAu);          // untouched
    CHECK_EQ(word(dst + 12), 0x00003001u);         // export 3 (Thumb bit kept)
    for (int i = 16; i < 24; ++i) CHECK_EQ(dst[i], 0);   // bss zeroed
    CHECK_EQ(dst[24], 0x55);                        // nothing past image + bss
    CHECK_EQ(entry, 0x1FFF8001u);
}

TEST(refuses_mismatches) {
    uint8_t dst[64] = {};
    {
        Built b;
        b.h.api = 2;
        CHECK(load(b.file(), dst, sizeof(dst), 0) == OtmError::Api);
    }
    {
        Built b;
        b.h.layoutHash ^= 1;
        CHECK(load(b.file(), dst, sizeof(dst), 0) == OtmError::Layout);
    }
    {
        Built b;
        b.h.abiFlags ^= 1;
        CHECK(load(b.file(), dst, sizeof(dst), 0) == OtmError::Abi);
    }
    {
        Built b;
        b.h.nExports = 4;   // this core has 3
        CHECK(load(b.file(), dst, sizeof(dst), 0) == OtmError::Exports);
    }
    {
        Built b;
        b.h.magic = 0;
        CHECK(load(b.file(), dst, sizeof(dst), 0) == OtmError::BadMagic);
    }
    {
        Built b;
        b.h.entryOffset = 2;   // not a Thumb address: would fault on every boot
        CHECK(load(b.file(), dst, sizeof(dst), 0) == OtmError::BadMagic);
    }
    CHECK_STR(otmErrorText(OtmError::Layout), "needs updating");
}

TEST(refuses_damaged_files) {
    uint8_t dst[64] = {};
    std::vector<uint8_t> f = Built().file();
    std::vector<uint8_t> bad = f;
    bad[kOtmHeaderBytes + 9] ^= 0x01;   // a byte of the image
    CHECK(load(bad, dst, sizeof(dst), 0) == OtmError::BadCrc);
    bad = f;
    bad.pop_back();
    CHECK(load(bad, dst, sizeof(dst), 0) == OtmError::Truncated);
    CHECK(otmLoad(f.data(), 10, kCore, dst, sizeof(dst), 0, nullptr) == OtmError::Truncated);
    Built b;
    b.fixups.push_back(14u);   // word at 14..17: past the 16-byte image
    CHECK(load(b.file(), dst, sizeof(dst), 0) == OtmError::BadFixup);
    Built c;
    c.fixups.push_back(0u | 4u << 24);   // kind past nExports (3)
    CHECK(load(c.file(), dst, sizeof(dst), 0) == OtmError::BadFixup);
}

TEST(refused_load_leaves_the_block_alone) {
    uint8_t dst[64];
    std::memset(dst, 0x55, sizeof(dst));
    Built b;
    b.fixups.push_back(14u);
    CHECK(load(b.file(), dst, sizeof(dst), 0) == OtmError::BadFixup);
    for (uint8_t x : dst) CHECK_EQ(x, 0x55);
}

TEST(refuses_small_or_misaligned_blocks) {
    const std::vector<uint8_t> f = Built().file();
    uint8_t dst[64] = {};
    CHECK(load(f, dst, 23, 0) == OtmError::TooBig);   // image 16 + bss 8
    CHECK(load(f, dst, 24, 0) == OtmError::None);
    CHECK(load(f, dst, 24, 4) == OtmError::Align);    // the module wants 8
}

// An image with a row: two 8-byte row functions, cuts at 8 (60 % of the
// work) and 16 (90 %), and an 8-byte main part. Row linked at 0, main part
// at kOtmMainLink.
//   row 0:  word 0 = a main-part address, BL at 4 into the main part;
//   row 8:  word 8 = the row function at 0 (Thumb), BL at 12 to row 0;
//   main:   word 16 (main + 0) = the row function at 8 (Thumb), B.W at 20 to row 0.
struct Built2 {
    std::vector<uint8_t> image;
    std::vector<uint32_t> fixups;
    std::vector<uint32_t> cuts{8, 600000, 16, 900000};
    OtmHeader h;
    static void bl(std::vector<uint8_t>& v, size_t at, int32_t off, bool link) {
        const uint32_t u = uint32_t(off), s = (u >> 24) & 1u, i1 = (u >> 23) & 1u, i2 = (u >> 22) & 1u;
        const uint32_t j1 = (~i1 ^ s) & 1u, j2 = (~i2 ^ s) & 1u;
        const uint16_t a = uint16_t(0xF000u | s << 10 | ((u >> 12) & 0x3FFu));
        const uint16_t b = uint16_t((link ? 0xD000u : 0x9000u) | j1 << 13 | j2 << 11 | ((u >> 1) & 0x7FFu));
        v[at] = uint8_t(a), v[at + 1] = uint8_t(a >> 8), v[at + 2] = uint8_t(b), v[at + 3] = uint8_t(b >> 8);
    }
    Built2() {
        image.assign(24, 0);
        auto w = [&](size_t at, uint32_t x) { for (int i = 0; i < 4; ++i) image[at + i] = uint8_t(x >> (8 * i)); };
        w(0, kOtmMainLink + 5);                                             // a main-part function (Thumb)
        bl(image, 4, int32_t(kOtmMainLink + 4) - int32_t(4 + 4), true);     // BL main+4
        w(8, 1);                                                            // row function 0 (Thumb)
        bl(image, 12, int32_t(0) - int32_t(12 + 4), true);                  // BL row 0
        w(16, 9);                                                           // row function 8 (Thumb)
        bl(image, 20, int32_t(0) - int32_t(kOtmMainLink + 4 + 4), false);   // B.W row 0 (main part offset 4)
        fixups = {0u, 4u | kOtmFixBranch << 24, 8u | kOtmFixFast << 24, 12u | kOtmFixBranch << 24,
                  16u | kOtmFixFast << 24, 20u | kOtmFixBranch << 24};
        h = OtmHeader{kOtmMagic, kOtmHeaderBytes, 1, kLayout, kAbi, {'T', 'W', 'O'}, 24, 4, 8, 9, 6, 0, 16, 2};
    }
    std::vector<uint8_t> file() const {
        OtmHeader hh = h;
        hh.nFixups = uint32_t(fixups.size());
        hh.nCuts = uint32_t(cuts.size() / 2);
        std::vector<uint8_t> v(sizeof(hh));
        std::memcpy(v.data(), &hh, sizeof(hh));
        v.insert(v.end(), image.begin(), image.end());
        for (uint32_t f : fixups) put32(v, f);
        for (uint32_t c : cuts) put32(v, c);
        put32(v, crc32(v.data(), v.size()));
        return v;
    }
};

// The displacement of a BL / B.W, read back the way the CPU does.
int32_t branchAt(const uint8_t* p) {
    const uint32_t a = uint32_t(p[0] | p[1] << 8), b = uint32_t(p[2] | p[3] << 8);
    const uint32_t s = (a >> 10) & 1u, i1 = ~(((b >> 13) & 1u) ^ s) & 1u, i2 = ~(((b >> 11) & 1u) ^ s) & 1u;
    uint32_t v = s << 24 | i1 << 23 | i2 << 22 | (a & 0x3FFu) << 12 | (b & 0x7FFu) << 1;
    if (s) v |= 0xFE000000u;
    return int32_t(v);
}

constexpr uint32_t kFb = 0x1FFFC000u, kMb = 0x20010000u;

// Loads Built2 at a cut; where row functions 0 and 8 and the main part's 4 landed.
struct Loaded2 {
    uint8_t fast[24], mainb[48];
    uint32_t entry = 0, row0 = 0, row8 = 0;
    OtmError e;
    uint8_t* at(uint32_t a) { return a >= kMb ? mainb + (a - kMb) : fast + (a - kFb); }
    explicit Loaded2(uint32_t cut) {
        std::memset(fast, 0x55, sizeof(fast));
        std::memset(mainb, 0x55, sizeof(mainb));
        const std::vector<uint8_t> f = Built2().file();
        e = otmLoad(f.data(), uint32_t(f.size()), kCore, OtmBlock{fast, sizeof(fast), kFb},
                    OtmBlock{mainb, sizeof(mainb), kMb}, cut, &entry);
        // The main part (8) + bss (4) = 12; the tail starts at 16 (a multiple of 8).
        row0 = cut > 0 ? kFb : kMb + 16;
        row8 = cut > 8 ? kFb + 8 : kMb + 16 + (8 - cut);
    }
};

TEST(row_cut_at_each_cut) {
    for (uint32_t cut : {0u, 8u, 16u}) {
        Loaded2 l(cut);
        CHECK(l.e == OtmError::None);
        CHECK_EQ(l.entry, l.row8 + 1);                                    // entry offset 9: row function 8
        CHECK_EQ(word(l.at(l.row0)), kMb + 5);                            // main-part address
        CHECK_EQ(int64_t(l.row0 + 8) + branchAt(l.at(l.row0 + 4)), int64_t(kMb + 4));   // BL lands on main + 4
        CHECK_EQ(word(l.at(l.row8)), l.row0 + 1);                         // row function 0
        CHECK_EQ(int64_t(l.row8 + 8) + branchAt(l.at(l.row8 + 4)), int64_t(l.row0));    // BL lands on row 0
        CHECK_EQ(word(l.mainb + 0), l.row8 + 1);                          // row function 8
        CHECK_EQ(int64_t(kMb + 8) + branchAt(l.mainb + 4), int64_t(l.row0));            // B.W lands on row 0
        CHECK_EQ(uint32_t(l.mainb[6] | l.mainb[7] << 8) & 0xD000u, 0x9000u);  // still a B.W, not a BL
        for (int i = 8; i < 12; ++i) CHECK_EQ(l.mainb[i], 0);            // bss zeroed
        CHECK_EQ(l.fast[cut], 0x55);                                      // nothing past the cut
        CHECK_EQ(l.mainb[cut < 16 ? 16 + (16 - cut) : 12], 0x55);         // nothing past the tail / bss
    }
}

TEST(row_cuts_read_back) {
    const std::vector<uint8_t> f = Built2().file();
    OtmHeader h;
    CHECK(otmCheck(f.data(), uint32_t(f.size()), kCore, &h) == OtmError::None);
    CHECK_EQ(otmCutBytes(f.data(), h, 1), 16u);
    CHECK_EQ(otmCutShare(f.data(), h, 0), 600000u);
    CHECK_EQ(otmCutFor(f.data(), h, 7), 0u);
    CHECK_EQ(otmCutFor(f.data(), h, 15), 8u);
    CHECK_EQ(otmCutFor(f.data(), h, 100), 16u);
    CHECK_EQ(otmMainSpan(h, 0), 16u + 16u);
    CHECK_EQ(otmMainSpan(h, 8), 16u + 8u);
    CHECK_EQ(otmMainSpan(h, 16), 12u);
}

TEST(row_in_one_block) {
    const std::vector<uint8_t> f = Built2().file();
    uint8_t dst[64];
    std::memset(dst, 0x55, sizeof(dst));
    const uint32_t base = 0x20010000u;
    CHECK(load(f, dst, sizeof(dst), base) == OtmError::None);   // cut 0: the row after the bss
    CHECK_EQ(word(dst + 16 + 8), base + 16 + 1);
    CHECK(load(f, dst, 16 + 16 - 1, base) == OtmError::TooBig);
}

TEST(row_refusals) {
    uint8_t a[24], b[48];
    {
        Built2 x;
        x.fixups.push_back(16u | kOtmFixBranch << 24);   // not a BL / B.W there
        const std::vector<uint8_t> f = x.file();
        std::memset(a, 0x55, sizeof(a));
        CHECK(otmLoad(f.data(), uint32_t(f.size()), kCore, OtmBlock{a, 24, kFb}, OtmBlock{b, 48, kMb}, 16,
                      nullptr) == OtmError::BadFixup);
        CHECK_EQ(a[0], 0x55);   // nothing written
    }
    {
        Built2 x;
        x.fixups.push_back(6u);   // a word across the cut at 8 (6..9)
        const std::vector<uint8_t> f = x.file();
        CHECK(otmLoad(f.data(), uint32_t(f.size()), kCore, OtmBlock{a, 24, kFb}, OtmBlock{b, 48, kMb}, 8, nullptr) ==
              OtmError::BadFixup);
    }
    {
        const std::vector<uint8_t> f = Built2().file();
        CHECK(otmLoad(f.data(), uint32_t(f.size()), kCore, OtmBlock{a, 12, kFb}, OtmBlock{b, 48, kMb}, 16,
                      nullptr) == OtmError::TooBig);   // cut 16 > 12
        CHECK(otmLoad(f.data(), uint32_t(f.size()), kCore, OtmBlock{a, 24, kFb + 4}, OtmBlock{b, 48, kMb}, 8,
                      nullptr) == OtmError::Align);
        CHECK(otmLoad(f.data(), uint32_t(f.size()), kCore, OtmBlock{a, 24, kFb}, OtmBlock{b, 48, kMb}, 12,
                      nullptr) == OtmError::BadFixup);   // not a cut
        CHECK(otmLoad(f.data(), uint32_t(f.size()), kCore, OtmBlock{a, 24, kFb}, OtmBlock{b, 16 + 8 - 1, kMb}, 8,
                      nullptr) == OtmError::TooBig);   // main 12, tail 8 at 16
    }
    {
        Built2 x;
        x.cuts = {8, 600000, 12, 900000};   // the last cut is not the row's end
        CHECK(load(x.file(), b, sizeof(b), 0) == OtmError::BadMagic);
    }
    {
        Built x;
        x.fixups.push_back(0u | kOtmFixFast << 24);   // a row fixup without a row
        CHECK(load(x.file(), b, sizeof(b), 0) == OtmError::BadFixup);
    }
}

// tools/make_otm.py --reference: the module linked by ld at kRefBase with
// export k at fake_export(k); otmLoad at that base must give the same bytes.
TEST(real_module_matches_the_linker) {
    const char* dir = std::getenv("OT_OTM_DIR");
    if (!dir) {
        std::printf("  (skipped: OT_OTM_DIR not set; make otm OTM_MODULE=... OTM_REF=0x1fff8000)\n");
        return;
    }
    const std::vector<uint8_t> f = readFile(std::string(dir) + "/tape.otm");
    const std::vector<uint8_t> ref = readFile(std::string(dir) + "/tape.ref.bin");
    if (f.empty() || ref.empty()) {
        std::printf("  (skipped: no tape.otm / tape.ref.bin in %s)\n", dir);
        return;
    }
    OtmHeader h;
    std::memcpy(&h, f.data(), sizeof(h));
    std::vector<uint32_t> exports(kOtmMaxExports);
    for (uint32_t k = 1; k <= kOtmMaxExports; ++k) exports[k - 1] = 0x0E000000u + 16u * k + 1u;   // make_otm.py fake_export
    const OtmCore core = {h.api, h.layoutHash, h.abiFlags, exports.data(), kOtmMaxExports};
    std::vector<uint8_t> dst(ref.size() + 64, 0x55);
    uint32_t entry = 0;
    CHECK(otmLoad(f.data(), uint32_t(f.size()), core, dst.data(), uint32_t(dst.size()), 0x1FFF8000u, &entry) ==
          OtmError::None);
    CHECK_EQ(h.imageBytes + h.bssBytes, uint32_t(ref.size()));
    CHECK(std::memcmp(dst.data(), ref.data(), ref.size()) == 0);
    CHECK_EQ(entry & 1u, 1u);   // a Thumb function
}

// The same with a row cut in the middle: make otm OTM_MODULE=modules/fm4
// OTM_SUFFIX=h OTM_HOT=<fm4.hot> OTM_REF=0x20010000 OTM_REF_FAST=0x1fffc000
// OTM_REF_CUT=<a cut> (the fast reference's size is the cut).
TEST(real_two_part_module_matches_the_linker) {
    const char* dir = std::getenv("OT_OTM_DIR");
    if (!dir) return;
    const std::vector<uint8_t> f = readFile(std::string(dir) + "/fm4h.otm");
    const std::vector<uint8_t> ref = readFile(std::string(dir) + "/fm4h.ref.bin");
    const std::vector<uint8_t> refFast = readFile(std::string(dir) + "/fm4h.ref-fast.bin");
    if (f.empty() || ref.empty() || refFast.empty()) {
        std::printf("  (skipped: no fm4h.otm / .ref.bin / .ref-fast.bin in %s)\n", dir);
        return;
    }
    OtmHeader h;
    std::memcpy(&h, f.data(), sizeof(h));
    const uint32_t cut = uint32_t(refFast.size());
    CHECK(cut > 0 && cut <= h.rowBytes);
    CHECK_EQ(otmCutFor(f.data(), h, cut), cut);
    std::vector<uint32_t> exports(kOtmMaxExports);
    for (uint32_t k = 1; k <= kOtmMaxExports; ++k) exports[k - 1] = 0x0E000000u + 16u * k + 1u;
    const OtmCore core = {h.api, h.layoutHash, h.abiFlags, exports.data(), kOtmMaxExports};
    std::vector<uint8_t> fast(refFast.size(), 0x55), mainb(ref.size() + 64, 0x55);
    uint32_t entry = 0;
    CHECK(otmLoad(f.data(), uint32_t(f.size()), core, OtmBlock{fast.data(), uint32_t(fast.size()), 0x1FFFC000u},
                  OtmBlock{mainb.data(), uint32_t(mainb.size()), 0x20010000u}, cut, &entry) == OtmError::None);
    CHECK_EQ(otmMainSpan(h, cut), uint32_t(ref.size()));
    CHECK(std::memcmp(mainb.data(), ref.data(), ref.size()) == 0);
    CHECK(std::memcmp(fast.data(), refFast.data(), refFast.size()) == 0);
    CHECK_EQ(entry & 1u, 1u);
}

// ---- Modules/*.otm from the card (modules/module_files.h) -------------------
namespace {

// Stand-ins for the code in the pool: enter() maps the entry address to one.
struct FakeModule : IModule {
    ModuleInfo in;
    explicit FakeModule(const char* id) : in{{id[0], id[1], id[2], id[3], 0}, kModuleApi, 100, "Fake", 0, 0, 0, 0, 0, 0} {}
    const ModuleInfo& info() const override { return in; }
    bool attach(ModuleHost&) override { return true; }
};
FakeModule gLdA("LDA1"), gLdB("LDB1"), gLdDup("LDA1");
uint8_t gPoolMem[96];
uint32_t gSynced = 0;
uint32_t gEntries[8];
int gNEntries = 0;

IModule& fakeEnter(uint32_t entry) {
    if (gNEntries < 8) gEntries[gNEntries++] = entry;
    const uint32_t off = entry - 0x1FFF8000u;
    return off < 24 ? static_cast<IModule&>(gLdA) : off < 48 ? static_cast<IModule&>(gLdB) : gLdDup;
}
void fakeSync(const void*, uint32_t len) { gSynced += len; }
// The device's stack guard move (hal/stack_guard.h setModulePoolBytes).
uint32_t gSizes[4];
int gNSizes = 0;
bool gResizeOk = true;
bool fakeResize(uint32_t bytes) {
    if (gNSizes < 4) gSizes[gNSizes++] = bytes;
    return gResizeOk;
}

}  // namespace

TEST(card_modules_load_in_name_order) {
    MemStorage fs;
    Built a;   // 16 B image + 8 B bss, align 8
    fs.files["Modules/b.otm"] = a.file();
    fs.files["Modules/a.otm"] = a.file();
    std::vector<uint8_t> bad = a.file();
    bad[kOtmHeaderBytes] ^= 1;
    fs.files["Modules/c.otm"] = bad;                // damaged
    Built other;
    other.h.api = 2;
    fs.files["Modules/d.otm"] = other.file();      // another core
    fs.files["Modules/e.otm"] = a.file();           // lands at 48: the duplicate id
    fs.files["Modules/readme.txt"] = {1, 2, 3};    // not a module
    const OtmPool pool = {gPoolMem, 0x1FFF8000u, sizeof(gPoolMem), kCore, &fakeSync, &fakeEnter, &fakeResize};
    uint8_t scratch[256];
    OtmLoadReport r;
    CHECK_EQ(loadModuleFiles(fs, pool, scratch, sizeof(scratch), &r), 2);
    CHECK_EQ(r.n, 5);
    CHECK_STR(r.items[0].file, "a.otm");
    CHECK(r.items[0].error == OtmError::None && !r.items[0].refused);
    CHECK_STR(r.items[1].file, "b.otm");
    CHECK(r.items[1].error == OtmError::None && !r.items[1].refused);
    CHECK(r.items[2].error == OtmError::BadCrc);
    CHECK(r.items[3].error == OtmError::Api);
    CHECK(r.items[4].error == OtmError::None && r.items[4].refused);   // LDA1 again
    CHECK_EQ(r.used, 48u);                       // two modules of 24 B, back to back (align 8)
    // Each file is entered once at the pool's start to see what it is, then
    // the ones that stay are loaded back to back.
    CHECK_EQ(gNEntries, 6);
    CHECK_EQ(gEntries[0], 0x1FFF8001u);
    CHECK_EQ(gEntries[2], 0x1FFF8001u);
    CHECK_EQ(gEntries[3], 0x1FFF8001u);
    CHECK_EQ(gEntries[4], 0x1FFF8019u);          // 24 + entryOffset 1
    CHECK_EQ(gSynced, 144u);
    CHECK_EQ(word(gPoolMem + 24), 0x1FFF8020u);  // b's first word: its own base + 8
    CHECK(ModuleHost::instance().find("LDA1") == &gLdA);
    CHECK(ModuleHost::instance().find("LDB1") == &gLdB);
    CHECK_STR(ModuleHost::instance().refused(), "LDA1");
    // The whole block while loading, then only what the modules took.
    CHECK_EQ(gNSizes, 2);
    CHECK_EQ(gSizes[0], uint32_t(sizeof(gPoolMem)));
    CHECK_EQ(gSizes[1], 48u);
}

TEST(card_modules_wait_when_the_stack_has_no_room) {
    MemStorage fs;
    fs.files["Modules/a.otm"] = Built().file();
    const OtmPool pool = {gPoolMem, 0x1FFF8000u, sizeof(gPoolMem), kCore, &fakeSync, &fakeEnter, &fakeResize};
    uint8_t scratch[256];
    OtmLoadReport r;
    gNSizes = 0;
    gResizeOk = false;
    CHECK_EQ(loadModuleFiles(fs, pool, scratch, sizeof(scratch), &r), 0);
    CHECK_EQ(r.n, 0);
    CHECK(r.noRoom);
    CHECK_EQ(gNSizes, 1);
    gResizeOk = true;
}

TEST(card_modules_without_a_pool_or_folder) {
    MemStorage fs;
    uint8_t scratch[64];
    OtmLoadReport r;
    const OtmPool none = {nullptr, 0, 0, kCore, nullptr, nullptr, nullptr};
    CHECK_EQ(loadModuleFiles(fs, none, scratch, sizeof(scratch), &r), 0);
    const OtmPool pool = {gPoolMem, 0x1FFF8000u, sizeof(gPoolMem), kCore, &fakeSync, &fakeEnter, nullptr};
    CHECK_EQ(loadModuleFiles(fs, pool, scratch, sizeof(scratch), &r), 0);   // no Modules/
    CHECK_EQ(r.n, 0);
    fs.files["Modules/big.otm"] = Built().file();
    CHECK_EQ(loadModuleFiles(fs, pool, scratch, 40, &r), 0);   // the file is bigger than the scratch
    CHECK(r.items[0].error == OtmError::TooBig);
}

// A card whose directory lists files in reverse name order.
struct ReverseStorage : MemStorage {
    int32_t listDir(const char* path, DirFn fn, void* ctx) override {
        const std::string prefix = std::string(path) + "/";
        int32_t n = 0;
        for (auto it = files.rbegin(); it != files.rend(); ++it) {
            if (it->first.compare(0, prefix.size(), prefix) != 0) continue;
            const std::string rest = it->first.substr(prefix.size());
            DirEntry e{rest.c_str(), false, uint32_t(it->second.size())};
            ++n;
            if (!fn(e, ctx)) break;
        }
        return n;
    }
};

TEST(card_modules_past_the_first_kmax_by_name) {
    ReverseStorage fs;
    Built a;
    // kMax + 2 files, listed in reverse name order: the first kMax by name
    // are loaded whatever the directory order, the other two counted.
    for (int i = OtmLoadReport::kMax + 1; i >= 0; --i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Modules/m%02d.otm", i);
        fs.files[name] = a.file();
    }
    const OtmPool pool = {gPoolMem, 0x1FFF8000u, sizeof(gPoolMem), kCore, &fakeSync, &fakeEnter, nullptr};
    uint8_t scratch[256];
    OtmLoadReport r;
    loadModuleFiles(fs, pool, scratch, sizeof(scratch), &r);
    CHECK_EQ(r.n, OtmLoadReport::kMax);
    CHECK_EQ(r.skipped, 2);
    for (int i = 0; i < r.n; ++i) {
        char want[24];
        std::snprintf(want, sizeof(want), "m%02d.otm", i);
        CHECK_STR(r.items[i].file, want);
    }
}

TEST(card_modules_with_no_pool_room_say_so) {
    MemStorage fs;
    fs.files["Modules/a.otm"] = Built().file();
    const OtmPool pool = {gPoolMem, 0x1FFF8000u, 0, kCore, &fakeSync, &fakeEnter, &fakeResize};
    uint8_t scratch[256];
    OtmLoadReport r;
    CHECK_EQ(loadModuleFiles(fs, pool, scratch, sizeof(scratch), &r), 0);
    CHECK(r.noRoom);
    MemStorage empty;
    CHECK_EQ(loadModuleFiles(empty, pool, scratch, sizeof(scratch), &r), 0);
    CHECK(!r.noRoom);   // no files: nothing to say
}

TEST(fast_block_shared_by_turns) {
    OtmShareCut cuts[3][kOtmMaxShareCuts] = {{1000, 2000, 3000, 9000}, {500, 4000}, {200, 400, 600}};
    const uint32_t n[3] = {4, 2, 3};
    uint32_t share[3];
    // Turns: 1000 + 500 + 200, then 2000 + 4000 + 400, then 3000 + 600 = 7600;
    // the first module's 9000 doesn't fit the rest.
    shareFastBlock(cuts, n, 3, 8192, share);
    CHECK_EQ(share[0], 3000u);
    CHECK_EQ(share[1], 4000u);
    CHECK_EQ(share[2], 600u);
    // A small block: 1000 + 500 + 200 = 1700, then 2000 doesn't fit, 4000
    // doesn't, 400 does (1900).
    shareFastBlock(cuts, n, 3, 2000, share);
    CHECK_EQ(share[0], 1000u);
    CHECK_EQ(share[1], 500u);
    CHECK_EQ(share[2], 400u);
    // One module alone takes its whole row.
    shareFastBlock(cuts, n, 1, 12288, share);
    CHECK_EQ(share[0], 9000u);
    shareFastBlock(cuts, n, 3, 0, share);
    CHECK_EQ(share[0] + share[1] + share[2], 0u);
}

namespace {
FakeModule gRowA("RWA1"), gRowB("RWB1");
// Entered as: probe a, probe b, load a, load b.
int gRowEntered = 0;
IModule& rowEnter(uint32_t) { return gRowEntered++ % 2 ? static_cast<IModule&>(gRowB) : gRowA; }
}  // namespace

TEST(card_modules_share_the_fast_block) {
    MemStorage fs;
    fs.files["Modules/a.otm"] = Built2().file();   // row cuts 8, 16
    fs.files["Modules/b.otm"] = Built2().file();
    static uint8_t mem[128], fastMem[24];
    std::memset(fastMem, 0x55, sizeof(fastMem));
    OtmPool pool = {mem, 0x20010000u, sizeof(mem), kCore, nullptr, &rowEnter, nullptr};
    pool.fastMem = fastMem;
    pool.fastBase = kFb;
    pool.fastCap = sizeof(fastMem);
    uint8_t scratch[256];
    OtmLoadReport r;
    CHECK_EQ(loadModuleFiles(fs, pool, scratch, sizeof(scratch), &r), 2);
    // Turns in 24 B: a 8, b 8, a 16; b's 16 doesn't fit.
    CHECK_EQ(r.items[0].fastBytes, 16u);
    CHECK_EQ(r.items[1].fastBytes, 8u);
    CHECK_EQ(r.items[0].rowBytes, 16u);
    CHECK_EQ(r.fastUsed, 24u);
    // a: main 12 B (no tail); b at the next multiple of 8 (16): main 12, its 8-byte tail at 16.
    CHECK_EQ(r.used, 16u + 16u + 8u);
    CHECK_EQ(word(fastMem + 8), kFb + 1);             // a's row function 8 points at a's row 0
    CHECK_EQ(word(fastMem + 16 + 0), 0x20010000u + 16u + 5u);   // b's row 0: b's main part + 5
    CHECK_EQ(word(mem + 16), 0x20010000u + 16u + 16u + 1u);     // b's main: its row 8, in its tail
}

// ---- card synths: listed at boot, loaded when wanted ----------------------
namespace {

const PatchParamRange kFkRange[1] = {{0.5f, 0.0f, 1.0f}};
const synthui::ParamUi kFkUi[1] = {{"Level", synthui::kFmtPct, nullptr, 0}};

// A synth module stand-in: one engine.
struct FakeSynth : FakeModule {
    SynthDescriptor d;
    int attached = 0;
    FakeSynth(const char* id, int32_t engine, const char* name)
        : FakeModule(id), d{engine, name, 0, 0, 1, kFkRange, kFkUi, nullptr, 0, 0, nullptr} {
        in.caps = kCapSynthEngine | kCapSynthDescriptor;
    }
    const SynthDescriptor* synthDescriptor(int i) const override { return i == 0 ? &d : nullptr; }
    bool attach(ModuleHost&) override {
        ++attached;
        return true;
    }
};
FakeSynth gSyA("SYA1", 0xE1, "FAKEA"), gSyB("SYB1", 0xE2, "FAKEB");
FakeModule gFxA("FXA1");

// The file's tag (image word 2) names the module; probes all land at the
// pool's start, so the address can't.
uint8_t gSyPool[256];
IModule& synthEnter(uint32_t entry) {
    const uint8_t* m = gSyPool + (entry - 1 - 0x20020000u);
    switch (word(m + 8)) {
        case 1: return gSyA;
        case 2: return gSyB;
        default: return gFxA;
    }
}
std::vector<uint8_t> tagged(uint32_t tag, uint32_t imageBytes = 16) {
    Built b;
    b.image.resize(imageBytes, 0);
    for (int i = 0; i < 4; ++i) b.image[8 + i] = uint8_t(tag >> (8 * i));
    return b.file();
}

}  // namespace

TEST(card_synths_listed_at_boot_and_loaded_when_wanted) {
    MemStorage fs;
    fs.files["Modules/fx.otm"] = tagged(0);
    fs.files["Modules/sa.otm"] = tagged(1);
    fs.files["Modules/sb.otm"] = tagged(2);
    const OtmPool pool = {gSyPool, 0x20020000u, sizeof(gSyPool), kCore, nullptr, &synthEnter, nullptr};
    uint8_t scratch[256];
    OtmLoadReport r;
    ModuleHost& h = ModuleHost::instance();
    CHECK_EQ(loadModuleFiles(fs, pool, scratch, sizeof(scratch), &r), 1);   // only the effect stays
    CHECK(h.find("FXA1") == &gFxA);
    CHECK(!h.find("SYA1") && !h.find("SYB1"));
    CHECK_EQ(gSyA.attached + gSyB.attached, 0);
    CHECK_EQ(r.used, 24u);
    CHECK(r.items[1].synth && !r.items[1].loaded);
    // Both engines are listed, neither loaded.
    CHECK_EQ(cardEngineCount(), 2);
    CHECK_STR(cardEngineAt(0)->name, "FAKEA");
    CHECK_EQ(cardEngine(0xE2)->file, 2);
    CHECK(!cardEngineLoaded(0xE1));
    CHECK(!h.synthDescriptor(0xE1));
    // Slots naming 0xE2 and a built-in engine: only B loads, after the effect.
    int32_t want[3] = {1, 0xE2, 0xE2};
    CHECK(cardSynthsDiffer(want, 3));
    CHECK(loadCardSynths(fs, want, 3, scratch, sizeof(scratch)));
    CHECK(cardEngineLoaded(0xE2) && !cardEngineLoaded(0xE1));
    CHECK(h.synthDescriptor(0xE2) == &gSyB.d);
    CHECK_EQ(gSyB.attached, 1);
    CHECK_EQ(r.used, 48u);
    CHECK(!cardSynthsDiffer(want, 3));
    CHECK(loadCardSynths(fs, want, 3, scratch, sizeof(scratch)));   // the same set: nothing happens
    CHECK_EQ(gSyB.attached, 1);
    // A new set: B leaves, A loads where B was.
    want[1] = want[2] = 0xE1;
    CHECK(loadCardSynths(fs, want, 3, scratch, sizeof(scratch)));
    CHECK(!h.find("SYB1") && !h.synthDescriptor(0xE2));
    CHECK(h.synthDescriptor(0xE1) == &gSyA.d);
    CHECK_EQ(r.used, 48u);
    // Both, then none.
    want[2] = 0xE2;
    CHECK(loadCardSynths(fs, want, 3, scratch, sizeof(scratch)));
    CHECK(cardEngineLoaded(0xE1) && cardEngineLoaded(0xE2));
    CHECK_EQ(r.used, 72u);
    const int32_t none[3] = {1, 2, 4};
    CHECK(loadCardSynths(fs, none, 3, scratch, sizeof(scratch)));
    CHECK(!h.find("SYA1") && !h.find("SYB1"));
    CHECK_EQ(r.used, 24u);
    CHECK(h.find("FXA1") == &gFxA);
    h.removeLoaded(gFxA);   // (the host is shared by the tests)
}

TEST(card_synths_first_wanted_gets_the_room) {
    MemStorage fs;
    fs.files["Modules/sa.otm"] = tagged(1, 96);   // 96 + 8 bss = 104 B each
    fs.files["Modules/sb.otm"] = tagged(2, 96);
    const OtmPool pool = {gSyPool, 0x20020000u, 160, kCore, nullptr, &synthEnter, nullptr};
    uint8_t scratch[256];
    OtmLoadReport r;
    CHECK_EQ(loadModuleFiles(fs, pool, scratch, sizeof(scratch), &r), 0);
    CHECK_EQ(cardEngineCount(), 2);
    const int32_t want[2] = {0xE2, 0xE1};   // B first
    CHECK(!loadCardSynths(fs, want, 2, scratch, sizeof(scratch)));
    CHECK(cardEngineLoaded(0xE2) && !cardEngineLoaded(0xE1));
    CHECK(r.items[0].error == OtmError::TooBig);
    CHECK(!loadCardSynths(fs, want, 2, scratch, sizeof(scratch)));   // still: no retry changes it
    const int32_t none[1] = {0};
    CHECK(loadCardSynths(fs, none, 1, scratch, sizeof(scratch)));
    CHECK(!cardEngineLoaded(0xE2));
}
