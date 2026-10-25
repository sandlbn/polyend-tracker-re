// WAV import/export, spec 02 §8.1 and §8.3.
#include "formats/wav.h"

#include <cstring>

#include "formats/bytes.h"
#include "model/constants.h"

namespace ot {

// ---------------------------------------------------------------- readers

int32_t MemReader::read(void* dst, uint32_t n) {
    size_t avail = pos_ < len_ ? len_ - pos_ : 0;
    size_t c = n < avail ? n : avail;
    if (c) std::memcpy(dst, data_ + pos_, c);
    pos_ += c;
    return int32_t(c);
}

bool MemReader::skip(uint32_t n) {
    if (pos_ + n > len_) {
        pos_ = len_;
        return false;
    }
    pos_ += n;
    return true;
}

int32_t StorageReader::read(void* dst, uint32_t n) {
    int32_t r = fs_.read(h_, dst, n);
    if (r > 0) pos_ += uint32_t(r);
    return r;
}

bool StorageReader::skip(uint32_t n) {
    int32_t sz = fs_.size(h_);
    if (sz < 0 || uint64_t(pos_) + n > uint64_t(sz)) return false;
    if (!fs_.seek(h_, pos_ + n)) return false;
    pos_ += n;
    return true;
}

// ----------------------------------------------------------------- parser

bool wavParseHeader(ByteReader& r, WavInfo& info) {
    info = WavInfo{};
    uint8_t b[16];
    // 1. "RIFF" + size, then the form type.
    if (r.read(b, 8) != 8 || std::memcmp(b, "RIFF", 4) != 0) return false;
    if (r.read(b, 4) != 4) return false;
    const bool formWave = std::memcmp(b, "WAVE", 4) == 0;

    // 2. Up to 10 chunks looking for "fmt " (must precede "data").
    bool haveFmt = false;
    for (uint32_t i = 0; i < wav::kMaxChunkScan && !haveFmt; ++i) {
        if (r.read(b, 8) != 8) return false;
        uint32_t size = rdU32(b + 4);
        uint32_t padded = size + (size & 1);
        if (std::memcmp(b, "fmt ", 4) == 0) {
            if (size < 16) return false;
            if (r.read(b, 16) != 16) return false;
            info.format = rdU16(b + 0);
            info.channels = rdU16(b + 2);
            info.sampleRate = rdU32(b + 4);
            info.byteRate = rdU32(b + 8);
            info.blockAlign = rdU16(b + 12);
            info.bitsPerSample = rdU16(b + 14);
            if (padded > 16 && !r.skip(padded - 16)) return false;
            haveFmt = true;
        } else if (!r.skip(padded)) {
            return false;
        }
    }
    if (!haveFmt) {
        info = WavInfo{};
        return false;
    }
    // 3. Up to 10 more chunks looking for "data".
    for (uint32_t i = 0; i < wav::kMaxChunkScan; ++i) {
        if (r.read(b, 8) != 8) break;
        uint32_t size = rdU32(b + 4);
        if (std::memcmp(b, "data", 4) == 0) {
            info.dataSize = size;
            info.dataOffset = r.position();
            info.formWave = formWave;
            info.valid = true;
            return true;
        }
        if (!r.skip(size + (size & 1))) break;
    }
    // 4. Not found: header invalid (all zero).
    info = WavInfo{};
    return false;
}

bool wavParseHeader(const uint8_t* file, size_t len, WavInfo& info) {
    MemReader r(file, len);
    return wavParseHeader(r, info);
}

bool wavIsSupported(const WavInfo& i) {
    if (!i.valid || !i.formWave) return false;
    if (i.channels != 1 && i.channels != 2) return false;
    if (i.sampleRate == 0) return false;  // guard against division by zero
    if (i.format == wav::kFormatPcm)
        return i.bitsPerSample == 8 || i.bitsPerSample == 16 || i.bitsPerSample == 24 || i.bitsPerSample == 32;
    if (i.format == wav::kFormatFloat) return i.bitsPerSample == 32;
    return false;
}

bool wavIsFastPath(const WavInfo& i) {
    return wavIsSupported(i) && i.format == wav::kFormatPcm && i.bitsPerSample == 16 && i.channels == 1 &&
           i.sampleRate == kSampleRate;
}

uint32_t wavBytesPerFrame(const WavInfo& i) { return uint32_t(i.channels) * (i.bitsPerSample / 8u); }

uint32_t wavSourceFrames(const WavInfo& i) {
    uint32_t bpf = wavBytesPerFrame(i);
    return bpf ? i.dataSize / bpf : 0;
}

uint32_t wavResampledFrames(uint32_t srcFrames, uint32_t srcRate) {
    return srcRate ? uint32_t(uint64_t(srcFrames) * kSampleRate / srcRate) : 0;
}

int16_t wavDecodeSample(const uint8_t* p, uint16_t format, uint16_t bits) {
    if (format == wav::kFormatFloat) {
        // (int16)(int)(x * 32767) (spec 07 Q8): truncation; |x| > 1 wraps
        // through the int16 cast instead of saturating. NaN and values
        // outside the int32 range (undefined in C) give 0 (OpenTracker guard).
        const float x = rdF32(p) * 32767.0f;
        if (!(x > -2147483648.0f && x < 2147483648.0f)) return 0;
        return int16_t(uint16_t(uint32_t(int32_t(x))));
    }
    switch (bits) {
        case 8: return int16_t((int(p[0]) - 128) * 256);
        case 16: return rdI16(p);
        case 24: return rdI16(p + 1);  // top 16 bits
        case 32: return rdI16(p + 2);  // top 16 bits
        default: return 0;
    }
}

// --------------------------------------------------------------- resampler

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
constexpr bool kLittleEndian = true;
#else
constexpr bool kLittleEndian = false;
#endif

void CubicResampler::begin(uint32_t srcRate, uint32_t srcFrames, int16_t* dst) {
    *this = CubicResampler{};
    srcRate_ = srcRate ? srcRate : kSampleRate;
    // spec 07 Q8: ratio = srcRate / 44100 in single precision (the same value
    // the per-sample expression gave; computed once here).
    ratio_ = float(srcRate_) / float(kSampleRate);
    srcFrames_ = srcFrames;
    outTotal_ = wavResampledFrames(srcFrames, srcRate_);
    dst_ = dst;
    locate();
}

int32_t CubicResampler::src(int64_t j) const {
    if (j < 0 || uint64_t(j) >= srcFrames_ || uint64_t(j) >= received_) return 0;
    return hist_[j & 3];
}

// Source position of output frame outNext_ (spec 07 Q8): t = i * ratio,
// n = floor(t), mu = t - n. t >= 0, so floor(t) is the truncation of t.
void CubicResampler::locate() {
    const float t = float(outNext_) * ratio_;
    const int64_t n = int64_t(t);
    curN_ = n;
    curMu_ = t - float(n);
    // Ready once source samples n-1..n+2 (clamped to the last one) are in.
    uint64_t need = uint64_t(n < 0 ? 0 : n) + 2;
    if (need >= srcFrames_) need = srcFrames_ - 1;
    curNeed_ = need;
}

void CubicResampler::emitOne() {
    const int64_t n = curN_;
    const float mu = curMu_;
    dst_[outNext_++] = cubicS16(float(src(n - 1)), float(src(n)), float(src(n + 1)), float(src(n + 2)), mu);
    locate();
}

void CubicResampler::pushBlock(const int16_t* s, uint32_t n) {
    for (uint32_t k = 0; k < n; ++k) {
        if (received_ >= srcFrames_) return;
        hist_[received_ & 3] = s[k];
        ++received_;
        while (outNext_ < outTotal_ && curNeed_ < received_) emitOne();
    }
}

void CubicResampler::finish() {
    while (outNext_ < outTotal_) emitOne();
}

// ---------------------------------------------------------------- importer

WavImportPlan wavPlanImport(const WavInfo& info, const WavImportOptions& opt) {
    WavImportPlan p;
    const uint32_t total = wavSourceFrames(info);
    uint32_t s = uint32_t(uint64_t(total) * opt.startMarker / 65535u);
    uint32_t e = uint32_t(uint64_t(total) * opt.endMarker / 65535u);
    if (e < s) e = s;
    p.srcStart = s;
    p.srcFrames = e - s;
    p.outFrames = wavResampledFrames(p.srcFrames, info.sampleRate);
    p.outChannels = (opt.stereo && info.channels == 2) ? 2 : 1;
    return p;
}

FmtStatus WavImporter::begin(const WavInfo& info, const WavImportOptions& opt, int16_t* out,
                             size_t outCapSamples) {
    *this = WavImporter{};
    if (!wavIsSupported(info)) return FmtStatus::Unsupported;
    info_ = info;
    plan_ = wavPlanImport(info, opt);
    if (size_t(plan_.outFrames) * plan_.outChannels > outCapSamples) return FmtStatus::NoSpace;
    bpf_ = wavBytesPerFrame(info);
    out_[0] = out;
    out_[1] = plan_.outChannels == 2 ? out + plan_.outFrames : nullptr;
    // 44.1 kHz: output frame i == source frame i exactly while float(i) is
    // exact (i < 2^24, i.e. well beyond the 0xB40000-byte sample bank).
    direct_ = info.sampleRate == kSampleRate && plan_.outFrames < (1u << 24);
    directRead_ = direct_ && kLittleEndian && info.format == wav::kFormatPcm && info.bitsPerSample == 16 &&
                  info.channels == 1;
    if (!direct_) {
        rs_[0].begin(info.sampleRate, plan_.srcFrames, out);
        if (plan_.outChannels == 2) rs_[1].begin(info.sampleRate, plan_.srcFrames, out + plan_.outFrames);
    }
    return FmtStatus::Ok;
}

// `count` whole source frames starting at frameIdx_. Channel 0 always;
// channel 1 only in stereo-import mode. Mono import of a stereo file keeps
// the left channel only, no averaging (spec 07 Q8).
void WavImporter::frames(const uint8_t* f, uint64_t count) {
    uint64_t idx = frameIdx_;
    frameIdx_ += count;
    const uint64_t lo = plan_.srcStart, hi = lo + plan_.srcFrames;
    if (idx + count <= lo || idx >= hi) return;
    if (idx < lo) {
        f += (lo - idx) * bpf_;
        count -= lo - idx;
        idx = lo;
    }
    if (idx + count > hi) count = hi - idx;
    const bool two = plan_.outChannels == 2;
    const uint16_t fmt = info_.format, bits = info_.bitsPerSample;
    const uint32_t bps = bits / 8u, bpf = bpf_;
    const bool pcm16 = fmt == wav::kFormatPcm && bits == 16;
    if (direct_) {
        const uint32_t k0 = uint32_t(idx - lo);
        int16_t* L = out_[0] + k0;
        int16_t* R = two ? out_[1] + k0 : nullptr;
        if (pcm16 && bpf == 2 && kLittleEndian) {
            std::memcpy(L, f, size_t(count) * 2);
        } else if (pcm16) {
            for (uint64_t i = 0; i < count; ++i, f += bpf) {
                L[i] = rdI16(f);
                if (R) R[i] = rdI16(f + 2);
            }
        } else {
            for (uint64_t i = 0; i < count; ++i, f += bpf) {
                L[i] = wavDecodeSample(f, fmt, bits);
                if (R) R[i] = wavDecodeSample(f + bps, fmt, bits);
            }
        }
        written_ = k0 + uint32_t(count);
        return;
    }
    // Resampler path: decode a block, then push it.
    int16_t tmp[2][128];
    while (count) {
        const uint32_t m = count < 128 ? uint32_t(count) : 128u;
        for (uint32_t i = 0; i < m; ++i, f += bpf) {
            tmp[0][i] = pcm16 ? rdI16(f) : wavDecodeSample(f, fmt, bits);
            if (two) tmp[1][i] = pcm16 ? rdI16(f + 2) : wavDecodeSample(f + bps, fmt, bits);
        }
        rs_[0].pushBlock(tmp[0], m);
        if (two) rs_[1].pushBlock(tmp[1], m);
        count -= m;
    }
}

void WavImporter::feed(const uint8_t* bytes, size_t n) {
    if (!bpf_) return;
    if (carryLen_) {
        while (n && carryLen_ < bpf_) {
            carry_[carryLen_++] = *bytes++;
            --n;
        }
        if (carryLen_ < bpf_) return;
        frames(carry_, 1);
        carryLen_ = 0;
    }
    const size_t whole = n / bpf_;
    if (whole) frames(bytes, whole);
    bytes += whole * bpf_;
    n -= whole * bpf_;
    while (n) {
        carry_[carryLen_++] = *bytes++;
        --n;
    }
}

uint8_t* WavImporter::directBuffer(uint32_t& maxBytes) {
    maxBytes = 0;
    if (!directRead_ || carryLen_ || frameIdx_ < plan_.srcStart || complete()) return nullptr;
    const uint64_t left = uint64_t(plan_.srcStart) + plan_.srcFrames - frameIdx_;
    maxBytes = left * 2 > 0x7FFFFFFFu ? 0x7FFFFFFEu : uint32_t(left * 2);
    return reinterpret_cast<uint8_t*>(out_[0] + (frameIdx_ - plan_.srcStart));
}

void WavImporter::directCommit(uint32_t bytes) {
    const uint32_t k0 = uint32_t(frameIdx_ - plan_.srcStart);
    frameIdx_ += bytes / 2;
    written_ = k0 + bytes / 2;
    if (bytes & 1) {
        // Half a frame: keep its byte for the next feed().
        carry_[0] = reinterpret_cast<const uint8_t*>(out_[0] + written_)[0];
        carryLen_ = 1;
    }
}

uint32_t WavImporter::availableFrames() const {
    uint32_t a;
    if (direct_) {   // 44.1 kHz: output frame i == source frame i, written as fed
        const uint64_t got = frameIdx_ > plan_.srcStart ? frameIdx_ - plan_.srcStart : 0;
        a = got > plan_.outFrames ? plan_.outFrames : uint32_t(got);
    } else {
        a = rs_[0].produced();
        if (plan_.outChannels == 2 && rs_[1].produced() < a) a = rs_[1].produced();
    }
    return a < plan_.outFrames ? a : plan_.outFrames;
}

uint32_t WavImporter::finish() {
    if (direct_) {
        // Source frames that never arrived read as zero (resampler rule).
        for (int c = 0; c < plan_.outChannels; ++c)
            if (out_[c] && written_ < plan_.outFrames)
                std::memset(out_[c] + written_, 0, size_t(plan_.outFrames - written_) * sizeof(int16_t));
        written_ = plan_.outFrames;
        return plan_.outFrames;
    }
    rs_[0].finish();
    if (plan_.outChannels == 2) rs_[1].finish();
    return plan_.outFrames;
}

FmtStatus wavImportMemory(const uint8_t* file, size_t len, const WavImportOptions& opt, int16_t* out,
                          size_t outCapSamples, WavImportResult& res) {
    res = WavImportResult{};
    WavInfo info;
    if (!wavParseHeader(file, len, info)) return FmtStatus::Unsupported;
    WavImporter imp;
    FmtStatus st = imp.begin(info, opt, out, outCapSamples);
    if (st != FmtStatus::Ok) return st;
    size_t avail = info.dataOffset < len ? len - info.dataOffset : 0;
    if (avail > info.dataSize) avail = info.dataSize;
    imp.feed(file + info.dataOffset, avail);
    res.frames = imp.finish();
    res.stereo = imp.plan().outChannels == 2;
    return FmtStatus::Ok;
}

FmtStatus wavImportFile(IStorage& fs, const char* path, const WavImportOptions& opt, int16_t* out,
                        size_t outCapSamples, WavImportResult& res) {
    res = WavImportResult{};
    IStorage::Handle h = fs.open(path, IStorage::kRead);
    if (h < 0) return FmtStatus::NotFound;
    WavInfo info;
    StorageReader r(fs, h);
    if (!wavParseHeader(r, info)) {
        fs.close(h);
        return FmtStatus::Unsupported;
    }
    WavImporter imp;
    FmtStatus st = imp.begin(info, opt, out, outCapSamples);
    if (st != FmtStatus::Ok) {
        fs.close(h);
        return st;
    }
    // The firmware streams 0x7F80-byte chunks; a smaller stack buffer is used
    // here (chunking is not observable in the result).
    uint8_t buf[2048];
    uint32_t left = info.dataSize;
    while (left) {
        uint32_t want = left < sizeof(buf) ? left : uint32_t(sizeof(buf));
        int32_t got = fs.read(h, buf, want);
        if (got <= 0) break;
        imp.feed(buf, size_t(got));
        left -= uint32_t(got);
    }
    fs.close(h);
    res.frames = imp.finish();
    res.stereo = imp.plan().outChannels == 2;
    return FmtStatus::Ok;
}

// ------------------------------------------------------------------ writer

void wavMakeHeader(uint8_t out[wav::kHeaderSize], uint16_t channels, uint32_t dataBytes) {
    const uint16_t blockAlign = uint16_t(channels * 2);
    std::memcpy(out, "RIFF", 4);
    wrU32(out + 4, dataBytes + 36);
    std::memcpy(out + 8, "WAVE", 4);
    std::memcpy(out + 12, "fmt ", 4);
    wrU32(out + 16, 16);
    wrU16(out + 20, wav::kFormatPcm);
    wrU16(out + 22, channels);
    wrU32(out + 24, kSampleRate);
    wrU32(out + 28, kSampleRate * blockAlign);
    wrU16(out + 32, blockAlign);
    wrU16(out + 34, 16);
    std::memcpy(out + 36, "data", 4);
    wrU32(out + 40, dataBytes);
}

size_t wavEncode(const int16_t* interleaved, uint32_t frames, uint16_t channels, uint8_t* out, size_t cap) {
    const size_t data = size_t(frames) * channels * 2;
    if (cap < wav::kHeaderSize + data) return 0;
    wavMakeHeader(out, channels, uint32_t(data));
    for (size_t i = 0; i < size_t(frames) * channels; ++i)
        wrU16(out + wav::kHeaderSize + 2 * i, uint16_t(interleaved[i]));
    return wav::kHeaderSize + data;
}

bool WavWriter::open(IStorage& fs, const char* path, uint16_t channels) {
    if (h_ >= 0 || (channels != 1 && channels != 2)) return false;
    if (fs.exists(path)) fs.remove(path);
    h_ = fs.open(path, IStorage::kWrite);
    if (h_ < 0) return false;
    fs_ = &fs;
    channels_ = channels;
    frames_ = 0;
    uint8_t hdr[wav::kHeaderSize];
    wavMakeHeader(hdr, channels, 0);
    if (fs.write(h_, hdr, sizeof(hdr)) != int32_t(sizeof(hdr))) {
        fs.close(h_);
        h_ = -1;
        return false;
    }
    return true;
}

bool WavWriter::write(const int16_t* interleaved, uint32_t frames) {
    if (h_ < 0) return false;
    uint8_t buf[256];
    size_t total = size_t(frames) * channels_;
    size_t i = 0;
    while (i < total) {
        size_t n = 0;
        for (; n < sizeof(buf) / 2 && i < total; ++n, ++i) wrU16(buf + 2 * n, uint16_t(interleaved[i]));
        if (fs_->write(h_, buf, uint32_t(2 * n)) != int32_t(2 * n)) return false;
    }
    frames_ += frames;
    return true;
}

bool WavWriter::close() {
    if (h_ < 0) return false;
    uint8_t hdr[wav::kHeaderSize];
    wavMakeHeader(hdr, channels_, frames_ * channels_ * 2u);
    bool ok = fs_->seek(h_, 0) && fs_->write(h_, hdr, sizeof(hdr)) == int32_t(sizeof(hdr));
    fs_->close(h_);
    h_ = -1;
    return ok;
}

}  // namespace ot
