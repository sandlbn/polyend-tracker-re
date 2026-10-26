// Pattern screen: Generate popup (OT fork; generators in src/core/gen).
//
// Opened from Pattern page 3 (More > More) "Generate" in edit mode. Laid out
// like the Fill popup: F1 picks the generator (list), F2..F7 its parameters
// (lists or value bars), F8 Generate; Pattern or Rec closes it. Two pages of
// parameters: Shift turns the page (page 1 F7 is always Seed). Pads set the
// selected field, the encoder / Up / Down change it, Left / Right move
// between fields, the encoder push draws a new random result.
//
// The right column previews what F8 will write: the generator runs on a copy
// of the pattern (SDRAM) whenever a parameter changes, with the same random
// draw F8 uses, so the preview is exactly the result. A playhead runs over it
// while the pattern plays. The title line explains the selected parameter.
//
// The target is the selection, or the cursor track from step 1 to the
// pattern length with a single-cell cursor. One undo step per Generate.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "app/live_gen.h"
#include "gen/gen.h"
#include "sdram_bss.h"
#include "ui/notes.h"
#include "ui/screens.h"

namespace ot {

namespace {
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
// The preview's working copy of the pattern (main loop only).
OT_SDRAM_BSS SdramLazy<Pattern> gPrevPat;
}  // namespace

void PatternScreen::genInitDefaults() {
    if (genInit_) return;
    genInit_ = true;
    for (int k = 0; k < gen::kKindCount; ++k) {
        for (int i = 0; i < gen::kParams; ++i) genVals[k][i] = gen::kind(k).p[i].def;
        for (int i = 0; i < gen::kParams; ++i) {
            const gen::ParamDesc& d = gen::param(k, i, genVals[k]);
            if (d.name) genVals[k][i] = d.def;
        }
    }
    // BBCut A / B per procedure: their defaults.
    for (int pr = 0; pr < 10; ++pr) {
        int16_t v[gen::kParams] = {};
        v[0] = int16_t(pr);
        for (int i = 1; i <= 2; ++i) {
            const gen::ParamDesc& d = gen::param(gen::kBBCut, i, v);
            genBBAB[pr][i - 1] = d.name ? d.def : 0;
        }
    }
}

bool PatternScreen::genFieldBlank(int f) const {
    if (f <= 0) return false;
    return gen::param(genKind, genParamIndex(f), genVals[genKind]).name == nullptr;
}

void PatternScreen::genOpenPopup() {
    genInitDefaults();
    genOpen = true;
    genField = 0;
    genPage = 0;
    genMsg[0] = 0;
    genChanged();
}

void PatternScreen::genChanged() {
    genPrevDirty_ = true;
    genMsg[0] = 0;
    ui_.dirty();
}

void PatternScreen::genValueText(int f, char* out, int cap) const {
    const int idx = genParamIndex(f);
    const gen::ParamDesc& d = gen::param(genKind, idx, genVals[genKind]);
    const int v = genVals[genKind][idx];
    if (!d.name) {
        out[0] = 0;
    } else if (d.labels) {
        std::snprintf(out, size_t(cap), "%s", d.labels[clampi(v - d.min, 0, d.max - d.min)]);
    } else if (idx == gen::kSeedParam && v == 0) {
        std::snprintf(out, size_t(cap), "Random");
    } else if (d.min < 0) {
        std::snprintf(out, size_t(cap), "%+d", v);
    } else {
        std::snprintf(out, size_t(cap), "%d", v);
    }
}

void PatternScreen::genSet(int f, int v, bool wrap) {
    if (f == 0) {
        genKind = wrap && v >= gen::kKindCount ? 0 : clampi(v, 0, gen::kKindCount - 1);
        genChanged();
        return;
    }
    const int idx = genParamIndex(f);
    const gen::ParamDesc& d = gen::param(genKind, idx, genVals[genKind]);
    if (!d.name) return;
    genVals[genKind][idx] = int16_t(wrap && v > d.max ? d.min : clampi(v, d.min, d.max));
    if (genKind == gen::kBBCut) {
        const int pr = clampi(genVals[genKind][0], 0, 9);
        if (idx == 0) {
            // Another procedure: its own A / B come back.
            genVals[genKind][1] = genBBAB[pr][0];
            genVals[genKind][2] = genBBAB[pr][1];
        } else if (idx == 1 || idx == 2) {
            genBBAB[pr][idx - 1] = genVals[genKind][idx];
        }
        live::setBBParams(pr, genBBAB[pr][0], genBBAB[pr][1]);
    }
    genChanged();
}

int PatternScreen::genGet(int f) const { return f == 0 ? genKind : genVals[genKind][genParamIndex(f)]; }

void PatternScreen::genDelta(int d, bool wrap) {
    if (d > 5) d = 5;
    if (d < -5) d = -5;
    genSet(genField, genGet(genField) + d, wrap);
}

void PatternScreen::genButton(uint8_t id) {
    if (id <= btn::F7) {
        const int f = int(id);
        if (genFieldBlank(f)) return;
        if (genField == f) genDelta(1, true);   // pressing the selected field again steps it, with wrap
        genField = f;
    } else if (id == btn::Pattern || id == btn::Rec) {
        genOpen = false;
    } else if (id == btn::F8) {
        genApply();
    } else if (id == btn::Shift) {
        genPage ^= 1;
    } else if (id == btn::EncPush) {
        genEntropy_ = genEntropy_ * 1664525u + 1013904223u;   // another random draw
        genChanged();
    } else if (id == btn::Right || id == btn::Left) {
        const int d = id == btn::Right ? 1 : -1;
        for (int k = genField + d; k >= 0 && k <= gen::kPageParams; k += d)
            if (!genFieldBlank(k)) {
                genField = k;
                break;
            }
    } else if (id == btn::Up || id == btn::Down) {
        // List fields step down their list with Down; numbers count up with Up.
        bool list = genField == 0;
        if (genField > 0) list = gen::param(genKind, genParamIndex(genField), genVals[genKind]).labels != nullptr;
        genDelta((id == btn::Down) == list ? 1 : -1);
    }
    if (genFieldBlank(genField)) genField = 0;
}

void PatternScreen::genPad(uint8_t pad) {
    const int lp = pad;   // pad index as the Fill popup reads it (item n = pad n)
    if (genField == 0) {
        genSet(0, lp);
        return;
    }
    const gen::ParamDesc& d = gen::param(genKind, genParamIndex(genField), genVals[genKind]);
    if (!d.name) return;
    const int span = d.max - d.min;
    // Lists: pad n = item n. Numbers: the 48 pads spread over the range.
    const int v = d.labels || span <= 47 ? d.min + lp : d.min + (lp * span + 23) / 47;
    genSet(genField, v);
}

// The generate target for the current selection (pattern `pat`), false when
// a check fails before anything is written (message in genMsg).
bool PatternScreen::genTarget(gen::Target& t, Pattern* pat, uint8_t* insOut) {
    App& a = app_;
    SelRect r = target();
    t = gen::Target{};
    t.pat = pat;
    if (r.single()) {
        t.s0 = 0;
        t.s1 = len() - 1;
    } else {
        t.s0 = r.s0;
        t.s1 = r.s1;
    }
    t.t0 = r.t0;
    t.t1 = r.t1;
    uint8_t ins = a.currentInstrument();
    // BBCut / Station slices want a sliced sample: the current one, else the
    // first sliced sample instrument already used on the track.
    auto sliced = [&](uint8_t i) {
        return i < kSampleInstrumentCount && a.instrPresent[i] && a.instruments[i].sliceCount > 0;
    };
    if ((genKind == gen::kBBCut || genKind == gen::kStation) && !sliced(ins)) {
        const Track& tr = a.pattern().tracks[t.t0];
        for (int s = 0; s <= tr.lastStep; ++s) {
            const Step& st = tr.steps[s];
            if (st.note >= 0 && sliced(st.instrument)) {
                ins = st.instrument;
                break;
            }
        }
    }
    if (insOut) *insOut = ins;
    // Track type: sample instruments on MIDI tracks become MIDI channel 1.
    t.instrument = pops::mapInstrument(t.t0, ins);
    const int pc = a.rootNote() % 12;
    // Synths and MIDI an octave lower than samples (unity note 60).
    t.rootNote = (ins < kSampleInstrumentCount ? 48 : 36) + pc;
    const uint8_t sc = a.scale();
    t.scaleMask = sc == 0 ? 0 : scaleMask(sc);   // chromatic: let the generator use minor
    t.stepsPerBeat = 4;
    if (sliced(ins)) {
        const Instrument& in = a.instruments[ins];
        t.slices.count = in.sliceCount > kSliceCount ? kSliceCount : in.sliceCount;
        t.slices.start = in.slices;
        t.slices.beatSlice = in.playMode == uint8_t(PlayMode::BeatSlice);
        const float bpm = a.seq.effectiveTempo();
        t.slices.sampleBeats = bpm > 1 ? float(in.lengthFrames) / 44100.0f * bpm / 60.0f : 0;
    }
    if (genKind == gen::kStation) {   // the radio's station: RDS name, PI, frequency
        t.freq10 = a.cfg.radioFreq10;
        if (a.radio()) {
            t.text = a.radio()->rdsName();
            t.pi = a.radio()->rdsPi();
        }
    }
    t.entropy = genEntropy_;
    if (genKind == gen::kBBCut && t.slices.count == 0) {
        std::snprintf(genMsg, sizeof(genMsg), "Needs a sliced instrument");
        return false;
    }
    return true;
}

void PatternScreen::genPreview() {
    if (!genPrevDirty_) return;
    genPrevDirty_ = false;
    Pattern& pp = gPrevPat.get();
    std::memcpy(static_cast<void*>(&pp), static_cast<const void*>(&app_.pattern()), sizeof(Pattern));
    gen::Target t;
    char keep[sizeof(genMsg)];
    std::memcpy(keep, genMsg, sizeof(keep));
    if (!genTarget(t, &pp, nullptr)) {
        genPrev_ = gen::Result{};
        std::snprintf(genPrev_.msg, sizeof(genPrev_.msg), "%s", genMsg);
        std::memcpy(genMsg, keep, sizeof(genMsg));
        genPrevN_ = 0;
        return;
    }
    genPrev_ = gen::generate(genKind, genVals[genKind], t);
    genPrevT0_ = t.t0;
    // Call+Resp on two tracks: show the answer track too.
    genPrevT1_ = t.t0;
    if (genKind == gen::kCallResponse && genVals[genKind][3] == 2)
        genPrevT1_ = t.t1 > t.t0 ? t.t1 : (t.t0 + 1 < kTrackCount ? t.t0 + 1 : t.t0);
    genPrevS0_ = t.s0;
    genPrevN_ = t.s1 - t.s0 + 1;
}

void PatternScreen::genApply() {
    App& a = app_;
    gen::Target t;
    if (!genTarget(t, &a.pattern(), nullptr)) {
        ui_.notify(genMsg);
        return;
    }
    // Checks that need no pattern change come first (no empty undo step).
    if (genKind == gen::kCallResponse && genVals[genKind][3] != 2 && t.s1 - t.s0 < 3) {
        std::snprintf(genMsg, sizeof(genMsg), "Select at least 4 steps");
        ui_.notify(genMsg);
        return;
    }
    a.pushUndo();
    const gen::Result res = gen::generate(genKind, genVals[genKind], t);
    std::snprintf(genMsg, sizeof(genMsg), "%s", res.msg);
    if (!res.ok) {
        ui_.notify(res.msg);
        return;
    }
    for (int tr = t.t0; tr <= t.t1; ++tr) live::release(tr);   // another generator owns the steps now
    if (genKind == gen::kAcid) live::acid(a, genVals[genKind], t, res.rngSeed);   // live density / Mutate
    if (genKind == gen::kBBCut) live::bbcut(a, genVals[genKind], t, res.rngSeed);   // live Mutate
    a.patternEdited();
    genOpen = false;
    ui_.notify(res.msg);
    // The next popup shows a new random draw.
    genEntropy_ = genEntropy_ * 1664525u + 1013904223u + uint32_t(step * 131 + track * 7919);
    genPrevDirty_ = true;
}

void PatternScreen::genBar(BottomBar& b) {
    char v[24];
    b.set(0, "Generator", gen::kind(genKind).name);
    for (int f = 1; f <= gen::kPageParams; ++f) {
        const gen::ParamDesc& d = gen::param(genKind, genParamIndex(f), genVals[genKind]);
        if (!d.name) {
            b.set(f, "", nullptr, false);
            continue;
        }
        genValueText(f, v, sizeof(v));
        b.set(f, d.name, v);
    }
    b.set(7, "Generate");
}

// Preview column: the target track(s) one row per step (up to 64 rows; longer
// targets fold two steps per row). A note's x position follows its pitch (or
// slice) within the notes of the result; accents are brighter, FX steps get a
// tick, OFF a dash. Call+Resp parts are split and tagged C / R; Station shows
// what it read from the radio and the digits.
void PatternScreen::drawGenPreview(Gfx& g, int x, int y, int w, int h) {
    const Theme& th = ui_.th();
    App& a = app_;
    g.fill(x, y, w, h, th.panelDark);
    if (genPrevN_ <= 0 || !genPrev_.ok) {
        const char* m = genPrev_.msg[0] ? genPrev_.msg : "No preview";
        // Wrap the message on spaces into the column (font 1, 9 chars per line).
        char line[12];
        int li = 0, yy = y + 14;
        for (const char* p = m;; ++p) {
            if (*p == ' ' || *p == 0 || li >= 9) {
                line[li] = 0;
                if (li) g.text(x + 4, yy, kFont1, th.dim, line, IDisplay::kOptCenterY);
                yy += 18;
                li = 0;
                if (!*p) break;
                if (*p != ' ') line[li++] = *p;
            } else {
                line[li++] = *p;
            }
        }
        return;
    }
    if (genKind == gen::kStation && genPrev_.info[0]) {
        int yy = y + 12;
        char line[16];
        int li = 0;
        int ln = 0;
        for (const char* p = genPrev_.info;; ++p) {
            if (*p == '\n' || *p == 0) {
                line[li] = 0;
                g.text(x + 4, yy, kFont1, ln < 3 ? th.dim : th.text, line, IDisplay::kOptCenterY);
                yy += ln == 2 ? 26 : 18;   // a gap between the sources and the digits
                li = 0;
                ++ln;
                if (!*p || yy > y + h - 9) break;
            } else if (li < 15) {
                line[li++] = *p;
            }
        }
        return;
    }
    const Pattern& pp = gPrevPat.get();
    const int n = genPrevN_;
    const int lanes = genPrevT1_ != genPrevT0_ ? 2 : 1;
    const int per = n > 64 ? 2 : 1;            // steps per row
    const int rows = (n + per - 1) / per;
    const int rowH = rows > 0 ? (h - 4) / rows : 1;
    const int laneW = (w - 4) / lanes;
    // Note range of the result.
    int lo = 127, hi = 0;
    for (int ln = 0; ln < lanes; ++ln) {
        const Track& tr = pp.tracks[ln == 0 ? genPrevT0_ : genPrevT1_];
        for (int i = 0; i < n; ++i) {
            const int nt = tr.steps[genPrevS0_ + i].note;
            if (nt >= 0) {
                lo = nt < lo ? nt : lo;
                hi = nt > hi ? nt : hi;
            }
        }
    }
    if (hi < lo) lo = hi = 60;
    const int span = hi - lo;
    const int bw = laneW > 30 ? 10 : 6;
    for (int ln = 0; ln < lanes; ++ln) {
        const Track& tr = pp.tracks[ln == 0 ? genPrevT0_ : genPrevT1_];
        const int lx = x + 2 + ln * laneW;
        if (ln) g.fill(lx - 1, y + 2, 1, h - 4, th.grid);
        // Normal notes, then accents (brighter), then FX ticks and OFF dashes.
        for (int pass = 0; pass < 3; ++pass) {
            g.rectsBegin(pass == 0 ? th.track[0] : (pass == 1 ? th.text : th.track[2]));
            for (int i = 0; i < n; ++i) {
                const Step& st = tr.steps[genPrevS0_ + i];
                const int ry = y + 2 + (i / per) * rowH;
                const int rh = rowH > 2 ? rowH - 1 : 1;
                const bool accent = (st.fx[0].type == 18 && st.fx[0].value >= 100) ||
                                    (st.fx[1].type == 18 && st.fx[1].value >= 100);
                if (pass < 2 && st.note >= 0 && accent == (pass == 1)) {
                    const int nx = lx + (span > 0 ? (st.note - lo) * (laneW - bw - 2) / span : (laneW - bw) / 2);
                    g.rectsAdd(nx, ry, bw, rh);
                } else if (pass == 2) {
                    if (st.note == kNoteOff) g.rectsAdd(lx + 2, ry + rh / 2, laneW - 6, 1);
                    if (st.fx[0].type || st.fx[1].type) g.rectsAdd(lx + laneW - 4, ry, 2, rh);
                }
            }
            g.rectsEnd();
        }
    }
    // Sections (Call+Resp form).
    for (int k = 0; k < genPrev_.nSec; ++k) {
        const int ry = y + 2 + (genPrev_.secStart[k] / per) * rowH;
        if (k) g.fill(x + 1, ry - 1, w - 2, 1, th.accent);
        const char tag[2] = {genPrev_.secTag[k], 0};
        g.text(x + w - 10, ry + 8, kFont1, th.accent, tag, IDisplay::kOptCenter);
    }
    // Playhead.
    if (a.seq.isPlaying()) {
        const int s = a.seq.step(genPrevT0_) - genPrevS0_;
        if (s >= 0 && s < n) g.fill(x + 1, y + 2 + (s / per) * rowH, w - 2, 2, th.accent);
    }
}

void PatternScreen::drawGen(Gfx& g) {
    const Theme& th = ui_.th();
    // Lists end above a help strip that explains the selected parameter.
    constexpr int kTop = 58, kListH = 343, kBarH = 342, kHelpY = kTop + kListH + 2, kHelpH = 20;
    genPreview();
    const gen::KindDesc& kd = gen::kind(genKind);
    char title[48];
    std::snprintf(title, sizeof(title), "Generate %s  %d/2", kd.name, genPage + 1);
    g.fill(0, wg::kMainTop, kScreenW, kTop - wg::kMainTop, th.slot(slot::kTitle + 1));
    g.text(10, wg::kMainTop + 12, kFont4, th.slot(slot::kTitle), title, IDisplay::kOptCenterY);
    g.text(790, wg::kMainTop + 12, kFont3, th.slot(slot::kTitle), genMsg[0] ? genMsg : kd.help,
           IDisplay::kOptCenterY | IDisplay::kOptRightX);
    const char* help = genField == 0 ? "F1 generator, F2-F7 settings, Shift: page 2, push: new draw, F8 writes the preview" : nullptr;
    if (genField > 0) {
        const gen::ParamDesc& d = gen::param(genKind, genParamIndex(genField), genVals[genKind]);
        help = d.help ? d.help : d.name;
    }
    g.fill(0, kHelpY, kScreenW, kHelpH, th.panel2);
    if (help) g.text(10, kHelpY + kHelpH / 2, kFont3, th.text, help, IDisplay::kOptCenterY);
    struct Ctx {
        const char* const* labels;
        int first;
    };
    auto item = [](void* ctx, int i, char* out, int cap) {
        const Ctx* c = static_cast<const Ctx*>(ctx);
        if (c->labels) std::snprintf(out, size_t(cap), "%s", c->labels[i]);
        else std::snprintf(out, size_t(cap), "%s", gen::kind(i).name);
    };
    auto list = [&](int k, int count, int cur, const char* const* labels) {
        Ctx ctx{labels, 0};
        wg::ListView lv;
        lv.x = wg::columnX(k);
        lv.y = kTop;
        lv.w = wg::columnW();
        lv.h = kListH;
        lv.count = count;
        lv.sel = cur;
        lv.top = wg::listFollow(lv.sel, count, wg::listVisibleRows(lv.h), 0);
        lv.selStyle = wg::ListSel::kWhite;
        lv.item = item;
        lv.ctx = &ctx;
        wg::list(g, th, lv);
    };
    list(0, gen::kKindCount, genKind, nullptr);
    for (int f = 1; f <= gen::kPageParams; ++f) {
        const int idx = genParamIndex(f);
        const gen::ParamDesc& d = gen::param(genKind, idx, genVals[genKind]);
        const int v = genVals[genKind][idx];
        if (!d.name) wg::panel(g, th, wg::columnX(f), kTop, wg::columnW(), kListH);
        else if (d.labels) list(f, d.max - d.min + 1, v - d.min, d.labels);
        else {
            const float span = float(d.max - d.min);
            wg::valueBar(g, th, wg::columnX(f), kTop, wg::columnW(), kBarH, span > 0 ? float(v - d.min) / span : 0);
        }
    }
    drawGenPreview(g, wg::columnX(7), kTop, wg::columnW(), kListH);
    if ((genKind == gen::kAcid && live::acidActive(track)) || (genKind == gen::kBBCut && live::bbcutActive(track)))
        g.text(wg::columnX(7) + 48, kTop + kListH - 12, kFont2, th.accent, "LIVE", IDisplay::kOptCenter);
    wg::focusFrame(g, th, wg::columnX(genField), kTop, wg::columnX(genField) + wg::columnW(), kTop + kListH);
}

}  // namespace ot
