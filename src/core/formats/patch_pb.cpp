// Tracker Mini `.patch` codec (spec 21 §7, §8). See patch_pb.h.
#include "formats/patch_pb.h"

#include <cmath>
#include <cstring>

namespace ot {

namespace {

// ---------------------------------------------------------------------------
// Wire format primitives
// ---------------------------------------------------------------------------

enum WireType : uint8_t { kWtVarint = 0, kWtFixed64 = 1, kWtLen = 2, kWtFixed32 = 5 };

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool atEnd() const { return p >= end; }
    size_t left() const { return size_t(end - p); }
};

PatchStatus readVarint(Reader& r, uint64_t* v) {
    uint64_t out = 0;
    for (int i = 0; i < 10; ++i) {
        if (r.atEnd()) return PatchStatus::kTruncated;
        uint8_t b = *r.p++;
        if (i == 9 && b > 1) return PatchStatus::kBadVarint;
        out |= uint64_t(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) {
            *v = out;
            return PatchStatus::kOk;
        }
    }
    return PatchStatus::kBadVarint;
}

PatchStatus readInt32(Reader& r, int32_t* v) {
    uint64_t u;
    PatchStatus s = readVarint(r, &u);
    if (s != PatchStatus::kOk) return s;
    int64_t sv = int64_t(u);
    if (sv < INT32_MIN || sv > INT32_MAX) return PatchStatus::kBadValue;
    *v = int32_t(sv);
    return PatchStatus::kOk;
}

PatchStatus readFixed32(Reader& r, uint32_t* v) {
    if (r.left() < 4) return PatchStatus::kTruncated;
    *v = uint32_t(r.p[0]) | (uint32_t(r.p[1]) << 8) | (uint32_t(r.p[2]) << 16) | (uint32_t(r.p[3]) << 24);
    r.p += 4;
    return PatchStatus::kOk;
}

// Reads a length prefix and splits off the sub-buffer.
PatchStatus readLen(Reader& r, Reader* sub) {
    uint64_t n;
    PatchStatus s = readVarint(r, &n);
    if (s != PatchStatus::kOk) return s;
    if (n > r.left()) return PatchStatus::kTruncated;
    sub->p = r.p;
    sub->end = r.p + n;
    r.p += n;
    return PatchStatus::kOk;
}

PatchStatus readTag(Reader& r, uint32_t* field, uint8_t* wt) {
    uint64_t t;
    PatchStatus s = readVarint(r, &t);
    if (s != PatchStatus::kOk) return s;
    if ((t >> 3) == 0 || (t >> 3) > 0x1FFFFFFFu) return PatchStatus::kBadField;
    *field = uint32_t(t >> 3);
    *wt = uint8_t(t & 7);
    if (*wt != kWtVarint && *wt != kWtFixed64 && *wt != kWtLen && *wt != kWtFixed32) return PatchStatus::kBadWireType;
    return PatchStatus::kOk;
}

PatchStatus skipValue(Reader& r, uint8_t wt) {
    switch (wt) {
        case kWtVarint: {
            uint64_t v;
            return readVarint(r, &v);
        }
        case kWtFixed64:
            if (r.left() < 8) return PatchStatus::kTruncated;
            r.p += 8;
            return PatchStatus::kOk;
        case kWtFixed32:
            if (r.left() < 4) return PatchStatus::kTruncated;
            r.p += 4;
            return PatchStatus::kOk;
        case kWtLen: {
            Reader sub;
            return readLen(r, &sub);
        }
        default:
            return PatchStatus::kBadWireType;
    }
}

#define PB_TRY(expr)                                    \
    do {                                                \
        PatchStatus s_ = (expr);                        \
        if (s_ != PatchStatus::kOk) return s_;          \
    } while (0)

#define PB_EXPECT_WT(wt, want) \
    do {                       \
        if ((wt) != (want)) return PatchStatus::kBadField; \
    } while (0)

struct Writer {
    uint8_t* p;
    uint8_t* end;
    bool ok = true;
    void byte(uint8_t b) {
        if (p < end) *p++ = b;
        else ok = false;
    }
    void bytes(const void* src, size_t n) {
        if (size_t(end - p) < n) {
            ok = false;
            p = end;
            return;
        }
        std::memcpy(p, src, n);
        p += n;
    }
    void varint(uint64_t v) {
        while (v >= 0x80) {
            byte(uint8_t(v) | 0x80);
            v >>= 7;
        }
        byte(uint8_t(v));
    }
    void tag(uint32_t field, uint8_t wt) { varint((uint64_t(field) << 3) | wt); }
    void fixed32(uint32_t v) {
        byte(uint8_t(v));
        byte(uint8_t(v >> 8));
        byte(uint8_t(v >> 16));
        byte(uint8_t(v >> 24));
    }
    void int32(int32_t v) { varint(uint64_t(int64_t(v))); }
    // Length-delimited field whose payload was built in a scratch buffer.
    void sub(uint32_t field, const uint8_t* data, size_t n) {
        tag(field, kWtLen);
        varint(n);
        bytes(data, n);
    }
};

// ---------------------------------------------------------------------------
// Sub-message decoders
// ---------------------------------------------------------------------------

PatchStatus decodeParam(Reader r, PatchParam* out) {
    while (!r.atEnd()) {
        uint32_t f;
        uint8_t wt;
        PB_TRY(readTag(r, &f, &wt));
        if (f == 1) {
            PB_EXPECT_WT(wt, kWtFixed32);
            PB_TRY(readFixed32(r, &out->bits));
            out->present = true;
        } else {
            PB_TRY(skipValue(r, wt));
        }
    }
    return PatchStatus::kOk;
}

PatchStatus decodeName(Reader r, PatchName* out) {
    while (!r.atEnd()) {
        uint32_t f;
        uint8_t wt;
        PB_TRY(readTag(r, &f, &wt));
        if (f == 1) {
            PB_EXPECT_WT(wt, kWtLen);
            Reader s;
            PB_TRY(readLen(r, &s));
            if (s.left() > size_t(kPatchNameMax)) return PatchStatus::kNameTooLong;
            out->len = uint16_t(s.left());
            std::memcpy(out->text, s.p, s.left());
            out->text[out->len] = 0;
        } else {
            PB_TRY(skipValue(r, wt));
        }
    }
    return PatchStatus::kOk;
}

PatchStatus decodeTarget(Reader r, PatchTarget* out) {
    while (!r.atEnd()) {
        uint32_t f;
        uint8_t wt;
        PB_TRY(readTag(r, &f, &wt));
        if (f == 1) {
            PB_EXPECT_WT(wt, kWtVarint);
            PB_TRY(readInt32(r, &out->param));
        } else if (f == 2) {
            PB_EXPECT_WT(wt, kWtFixed32);
            PB_TRY(readFixed32(r, &out->amountBits));
        } else {
            PB_TRY(skipValue(r, wt));
        }
    }
    return PatchStatus::kOk;
}

PatchStatus decodeMacro(Reader r, PatchMacro* out) {
    int nTargets = 0;
    while (!r.atEnd()) {
        uint32_t f;
        uint8_t wt;
        PB_TRY(readTag(r, &f, &wt));
        switch (f) {
            case 1: {
                PB_EXPECT_WT(wt, kWtLen);
                Reader s;
                PB_TRY(readLen(r, &s));
                if (s.left() > size_t(kPatchMacroNameMax)) return PatchStatus::kNameTooLong;
                out->nameLen = uint8_t(s.left());
                std::memcpy(out->name, s.p, s.left());
                out->name[out->nameLen] = 0;
                break;
            }
            case 2: {
                PB_EXPECT_WT(wt, kWtVarint);
                uint64_t v;
                PB_TRY(readVarint(r, &v));
                out->bipolar = v != 0;
                break;
            }
            case 3:
                PB_EXPECT_WT(wt, kWtVarint);
                PB_TRY(readInt32(r, &out->count));
                break;
            case 4: {
                PB_EXPECT_WT(wt, kWtLen);
                if (nTargets >= kPatchMacroTargets) return PatchStatus::kTooManyEntries;
                Reader s;
                PB_TRY(readLen(r, &s));
                PB_TRY(decodeTarget(s, &out->targets[nTargets++]));
                break;
            }
            default:
                PB_TRY(skipValue(r, wt));
        }
    }
    return PatchStatus::kOk;
}

// Everything at proto3 zero values. Unused macro targets are {0, 0}: the
// factory writer encodes every target past target_count as an empty message.
void resetPatch(SynthPatch* p) { std::memset(p, 0, sizeof(*p)); }

// Length of one raw top-level field at d[0..n) and its field number.
PatchStatus rawFieldLen(const uint8_t* d, size_t n, uint32_t* field, size_t* len) {
    Reader r{d, d + n};
    uint8_t wt;
    PB_TRY(readTag(r, field, &wt));
    PB_TRY(skipValue(r, wt));
    *len = size_t(r.p - d);
    return PatchStatus::kOk;
}

// ---------------------------------------------------------------------------
// Engine parameter tables (spec 21 §8; MINI_PLAN corrections: VAP 27/31 and
// WTFM 32/36 LFO Ratio are Hz 0..100, hi-hat/cymbal Timbre 0..24).
// ---------------------------------------------------------------------------

constexpr uint8_t NR = kParamFlagNoRange | kParamFlagInitAbsent;

const PatchParamRange kAcd[29] = {
    {1, 0, 1},     {0, 0, 1},       {0, 0, 1},       {0, -1, 1},    {0.5f, 0, 1},  {0, 0, 5},
    {0, -1, 1},    {0, -1, 1},      {0, -3, 3},      {400, 20, 20000}, {0.5f, 0, 1.5f}, {1, 0, 2},
    {1, -2, 2},    {0, -1, 1},      {0.14f, -1, 1},  {0, 0, 10},    {0.5f, 0, 10}, {0, 0, 1},
    {0.5f, 0, 10}, {0, 0, 10},      {0.5f, 0, 10},   {0, 0, 1},     {0.5f, 0, 10}, {1, 0.1f, 100},
    {1, 0, 5},     {2, 0, 2},       {0, 0, 3},       {0, 0, 3},     {1, 0, 2},
};

const PatchParamRange kFat[25] = {
    {0.1f, 0, 1},  {1, -1, 1},      {0.05f, 0, 1},   {0, 0, 1},     {0, 0, 1},     {1, 0, 1},
    {0, 0, 2},     {400, 20, 20000}, {0.5f, 0, 1.5f}, {0, -1, 1},   {0.14f, -1, 1}, {0, 0, 10},
    {0.5f, 0, 10}, {0, 0, 1},       {0.5f, 0, 10},   {0, 0, 10},    {0.5f, 0, 10}, {0, 0, 1},
    {0.5f, 0, 10}, {1, 0.1f, 100},  {1, 0, 5},       {2, 0, 2},     {0, 0, 3},     {0, 0, 3},
    {1, 0, 2},
};

#define MODSLOT {0, 0, 4}, {0, 0, 13}, {0, -4, 4}

const PatchParamRange kVap[60] = {
    {0.5f, 0, 1},  {0.5f, 0, 1},    {0, -0.5f, 0.5f}, {0, -0.5f, 0.5f}, {0, -1, 1},  {0, -36, 36},
    {0, -36, 36},  {0, 0, 100},     {0, -100, 100},  {0, 0, 1},     {0, 0, 1},     {0, 0, 14},
    {400, 20, 20000}, {0.5f, 0, 1.5f}, {0.14f, -1, 1}, {0, 0, 10},   {0.5f, 0, 10}, {1, 0, 1},
    {0.3f, 0, 10}, {0, 0, 10},      {0.5f, 0, 10},   {0, 0, 1},     {0.5f, 0, 10}, {0, 0, 10},
    {0.5f, 0, 10}, {0, 0, 1},       {0.3f, 0, 10},
    {0, 0, 100},   {1, 0, 5},       {2, 0, 2},       {0, 0, 1},  // 27-30 LFO 1
    {0, 0, 100},   {1, 0, 5},       {2, 0, 2},       {0, 0, 1},  // 31-34 LFO 2
    {0, 0, 3},     {0, 0, 3},       {1, -2, 2},      {0, -2, 2},    {1, 0, 2},     {0, 0, 1},
    {0, 0, 1},     // 41 Noise
    MODSLOT,       MODSLOT,         MODSLOT,         MODSLOT,       MODSLOT,       MODSLOT,
};

const PatchParamRange kWtfm[63] = {
    {3, 0, 47},    {3, 0, 47},      {0, -100, 100},  {0, -100, 100}, {0, 0, 1},    {0, 0, 1},
    {0, 0, 0, NR}, {0, 0, 0, NR},   {0, 0, 1},       {0, 0, 8},     {0, 0, 8},     {0, 0, 10},
    {0, 0, 1},     {0, 0, 1},       {0, 0, 1},       {0, 0, 14},    {400, 20, 20000}, {0.5f, 0, 1.5f},
    {0.14f, -1, 1}, {1, -2, 2},
    {0, 0, 10},    {0.5f, 0, 10},   {1, 0, 1},       {0.3f, 0, 10},  // 20-23 Amp Env
    {0, 0, 10},    {0.5f, 0, 10},   {0, 0, 1},       {0.5f, 0, 10},  // 24-27 Env 1
    {0, 0, 10},    {0.5f, 0, 10},   {0, 0, 1},       {0.3f, 0, 10},  // 28-31 Env 2
    {0, 0, 100},   {1, 0, 5},       {2, 0, 2},       {0, 0, 1},      // 32-35 LFO 1
    {0, 0, 100},   {1, 0, 5},       {2, 0, 2},       {0, 0, 1},      // 36-39 LFO 2
    {0, 0, 3},     {0, 0, 3},       {1, 0, 2},
    MODSLOT,       MODSLOT,         MODSLOT,         MODSLOT,       MODSLOT,       MODSLOT,
    {0, 0, 1},     {0, 0, 1},
};

#undef MODSLOT
#define MIXER(level, pan, delay) {level, 0, 2}, {pan, -1, 1}, {delay, 0, 1}, {0, 0, 1}, {0, 0, 2}

const PatchParamRange kPerc[84] = {
    // Kick 0-8
    {0, 0, 175}, {31, 12, 119}, {1.8f, 0, 100}, {0.26f, 0, 10}, {0.32f, -1, 1}, {0.5f, 0, 1},
    {0.13f, 0, 10}, {0.2f, -1, 1}, {2, 0, 2},
    // Tom 9-17
    {0, 0, 175}, {1.4f, 0, 100}, {0.41f, 0, 10}, {0.58f, 0, 1}, {1, 0, 2}, {0.05f, 0, 10},
    {1.65f, -10, 10}, {0.4f, -1, 1}, {1, 0, 2},
    // Snare 18-26
    {0, 0, 175}, {50, 12, 119}, {0.5f, 0, 1}, {1, 0, 4}, {0.17f, 0, 10}, {0.8f, 0, 100},
    {0.26f, 0, 10}, {-0.39f, -1, 1}, {0.13f, 0, 1},
    // Hi-hat 27-35
    {0.12f, 0, 1}, {0.16f, 0, 10}, {0.81f, 0, 10}, {5080, 10, 20000}, {9040, 10, 20000},
    {209, 100, 880}, {-0.09f, -1, 1}, {0, 0, 24}, {0, 0, 1},
    // Cymbal 36-44
    {3000, 500, 10000}, {7000, 500, 10000}, {1.5f, 0, 10}, {0.25f, 0, 10}, {204, 80, 880},
    {-0.39f, -1, 1}, {0.29f, -1, 1}, {0, 0, 1}, {0, 0, 24},
    // Perc 45-48
    {0, 0, 4}, {0.07f, 0, 1}, {0.62f, 0, 1}, {0.14f, 0, 2},
    // Mixer 49-78: Kick, Tom, Snare, Hihat, Cymbal, Perc
    MIXER(1, 0, 0), MIXER(0.8f, 0.2f, 0), MIXER(1, -0.1f, 0.5f), MIXER(1, 0.3f, 0.5f),
    MIXER(1, -0.3f, 0.5f), MIXER(1, 0.1f, 0.5f),
    // FX 79-83
    {0, 0, 1}, {0, 0, 4}, {0, -18, 18}, {0, -18, 18}, {0, -18, 18},
};

#undef MIXER

// Nearest IEEE half-precision value (round to nearest even): the device keeps
// each min/max as a half pair (§8.1), so bounds compare at half precision.
float toHalfPrecision(float f) {
    if (f == 0 || !std::isfinite(f)) return f;
    int e;
    std::frexp(f, &e);  // |f| = m * 2^e, m in [0.5, 1)
    // Half: 11 significant bits for normal values (exponent >= -13 here).
    int shift = 11 - e;
    if (e < -13) shift = 24;  // subnormal half: fixed step 2^-24
    float scaled = std::ldexp(f, shift);
    return std::ldexp(std::nearbyint(scaled), -shift);
}

}  // namespace

bool patchFieldSpan(const uint8_t* data, size_t len, uint32_t field, size_t* beg, size_t* end) {
    size_t pos = 0;
    while (pos < len) {
        uint32_t f;
        size_t n;
        if (rawFieldLen(data + pos, len - pos, &f, &n) != PatchStatus::kOk) return false;
        if (f == field) {
            *beg = pos;
            *end = pos + n;
            return true;
        }
        pos += n;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

const char* patchStatusText(PatchStatus s) {
    switch (s) {
        case PatchStatus::kOk: return "ok";
        case PatchStatus::kTooLarge: return "patch file too large";
        case PatchStatus::kTruncated: return "patch truncated";
        case PatchStatus::kBadVarint: return "bad varint";
        case PatchStatus::kBadWireType: return "bad wire type";
        case PatchStatus::kBadField: return "bad field";
        case PatchStatus::kBadValue: return "value out of range";
        case PatchStatus::kTooManyParams: return "too many parameters";
        case PatchStatus::kTooManyEntries: return "too many entries";
        case PatchStatus::kNameTooLong: return "name too long";
        case PatchStatus::kOpaqueOverflow: return "too many unknown fields";
        case PatchStatus::kBufferTooSmall: return "buffer too small";
    }
    return "?";
}

PatchStatus decodePatch(const uint8_t* data, size_t len, SynthPatch* out) {
    resetPatch(out);
    if (len > kPatchMaxFileSize) return PatchStatus::kTooLarge;
    if (len && !data) return PatchStatus::kTruncated;
    Reader r{data, data + len};
    int nNames = 0, nMacros = 0;
    while (!r.atEnd()) {
        const uint8_t* start = r.p;
        uint32_t f;
        uint8_t wt;
        PB_TRY(readTag(r, &f, &wt));
        switch (f) {
            case 1:
                PB_EXPECT_WT(wt, kWtVarint);
                PB_TRY(readInt32(r, &out->engine));
                break;
            case 6: {
                PB_EXPECT_WT(wt, kWtLen);
                if (out->paramCount >= kPatchMaxParams) return PatchStatus::kTooManyParams;
                Reader s;
                PB_TRY(readLen(r, &s));
                PB_TRY(decodeParam(s, &out->params[out->paramCount]));
                ++out->paramCount;
                break;
            }
            case 7: {
                PB_EXPECT_WT(wt, kWtLen);
                if (nNames >= kPatchNameSlots) return PatchStatus::kTooManyEntries;
                Reader s;
                PB_TRY(readLen(r, &s));
                PB_TRY(decodeName(s, &out->names[nNames++]));
                break;
            }
            case 8: {
                PB_EXPECT_WT(wt, kWtLen);
                if (nMacros >= kPatchMacros) return PatchStatus::kTooManyEntries;
                Reader s;
                PB_TRY(readLen(r, &s));
                PB_TRY(decodeMacro(s, &out->macros[nMacros++]));
                break;
            }
            case 9: {
                PB_EXPECT_WT(wt, kWtLen);
                Reader s;
                PB_TRY(readLen(r, &s));  // contents ignored on load (§7.2)
                out->extraPresent = true;
                break;
            }
            case kPatchExtField: {
                // [OT] extension parameters; entries past kPatchExtMax (a
                // later build's) are dropped.
                PB_EXPECT_WT(wt, kWtLen);
                Reader s;
                PB_TRY(readLen(r, &s));
                if (out->extCount < kPatchExtMax) {
                    PB_TRY(decodeParam(s, &out->ext[out->extCount]));
                    ++out->extCount;
                }
                break;
            }
            default: {
                PB_TRY(skipValue(r, wt));
                size_t n = size_t(r.p - start);
                if (out->opaqueLen + n > size_t(kPatchOpaqueMax)) return PatchStatus::kOpaqueOverflow;
                std::memcpy(out->opaque + out->opaqueLen, start, n);
                out->opaqueLen = uint16_t(out->opaqueLen + n);
            }
        }
    }
    return PatchStatus::kOk;
}

void patchExtraPayload(uint8_t* out) {
    // Extra { a = ExtraA{ x 0, a1 {x 3, y 1}, a2 {0, 0} },
    //         b = ExtraB{ x 0, rows[64] {x 0, v[2] packed fixed64 {0, 0}, y 0}, y 0, z 0 } }
    // with proto3 omission of zero scalars: a = 0A 08 | 12 04 08 03 10 01 | 1A 00,
    // b = 12 <1280> then 64 x (12 12 | 12 10 <16 zero bytes>).
    static const uint8_t kHead[] = {0x0A, 0x08, 0x12, 0x04, 0x08, 0x03, 0x10, 0x01, 0x1A, 0x00,
                                    0x12, 0x80, 0x0A};
    std::memcpy(out, kHead, sizeof(kHead));
    uint8_t* p = out + sizeof(kHead);
    for (int i = 0; i < 64; ++i) {
        p[0] = 0x12;
        p[1] = 0x12;
        p[2] = 0x12;
        p[3] = 0x10;
        std::memset(p + 4, 0, 16);
        p += 20;
    }
}

PatchStatus encodePatch(const SynthPatch& pt, uint8_t* out, size_t cap, size_t* written) {
    *written = 0;
    Writer w{out, out + cap};
    if (pt.opaqueLen > kPatchOpaqueMax || pt.paramCount > kPatchMaxParams || pt.extCount > kPatchExtMax)
        return PatchStatus::kBadValue;

    // Unknown fields are flushed, in stored order, ahead of the first known
    // field with a higher number.
    size_t opos = 0;
    auto flushOpaque = [&](uint32_t below) -> PatchStatus {
        while (opos < pt.opaqueLen) {
            uint32_t f;
            size_t n;
            PB_TRY(rawFieldLen(pt.opaque + opos, pt.opaqueLen - opos, &f, &n));
            if (f >= below) break;
            w.bytes(pt.opaque + opos, n);
            opos += n;
        }
        return PatchStatus::kOk;
    };

    PB_TRY(flushOpaque(1));
    if (pt.engine != 0) {
        w.tag(1, kWtVarint);
        w.int32(pt.engine);
    }

    PB_TRY(flushOpaque(6));
    for (int i = 0; i < pt.paramCount; ++i) {
        w.tag(6, kWtLen);
        if (pt.params[i].present) {
            w.byte(5);
            w.tag(1, kWtFixed32);
            w.fixed32(pt.params[i].bits);
        } else {
            w.byte(0);
        }
    }

    PB_TRY(flushOpaque(7));
    for (const PatchName& n : pt.names) {
        if (n.len > kPatchNameMax) return PatchStatus::kNameTooLong;
        w.tag(7, kWtLen);
        if (n.len) {
            w.varint(1u + (n.len < 0x80 ? 1u : 2u) + n.len);   // tag + length varint (<= 256: 1-2 bytes) + text
            w.tag(1, kWtLen);
            w.varint(n.len);
            w.bytes(n.text, n.len);
        } else {
            w.byte(0);
        }
    }

    PB_TRY(flushOpaque(8));
    for (const PatchMacro& m : pt.macros) {
        if (m.nameLen > kPatchMacroNameMax) return PatchStatus::kNameTooLong;
        uint8_t buf[256];
        Writer s{buf, buf + sizeof(buf)};
        if (m.nameLen) {
            s.tag(1, kWtLen);
            s.varint(m.nameLen);
            s.bytes(m.name, m.nameLen);
        }
        if (m.bipolar) {
            s.tag(2, kWtVarint);
            s.byte(1);
        }
        if (m.count != 0) {
            s.tag(3, kWtVarint);
            s.int32(m.count);
        }
        for (const PatchTarget& t : m.targets) {
            uint8_t tb[24];
            Writer ts{tb, tb + sizeof(tb)};
            if (t.param != 0) {
                ts.tag(1, kWtVarint);
                ts.int32(t.param);
            }
            if (t.amountBits != 0) {
                ts.tag(2, kWtFixed32);
                ts.fixed32(t.amountBits);
            }
            s.sub(4, tb, size_t(ts.p - tb));
        }
        if (!s.ok) return PatchStatus::kBufferTooSmall;  // cannot happen: max ~150 B
        w.sub(8, buf, size_t(s.p - buf));
    }

    PB_TRY(flushOpaque(9));
    w.tag(9, kWtLen);
    w.varint(kPatchExtraSize);
    if (w.ok && size_t(w.end - w.p) >= kPatchExtraSize) {
        patchExtraPayload(w.p);
        w.p += kPatchExtraSize;
    } else {
        w.ok = false;
    }

    // [OT] extension parameters, up to the last one set: a patch without
    // them encodes exactly as the Mini's.
    PB_TRY(flushOpaque(kPatchExtField));
    int extN = pt.extCount;
    while (extN > 0 && !pt.ext[extN - 1].present) --extN;
    for (int i = 0; i < extN; ++i) {
        w.tag(kPatchExtField, kWtLen);
        if (pt.ext[i].present) {
            w.byte(5);
            w.tag(1, kWtFixed32);
            w.fixed32(pt.ext[i].bits);
        } else {
            w.byte(0);
        }
    }

    PB_TRY(flushOpaque(0xFFFFFFFFu));
    if (opos != pt.opaqueLen) return PatchStatus::kBadField;
    if (!w.ok) return PatchStatus::kBufferTooSmall;
    *written = size_t(w.p - out);
    return PatchStatus::kOk;
}

const PatchParamRange* patchEngineParams(int32_t engine, int* count) {
    const PatchParamRange* t = nullptr;
    int n = 0;
    switch (engine) {
        case kSynthEngineAcd: t = kAcd, n = 29; break;
        case kSynthEngineFat: t = kFat, n = 25; break;
        case kSynthEngineVap: t = kVap, n = 60; break;
        case kSynthEngineWtfm: t = kWtfm, n = 63; break;
        case kSynthEnginePerc: t = kPerc, n = 84; break;
        default: break;
    }
    if (count) *count = n;
    return t;
}

namespace {
// [OT] Width (all but PERC) and FAT Stereo: 0..1, 0 = mono (today's sound).
const PatchParamRange kExtWidth[] = {{0.0f, 0.0f, 1.0f, kParamFlagInitAbsent}};
const PatchParamRange kExtFat[] = {{0.0f, 0.0f, 1.0f, kParamFlagInitAbsent}, {0.0f, 0.0f, 1.0f, kParamFlagInitAbsent}};
}  // namespace

const PatchParamRange* patchEngineExtParams(int32_t engine, int* count) {
    const PatchParamRange* t = nullptr;
    int n = 0;
    switch (engine) {
        case kSynthEngineAcd:
        case kSynthEngineVap:
        case kSynthEngineWtfm: t = kExtWidth, n = 1; break;
        case kSynthEngineFat: t = kExtFat, n = 2; break;
        default: break;
    }
    if (count) *count = n;
    return t;
}

const char* patchEngineName(int32_t engine) {
    switch (engine) {
        case kSynthEngineAcd: return "ACD";
        case kSynthEngineFat: return "FAT";
        case kSynthEngineVap: return "VAP";
        case kSynthEngineWtfm: return "WTFM";
        case kSynthEnginePerc: return "PERC";
        default: return nullptr;
    }
}

int patchVolumeParam(int32_t engine) {
    switch (engine) {
        case kSynthEngineAcd: return 28;
        case kSynthEngineFat: return 24;
        case kSynthEngineVap: return 39;
        case kSynthEngineWtfm: return 42;
        default: return -1;
    }
}

bool patchInit(int32_t engine, SynthPatch* out) {
    int n = 0;
    const PatchParamRange* t = patchEngineParams(engine, &n);
    resetPatch(out);
    if (!t) return false;
    out->engine = engine;
    out->paramCount = uint16_t(n);
    for (int i = 0; i < n; ++i) {
        out->params[i].present = !(t[i].flags & kParamFlagInitAbsent);
        out->params[i].bits = out->params[i].present ? patchFloatToBits(t[i].def) : 0;
    }
    auto setName = [](PatchMacro& m, const char* s) {
        size_t l = std::strlen(s);
        m.nameLen = uint8_t(l);
        std::memcpy(m.name, s, l + 1);
    };
    char nm[] = "Macro 0";
    for (int k = 0; k < 6; ++k) {
        nm[6] = char('1' + k);
        setName(out->macros[k], nm);
    }
    setName(out->macros[kPatchMacroVelocity], "Velocity");
    setName(out->macros[kPatchMacroPressure], "Pressure");
    int vol = patchVolumeParam(engine);
    if (vol >= 0) {
        PatchMacro& m = out->macros[kPatchMacroVelocity];
        m.count = 1;
        m.targets[0].param = vol;
        m.targets[0].amountBits = patchFloatToBits(1.0f);
    }
    out->extraPresent = true;
    return true;
}

bool patchParamInRange(int32_t engine, int index, float v) {
    int n = 0, ne = 0;
    const PatchParamRange* t = patchEngineParams(engine, &n);
    const PatchParamRange* x = patchEngineExtParams(engine, &ne);
    if (!t || index < 0 || index >= n + ne || !std::isfinite(v)) return false;
    const PatchParamRange& r = index < n ? t[index] : x[index - n];
    if (r.flags & kParamFlagNoRange) return true;
    return v >= toHalfPrecision(r.min) && v <= toHalfPrecision(r.max);
}

int validatePatch(const SynthPatch& p, PatchIssue* out, int cap) {
    int total = 0;
    auto add = [&](PatchIssue::Kind k, int index, int sub) {
        if (out && total < cap) out[total] = PatchIssue{k, int16_t(index), int8_t(sub)};
        ++total;
    };
    int n = 0;
    const PatchParamRange* t = patchEngineParams(p.engine, &n);
    if (!t) add(PatchIssue::kEngine, -1, -1);
    if (t && p.paramCount != n) add(PatchIssue::kParamCount, p.paramCount, -1);
    if (t) {
        int m = p.paramCount < n ? p.paramCount : n;
        for (int i = 0; i < m; ++i)
            if (p.params[i].present && !patchParamInRange(p.engine, i, patchBitsToFloat(p.params[i].bits)))
                add(PatchIssue::kParamRange, i, -1);
        // [OT] extension parameters: index n + k; entries this engine does not have are ignored.
        int ne = 0;
        patchEngineExtParams(p.engine, &ne);
        for (int k = 0; k < p.extCount && k < ne; ++k)
            if (p.ext[k].present && !patchParamInRange(p.engine, n + k, patchBitsToFloat(p.ext[k].bits)))
                add(PatchIssue::kParamRange, n + k, -1);
    }
    for (int k = 0; k < kPatchMacros; ++k) {
        const PatchMacro& mc = p.macros[k];
        if (mc.nameLen > kPatchMacroNameMax) add(PatchIssue::kNameLen, k, -1);
        if (mc.count < 0 || mc.count > kPatchMacroTargets) add(PatchIssue::kMacroCount, k, -1);
        for (int j = 0; j < kPatchMacroTargets; ++j) {
            const PatchTarget& tg = mc.targets[j];
            if (tg.param < -1 || (t && tg.param >= n)) add(PatchIssue::kTargetParam, k, j);
            float a = patchBitsToFloat(tg.amountBits);
            if (!std::isfinite(a) || a < -2.0f || a > 2.0f) add(PatchIssue::kTargetAmount, k, j);
        }
    }
    return total;
}

}  // namespace ot
