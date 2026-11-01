// OT3 container (formats/ot3.h).
#include "formats/ot3.h"

#include <cstring>

#include "formats/crc32.h"

namespace ot {
namespace ot3 {

namespace {
constexpr uint8_t kMagic[4] = {'O', 'T', '3', 0};
inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline void wr16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
inline void wr32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
inline uint32_t pad4(uint32_t n) { return (n + 3u) & ~3u; }
}  // namespace

const char* errorText(Error e) {
    switch (e) {
        case Error::kOk: return "ok";
        case Error::kTooSmall: return "too small";
        case Error::kTooLarge: return "too large";
        case Error::kMagic: return "not an OT3 file";
        case Error::kKind: return "wrong kind";
        case Error::kSize: return "size mismatch";
        case Error::kCrc: return "CRC mismatch";
        case Error::kChunk: return "damaged chunk";
        case Error::kNewer: return "newer format";
    }
    return "?";
}

Error Reader::open(const uint8_t* file, uint32_t len, uint16_t kind) {
    p_ = file;
    pos_ = end_ = 0;
    if (!file || len < kHeaderSize + 4) return err_ = Error::kTooSmall;
    if (len > kMaxFile) return err_ = Error::kTooLarge;
    if (std::memcmp(file, kMagic, 4) != 0) return err_ = Error::kMagic;
    version_ = rd16(file + 4);
    if (rd16(file + 6) != kind) return err_ = Error::kKind;
    std::memcpy(writer_, file + 8, 4);
    if (rd32(file + 12) != len || (len & 3u)) return err_ = Error::kSize;
    if (crc32(file, len - 4) != rd32(file + len - 4)) return err_ = Error::kCrc;
    pos_ = kHeaderSize;
    end_ = len - 4;
    return err_ = (version_ > kVersion ? Error::kNewer : Error::kOk);
}

bool Reader::next(Chunk& c) {
    if (!p_ || pos_ >= end_) return false;
    if (end_ - pos_ < kChunkHeaderSize) {
        err_ = Error::kChunk;
        return false;
    }
    const uint8_t* h = p_ + pos_;
    c.tag = rd16(h);
    c.flags = rd16(h + 2);
    c.len = rd32(h + 4);
    const uint32_t body = pos_ + kChunkHeaderSize;
    if (c.len > end_ - body || pad4(c.len) > end_ - body) {
        err_ = Error::kChunk;
        return false;
    }
    c.payload = p_ + body;
    pos_ = body + pad4(c.len);
    return true;
}

void Writer::begin(uint8_t* buf, uint32_t cap, uint16_t kind, const uint8_t writer[4]) {
    buf_ = buf;
    cap_ = cap > kMaxFile ? kMaxFile : cap;
    pos_ = kHeaderSize;
    ok_ = buf && cap_ >= kHeaderSize + 4;
    if (!ok_) return;
    std::memcpy(buf, kMagic, 4);
    wr16(buf + 4, kVersion);
    wr16(buf + 6, kind);
    std::memcpy(buf + 8, writer, 4);
    wr32(buf + 12, 0);
}

bool Writer::chunk(uint16_t tag, uint16_t flags, const void* payload, uint32_t len) {
    return chunk2(tag, flags, payload, len, nullptr, 0);
}

bool Writer::chunk2(uint16_t tag, uint16_t flags, const void* a, uint32_t alen, const void* b, uint32_t blen) {
    if (!ok_) return false;
    const uint32_t len = alen + blen;
    if (len > cap_ || pos_ + kChunkHeaderSize + pad4(len) + 4 > cap_) return ok_ = false;
    uint8_t* h = buf_ + pos_;
    wr16(h, tag);
    wr16(h + 2, flags);
    wr32(h + 4, len);
    uint8_t* d = h + kChunkHeaderSize;
    if (alen) std::memcpy(d, a, alen);
    if (blen) std::memcpy(d + alen, b, blen);
    for (uint32_t i = len; i < pad4(len); ++i) d[i] = 0;
    pos_ += kChunkHeaderSize + pad4(len);
    return true;
}

uint32_t Writer::end() {
    if (!ok_) return 0;
    const uint32_t size = pos_ + 4;
    wr32(buf_ + 12, size);
    wr32(buf_ + pos_, crc32(buf_, pos_));
    ok_ = false;
    return size;
}

}  // namespace ot3
}  // namespace ot
