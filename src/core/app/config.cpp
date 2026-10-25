#include "app/config.h"

#include <cstddef>
#include <cstring>

namespace ot {

namespace {
constexpr uint8_t kSignature[7] = {'E', 'E', 'P', 'R', 'O', 'M', 0};   // spec 01 §10
constexpr uint8_t kFileMagic[4] = {'O', 'T', 'C', 'F'};
constexpr uint8_t kFileV1 = 1;   // older OpenTracker: raw typed fields
constexpr uint8_t kFileV2 = 2;   // config image

// Typed field <-> image offset (spec 05 §5.4 table, spec 03 §4.1).
struct Map {
    uint16_t field;   // offsetof(Config, ...)
    uint8_t img;
};
#define F(m) uint16_t(offsetof(Config, m))
const Map kMap[] = {
    {F(masterVolume), 0x1B},
    {F(clockIn), 0x26},          {F(clockOut), 0x27},     {F(transportIn), 0x28},
    {F(transportOut), 0x29},     {F(notesIn), 0x2A},      {F(notesInChannel), 0x2B},
    {F(midiOut), 0x2C},          {F(ccIn), 0x2D},         {F(ccInChannel), 0x2E},
    {F(middleC), 0x2F},          {F(clockCorrection), 0x30},
    {F(radioRegion), 0xCA},      {F(brightness), 0xCB},   {F(padsBrightness), 0xCC},
    {F(recordingOptions), 0xCE}, {F(perfPresets), 0xCF},  {F(numbering), 0xD0},
    {F(padsPreview), 0xD1},      {F(arrangement), 0xD2},  {F(patternTopInfo), 0xD3},
    {F(lineInChannel), 0xD4},    {F(theme), 0xD5},        {F(font), 0xD6},
    {F(previewVolume), 0xD7},
    {F(metronome), 0xE6},        {F(preRoll), 0xE7},      {F(tsNum), 0xE8},
    {F(tsDen), 0xE9},            {F(metronomeVolume), 0xEA},
    {F(sliceCount), 0xF2},
};
#undef F

template <typename T>
void clampField(T& v, T max, T def) {
    if (v > max) v = def;
}

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
void wr16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

// Defaults of the bytes that are not typed fields (spec 05 §5.4 "Other
// global config fields", spec 12 §A.2 preset defaults).
void defaultImage(uint8_t* img) {
    std::memset(img, 0, Config::kImageSize);
    std::memcpy(img, kSignature, sizeof(kSignature));
    wr16(img + Config::kOffVersion, 0x0A00);
    img[Config::kOffSize] = 0xF4;
    const float master = 81.0f;   // 0x10 f32
    std::memcpy(img + 0x10, &master, 4);
    const uint8_t levels[8] = {0x23, 0, 0, 0x0F, 0x0F, 0x1D, 0x1D, 0x51};   // 0x14..0x1B
    std::memcpy(img + 0x14, levels, 8);
    img[0x21] = 0xFF;                     // no "beta" suffix (spec 05 §1.5 state 1)
    for (int t = 0; t < 8; ++t) wr16(img + Config::kOffPerfPatterns + 2 * t, 1);
    for (int s = 0; s < 12; ++s) img[Config::kOffPerfTypes + s] = uint8_t(s + 1);
    img[0xD7] = 0x56;
    for (int i = 0; i < 7; ++i) img[0xDC + i] = uint8_t(i + 1);
    img[0xF2] = 8;
}

// configValidate (spec 05 §5.4) for the bytes that are not typed fields.
void validateImage(uint8_t* img) {
    clampField<uint8_t>(img[0x14], 0x3F, 0x23);
    clampField<uint8_t>(img[0x15], 1, 0);
    clampField<uint8_t>(img[0x16], 1, 0);
    for (int i = 0x19; i <= 0x1A; ++i)
        if (img[i] < 13 || img[i] > 31) img[i] = 0x1D;
    // Performance presets (spec 12 §A.2 "Validation on load").
    for (int t = 0; t < 8; ++t) {
        const uint16_t v = rd16(img + Config::kOffPerfPatterns + 2 * t);
        if (v < 1 || v > 255) wr16(img + Config::kOffPerfPatterns + 2 * t, 1);
    }
    for (int s = 0; s < 12; ++s) {
        uint8_t& ty = img[Config::kOffPerfTypes + s];
        if (ty >= 22) ty = uint8_t(s + 1 < 22 ? s + 1 : 0);
        uint8_t& r = img[Config::kOffPerfRows + s];
        if (r > 3) r = 0;
        uint8_t* v = img + Config::kOffPerfValues + 8 * s;
        wr16(v, 0);
        for (int k = 1; k < 4; ++k) {
            const int16_t x = int16_t(rd16(v + 2 * k));
            if (x < -255 || x > 255) wr16(v + 2 * k, 0);
        }
    }
}
}  // namespace

Config::Config() { defaultImage(image); }

void Config::setDefaults() {
    Config d;
    *this = d;
}

void Config::sanitize() {
    // Range validation in the spirit of spec 05 §5.4 configValidate.
    clampField<uint8_t>(radioRegion, 3, 0);
    clampField<uint8_t>(exportRate, 1, 1);
    clampField<uint8_t>(brightness, 2, 2);
    clampField<uint8_t>(theme, 2, 0);
    clampField<uint8_t>(font, 1, 0);
    clampField<uint8_t>(padsBrightness, 2, 2);
    clampField<uint8_t>(padsPreview, 2, 2);
    clampField<uint8_t>(recordingOptions, 3, 0);
    clampField<uint8_t>(numbering, 2, 1);
    clampField<uint8_t>(arrangement, 2, 0);
    clampField<uint8_t>(patternTopInfo, 2, 0);
    clampField<uint8_t>(lineInChannel, 2, 0);
    clampField<uint8_t>(perfPresets, 1, 0);   // spec 12 §A.3: > 1 -> 0
    clampField<uint8_t>(metronome, 1, 0);
    clampField<uint8_t>(preRoll, 1, 0);
    clampField<uint8_t>(tsNum, 11, 3);
    clampField<uint8_t>(tsDen, 11, 3);
    clampField<uint8_t>(metronomeVolume, 100, 50);
    clampField<uint8_t>(clockIn, 2, 0);
    clampField<uint8_t>(clockOut, 3, 0);
    clampField<uint8_t>(transportIn, 2, 0);
    clampField<uint8_t>(transportOut, 3, 0);
    clampField<uint8_t>(notesIn, 3, 0);
    clampField<uint8_t>(notesInChannel, 16, 0);
    clampField<uint8_t>(midiOut, 3, 0);
    clampField<uint8_t>(ccIn, 3, 0);
    clampField<uint8_t>(ccInChannel, 16, 0);
    clampField<uint8_t>(middleC, 3, 2);
    clampField<uint8_t>(clockCorrection, 12, 6);
    clampField<uint8_t>(masterVolume, 100, 81);
    clampField<uint8_t>(previewVolume, 100, 86);
    if (sliceCount < 1 || sliceCount > 48) sliceCount = 8;
    clampField<uint8_t>(recSource, 3, 0);
    for (auto& g : recGain) clampField<uint8_t>(g, 100, 50);
    clampField<uint8_t>(recMonitor, 1, 1);
    if (radioFreq10 < 760 || radioFreq10 > 1080) radioFreq10 = 875;
    clampField<uint8_t>(cpuMeter, 2, 0);
    if (synthVoices < 1 || synthVoices > kMaxSynthVoices) synthVoices = kDefaultSynthVoices;
    validateImage(image);
}

// Pads brightness -> LED levels (spec 05 appendix: k = choice + 1,
// DIM = floor(10k/3), FULL = floor(31k/3), LEVEL_3 = floor(5k/3)).
uint8_t Config::padDim() const { return uint8_t(10 * ((padsBrightness < 3 ? padsBrightness : 2) + 1) / 3); }
uint8_t Config::padFull() const { return uint8_t(31 * ((padsBrightness < 3 ? padsBrightness : 2) + 1) / 3); }

void Config::toImage() {
    std::memcpy(image, kSignature, sizeof(kSignature));
    const uint8_t* base = reinterpret_cast<const uint8_t*>(this);
    for (const Map& m : kMap) image[m.img] = base[m.field];
    const int k = (padsBrightness < 3 ? padsBrightness : 2) + 1;
    image[0x34] = uint8_t(10 * k / 3);
    image[0x35] = uint8_t(31 * k / 3);
    image[0x36] = uint8_t(5 * k / 3);
}

void Config::toExt(uint8_t* out) const {
    out[0] = 'O';
    out[1] = 'T';
    out[2] = 'X';
    out[3] = kExtVersion;
    out[4] = uint8_t(kExtPayload);
    out[5] = cpuMeter;
    out[6] = synthVoices;
    out[7] = extKeep[0];
    out[8] = extKeep[1];
    out[9] = extKeep[2];
    out[10] = exportRate;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < kExtPayload; ++i) sum = uint8_t(sum + out[5 + i]);
    out[5 + kExtPayload] = sum;
}

bool Config::fromExt(const uint8_t* in, uint32_t len) {
    if (len < 6 || in[0] != 'O' || in[1] != 'T' || in[2] != 'X' || in[3] == 0) return false;
    const uint32_t n = in[4];
    if (n == 0 || len < 6 + n) return false;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < n; ++i) sum = uint8_t(sum + in[5 + i]);
    if (sum != in[5 + n]) return false;
    // Newer versions only append fields: read the ones this build knows.
    cpuMeter = in[5] <= 2 ? in[5] : 0;
    synthVoices = (n >= 2 && in[6] >= 1 && in[6] <= kMaxSynthVoices) ? in[6] : kDefaultSynthVoices;
    const Config d;
    for (uint32_t i = 0; i < 3; ++i) extKeep[i] = n >= 3 + i ? in[7 + i] : d.extKeep[i];
    exportRate = (n >= 6 && in[10] <= 1) ? in[10] : 1;
    return true;
}

bool Config::fromImage(const uint8_t* img) {
    if (std::memcmp(img, kSignature, sizeof(kSignature)) != 0) return false;
    Config c;
    std::memcpy(c.image, img, kImageSize);
    uint8_t* base = reinterpret_cast<uint8_t*>(&c);
    for (const Map& m : kMap) base[m.field] = c.image[m.img];
    // Recorder working copies are per project, not in the image.
    std::memcpy(c.recGain, recGain, sizeof(recGain));
    c.recSource = recSource;
    c.recMonitor = recMonitor;
    c.radioFreq10 = radioFreq10;
    c.cpuMeter = cpuMeter;   // extension record, not in the image
    c.synthVoices = synthVoices;
    c.exportRate = exportRate;
    std::memcpy(c.extKeep, extKeep, sizeof(extKeep));
    c.sanitize();
    *this = c;
    toImage();
    return true;
}

bool Config::load(INvStore& nv) {
    uint8_t buf[kImageSize];
    if (nv.size() < kImageSize || !nv.read(0, buf, kImageSize)) return false;
    if (!fromImage(buf)) return false;
    uint8_t ext[kExtSize];
    const Config d;
    cpuMeter = d.cpuMeter;
    synthVoices = d.synthVoices;
    exportRate = d.exportRate;
    std::memcpy(extKeep, d.extKeep, sizeof(extKeep));
    // The longest record that fits the store (older records are shorter;
    // fromExt reads the payload length from the record).
    for (uint32_t len = kExtSize; len >= 7; --len)
        if (nv.size() >= kExtOffset + len && nv.read(kExtOffset, ext, len)) {
            fromExt(ext, len);
            break;
        }
    return true;
}

bool Config::save(INvStore& nv) {
    toImage();
    if (nv.size() < kImageSize || !nv.write(0, image, kImageSize)) return false;
    // Extension record (best effort): written once the option leaves its
    // default; a store that never had one keeps its erased bytes past 0xF4.
    if (nv.size() < kExtOffset + kExtSize) return true;
    uint8_t ext[kExtSize];
    const Config d;
    if (cpuMeter == d.cpuMeter && synthVoices == d.synthVoices && exportRate == d.exportRate &&
        std::memcmp(extKeep, d.extKeep, sizeof(extKeep)) == 0) {
        Config probe;
        if (!nv.read(kExtOffset, ext, kExtSize) || !probe.fromExt(ext, kExtSize)) return true;
    }
    toExt(ext);
    nv.write(kExtOffset, ext, kExtSize);
    return true;
}

bool Config::load(IStorage& fs) {
    IStorage::Handle h = fs.open(kConfigPath, IStorage::kRead);
    if (h < 0) return false;
    uint8_t buf[8 + kImageSize + kExtSize + sizeof(Config)] = {};
    int32_t n = 0, r;
    while (n < int32_t(sizeof(buf)) && (r = fs.read(h, buf + n, uint32_t(sizeof(buf) - n))) > 0) n += r;
    fs.close(h);
    if (n < 8 || std::memcmp(buf, kFileMagic, 4) != 0) return false;
    const uint32_t len = uint32_t(buf[5]) | (uint32_t(buf[6]) << 8);
    const uint32_t avail = uint32_t(n) - 8;
    if (buf[4] == kFileV2) {
        if (len != kImageSize || avail < kImageSize) return false;
        if (!fromImage(buf + 8)) return false;
        const Config d;
        cpuMeter = d.cpuMeter;
        synthVoices = d.synthVoices;
        exportRate = d.exportRate;
        std::memcpy(extKeep, d.extKeep, sizeof(extKeep));
        fromExt(buf + 8 + kImageSize, avail - kImageSize);
        return true;
    }
    if (buf[4] != kFileV1) return false;
    // v1: a copy of the typed fields (an older OpenTracker build).
    Config c;
    uint32_t take = len < avail ? len : avail;
    if (take > offsetof(Config, image)) take = offsetof(Config, image);
    std::memcpy(&c, buf + 8, take);
    c.sanitize();
    *this = c;
    toImage();
    return true;
}

bool Config::save(IStorage& fs) {
    toImage();
    fs.mkdir(kConfigDir);
    uint8_t buf[8 + kImageSize + kExtSize] = {};
    std::memcpy(buf, kFileMagic, 4);
    buf[4] = kFileV2;
    buf[5] = uint8_t(kImageSize);
    buf[6] = uint8_t(kImageSize >> 8);
    std::memcpy(buf + 8, image, kImageSize);
    toExt(buf + 8 + kImageSize);
    if (fs.exists(kConfigPath)) fs.remove(kConfigPath);
    IStorage::Handle h = fs.open(kConfigPath, IStorage::kWrite);
    if (h < 0) return false;
    bool ok = fs.write(h, buf, sizeof(buf)) == int32_t(sizeof(buf));
    fs.close(h);
    return ok;
}

}  // namespace ot
