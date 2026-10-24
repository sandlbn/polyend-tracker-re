#include "app/pattern_ops.h"

#include <cmath>
#include <cstring>

#include "sequencer/seq_tables.h"
#include "ui/notes.h"

namespace ot {
namespace pops {

namespace {
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Clear selected columns of one step (spec 06 §1.11 Delete modes).
void clearCols(Step& s, ColMode m) {
    switch (m) {
        case ColMode::All:
            s.clear();
            break;
        case ColMode::Pattern:
            break;
        case ColMode::Note:
            // Note clear also drops instrument and Fx types for sample instruments or special notes.
            if (isSampleInstrument(s.instrument) || s.note < 0) {
                s.instrument = 0;
                s.fx[0].type = 0;
                s.fx[1].type = 0;
            }
            s.note = kNoteEmpty;
            break;
        case ColMode::Instr:
            s.note = kNoteEmpty;
            s.instrument = 0;
            break;
        case ColMode::Fx2:
            s.fx[1].type = 0;
            break;
        case ColMode::Fx1:
            s.fx[0].type = 0;
            break;
    }
}

void copyCols(Step& d, const Step& s, ColMode m, uint8_t curInstr) {
    switch (m) {
        case ColMode::All:
        case ColMode::Pattern:
            d = s;
            break;
        case ColMode::Note:
            d.note = s.note;
            d.instrument = curInstr;
            break;
        case ColMode::Instr:
            d.note = s.note;
            d.instrument = s.instrument;
            break;
        case ColMode::Fx2:
            d.fx[1] = s.fx[1];
            break;
        case ColMode::Fx1:
            d.fx[0] = s.fx[0];
            break;
    }
}

void swapCols(Step& a, Step& b, ColMode m) {
    Step ta = a, tb = b;
    switch (m) {
        case ColMode::All:
        case ColMode::Pattern:
            a = tb;
            b = ta;
            break;
        case ColMode::Note:
        case ColMode::Instr:
            a.note = tb.note;
            a.instrument = tb.instrument;
            b.note = ta.note;
            b.instrument = ta.instrument;
            break;
        case ColMode::Fx2:
            a.fx[1] = tb.fx[1];
            b.fx[1] = ta.fx[1];
            break;
        case ColMode::Fx1:
            a.fx[0] = tb.fx[0];
            b.fx[0] = ta.fx[0];
            break;
    }
}

bool validRect(const SelRect& r) { return r.t0 >= 0 && r.t1 < kTrackCount && r.s0 >= 0 && r.s1 < kStepCount; }
}  // namespace

int length(const Pattern& p) { return p.tracks[0].length(); }

void setLength(Pattern& p, int len) {
    len = clampi(len, 1, kStepCount);
    for (auto& t : p.tracks) t.lastStep = uint8_t(len - 1);
}

uint8_t mapInstrument(int track, int instr) {
    // 2.x synth slots 64..66 play on any of the 16 tracks (Tracker Mini).
    if (instr >= 0 && isSynthInstrument(uint8_t(instr))) return uint8_t(instr);
    if (isMidiTrack(track)) return (instr >= kFirstMidiInstrument && instr < kInstrumentCount) ? uint8_t(instr) : 48;
    return (instr >= 0 && instr < kInstrumentCount) ? uint8_t(instr) : 0;
}

void clearRect(Pattern& p, const SelRect& r, ColMode m) {
    if (!validRect(r)) return;
    for (int t = r.t0; t <= r.t1; ++t)
        for (int s = r.s0; s <= r.s1; ++s) clearCols(p.tracks[t].steps[s], m);
}

void insertRows(Pattern& p, const SelRect& r) {
    // spec 06 §1.4 Insert: steps start..126 shift down, 127 lost, start emptied.
    if (!validRect(r)) return;
    for (int t = r.t0; t <= r.t1; ++t) {
        Step* st = p.tracks[t].steps;
        for (int s = kStepCount - 1; s > r.s0; --s) st[s] = st[s - 1];
        st[r.s0].clear();
        st[r.s0].fx[0].value = st[r.s0].fx[1].value = 0;
    }
}

bool backspaceRows(Pattern& p, const SelRect& r) {
    // spec 06 §1.4 Shift+Delete (backspace): requires start > 0; the rows
    // below pull up by one (to len-1), the last step is cleared and the
    // cursor moves up. OpenTracker reads this as removing the row above the
    // start row, like a text backspace.
    if (!validRect(r) || r.s0 <= 0) return false;
    const int len = length(p);
    for (int t = r.t0; t <= r.t1; ++t) {
        Step* st = p.tracks[t].steps;
        for (int s = r.s0 - 1; s < len - 1; ++s) st[s] = st[s + 1];
        st[len - 1] = Step{};
    }
    return true;
}

void paste(Pattern& dst, const Pattern& clip, const SelRect& src, int dstStep, int dstTrack, ColMode m,
           uint8_t curInstr) {
    // spec 06 §1.11 Paste.
    if (m == ColMode::Pattern) {
        dst = clip;
        return;
    }
    for (int ti = 0; ti <= src.t1 - src.t0; ++ti) {
        const int dt = dstTrack + ti;
        if (dt >= kTrackCount) break;   // 2.x: 16 tracks (2x-01 §2.5)
        const int dlen = dst.tracks[dt].length();
        for (int si = 0; si <= src.s1 - src.s0; ++si) {
            const int ds = dstStep + si;
            if (ds >= kStepCount || ds >= dlen) break;
            Step& d = dst.tracks[dt].steps[ds];
            const Step& s = clip.tracks[src.t0 + ti].steps[src.s0 + si];
            clearCols(d, m);
            copyCols(d, s, m, curInstr);
            if (isMidiTrack(dt) && isSampleInstrument(d.instrument)) {
                if (d.note >= 0) d.note = kNoteEmpty;
                d.instrument = 0;
            }
        }
    }
}

void invert(Pattern& p, const SelRect& r, ColMode m) {
    if (!validRect(r)) return;
    for (int t = r.t0; t <= r.t1; ++t)
        for (int a = r.s0, b = r.s1; a < b; ++a, --b) swapCols(p.tracks[t].steps[a], p.tracks[t].steps[b], m);
}

bool duplicate(Pattern& p) {
    const int len = length(p);
    if (len > 64) return false;
    for (auto& tr : p.tracks)
        for (int s = 0; s < len; ++s) tr.steps[len + s] = tr.steps[s];
    setLength(p, 2 * len);
    return true;
}

bool expand(Pattern& p) {
    const int len = length(p);
    if (len > 64) return false;
    for (auto& tr : p.tracks) {
        for (int s = len - 1; s >= 0; --s) {
            tr.steps[2 * s] = tr.steps[s];
            tr.steps[2 * s + 1] = Step{};
        }
    }
    setLength(p, 2 * len);
    return true;
}

bool shrink(Pattern& p) {
    const int len = length(p);
    if (len <= 1) return false;
    const int nl = ((len - 1) >> 1) + 1;
    for (auto& tr : p.tracks) {
        for (int s = 0; s < nl; ++s) tr.steps[s] = tr.steps[2 * s];
        for (int s = nl; s < len; ++s) tr.steps[s] = Step{};
    }
    setLength(p, nl);
    return true;
}

void writeNote(Pattern& p, const SelRect& r, int note, uint8_t curInstr) {
    // spec 06 §1.6 Note column (0x133f0).
    if (!validRect(r)) return;
    if (note > 127) note = 60;
    if (r.single()) {
        Step& s = p.tracks[r.t0].steps[r.s0];
        if (emptyNote(s.note)) s.instrument = mapInstrument(r.t0, curInstr);
        s.note = int8_t(note);
        return;
    }
    for (int t = r.t0; t <= r.t1; ++t)
        for (int s = r.s0; s <= r.s1; ++s) {
            Step& st = p.tracks[t].steps[s];
            if (st.note != kNoteEmpty) st.note = int8_t(note);
        }
}

void writeInstrument(Pattern& p, const SelRect& r, uint8_t instr) {
    if (!validRect(r)) return;
    for (int t = r.t0; t <= r.t1; ++t)
        for (int s = r.s0; s <= r.s1; ++s) {
            Step& st = p.tracks[t].steps[s];
            const uint8_t v = mapInstrument(t, instr);
            if (r.single()) {
                const bool plays = isSampleInstrument(v) || isSynthInstrument(v);
                if (st.note == kNoteEmpty && plays) st.note = 60;
                if (st.note < kNoteEmpty && plays) continue;   // specials keep theirs
                st.instrument = v;
            } else if (st.note != kNoteEmpty) {
                // 2.x: OFF / CUT / FADE steps of the selection take the new
                // instrument too (user request; stock keeps theirs, so 1.9.x
                // does), so a re-instrumented live take stays consistent.
                st.instrument = v;
            }
        }
}

// The value a step gets when its Fx type changes: the default, inside the
// engine range (Bit Depth defaulted to 0 below its 4..16; manual p.176,
// MANUAL_AUDIT_2026-10-23 #9).
static uint8_t newFxValue(uint8_t type) {
    return uint8_t(clampi(fxInfo(type).defaultValue, seq::fxMin(type), seq::fxMax(type)));
}

void writeFxType(Pattern& p, const SelRect& r, int col, uint8_t type) {
    if (!validRect(r) || col < 2) return;
    const int slot = fxSlotForColumn(col);
    for (int t = r.t0; t <= r.t1; ++t)
        for (int s = r.s0; s <= r.s1; ++s) {
            StepFx& f = p.tracks[t].steps[s].fx[slot];
            if (f.type != type) f.value = newFxValue(type);
            f.type = type;
        }
}

void writeFxValue(Pattern& p, const SelRect& r, int col, uint8_t value) {
    if (!validRect(r) || col < 2) return;
    const int slot = fxSlotForColumn(col);
    for (int t = r.t0; t <= r.t1; ++t)
        for (int s = r.s0; s <= r.s1; ++s) {
            StepFx& f = p.tracks[t].steps[s].fx[slot];
            f.value = uint8_t(clampi(value, seq::fxMin(f.type), seq::fxMax(f.type)));
        }
}

int stepNote(Pattern& p, const SelRect& r, int delta, uint8_t curInstr) {
    // spec 06 §1.7 Note column (0x12c40).
    if (!validRect(r)) return -1;
    if (r.single()) {
        Step& s = p.tracks[r.t0].steps[r.s0];
        if (s.note == kNoteEmpty) {
            if (delta > 0) s.note = 60;
            else s.note = int8_t(clampi(-1 + delta, kNoteOff, -1));
            if (isSampleInstrument(s.instrument)) s.instrument = mapInstrument(r.t0, curInstr);
        } else {
            s.note = int8_t(clampi(s.note + delta, kNoteOff, kNoteMax));
        }
        return s.note;
    }
    for (int t = r.t0; t <= r.t1; ++t)
        for (int s = r.s0; s <= r.s1; ++s) {
            Step& st = p.tracks[t].steps[s];
            if (st.note >= 0) st.note = int8_t(clampi(st.note + delta, 0, 127));
        }
    return -1;
}

void stepInstrument(Pattern& p, const SelRect& r, int delta) {
    if (!validRect(r)) return;
    for (int t = r.t0; t <= r.t1; ++t)
        for (int s = r.s0; s <= r.s1; ++s) {
            Step& st = p.tracks[t].steps[s];
            if (!r.single() && st.note < 0) continue;
            const int lo = isMidiTrack(t) ? kFirstMidiInstrument : 0;
            int v = mapInstrument(t, st.instrument) + delta;
            st.instrument = uint8_t(clampi(v, lo, kFirstSynthInstrument + kSynthSlotCount - 1));   // + S1..S3
            if (r.single() && st.note == kNoteEmpty) st.note = 60;
        }
}

void stepFxValue(Pattern& p, const SelRect& r, int col, int delta) {
    if (!validRect(r) || col < 2) return;
    const int slot = fxSlotForColumn(col);
    for (int t = r.t0; t <= r.t1; ++t)
        for (int s = r.s0; s <= r.s1; ++s) {
            StepFx& f = p.tracks[t].steps[s].fx[slot];
            if (f.type == 0 && !r.single()) continue;
            f.value = uint8_t(clampi(int(f.value) + delta, seq::fxMin(f.type), seq::fxMax(f.type)));
        }
}

int fxOrderIndex(uint8_t type) {
    for (int i = 0; i < kFxCount; ++i)
        if (kFxMenuOrder[i] == type) return i;
    return 0;
}

void stepFxType(Pattern& p, const SelRect& r, int col, int delta) {
    if (!validRect(r) || col < 2) return;
    const int slot = fxSlotForColumn(col);
    for (int t = r.t0; t <= r.t1; ++t)
        for (int s = r.s0; s <= r.s1; ++s) {
            StepFx& f = p.tracks[t].steps[s].fx[slot];
            int idx = fxOrderIndex(f.type) + delta;
            // Wraps to "none" past the end (spec 06 §1.7).
            if (idx >= kFxCount || idx < 0) idx = 0;
            const uint8_t nt = kFxMenuOrder[idx];
            if (nt != f.type) f.value = newFxValue(nt);
            f.type = nt;
        }
}

uint8_t fxValueFromPad(uint8_t type, int pad) {
    // spec 16 §B.7 (0x14458): with n = max - min, a range shorter than the
    // pads gives one value per pad (the rest give max); otherwise
    // round(min + n*p/47). Roll, the LFO rates and Reverse follow from it.
    const int lo = seq::fxMin(type), hi = seq::fxMax(type);
    if (pad < 0) pad = 0;
    if (pad > 47) pad = 47;
    const int n = hi - lo;
    if (n < 47) return uint8_t(lo + (pad < n ? pad : n));
    return uint8_t(std::lround(float(lo) + float(n) * float(pad) / 47.0f));
}

void fill(Pattern& p, const SelRect& r, int col, const FillParams& f, uint8_t rootNote, uint8_t curInstr,
          RandFn rnd, void* rndCtx) {
    // spec 06 §1.10 Apply semantics.
    if (!validRect(r)) return;
    const int L = r.rows();
    auto rand = [&](int lo, int hi) -> int {
        if (hi <= lo) return lo;
        return lo + int(rnd(rndCtx) % uint32_t(hi - lo + 1));
    };
    auto snap = [&](int n) -> int {
        int k = 0;
        while (k < 12 && !isInScale(n + k, rootNote, uint8_t(f.scaleOrFx))) ++k;
        return clampi(n + (k < 12 ? k : 0), 0, 127);
    };
    const int param = f.where >= 4 && f.where <= 6 ? f.param[f.where] : 1;
    for (int t = r.t0; t <= r.t1; ++t) {
        for (int row = 0; row < L; ++row) {
            Step& st = p.tracks[t].steps[r.s0 + row];
            bool sel = false;
            switch (f.where) {
                case 0: sel = st.note >= 0; break;
                case 1: sel = st.note < 0; break;
                case 2: sel = st.fx[0].type != 0 || st.fx[1].type != 0; break;
                case 3: sel = st.fx[0].type == 0 && st.fx[1].type == 0; break;
                case 4: sel = int(rnd(rndCtx) % 128) < param; break;
                case 5: sel = param > 0 && row % param == 0; break;
                case 6: {
                    const int e = param > 0 ? param : 1;
                    const float sp = float(L) / float(e);
                    const float q = std::round(float(row) / sp) * sp;
                    sel = std::fabs(q - float(row)) <= 0.5f;
                    break;
                }
                default: break;
            }
            if (!sel) continue;
            int v;
            if (f.type == 0) v = f.from;
            else if (f.type == 1) v = L > 1 ? f.from + (f.to - f.from) * row / (L - 1) : f.from;
            else v = rand(f.from < f.to ? f.from : f.to, f.from < f.to ? f.to : f.from);
            if (col == 0) {
                int n = snap(v);
                if (f.type == 2) {
                    for (int tries = 0; tries < 100 && !isInScale(n, rootNote, uint8_t(f.scaleOrFx)); ++tries)
                        n = rand(f.from < f.to ? f.from : f.to, f.from < f.to ? f.to : f.from);
                }
                if (st.note < 0) st.instrument = mapInstrument(t, curInstr);
                st.note = int8_t(clampi(n, 0, 127));
            } else if (col == 1) {
                const uint8_t ins = mapInstrument(t, clampi(v, 0, kInstrumentCount - 1));
                if (st.note < 0 && isSampleInstrument(ins)) st.note = 60;
                st.instrument = ins;
            } else {
                StepFx& fx = st.fx[fxSlotForColumn(col)];
                uint8_t type;
                if (f.scaleOrFx < 0) {
                    type = kFxMenuOrder[rand(2, kFxCount - 2)];   // a type of the Fx Type list after None and Off
                } else {
                    type = kFxMenuOrder[clampi(f.scaleOrFx, 0, kFxCount - 1)];   // spec 06 §1.10: index 0 = None
                }
                fx.type = type;
                fx.value = uint8_t(clampi(v, seq::fxMin(type), seq::fxMax(type)));
            }
        }
    }
}

}  // namespace pops
}  // namespace ot
