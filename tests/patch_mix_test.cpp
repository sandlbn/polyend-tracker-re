// OpenTracker preset mix: the slot gain, pan and reverb / delay sends a synth
// preset carries, in the patch's own top-level field 17 (`repeated ParamValue
// ot_mix`, 4 entries), written after the extension parameters (field 16) and
// only when set. A patch without it encodes byte-exact as before; the Mini
// skips the field, and a build without it keeps it as an unknown field.
#include <cmath>
#include <cstring>
#include <vector>

#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "test.h"

using namespace ot;
using namespace ot::audio;

namespace {

std::vector<uint8_t> encode(const SynthPatch& p) {
    std::vector<uint8_t> out(kPatchMaxFileSize);
    size_t n = 0;
    CHECK(encodePatch(p, out.data(), out.size(), &n) == PatchStatus::kOk);
    out.resize(n);
    return out;
}

void entry(std::vector<uint8_t>& b, uint32_t field, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    const uint32_t tag = (field << 3) | 2;   // length-delimited, two varint bytes
    b.push_back(uint8_t((tag & 0x7F) | 0x80));
    b.push_back(uint8_t(tag >> 7));
    b.push_back(5);
    b.push_back(0x0D);
    for (int k = 0; k < 4; ++k) b.push_back(uint8_t(bits >> (8 * k)));
}

}  // namespace

TEST(mix_absent_encodes_byte_exact) {
    SynthPatch p;
    CHECK(patchInit(kSynthEngineAcd, &p));
    CHECK_EQ(int(p.mixCount), 0);
    CHECK(!patchHasMix(p));
    const std::vector<uint8_t> b = encode(p);
    size_t beg = 0, end = 0;
    CHECK(!patchFieldSpan(b.data(), b.size(), kPatchMixField, &beg, &end));
    SynthPatch q;
    CHECK(decodePatch(b.data(), b.size(), &q) == PatchStatus::kOk);
    CHECK_EQ(int(q.mixCount), 0);
    CHECK(encode(q) == b);
}

TEST(mix_roundtrip_field17_after_field16) {
    SynthPatch p;
    CHECK(patchInit(kSynthEngineFat, &p));
    CHECK(synthParamSet(p, synthWidthParam(kSynthEngineFat), 0.5f));   // field 16 too
    patchSetMix(p, -3.5f, -40.0f, 25.0f, 60.0f);
    CHECK(patchHasMix(p));
    const std::vector<uint8_t> b = encode(p);
    size_t b16 = 0, e16 = 0, b17 = 0, e17 = 0, b9 = 0, e9 = 0;
    CHECK(patchFieldSpan(b.data(), b.size(), 9, &b9, &e9));
    CHECK(patchFieldSpan(b.data(), b.size(), kPatchExtField, &b16, &e16));
    CHECK(patchFieldSpan(b.data(), b.size(), kPatchMixField, &b17, &e17));
    CHECK(b16 >= e9);
    CHECK(b17 >= e16);
    CHECK_EQ(b.size() - b17, size_t(4 * 8));   // 4 entries of 8 bytes, last in the file
    SynthPatch q;
    CHECK(decodePatch(b.data(), b.size(), &q) == PatchStatus::kOk);
    CHECK(patchHasMix(q));
    CHECK(patchMix(q, kPatchMixGain) == -3.5f);
    CHECK(patchMix(q, kPatchMixPan) == -40.0f);
    CHECK(patchMix(q, kPatchMixReverb) == 25.0f);
    CHECK(patchMix(q, kPatchMixDelay) == 60.0f);
    CHECK_EQ(int(q.opaqueLen), 0);
    CHECK(encode(q) == b);
    CHECK_EQ(validatePatch(q, nullptr, 0), 0);
    // Without the mix the same patch is the file it was before.
    SynthPatch r = q;
    r.mixCount = 0;
    std::vector<uint8_t> head(b.begin(), b.begin() + long(b17));
    CHECK(encode(r) == head);
}

TEST(older_build_keeps_field17_as_unknown) {
    // A build without the mix keeps field 17 raw (unknown, in file order) and
    // writes it back after its last known field: the same bytes.
    SynthPatch p;
    CHECK(patchInit(kSynthEngineWtfm, &p));
    patchSetMix(p, 0.0f, 10.0f, 0.0f, 100.0f);
    const std::vector<uint8_t> b = encode(p);
    size_t b17 = 0, e17 = 0;
    CHECK(patchFieldSpan(b.data(), b.size(), kPatchMixField, &b17, &e17));
    SynthPatch old;
    CHECK(decodePatch(b.data(), b.size(), &old) == PatchStatus::kOk);
    old.mixCount = 0;
    old.opaqueLen = uint16_t(b.size() - b17);
    std::memcpy(old.opaque, b.data() + b17, old.opaqueLen);
    CHECK(encode(old) == b);
    // Hand-built entries read the same.
    std::vector<uint8_t> h = encode(old);
    h.resize(b17);
    entry(h, kPatchMixField, 0.0f);
    entry(h, kPatchMixField, 10.0f);
    entry(h, kPatchMixField, 0.0f);
    entry(h, kPatchMixField, 100.0f);
    CHECK(h == b);
}

TEST(mix_entries_past_four_are_dropped_and_a_partial_mix_is_none) {
    SynthPatch p;
    CHECK(patchInit(kSynthEngineAcd, &p));
    std::vector<uint8_t> b = encode(p);
    const size_t base = b.size();
    for (int k = 0; k < 6; ++k) entry(b, kPatchMixField, float(k));
    SynthPatch q;
    CHECK(decodePatch(b.data(), b.size(), &q) == PatchStatus::kOk);
    CHECK_EQ(int(q.mixCount), kPatchMixCount);
    CHECK(patchHasMix(q));
    CHECK(patchMix(q, 3) == 3.0f);
    CHECK_EQ(encode(q).size(), base + 4 * 8);
    // Two entries only (a damaged or foreign file): no mix.
    b.resize(base);
    entry(b, kPatchMixField, -6.0f);
    entry(b, kPatchMixField, 0.0f);
    CHECK(decodePatch(b.data(), b.size(), &q) == PatchStatus::kOk);
    CHECK_EQ(int(q.mixCount), 2);
    CHECK(!patchHasMix(q));
    CHECK(encode(q) == b);   // kept as read
    // An entry without its value: no mix either.
    patchSetMix(q, 0.0f, 0.0f, 0.0f, 0.0f);
    q.mix[2].present = false;
    CHECK(!patchHasMix(q));
    // Nor one that is not a number.
    patchSetMix(q, 0.0f, INFINITY, 0.0f, 0.0f);
    CHECK(!patchHasMix(q));
    patchSetMix(q, 0.0f, 0.0f, 0.0f, NAN);
    CHECK(!patchHasMix(q));
    // More entries than the patch holds: refused.
    q.mixCount = kPatchMixCount + 1;
    std::vector<uint8_t> out(kPatchMaxFileSize);
    size_t n = 0;
    CHECK(encodePatch(q, out.data(), out.size(), &n) == PatchStatus::kBadValue);
}
