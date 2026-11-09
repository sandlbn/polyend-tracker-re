// OpenTracker preset fields: the name (field 18, `string ot_name`, 1..32
// bytes) and the polyphony (field 19) a synth preset carries, written after the
// preset mix (field 17) and only when set. A patch without them encodes
// byte-exact as before; a field this build cannot hold stays unknown, byte-exact.
#include <cstring>
#include <vector>

#include "formats/patch_pb.h"
#include "test.h"

using namespace ot;

namespace {

std::vector<uint8_t> encode(const SynthPatch& p) {
    std::vector<uint8_t> out(kPatchMaxFileSize);
    size_t n = 0;
    CHECK(encodePatch(p, out.data(), out.size(), &n) == PatchStatus::kOk);
    out.resize(n);
    return out;
}

void nameField(std::vector<uint8_t>& b, const char* s, size_t n) {
    const uint32_t tag = (kPatchPresetNameField << 3) | 2;
    b.push_back(uint8_t((tag & 0x7F) | 0x80));
    b.push_back(uint8_t(tag >> 7));
    b.push_back(uint8_t(n));
    for (size_t k = 0; k < n; ++k) b.push_back(uint8_t(s[k]));
}

}  // namespace

TEST(name_absent_encodes_byte_exact) {
    SynthPatch p;
    CHECK(patchInit(kSynthEngineAcd, &p));
    CHECK_EQ(int(p.presetNameLen), 0);
    const std::vector<uint8_t> b = encode(p);
    size_t beg = 0, end = 0;
    CHECK(!patchFieldSpan(b.data(), b.size(), kPatchPresetNameField, &beg, &end));
    SynthPatch q;
    CHECK(decodePatch(b.data(), b.size(), &q) == PatchStatus::kOk);
    CHECK_EQ(int(q.presetNameLen), 0);
    CHECK(encode(q) == b);
}

TEST(name_roundtrip_after_field17) {
    SynthPatch p;
    CHECK(patchInit(kSynthEngineAcd, &p));
    patchSetMix(p, -3.0f, 0.0f, 10.0f, 20.0f);
    patchSetPresetName(p, "Bell Pad");
    CHECK_STR(p.presetName, "Bell Pad");
    const std::vector<uint8_t> b = encode(p);
    size_t b17 = 0, e17 = 0, b18 = 0, e18 = 0;
    CHECK(patchFieldSpan(b.data(), b.size(), kPatchMixField, &b17, &e17));
    CHECK(patchFieldSpan(b.data(), b.size(), kPatchPresetNameField, &b18, &e18));
    CHECK(b18 >= e17);
    CHECK_EQ(b.size() - b18, size_t(3 + 8));   // tag (2) + length + text, last in the file
    SynthPatch q;
    CHECK(decodePatch(b.data(), b.size(), &q) == PatchStatus::kOk);
    CHECK_EQ(int(q.presetNameLen), 8);
    CHECK_STR(q.presetName, "Bell Pad");
    CHECK_EQ(int(q.opaqueLen), 0);
    CHECK(encode(q) == b);
    // Without the name the same patch is the file it was before.
    SynthPatch r = q;
    patchSetPresetName(r, "");
    std::vector<uint8_t> want(b.begin(), b.begin() + long(b18));
    CHECK(encode(r) == want);
    // patchStripPreset drops the mix and the name together.
    patchStripPreset(q);
    CHECK_EQ(int(q.mixCount), 0);
    CHECK_EQ(int(q.presetNameLen), 0);
}

TEST(a_long_name_is_cut_where_it_is_set) {
    SynthPatch p;
    CHECK(patchInit(kSynthEngineAcd, &p));
    patchSetPresetName(p, "0123456789012345678901234567890123456789");
    CHECK_EQ(int(p.presetNameLen), kPatchPresetNameMax);
    CHECK_EQ(int(std::strlen(p.presetName)), kPatchPresetNameMax);
}

TEST(a_name_this_build_cannot_hold_stays_unknown_and_byte_exact) {
    SynthPatch p;
    CHECK(patchInit(kSynthEngineAcd, &p));
    const std::vector<uint8_t> base = encode(p);
    const char* longName = "0123456789012345678901234567890123456789";
    for (int way = 0; way < 3; ++way) {
        std::vector<uint8_t> b = base;
        if (way == 0) nameField(b, longName, 40);   // too long
        if (way == 1) nameField(b, "", 0);          // empty
        if (way == 2) {                              // a second one
            nameField(b, "One", 3);
            nameField(b, "Two", 3);
        }
        SynthPatch q;
        CHECK(decodePatch(b.data(), b.size(), &q) == PatchStatus::kOk);
        if (way == 2) CHECK_STR(q.presetName, "One");
        else CHECK_EQ(int(q.presetNameLen), 0);
        CHECK(q.opaqueLen > 0);
        CHECK(encode(q) == b);
    }
}

// [OT] The polyphony a preset carries: top-level field 19 (`int32 ot_poly`,
// 0..8), written after field 18 only when set; any other field 19 stays an
// unknown field. Field 17 keeps its 4 entries.
namespace {
void polyField(std::vector<uint8_t>& b, uint8_t v) {
    const uint32_t tag = (kPatchPolyField << 3) | 0;
    b.push_back(uint8_t((tag & 0x7F) | 0x80));
    b.push_back(uint8_t(tag >> 7));
    b.push_back(v);
}
}  // namespace

TEST(poly_absent_encodes_byte_exact) {
    SynthPatch p;
    CHECK(patchInit(kSynthEngineAcd, &p));
    CHECK(!patchHasPoly(p));
    const std::vector<uint8_t> b = encode(p);
    size_t beg = 0, end = 0;
    CHECK(!patchFieldSpan(b.data(), b.size(), kPatchPolyField, &beg, &end));
    SynthPatch q;
    CHECK(decodePatch(b.data(), b.size(), &q) == PatchStatus::kOk);
    CHECK(!patchHasPoly(q));
    CHECK(encode(q) == b);
}

TEST(poly_roundtrip_after_field18_and_mix_stays_four_entries) {
    for (int v : {0, 1, 4, 8}) {
        SynthPatch p;
        CHECK(patchInit(kSynthEngineAcd, &p));
        patchSetMix(p, -3.0f, 0.0f, 10.0f, 20.0f);
        patchSetPresetName(p, "Lead");
        patchSetPoly(p, uint32_t(v));
        const std::vector<uint8_t> b = encode(p);
        size_t b18 = 0, e18 = 0, b19 = 0, e19 = 0, b17 = 0, e17 = 0;
        CHECK(patchFieldSpan(b.data(), b.size(), kPatchPresetNameField, &b18, &e18));
        CHECK(patchFieldSpan(b.data(), b.size(), kPatchPolyField, &b19, &e19));
        CHECK(patchFieldSpan(b.data(), b.size(), kPatchMixField, &b17, &e17));
        CHECK(b19 >= e18);
        CHECK_EQ(b.size() - b19, size_t(3));   // tag (2) + value, last in the file
        SynthPatch q;
        CHECK(decodePatch(b.data(), b.size(), &q) == PatchStatus::kOk);
        CHECK(patchHasPoly(q));
        CHECK_EQ(int(q.poly), v);
        CHECK_EQ(int(q.mixCount), kPatchMixCount);
        CHECK(patchHasMix(q));
        CHECK_EQ(int(q.opaqueLen), 0);
        CHECK(encode(q) == b);
        patchStripPreset(q);
        CHECK(!patchHasPoly(q));
        CHECK(!patchHasMix(q));
        CHECK_EQ(int(q.presetNameLen), 0);
    }
}

TEST(a_poly_this_build_cannot_hold_stays_unknown_and_byte_exact) {
    SynthPatch p;
    CHECK(patchInit(kSynthEngineAcd, &p));
    const std::vector<uint8_t> base = encode(p);
    for (int way = 0; way < 3; ++way) {
        std::vector<uint8_t> b = base;
        if (way == 0) polyField(b, 9);   // out of range
        if (way == 1) {                  // a second one
            polyField(b, 2);
            polyField(b, 3);
        }
        if (way == 2) {                  // not a varint
            const uint32_t tag = (kPatchPolyField << 3) | 2;
            b.push_back(uint8_t((tag & 0x7F) | 0x80));
            b.push_back(uint8_t(tag >> 7));
            b.push_back(1);
            b.push_back(4);
        }
        SynthPatch q;
        CHECK(decodePatch(b.data(), b.size(), &q) == PatchStatus::kOk);
        if (way == 1) CHECK_EQ(int(q.poly), 2);
        else CHECK(!patchHasPoly(q));
        CHECK(q.opaqueLen > 0);
        CHECK(encode(q) == b);
    }
}
