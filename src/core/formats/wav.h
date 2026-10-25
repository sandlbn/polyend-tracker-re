// WAV import (parse, decode, cubic resample to 44.1 kHz int16) and 16-bit
// 44.1 kHz WAV writing. spec 02 §8.1, §8.3.
#pragma once

#include <cstddef>
#include <cstdint>

#include "formats/format_common.h"
#include "hal_api.h"

namespace ot {

namespace wav {
constexpr uint32_t kHeaderSize = 44;       // canonical header (spec 02 §8.3)
constexpr uint32_t kMaxChunkScan = 10;     // per phase (spec 02 §8.1)
constexpr uint32_t kStreamChunk = 0x7F80;  // conversion chunk size
constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;
}  // namespace wav

struct WavInfo {
    bool valid = false;        // RIFF + fmt + data found within the scan limits
    bool formWave = false;     // RIFF form type == "WAVE"
    uint16_t format = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint32_t byteRate = 0;
    uint16_t blockAlign = 0;
    uint16_t bitsPerSample = 0;
    uint32_t dataSize = 0;     // bytes, as declared
    uint32_t dataOffset = 0;   // file offset of the first data byte
};

// Sequential byte source for the header parser.
class ByteReader {
public:
    virtual ~ByteReader() = default;
    virtual int32_t read(void* dst, uint32_t n) = 0;   // bytes read
    virtual bool skip(uint32_t n) = 0;
    virtual uint32_t position() const = 0;
};

class MemReader : public ByteReader {
public:
    MemReader(const uint8_t* data, size_t len) : data_(data), len_(len) {}
    int32_t read(void* dst, uint32_t n) override;
    bool skip(uint32_t n) override;
    uint32_t position() const override { return uint32_t(pos_); }

private:
    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
};

class StorageReader : public ByteReader {
public:
    StorageReader(IStorage& fs, IStorage::Handle h) : fs_(fs), h_(h) {}
    int32_t read(void* dst, uint32_t n) override;
    bool skip(uint32_t n) override;
    uint32_t position() const override { return pos_; }

private:
    IStorage& fs_;
    IStorage::Handle h_;
    uint32_t pos_ = 0;
};

// Header parser (spec 02 §8.1 "Parser"). On failure info.valid == false.
bool wavParseHeader(ByteReader& r, WavInfo& info);
bool wavParseHeader(const uint8_t* file, size_t len, WavInfo& info);

// Accepted formats (spec 02 §8.1): WAVE, 1-2 channels, PCM 8/16/24/32 or float 32; any rate.
bool wavIsSupported(const WavInfo& info);
// Fast path: PCM 16-bit mono 44.1 kHz (copied verbatim). The importer also
// skips the resampler for every other 44.1 kHz format (see WavImporter).
bool wavIsFastPath(const WavInfo& info);
uint32_t wavBytesPerFrame(const WavInfo& info);
uint32_t wavSourceFrames(const WavInfo& info);
// Output length = srcFrames * 44100 / srcRate (spec 02 §8.1).
uint32_t wavResampledFrames(uint32_t srcFrames, uint32_t srcRate);
// Decode one sample to int16 (spec 02 §8.1 "Sample decoding").
int16_t wavDecodeSample(const uint8_t* p, uint16_t format, uint16_t bits);

// One output sample of the 4-point cubic between p1 (mu = 0) and p2 (mu = 1):
// truncation toward zero, then clamp (spec 07 Q8). Shared by the WAV import
// (CubicResampler) and the export rate conversion (AudioEngine::exportWav),
// so both interpolate the same way.
inline int16_t cubicS16(float p0, float p1, float p2, float p3, float mu) {
    const float a0 = p3 - p2 - p0 + p1;
    const float a1 = p0 - p1 - a0;
    const float a2 = p2 - p0;
    const float y = ((a0 * mu + a1) * mu + a2) * mu + p1;
    int32_t v = int32_t(y);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return int16_t(v);
}

// Streaming 4-point cubic resampler to 44.1 kHz (spec 02 §8.1). Source
// samples outside [0, srcFrames) read as zero. Writes outputFrames() samples
// to dst over the life of one conversion. The position arithmetic is single
// precision exactly as spec 07 Q8 states (t = i * ratio, n = floor(t),
// mu = t - n); the ratio is computed once per conversion and the next output's
// source position is cached, so a pushed sample costs one integer compare
// unless an output becomes ready.
class CubicResampler {
public:
    void begin(uint32_t srcRate, uint32_t srcFrames, int16_t* dst);
    void push(int16_t s) { pushBlock(&s, 1); }
    void pushBlock(const int16_t* s, uint32_t n);
    // Treat all remaining source samples as zero and emit the rest.
    void finish();
    uint32_t outputFrames() const { return uint32_t(outTotal_); }
    uint32_t produced() const { return uint32_t(outNext_); }

private:
    void locate();      // n / mu / readiness threshold of output outNext_
    void emitOne();
    int32_t src(int64_t j) const;

    int16_t hist_[4] = {};
    uint32_t srcRate_ = 44100;
    float ratio_ = 1.0f;
    uint64_t srcFrames_ = 0;
    uint64_t received_ = 0;
    uint64_t outNext_ = 0;
    uint64_t outTotal_ = 0;
    int64_t curN_ = 0;          // floor(t) of output outNext_
    float curMu_ = 0.0f;        // t - floor(t)
    uint64_t curNeed_ = 0;      // source samples needed (received_ > curNeed_)
    int16_t* dst_ = nullptr;
};

struct WavImportOptions {
    bool stereo = false;           // stereo-import mode: keep 2 channels (planar)
    uint16_t startMarker = 0;      // partial import, 1/65535 units
    uint16_t endMarker = 65535;
};

struct WavImportPlan {
    uint32_t srcStart = 0;     // first source frame used
    uint32_t srcFrames = 0;    // source frames used
    uint32_t outFrames = 0;    // 44.1 kHz frames per channel
    uint8_t outChannels = 1;
};
WavImportPlan wavPlanImport(const WavInfo& info, const WavImportOptions& opt);

// Streaming importer: feed the data-chunk bytes in order (any chunking).
// Output is planar int16: outFrames left samples, then (stereo) outFrames
// right samples. A 44.1 kHz source is decoded straight into the output (the
// resampler at ratio 1 returns each source sample unchanged: t = i exactly
// for i < 2^24, mu = 0), other rates go through CubicResampler in blocks.
class WavImporter {
public:
    FmtStatus begin(const WavInfo& info, const WavImportOptions& opt, int16_t* out, size_t outCapSamples);
    void feed(const uint8_t* bytes, size_t n);
    // Direct-read fast path (16-bit PCM mono 44.1 kHz on a little-endian
    // CPU): the next data bytes may be read straight into the returned output
    // position (up to maxBytes), then reported with directCommit(). Null when
    // the bytes must go through feed().
    uint8_t* directBuffer(uint32_t& maxBytes);
    void directCommit(uint32_t bytes);
    // Every source frame of the plan has been received.
    bool complete() const { return frameIdx_ >= uint64_t(plan_.srcStart) + plan_.srcFrames; }
    // Returns frames per channel written.
    uint32_t finish();
    const WavImportPlan& plan() const { return plan_; }
    // Output frames per channel complete so far (both channels), <= outFrames.
    uint32_t availableFrames() const;

private:
    void frames(const uint8_t* f, uint64_t count);

    WavInfo info_;
    WavImportPlan plan_;
    CubicResampler rs_[2];
    int16_t* out_[2] = {};
    uint8_t carry_[8] = {};
    uint32_t carryLen_ = 0;
    uint32_t bpf_ = 0;
    uint64_t frameIdx_ = 0;
    uint32_t written_ = 0;     // direct path: output frames filled so far
    bool direct_ = false;      // 44.1 kHz: decode without the resampler
    bool directRead_ = false;  // ... and the raw bytes are the output samples
};

struct WavImportResult {
    uint32_t frames = 0;
    bool stereo = false;
};
FmtStatus wavImportMemory(const uint8_t* file, size_t len, const WavImportOptions& opt, int16_t* out,
                          size_t outCapSamples, WavImportResult& res);
FmtStatus wavImportFile(IStorage& fs, const char* path, const WavImportOptions& opt, int16_t* out,
                        size_t outCapSamples, WavImportResult& res);

// --- writing (spec 02 §8.3) ---------------------------------------------

// Canonical 44-byte PCM 16-bit 44.1 kHz header; RIFF size = data + 36.
void wavMakeHeader(uint8_t out[wav::kHeaderSize], uint16_t channels, uint32_t dataBytes);

// Encode interleaved int16 frames to a complete WAV in memory. Returns the
// byte count, 0 if cap is too small.
size_t wavEncode(const int16_t* interleaved, uint32_t frames, uint16_t channels, uint8_t* out, size_t cap);

// Streaming writer: header placeholder on open, patched on close.
class WavWriter {
public:
    bool open(IStorage& fs, const char* path, uint16_t channels);
    bool write(const int16_t* interleaved, uint32_t frames);
    bool close();
    uint32_t frames() const { return frames_; }

private:
    IStorage* fs_ = nullptr;
    IStorage::Handle h_ = -1;
    uint16_t channels_ = 1;
    uint32_t frames_ = 0;
};

}  // namespace ot
