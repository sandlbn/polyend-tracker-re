// File > Import Mod (MOD / IT) and Export As .it (spec 11), with the stock
// defects fixed (reimpl/README.md "Differences from the original").
// All modules here are synthetic, built byte by byte from the public
// ProTracker / ITTECH layouts.
#include <cmath>
#include <string>
#include <vector>

#include "test.h"
#include "mem_storage.h"
#include "ui_fakes.h"

#include "formats/it_export.h"
#include "formats/it_import.h"
#include "formats/mod_import.h"
#include "model/fx.h"
#include "ui/screens.h"

using namespace ot;

namespace {

// ---- import target backed by plain arrays ---------------------------------
struct Target {
    Project proj;
    Instrument ins[kSampleInstrumentCount];
    bool present[kSampleInstrumentCount] = {};
    std::vector<int16_t> pool = std::vector<int16_t>(1 << 21);
    size_t used = 0;
    size_t limit = 1 << 21;
    int16_t* ptr[kSampleInstrumentCount] = {};
    uint32_t frames[kSampleInstrumentCount] = {};
    bool stereo[kSampleInstrumentCount] = {};
    std::vector<PatternStock> pats = std::vector<PatternStock>(kPatternCount);
    bool patPresent[kPatternCount + 1] = {};
    std::vector<uint8_t> scratch = std::vector<uint8_t>(0x10000);

    ModuleTarget target() {
        ModuleTarget t;
        t.project = &proj;
        t.instruments = ins;
        t.present = present;
        t.ctx = this;
        t.reserve = [](void* c, int slot, uint32_t fr, bool st) -> int16_t* {
            auto* s = static_cast<Target*>(c);
            const size_t n = size_t(fr) * (st ? 2 : 1);
            if (s->used + n > s->limit) return nullptr;
            s->ptr[slot] = s->pool.data() + s->used;
            s->frames[slot] = fr;
            s->stereo[slot] = st;
            s->used += n;
            return s->ptr[slot];
        };
        t.pattern = [](void* c, int n) -> PatternStock* {
            auto* s = static_cast<Target*>(c);
            s->patPresent[n] = true;
            return &s->pats[n - 1];
        };
        return t;
    }
    PatternStock& pat(int n) { return pats[n - 1]; }
    Step& step(int n, int track, int row) { return pats[n - 1].tracks[track].steps[row]; }
};

template <class Imp>
ImportStep runAll(Imp& imp) {
    uint32_t cost = 0;
    for (int guard = 0; guard < 1000000; ++guard) {
        const ImportStep r = imp.step(cost);
        if (r != ImportStep::More) return r;
    }
    return ImportStep::Failed;
}

void be16(std::vector<uint8_t>& v, size_t off, uint16_t x) {
    v[off] = uint8_t(x >> 8);
    v[off + 1] = uint8_t(x);
}

// ---- synthetic ProTracker module ------------------------------------------
struct ModBuilder {
    int nSmp = 31;
    int ch = 4;
    std::string sig = "M.K.";
    struct S {
        std::string name;
        uint8_t fine = 0, vol = 64;
        uint16_t rs = 0, rl = 1;
        std::vector<int8_t> data;
    } smp[31];
    uint8_t songLen = 1;
    uint8_t orders[128] = {};
    std::vector<std::vector<uint8_t>> pats;

    void cell(int p, int r, int c, uint16_t period, uint8_t sample, uint8_t eff, uint8_t par) {
        while (int(pats.size()) <= p) pats.push_back(std::vector<uint8_t>(64 * ch * 4, 0));
        uint8_t* b = pats[p].data() + (r * ch + c) * 4;
        b[0] = uint8_t((sample & 0xF0) | ((period >> 8) & 0x0F));
        b[1] = uint8_t(period);
        b[2] = uint8_t(((sample & 0x0F) << 4) | (eff & 0x0F));
        b[3] = par;
    }
    std::vector<uint8_t> build() {
        if (pats.empty()) pats.push_back(std::vector<uint8_t>(64 * ch * 4, 0));
        const size_t hdr = 20 + 30 * nSmp + 2 + 128 + (nSmp == 31 ? 4 : 0);
        std::vector<uint8_t> v(hdr, 0);
        std::memcpy(v.data(), "synthetic", 9);
        for (int k = 0; k < nSmp; ++k) {
            const size_t o = 20 + 30 * k;
            std::memcpy(v.data() + o, smp[k].name.c_str(), smp[k].name.size() < 22 ? smp[k].name.size() : 22);
            be16(v, o + 22, uint16_t(smp[k].data.size() / 2));
            v[o + 24] = smp[k].fine;
            v[o + 25] = smp[k].vol;
            be16(v, o + 26, smp[k].rs);
            be16(v, o + 28, smp[k].rl);
        }
        const size_t ob = 20 + 30 * nSmp;
        v[ob] = songLen;
        v[ob + 1] = 127;
        std::memcpy(v.data() + ob + 2, orders, 128);
        if (nSmp == 31) std::memcpy(v.data() + ob + 130, sig.data(), 4);
        int maxPat = 0;
        for (int i = 0; i < 128; ++i) maxPat = orders[i] > maxPat ? orders[i] : maxPat;
        for (int p = 0; p <= maxPat; ++p) {
            if (p < int(pats.size())) v.insert(v.end(), pats[p].begin(), pats[p].end());
            else v.insert(v.end(), size_t(64 * ch * 4), uint8_t(0));
        }
        for (int k = 0; k < nSmp; ++k)
            for (int8_t x : smp[k].data) v.push_back(uint8_t(x));
        return v;
    }
};

bool importMod(MemStorage& fs, Target& t, ModImport& imp, const std::vector<uint8_t>& file) {
    fs.files["Mod/test.mod"] = file;
    if (!imp.begin(fs, "Mod/test.mod", t.scratch.data(), uint32_t(t.scratch.size()))) return false;
    imp.apply(t.target());
    return runAll(imp) == ImportStep::Done;
}

// ---- IT 2.14 / 2.15 sample compressor (test only) --------------------------
struct BitWriter {
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    void put(uint32_t v, int w) {
        acc |= (v & ((1u << w) - 1)) << bits;
        bits += w;
        while (bits >= 8) {
            out.push_back(uint8_t(acc));
            acc >>= 8;
            bits -= 8;
        }
    }
    void flush() {
        if (bits) out.push_back(uint8_t(acc));
        acc = 0;
        bits = 0;
    }
};

// Encodes one channel; uses widths 4, 8/16 and 9/17 and every switch method.
std::vector<uint8_t> itCompress(const std::vector<int16_t>& x, bool is16, bool it215) {
    std::vector<uint8_t> file;
    const size_t blockFrames = is16 ? 0x4000 : 0x8000;
    const int maxW = is16 ? 17 : 9, midW = is16 ? 16 : 8;
    for (size_t b0 = 0; b0 < x.size(); b0 += blockFrames) {
        BitWriter bw;
        int width = maxW;
        int32_t prev = 0, prevD = 0;
        const size_t end = b0 + blockFrames < x.size() ? b0 + blockFrames : x.size();
        for (size_t i = b0; i < end; ++i) {
            const int32_t cur = is16 ? x[i] : int8_t(x[i] >> 8);
            int32_t d = cur - prev;
            prev = cur;
            int32_t e = it215 ? d - prevD : d;
            prevD = d;
            // Wrap into the sample word.
            e = is16 ? int16_t(uint16_t(e)) : int8_t(uint8_t(e));
            const int lim = is16 ? 32759 : 123;
            int t = (e >= -7 && e <= 7) ? 4 : ((e >= -lim - 1 && e <= lim) ? midW : maxW);
            if (t != width) {
                if (width == maxW) {
                    bw.put((1u << (maxW - 1)) | uint32_t(t - 1), maxW);
                } else if (width == midW) {
                    const uint32_t border = ((is16 ? 0xFFFFu : 0xFFu) >> (maxW - midW)) - (is16 ? 8u : 4u);
                    const int n = t < width ? t : t - 1;
                    bw.put(border + uint32_t(n), midW);
                } else {   // width 4: method 1
                    bw.put(1u << 3, 4);
                    const int n = t < width ? t : t - 1;
                    bw.put(uint32_t(n - 1), is16 ? 4 : 3);
                }
                width = t;
            }
            bw.put(uint32_t(e) & ((1u << width) - 1) & (width == maxW ? (1u << (maxW - 1)) - 1 : 0xFFFFFFFFu), width);
        }
        bw.flush();
        file.push_back(uint8_t(bw.out.size()));
        file.push_back(uint8_t(bw.out.size() >> 8));
        file.insert(file.end(), bw.out.begin(), bw.out.end());
    }
    return file;
}

// ---- synthetic Impulse Tracker module --------------------------------------
void le16(std::vector<uint8_t>& v, size_t o, uint16_t x) {
    v[o] = uint8_t(x);
    v[o + 1] = uint8_t(x >> 8);
}
void le32(std::vector<uint8_t>& v, size_t o, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[o + i] = uint8_t(x >> (8 * i));
}

struct ItBuilder {
    bool instruments = false;
    uint16_t cmwt = 0x0214;
    uint8_t is = 6, it = 125;
    std::string magic = "IMPM";
    std::vector<uint8_t> orders{0, 0xFF};
    struct Ins {
        std::string name;
        uint8_t gbv = 128;
        std::vector<std::pair<uint8_t, uint8_t>> keys;   // (note, sample)
    };
    std::vector<Ins> ins;
    struct Smp {
        std::string name;
        uint8_t gvl = 64, flg = 1, vol = 64, cvt = 1;
        uint32_t len = 0, lb = 0, le = 0, c5 = 44100, slb = 0, sle = 0;
        std::vector<uint8_t> data;   // raw bytes as stored
        bool present = true;         // non-zero pointer
    };
    std::vector<Smp> smp;
    struct Cell {
        int row, ch;
        int note = -1, ins = -1, vol = -1, cmd = -1, par = 0;
        uint8_t mask = 0;   // 0: computed from the fields; else used as is (memory bits)
    };
    struct Pat {
        int rows = 64;
        std::vector<Cell> cells;
        bool null = false;
    };
    std::vector<Pat> pats;

    std::vector<uint8_t> packPattern(const Pat& p) {
        std::vector<uint8_t> d;
        for (int r = 0; r < p.rows; ++r) {
            for (const Cell& c : p.cells) {
                if (c.row != r) continue;
                uint8_t m = c.mask;
                if (!m) m = uint8_t((c.note >= 0 ? 1 : 0) | (c.ins >= 0 ? 2 : 0) | (c.vol >= 0 ? 4 : 0) | (c.cmd >= 0 ? 8 : 0));
                d.push_back(uint8_t((c.ch + 1) | 0x80));
                d.push_back(m);
                if (m & 1) d.push_back(uint8_t(c.note));
                if (m & 2) d.push_back(uint8_t(c.ins));
                if (m & 4) d.push_back(uint8_t(c.vol));
                if (m & 8) {
                    d.push_back(uint8_t(c.cmd));
                    d.push_back(uint8_t(c.par));
                }
            }
            d.push_back(0);
        }
        return d;
    }

    std::vector<uint8_t> build() {
        const size_t ordNum = orders.size(), insNum = instruments ? ins.size() : 0, smpNum = smp.size(),
                     patNum = pats.size();
        const size_t hdr = 0xC0 + ordNum + 4 * (insNum + smpNum + patNum);
        std::vector<uint8_t> v(hdr, 0);
        std::memcpy(v.data(), magic.data(), 4);
        std::memcpy(v.data() + 4, "synthetic it", 12);
        le16(v, 0x20, uint16_t(ordNum));
        le16(v, 0x22, uint16_t(insNum));
        le16(v, 0x24, uint16_t(smpNum));
        le16(v, 0x26, uint16_t(patNum));
        le16(v, 0x28, 0x0214);
        le16(v, 0x2A, cmwt);
        le16(v, 0x2C, uint16_t(0x09 | (instruments ? 4 : 0)));
        v[0x30] = 128;
        v[0x31] = 48;
        v[0x32] = is;
        v[0x33] = it;
        for (size_t i = 0; i < ordNum; ++i) v[0xC0 + i] = orders[i];
        size_t tab = 0xC0 + ordNum;
        for (size_t i = 0; i < insNum; ++i) {
            le32(v, tab + 4 * i, uint32_t(v.size()));
            std::vector<uint8_t> b(554, 0);
            std::memcpy(b.data(), "IMPI", 4);
            b[0x18] = ins[i].gbv;
            std::memcpy(b.data() + 0x20, ins[i].name.data(), ins[i].name.size());
            for (int n = 0; n < 120; ++n) b[0x40 + 2 * n] = uint8_t(n);
            for (auto& k : ins[i].keys) b[0x41 + 2 * k.first] = k.second;
            v.insert(v.end(), b.begin(), b.end());
        }
        tab += 4 * insNum;
        std::vector<size_t> hdrAt(smpNum);
        for (size_t s = 0; s < smpNum; ++s) {
            if (!smp[s].present) continue;
            le32(v, tab + 4 * s, uint32_t(v.size()));
            hdrAt[s] = v.size();
            std::vector<uint8_t> b(80, 0);
            std::memcpy(b.data(), "IMPS", 4);
            b[0x11] = smp[s].gvl;
            b[0x12] = smp[s].flg;
            b[0x13] = smp[s].vol;
            std::memcpy(b.data() + 0x14, smp[s].name.data(), smp[s].name.size());
            b[0x2E] = smp[s].cvt;
            le32(b, 0x30, smp[s].len);
            le32(b, 0x34, smp[s].lb);
            le32(b, 0x38, smp[s].le);
            le32(b, 0x3C, smp[s].c5);
            le32(b, 0x40, smp[s].slb);
            le32(b, 0x44, smp[s].sle);
            v.insert(v.end(), b.begin(), b.end());
        }
        tab += 4 * smpNum;
        for (size_t s = 0; s < smpNum; ++s) {
            if (!smp[s].present) continue;
            le32(v, hdrAt[s] + 0x48, uint32_t(v.size()));
            v.insert(v.end(), smp[s].data.begin(), smp[s].data.end());
        }
        for (size_t k = 0; k < patNum; ++k) {
            if (pats[k].null) continue;
            le32(v, tab + 4 * k, uint32_t(v.size()));
            std::vector<uint8_t> d = packPattern(pats[k]);
            std::vector<uint8_t> h(8, 0);
            le16(h, 0, uint16_t(d.size()));
            le16(h, 2, uint16_t(pats[k].rows));
            v.insert(v.end(), h.begin(), h.end());
            v.insert(v.end(), d.begin(), d.end());
        }
        return v;
    }
};

bool importIt(MemStorage& fs, Target& t, ItImport& imp, const std::vector<uint8_t>& file) {
    fs.files["Mod/test.it"] = file;
    if (!imp.begin(fs, "Mod/test.it", t.scratch.data(), uint32_t(t.scratch.size()))) return false;
    imp.apply(t.target());
    return runAll(imp) == ImportStep::Done;
}

std::vector<uint8_t> bytes8(const std::vector<int16_t>& s) {
    std::vector<uint8_t> v;
    for (int16_t x : s) v.push_back(uint8_t(x >> 8));
    return v;
}
std::vector<uint8_t> bytes16(const std::vector<int16_t>& s) {
    std::vector<uint8_t> v;
    for (int16_t x : s) {
        v.push_back(uint8_t(x));
        v.push_back(uint8_t(uint16_t(x) >> 8));
    }
    return v;
}

}  // namespace

// ===========================================================================
// Shared helpers (spec 11 §1.1)

TEST(list_filter_kind_and_project_name) {
    CHECK(moduleListFilter("song.MOD"));
    CHECK(moduleListFilter("tune.it"));
    CHECK(moduleListFilter("my.mod.bak"));   // substring test, not a suffix test
    CHECK(!moduleListFilter("kick.wav"));
    CHECK(moduleKind("a.it.mod") == ModuleKind::Mod);
    CHECK(moduleKind("A.IT") == ModuleKind::It);
    CHECK(moduleKind("x.xm") == ModuleKind::None);
    char n[40];
    moduleProjectName("my.song.mod", n, sizeof(n));
    CHECK_STR(n, "my");
    moduleProjectName("abcdefghijabcdefghijabcdefghijabcdefghij.mod", n, sizeof(n));
    CHECK_EQ(int(std::strlen(n)), 32);
}

TEST(period_to_note_with_full_length_samples) {
    // Fix: index + 7 for full-length 16-bit data (stock: index - 5 with
    // half-length data). ProTracker C-1 856 -> 19, C-2 428 -> 31, C-3 214 -> 43.
    CHECK_EQ(modfmt::periodToNote(856), 19);
    CHECK_EQ(modfmt::periodToNote(428), 31);
    CHECK_EQ(modfmt::periodToNote(214), 43);
    CHECK_EQ(modfmt::periodToNote(113), 54);
    // Extended octave (stock dropped 1712..1357) and finetuned periods.
    CHECK_EQ(modfmt::periodToNote(1712), 7);
    CHECK_EQ(modfmt::periodToNote(1357), 11);
    CHECK_EQ(modfmt::periodToNote(57), 66);
    CHECK_EQ(modfmt::periodToNote(425), 31);   // finetune +1 period of C-2
    CHECK_EQ(modfmt::periodToNote(431), 31);
    CHECK_EQ(modfmt::periodToNote(0), -1);
    // Pitch check: note 31 at the engine's unity note 60 = 44100 Hz plays
    // at 44100 * 2^(-29/12) ~ 8270 Hz, the Amiga rate of period 428.
    CHECK_NEAR(44100.0 * std::pow(2.0, (31 - 60) / 12.0), 7093789.2 / (2 * 428), 40.0);
}

TEST(finetune_and_tempo_conversions) {
    CHECK_EQ(int(modfmt::finetuneCents(0)), 0);
    CHECK_EQ(int(modfmt::finetuneCents(1)), 13);
    CHECK_EQ(int(modfmt::finetuneCents(7)), 88);
    CHECK_EQ(int(modfmt::finetuneCents(8)), -100);
    CHECK_EQ(int(modfmt::finetuneCents(15)), -13);
    CHECK_NEAR(modfmt::trackerBpm(6, 125), 125.0, 1e-4);
    CHECK_NEAR(modfmt::trackerBpm(3, 125), 250.0, 1e-4);
    CHECK_NEAR(modfmt::trackerBpm(4, 150), 225.0, 1e-4);
    // Tempo FX v means 2v BPM (spec 03 §3.6): 125 BPM -> 63 (stock wrote 125 = 250 BPM).
    CHECK_EQ(int(modfmt::tempoFxValue(125)), 63);
    CHECK_EQ(int(modfmt::tempoFxValue(250)), 125);
    CHECK_EQ(int(modfmt::tempoFxValue(2)), 5);       // never 4 = STOP
    CHECK_EQ(int(modfmt::tempoFxValue(1000)), 200);
    CHECK_EQ(int(modfmt::positionFxValue(1024, 2048)), 50);
    CHECK_EQ(int(modfmt::positionFxValue(4096, 2048)), 100);
    CHECK_EQ(int(modfmt::positionFxValue(10, 0)), 0);
}

// ===========================================================================
// MOD import (spec 11 §1.2)

TEST(mod_mk_instruments_and_samples) {
    ModBuilder m;
    m.smp[0].name = "lead";
    m.smp[0].vol = 64;
    m.smp[0].fine = 1;
    m.smp[0].data = {-128, -1, 0, 1, 127, 64, 2, 3};
    m.smp[0].rs = 1;
    m.smp[0].rl = 2;   // words 1..3 of 4
    m.smp[1].vol = 32;
    m.smp[1].data = {10, 20};
    m.smp[2].vol = 99;   // > 64 clamps
    m.cell(0, 0, 0, 428, 1, 0, 0);
    MemStorage fs;
    Target t;
    ModImport imp;
    CHECK(importMod(fs, t, imp, m.build()));
    CHECK_EQ(imp.channels(), 4);
    CHECK_EQ(imp.sampleCount(), 31);
    CHECK_STR(t.ins[0].name, "lead");
    CHECK_STR(t.ins[1].name, "-");
    CHECK_EQ(int(t.ins[0].volume), 50);
    CHECK_EQ(int(t.ins[1].volume), 25);
    CHECK_EQ(int(t.ins[2].volume), 50);
    CHECK_EQ(int(t.ins[0].finetune), 13);   // finetune is used (stock ignored it)
    CHECK(t.present[30]);
    CHECK(!t.present[31]);
    // Full-length 8 -> 16 bit conversion (stock: half-length raw frames).
    CHECK_EQ(t.ins[0].lengthFrames, 8u);
    CHECK_EQ(t.frames[0], 8u);
    CHECK_EQ(int(t.ptr[0][0]), -32768);
    CHECK_EQ(int(t.ptr[0][1]), -256);
    CHECK_EQ(int(t.ptr[0][4]), 127 * 256);
    CHECK_EQ(int(t.ptr[1][1]), 20 * 256);
    // Loop: repeat 1..3 of 4 words -> forward loop points.
    CHECK_EQ(int(t.ins[0].playMode), int(PlayMode::ForwardLoop));
    CHECK_EQ(int(t.ins[0].loopPoint1), 65535 / 4);
    CHECK_EQ(int(t.ins[0].loopPoint2), 3 * 65535 / 4);
    CHECK_EQ(int(t.ins[1].playMode), int(PlayMode::OneShot));
    CHECK_EQ(int(t.ins[1].loopPoint1), 1);
}

TEST(mod_notes_instruments_and_tracks) {
    ModBuilder m;
    m.smp[0].data = std::vector<int8_t>(2048, 1);
    m.smp[2].data = std::vector<int8_t>(64, 1);
    m.cell(0, 0, 0, 428, 1, 0, 0);
    m.cell(0, 1, 0, 214, 0, 0, 0);    // no sample: channel's last instrument
    m.cell(0, 0, 1, 428, 0, 0, 0);    // never set: instrument 48 (stock)
    m.cell(0, 2, 2, 0, 3, 0, 0);      // sample without note: sets the channel instrument
    m.cell(0, 3, 2, 856, 0, 0, 0);
    m.cell(0, 4, 3, 1712, 1, 0, 0);   // extended octave
    MemStorage fs;
    Target t;
    ModImport imp;
    CHECK(importMod(fs, t, imp, m.build()));
    CHECK_EQ(int(t.step(1, 0, 0).note), 31);
    CHECK_EQ(int(t.step(1, 0, 0).instrument), 0);
    CHECK_EQ(int(t.step(1, 0, 1).note), 43);
    CHECK_EQ(int(t.step(1, 0, 1).instrument), 0);
    CHECK_EQ(int(t.step(1, 1, 0).instrument), 47);
    CHECK_EQ(int(t.step(1, 2, 2).note), int(kNoteEmpty));
    CHECK_EQ(int(t.step(1, 2, 3).note), 19);
    CHECK_EQ(int(t.step(1, 2, 3).instrument), 2);
    CHECK_EQ(int(t.step(1, 3, 4).note), 7);
    for (int tr = 0; tr < 8; ++tr) CHECK_EQ(int(t.pat(1).tracks[tr].lastStep), 63);
    CHECK_EQ(int(t.pat(1).tracks[8].lastStep), kDefaultTrackLastStep);
}

TEST(mod_effects_mapping) {
    ModBuilder m;
    m.smp[0].data = std::vector<int8_t>(2048, 0);
    m.cell(0, 0, 0, 428, 1, 0x3, 0xFF);   // tone porta -> Glide, clamped 100
    m.cell(0, 1, 0, 428, 0, 0x3, 0x20);
    m.cell(0, 2, 0, 428, 0, 0x9, 0x04);   // offset 4*256 of 2048 -> 50 %
    m.cell(0, 3, 0, 0, 0, 0xC, 0x40);     // volume 64 -> 50
    m.cell(0, 4, 0, 0, 0, 0xC, 0x20);     // 32 -> 25
    m.cell(0, 5, 0, 0, 0, 0xC, 0x50);     // > 64 clamps
    m.cell(0, 6, 0, 0, 0, 0xD, 0x10);     // break, value 1
    m.cell(0, 7, 0, 0, 0, 0xA, 0x0F);     // volume slide: dropped
    m.cell(0, 8, 0, 0, 0, 0xE, 0x91);     // retrigger: dropped
    m.cell(0, 9, 0, 0, 0, 0x0, 0x37);     // arpeggio: dropped
    m.cell(0, 10, 0, 0, 0, 0xF, 0x7D);    // tempo 125 at speed 6
    m.cell(0, 11, 0, 0, 0, 0xF, 0x03);    // speed 3 -> 250 BPM
    m.cell(0, 12, 0, 0, 0, 0xF, 0x20);    // F20 is a tempo (32), not a speed
    m.cell(0, 13, 0, 0, 0, 0xF, 0x00);    // F00: dropped
    MemStorage fs;
    Target t;
    ModImport imp;
    CHECK(importMod(fs, t, imp, m.build()));
    auto fx = [&](int row) { return t.step(1, 0, row).fx[1]; };
    CHECK_EQ(int(fx(0).type), int(FxId::Glide));
    CHECK_EQ(int(fx(0).value), 100);
    CHECK_EQ(int(fx(1).value), 0x20);
    CHECK_EQ(int(fx(2).type), int(FxId::Position));
    CHECK_EQ(int(fx(2).value), 50);
    CHECK_EQ(int(fx(3).type), int(FxId::Volume));
    CHECK_EQ(int(fx(3).value), 50);
    CHECK_EQ(int(fx(4).value), 25);
    CHECK_EQ(int(fx(5).value), 50);
    CHECK_EQ(int(fx(6).type), int(FxId::BreakPattern));
    CHECK_EQ(int(fx(6).value), 1);
    CHECK_EQ(int(fx(7).type), 0);
    CHECK_EQ(int(fx(8).type), 0);
    CHECK_EQ(int(fx(9).type), 0);
    CHECK_EQ(int(fx(10).type), int(FxId::Tempo));
    CHECK_EQ(int(fx(10).value), 63);    // 125 BPM, not doubled
    CHECK_EQ(int(fx(11).value), 125);   // 6*125/3 = 250 BPM
    CHECK_EQ(int(fx(12).value), 32);    // 6*32/3 = 64 BPM
    CHECK_EQ(int(fx(13).type), 0);
    CHECK_EQ(int(t.step(1, 0, 0).fx[0].type), 0);   // FX slot 0 stays empty
    CHECK_NEAR(t.proj.tempo(), 125.0, 1e-3);
}

TEST(mod_song_order_and_tempo_carry) {
    ModBuilder m;
    m.songLen = 3;
    m.orders[0] = 0;
    m.orders[1] = 2;
    m.orders[2] = 1;
    m.cell(0, 0, 0, 0, 0, 0xF, 0x03);   // song starts at speed 3 -> 250 BPM
    m.cell(0, 5, 1, 0, 0, 0xF, 0x06);   // back to 125 BPM before pattern 2
    m.cell(1, 0, 0, 428, 1, 0, 0);
    m.cell(2, 0, 0, 428, 1, 0, 0);
    MemStorage fs;
    Target t;
    ModImport imp;
    CHECK(importMod(fs, t, imp, m.build()));
    // Song = the MOD song length (stock: all 128 orders, trailing "pattern 1").
    CHECK_EQ(int(t.proj.playlist[0]), 1);
    CHECK_EQ(int(t.proj.playlist[1]), 3);
    CHECK_EQ(int(t.proj.playlist[2]), 2);
    CHECK_EQ(int(t.proj.playlist[3]), 0);
    CHECK_EQ(int(t.proj.playlist[127]), 0);
    CHECK_NEAR(t.proj.tempo(), 250.0, 1e-3);   // project tempo = start BPM
    // Pattern 3 (MOD 2) is entered at 125 BPM: restated on row 0.
    CHECK_EQ(int(t.step(3, 0, 0).fx[0].type), int(FxId::Tempo));
    CHECK_EQ(int(t.step(3, 0, 0).fx[0].value), 63);
    CHECK_EQ(int(t.step(2, 0, 0).fx[0].type), int(FxId::Tempo));
    CHECK_EQ(int(t.step(1, 0, 0).fx[0].type), 0);
    CHECK_EQ(int(t.step(1, 0, 0).fx[1].value), 125);
}

TEST(mod_one_pattern_sample_offset) {
    // Stock reads max(1, max order) + 1 patterns, so a one-pattern module
    // had its samples shifted by a pattern.
    ModBuilder m;
    m.smp[0].data = {11, 22, 33, 44};
    MemStorage fs;
    Target t;
    ModImport imp;
    CHECK(importMod(fs, t, imp, m.build()));
    CHECK_EQ(imp.patternCount(), 1);
    CHECK_EQ(int(t.ptr[0][0]), 11 * 256);
    CHECK_EQ(int(t.ptr[0][3]), 44 * 256);
}

TEST(mod_15_sample_layout) {
    ModBuilder m;
    m.nSmp = 15;
    m.smp[0].data = {5, 6, 7, 8};
    m.smp[14].data = {-5, -6};
    m.cell(0, 0, 0, 428, 1, 0, 0);
    m.cell(0, 63, 3, 214, 15, 0, 0);
    MemStorage fs;
    Target t;
    ModImport imp;
    CHECK(importMod(fs, t, imp, m.build()));
    CHECK_EQ(imp.sampleCount(), 15);
    CHECK_EQ(int(t.step(1, 0, 0).note), 31);
    CHECK_EQ(int(t.step(1, 3, 63).note), 43);
    CHECK_EQ(int(t.step(1, 3, 63).instrument), 14);
    CHECK_EQ(int(t.ptr[0][0]), 5 * 256);
    CHECK_EQ(int(t.ptr[14][1]), -6 * 256);
    CHECK(!t.present[15]);
}

TEST(mod_signatures_and_channels) {
    for (const char* sig : {"M!K!", "FLT4", "4CHN"}) {
        ModBuilder m;
        m.sig = sig;
        m.cell(0, 0, 3, 428, 1, 0, 0);
        MemStorage fs;
        Target t;
        ModImport imp;
        CHECK(importMod(fs, t, imp, m.build()));
        CHECK_EQ(imp.channels(), 4);
        CHECK_EQ(int(t.step(1, 3, 0).note), 31);
    }
    for (const char* sig : {"8CHN", "FLT8", "OKTA"}) {
        ModBuilder m;
        m.sig = sig;
        m.ch = 8;
        m.cell(0, 1, 7, 214, 2, 0, 0);
        MemStorage fs;
        Target t;
        ModImport imp;
        CHECK(importMod(fs, t, imp, m.build()));
        CHECK_EQ(imp.channels(), 8);
        CHECK_EQ(int(t.step(1, 7, 1).note), 43);
        CHECK_EQ(int(t.step(1, 7, 1).instrument), 1);
    }
    {
        ModBuilder m;
        m.sig = "6CHN";
        m.ch = 6;
        m.cell(0, 0, 5, 428, 1, 0, 0);
        MemStorage fs;
        Target t;
        ModImport imp;
        CHECK(importMod(fs, t, imp, m.build()));
        CHECK_EQ(int(t.step(1, 5, 0).note), 31);
    }
    {
        // More channels than sample tracks: refused, nothing changed.
        ModBuilder m;
        m.sig = "10CH";
        m.ch = 10;
        MemStorage fs;
        Target t;
        ModImport imp;
        fs.files["Mod/x.mod"] = m.build();
        CHECK(!imp.begin(fs, "Mod/x.mod", t.scratch.data(), uint32_t(t.scratch.size())));
        CHECK(std::strstr(imp.error(), "10-channel") != nullptr);
    }
    {
        MemStorage fs;
        Target t;
        ModImport imp;
        std::vector<uint8_t> junk(2000, 0xEE);
        fs.files["Mod/j.mod"] = junk;
        CHECK(!imp.begin(fs, "Mod/j.mod", t.scratch.data(), uint32_t(t.scratch.size())));
        CHECK_STR(imp.error(), "Unknown module format");
    }
}

TEST(mod_sample_memory_full) {
    ModBuilder m;
    m.smp[0].data = std::vector<int8_t>(100, 1);
    m.smp[1].data = std::vector<int8_t>(100, 1);
    MemStorage fs;
    Target t;
    t.limit = 150;
    ModImport imp;
    CHECK(importMod(fs, t, imp, m.build()));
    CHECK_EQ(t.ins[0].lengthFrames, 100u);
    CHECK_EQ(t.ins[1].lengthFrames, 0u);
    CHECK_STR(imp.warning(), "Not enough sample memory");
}

// ===========================================================================
// IT import (spec 11 §1.3)

TEST(it_magic_and_old_format) {
    ItBuilder b;
    b.magic = "XXXX";
    MemStorage fs;
    Target t;
    ItImport imp;
    fs.files["Mod/a.it"] = b.build();
    CHECK(!imp.begin(fs, "Mod/a.it", t.scratch.data(), uint32_t(t.scratch.size())));
    CHECK_STR(imp.error(), "Not an IT file");
    ItBuilder c;
    c.instruments = true;
    c.cmwt = 0x100;
    c.ins.push_back({});
    fs.files["Mod/b.it"] = c.build();
    CHECK(!imp.begin(fs, "Mod/b.it", t.scratch.data(), uint32_t(t.scratch.size())));
}

TEST(it_sample_mode_8bit_reference_rate) {
    ItBuilder b;
    b.is = 3;
    b.it = 125;
    std::vector<int16_t> s = {0, 256, -256, 32512};
    ItBuilder::Smp a;
    a.name = "eight";
    a.flg = 1;
    a.len = 4;
    a.c5 = 44100;
    a.gvl = 32;
    a.vol = 64;
    a.data = bytes8(s);
    b.smp.push_back(a);
    ItBuilder::Smp h = a;
    h.name = "half";
    h.c5 = 22050;
    h.gvl = 64;
    h.vol = 32;
    b.smp.push_back(h);
    b.pats.push_back({});
    MemStorage fs;
    Target t;
    ItImport imp;
    CHECK(importIt(fs, t, imp, b.build()));
    CHECK(!imp.instrumentMode());
    CHECK_EQ(imp.instrumentCount(), 2);
    CHECK_STR(t.ins[0].name, "eight");
    CHECK_EQ(int(t.ins[0].volume), 25);   // GvL 32 of 64
    CHECK_EQ(int(t.ins[1].volume), 25);   // Vol 32 of 64
    // 8-bit reference is 44100 (stock 22050: one octave high).
    CHECK_EQ(int(t.ins[0].tune), 0);
    CHECK_EQ(int(t.ins[1].tune), -12);
    CHECK_EQ(int(t.ptr[0][1]), 256);
    CHECK_EQ(int(t.ptr[0][3]), 32512);
    // Project tempo = 6 * IT / IS (stock ignored IS).
    CHECK_NEAR(t.proj.tempo(), 250.0, 1e-3);
}

TEST(it_instrument_mode_and_transpose) {
    ItBuilder b;
    b.instruments = true;
    ItBuilder::Ins i0;
    i0.name = "piano";
    i0.gbv = 128;
    i0.keys = {{60, 2}};
    b.ins.push_back(i0);
    ItBuilder::Ins i1;   // no keyboard entry: falls back to sample i+1 (= 2)
    i1.name = "";
    i1.gbv = 64;
    b.ins.push_back(i1);
    ItBuilder::Smp s0;
    s0.len = 4;
    s0.flg = 1 | 2;
    s0.data = bytes16({1, 2, 3, 4});
    b.smp.push_back(s0);
    ItBuilder::Smp s1;
    s1.len = 2;
    s1.flg = 1 | 2;
    s1.vol = 32;
    s1.c5 = 8363;
    s1.data = bytes16({100, -100});
    b.smp.push_back(s1);
    ItBuilder::Pat p;
    p.cells.push_back({0, 0, 60, 1});
    b.pats.push_back(p);
    MemStorage fs;
    Target t;
    ItImport imp;
    CHECK(importIt(fs, t, imp, b.build()));
    CHECK(imp.instrumentMode());
    CHECK_STR(t.ins[0].name, "piano");
    CHECK_STR(t.ins[1].name, "-");
    CHECK_EQ(int(t.ins[0].volume), 25);   // GbV 128 -> 50, x Vol 32/64
    CHECK_EQ(int(t.ins[1].volume), 12);   // GbV 64 -> 25, x 32/64
    CHECK_EQ(int(t.ptr[0][0]), 100);       // instrument 1 uses sample 2
    CHECK_EQ(int(t.ptr[1][1]), -100);
    // C5Speed 8363: 12*log2(8363/44100) = -28.78 -> transpose -28, finetune -78.
    CHECK_EQ(int(imp.transpose(0)), -28);
    CHECK_EQ(int(t.ins[0].tune), 0);
    CHECK_EQ(int(t.ins[0].finetune), -78);
    CHECK_EQ(int(t.step(1, 0, 0).note), 32);
    CHECK_EQ(int(t.step(1, 0, 0).instrument), 0);
}

TEST(it_loops_and_sample_formats) {
    ItBuilder b;
    auto add = [&](uint8_t flg, uint8_t cvt, std::vector<uint8_t> data, uint32_t len) {
        ItBuilder::Smp s;
        s.flg = flg;
        s.cvt = cvt;
        s.len = len;
        s.lb = 1;
        s.le = 3;
        s.slb = 2;
        s.sle = 4;
        s.data = data;
        b.smp.push_back(s);
    };
    add(1 | 0x10, 1, bytes8({0, 0, 0, 0}), 4);          // forward loop
    add(1 | 0x10 | 0x40, 1, bytes8({0, 0, 0, 0}), 4);   // ping-pong loop
    add(1 | 0x40, 1, bytes8({0, 0, 0, 0}), 4);          // ping-pong bit without loop: one-shot
    add(1 | 0x20 | 0x80, 1, bytes8({0, 0, 0, 0}), 4);   // sustain ping-pong loop
    add(1, 0, {0x80, 0x81, 0x7F, 0x00}, 4);             // unsigned 8-bit
    add(1, 1 | 4, {10, 5, 0xFB, 0}, 4);                 // delta 8-bit: 10, 15, 10, 10
    add(1 | 2 | 4, 1, bytes16({1, 2, 3, -1, -2, -3}), 3);   // stereo 16-bit, planar
    add(1 | 2, 0, {0x00, 0x80, 0xFF, 0xFF}, 2);         // unsigned 16-bit
    b.pats.push_back({});
    MemStorage fs;
    Target t;
    ItImport imp;
    CHECK(importIt(fs, t, imp, b.build()));
    CHECK_EQ(int(t.ins[0].playMode), int(PlayMode::ForwardLoop));
    CHECK_EQ(int(t.ins[0].loopPoint1), 65535 / 4);
    CHECK_EQ(int(t.ins[0].loopPoint2), 3 * 65535 / 4);
    CHECK_EQ(int(t.ins[1].playMode), int(PlayMode::PingPongLoop));
    CHECK_EQ(int(t.ins[2].playMode), int(PlayMode::OneShot));
    CHECK_EQ(int(t.ins[3].playMode), int(PlayMode::PingPongLoop));
    CHECK_EQ(int(t.ins[3].loopPoint1), 2 * 65535 / 4);
    CHECK_EQ(int(t.ins[3].loopPoint2), 65535);
    CHECK_EQ(int(t.ptr[4][0]), 0);
    CHECK_EQ(int(t.ptr[4][1]), 256);
    CHECK_EQ(int(t.ptr[4][2]), -256);
    CHECK_EQ(int(t.ptr[5][1]), 15 * 256);
    CHECK_EQ(int(t.ptr[5][2]), 10 * 256);
    CHECK(t.stereo[6]);
    CHECK(t.ins[6].stereo);
    CHECK_EQ(t.ins[6].lengthFrames, 3u);
    CHECK_EQ(int(t.ptr[6][2]), 3);
    CHECK_EQ(int(t.ptr[6][3]), -1);
    CHECK_EQ(int(t.ptr[7][0]), 0);
    CHECK_EQ(int(t.ptr[7][1]), 32767);
}

TEST(it_compressed_samples) {
    std::vector<int16_t> s8, s16;
    for (int i = 0; i < 40000; ++i) {
        const int v = int(100 * std::sin(i * 0.05)) + ((i % 97) == 0 ? 120 : 0);
        s8.push_back(int16_t(int8_t(v > 127 ? 127 : v) * 256));
    }
    for (int i = 0; i < 20000; ++i) s16.push_back(int16_t(20000 * std::sin(i * 0.01) + ((i % 50) == 0 ? 9000 : i % 5)));
    for (int v215 = 0; v215 < 2; ++v215) {
        ItBuilder b;
        ItBuilder::Smp a;
        a.flg = 1 | 8;
        a.cvt = uint8_t(1 | (v215 ? 4 : 0));
        a.len = uint32_t(s8.size());
        a.data = itCompress(s8, false, v215);
        b.smp.push_back(a);
        ItBuilder::Smp c;
        c.flg = 1 | 2 | 8;
        c.cvt = uint8_t(1 | (v215 ? 4 : 0));
        c.len = uint32_t(s16.size());
        c.data = itCompress(s16, true, v215);
        b.smp.push_back(c);
        // Stereo compressed: left blocks, then right blocks.
        ItBuilder::Smp d;
        d.flg = 1 | 2 | 4 | 8;
        d.cvt = uint8_t(1 | (v215 ? 4 : 0));
        std::vector<int16_t> l(s16.begin(), s16.begin() + 5000), r(s16.begin() + 5000, s16.begin() + 10000);
        d.len = 5000;
        d.data = itCompress(l, true, v215);
        auto rd = itCompress(r, true, v215);
        d.data.insert(d.data.end(), rd.begin(), rd.end());
        b.smp.push_back(d);
        b.pats.push_back({});
        MemStorage fs;
        Target t;
        ItImport imp;
        CHECK(importIt(fs, t, imp, b.build()));
        CHECK_STR(imp.warning(), "");
        bool ok8 = true, ok16 = true, okSt = true;
        for (size_t i = 0; i < s8.size(); ++i) ok8 = ok8 && t.ptr[0][i] == s8[i];
        for (size_t i = 0; i < s16.size(); ++i) ok16 = ok16 && t.ptr[1][i] == s16[i];
        for (size_t i = 0; i < 5000; ++i) okSt = okSt && t.ptr[2][i] == l[i] && t.ptr[2][5000 + i] == r[i];
        CHECK(ok8);
        CHECK(ok16);
        CHECK(okSt);
    }
}

TEST(it_patterns_memory_notes_and_effects) {
    ItBuilder b;
    b.orders = {0, 0xFE, 1, 2, 0xFF, 0};
    for (int i = 0; i < 48; ++i) {
        ItBuilder::Smp s;
        s.len = 1024;
        s.data = std::vector<uint8_t>(1024, 0);
        b.smp.push_back(s);
    }
    ItBuilder::Pat p;
    using C = ItBuilder::Cell;
    p.cells.push_back(C{0, 0, 60, 1});
    p.cells.push_back(C{0, 1, 64, 2});
    // Row 1: channel 0 repeats its own last note (mask bit 4) - stock took
    // the value last read in any channel (64).
    C mem{1, 0};
    mem.mask = 16;
    p.cells.push_back(mem);
    p.cells.push_back(C{2, 1, 67});            // note without instrument: channel's instrument 2
    p.cells.push_back(C{3, 0, 255});           // note off
    p.cells.push_back(C{3, 1, 254});           // cut
    p.cells.push_back(C{3, 2, 200});           // fade
    p.cells.push_back(C{4, 0, 60, 48});        // instrument 48 is usable (stock dropped it)
    p.cells.push_back(C{4, 1, 60, -1, 64});    // volume 64 -> Volume 50
    p.cells.push_back(C{5, 1, -1, -1, 160});   // pan 32 -> Panning 50
    p.cells.push_back(C{6, 0, -1, -1, -1, 1, 3});      // A03 -> 250 BPM at tempo 125
    p.cells.push_back(C{7, 0, -1, -1, -1, 20, 150});   // T96 -> 300 BPM
    p.cells.push_back(C{8, 0, -1, -1, -1, 20, 0x10});  // tempo slide: dropped
    p.cells.push_back(C{9, 0, -1, -1, -1, 3, 0});      // C00 -> Break
    p.cells.push_back(C{10, 0, -1, -1, -1, 7, 16});    // G10 -> Glide 16
    p.cells.push_back(C{11, 0, -1, -1, -1, 7, 0});     // G00: previous value
    p.cells.push_back(C{12, 0, 60, 1, 32, 15, 2});     // O02 on 1024 frames -> 50 %, plus volume 32
    p.cells.push_back(C{13, 0, -1, -1, -1, 24, 255});  // X FF -> Panning 100
    p.cells.push_back(C{14, 12, 60, 1});               // channel 13: dropped
    p.cells.push_back(C{15, 0, 60, 49});               // instrument beyond the imported ones
    b.pats.push_back(p);
    ItBuilder::Pat big;
    big.rows = 200;
    big.cells.push_back(C{127, 3, 48, 3});
    big.cells.push_back(C{150, 3, 48, 3});
    b.pats.push_back(big);
    ItBuilder::Pat empty;
    empty.null = true;
    b.pats.push_back(empty);
    MemStorage fs;
    Target t;
    ItImport imp;
    CHECK(importIt(fs, t, imp, b.build()));
    // Song: "+++" skipped, ends at "---".
    CHECK_EQ(int(t.proj.playlist[0]), 1);
    CHECK_EQ(int(t.proj.playlist[1]), 2);
    CHECK_EQ(int(t.proj.playlist[2]), 3);
    CHECK_EQ(int(t.proj.playlist[3]), 0);
    CHECK_EQ(int(t.step(1, 0, 0).note), 60);
    CHECK_EQ(int(t.step(1, 1, 0).instrument), 1);
    CHECK_EQ(int(t.step(1, 0, 1).note), 60);
    CHECK_EQ(int(t.step(1, 0, 1).instrument), 0);
    CHECK_EQ(int(t.step(1, 1, 2).note), 67);
    CHECK_EQ(int(t.step(1, 1, 2).instrument), 1);
    CHECK_EQ(int(t.step(1, 0, 3).note), int(kNoteOff));
    CHECK_EQ(int(t.step(1, 1, 3).note), int(kNoteCut));
    CHECK_EQ(int(t.step(1, 2, 3).note), int(kNoteFade));
    CHECK_EQ(int(t.step(1, 0, 4).instrument), 47);
    CHECK_EQ(int(t.step(1, 1, 4).fx[1].type), int(FxId::Volume));
    CHECK_EQ(int(t.step(1, 1, 4).fx[1].value), 50);
    CHECK_EQ(int(t.step(1, 1, 5).fx[1].type), int(FxId::Panning));
    CHECK_EQ(int(t.step(1, 1, 5).fx[1].value), 50);
    CHECK_EQ(int(t.step(1, 0, 6).fx[0].type), int(FxId::Tempo));
    CHECK_EQ(int(t.step(1, 0, 6).fx[0].value), 125);
    CHECK_EQ(int(t.step(1, 0, 7).fx[0].value), 150);   // 6*150/3 = 300 BPM
    CHECK_EQ(int(t.step(1, 0, 8).fx[0].type), 0);
    CHECK_EQ(int(t.step(1, 0, 9).fx[0].type), int(FxId::BreakPattern));
    CHECK_EQ(int(t.step(1, 0, 10).fx[0].type), int(FxId::Glide));
    CHECK_EQ(int(t.step(1, 0, 10).fx[0].value), 16);
    CHECK_EQ(int(t.step(1, 0, 11).fx[0].value), 16);
    CHECK_EQ(int(t.step(1, 0, 12).fx[0].type), int(FxId::Position));
    CHECK_EQ(int(t.step(1, 0, 12).fx[0].value), 50);
    CHECK_EQ(int(t.step(1, 0, 12).fx[1].value), 25);
    CHECK_EQ(int(t.step(1, 0, 13).fx[0].type), int(FxId::Panning));
    CHECK_EQ(int(t.step(1, 0, 13).fx[0].value), 100);
    CHECK_EQ(int(t.step(1, 0, 15).note), int(kNoteEmpty));
    // Pattern 2: 200 rows -> 128 steps on every track.
    CHECK_EQ(int(t.pat(2).tracks[0].lastStep), 127);
    CHECK_EQ(int(t.pat(2).tracks[11].lastStep), 127);
    CHECK_EQ(int(t.step(2, 3, 127).note), 48);
    // Pattern 2 is entered at 300 BPM (end of pattern 1): restated on row 0.
    CHECK_EQ(int(t.step(2, 0, 0).fx[0].type), int(FxId::Tempo));
    CHECK_EQ(int(t.step(2, 0, 0).fx[0].value), 150);
    // Pattern 3: null pointer, in the song -> 64 empty rows.
    CHECK(t.patPresent[3]);
    CHECK_EQ(int(t.pat(3).tracks[0].lastStep), 63);
}

// ===========================================================================
// Export As .it (spec 11 §2)

namespace {
struct ExportSrc {
    Project proj;
    Instrument ins[kSampleInstrumentCount];
    bool present[kSampleInstrumentCount] = {};
    std::vector<int16_t> data[kSampleInstrumentCount];
    bool stereo[kSampleInstrumentCount] = {};
    std::vector<PatternStock> pats = std::vector<PatternStock>(kPatternCount);
    bool patPresent[kPatternCount + 1] = {};
    std::vector<uint8_t> scratch = std::vector<uint8_t>(0x10000);

    ItExportSource source() {
        ItExportSource s;
        s.project = &proj;
        s.instruments = ins;
        s.present = present;
        s.ctx = this;
        s.samples = [](void* c, int slot, uint32_t* fr, bool* st) -> const int16_t* {
            auto* e = static_cast<ExportSrc*>(c);
            *st = e->stereo[slot];
            *fr = uint32_t(e->data[slot].size() / (*st ? 2 : 1));
            return *fr ? e->data[slot].data() : nullptr;
        };
        s.pattern = [](void* c, int n) -> const PatternStock* {
            auto* e = static_cast<ExportSrc*>(c);
            return e->patPresent[n] ? &e->pats[n - 1] : nullptr;
        };
        return s;
    }
};

bool runExport(MemStorage& fs, ExportSrc& e, ItExport& ex) {
    if (!ex.begin(fs, "Mod/Song.it", e.source(), e.scratch.data(), uint32_t(e.scratch.size()))) return false;
    return runAll(ex) == ImportStep::Done;
}

uint32_t rd32(const std::vector<uint8_t>& v, size_t o) {
    return uint32_t(v[o]) | uint32_t(v[o + 1]) << 8 | uint32_t(v[o + 2]) << 16 | uint32_t(v[o + 3]) << 24;
}
uint16_t rd16(const std::vector<uint8_t>& v, size_t o) { return uint16_t(v[o] | v[o + 1] << 8); }

void buildProject(ExportSrc& e) {
    e.proj.setDefault();
    e.proj.setName("Song");
    e.proj.setTempo(120.0f);
    std::memset(e.proj.playlist, 0, sizeof(e.proj.playlist));
    e.proj.playlist[0] = 1;
    e.proj.playlist[1] = 2;
    e.proj.playlist[2] = 1;
    // Instrument 1: mono, forward loop; 2: stereo; 3: present without sample.
    e.present[0] = e.present[1] = e.present[2] = true;
    std::snprintf(e.ins[0].name, sizeof(e.ins[0].name), "mono");
    e.ins[0].volume = 50;
    e.ins[0].playMode = uint8_t(PlayMode::ForwardLoop);
    e.ins[0].loopPoint1 = 16384;
    e.ins[0].loopPoint2 = 49151;
    for (int i = 0; i < 1000; ++i) e.data[0].push_back(int16_t(i * 7));
    e.ins[0].lengthFrames = 1000;
    std::snprintf(e.ins[1].name, sizeof(e.ins[1].name), "stereo");
    e.ins[1].volume = 25;
    e.stereo[1] = true;
    for (int i = 0; i < 600; ++i) e.data[1].push_back(int16_t(i < 300 ? 1000 + i : -1000 - i));
    e.ins[1].lengthFrames = 300;
    e.ins[1].env[kAutoCutoff].enabled = 1;
    e.ins[1].env[kAutoCutoff].sustain = 1.0f;
    e.ins[2].env[kAutoCutoff].enabled = 1;
    e.ins[2].env[kAutoCutoff].loop = 1;   // filter LFO; envelope 0 is not in LFO mode
    e.patPresent[1] = e.patPresent[2] = true;
    PatternStock& p = e.pats[0];
    p.setDefault();
    for (auto& t : p.tracks) t.lastStep = 15;
    Step* s = p.tracks[0].steps;
    s[0].note = 60;
    s[0].instrument = 0;
    s[0].fx[1] = {uint8_t(FxId::Volume), 50};
    s[1].note = 62;
    s[1].instrument = 1;
    s[1].fx[1] = {uint8_t(FxId::Volume), 25};
    s[1].fx[0] = {uint8_t(FxId::Panning), 100};
    s[2].fx[1] = {uint8_t(FxId::Volume), 40};   // FX without a note
    s[3].note = kNoteOff;
    s[3].instrument = 0;
    s[4].note = 64;
    s[4].instrument = 50;   // MIDI instrument: skipped
    s[5].fx[0] = {uint8_t(FxId::Tempo), 70};
    s[6].fx[0] = {uint8_t(FxId::BreakPattern), 1};
    p.tracks[11].steps[7].note = 72;
    p.tracks[11].steps[7].instrument = 1;
    PatternStock& q = e.pats[1];
    q.setDefault();
    q.tracks[0].lastStep = 63;
    q.tracks[2].steps[10].note = 48;
    q.tracks[2].steps[10].instrument = 0;
}
}  // namespace

TEST(it_export_layout_and_fixes) {
    ExportSrc e;
    buildProject(e);
    MemStorage fs;
    fs.files["Mod/Song.it"] = std::vector<uint8_t>(10, 0xAA);   // replaced
    ItExport ex;
    CHECK(runExport(fs, e, ex));
    const std::vector<uint8_t>& v = fs.files["Mod/Song.it"];
    CHECK(v.size() > 0x624);
    CHECK(!std::memcmp(v.data(), "IMPM", 4));
    CHECK(!std::memcmp(v.data() + 4, "Song", 5));
    CHECK_EQ(int(rd16(v, 0x20)), 4);    // 3 orders + "---" (stock: 200)
    CHECK_EQ(int(rd16(v, 0x22)), 48);
    CHECK_EQ(int(rd16(v, 0x24)), 48);
    CHECK_EQ(int(rd16(v, 0x26)), 2);
    CHECK_EQ(int(rd16(v, 0x28)), 0x0217);
    CHECK_EQ(int(rd16(v, 0x2A)), 0x0200);
    CHECK_EQ(int(rd16(v, 0x2C)), 0x000D);   // stereo bit set (stock 0x000C)
    CHECK_EQ(int(v[0x32]), 6);
    CHECK_EQ(int(v[0x33]), 120);
    CHECK_EQ(int(v[0x34]), 128);            // separation
    CHECK_EQ(int(v[0x40]), 32);
    CHECK_EQ(int(v[0x80]), 64);
    CHECK_EQ(int(v[0xC0]), 0);
    CHECK_EQ(int(v[0xC1]), 1);
    CHECK_EQ(int(v[0xC2]), 0);
    CHECK_EQ(int(v[0xC3]), 0xFF);
    const size_t insTab = 0xC0 + 4, smpTab = insTab + 4 * 48, patTab = smpTab + 4 * 48;
    const uint32_t i0 = rd32(v, insTab);
    CHECK_EQ(i0, uint32_t(patTab + 8));
    CHECK(!std::memcmp(v.data() + i0, "IMPI", 4));
    CHECK(!std::memcmp(v.data() + i0 + 4, "Instr001.mti", 12));
    CHECK_EQ(int(v[i0 + 0x11]), 2);
    CHECK_EQ(int(rd16(v, i0 + 0x14)), 250);
    CHECK_EQ(int(v[i0 + 0x1A]), 26);
    CHECK_EQ(int(v[i0 + 0x41 + 2 * 60]), 1);
    // Sample headers.
    const uint32_t h0 = rd32(v, smpTab), h1 = rd32(v, smpTab + 4), h2 = rd32(v, smpTab + 8);
    CHECK(!std::memcmp(v.data() + h0, "IMPS", 4));
    CHECK(!std::memcmp(v.data() + h0 + 4, "Instr001.mts", 12));
    CHECK_EQ(int(v[h0 + 0x11]), 64);    // volume 50 = 0 dB -> 64 (stock 32)
    CHECK_EQ(int(v[h1 + 0x13]), 32);
    CHECK_EQ(int(v[h0 + 0x12]), 0x33);  // present, 16-bit, loop + sustain loop
    CHECK_EQ(int(v[h1 + 0x12]), 0x07);  // stereo
    CHECK_EQ(int(v[h2 + 0x12]), 0x00);  // no sample: not "present" (stock 0x03)
    CHECK_EQ(rd32(v, h0 + 0x30), 1000u);
    CHECK_EQ(rd32(v, h0 + 0x34), uint32_t(16384ull * 1000 / 65535));
    CHECK_EQ(rd32(v, h0 + 0x38), uint32_t(49151ull * 1000 / 65535));
    CHECK_EQ(rd32(v, h0 + 0x3C), 44100u);
    // Per-instrument data pointers: the stereo sample is complete and the
    // next one is not shifted (stock running-cursor defect).
    const uint32_t d0 = rd32(v, h0 + 0x48), d1 = rd32(v, h1 + 0x48);
    CHECK_EQ(d1, d0 + 2000);
    CHECK_EQ(int(int16_t(rd16(v, d0 + 2 * 999))), 999 * 7);
    CHECK_EQ(int(int16_t(rd16(v, d1))), 1000);
    CHECK_EQ(int(int16_t(rd16(v, d1 + 2 * 300))), -1300);
    CHECK_EQ(rd32(v, h2 + 0x48), 0u);
    // Filter envelope: written with the filter flag, its own LFO flag and
    // the +-32 range (stock: sustain x64, envelope 0's LFO flag).
    const uint32_t i1 = rd32(v, insTab + 4), i2 = rd32(v, insTab + 8);
    CHECK_EQ(int(v[i1 + 0x1D4]), 0x85);
    CHECK_EQ(int(v[i1 + 0x1D4 + 1]), 5);
    CHECK_EQ(int(int8_t(v[i1 + 0x1D4 + 6 + 3 * 2])), 32);   // sustain node, <= 32
    CHECK_EQ(int(v[i2 + 0x1D4]), 0x85);
    CHECK_EQ(int(v[i2 + 0x1D4 + 1]), 3);   // triangle LFO shape (3 nodes)
    CHECK_EQ(int(v[i0 + 0x130]), 0x05);    // volume envelope (enabled by default)
    // Patterns.
    const uint32_t p0 = rd32(v, patTab), p1 = rd32(v, patTab + 4);
    CHECK_EQ(int(rd16(v, p0 + 2)), 16);
    CHECK_EQ(int(rd16(v, p1 + 2)), 64);
    CHECK_EQ(p1, p0 + 8 + rd16(v, p0));
}

TEST(it_export_reimport_round_trip) {
    ExportSrc e;
    buildProject(e);
    MemStorage fs;
    ItExport ex;
    CHECK(runExport(fs, e, ex));
    Target t;
    ItImport imp;
    CHECK(imp.begin(fs, "Mod/Song.it", t.scratch.data(), uint32_t(t.scratch.size())));
    imp.apply(t.target());
    CHECK(runAll(imp) == ImportStep::Done);
    CHECK_NEAR(t.proj.tempo(), 120.0, 1e-3);
    CHECK_EQ(int(t.proj.playlist[0]), 1);
    CHECK_EQ(int(t.proj.playlist[1]), 2);
    CHECK_EQ(int(t.proj.playlist[2]), 1);
    CHECK_EQ(int(t.proj.playlist[3]), 0);
    CHECK_STR(t.ins[0].name, "mono");
    CHECK_EQ(int(t.ins[0].volume), 50);
    CHECK_EQ(int(t.ins[1].volume), 25);
    CHECK_EQ(int(t.ins[0].tune), 0);
    CHECK_EQ(int(t.ins[0].playMode), int(PlayMode::ForwardLoop));
    CHECK_EQ(t.ins[0].lengthFrames, 1000u);
    CHECK(t.ins[1].stereo);
    bool same = true;
    for (int i = 0; i < 1000; ++i) same = same && t.ptr[0][i] == e.data[0][i];
    for (int i = 0; i < 600; ++i) same = same && t.ptr[1][i] == e.data[1][i];
    CHECK(same);
    CHECK_EQ(int(t.step(1, 0, 0).note), 60);
    CHECK_EQ(int(t.step(1, 0, 0).fx[1].type), int(FxId::Volume));
    CHECK_EQ(int(t.step(1, 0, 0).fx[1].value), 50);
    CHECK_EQ(int(t.step(1, 0, 1).instrument), 1);
    CHECK_EQ(int(t.step(1, 0, 1).fx[1].value), 25);
    CHECK_EQ(int(t.step(1, 0, 1).fx[0].type), int(FxId::Panning));   // volume and panning both kept
    CHECK_EQ(int(t.step(1, 0, 1).fx[0].value), 100);
    CHECK_EQ(int(t.step(1, 0, 2).note), int(kNoteEmpty));
    CHECK_EQ(int(t.step(1, 0, 2).fx[1].value), 40);                  // FX without a note
    CHECK_EQ(int(t.step(1, 0, 3).note), int(kNoteOff));
    CHECK_EQ(int(t.step(1, 0, 4).note), int(kNoteEmpty));            // MIDI step skipped
    CHECK_EQ(int(t.step(1, 0, 5).fx[0].type), int(FxId::Tempo));
    CHECK_EQ(int(t.step(1, 0, 5).fx[0].value), 70);
    CHECK_EQ(int(t.step(1, 0, 6).fx[0].type), int(FxId::BreakPattern));
    CHECK_EQ(int(t.step(1, 11, 7).note), 72);
    CHECK_EQ(int(t.pat(1).tracks[0].lastStep), 15);
    CHECK_EQ(int(t.step(2, 2, 10).note), 48);
}

TEST(it_export_high_tempo_uses_speed_3) {
    uint8_t s = 0, t = 0;
    itfmt::exportSpeedTempo(300.0f, s, t);
    CHECK_EQ(int(s), 3);
    CHECK_EQ(int(t), 150);
    itfmt::exportSpeedTempo(130.0f, s, t);
    CHECK_EQ(int(s), 6);
    CHECK_EQ(int(t), 130);
    CHECK_EQ(int(itfmt::itVolume(50)), 64);
    CHECK_EQ(int(itfmt::itVolume(100)), 64);
    CHECK_EQ(int(itfmt::itVolume(25)), 32);
    ExportSrc e;
    buildProject(e);
    e.proj.setTempo(300.0f);
    MemStorage fs;
    ItExport ex;
    CHECK(runExport(fs, e, ex));
    Target tg;
    ItImport imp;
    CHECK(imp.begin(fs, "Mod/Song.it", tg.scratch.data(), uint32_t(tg.scratch.size())));
    imp.apply(tg.target());
    CHECK(runAll(imp) == ImportStep::Done);
    CHECK_NEAR(tg.proj.tempo(), 300.0, 1e-3);
    CHECK_EQ(int(tg.step(1, 0, 5).fx[0].value), 70);   // Tempo FX survives at speed 3
}

// ===========================================================================
// File screen wiring (spec 05 appendix E1.2 / E1.4, spec 11 §1.1, §2.1)

namespace {
void runJob(Harness& h) {
    for (int i = 0; i < 2000 && h.app->jobBusy(); ++i) h.run(10);
}
}  // namespace

TEST(file_screen_import_mod_and_export_it) {
    Harness h;
    App& a = *h.app;
    ModBuilder m;
    m.smp[0].name = "square";
    for (int i = 0; i < 4000; ++i) m.smp[0].data.push_back(int8_t((i / 20) % 2 ? 90 : -90));
    m.smp[0].rs = 0;
    m.smp[0].rl = 2000;
    m.cell(0, 0, 0, 428, 1, 0xF, 0x7D);
    h.fs.dirs.insert("Mod");
    h.fs.files["Mod/My Tune.mod"] = m.build();
    h.fs.files["Mod/readme.txt"] = {1, 2, 3};
    h.tap(btn::File);
    h.tap(btn::F3);
    auto& fsScr = *static_cast<FileScreen*>(a.ui.screen(kModFile));
    CHECK_EQ(fsScr.state, 2);
    CHECK_EQ(fsScr.count, 1);   // only the .mod passes the filter
    h.tap(btn::F8);
    runJob(h);
    CHECK(!a.jobBusy());
    CHECK_STR(a.project.name, "My Tune");
    CHECK(a.unsavedChanges());
    CHECK(a.instrPresent[0]);
    CHECK_STR(a.instruments[0].name, "square");
    CHECK_EQ(a.bank.frames(0), 4000u);
    CHECK_EQ(int(a.stored(1).tracks[0].steps[0].note), 31);
    CHECK_EQ(int(a.pattern().tracks[0].steps[0].note), 31);
    CHECK_NEAR(a.project.tempo(), 125.0, 1e-3);
    CHECK_EQ(a.ui.active(), int(kModPattern));
    // Autosave writes the imported project into the Workspace.
    a.autosave();
    CHECK(h.fs.files.count("Workspace/patterns/pattern_01.mtp"));
    CHECK(h.fs.files.count("Workspace/instruments/instrument_01.pti"));
    // Export As .it: save prompt first (unsaved changes) -> Don't Save.
    h.fs.dirs.erase("Mod");
    for (auto it = h.fs.files.begin(); it != h.fs.files.end();)
        it = it->first.compare(0, 4, "Mod/") == 0 ? h.fs.files.erase(it) : std::next(it);
    h.tap(btn::File);
    h.tap(btn::F6);
    CHECK_EQ(fsScr.state, 4);
    h.tap(btn::F5);
    CHECK(a.ui.askOpen());
    h.tap(btn::F7);   // Don't Save
    runJob(h);
    CHECK(h.fs.dirs.count("Mod"));   // created when missing
    CHECK(h.fs.files.count("Mod/My Tune.it"));
    const auto& it = h.fs.files["Mod/My Tune.it"];
    CHECK(it.size() > 8000);
    CHECK(!std::memcmp(it.data(), "IMPM", 4));
}

TEST(file_screen_import_refused_keeps_project) {
    Harness h;
    App& a = *h.app;
    ModBuilder m;
    m.sig = "12CH";
    m.ch = 12;
    h.fs.dirs.insert("Mod");
    h.fs.files["Mod/wide.mod"] = m.build();
    a.project.setName("Keep");
    h.tap(btn::File);
    h.tap(btn::F3);
    h.tap(btn::F8);
    runJob(h);
    CHECK_STR(a.project.name, "Keep");
}

// File > Import Mod on a .it (here one written by Export As .it): the
// project gets the module's samples and notes, like a .mod import.
TEST(file_screen_import_it) {
    Harness h;
    App& a = *h.app;
    ModBuilder m;
    m.smp[0].name = "square";
    for (int i = 0; i < 4000; ++i) m.smp[0].data.push_back(int8_t((i / 20) % 2 ? 90 : -90));
    m.cell(0, 0, 0, 428, 1, 0, 0);
    m.cell(0, 4, 1, 214, 1, 0, 0);
    h.fs.dirs.insert("Mod");
    h.fs.files["Mod/Tune.mod"] = m.build();
    h.tap(btn::File);
    h.tap(btn::F3);
    h.tap(btn::F8);
    runJob(h);
    CHECK_EQ(a.bank.frames(0), 4000u);
    // Export As .it (Don't Save), then only the .it is left to import.
    h.tap(btn::File);
    h.tap(btn::F6);
    h.tap(btn::F5);
    h.tap(btn::F7);
    runJob(h);
    CHECK(h.fs.files.count("Mod/Tune.it"));
    h.fs.files.erase("Mod/Tune.mod");
    a.newProject();
    CHECK_EQ(a.bank.frames(0), 0u);
    h.tap(btn::F8);   // Back from the Export page
    h.tap(btn::F3);
    h.tap(btn::F8);
    runJob(h);
    CHECK_STR(a.ui.notice(), "Module imported");
    CHECK_STR(a.project.name, "Tune");
    CHECK(a.instrPresent[0]);
    CHECK_EQ(a.bank.frames(0), 4000u);
    CHECK_EQ(int(a.stored(1).tracks[0].steps[0].note), 31);
    CHECK_EQ(int(a.stored(1).tracks[1].steps[4].note), 43);
}
