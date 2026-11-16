// Export "As .it" (spec 11 §2).
//
// Deviations from stock 1.9.2 (spec 11 defects; reimpl/README.md lists them):
// - sample data is taken per instrument (stock: one running cursor from the
//   pool start, so a stereo instrument shifted every later sample); stereo
//   instruments are written as stereo IT samples; empty slots have no
//   "sample present" flag;
// - the filter envelope checks envelope 2's own LFO flag (stock reads
//   envelope 0's), its sustain level is scaled to the +-32 IT range (stock
//   x64), and an LFO-converted filter envelope keeps the filter flag;
//   pan / filter LFO periods use their own rate table (stock reads the
//   24-entry volume table, past its end for speeds 24..28);
// - the song is exported up to its end (stock: 200 slots, 0 -> "---"), and
//   patterns 1..200 (stock 1..199); the stereo flag is set with full
//   separation (stock: mono module);
// - volume maps 50 (0 dB) -> 64 (stock: 100 -> 64, i.e. half volume);
//   volume and panning FX both survive (panning as Xxx when the volume
//   column is taken; stock: panning replaced volume); FX on steps without
//   a note are written; Tempo / Break / Glide / Position FX become
//   Txx / C00 / Gxx / Oxx; MIDI-instrument steps are skipped (stock wrote
//   instrument numbers 49..64); fade is IT note fade (stock: cut);
// - a project tempo above 255 BPM uses speed 3 (stock wraps the byte).
#include "formats/it_export.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio/audio_tables.h"
#include "formats/bytes.h"
#include "model/fx.h"

namespace ot {

namespace itfmt {

void exportSpeedTempo(float bpm, uint8_t& speed, uint8_t& tempo) {
    speed = bpm > 255.0f ? 3 : 6;
    long t = std::lround(bpm * float(speed) / 6.0f);
    tempo = uint8_t(t < 32 ? 32 : (t > 255 ? 255 : t));
}

// Rounded up, so the import's v * 50 / 64 (truncating) gives v back.
uint8_t itVolume(int v) {
    if (v < 0) v = 0;
    const int r = (v * 64 + 49) / 50;
    return uint8_t(r > 64 ? 64 : r);
}

namespace {
const StepFx* findFx(const Step& s, FxId id) {
    if (s.fx[1].type == uint8_t(id)) return &s.fx[1];
    if (s.fx[0].type == uint8_t(id)) return &s.fx[0];
    return nullptr;
}
}  // namespace

uint32_t encodePattern(const PatternStock& p, const Instrument* ins, uint8_t speed, uint8_t restoreTempo, uint8_t* out) {
    uint8_t* o = out;
    bool row0Tempo = false;
    const int rows = p.tracks[0].length();   // spec 11 §2.6: track 0's length
    for (int r = 0; r < rows; ++r) {
        for (int t = 0; t < kStockTrackCount; ++t) {
            const Step& s = p.tracks[t].steps[r];
            if (s.note != kNoteEmpty && !isSampleInstrument(s.instrument)) continue;   // MIDI steps
            uint8_t mask = 0, note = 0, vol = 0, cmd = 0, par = 0;
            if (s.note != kNoteEmpty) {
                mask |= 3;
                if (s.note >= 0) note = uint8_t(s.note > 119 ? 119 : s.note);
                else if (s.note == kNoteOff) note = 255;
                else if (s.note == kNoteCut) note = 254;
                else note = 253;   // fade
            }
            const StepFx* v = findFx(s, FxId::Volume);
            const StepFx* pan = findFx(s, FxId::Panning);
            if (v) {
                mask |= 4;
                vol = itVolume(v->value);
            } else if (pan) {
                mask |= 4;
                const int pv = (pan->value * 64 + 99) / 100;
                vol = uint8_t(128 + (pv > 64 ? 64 : pv));
                pan = nullptr;
            }
            const StepFx* fx;
            if ((fx = findFx(s, FxId::Tempo)) && fx->value > 4) {
                cmd = 20;   // Txx: BPM = 2v at the exported speed
                long t2 = std::lround(2.0f * fx->value * float(speed) / 6.0f);
                par = uint8_t(t2 < 32 ? 32 : (t2 > 255 ? 255 : t2));
            } else if (findFx(s, FxId::BreakPattern)) {
                cmd = 3;
                par = 0;
            } else if ((fx = findFx(s, FxId::Glide))) {
                cmd = 7;
                par = fx->value;
            } else if ((fx = findFx(s, FxId::Position)) && isSampleInstrument(s.instrument)) {
                cmd = 15;
                const uint64_t off = uint64_t(fx->value) * ins[s.instrument].lengthFrames / 100 / 256;
                par = uint8_t(off > 255 ? 255 : off);
            } else if (pan) {
                cmd = 24;
                par = uint8_t(pan->value >= 100 ? 255 : (pan->value * 255 + 99) / 100);
            }
            if (cmd) mask |= 8;
            if (cmd == 20 && r == 0) row0Tempo = true;
            if (!mask) continue;
            *o++ = uint8_t((t + 1) | 0x80);
            *o++ = mask;
            if (mask & 1) {
                *o++ = note;
                *o++ = uint8_t(s.instrument + 1);
            }
            if (mask & 4) *o++ = vol;
            if (mask & 8) {
                *o++ = cmd;
                *o++ = par;
            }
        }
        if (r == 0 && restoreTempo && !row0Tempo) {
            // A Tempo FX ends at the next song pattern (spec 03 §3.6) but an IT
            // tempo persists: restate the project tempo on channel 13.
            *o++ = uint8_t(13 | 0x80);
            *o++ = 8;
            *o++ = 20;
            *o++ = restoreTempo;
        }
        *o++ = 0;
    }
    return uint32_t(o - out);
}

namespace {
// IT ticks for a time in ms: one tick = 2500 / tempo ms (spec 11 §2.3).
uint16_t ticks(uint16_t ms, uint8_t itTempo) {
    const uint32_t t = uint32_t(ms) * itTempo / 2500u;
    return uint16_t(t == 0 ? 1 : (t > 9999 ? 9999 : t));
}

struct EnvOut {
    uint8_t* e;
    void node(int n, int y, uint32_t tick) {
        uint8_t* p = e + 6 + 3 * n;
        p[0] = uint8_t(int8_t(y));
        wrU16(p + 1, uint16_t(tick > 9999 ? 9999 : tick));
    }
};

// ADSR shape (spec 11 §2.3): (0,0) (peak,A) (S,A+D) (S,A+D+10) (0,A+D+10+R).
void adsr(uint8_t* e, uint8_t flg, const Envelope& env, int peak, int sus, uint8_t itTempo) {
    e[0] = flg;
    e[1] = 5;
    e[4] = 2;
    e[5] = 3;
    const uint32_t a = ticks(env.attackMs, itTempo), d = ticks(env.decayMs, itTempo),
                   r = ticks(env.releaseMs, itTempo);
    EnvOut o{e};
    o.node(0, 0, 0);
    o.node(1, peak, a);
    o.node(2, sus, a + d);
    o.node(3, sus, a + d + 10);
    o.node(4, 0, a + d + 10 + r);
}

// LFO -> envelope (spec 11 §2.3 table). `rate` = steps per LFO cycle.
void lfoEnv(uint8_t* e, uint8_t flg, const Lfo& lfo, float rate, int am, uint8_t speed, uint32_t& seed) {
    float pf = rate * float(speed);
    int32_t P = int32_t(pf);
    if (P < 2) P = 2;
    if (P > 9999) P = 9999;
    const uint32_t p = uint32_t(P);
    e[0] = flg;
    EnvOut o{e};
    o.node(0, 0, 0);
    switch (lfo.shape) {
        case 0:   // reverse saw
            e[1] = 3, e[5] = 2;
            o.node(1, am, 1);
            o.node(2, 0, p);
            break;
        case 1:   // saw
            e[1] = 3, e[5] = 2;
            o.node(1, am, p);
            o.node(2, 0, p + 1);
            break;
        case 2:   // triangle
            e[1] = 3, e[5] = 2;
            o.node(1, am, p / 2);
            o.node(2, 0, p > p / 2 + 1 ? p : p / 2 + 1);
            break;
        case 3:   // square
            e[1] = 4, e[5] = 3;
            o.node(1, 0, p / 2);
            o.node(2, am, p / 2 + 1);
            o.node(3, am, p > p / 2 + 2 ? p : p / 2 + 2);
            break;
        default: {   // random: five nodes at ~P/3, P/2, 4P/6, 5P/6, P
            e[1] = 6, e[5] = 5;
            const uint32_t at[5] = {p / 3, p / 2, 4 * p / 6, 5 * p / 6, p};
            uint32_t prev = 0;
            for (int k = 0; k < 5; ++k) {
                seed = seed * 1103515245u + 12345u;
                const int span = am < 0 ? -am : am;
                int y = span ? int((seed >> 16) % uint32_t(span)) : 0;
                if (am < 0) y = -y;
                uint32_t t = at[k] > prev ? at[k] : prev + 1;
                o.node(k + 1, y, t);
                prev = t;
            }
            break;
        }
    }
}

// "Instr<NNN>.<ext>" (12 bytes, spec 11 §2.3 / §2.4).
void dosName(uint8_t* dst, int index, const char* ext) {
    const int n = (index + 1) % 1000;
    char fn[12] = {'I', 'n', 's', 't', 'r', char('0' + n / 100), char('0' + n / 10 % 10), char('0' + n % 10), '.',
                   ext[0], ext[1], ext[2]};
    std::memcpy(dst, fn, 12);
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

void buildInstrument(uint8_t* b, int index, const Instrument& in, uint8_t speed, uint8_t itTempo) {
    std::memset(b, 0, kImpiSize);
    std::memcpy(b, "IMPI", 4);
    dosName(b + 4, index, "mti");
    b[0x11] = 2;                 // NNA
    wrU16(b + 0x14, 250);        // FadeOut
    b[0x17] = 60;                // PPC
    b[0x18] = 128;               // GbV
    b[0x19] = 32;                // DfP
    b[0x1A] = 26;                // RV (stock constant)
    b[0x1B] = 21;                // RP (stock constant)
    wrU16(b + 0x1C, 0x0211);     // TrkVers
    b[0x1E] = 1;                 // NoS
    wrStr(b + 0x20, 26, in.name);
    for (int n = 0; n < 120; ++n) {
        b[0x40 + 2 * n] = uint8_t(n);
        b[0x41 + 2 * n] = uint8_t(index + 1);
    }
    uint32_t seed = 0x1234567u + uint32_t(index);
    // Volume envelope (Tracker envelope 0).
    const Envelope& ev = in.env[kAutoVolume];
    if (ev.loop)
        lfoEnv(b + 0x130, ev.enabled ? 5 : 4, in.lfo[kAutoVolume], audio::volLfoRateSteps(in.lfo[kAutoVolume].speed),
               clampi(int(in.lfo[kAutoVolume].amount * 64.0f), 0, 64), speed, seed);
    else if (ev.enabled)
        adsr(b + 0x130, 0x05, ev, 64, clampi(int(ev.sustain * 64.0f), 0, 64), itTempo);
    // Panning envelope (envelope 1).
    const Envelope& ep = in.env[kAutoPanning];
    if (ep.loop)
        lfoEnv(b + 0x182, ep.enabled ? 5 : 4, in.lfo[kAutoPanning], audio::lfoRateSteps(in.lfo[kAutoPanning].speed),
               -clampi(int(in.lfo[kAutoPanning].amount * 32.0f), 0, 32), speed, seed);
    else if (ep.enabled)
        adsr(b + 0x182, 0x05, ep, 32, clampi(int(ep.sustain * 32.0f), 0, 32), itTempo);
    // Filter envelope (envelope 2, cutoff): only when enabled; its own LFO flag.
    const Envelope& ef = in.env[kAutoCutoff];
    if (ef.enabled) {
        if (ef.loop)
            lfoEnv(b + 0x1D4, 0x85, in.lfo[kAutoCutoff], audio::lfoRateSteps(in.lfo[kAutoCutoff].speed),
                   clampi(int(in.lfo[kAutoCutoff].amount * 32.0f), 0, 32), speed, seed);
        else
            adsr(b + 0x1D4, 0x85, ef, 32, clampi(int(ef.sustain * 32.0f), 0, 32), itTempo);
    }
}

void buildSampleHeader(uint8_t* b, int index, const Instrument& in, uint32_t frames, bool stereo,
                       uint32_t dataOffset) {
    std::memset(b, 0, kImpsSize);
    std::memcpy(b, "IMPS", 4);
    dosName(b + 4, index, "mts");
    const uint8_t v = itVolume(in.volume);
    b[0x11] = v;   // GvL
    b[0x13] = v;   // Vol
    uint8_t flg = 0;
    uint32_t lb = 0, le = 0;
    if (frames) {
        flg = 0x03 | (stereo ? 0x04 : 0);
        const uint8_t pm = in.playMode;
        lb = uint32_t(uint64_t(in.loopPoint1) * frames / 65535);
        le = uint32_t(uint64_t(in.loopPoint2) * frames / 65535);
        if (pm >= 1 && pm <= 3 && le > lb) {
            flg |= 0x30;
            if (pm == 3) flg |= 0xC0;
        }
    }
    b[0x12] = flg;
    wrStr(b + 0x14, 26, in.name);
    b[0x2E] = 1;   // Cvt: signed
    wrU32(b + 0x30, frames);
    wrU32(b + 0x34, lb);
    wrU32(b + 0x38, le);
    wrU32(b + 0x3C, 44100);   // C5Speed (spec 11 §2.4)
    wrU32(b + 0x40, lb);
    wrU32(b + 0x44, le);
    wrU32(b + 0x48, frames ? dataOffset : 0);
}

}  // namespace itfmt

// ---------------------------------------------------------------------------

namespace {
// Built on first use (an export job, main loop); SDRAM on the device (2.0.1:
// SRAM for the USB Storage ring).
const Instrument& emptyInstrument() { return defaultInstrument(); }
}  // namespace

bool ItExport::fail(const char* msg) {
    std::snprintf(err_, sizeof(err_), "%s", msg);
    abort();
    stage_ = kIdle;
    return false;
}

bool ItExport::put(const void* p, uint32_t n) {
    return n == 0 || fs_->write(h_, p, n) == int32_t(n);
}

bool ItExport::begin(IStorage& fs, const char* path, const ItExportSource& src, uint8_t* scratch,
                     uint32_t scratchBytes) {
    *this = ItExport{};
    fs_ = &fs;
    src_ = src;
    if (!scratch || scratchBytes < kScratchBytes) return fail("No memory for the export");
    buf_ = scratch;
    const Project& pr = *src_.project;
    itfmt::exportSpeedTempo(pr.tempo(), speed_, tempo_);
    // Patterns 1..200 that exist (IT patterns 0..199).
    patNum_ = 0;
    for (int k = 1; k <= itfmt::kExportMaxPatterns; ++k)
        if (src_.pattern(src_.ctx, k)) patNum_ = uint16_t(k);
    // Song: slots up to the first empty one, then "---".
    ordNum_ = 0;
    for (int i = 0; i < kSongSlots && pr.playlist[i]; ++i)
        if (pr.playlist[i] <= patNum_) ++ordNum_;
    ++ordNum_;
    hdrSize_ = 0xC0u + ordNum_ + 4u * (itfmt::kExportInstruments * 2u + patNum_);
    uint32_t off = hdrSize_ + itfmt::kImpiSize * itfmt::kExportInstruments + itfmt::kImpsSize * itfmt::kExportInstruments;
    total_ = 2 + 2 * itfmt::kExportInstruments + patNum_;
    for (int j = 0; j < itfmt::kExportInstruments; ++j) {
        uint32_t frames = 0;
        bool st = false;
        const int16_t* d = src_.present[j] ? src_.samples(src_.ctx, j, &frames, &st) : nullptr;
        const uint32_t bytes = d ? frames * 2u * (st ? 2u : 1u) : 0;
        dataOff_[j] = off;
        off += bytes;
        total_ += (bytes + 0x3FFF) / 0x4000;
    }
    // Any Tempo FX: every pattern restates the project tempo on row 0.
    restoreT_ = 0;
    for (int k = 1; k <= patNum_ && !restoreT_; ++k) {
        const PatternStock* p = src_.pattern(src_.ctx, k);
        for (int t = 0; p && t < kStockTrackCount && !restoreT_; ++t)
            for (int r = 0; r < p->tracks[t].length() && !restoreT_; ++r)
                for (const StepFx& f : p->tracks[t].steps[r].fx)
                    if (f.type == uint8_t(FxId::Tempo) && f.value > 4) restoreT_ = tempo_;
    }
    for (int k = 1; k <= patNum_; ++k) {
        const PatternStock* p = src_.pattern(src_.ctx, k);
        patSize_[k - 1] = p ? uint16_t(itfmt::encodePattern(*p, src_.instruments, speed_, restoreT_, buf_)) : 0;
    }
    if (fs.exists(path)) fs.remove(path);   // spec 11 §2.1: an existing file is deleted first
    h_ = fs.open(path, IStorage::kWrite);
    if (h_ < 0) return fail("Cannot create file");
    stage_ = kHeader;
    return true;
}

ImportStep ItExport::step(uint32_t& cost) {
    const Project& pr = *src_.project;
    switch (stage_) {
        case kHeader: {
            uint8_t* b = buf_;
            std::memset(b, 0, hdrSize_);
            std::memcpy(b, "IMPM", 4);
            wrStr(b + 4, 26, pr.name);
            wrU16(b + 0x20, ordNum_);
            wrU16(b + 0x22, itfmt::kExportInstruments);
            wrU16(b + 0x24, itfmt::kExportInstruments);
            wrU16(b + 0x26, patNum_);
            wrU16(b + 0x28, 0x0217);
            wrU16(b + 0x2A, 0x0200);
            wrU16(b + 0x2C, 0x000D);   // stereo, instruments, linear slides
            b[0x30] = 128;             // GV
            b[0x31] = 128;             // MV
            b[0x32] = speed_;
            b[0x33] = tempo_;
            b[0x34] = 128;             // Sep: full separation
            std::memset(b + 0x40, 32, 64);
            std::memset(b + 0x80, 64, 64);
            int o = 0;
            for (int i = 0; i < kSongSlots && pr.playlist[i]; ++i)
                if (pr.playlist[i] <= patNum_) b[0xC0 + o++] = uint8_t(pr.playlist[i] - 1);
            b[0xC0 + o] = 0xFF;
            uint8_t* t = b + 0xC0 + ordNum_;
            for (int i = 0; i < itfmt::kExportInstruments; ++i)
                wrU32(t + 4 * i, hdrSize_ + itfmt::kImpiSize * uint32_t(i));
            t += 4 * itfmt::kExportInstruments;
            const uint32_t smp0 = hdrSize_ + itfmt::kImpiSize * itfmt::kExportInstruments;
            for (int j = 0; j < itfmt::kExportInstruments; ++j) wrU32(t + 4 * j, smp0 + itfmt::kImpsSize * uint32_t(j));
            t += 4 * itfmt::kExportInstruments;
            uint32_t po = dataOff_[itfmt::kExportInstruments - 1];
            {
                uint32_t frames = 0;
                bool st = false;
                const int last = itfmt::kExportInstruments - 1;
                const int16_t* d = src_.present[last] ? src_.samples(src_.ctx, last, &frames, &st) : nullptr;
                if (d) po += frames * 2u * (st ? 2u : 1u);
            }
            for (int k = 0; k < patNum_; ++k) {
                wrU32(t + 4 * k, patSize_[k] ? po : 0);
                if (patSize_[k]) po += 8u + patSize_[k];
            }
            if (!put(b, hdrSize_)) {
                fail("Cannot write file");
                return ImportStep::Failed;
            }
            cost += hdrSize_;
            ++units_;
            stage_ = kInstr;
            idx_ = 0;
            return ImportStep::More;
        }
        case kInstr: {
            const Instrument& in = src_.present[idx_] ? src_.instruments[idx_] : emptyInstrument();
            itfmt::buildInstrument(buf_, idx_, in, speed_, tempo_);
            if (!put(buf_, itfmt::kImpiSize)) {
                fail("Cannot write file");
                return ImportStep::Failed;
            }
            cost += itfmt::kImpiSize;
            ++units_;
            if (++idx_ >= itfmt::kExportInstruments) {
                stage_ = kSmpHdr;
                idx_ = 0;
            }
            return ImportStep::More;
        }
        case kSmpHdr: {
            uint32_t frames = 0;
            bool st = false;
            const bool has = src_.present[idx_] && src_.samples(src_.ctx, idx_, &frames, &st);
            const Instrument& in = src_.present[idx_] ? src_.instruments[idx_] : emptyInstrument();
            itfmt::buildSampleHeader(buf_, idx_, in, has ? frames : 0, has && st, dataOff_[idx_]);
            if (!put(buf_, itfmt::kImpsSize)) {
                fail("Cannot write file");
                return ImportStep::Failed;
            }
            cost += itfmt::kImpsSize;
            ++units_;
            if (++idx_ >= itfmt::kExportInstruments) {
                stage_ = kData;
                idx_ = 0;
                pos_ = 0;
            }
            return ImportStep::More;
        }
        case kData: {
            if (idx_ >= itfmt::kExportInstruments) {
                stage_ = kPatterns;
                idx_ = 0;
                return ImportStep::More;
            }
            uint32_t frames = 0;
            bool st = false;
            const int16_t* d = src_.present[idx_] ? src_.samples(src_.ctx, idx_, &frames, &st) : nullptr;
            const uint32_t bytes = d ? frames * 2u * (st ? 2u : 1u) : 0;
            if (pos_ >= bytes) {
                ++idx_;
                pos_ = 0;
                return ImportStep::More;
            }
            // Uncompressed signed 16-bit little-endian (spec 11 §2.5); the
            // sample memory is little-endian int16 already.
            const uint32_t n = bytes - pos_ < 0x4000 ? bytes - pos_ : 0x4000;
            if (!put(reinterpret_cast<const uint8_t*>(d) + pos_, n)) {
                fail("Cannot write file");
                return ImportStep::Failed;
            }
            pos_ += n;
            cost += n;
            ++units_;
            return ImportStep::More;
        }
        case kPatterns: {
            while (idx_ < patNum_ && !patSize_[idx_]) {
                ++idx_;
                ++units_;
            }
            if (idx_ >= patNum_) {
                abort();
                stage_ = kDone;
                units_ = total_;
                return ImportStep::Done;
            }
            const PatternStock* p = src_.pattern(src_.ctx, idx_ + 1);
            uint8_t* b = buf_;
            const uint32_t n = p ? itfmt::encodePattern(*p, src_.instruments, speed_, restoreT_, b + 8) : 0;
            if (n != patSize_[idx_]) {
                fail("Pattern changed during export");
                return ImportStep::Failed;
            }
            wrU16(b, uint16_t(n));
            wrU16(b + 2, uint16_t(p->tracks[0].length()));
            std::memset(b + 4, 0, 4);
            if (!put(b, 8 + n)) {
                fail("Cannot write file");
                return ImportStep::Failed;
            }
            cost += 8 + n;
            ++units_;
            ++idx_;
            return ImportStep::More;
        }
        case kDone:
            return ImportStep::Done;
        default:
            return ImportStep::Failed;
    }
}

int ItExport::percent() const {
    const uint32_t p = units_ * 100u / (total_ ? total_ : 1);
    return p > 100 ? 100 : int(p);
}

void ItExport::abort() {
    if (fs_ && h_ >= 0) fs_->close(h_);
    h_ = -1;
}

}  // namespace ot
