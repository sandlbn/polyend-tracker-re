// OpenTracker patch extension parameters (2.x stereo): settings the Mini does
// not have (Width; FAT Stereo) live after the Mini's parameter list, in their
// own top-level field 16 (`repeated ParamValue ot_ext`), so the Mini's
// parameter count and macro targets are untouched. Absent = default 0 (mono,
// today's sound); a patch without them encodes byte-exact as before.
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

const int32_t kStereoEngines[] = {kSynthEngineAcd, kSynthEngineFat, kSynthEngineVap, kSynthEngineWtfm};

}  // namespace

TEST(ext_param_tables) {
    for (int32_t e : kStereoEngines) {
        const int n = synthParamCount(e);
        CHECK(n > 0);
        CHECK_EQ(synthParamTotal(e), n + (e == kSynthEngineFat ? 2 : 1));
        CHECK_EQ(synthWidthParam(e), n);
        const PatchParamRange* r = synthParamRange(e, n);
        CHECK(r != nullptr);
        if (r) {
            CHECK(r->def == 0.0f && r->min == 0.0f && r->max == 1.0f);
            CHECK(r->flags & kParamFlagInitAbsent);
        }
    }
    CHECK_EQ(synthFatStereoParam(), synthParamCount(kSynthEngineFat) + 1);
    // PERC has its own per-drum pan: no extension parameters.
    CHECK_EQ(synthParamTotal(kSynthEnginePerc), synthParamCount(kSynthEnginePerc));
    CHECK_EQ(synthWidthParam(kSynthEnginePerc), -1);
}

TEST(ext_absent_encodes_byte_exact_and_reads_default) {
    for (int32_t e : kStereoEngines) {
        SynthPatch p;
        CHECK(patchInit(e, &p));
        CHECK_EQ(int(p.extCount), 0);
        const std::vector<uint8_t> b = encode(p);
        size_t beg = 0, end = 0;
        CHECK(!patchFieldSpan(b.data(), b.size(), kPatchExtField, &beg, &end));
        CHECK(synthParamGet(p, synthWidthParam(e)) == 0.0f);
        // Setting a Mini parameter does not create the extension field.
        CHECK(synthParamSet(p, 0, synthParamGet(p, 0)));
        CHECK_EQ(int(p.extCount), 0);
    }
}

TEST(ext_roundtrip_field16) {
    SynthPatch p;
    CHECK(patchInit(kSynthEngineFat, &p));
    const int w = synthWidthParam(kSynthEngineFat), st = synthFatStereoParam();
    CHECK(synthParamSet(p, st, 0.75f));   // Stereo only: Width stays absent in front of it
    CHECK_EQ(int(p.extCount), 2);
    CHECK(!p.ext[0].present);
    CHECK(synthParamSet(p, w, 0.5f));
    const std::vector<uint8_t> b = encode(p);
    size_t beg = 0, end = 0;
    CHECK(patchFieldSpan(b.data(), b.size(), kPatchExtField, &beg, &end));
    // After Extra (field 9): the Mini's fields keep their place.
    size_t b9 = 0, e9 = 0;
    CHECK(patchFieldSpan(b.data(), b.size(), 9, &b9, &e9));
    CHECK(beg >= e9);
    SynthPatch q;
    CHECK(decodePatch(b.data(), b.size(), &q) == PatchStatus::kOk);
    CHECK_EQ(int(q.paramCount), synthParamCount(kSynthEngineFat));   // Mini count unchanged
    CHECK(synthParamGet(q, w) == 0.5f);
    CHECK(synthParamGet(q, st) == 0.75f);
    CHECK(encode(q) == b);
    CHECK_EQ(validatePatch(q, nullptr, 0), 0);
}

TEST(ext_out_of_range_takes_default) {
    SynthPatch p;
    CHECK(patchInit(kSynthEngineAcd, &p));
    const int w = synthWidthParam(kSynthEngineAcd);
    CHECK(synthParamSet(p, w, 5.0f));   // clamped
    CHECK(synthParamGet(p, w) == 1.0f);
    p.ext[0].bits = patchFloatToBits(-3.0f);   // as if read from a damaged file
    CHECK(synthParamGet(p, w) == 0.0f);
    PatchIssue is[4];
    CHECK_EQ(validatePatch(p, is, 4), 1);
    CHECK(is[0].kind == PatchIssue::kParamRange && is[0].index == w);
}

TEST(macro_cannot_target_ext) {
    SynthPatch p;
    CHECK(patchInit(kSynthEngineVap, &p));
    const int w = synthWidthParam(kSynthEngineVap);
    p.macros[0].count = 1;
    p.macros[0].targets[0].param = w;
    p.macros[0].targets[0].amountBits = patchFloatToBits(1.0f);
    PatchIssue is[4];
    CHECK_EQ(validatePatch(p, is, 4), 1);
    CHECK(is[0].kind == PatchIssue::kTargetParam);
    // The macro law ignores it: Width stays at the patch value.
    CHECK(synthParamSet(p, w, 0.25f));
    float out[kPatchMaxParams + kPatchExtMax] = {};
    const uint8_t vals[kSynthSeqMacros] = {127, 0, 0, 0, 0, 0};
    CHECK_EQ(synthEffectiveParams(p, vals, 1.0f, out), synthParamCount(kSynthEngineVap));
}

TEST(older_build_keeps_field16_as_unknown) {
    // A build without the extension (2.0 - 2.2) keeps field 16 raw and writes
    // it back after Extra; this build reads it from there.
    SynthPatch p;
    CHECK(patchInit(kSynthEngineWtfm, &p));
    std::vector<uint8_t> b = encode(p);
    const float v = 0.5f;
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    const uint8_t f16[] = {0x82, 0x01, 0x05, 0x0D, uint8_t(bits), uint8_t(bits >> 8), uint8_t(bits >> 16),
                           uint8_t(bits >> 24)};
    b.insert(b.end(), f16, f16 + sizeof(f16));
    SynthPatch q;
    CHECK(decodePatch(b.data(), b.size(), &q) == PatchStatus::kOk);
    CHECK_EQ(int(q.opaqueLen), 0);
    CHECK(synthParamGet(q, synthWidthParam(kSynthEngineWtfm)) == 0.5f);
    CHECK(encode(q) == b);
}
