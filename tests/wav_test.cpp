// WAV parse / convert / write tests (spec 02 §8.1, §8.3).
#include "test.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "formats/bytes.h"
#include "formats/wav.h"
#include "mem_storage.h"

using namespace ot;

// Build a WAV in memory with optional extra chunks.
struct WavBuild {
    uint16_t format = 1, channels = 1, bits = 16;
    uint32_t rate = 44100;
    uint32_t fmtSize = 16;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> before;  // chunks before fmt
    std::vector<std::pair<std::string, std::vector<uint8_t>>> between; // between fmt and data
    std::string form = "WAVE";
    std::vector<uint8_t> data;

    std::vector<uint8_t> build() const {
        std::vector<uint8_t> v;
        auto put = [&](const void* p, size_t n) {
            const uint8_t* b = static_cast<const uint8_t*>(p);
            v.insert(v.end(), b, b + n);
        };
        auto u32 = [&](uint32_t x) {
            uint8_t b[4];
            wrU32(b, x);
            put(b, 4);
        };
        auto u16 = [&](uint16_t x) {
            uint8_t b[2];
            wrU16(b, x);
            put(b, 2);
        };
        auto chunk = [&](const std::string& id, const std::vector<uint8_t>& d) {
            put(id.data(), 4);
            u32(uint32_t(d.size()));
            put(d.data(), d.size());
            if (d.size() & 1) v.push_back(0);
        };
        put("RIFF", 4);
        u32(0);
        put(form.data(), 4);
        for (auto& c : before) chunk(c.first, c.second);
        put("fmt ", 4);
        u32(fmtSize);
        u16(format);
        u16(channels);
        u32(rate);
        u32(rate * channels * bits / 8);
        u16(uint16_t(channels * bits / 8));
        u16(bits);
        for (uint32_t i = 16; i < fmtSize; ++i) v.push_back(0xEE);
        if (fmtSize & 1) v.push_back(0);
        for (auto& c : between) chunk(c.first, c.second);
        chunk("data", data);
        wrU32(v.data() + 4, uint32_t(v.size() - 8));
        return v;
    }
};

static std::vector<uint8_t> pcm16(const std::vector<int16_t>& s) {
    std::vector<uint8_t> d(s.size() * 2);
    for (size_t i = 0; i < s.size(); ++i) wrU16(d.data() + 2 * i, uint16_t(s[i]));
    return d;
}

// Double-precision reference of the spec 02 §8.1 cubic resampler.
static std::vector<int16_t> refResample(const std::vector<int16_t>& src, uint32_t rate) {
    uint32_t outN = uint32_t(uint64_t(src.size()) * 44100 / rate);
    std::vector<int16_t> out(outN);
    auto at = [&](long j) -> double { return (j < 0 || j >= long(src.size())) ? 0.0 : double(src[size_t(j)]); };
    for (uint32_t i = 0; i < outN; ++i) {
        // spec 07 Q8: index and mu in single precision.
        const float tf = float(i) * (float(rate) / 44100.0f);
        long n = long(std::floor(tf));
        double mu = double(tf - float(n));
        double p0 = at(n - 1), p1 = at(n), p2 = at(n + 1), p3 = at(n + 2);
        double a0 = p3 - p2 - p0 + p1, a1 = p0 - p1 - a0, a2 = p2 - p0, a3 = p1;
        double y = a0 * mu * mu * mu + a1 * mu * mu + a2 * mu + a3;
        long v = long(y);   // toward zero, then clamp (spec 07 Q8)
        out[i] = int16_t(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
    return out;
}

static std::vector<int16_t> sine(size_t n, double freq, double rate, double amp = 20000) {
    std::vector<int16_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = int16_t(amp * std::sin(2 * 3.14159265358979323846 * freq * double(i) / rate));
    return v;
}

TEST(parse_basic) {
    WavBuild b;
    b.data = pcm16({1, 2, 3});
    auto f = b.build();
    WavInfo info;
    CHECK(wavParseHeader(f.data(), f.size(), info));
    CHECK(info.valid && info.formWave);
    CHECK_EQ(info.format, 1);
    CHECK_EQ(info.channels, 1);
    CHECK_EQ(info.sampleRate, 44100u);
    CHECK_EQ(info.bitsPerSample, 16);
    CHECK_EQ(info.blockAlign, 2);
    CHECK_EQ(info.dataSize, 6u);
    CHECK_EQ(info.dataOffset, 44u);
    CHECK(wavIsSupported(info));
    CHECK(wavIsFastPath(info));
    CHECK_EQ(wavSourceFrames(info), 3u);
}

TEST(parse_extra_chunks_and_padding) {
    WavBuild b;
    b.fmtSize = 18;  // WAVEFORMATEX cbSize
    b.before.push_back({"JUNK", std::vector<uint8_t>(5, 0x11)});  // odd: padded
    b.between.push_back({"LIST", std::vector<uint8_t>(9, 0x22)});
    b.between.push_back({"fact", std::vector<uint8_t>(4, 0x33)});
    b.data = pcm16({7, 8});
    auto f = b.build();
    WavInfo info;
    CHECK(wavParseHeader(f.data(), f.size(), info));
    CHECK_EQ(info.dataSize, 4u);
    CHECK_EQ(rdI16(f.data() + info.dataOffset), 7);
    CHECK_EQ(info.channels, 1);
}

TEST(parse_failures) {
    WavInfo info;
    WavBuild b;
    b.data = pcm16({1});
    auto f = b.build();
    f[0] = 'X';
    CHECK(!wavParseHeader(f.data(), f.size(), info));
    CHECK(!info.valid);
    CHECK_EQ(info.channels, 0);

    // fmt beyond the 10-chunk scan.
    WavBuild c;
    for (int i = 0; i < 10; ++i) c.before.push_back({"JUNK", {1, 2}});
    c.data = pcm16({1});
    f = c.build();
    CHECK(!wavParseHeader(f.data(), f.size(), info));
    c.before.pop_back();  // 9 junk + fmt = 10 chunks: ok
    f = c.build();
    CHECK(wavParseHeader(f.data(), f.size(), info));

    // data beyond 10 chunks after fmt.
    WavBuild d;
    for (int i = 0; i < 10; ++i) d.between.push_back({"JUNK", {1, 2}});
    d.data = pcm16({1});
    f = d.build();
    CHECK(!wavParseHeader(f.data(), f.size(), info));

    // data before fmt: fmt must come first.
    WavBuild e;
    e.data = pcm16({1});
    auto g = e.build();
    std::vector<uint8_t> swapped(g.begin(), g.begin() + 12);
    swapped.insert(swapped.end(), g.begin() + 36, g.end());       // data chunk
    swapped.insert(swapped.end(), g.begin() + 12, g.begin() + 36);  // fmt chunk
    CHECK(!wavParseHeader(swapped.data(), swapped.size(), info));

    // Truncated.
    CHECK(!wavParseHeader(g.data(), 20, info));
}

TEST(supported_formats) {
    auto check = [](uint16_t fmt, uint16_t ch, uint16_t bits, const char* form, bool expect) {
        WavBuild b;
        b.format = fmt;
        b.channels = ch;
        b.bits = bits;
        b.form = form;
        b.rate = 12345;  // any rate accepted
        auto f = b.build();
        WavInfo info;
        wavParseHeader(f.data(), f.size(), info);
        CHECK_EQ(wavIsSupported(info), expect);
    };
    check(1, 1, 8, "WAVE", true);
    check(1, 2, 16, "WAVE", true);
    check(1, 1, 24, "WAVE", true);
    check(1, 2, 32, "WAVE", true);
    check(3, 1, 32, "WAVE", true);
    check(3, 1, 64, "WAVE", false);
    check(1, 1, 12, "WAVE", false);
    check(1, 3, 16, "WAVE", false);
    check(2, 1, 16, "WAVE", false);  // ADPCM
    check(1, 1, 16, "AVI ", false);
}

TEST(sample_decoding) {
    uint8_t b8[] = {0x00, 0x80, 0xFF};
    CHECK_EQ(wavDecodeSample(b8 + 0, 1, 8), -32768);
    CHECK_EQ(wavDecodeSample(b8 + 1, 1, 8), 0);
    CHECK_EQ(wavDecodeSample(b8 + 2, 1, 8), 127 * 256);
    uint8_t b16[] = {0x34, 0x12};
    CHECK_EQ(wavDecodeSample(b16, 1, 16), 0x1234);
    uint8_t b24[] = {0xAB, 0xCD, 0xEF};  // top 16 bits = 0xEFCD
    CHECK_EQ(wavDecodeSample(b24, 1, 24), int16_t(0xEFCD));
    uint8_t b32[] = {0x11, 0x22, 0x33, 0x44};
    CHECK_EQ(wavDecodeSample(b32, 1, 32), 0x4433);
    uint8_t f[4];
    wrF32(f, 0.5f);
    CHECK_EQ(wavDecodeSample(f, 3, 32), 16383);  // trunc(0.5 * 32767)
    wrF32(f, -1.0f);
    CHECK_EQ(wavDecodeSample(f, 3, 32), -32767);
    wrF32(f, 2.0f);   // past full scale: held at the top
    CHECK_EQ(wavDecodeSample(f, 3, 32), 32767);
    wrF32(f, -0.00001f);
    CHECK_EQ(wavDecodeSample(f, 3, 32), 0);
}

TEST(fast_path_verbatim) {
    auto s = sine(1000, 440, 44100);
    s[3] = -32768;
    s[4] = 32767;
    WavBuild b;
    b.data = pcm16(s);
    auto f = b.build();
    std::vector<int16_t> out(1000);
    WavImportResult r;
    CHECK(wavImportMemory(f.data(), f.size(), {}, out.data(), out.size(), r) == FmtStatus::Ok);
    CHECK_EQ(r.frames, 1000u);
    CHECK(!r.stereo);
    CHECK(out == s);
}

TEST(resample_lengths) {
    CHECK_EQ(wavResampledFrames(22050, 22050), 44100u);
    CHECK_EQ(wavResampledFrames(48000, 48000), 44100u);
    CHECK_EQ(wavResampledFrames(1000, 48000), 918u);
    CHECK_EQ(wavResampledFrames(7, 8000), 38u);
    CHECK_EQ(wavResampledFrames(0, 8000), 0u);
}

static void resampleCase(uint32_t rate, size_t n) {
    auto s = sine(n, 1000, rate, 30000);
    WavBuild b;
    b.rate = rate;
    b.data = pcm16(s);
    auto f = b.build();
    auto ref = refResample(s, rate);
    std::vector<int16_t> out(ref.size() + 1, 0x5555);
    WavImportResult r;
    CHECK(wavImportMemory(f.data(), f.size(), {}, out.data(), out.size(), r) == FmtStatus::Ok);
    CHECK_EQ(r.frames, uint32_t(ref.size()));
    int maxErr = 0;
    for (size_t i = 0; i < ref.size(); ++i) maxErr = std::max(maxErr, std::abs(int(out[i]) - int(ref[i])));
    CHECK(maxErr <= 1);
    CHECK_EQ(out[ref.size()], 0x5555);  // nothing written past the end
}

TEST(resample_up_and_down) {
    resampleCase(22050, 2000);
    resampleCase(8000, 999);
    resampleCase(48000, 4801);
    resampleCase(96000, 3000);
    resampleCase(11025, 17);
    resampleCase(44100, 300);
}

TEST(resample_exact_points) {
    // 22050 -> 44100: even outputs land on source samples exactly (mu = 0).
    std::vector<int16_t> s = {1000, -2000, 3000, -4000, 5000};
    WavBuild b;
    b.rate = 22050;
    b.data = pcm16(s);
    auto f = b.build();
    std::vector<int16_t> out(10);
    WavImportResult r;
    CHECK(wavImportMemory(f.data(), f.size(), {}, out.data(), out.size(), r) == FmtStatus::Ok);
    CHECK_EQ(r.frames, 10u);
    for (int i = 0; i < 5; ++i) CHECK_EQ(out[2 * i], s[size_t(i)]);
    // Midpoint between 1000 and -2000 with p0 = 0 (outside), p3 = 3000:
    // a0 = 3000+2000-0+1000 = 6000, a1 = 0-1000-6000 = -7000, a2 = -2000.
    // y = 6000/8 - 7000/4 - 1000 + 1000 = -1000.
    CHECK_EQ(out[1], -1000);
}

TEST(clamping) {
    std::vector<int16_t> s = {0, 32767, 32767, 0, -32768, -32768, 0};
    WavBuild b;
    b.rate = 11025;
    b.data = pcm16(s);
    auto f = b.build();
    std::vector<int16_t> out(28);
    WavImportResult r;
    CHECK(wavImportMemory(f.data(), f.size(), {}, out.data(), out.size(), r) == FmtStatus::Ok);
    auto ref = refResample(s, 11025);
    bool hitMax = false, hitMin = false;
    for (size_t i = 0; i < ref.size(); ++i) {
        CHECK(std::abs(out[i] - ref[i]) <= 1);
        hitMax = hitMax || out[i] == 32767;
        hitMin = hitMin || out[i] == -32768;
    }
    CHECK(hitMax && hitMin);
}

TEST(stereo_import_planar) {
    const size_t n = 500;
    auto L = sine(n, 300, 44100), R = sine(n, 700, 44100, 10000);
    std::vector<int16_t> inter(2 * n);
    for (size_t i = 0; i < n; ++i) {
        inter[2 * i] = L[i];
        inter[2 * i + 1] = R[i];
    }
    WavBuild b;
    b.channels = 2;
    b.data = pcm16(inter);
    auto f = b.build();
    std::vector<int16_t> out(2 * n);
    WavImportResult r;
    WavImportOptions o;
    o.stereo = true;
    CHECK(wavImportMemory(f.data(), f.size(), o, out.data(), 2 * n - 1, r) == FmtStatus::NoSpace);
    CHECK(wavImportMemory(f.data(), f.size(), o, out.data(), out.size(), r) == FmtStatus::Ok);
    CHECK(r.stereo);
    CHECK_EQ(r.frames, uint32_t(n));
    CHECK(std::equal(L.begin(), L.end(), out.begin()));
    CHECK(std::equal(R.begin(), R.end(), out.begin() + long(n)));
    // Mono import of a stereo file uses the first channel.
    o.stereo = false;
    std::fill(out.begin(), out.end(), 0);
    CHECK(wavImportMemory(f.data(), f.size(), o, out.data(), n, r) == FmtStatus::Ok);
    CHECK(!r.stereo);
    CHECK(std::equal(L.begin(), L.end(), out.begin()));
}

TEST(stereo_24bit_resampled) {
    const size_t n = 960;
    auto L = sine(n, 500, 48000), R = sine(n, 900, 48000, 8000);
    std::vector<uint8_t> d(n * 6);
    for (size_t i = 0; i < n; ++i) {
        d[6 * i + 0] = 0x7F;  // low byte discarded
        wrU16(d.data() + 6 * i + 1, uint16_t(L[i]));
        d[6 * i + 3] = 0x01;
        wrU16(d.data() + 6 * i + 4, uint16_t(R[i]));
    }
    WavBuild b;
    b.channels = 2;
    b.bits = 24;
    b.rate = 48000;
    b.data = d;
    auto f = b.build();
    auto refL = refResample(L, 48000), refR = refResample(R, 48000);
    std::vector<int16_t> out(2 * refL.size());
    WavImportResult r;
    WavImportOptions o;
    o.stereo = true;
    CHECK(wavImportMemory(f.data(), f.size(), o, out.data(), out.size(), r) == FmtStatus::Ok);
    CHECK_EQ(r.frames, uint32_t(refL.size()));
    int maxErr = 0;
    for (size_t i = 0; i < refL.size(); ++i) {
        maxErr = std::max(maxErr, std::abs(out[i] - refL[i]));
        maxErr = std::max(maxErr, std::abs(out[refL.size() + i] - refR[i]));
    }
    CHECK(maxErr <= 1);
}

TEST(eight_bit_and_float) {
    WavBuild b;
    b.bits = 8;
    b.data = {0, 64, 128, 192, 255};
    auto f = b.build();
    std::vector<int16_t> out(5);
    WavImportResult r;
    CHECK(wavImportMemory(f.data(), f.size(), {}, out.data(), out.size(), r) == FmtStatus::Ok);
    CHECK(out == std::vector<int16_t>({-32768, -16384, 0, 16384, 32512}));

    WavBuild c;
    c.format = 3;
    c.bits = 32;
    c.data.resize(12);
    wrF32(c.data.data(), 1.0f);
    wrF32(c.data.data() + 4, -0.5f);
    wrF32(c.data.data() + 8, 0.25f);
    f = c.build();
    out.assign(3, 0);
    CHECK(wavImportMemory(f.data(), f.size(), {}, out.data(), out.size(), r) == FmtStatus::Ok);
    CHECK(out == std::vector<int16_t>({32767, -16383, 8191}));
}

TEST(partial_import_markers) {
    std::vector<int16_t> s(1000);
    for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t(i);
    WavBuild b;
    b.data = pcm16(s);
    auto f = b.build();
    WavImportOptions o;
    o.startMarker = 65535 / 4;  // frame 249
    o.endMarker = 65535 / 2;    // frame 499
    std::vector<int16_t> out(1000);
    WavImportResult r;
    CHECK(wavImportMemory(f.data(), f.size(), o, out.data(), out.size(), r) == FmtStatus::Ok);
    CHECK_EQ(r.frames, 250u);
    CHECK_EQ(out[0], 249);
    CHECK_EQ(out[249], 498);
    WavInfo info;
    wavParseHeader(f.data(), f.size(), info);
    WavImportPlan p = wavPlanImport(info, o);
    CHECK_EQ(p.srcStart, 249u);
    CHECK_EQ(p.srcFrames, 250u);
}

TEST(streaming_chunking_equivalent) {
    auto s = sine(3001, 1234, 32000);
    std::vector<int16_t> inter(2 * s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        inter[2 * i] = s[i];
        inter[2 * i + 1] = int16_t(-s[i]);
    }
    WavBuild b;
    b.channels = 2;
    b.rate = 32000;
    b.data = pcm16(inter);
    auto f = b.build();
    WavInfo info;
    CHECK(wavParseHeader(f.data(), f.size(), info));
    WavImportOptions o;
    o.stereo = true;
    std::vector<int16_t> whole(2 * wavResampledFrames(3001, 32000)), chunked(whole.size());
    WavImportResult r;
    CHECK(wavImportMemory(f.data(), f.size(), o, whole.data(), whole.size(), r) == FmtStatus::Ok);
    WavImporter imp;
    CHECK(imp.begin(info, o, chunked.data(), chunked.size()) == FmtStatus::Ok);
    const uint8_t* d = f.data() + info.dataOffset;
    size_t left = info.dataSize, step = 1;
    while (left) {
        size_t c = std::min(left, step);
        imp.feed(d, c);
        d += c;
        left -= c;
        step = step % 7 + 1;  // odd chunk sizes split frames
    }
    CHECK_EQ(imp.finish(), r.frames);
    CHECK(whole == chunked);
}

TEST(truncated_data_zero_padded) {
    auto s = sine(400, 100, 22050);
    WavBuild b;
    b.rate = 22050;
    b.data = pcm16(s);
    auto f = b.build();
    f.resize(f.size() - 200);  // declared size larger than the file
    std::vector<int16_t> out(800, 0x7777);
    WavImportResult r;
    CHECK(wavImportMemory(f.data(), f.size(), {}, out.data(), out.size(), r) == FmtStatus::Ok);
    CHECK_EQ(r.frames, 800u);
    CHECK_EQ(out[799], 0);
    CHECK_EQ(out[0], s[0]);
}

TEST(header_writer_golden) {
    uint8_t h[44];
    wavMakeHeader(h, 1, 1000);
    CHECK_BYTES(h, 'R', 'I', 'F', 'F', 0x0C, 0x04, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1,
                0, 0x44, 0xAC, 0, 0, 0x88, 0x58, 0x01, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0xE8, 0x03, 0, 0);
    wavMakeHeader(h, 2, 0);
    CHECK_EQ(rdU32(h + 4), 36u);
    CHECK_EQ(rdU16(h + 22), 2);
    CHECK_EQ(rdU32(h + 28), 176400u);
    CHECK_EQ(rdU16(h + 32), 4);
}

TEST(encode_then_import_round_trip) {
    std::vector<int16_t> inter = {1, -1, 2, -2, 32767, -32768};
    std::vector<uint8_t> f(44 + 12);
    CHECK_EQ(wavEncode(inter.data(), 3, 2, f.data(), 10), size_t(0));
    CHECK_EQ(wavEncode(inter.data(), 3, 2, f.data(), f.size()), size_t(56));
    std::vector<int16_t> out(6);
    WavImportResult r;
    WavImportOptions o;
    o.stereo = true;
    CHECK(wavImportMemory(f.data(), f.size(), o, out.data(), out.size(), r) == FmtStatus::Ok);
    CHECK(out == std::vector<int16_t>({1, 2, 32767, -1, -2, -32768}));
}

TEST(writer_and_file_import) {
    MemStorage fs;
    auto s = sine(5000, 440, 44100);
    std::vector<int16_t> inter(2 * s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        inter[2 * i] = s[i];
        inter[2 * i + 1] = int16_t(s[i] / 2);
    }
    fs.files["Export/x.wav"] = {1, 2, 3};  // replaced
    WavWriter w;
    CHECK(w.open(fs, "Export/x.wav", 2));
    CHECK(w.write(inter.data(), 1000));
    CHECK(w.write(inter.data() + 2000, 4000));
    CHECK(w.close());
    CHECK_EQ(w.frames(), 5000u);
    auto& f = fs.files["Export/x.wav"];
    CHECK_EQ(f.size(), size_t(44 + 20000));
    CHECK_EQ(rdU32(f.data() + 40), 20000u);
    CHECK_EQ(rdU32(f.data() + 4), 20036u);
    std::vector<int16_t> out(10000);
    WavImportResult r;
    WavImportOptions o;
    o.stereo = true;
    CHECK(wavImportFile(fs, "Export/x.wav", o, out.data(), out.size(), r) == FmtStatus::Ok);
    CHECK(r.stereo);
    CHECK_EQ(r.frames, 5000u);
    CHECK(std::equal(s.begin(), s.end(), out.begin()));
    CHECK_EQ(out[5000 + 17], int16_t(s[17] / 2));
    CHECK(wavImportFile(fs, "missing.wav", o, out.data(), out.size(), r) == FmtStatus::NotFound);
    fs.files["bad.wav"] = {'R', 'I', 'F', 'F'};
    CHECK(wavImportFile(fs, "bad.wav", o, out.data(), out.size(), r) == FmtStatus::Unsupported);
}

// ---- 2.x loading performance: the block / 44.1 kHz / direct-read importer
// paths must give exactly the samples of the 1.9.3 per-sample importer
// (copied below as the reference: per-sample float position, push + emit).
namespace {
struct OldResampler {
    int16_t hist[4] = {};
    uint32_t rate = 44100;
    uint64_t frames = 0, received = 0, next = 0, total = 0;
    bool ended = false;
    int16_t* dst = nullptr;
    void begin(uint32_t r, uint32_t f, int16_t* d) {
        *this = OldResampler{};
        rate = r ? r : 44100;
        frames = f;
        total = wavResampledFrames(f, rate);
        dst = d;
    }
    int32_t src(int64_t j) const { return (j < 0 || uint64_t(j) >= frames || uint64_t(j) >= received) ? 0 : hist[j & 3]; }
    int64_t index(uint64_t i, float* mu) const {
        const float ratio = float(rate) / float(44100);
        const float t = float(i) * ratio;
        const float n = std::floor(t);
        if (mu) *mu = t - n;
        return int64_t(n);
    }
    bool ready(uint64_t i) const {
        if (ended) return true;
        const int64_t ni = index(i, nullptr);
        uint64_t need = (ni < 0 ? 0 : uint64_t(ni)) + 2;
        if (need >= frames) need = frames - 1;
        return need < received;
    }
    void emit() {
        while (next < total && ready(next)) {
            float mu = 0;
            const int64_t n = index(next, &mu);
            const float p0 = float(src(n - 1)), p1 = float(src(n)), p2 = float(src(n + 1)), p3 = float(src(n + 2));
            const float a0 = p3 - p2 - p0 + p1, a1 = p0 - p1 - a0, a2 = p2 - p0, a3 = p1;
            const float y = ((a0 * mu + a1) * mu + a2) * mu + a3;
            int32_t v = int32_t(y);
            v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
            dst[next++] = int16_t(v);
        }
    }
    void push(int16_t s) {
        if (received >= frames) return;
        hist[received & 3] = s;
        ++received;
        emit();
    }
    void finish() {
        ended = true;
        emit();
    }
};

std::vector<int16_t> oldImport(const WavInfo& info, const WavImportOptions& opt, const uint8_t* data, size_t n) {
    const WavImportPlan p = wavPlanImport(info, opt);
    std::vector<int16_t> out(size_t(p.outFrames) * p.outChannels + 1, 0x5A5A);
    OldResampler rs[2];
    rs[0].begin(info.sampleRate, p.srcFrames, out.data());
    if (p.outChannels == 2) rs[1].begin(info.sampleRate, p.srcFrames, out.data() + p.outFrames);
    const uint32_t bpf = wavBytesPerFrame(info), bps = info.bitsPerSample / 8u;
    for (uint64_t k = 0; (k + 1) * bpf <= n; ++k) {
        if (k < p.srcStart || k >= uint64_t(p.srcStart) + p.srcFrames) continue;
        const uint8_t* f = data + k * bpf;
        rs[0].push(wavDecodeSample(f, info.format, info.bitsPerSample));
        if (p.outChannels == 2) rs[1].push(wavDecodeSample(f + bps, info.format, info.bitsPerSample));
    }
    rs[0].finish();
    if (p.outChannels == 2) rs[1].finish();
    return out;
}
}  // namespace

TEST(block_and_direct_importer_match_per_sample_reference) {
    uint32_t seed = 12345;
    auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    const uint32_t rates[] = {44100, 44100, 48000, 22050, 32000, 96000, 8000, 11025};
    const uint16_t bitsList[] = {8, 16, 16, 24, 32};
    int cases = 0, bad = 0;
    for (int it = 0; it < 120; ++it) {
        WavBuild b;
        b.rate = rates[rnd() % 8];
        const bool flt = rnd() % 6 == 0;
        b.format = flt ? 3 : 1;
        b.bits = flt ? 32 : bitsList[rnd() % 5];
        b.channels = uint16_t(1 + rnd() % 2);
        const uint32_t frames = 1 + rnd() % 3000;
        const uint32_t bpf = b.channels * b.bits / 8u;
        b.data.resize(size_t(frames) * bpf);
        for (size_t i = 0; i < b.data.size(); ++i) b.data[i] = uint8_t(rnd());
        if (flt)
            for (size_t i = 0; i + 4 <= b.data.size(); i += 4) {
                const float x = float(int(rnd() % 4001) - 2000) / 1700.0f;
                std::memcpy(&b.data[i], &x, 4);
            }
        auto file = b.build();
        if (rnd() % 4 == 0) file.resize(file.size() - std::min<size_t>(file.size() - 60, 1 + rnd() % 200));
        WavInfo info;
        CHECK(wavParseHeader(file.data(), file.size(), info));
        WavImportOptions opt;
        opt.stereo = rnd() % 2;
        if (rnd() % 3 == 0) {
            opt.startMarker = uint16_t(rnd() % 30000);
            opt.endMarker = uint16_t(35000 + rnd() % 30535);
        }
        const size_t avail = std::min<size_t>(file.size() - info.dataOffset, info.dataSize);
        const std::vector<int16_t> want = oldImport(info, opt, file.data() + info.dataOffset, avail);
        // New importer: random chunking, direct reads when offered.
        std::vector<int16_t> got(want.size(), 0x5A5A);
        WavImporter imp;
        CHECK(imp.begin(info, opt, got.data(), got.size() - 1) == FmtStatus::Ok);
        const uint8_t* d = file.data() + info.dataOffset;
        size_t left = avail;
        while (left) {
            uint32_t maxb = 0;
            uint8_t* direct = imp.directBuffer(maxb);
            size_t c = std::min<size_t>(left, 1 + rnd() % 5000);
            if (direct && rnd() % 2) {
                c = std::min<size_t>(c, maxb);
                std::memcpy(direct, d, c);
                imp.directCommit(uint32_t(c));
            } else {
                imp.feed(d, c);
            }
            d += c;
            left -= c;
        }
        CHECK_EQ(imp.finish(), wavPlanImport(info, opt).outFrames);
        ++cases;
        if (got != want) ++bad;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(cases, 120);
}
