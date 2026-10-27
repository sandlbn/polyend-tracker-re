// SD streaming C14a: WavChunkConverter, the WAV import in bounded windows
// (one output channel per pass), for files larger than the sample memory.
// Its output must be the in-memory importer's (wavImportMemory), byte for
// byte, for every supported format, rate and channel layout, however the
// input is chunked, and a window must never overflow.
#include "test.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "formats/wav.h"

using namespace ot;

namespace {

// A WAV file with a deterministic signal (a sweep plus noise, full scale).
std::vector<uint8_t> makeWav(uint16_t format, uint16_t bits, uint16_t channels, uint32_t rate, uint32_t frames) {
    const uint32_t bps = bits / 8u, bpf = bps * channels, data = frames * bpf;
    std::vector<uint8_t> f(44 + data);
    auto w32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) f[o + i] = uint8_t(v >> (8 * i)); };
    auto w16 = [&](size_t o, uint16_t v) { f[o] = uint8_t(v); f[o + 1] = uint8_t(v >> 8); };
    std::memcpy(&f[0], "RIFF", 4);
    w32(4, 36 + data);
    std::memcpy(&f[8], "WAVEfmt ", 8);
    w32(16, 16);
    w16(20, format);
    w16(22, channels);
    w32(24, rate);
    w32(28, rate * bpf);
    w16(32, uint16_t(bpf));
    w16(34, bits);
    std::memcpy(&f[36], "data", 4);
    w32(40, data);
    uint32_t seed = 12345;
    for (uint32_t i = 0; i < frames; ++i)
        for (uint16_t c = 0; c < channels; ++c) {
            seed = seed * 1664525u + 1013904223u;
            const double x = 0.8 * std::sin(0.0005 * i * i * (c + 1)) + 0.15 * (double(int32_t(seed)) / 2147483648.0);
            uint8_t* p = &f[44 + size_t(i) * bpf + c * bps];
            if (format == wav::kFormatFloat) {
                const float v = float(x);
                std::memcpy(p, &v, 4);
            } else {
                const int64_t full = int64_t(x * double((1LL << (bits - 1)) - 1));
                if (bits == 8) p[0] = uint8_t(full + 128);
                else for (uint32_t b = 0; b < bps; ++b) p[b] = uint8_t(uint64_t(full) >> (8 * b));
            }
        }
    return f;
}

struct Sink {
    std::vector<int16_t> out;
    uint32_t maxChunk = 0;
    static void put(void* ctx, const int16_t* s, uint32_t n) {
        Sink* k = static_cast<Sink*>(ctx);
        k->out.insert(k->out.end(), s, s + n);
        if (n > k->maxChunk) k->maxChunk = n;
    }
};

// One channel through the converter, the data fed in pieces of `step` bytes.
std::vector<int16_t> convert(const std::vector<uint8_t>& file, const WavImportOptions& opt, int channel,
                             uint32_t winFrames, uint32_t step, uint32_t* maxChunk) {
    WavInfo info;
    CHECK(wavParseHeader(file.data(), file.size(), info));
    std::vector<int16_t> win(winFrames + 8, int16_t(0x5A5A));
    WavChunkConverter c;
    CHECK(c.begin(info, opt, channel, win.data(), winFrames) == FmtStatus::Ok);
    Sink k;
    const uint8_t* d = file.data() + info.dataOffset;
    uint32_t left = info.dataSize;
    while (left) {
        const uint32_t n = left < step ? left : step;
        c.feed(d, n, &Sink::put, &k);
        d += n;
        left -= n;
    }
    c.finish(&Sink::put, &k);
    for (uint32_t i = winFrames; i < winFrames + 8; ++i) CHECK_EQ(win[i], int16_t(0x5A5A));   // guard intact
    *maxChunk = k.maxChunk;
    return k.out;
}

}  // namespace

TEST(chunked_conversion_matches_memory_import) {
    struct Fmt { uint16_t format, bits; };
    const Fmt fmts[] = {{wav::kFormatPcm, 8}, {wav::kFormatPcm, 16}, {wav::kFormatPcm, 24},
                        {wav::kFormatPcm, 32}, {wav::kFormatFloat, 32}};
    const uint32_t rates[] = {22050, 44100, 48000, 96000, 8000};
    const uint32_t steps[] = {1, 7, 333, 4096, 100000};
    int cases = 0;
    for (const Fmt& f : fmts)
        for (uint16_t ch : {uint16_t(1), uint16_t(2)})
            for (uint32_t rate : rates)
                for (bool stereo : {false, true}) {
                    const uint32_t frames = 3000 + rate / 50;
                    const std::vector<uint8_t> file = makeWav(f.format, f.bits, ch, rate, frames);
                    WavImportOptions opt;
                    opt.stereo = stereo;
                    if (cases % 3 == 1) {   // a partial import (markers)
                        opt.startMarker = 5000;
                        opt.endMarker = 60000;
                    }
                    std::vector<int16_t> ref(size_t(frames) * 13 + 64);
                    WavImportResult res;
                    CHECK(wavImportMemory(file.data(), file.size(), opt, ref.data(), ref.size(), res) == FmtStatus::Ok);
                    const int outCh = res.stereo ? 2 : 1;
                    for (int c = 0; c < outCh; ++c) {
                        const uint32_t step = steps[(cases + c) % 5];
                        uint32_t maxChunk = 0;
                        const std::vector<int16_t> got = convert(file, opt, c, 512, step, &maxChunk);
                        CHECK_EQ(uint32_t(got.size()), res.frames);
                        const bool same = got.size() == res.frames &&
                                          std::memcmp(got.data(), ref.data() + size_t(c) * res.frames,
                                                      size_t(res.frames) * 2) == 0;
                        if (!same)
                            std::printf("       fmt %u/%u ch %u rate %u stereo %d channel %d step %u differs\n", f.format,
                                        f.bits, ch, rate, int(stereo), c, step);
                        CHECK(same);
                        CHECK(maxChunk <= 512u);
                    }
                    ++cases;
                }
    std::printf("  %d format / rate / layout cases\n", cases);
}

TEST(chunked_output_never_exceeds_window) {
    // A tiny window and a very low source rate (many outputs per input).
    const std::vector<uint8_t> file = makeWav(wav::kFormatPcm, 16, 1, 4000, 2000);
    WavImportOptions opt;
    uint32_t maxChunk = 0;
    const std::vector<int16_t> got = convert(file, opt, 0, 64, 3000, &maxChunk);
    CHECK_EQ(uint32_t(got.size()), wavResampledFrames(2000, 4000));
    CHECK(maxChunk <= 64u);
}

TEST(chunked_truncated_file_pads_with_silence) {
    // A data chunk shorter than declared: the in-memory importer's length,
    // the missing tail as zeros, still window by window.
    std::vector<uint8_t> file = makeWav(wav::kFormatPcm, 16, 1, 22050, 4000);
    file.resize(44 + 2000 * 2);   // half the declared data
    WavInfo info;
    CHECK(wavParseHeader(file.data(), file.size(), info));
    std::vector<int16_t> win(128);
    WavChunkConverter c;
    WavImportOptions opt;
    CHECK(c.begin(info, opt, 0, win.data(), 128) == FmtStatus::Ok);
    Sink k;
    c.feed(file.data() + 44, 2000 * 2, &Sink::put, &k);
    c.finish(&Sink::put, &k);
    CHECK_EQ(uint32_t(k.out.size()), c.outputFrames());
    CHECK(k.maxChunk <= 128u);
    CHECK(k.out.back() == 0);
}
