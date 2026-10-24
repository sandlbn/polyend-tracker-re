// Pattern screen: Generate popup (OT fork; generators in src/core/gen).
//
// Opened from Pattern page 3 (More > More) "Generate" in edit mode. Laid out
// like the Fill popup: F1 picks the generator (list), F2..F7 its parameters
// (lists or value bars; the last is Seed), F8 Generate; Pattern or Rec closes
// it. Pads set the selected field, the encoder / Up / Down change it, Left /
// Right move between fields.
// The target is the selection, or the cursor track from step 1 to the
// pattern length with a single-cell cursor. One undo step per Generate.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "app/live_gen.h"
#include "gen/gen.h"
#include "ui/notes.h"
#include "ui/screens.h"

namespace ot {

namespace {
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
uint32_t gGenEntropy = 0x6A09E667u;
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
}

bool PatternScreen::genFieldBlank(int f) const {
    if (f <= 0) return false;
    return gen::param(genKind, f - 1, genVals[genKind]).name == nullptr;
}

void PatternScreen::genOpenPopup() {
    genInitDefaults();
    genOpen = true;
    genField = 0;
    genMsg[0] = 0;
}

void PatternScreen::genValueText(int f, char* out, int cap) const {
    const gen::ParamDesc& d = gen::param(genKind, f - 1, genVals[genKind]);
    const int v = genVals[genKind][f - 1];
    if (!d.name) {
        out[0] = 0;
    } else if (d.labels) {
        std::snprintf(out, size_t(cap), "%s", d.labels[clampi(v - d.min, 0, d.max - d.min)]);
    } else if (f == gen::kParams && v == 0) {
        std::snprintf(out, size_t(cap), "Random");
    } else {
        std::snprintf(out, size_t(cap), "%d", v);
    }
}

void PatternScreen::genSet(int f, int v, bool wrap) {
    if (f == 0) {
        genKind = wrap && v >= gen::kKindCount ? 0 : clampi(v, 0, gen::kKindCount - 1);
        return;
    }
    const gen::ParamDesc& d = gen::param(genKind, f - 1, genVals[genKind]);
    if (!d.name) return;
    genVals[genKind][f - 1] = int16_t(wrap && v > d.max ? d.min : clampi(v, d.min, d.max));
    if (genKind == gen::kBBCut && f == 1) {
        // A new procedure: its A / B take their defaults.
        for (int i = 1; i <= 2; ++i) {
            const gen::ParamDesc& pd = gen::param(genKind, i, genVals[genKind]);
            if (pd.name) genVals[genKind][i] = pd.def;
        }
    }
}

int PatternScreen::genGet(int f) const { return f == 0 ? genKind : genVals[genKind][f - 1]; }

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
    } else if (id == btn::Right || id == btn::Left) {
        const int d = id == btn::Right ? 1 : -1;
        for (int k = genField + d; k >= 0 && k <= gen::kParams; k += d)
            if (!genFieldBlank(k)) {
                genField = k;
                break;
            }
    } else if (id == btn::Up || id == btn::Down) {
        // List fields step down their list with Down; numbers count up with Up.
        bool list = genField == 0;
        if (genField > 0) list = gen::param(genKind, genField - 1, genVals[genKind]).labels != nullptr;
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
    const gen::ParamDesc& d = gen::param(genKind, genField - 1, genVals[genKind]);
    if (!d.name) return;
    const int span = d.max - d.min;
    // Lists: pad n = item n. Numbers: the 48 pads spread over the range.
    const int v = d.labels || span <= 47 ? d.min + lp : d.min + (lp * span + 23) / 47;
    genSet(genField, v);
}

void PatternScreen::genApply() {
    App& a = app_;
    SelRect r = target();
    gen::Target t;
    t.pat = &a.pattern();
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
    // BBCut / Turnaround stutter want a sliced sample: the current one, else
    // the first sliced sample instrument already used on the track.
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
    gGenEntropy = gGenEntropy * 1664525u + 1013904223u + uint32_t(step * 131 + track * 7919);
    t.entropy = gGenEntropy;
    // Checks that need no pattern change come first (no empty undo step).
    if (genKind == gen::kBBCut && t.slices.count == 0) {
        std::snprintf(genMsg, sizeof(genMsg), "Needs a sliced instrument");
        ui_.notify(genMsg);
        return;
    }
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
    if (genKind == gen::kAcid) live::acid(a, genVals[genKind], t, res.rngSeed);   // Mutate: keeps changing per loop
    a.patternEdited();
    genOpen = false;
    ui_.notify(res.msg);
}

void PatternScreen::genBar(BottomBar& b) {
    char v[24];
    b.set(0, "Generator", gen::kind(genKind).name);
    for (int f = 1; f <= gen::kParams; ++f) {
        const gen::ParamDesc& d = gen::param(genKind, f - 1, genVals[genKind]);
        if (!d.name) {
            b.set(f, "", nullptr, false);
            continue;
        }
        genValueText(f, v, sizeof(v));
        b.set(f, d.name, v);
    }
    b.set(7, "Generate");
}

void PatternScreen::drawGen(Gfx& g) {
    const Theme& th = ui_.th();
    constexpr int kTop = 58, kListH = 365, kBarH = 364;
    const gen::KindDesc& kd = gen::kind(genKind);
    char title[48];
    std::snprintf(title, sizeof(title), "Generate %s", kd.name);
    g.fill(0, wg::kMainTop, kScreenW, kTop - wg::kMainTop, th.slot(slot::kTitle + 1));
    g.text(10, wg::kMainTop + 12, kFont4, th.slot(slot::kTitle), title, IDisplay::kOptCenterY);
    g.text(790, wg::kMainTop + 12, kFont3, th.slot(slot::kTitle), genMsg[0] ? genMsg : kd.help,
           IDisplay::kOptCenterY | IDisplay::kOptRightX);
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
    for (int f = 1; f <= gen::kParams; ++f) {
        const gen::ParamDesc& d = gen::param(genKind, f - 1, genVals[genKind]);
        const int v = genVals[genKind][f - 1];
        if (!d.name) wg::panel(g, th, wg::columnX(f), kTop, wg::columnW(), kListH);
        else if (d.labels) list(f, d.max - d.min + 1, v - d.min, d.labels);
        else {
            const float span = float(d.max - d.min);
            wg::valueBar(g, th, wg::columnX(f), kTop, wg::columnW(), kBarH, span > 0 ? float(v - d.min) / span : 0);
        }
    }
    wg::panel(g, th, wg::columnX(7), kTop, wg::columnW(), kListH);
    if (genKind == gen::kAcid && live::acidActive(track))
        g.text(wg::columnX(7) + 48, kTop + 20, kFont2, th.slot(slot::kTitle), "LIVE", IDisplay::kOptCenterX);
    wg::focusFrame(g, th, wg::columnX(genField), kTop, wg::columnX(genField) + wg::columnW(), 422);
}

}  // namespace ot
