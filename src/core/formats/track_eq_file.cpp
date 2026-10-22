#include "formats/track_eq_file.h"

#include <cstdio>

#include "formats/bytes.h"
#include "formats/crc32.h"
#include "formats/file_io.h"

namespace ot {

namespace {
constexpr uint32_t kHeader = 8;
constexpr uint32_t kBandBytes = 8;
constexpr uint32_t kBody = kHeader + uint32_t(audio::kVoices) * uint32_t(audio::kEqBands) * kBandBytes;
static_assert(kBody + 4 == kTrackEqFileSize, "track EQ file layout");
}  // namespace

bool trackEqFilePath(char* out, size_t cap, const char* base) {
    const int n = std::snprintf(out, cap, "%s/%s", base, kTrackEqFileName);
    return n > 0 && size_t(n) < cap;
}

void encodeTrackEq(const audio::TrackEqSet& s, uint8_t* out) {
    out[0] = 'O';
    out[1] = 'T';
    out[2] = 'E';
    out[3] = 'Q';
    out[4] = kTrackEqFileVersion;
    out[5] = uint8_t(audio::kVoices);
    out[6] = uint8_t(audio::kEqBands);
    out[7] = 0;
    uint8_t* p = out + kHeader;
    for (int t = 0; t < audio::kVoices; ++t)
        for (int i = 0; i < audio::kEqBands; ++i) {
            audio::EqBand b = s.track[t].band[i];
            audio::eqSanitize(b);
            if (!audio::eqSlopeAllowed(i, b.type)) b.slope = 0;
            p[0] = b.type;
            p[1] = uint8_t((b.on ? 1 : 0) | (b.slope ? 2 : 0));
            wrU16(p + 2, b.freqHz);
            wrU16(p + 4, uint16_t(b.gainDb10));
            wrU16(p + 6, b.q100);
            p += kBandBytes;
        }
    wrU32(out + kBody, crc32(out, kBody));
}

FmtStatus decodeTrackEq(const uint8_t* data, uint32_t len, audio::TrackEqSet& s) {
    if (len < kTrackEqFileSize) return FmtStatus::TooShort;
    if (data[0] != 'O' || data[1] != 'T' || data[2] != 'E' || data[3] != 'Q') return FmtStatus::BadMagic;
    if (rdU32(data + kBody) != crc32(data, kBody)) return FmtStatus::BadMagic;
    if (data[4] != kTrackEqFileVersion || data[5] != audio::kVoices || data[6] != audio::kEqBands) return FmtStatus::Unsupported;
    audio::TrackEqSet r;
    const uint8_t* p = data + kHeader;
    for (int t = 0; t < audio::kVoices; ++t)
        for (int i = 0; i < audio::kEqBands; ++i) {
            audio::EqBand& b = r.track[t].band[i];
            b.type = p[0];
            b.on = (p[1] & 1) != 0;
            b.slope = (p[1] & 2) ? 1 : 0;
            b.freqHz = rdU16(p + 2);
            b.gainDb10 = rdI16(p + 4);
            b.q100 = rdU16(p + 6);
            audio::eqSanitize(b);
            if (!audio::eqSlopeAllowed(i, b.type)) b.slope = 0;
            p += kBandBytes;
        }
    s = r;
    return FmtStatus::Ok;
}

FmtStatus loadTrackEqFile(IStorage& fs, const char* path, audio::TrackEqSet& s) {
    uint8_t buf[kTrackEqFileSize];
    uint32_t got = 0;
    const FmtStatus st = readWholeFile(fs, path, buf, kTrackEqFileSize, &got);
    if (st != FmtStatus::Ok) return st;
    return decodeTrackEq(buf, got, s);
}

FmtStatus saveTrackEqFile(IStorage& fs, const char* path, const audio::TrackEqSet& s) {
    uint8_t buf[kTrackEqFileSize];
    encodeTrackEq(s, buf);
    return writeWholeFile(fs, path, buf, kTrackEqFileSize);
}

}  // namespace ot
