// Listening test for the Sample Editor speed work: renders effect settings
// through the old (sample_fx_ref.cpp) and the new (app/sample_fx.cpp) code to
// WAV files, before / after side by side, plus a blind A / B pair per case
// with the answer in key.txt.
//
//   make -C tests sfx-render                       built-in kick, tonal mix, drum loop
//   ../build/bench/sfx_render OUT_DIR [IN.wav ...]  also your own 16-bit WAVs
//
// Stereo files are processed per channel, as the Sample Editor does.
// Per case also the difference new - old: __diff.wav at its real level and
// __diff_x64.wav amplified 64 times (+36 dB) to hear what kind of change it is;
// levels.txt lists its RMS below the signal's and below full scale.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "app/sample_fx.h"
#include "sample_fx_ref.h"

using namespace ot;

namespace {

struct Audio {
    std::string name;
    int channels = 1;
    std::vector<std::vector<int16_t>> ch;   // planar
};

uint32_t g_rng = 1;
int noise() {
    g_rng = g_rng * 1664525u + 1013904223u;
    return int(g_rng >> 16) - 32768;
}
int16_t clip16(double v) { return int16_t(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

Audio kick() {
    Audio a{"kick", 1, {std::vector<int16_t>(19845)}};
    double ph = 0.0;
    for (size_t i = 0; i < a.ch[0].size(); ++i) {
        const double t = double(i) / 44100.0;
        ph += 2.0 * 3.14159265358979 * (45.0 + 120.0 * std::exp(-t * 30.0)) / 44100.0;
        a.ch[0][i] = int16_t(28000.0 * std::exp(-t * 7.0) * std::sin(ph));
    }
    return a;
}

Audio mix() {
    Audio a{"mix", 1, {std::vector<int16_t>(6 * 44100)}};
    for (size_t i = 0; i < a.ch[0].size(); ++i) {
        const double t = double(i) / 44100.0;
        const double f = (int(t * 2.0) % 2) ? 220.0 : 164.81;
        a.ch[0][i] = clip16(9000.0 * std::sin(2.0 * 3.14159265358979 * f * t) +
                            6000.0 * std::sin(2.0 * 3.14159265358979 * f * 1.5 * t) + 0.08 * noise());
    }
    return a;
}

Audio loop() {
    Audio a{"loop", 1, {std::vector<int16_t>(4 * 88200, 0)}};
    const uint32_t beat = 22050;
    for (uint32_t b = 0; b < 16; ++b)
        for (uint32_t i = 0; i < beat; ++i) {
            const double t = double(i) / 44100.0;
            double v = 0.15 * noise() * std::exp(-t * 60.0);
            if (b % 2 == 0) v += 26000.0 * std::exp(-t * 12.0) * std::sin(2.0 * 3.14159265358979 * 55.0 * t);
            else v += 0.6 * noise() * std::exp(-t * 18.0);
            a.ch[0][b * beat + i] = clip16(double(a.ch[0][b * beat + i]) + v);
        }
    return a;
}

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

bool readWav(const char* path, Audio& a) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + got);
    std::fclose(f);
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return false;
    int ch = 0, bits = 0;
    for (size_t p = 12; p + 8 <= d.size();) {
        const uint32_t len = rd32(&d[p + 4]);
        if (!std::memcmp(&d[p], "fmt ", 4) && len >= 16) {
            ch = rd16(&d[p + 10]);
            bits = rd16(&d[p + 22]);
        } else if (!std::memcmp(&d[p], "data", 4)) {
            if (bits != 16 || ch < 1 || ch > 2) return false;
            const size_t frames = std::min<size_t>(len, d.size() - p - 8) / (2 * ch);
            a.channels = ch;
            a.ch.assign(ch, std::vector<int16_t>(frames));
            for (size_t i = 0; i < frames; ++i)
                for (int c = 0; c < ch; ++c) a.ch[c][i] = int16_t(rd16(&d[p + 8 + 2 * (i * ch + c)]));
            std::string n = path;
            const size_t s = n.find_last_of('/');
            if (s != std::string::npos) n = n.substr(s + 1);
            if (n.size() > 4) n = n.substr(0, n.size() - 4);
            a.name = n;
            return frames > 0;
        }
        p += 8 + len + (len & 1);
    }
    return false;
}

void wr32(FILE* f, uint32_t v) {
    const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    std::fwrite(b, 1, 4, f);
}
void wr16(FILE* f, uint16_t v) {
    const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
    std::fwrite(b, 1, 2, f);
}

void writeWav(const std::string& path, const std::vector<std::vector<int16_t>>& ch) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "sfx_render: cannot write %s\n", path.c_str());
        std::exit(1);
    }
    const uint32_t c = uint32_t(ch.size()), n = uint32_t(ch[0].size());
    std::fwrite("RIFF", 1, 4, f);
    wr32(f, 36 + n * c * 2);
    std::fwrite("WAVEfmt ", 1, 8, f);
    wr32(f, 16);
    wr16(f, 1);
    wr16(f, uint16_t(c));
    wr32(f, 44100);
    wr32(f, 44100 * c * 2);
    wr16(f, uint16_t(c * 2));
    wr16(f, 16);
    std::fwrite("data", 1, 4, f);
    wr32(f, n * c * 2);
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t k = 0; k < c; ++k) wr16(f, uint16_t(ch[k][i]));
    std::fclose(f);
}

struct Case {
    const char* name;
    int fx;
    std::vector<std::pair<int, float>> set;
};
const Case kCases[] = {
    {"eq-typical", sfx::kEqualizer, {{0, 6}, {3, -3}, {6, 4}}},
    {"eq-10hz-plus12-narrow", sfx::kEqualizer, {{0, 12}, {1, 10}, {2, 0.5f}}},
    {"eq-scoop", sfx::kEqualizer, {{0, -12}, {1, 200}, {3, 12}, {4, 100}, {6, -12}, {7, 20000}}},
    {"eq-bass-boost-60hz", sfx::kEqualizer, {{0, 12}, {1, 60}, {2, 1}}},
    {"limiter-minus20", sfx::kLimiter, {{0, -20}}},
    {"compressor-minus30-8to1", sfx::kCompressor, {{0, -30}, {1, 8}}},
    {"timestretch-beat-default", sfx::kStretchBeat, {}},
};

std::vector<std::vector<int16_t>> render(bool fresh, int fx, const float* p, const Audio& a) {
    std::vector<std::vector<int16_t>> out;
    for (const auto& in : a.ch) {
        const uint32_t m = sfx::outputFrames(fx, p, uint32_t(in.size()));
        std::vector<int16_t> o(m);
        const uint32_t w = fresh ? sfx::process(fx, p, in.data(), uint32_t(in.size()), o.data(), m)
                                 : sfx_ref::process(fx, p, in.data(), uint32_t(in.size()), o.data(), m);
        o.resize(w);
        out.push_back(o);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: sfx_render OUT_DIR [IN.wav ...]\n");
        return 2;
    }
    const std::string dir = argv[1];
    std::vector<Audio> inputs = {kick(), mix(), loop()};
    for (int i = 2; i < argc; ++i) {
        Audio a;
        if (!readWav(argv[i], a)) {
            std::fprintf(stderr, "sfx_render: %s is not a 16-bit mono / stereo WAV\n", argv[i]);
            return 1;
        }
        inputs.push_back(a);
    }
    FILE* key = std::fopen((dir + "/key.txt").c_str(), "w");
    if (!key) {
        std::fprintf(stderr, "sfx_render: cannot write to %s\n", dir.c_str());
        return 1;
    }
    std::fprintf(key, "Blind pairs: which of A / B is the new (float) code.\n\n");
    FILE* levels = std::fopen((dir + "/levels.txt").c_str(), "w");
    if (!levels) return 1;
    std::fprintf(levels, "Difference new - old per case (16-bit dither / rounding noise is about -101 dBFS RMS).\n\n");
    uint32_t coin = 12345;
    for (const Audio& a : inputs) {
        writeWav(dir + "/" + a.name + "__original.wav", a.ch);
        for (const Case& c : kCases) {
            float p[sfx::kMaxParams];
            sfx::defaults(c.fx, p);
            for (const auto& kv : c.set) p[kv.first] = kv.second;
            const auto before = render(false, c.fx, p, a), after = render(true, c.fx, p, a);
            int maxAbs = 0;
            double sig2 = 0.0, dif2 = 0.0;
            size_t count = 0;
            std::vector<std::vector<int16_t>> diff(before.size()), diff64(before.size());
            for (size_t k = 0; k < before.size(); ++k) {
                const size_t len = std::min(before[k].size(), after[k].size());
                diff[k].resize(len);
                diff64[k].resize(len);
                for (size_t i = 0; i < len; ++i) {
                    const int e = int(after[k][i]) - int(before[k][i]);
                    maxAbs = std::max(maxAbs, std::abs(e));
                    sig2 += double(before[k][i]) * before[k][i];
                    dif2 += double(e) * e;
                    ++count;
                    diff[k][i] = int16_t(e);
                    diff64[k][i] = clip16(64.0 * e);
                }
            }
            const double sigRms = std::sqrt(sig2 / double(count ? count : 1));
            const double difRms = std::sqrt(dif2 / double(count ? count : 1));
            const std::string base = dir + "/" + a.name + "__" + c.name;
            writeWav(base + "__before.wav", before);
            writeWav(base + "__after.wav", after);
            writeWav(base + "__diff.wav", diff);
            writeWav(base + "__diff_x64.wav", diff64);
            if (difRms > 0.0)
                std::fprintf(levels, "%-40s max %3d LSB  diff RMS %7.3f LSB  = %6.1f dB below the signal, %6.1f dBFS\n",
                             (a.name + "__" + c.name).c_str(), maxAbs, difRms, 20.0 * std::log10(sigRms / difRms),
                             20.0 * std::log10(difRms / 32768.0));
            else
                std::fprintf(levels, "%-40s identical\n", (a.name + "__" + c.name).c_str());
            coin = coin * 1103515245u + 12345u;
            const bool newIsA = (coin >> 16) & 1;
            writeWav(base + "__A.wav", newIsA ? after : before);
            writeWav(base + "__B.wav", newIsA ? before : after);
            std::fprintf(key, "%-40s new = %s   (max difference %d LSB)\n", (a.name + "__" + c.name).c_str(),
                         newIsA ? "A" : "B", maxAbs);
            std::printf("%-40s max difference %5d LSB\n", (a.name + "__" + c.name).c_str(), maxAbs);
        }
    }
    std::fclose(key);
    std::fclose(levels);
    return 0;
}
