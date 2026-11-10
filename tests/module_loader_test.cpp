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
        h = OtmHeader{kOtmMagic, kOtmHeaderBytes, 1, kLayout, kAbi, {'T', 'E', 'S', 'T'}, 16, 8, 8, 1, 3, 3};
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
    std::vector<uint32_t> exports(255);
    for (uint32_t k = 1; k <= 255; ++k) exports[k - 1] = 0x0E000000u + 16u * k + 1u;   // make_otm.py fake_export
    const OtmCore core = {h.api, h.layoutHash, h.abiFlags, exports.data(), 255};
    std::vector<uint8_t> dst(ref.size() + 64, 0x55);
    uint32_t entry = 0;
    CHECK(otmLoad(f.data(), uint32_t(f.size()), core, dst.data(), uint32_t(dst.size()), 0x1FFF8000u, &entry) ==
          OtmError::None);
    CHECK_EQ(h.imageBytes + h.bssBytes, uint32_t(ref.size()));
    CHECK(std::memcmp(dst.data(), ref.data(), ref.size()) == 0);
    CHECK_EQ(entry & 1u, 1u);   // a Thumb function
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
uint32_t gEntries[4];
int gNEntries = 0;

IModule& fakeEnter(uint32_t entry) {
    if (gNEntries < 4) gEntries[gNEntries++] = entry;
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
    CHECK_EQ(gNEntries, 3);
    CHECK_EQ(gEntries[0], 0x1FFF8001u);
    CHECK_EQ(gEntries[1], 0x1FFF8019u);          // 24 + entryOffset 1
    CHECK_EQ(gSynced, 72u);
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

TEST(card_modules_past_the_first_eight_by_name) {
    ReverseStorage fs;
    Built a;
    // Ten files, listed in reverse name order: the first eight by name are
    // loaded whatever the directory order, the other two counted.
    for (int i = 9; i >= 0; --i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Modules/m%d.otm", i);
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
        std::snprintf(want, sizeof(want), "m%d.otm", i);
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
