// Tracker Mini `.patch` codec tests (spec 21 §7/§8, MINI_PLAN §1, WP B).
//
// The factory patches are private vendor content: they are read in place from
// $OT_VENDOR_FIXTURES/mini-patches-2.0/<ENGINE>/*.patch, never copied, and the
// fixture tests skip when the folder is absent.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "formats/patch_pb.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace ot;

namespace {

const int32_t kEngines[] = {kSynthEngineAcd, kSynthEngineFat, kSynthEngineVap, kSynthEngineWtfm, kSynthEnginePerc};

struct VendorFile {
    std::string path;
    int32_t engine;  // from the folder name
    std::vector<uint8_t> bytes;
};

std::vector<VendorFile> vendorPatches() {
    std::vector<VendorFile> out;
    const char* root = std::getenv("OT_VENDOR_FIXTURES");
    if (!root) return out;
    fs::path base = fs::path(root) / "mini-patches-2.0";
    for (int32_t e : kEngines) {
        std::error_code ec;
        fs::path dir = base / patchEngineName(e);
        if (!fs::is_directory(dir, ec)) continue;
        std::vector<fs::path> files;
        for (auto& de : fs::directory_iterator(dir, ec))
            if (de.path().extension() == ".patch") files.push_back(de.path());
        std::sort(files.begin(), files.end());
        for (auto& p : files) {
            std::ifstream f(p, std::ios::binary);
            VendorFile v{p.string(), e,
                         std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>())};
            out.push_back(std::move(v));
        }
    }
    return out;
}

const std::vector<VendorFile>& vendor() {
    static std::vector<VendorFile> v = vendorPatches();
    return v;
}

bool skipVendor() {
    if (vendor().empty()) {
        std::printf("  SKIP: no vendor patches (OT_VENDOR_FIXTURES=%s)\n",
                    std::getenv("OT_VENDOR_FIXTURES") ? std::getenv("OT_VENDOR_FIXTURES") : "unset");
        return true;
    }
    return false;
}

std::vector<uint8_t> encode(const SynthPatch& p) {
    std::vector<uint8_t> buf(kPatchMaxFileSize);
    size_t n = 0;
    PatchStatus s = encodePatch(p, buf.data(), buf.size(), &n);
    if (s != PatchStatus::kOk) return {};
    buf.resize(n);
    return buf;
}

// Field-wise (struct padding is not compared).
bool samePatch(const SynthPatch& a, const SynthPatch& b) {
    if (a.engine != b.engine || a.paramCount != b.paramCount || a.extraPresent != b.extraPresent ||
        a.opaqueLen != b.opaqueLen || std::memcmp(a.opaque, b.opaque, a.opaqueLen) != 0)
        return false;
    for (int i = 0; i < a.paramCount; ++i)  // entries past paramCount are not part of the patch
        if (a.params[i].bits != b.params[i].bits || a.params[i].present != b.params[i].present) return false;
    for (int i = 0; i < kPatchNameSlots; ++i)
        if (a.names[i].len != b.names[i].len || std::memcmp(a.names[i].text, b.names[i].text, a.names[i].len) != 0)
            return false;
    for (int k = 0; k < kPatchMacros; ++k) {
        const PatchMacro &x = a.macros[k], &y = b.macros[k];
        if (x.nameLen != y.nameLen || std::memcmp(x.name, y.name, x.nameLen) != 0 || x.bipolar != y.bipolar ||
            x.count != y.count)
            return false;
        for (int j = 0; j < kPatchMacroTargets; ++j)
            if (x.targets[j].param != y.targets[j].param || x.targets[j].amountBits != y.targets[j].amountBits)
                return false;
    }
    return true;
}

std::string macroName(const PatchMacro& m) { return std::string(m.name, m.nameLen); }

// Offsets of top-level field boundaries of a well-formed buffer.
std::vector<size_t> fieldBoundaries(const std::vector<uint8_t>& b) {
    std::vector<size_t> out{0};
    size_t i = 0;
    auto varint = [&](uint64_t& v) {
        v = 0;
        for (int s = 0;; s += 7) {
            uint8_t c = b[i++];
            v |= uint64_t(c & 0x7F) << s;
            if (!(c & 0x80)) break;
        }
    };
    while (i < b.size()) {
        uint64_t t, v;
        varint(t);
        switch (t & 7) {
            case 0: varint(v); break;
            case 1: i += 8; break;
            case 5: i += 4; break;
            case 2: varint(v); i += v; break;
        }
        out.push_back(i);
    }
    return out;
}

void printIssues(const SynthPatch& p, const char* what) {
    PatchIssue is[16];
    int n = validatePatch(p, is, 16);
    for (int i = 0; i < n && i < 16; ++i) {
        float v = is[i].kind == PatchIssue::kParamRange ? patchBitsToFloat(p.params[is[i].index].bits) : 0.0f;
        std::printf("       %s: issue kind %d index %d sub %d value %g\n", what, is[i].kind, is[i].index, is[i].sub, v);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

TEST(vendor_roundtrip_byte_exact) {
    if (skipVendor()) return;
    int ok = 0;
    for (const VendorFile& v : vendor()) {
        SynthPatch p;
        PatchStatus s = decodePatch(v.bytes.data(), v.bytes.size(), &p);
        if (s != PatchStatus::kOk) {
            std::printf("       %s: %s\n", v.path.c_str(), patchStatusText(s));
            CHECK(false);
            continue;
        }
        std::vector<uint8_t> out = encode(p);
        bool same = out == v.bytes;
        if (!same) std::printf("       %s: re-encode differs (%zu vs %zu B)\n", v.path.c_str(), out.size(), v.bytes.size());
        CHECK(same);
        ok += same;
    }
    std::printf("  %d / %zu vendor patches byte-exact\n", ok, vendor().size());
    CHECK_EQ(int(vendor().size()), 299);
}

TEST(vendor_invariants_and_ranges) {
    if (skipVendor()) return;
    int withOpaque = 0;
    for (const VendorFile& v : vendor()) {
        SynthPatch p;
        if (decodePatch(v.bytes.data(), v.bytes.size(), &p) != PatchStatus::kOk) {
            CHECK(false);
            continue;
        }
        int n = 0;
        patchEngineParams(v.engine, &n);
        CHECK_EQ(p.engine, v.engine);
        CHECK_EQ(int(p.paramCount), n);
        CHECK(p.extraPresent);
        CHECK(v.bytes.size() <= kPatchMaxFileSize);
        CHECK(macroName(p.macros[kPatchMacroVelocity]) == "Velocity");
        CHECK(macroName(p.macros[kPatchMacroPressure]) == "Pressure");
        for (const PatchMacro& m : p.macros) {
            CHECK(m.count >= 0 && m.count <= 5);
            // Targets beyond the count are empty messages {0, 0} (spec 21 §7.2 says -1).
            for (int j = m.count < 0 ? 0 : m.count; j < kPatchMacroTargets; ++j) {
                CHECK_EQ(m.targets[j].param, 0);
                CHECK_EQ(m.targets[j].amountBits, 0u);
            }
            // Live targets: a parameter index, or -1 for an empty slot inside the count
            // (factory files keep a stale amount there).
            for (int j = 0; j < m.count && j < kPatchMacroTargets; ++j)
                CHECK(m.targets[j].param >= -1 && m.targets[j].param < n);
        }
        // Every entry present except WTFM 6/7, which are absent in every file (U6).
        for (int i = 0; i < p.paramCount; ++i)
            CHECK_EQ(p.params[i].present, !(v.engine == kSynthEngineWtfm && (i == 6 || i == 7)));
        int issues = validatePatch(p, nullptr, 0);
        if (issues) printIssues(p, v.path.c_str());
        CHECK_EQ(issues, 0);
        withOpaque += p.opaqueLen > 0;
    }
    // 59 files carry fields 2-5, 32 carry 10 (x8) + 11; 89 in total.
    std::printf("  %d vendor patches carry unknown fields (kept raw)\n", withOpaque);
    CHECK_EQ(withOpaque, 89);
}

TEST(vendor_truncation_every_offset) {
    if (skipVendor()) return;
    // Every 7th file in full keeps the run time low; the rest share the same shapes.
    for (size_t k = 0; k < vendor().size(); k += 7) {
        const std::vector<uint8_t>& b = vendor()[k].bytes;
        std::vector<size_t> bounds = fieldBoundaries(b);
        for (size_t cut = 0; cut < b.size(); ++cut) {
            SynthPatch p;
            PatchStatus s = decodePatch(b.data(), cut, &p);
            bool boundary = std::binary_search(bounds.begin(), bounds.end(), cut);
            if (boundary) CHECK(s == PatchStatus::kOk);
            else CHECK(s == PatchStatus::kTruncated);
        }
    }
}

// ---------------------------------------------------------------------------
// Init patches
// ---------------------------------------------------------------------------

TEST(init_patch_per_engine) {
    for (int32_t e : kEngines) {
        SynthPatch p;
        CHECK(patchInit(e, &p));
        int n = 0;
        const PatchParamRange* t = patchEngineParams(e, &n);
        CHECK(t != nullptr);
        CHECK_EQ(int(p.paramCount), n);
        CHECK_EQ(validatePatch(p, nullptr, 0), 0);
        CHECK(macroName(p.macros[0]) == "Macro 1");
        CHECK(macroName(p.macros[5]) == "Macro 6");
        CHECK(macroName(p.macros[6]) == "Velocity");
        CHECK(macroName(p.macros[7]) == "Pressure");
        int vol = patchVolumeParam(e);
        if (vol >= 0) {
            CHECK_EQ(p.macros[6].count, 1);
            CHECK_EQ(p.macros[6].targets[0].param, vol);
            CHECK(patchBitsToFloat(p.macros[6].targets[0].amountBits) == 1.0f);
        }
        for (int i = 0; i < n; ++i) {
            if (t[i].flags & kParamFlagInitAbsent) {
                CHECK(!p.params[i].present);
            } else {
                CHECK(p.params[i].present);
                CHECK(patchBitsToFloat(p.params[i].bits) == t[i].def);
                CHECK(patchParamInRange(e, i, t[i].def));
            }
        }
        std::vector<uint8_t> b = encode(p);
        CHECK(!b.empty() && b.size() <= kPatchMaxFileSize);
        SynthPatch q;
        CHECK(decodePatch(b.data(), b.size(), &q) == PatchStatus::kOk);
        CHECK(samePatch(p, q));
        CHECK(encode(q) == b);
    }
    SynthPatch p;
    CHECK(!patchInit(3, &p));
    CHECK(!patchInit(0, &p));
}

TEST(engine_param_counts) {
    const int want[] = {29, 25, 60, 63, 84};
    for (int i = 0; i < 5; ++i) {
        int n = 0;
        patchEngineParams(kEngines[i], &n);
        CHECK_EQ(n, want[i]);
    }
    int n = 7;
    CHECK(patchEngineParams(3, &n) == nullptr);
    CHECK_EQ(n, 0);
}

// ---------------------------------------------------------------------------
// Wire format units
// ---------------------------------------------------------------------------

TEST(extra_constant_block) {
    uint8_t e[kPatchExtraSize];
    patchExtraPayload(e);
    CHECK_EQ(kPatchExtraSize, size_t(1293));
    CHECK_BYTES(e, 0x0A, 0x08, 0x12, 0x04, 0x08, 0x03, 0x10, 0x01, 0x1A, 0x00, 0x12, 0x80, 0x0A);
    CHECK_BYTES(e + 13, 0x12, 0x12, 0x12, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x12, 0x12);
    CHECK_BYTES(e + kPatchExtraSize - 20, 0x12, 0x12, 0x12, 0x10);
}

TEST(proto3_omission_and_negative_int32) {
    SynthPatch p;
    patchInit(kSynthEngineAcd, &p);
    p.paramCount = 2;
    p.params[0] = {0, true};   // present zero: still written
    p.params[1] = {0, false};  // absent: empty sub-message
    std::vector<uint8_t> b = encode(p);
    CHECK_BYTES(b.data(), 0x08, 0x01, 0x32, 0x05, 0x0D, 0, 0, 0, 0, 0x32, 0x00);
    // names[4]: four empty sub-messages
    CHECK_BYTES(b.data() + 11, 0x3A, 0x00, 0x3A, 0x00, 0x3A, 0x00, 0x3A, 0x00);
    // macro 1: name "Macro 1", no bipolar/count, 6 empty targets {0, 0}
    CHECK_BYTES(b.data() + 19, 0x42, 9 + 6 * 2, 0x0A, 7, 'M', 'a', 'c', 'r', 'o', ' ', '1', 0x22, 0x00, 0x22, 0x00);
    // A -1 target is a 10-byte varint.
    p.macros[0].targets[5].param = -1;
    b = encode(p);
    CHECK_BYTES(b.data() + 19, 0x42, 9 + 5 * 2 + 13);
    CHECK_BYTES(b.data() + 19 + 2 + 9 + 10, 0x22, 11, 0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01);
    SynthPatch q;
    CHECK(decodePatch(b.data(), b.size(), &q) == PatchStatus::kOk);
    CHECK(samePatch(p, q));
    CHECK_EQ(q.macros[0].targets[5].param, -1);

    // Engine 0 and a zero param index are omitted.
    SynthPatch z;
    patchInit(kSynthEngineAcd, &z);
    z.engine = 0;
    z.macros[2].targets[0] = {0, patchFloatToBits(-0.5f)};
    z.macros[2].bipolar = true;
    z.macros[2].count = 1;
    std::vector<uint8_t> zb = encode(z);
    CHECK(zb[0] == 0x32);
    SynthPatch zq;
    CHECK(decodePatch(zb.data(), zb.size(), &zq) == PatchStatus::kOk);
    CHECK(samePatch(z, zq));
}

TEST(unknown_fields_kept_in_order) {
    SynthPatch p;
    patchInit(kSynthEngineVap, &p);
    std::vector<uint8_t> base = encode(p);
    // Insert field 3 (varint 7) after field 1, and fields 10 (len 2), 11 (varint) after Extra.
    std::vector<uint8_t> b(base.begin(), base.begin() + 2);
    const uint8_t f3[] = {0x18, 0x07};
    const uint8_t f2[] = {0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
    b.insert(b.end(), f3, f3 + 2);
    b.insert(b.end(), f2, f2 + 6);  // out of order on purpose: field 3 then 2
    b.insert(b.end(), base.begin() + 2, base.end());
    const uint8_t tail[] = {0x52, 0x02, 0x08, 0x01, 0x52, 0x00, 0x58, 0x2A, 0x61, 1, 2, 3, 4, 5, 6, 7, 8};
    b.insert(b.end(), tail, tail + sizeof(tail));
    SynthPatch q;
    CHECK(decodePatch(b.data(), b.size(), &q) == PatchStatus::kOk);
    CHECK_EQ(int(q.opaqueLen), 2 + 6 + int(sizeof(tail)));
    CHECK(encode(q) == b);
}

TEST(error_cases) {
    SynthPatch p;
    // Too large.
    std::vector<uint8_t> big(kPatchMaxFileSize + 1, 0);
    CHECK(decodePatch(big.data(), big.size(), &p) == PatchStatus::kTooLarge);
    // Empty input: valid empty message.
    CHECK(decodePatch(nullptr, 0, &p) == PatchStatus::kOk);
    CHECK_EQ(p.macros[0].targets[0].param, 0);
    // Field 0.
    const uint8_t f0[] = {0x00, 0x01};
    CHECK(decodePatch(f0, sizeof(f0), &p) == PatchStatus::kBadField);
    // Group wire type.
    const uint8_t grp[] = {0x0B};
    CHECK(decodePatch(grp, sizeof(grp), &p) == PatchStatus::kBadWireType);
    // Known field, wrong wire type.
    const uint8_t wwt[] = {0x0D, 1, 2, 3, 4};
    CHECK(decodePatch(wwt, sizeof(wwt), &p) == PatchStatus::kBadField);
    // Overlong varint.
    const uint8_t ov[] = {0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
    CHECK(decodePatch(ov, sizeof(ov), &p) == PatchStatus::kBadVarint);
    // int32 out of range (2^31).
    const uint8_t big32[] = {0x08, 0x80, 0x80, 0x80, 0x80, 0x08};
    CHECK(decodePatch(big32, sizeof(big32), &p) == PatchStatus::kBadValue);
    // Length past end.
    const uint8_t lpe[] = {0x32, 0x05, 0x0D, 0x00};
    CHECK(decodePatch(lpe, sizeof(lpe), &p) == PatchStatus::kTruncated);
    // 85 params.
    std::vector<uint8_t> many;
    for (int i = 0; i < 85; ++i) many.insert(many.end(), {0x32, 0x00});
    CHECK(decodePatch(many.data(), many.size(), &p) == PatchStatus::kTooManyParams);
    // 5 names, 9 macros, 7 targets.
    std::vector<uint8_t> nm;
    for (int i = 0; i < 5; ++i) nm.insert(nm.end(), {0x3A, 0x00});
    CHECK(decodePatch(nm.data(), nm.size(), &p) == PatchStatus::kTooManyEntries);
    std::vector<uint8_t> mc;
    for (int i = 0; i < 9; ++i) mc.insert(mc.end(), {0x42, 0x00});
    CHECK(decodePatch(mc.data(), mc.size(), &p) == PatchStatus::kTooManyEntries);
    std::vector<uint8_t> tg{0x42, 14};
    for (int i = 0; i < 7; ++i) tg.insert(tg.end(), {0x22, 0x00});
    CHECK(decodePatch(tg.data(), tg.size(), &p) == PatchStatus::kTooManyEntries);
    // Name slot of 257 bytes (NameSlot.text <= 256, spec 21 §7.1); macro name of 33 bytes.
    std::vector<uint8_t> ln{0x3A, 0x84, 0x02, 0x0A, 0x81, 0x02};   // 260 = 1 + 2 + 257
    ln.resize(ln.size() + 257, 'a');
    CHECK(decodePatch(ln.data(), ln.size(), &p) == PatchStatus::kNameTooLong);
    // 256 bytes are accepted and re-encode byte-exact (two-byte length varints).
    {
        SynthPatch q;
        CHECK(patchInit(kSynthEngineAcd, &q));
        std::vector<uint8_t> init(kPatchMaxFileSize);
        size_t n0 = 0;
        CHECK(encodePatch(q, init.data(), init.size(), &n0) == PatchStatus::kOk);
        for (int len : {127, 128, 256}) {
            q.names[2].len = uint16_t(len);
            std::memset(q.names[2].text, 'n', size_t(len));
            q.names[2].text[len] = 0;
            std::vector<uint8_t> enc(kPatchMaxFileSize);
            size_t n = 0;
            CHECK(encodePatch(q, enc.data(), enc.size(), &n) == PatchStatus::kOk);
            enc.resize(n);
            CHECK(decodePatch(enc.data(), enc.size(), &p) == PatchStatus::kOk);
            CHECK_EQ(int(p.names[2].len), len);
            CHECK(std::memcmp(p.names[2].text, q.names[2].text, size_t(len)) == 0);
            std::vector<uint8_t> again(kPatchMaxFileSize);
            size_t n2 = 0;
            CHECK(encodePatch(p, again.data(), again.size(), &n2) == PatchStatus::kOk);
            again.resize(n2);
            CHECK(again == enc);
            const size_t inner = len < 128 ? 1u : 2u, outer = 1u + inner + size_t(len) < 128 ? 1u : 2u;
            CHECK_EQ(n, n0 + size_t(len) + inner + outer);   // the empty slot was tag + 0x00
        }
    }
    std::vector<uint8_t> mn{0x42, 35, 0x0A, 33};
    mn.resize(mn.size() + 33, 'b');
    CHECK(decodePatch(mn.data(), mn.size(), &p) == PatchStatus::kNameTooLong);
    // Unknown fields over 256 bytes.
    std::vector<uint8_t> op{0x52, 0x80, 0x02};
    op.resize(op.size() + 256, 0);
    CHECK(decodePatch(op.data(), op.size(), &p) == PatchStatus::kOpaqueOverflow);
    // Encode into a short buffer.
    SynthPatch i;
    patchInit(kSynthEnginePerc, &i);
    uint8_t small[100];
    size_t n = 99;
    CHECK(encodePatch(i, small, sizeof(small), &n) == PatchStatus::kBufferTooSmall);
    CHECK_EQ(n, size_t(0));
    CHECK(std::strcmp(patchStatusText(PatchStatus::kTruncated), "patch truncated") == 0);
}

TEST(validation_ranges) {
    // Half-precision bounds: LFO Frequency min 0.1 is stored as the half 0.0999755859375.
    CHECK(patchParamInRange(kSynthEngineAcd, 23, 0.0999755859375f));
    CHECK(!patchParamInRange(kSynthEngineAcd, 23, 0.09f));
    CHECK(patchParamInRange(kSynthEngineAcd, 9, 20000.0f));
    CHECK(!patchParamInRange(kSynthEngineAcd, 9, 20001.0f));
    CHECK(!patchParamInRange(kSynthEngineAcd, 0, NAN));
    // 29 is ACD's [OT] Width (extension parameter, 0..1); 30 is past the end.
    CHECK(patchParamInRange(kSynthEngineAcd, 29, 0.0f));
    CHECK(!patchParamInRange(kSynthEngineAcd, 29, 1.5f));
    CHECK(!patchParamInRange(kSynthEngineAcd, 30, 0.0f));
    CHECK(!patchParamInRange(3, 0, 0.0f));
    // MINI_PLAN corrections.
    CHECK(patchParamInRange(kSynthEngineVap, 27, 37.5f));
    CHECK(patchParamInRange(kSynthEngineVap, 31, 100.0f));
    CHECK(patchParamInRange(kSynthEngineWtfm, 32, 2.25f));
    CHECK(patchParamInRange(kSynthEngineWtfm, 36, 99.0f));
    CHECK(patchParamInRange(kSynthEnginePerc, 34, 24.0f));
    CHECK(!patchParamInRange(kSynthEnginePerc, 44, 25.0f));
    CHECK(patchParamInRange(kSynthEnginePerc, 1, 36.5f));  // fractional pitch
    CHECK(patchParamInRange(kSynthEngineWtfm, 6, 1234.0f)); // U6: no known range

    SynthPatch p;
    patchInit(kSynthEngineFat, &p);
    p.params[7].bits = patchFloatToBits(5.0f);  // cutoff below 20 Hz
    p.macros[1].count = 7;
    p.macros[2].targets[0] = {25, patchFloatToBits(1.0f)};
    p.macros[3].targets[1] = {3, patchFloatToBits(2.5f)};
    p.paramCount = 24;
    PatchIssue is[8];
    int n = validatePatch(p, is, 8);
    CHECK_EQ(n, 5);
    CHECK(is[0].kind == PatchIssue::kParamCount);
    CHECK(is[1].kind == PatchIssue::kParamRange && is[1].index == 7);
    CHECK(is[2].kind == PatchIssue::kMacroCount && is[2].index == 1);
    CHECK(is[3].kind == PatchIssue::kTargetParam && is[3].index == 2 && is[3].sub == 0);
    CHECK(is[4].kind == PatchIssue::kTargetAmount && is[4].index == 3 && is[4].sub == 1);
    p.engine = 9;
    CHECK(validatePatch(p, nullptr, 0) >= 1);
}

// ---------------------------------------------------------------------------
// Fuzz
// ---------------------------------------------------------------------------

namespace {
uint32_t g_rng = 0x2545F491u;
uint32_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

void checkDecoded(const std::vector<uint8_t>& m) {
    SynthPatch p;
    PatchStatus s = decodePatch(m.data(), m.size(), &p);
    CHECK(uint8_t(s) <= uint8_t(PatchStatus::kOpaqueOverflow));
    if (s != PatchStatus::kOk) return;
    // Accepted input: re-encode must succeed, fit 8 KB and be a fixed point.
    std::vector<uint8_t> e = encode(p);
    CHECK(!e.empty() && e.size() <= kPatchMaxFileSize);
    SynthPatch q;
    CHECK(decodePatch(e.data(), e.size(), &q) == PatchStatus::kOk);
    p.extraPresent = true;  // the writer always emits Extra
    CHECK(samePatch(p, q));
    CHECK(encode(q) == e);
}
}  // namespace

TEST(fuzz_mutations_and_truncation) {
    std::vector<std::vector<uint8_t>> seeds;
    for (int32_t e : kEngines) {
        SynthPatch p;
        patchInit(e, &p);
        p.macros[0].bipolar = true;
        p.macros[0].count = 2;
        p.macros[0].targets[0] = {3, patchFloatToBits(0.75f)};
        p.macros[0].targets[1] = {0, patchFloatToBits(-1.5f)};
        p.names[1].len = 3;
        std::memcpy(p.names[1].text, "abc", 4);
        seeds.push_back(encode(p));
    }
    for (size_t k = 0; k < vendor().size(); k += 13) seeds.push_back(vendor()[k].bytes);

    // Truncation at every offset of the synthetic seeds.
    for (size_t k = 0; k < 5; ++k) {
        const std::vector<uint8_t>& b = seeds[k];
        std::vector<size_t> bounds = fieldBoundaries(b);
        for (size_t cut = 0; cut < b.size(); ++cut) {
            SynthPatch p;
            PatchStatus s = decodePatch(b.data(), cut, &p);
            bool boundary = std::binary_search(bounds.begin(), bounds.end(), cut);
            CHECK(s == (boundary ? PatchStatus::kOk : PatchStatus::kTruncated));
        }
    }

    int accepted = 0;
    const int kIters = 10000;
    for (int it = 0; it < kIters; ++it) {
        std::vector<uint8_t> m = seeds[rnd() % seeds.size()];
        int edits = 1 + int(rnd() % 4);
        for (int e = 0; e < edits && !m.empty(); ++e) {
            size_t pos = rnd() % m.size();
            switch (rnd() % 6) {
                case 0: m[pos] ^= uint8_t(1u << (rnd() % 8)); break;
                case 1: m[pos] = uint8_t(rnd()); break;
                case 2: m.erase(m.begin() + long(pos)); break;
                case 3: m.insert(m.begin() + long(pos), uint8_t(rnd())); break;
                case 4: m.resize(pos); break;
                case 5: {
                    static const uint8_t specials[] = {0x00, 0x7F, 0x80, 0xFF, 0x0B, 0x0F};
                    m[pos] = specials[rnd() % sizeof(specials)];
                    break;
                }
            }
        }
        SynthPatch probe;
        accepted += decodePatch(m.data(), m.size(), &probe) == PatchStatus::kOk;
        checkDecoded(m);
    }
    std::printf("  fuzz: %d / %d mutants accepted (all re-encode to a fixed point)\n", accepted, kIters);
}
