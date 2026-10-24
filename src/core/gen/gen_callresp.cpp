// Call & Answer generator: a C++ port of the phrase logic of Nordl3's
// Call / Response Generator (MIT, github.com/Nordl3/Call-Response-Generator):
// style profiles, motif template, call builder, the 38 answer strategies,
// articulation and endings. The browser tool's other menus are fixed at its
// defaults (motif "Acid nibble", contour "Stepwise", medium register,
// "Repetitive", note pool with spice notes, articulation "Acidic", gate
// "Mixed", "Legato", slides "Occasional", "Motif accents", ending "Response
// more resolved than call").
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "gen/gen_internal.h"

namespace ot {
namespace gen {

namespace {

enum Cat : uint8_t { kCatAcid, kCatTechno, kCatHouse, kCatRave, kCatOther };

// Style presets (stylePresetMap). Fields as hundredths; 255 = not set (the
// keyword defaults of styleProfile apply).
struct StylePreset {
    const char* name;
    uint8_t cat;
    uint8_t density, repeat, slide, accent, rootBias, syncBias, choppy, sustain, rest, motifVar, tight, leap,
        offbeat, octave, stableEnd;
};
constexpr uint8_t U = 255;
const StylePreset kStyles[kCrStyleCount] = {
    // name           cat        dens rep slide acc root sync chop sus rest mvar tight leap off  oct  stbl
    {"Classic Acid", kCatAcid, 56, 66, 38, 46, 30, 28, U, U, U, U, U, U, U, U, U},
    {"Raw Acid", kCatAcid, 62, 58, 32, 54, 24, U, 24, U, U, U, U, U, U, U, U},
    {"Squelchy Acid", kCatAcid, 54, 62, 46, 42, U, U, U, 28, U, U, U, U, U, U, U},
    {"Dark Acid", kCatAcid, 46, 70, 26, 34, 38, U, U, U, 20, U, U, U, U, U, U},
    {"Hypno Acid", kCatAcid, 48, 78, 24, 28, U, U, U, 42, U, U, U, U, U, U, U},
    {"Sparse Acid", kCatAcid, 34, 74, 22, 30, U, U, U, U, 30, U, U, U, U, U, U},
    {"Busy Acid", kCatAcid, 72, 54, 36, 44, U, U, U, U, U, 24, U, U, U, U, U},
    {"Acid Stabs", kCatAcid, 42, 66, 10, 52, U, U, 46, U, U, U, U, U, U, U, U},
    {"Acid Runner", kCatAcid, 70, 60, 34, 36, U, U, U, U, U, U, U, 26, U, U, U},
    {"Acid Q/A", kCatAcid, 52, 68, 26, 40, U, U, U, U, U, U, 78, U, U, U, U},
    {"Driving Tech", kCatTechno, 44, 78, U, 24, 38, U, 34, U, U, U, U, U, U, U, U},
    {"Hypno Techno", kCatTechno, 36, 86, U, 18, U, U, U, 28, U, U, 78, U, U, U, U},
    {"Min Techno", kCatTechno, 28, 84, U, 16, U, U, 40, U, 34, U, U, U, U, U, U},
    {"Raw Warehouse", kCatTechno, 48, 66, U, 34, U, U, 42, U, U, U, U, 18, U, U, U},
    {"Tool Techno", kCatTechno, 40, 82, U, 18, 44, U, U, U, U, U, U, U, U, U, U},
    {"Dub Techno", kCatTechno, 26, 88, U, 14, U, U, U, 36, 32, U, U, U, U, U, U},
    {"Industrial", kCatTechno, 46, 60, U, 38, U, U, 48, U, U, U, U, 20, U, U, U},
    {"Perc Techno", kCatTechno, 42, 74, U, 32, U, U, 44, U, U, U, U, U, U, U, U},
    {"Loop Techno", kCatTechno, 38, 90, U, 16, U, U, U, U, U, U, 84, U, U, U, U},
    {"Broken Tech", kCatTechno, 44, 48, U, 24, U, U, U, U, U, 28, U, 22, U, U, U},
    {"Jackin House", kCatHouse, 52, 66, U, 24, U, U, U, U, U, U, U, U, 42, U, U},
    {"Deep House", kCatHouse, 36, 76, U, 18, U, U, U, 24, U, U, U, U, 36, U, U},
    {"Min House", kCatHouse, 30, 82, U, 18, U, U, U, U, 26, U, U, U, 30, U, U},
    {"Classic House", kCatHouse, 46, 70, U, 22, U, U, U, U, U, U, U, U, 38, U, U},
    {"Tech House", kCatHouse, 44, 68, U, 22, U, U, 22, U, U, U, U, U, 28, U, U},
    {"Bass House", kCatHouse, 58, 62, U, 28, 40, U, U, U, U, U, U, U, 26, U, U},
    {"Shuffly House", kCatHouse, 48, 66, U, 22, U, U, U, 18, U, U, U, U, 48, U, U},
    {"Organ House", kCatHouse, 42, 72, U, 18, U, U, U, 30, U, U, U, U, 34, U, U},
    {"House Stabs", kCatHouse, 40, 70, U, 34, U, U, 34, U, U, U, U, U, 30, U, U},
    {"House Bounce", kCatHouse, 50, 68, U, 22, U, U, U, U, U, U, U, 10, 50, U, U},
    {"90s Rave", kCatRave, 56, 46, U, 28, U, U, U, U, U, U, U, 30, U, 22, U},
    {"Dark Rave", kCatRave, 44, 56, U, 24, 30, U, U, U, U, U, U, 24, U, U, U},
    {"Rave Stab", kCatRave, 40, 62, U, 38, U, U, 36, U, U, U, U, U, U, U, U},
    {"Electro Pulse", kCatRave, 46, 62, U, 24, U, U, U, U, U, U, U, U, 24, U, U},
    {"Breaky Elec", kCatRave, 48, 42, U, 22, U, U, U, U, U, 26, U, U, U, U, U},
    {"Detroit-ish", kCatOther, 40, 70, U, 18, 34, U, U, 20, U, U, U, U, U, U, U},
    {"Proto-Trance", kCatRave, 48, 64, U, 18, U, U, U, 24, U, U, U, 18, U, U, U},
    {"Hoover Riff", kCatRave, 52, 50, U, 30, U, U, U, U, U, U, U, 34, U, U, U},
    {"Motif Study", kCatOther, 42, 92, U, 14, U, U, U, U, U, 6, 88, U, U, U, U},
    {"Rooted Groove", kCatOther, 40, 80, U, 16, 56, U, U, U, U, U, U, U, U, U, U},
    {"Repeat Hook", kCatOther, 46, 90, U, 18, U, U, U, U, U, 10, U, U, U, U, U},
    {"Wide Answer", kCatOther, 44, 60, U, 18, U, U, U, U, U, U, 46, 28, U, U, U},
    {"Tight Answer", kCatOther, 44, 72, U, 18, U, U, U, U, U, U, 88, U, U, U, U},
    {"Tension", kCatOther, 48, 66, U, 22, U, U, U, U, U, U, U, 22, U, U, 40},
    {"Resolver", kCatOther, 40, 74, U, 16, U, U, U, U, U, U, 78, U, U, U, 90},
    {"Lead/Support", kCatOther, 42, 72, U, 18, U, U, U, U, U, U, 84, U, U, U, U},
};

const char* const kStrategies[kCrStrategyCount] = {
    "Same+resolve", "Same+contour", "Simplify",    "Extend",      "Truncate",    "Echo",        "Upward",
    "Downward",     "Higher",       "Lower",       "Invert",      "Mirror",      "Rhythm only", "Notes only",
    "Transp up",    "Transp down",  "To root",     "To 5th",      "Res lower",   "Res harder",  "Repeat+brk",
    "Tighter",      "Sparser",      "Busier",      "Accented",    "Slid",        "Staccato",    "Legato",
    "Delayed",      "Half phrase",  "Fragment",    "Tail",        "Opposite",    "Darker",      "Brighter",
    "Q+closure",    "Hook+fill",    "Dom/tonic",
};

struct Profile {
    float density, leap, repeat, stableEnd, accent, slide, restBias, motifVar, tight, rootBias, octave, sync,
        offbeat, sustain, choppy;
    uint8_t cat;
};

// Substring test (no strstr: it pulls ~1.5 KB of libc into the image).
bool has(const char* s, const char* k) {
    for (; *s; ++s) {
        int i = 0;
        while (k[i] && s[i] == k[i]) ++i;
        if (!k[i]) return true;
    }
    return false;
}

Profile profile(int style) {
    const StylePreset& sp = kStyles[style];
    char lower[24];
    int i = 0;
    for (; sp.name[i] && i < 23; ++i) lower[i] = char(sp.name[i] >= 'A' && sp.name[i] <= 'Z' ? sp.name[i] + 32 : sp.name[i]);
    lower[i] = 0;
    Profile p{0.45f, 0.18f, 0.45f, 0.7f, 0.25f, 0.18f, 0.18f, 0.18f, 0.5f, 0.22f, 0.12f, 0.18f, 0.14f, 0.18f, 0.14f, sp.cat};
    if (has(lower, "acid")) {
        p.density = 0.55f; p.leap = 0.22f; p.repeat = 0.62f; p.accent = 0.45f; p.slide = 0.35f;
        p.rootBias = 0.28f; p.tight = 0.66f; p.sync = 0.26f; p.sustain = 0.34f;
    }
    if (has(lower, "raw")) { p.density += 0.08f; p.accent += 0.08f; p.motifVar += 0.08f; p.choppy += 0.12f; }
    if (has(lower, "hypno")) { p.repeat += 0.15f; p.leap -= 0.08f; p.stableEnd = 0.55f; p.restBias = 0.1f; p.tight = 0.72f; p.sustain += 0.16f; }
    if (has(lower, "min")) { p.density -= 0.12f; p.repeat += 0.12f; p.restBias = 0.28f; }
    if (has(lower, "busy")) { p.density += 0.18f; p.restBias -= 0.06f; }
    if (has(lower, "sparse")) { p.density -= 0.18f; p.restBias += 0.12f; }
    if (has(lower, "house")) {
        p.density = 0.48f; p.leap = 0.12f; p.repeat = 0.58f; p.accent = 0.22f; p.slide = 0.1f; p.rootBias = 0.32f;
        p.tight = 0.62f; p.offbeat = 0.3f; p.sync = 0.2f; p.sustain = 0.12f;
    }
    if (has(lower, "tech") && !has(lower, "house")) {
        p.density = 0.42f; p.leap = 0.14f; p.repeat = 0.68f; p.accent = 0.2f; p.slide = 0.08f; p.rootBias = 0.35f;
        p.restBias = 0.24f; p.tight = 0.7f; p.choppy = 0.3f; p.sync = 0.12f;
    }
    if (has(lower, "rave") || has(lower, "elec") || has(lower, "hoover")) {
        p.density = 0.5f; p.leap = 0.24f; p.repeat = 0.35f; p.accent = 0.3f; p.slide = 0.06f; p.octave = 0.2f; p.offbeat = 0.22f;
    }
    if (has(lower, "dub")) { p.density = 0.34f; p.repeat = 0.7f; p.restBias = 0.3f; p.stableEnd = 0.78f; p.sustain = 0.22f; }
    if (has(lower, "industrial") || has(lower, "warehouse")) {
        p.density = 0.46f; p.leap = 0.18f; p.repeat = 0.56f; p.accent = 0.34f; p.rootBias = 0.4f; p.choppy = 0.28f;
    }
    if (has(lower, "rooted")) { p.rootBias = 0.5f; p.repeat = 0.7f; p.stableEnd = 0.8f; }
    auto set = [](float& f, uint8_t v) {
        if (v != U) f = v / 100.0f;
    };
    set(p.density, sp.density); set(p.repeat, sp.repeat); set(p.slide, sp.slide); set(p.accent, sp.accent);
    set(p.rootBias, sp.rootBias); set(p.sync, sp.syncBias); set(p.choppy, sp.choppy); set(p.sustain, sp.sustain);
    set(p.restBias, sp.rest); set(p.motifVar, sp.motifVar); set(p.tight, sp.tight); set(p.leap, sp.leap);
    set(p.offbeat, sp.offbeat); set(p.octave, sp.octave); set(p.stableEnd, sp.stableEnd);
    return p;
}

struct CStep {
    bool active = false;
    int degree = 0;
    float gate = 0.82f;
    bool accent = false, slide = false, tie = false;
};

struct Ctx {
    Rng* rng;
    Profile sp;
    int scaleLen;
    int steps;
    float dens;
    int pool[8];
    int poolN;
};

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
float fmaxf2(float a, float b) { return a > b ? a : b; }
float fminf2(float a, float b) { return a < b ? a : b; }

int pickFrom(Ctx& c, const int* a, int n) { return a[c.rng->below(n)]; }
// rand([..]) over a literal list, dropping entries >= scaleLen when `inScale`.
int pickScaled(Ctx& c, std::initializer_list<int> l, bool inScale = true) {
    int buf[8], n = 0;
    for (int x : l)
        if (!inScale || x < c.scaleLen) buf[n++] = x;
    if (!n) return 0;
    return buf[c.rng->below(n)];
}
int pick(Ctx& c, std::initializer_list<int> l) { return pickScaled(c, l, false); }

float stepAct(uint8_t cat, int idx) {
    static const float kA[5][4] = {{0.82f, 0.35f, 0.72f, 0.28f}, {0.86f, 0.18f, 0.58f, 0.22f},
                                   {0.72f, 0.48f, 0.62f, 0.34f}, {0.78f, 0.42f, 0.68f, 0.38f},
                                   {0.7f, 0.35f, 0.6f, 0.3f}};
    return kA[cat][idx % 4];
}

void articulation(Ctx& c, int idx, bool active, int motifPos, CStep& s) {
    Rng& r = *c.rng;
    const Profile& sp = c.sp;
    static const float kMixed[5] = {0.55f, 0.7f, 0.85f, 1.0f, 1.15f};
    float gate = kMixed[idx % 5];
    bool accent = false, slide = false, tie = false;
    if (sp.cat == kCatAcid) {
        gate = fmaxf2(gate, 0.72f + (motifPos == 0 ? 0.12f : 0));
        if (idx % 4 == 3 && r.coin(sp.sustain)) gate = fmaxf2(gate, 1.0f);
    } else if (sp.cat == kCatTechno) {
        gate = fminf2(gate, 0.78f);
        if (idx % 4 == 0) gate = fminf2(0.68f, gate);
    } else if (sp.cat == kCatHouse) {
        if (idx % 2 == 1 && r.coin(sp.offbeat)) gate = fmaxf2(gate, 0.9f);
        else gate = fminf2(fmaxf2(gate, 0.62f), 0.95f);
    }
    gate = fmaxf2(gate, 1.0f);   // connection "Legato"
    // articulation "Acidic"
    accent = r.coin(sp.accent);
    slide = r.coin(sp.slide);
    gate = fmaxf2(gate, 0.86f);
    slide = r.coin(0.16f);                       // slides "Occasional"
    accent = motifPos == 0 || r.coin(0.08f);     // "Motif accents"
    if (sp.cat == kCatAcid && active) {
        if (idx % 4 == 0) accent = accent || r.coin(0.45f);
        if (idx % 4 == 2) slide = slide || r.coin(0.2f);
        tie = tie || r.coin(0.08f);
    } else if (sp.cat == kCatTechno && active) {
        accent = accent || (idx % 4 == 0 && r.coin(0.35f));
        gate = fminf2(gate, r.coin(sp.choppy) ? 0.58f : gate);
        slide = slide && r.coin(0.5f);
    } else if (sp.cat == kCatHouse && active) {
        accent = accent || (idx % 2 == 1 && r.coin(0.28f));
        if (idx % 2 == 1 && r.coin(sp.offbeat)) gate = fmaxf2(gate, 0.95f);
    }
    if (!active) accent = slide = tie = false;
    s.gate = gate;
    s.accent = accent;
    s.slide = slide;
    s.tie = tie;
}

void postProcess(Ctx& c, CStep* ph, int n) {
    Rng& r = *c.rng;
    if (c.sp.cat == kCatAcid) {
        for (int i = 0; i < n; ++i) {
            CStep& s = ph[i];
            if (!s.active) continue;
            if ((i % 4 == 0) && r.coin(0.55f)) s.accent = true;
            if (s.degree == 0 && r.coin(0.45f)) s.accent = true;
            if (i < n - 1 && ph[i + 1].active) {
                int diff = ph[i + 1].degree - s.degree;
                if (diff < 0) diff = -diff;
                if (diff >= 1 && diff <= 3 && r.coin(0.28f)) s.slide = true;
                if (diff <= 2 && r.coin(0.16f)) {
                    s.tie = true;
                    s.gate = fmaxf2(s.gate, 1.05f);
                }
            }
            if (!s.slide && !s.tie && r.coin(0.35f)) s.gate = fminf2(s.gate, 0.82f);
        }
        int slides = 0;
        for (int i = 0; i < n; ++i) slides += ph[i].slide;
        const int maxSlides = (n + 2) / 3;
        for (int i = n - 1; i >= 0 && slides > maxSlides; --i)
            if (ph[i].slide) {
                ph[i].slide = false;
                --slides;
            }
    } else if (c.sp.cat == kCatTechno) {
        for (int i = 0; i < n; ++i)
            if (ph[i].active) {
                ph[i].slide = false;
                if (i % 4 == 0 && r.coin(0.42f)) ph[i].accent = true;
                if (r.coin(0.3f)) ph[i].gate = fminf2(ph[i].gate, 0.62f);
            }
    } else if (c.sp.cat == kCatHouse) {
        for (int i = 0; i < n; ++i)
            if (ph[i].active) {
                if (i % 2 == 1 && r.coin(0.36f)) ph[i].accent = true;
                if (i % 2 == 1 && r.coin(0.4f)) ph[i].gate = fmaxf2(ph[i].gate, 0.92f);
                if (r.coin(0.1f)) ph[i].tie = true;
            }
    }
}

void applyEnding(Ctx& c, CStep* ph, int n, bool response) {
    const int last = n - 1;
    if (last < 0) return;
    if (!ph[last].active) {
        ph[last].active = true;
        ph[last].degree = 0;
    }
    if (response) ph[last].degree = pickScaled(c, {0, 0, 2, 4});   // "more resolved than call"
}

void buildCall(Ctx& c, CStep* out) {
    Rng& r = *c.rng;
    const Profile& sp = c.sp;
    const int steps = c.steps;
    // Motif template "Acid nibble": rhythm chance 0.65, contour [0,0,1,0,-1].
    const int mlen = steps <= 4 ? 2 : (steps <= 8 ? r.range(2, 4) : r.range(3, 4));
    bool rhythm[5];
    for (int m = 0; m < mlen; ++m) rhythm[m] = r.coin(0.65f);
    bool anyR = false;
    for (int m = 0; m < mlen; ++m) anyR = anyR || rhythm[m];
    if (!anyR) rhythm[0] = true;
    static const int kContour[5] = {0, 0, 1, 0, -1};
    const float rep = 0.72f;   // "Repetitive"
    const int center = r.range(0, 2);   // medium register
    CStep motif[5];
    int prev = center + pick(c, {0, 0, 1, -1});
    for (int m = 0; m < mlen; ++m) {
        bool active = rhythm[m];
        if (r.coin(sp.restBias + (1 - c.dens) * 0.25f)) active = active && r.coin(0.68f);
        int deg = prev;
        if (m == 0 && r.coin(sp.rootBias)) deg = pickScaled(c, {0, 0, 1, 2});
        else if (m > 0) deg = prev + pick(c, {-1, 0, 1});   // contour "Stepwise"
        (void)kContour;
        if (r.coin(sp.leap)) deg += pick(c, {-3, 3});
        if (sp.cat == kCatAcid && r.coin(sp.sync)) deg += pick(c, {-1, 0, 1});
        if (sp.cat == kCatHouse && r.coin(sp.offbeat) && m % 2 == 1) deg += pick(c, {0, 1});
        if (sp.cat == kCatTechno && r.coin(sp.rootBias * 0.4f)) deg = pickScaled(c, {0, 0, 2, 4});
        deg = clampi(deg, 0, 8);
        prev = deg;
        motif[m].active = active;
        motif[m].degree = deg;
        articulation(c, m, active, m, motif[m]);
    }
    bool anyM = false;
    for (int m = 0; m < mlen; ++m) anyM = anyM || motif[m].active;
    if (!anyM) {
        motif[0].active = true;
        motif[0].degree = 0;
    }
    for (int i = 0; i < steps; ++i) {
        const int m = i % mlen, block = i / mlen;
        CStep s = motif[m];
        if (block > 0) {
            if (!r.coin(rep)) {
                if (s.active) s.degree = clampi(s.degree + pick(c, {-2, -1, 0, 1, 2}), 0, 8);
                if (r.coin(sp.motifVar + (1 - rep) * 0.25f)) s.active = !s.active ? r.coin(0.38f) : s.active;
            }
            if (i >= (steps * 3) / 4 && s.active && r.coin(sp.stableEnd)) s.degree = clampi(s.degree + pick(c, {-1, 0, 1}), 0, 10);
        }
        float dens = c.dens * 0.6f + stepAct(sp.cat, i) * 0.4f;
        dens = dens < 0.05f ? 0.05f : (dens > 0.98f ? 0.98f : dens);
        if (r.coin(1 - dens)) s.active = s.active && r.coin(dens);
        if (s.active && r.coin(sp.rootBias * 0.4f) && i % (mlen > 2 ? mlen : 2) == 0) s.degree = pickScaled(c, {0, 0, 2, 4});
        if (sp.cat == kCatHouse && s.active && i % 2 == 1 && r.coin(sp.offbeat)) s.gate = fmaxf2(s.gate, 0.95f);
        if (sp.cat == kCatTechno && s.active && i % 4 == 0 && r.coin(0.4f)) s.accent = true;
        out[i] = s;
    }
    const int cloneLen = mlen < steps / 2 ? mlen : steps / 2;
    for (int i = cloneLen; i < steps && cloneLen > 0; ++i)
        if (r.coin(0.72f)) {
            CStep src = out[i % cloneLen];
            articulation(c, i, src.active, i % mlen, src);
            src.active = out[i % cloneLen].active;
            src.degree = out[i % cloneLen].degree;
            out[i] = src;
        }
    applyEnding(c, out, steps, false);
    postProcess(c, out, steps);
}

int contourOf(const CStep* ph, int n) {
    int first = -1, last = 0;
    for (int i = 0; i < n; ++i)
        if (ph[i].active) {
            if (first < 0) first = ph[i].degree;
            last = ph[i].degree;
        }
    return first < 0 ? 0 : last - first;
}

void buildResponse(Ctx& c, const CStep* call, int strategy, CStep* out) {
    Rng& r = *c.rng;
    const Profile& sp = c.sp;
    const int steps = c.steps;
    for (int i = 0; i < steps; ++i) out[i] = call[i];
    int motifLen = steps / 2;
    motifLen = motifLen < 2 ? 2 : (motifLen > 4 ? 4 : motifLen);
    float avg = 0;
    int cnt = 0;
    for (int i = 0; i < steps; ++i)
        if (call[i].active) {
            avg += float(call[i].degree);
            ++cnt;
        }
    avg = cnt ? avg / float(cnt) : 0;
    const int callContour = contourOf(call, steps);
    auto stable = [&]() { return pickScaled(c, {0, 2, 4, 6}); };
    // Degree transforms of the answer strategies (one non-template mapMotif
    // keeps the code small on the device).
    enum Xf { kJit, kJit4, kUp123, kDn112, kInv, kUp234, kDn223, kDn12, kP3, kM1, kP1 };
    auto xform = [&](int xf, int d) {
        switch (xf) {
            case kJit4: return d + pick(c, {-1, 0, 0, 1});
            case kUp123: return d + pick(c, {1, 2, 3});
            case kDn112: return d - pick(c, {1, 1, 2});
            case kInv: return int(avg - (float(d) - avg) + 0.5f) + pick(c, {-1, 0, 1});
            case kUp234: return d + pick(c, {2, 3, 4});
            case kDn223: return d - pick(c, {2, 2, 3});
            case kDn12: return d - pick(c, {1, 2});
            case kP3: return d + 3;
            case kM1: return d - 1;
            case kP1: return d + 1;
            default: return d + pick(c, {-1, 0, 1});
        }
    };
    auto mapMotif = [&](int xf) {
        for (int i = 0; i < steps; ++i) {
            const CStep& base = call[i % motifLen];
            out[i] = base;
            if (out[i].active) {
                int d = xform(xf, base.degree);
                if (r.coin(sp.tight * 0.28f) && c.poolN) {
                    const int sl = c.scaleLen > 0 ? c.scaleLen : 1;
                    d = pickFrom(c, c.pool, c.poolN) + (d / sl) * sl;
                }
                out[i].degree = clampi(d, 0, 10);
            }
        }
    };
    const int jitter = kJit;
    switch (strategy) {
        case 0:
            for (int i = 0; i < steps; ++i) {
                out[i].active = call[i].active;
                out[i].degree = clampi(call[i].degree + pick(c, {-1, 0, 0, 1}), 0, 10);
            }
            break;
        case 1: {
            const int dir = callContour >= 0 ? -1 : 1;
            int q = steps / 4;
            if (q < 2) q = 2;
            for (int i = 0; i < steps; ++i) {
                out[i].active = call[i].active;
                out[i].degree = clampi(call[i].degree + dir * (i / q) + pick(c, {-1, 0, 1}), 0, 10);
            }
            break;
        }
        case 2:
            mapMotif(kJit4);
            for (int i = 0; i < steps; ++i) out[i].active = out[i].active && r.coin(i % 2 == 0 ? 1.0f : 0.35f);
            break;
        case 3:
            mapMotif(jitter);
            for (int i = 1; i < steps; ++i)
                if (!out[i].active && out[i - 1].active && r.coin(0.42f)) {
                    out[i].active = true;
                    out[i].degree = out[i - 1].degree;
                    out[i].tie = true;
                    out[i].gate = fmaxf2(out[i].gate, 1.0f);
                }
            break;
        case 4:
            mapMotif(jitter);
            for (int i = (steps * 65) / 100; i < steps; ++i)
                if (r.coin(0.65f)) out[i].active = false;
            break;
        case 5:
        case 28:
            for (int i = 0; i < steps; ++i) {
                const CStep& src = call[(i - 2 + steps * 2) % steps];
                out[i].active = src.active;
                out[i].degree = clampi(src.degree + pick(c, {-1, 0, 1}), 0, 10);
            }
            if (strategy == 28) {
                for (int i = steps - 1; i > 0; --i) out[i] = out[i - 1];
                out[0] = CStep{};
            }
            break;
        case 6:
        case 8: mapMotif(kUp123); break;
        case 7:
        case 9: mapMotif(kDn112); break;
        case 10:
        case 11: mapMotif(kInv); break;
        case 12:
            for (int i = 0; i < steps; ++i) {
                out[i].active = call[i].active;
                out[i].degree = clampi(pickFrom(c, c.pool, c.poolN) + pick(c, {0, 0, 2}), 0, 10);
            }
            break;
        case 13:
            for (int i = 0; i < steps; ++i) {
                out[i].degree = call[i].degree;
                out[i].active = r.coin(call[i].active ? 0.68f : 0.22f);
            }
            break;
        case 14: mapMotif(kUp234); break;
        case 15: mapMotif(kDn223); break;
        case 16:
            mapMotif(jitter);
            for (int i = steps > 3 ? steps - 3 : 0; i < steps; ++i)
                if (out[i].active) out[i].degree = pickScaled(c, {0, 0, 2});
            break;
        case 17:
            mapMotif(jitter);
            for (int i = steps > 3 ? steps - 3 : 0; i < steps; ++i)
                if (out[i].active) out[i].degree = pickScaled(c, {4, 2, 0});
            break;
        case 18:
            mapMotif(kDn12);
            for (int i = steps > 2 ? steps - 2 : 0; i < steps; ++i)
                if (out[i].active) out[i].degree = clampi(out[i].degree - 1, 0, 10);
            break;
        case 19:
            mapMotif(jitter);
            for (int i = steps > 4 ? steps - 4 : 0; i < steps; ++i)
                if (out[i].active) {
                    out[i].degree = stable();
                    out[i].accent = true;
                }
            break;
        case 20:
            mapMotif(jitter);
            for (int i = 0; i < steps - 2; ++i)
                if (r.coin(0.72f)) out[i] = call[i];
            if (steps >= 2 && out[steps - 2].active) out[steps - 2].degree = clampi(out[steps - 2].degree + pick(c, {2, 3, -2}), 0, 10);
            if (out[steps - 1].active) out[steps - 1].degree = stable();
            break;
        case 21:
            mapMotif(jitter);
            for (int i = 0; i < steps; ++i)
                if (out[i].active) {
                    out[i].gate = 0.48f + (i % 2 == 0 ? 0.04f : 0);
                    out[i].accent = r.coin(0.3f);
                }
            break;
        case 22:
            mapMotif(jitter);
            for (int i = 0; i < steps; ++i) out[i].active = out[i].active && r.coin(i % 2 == 0 ? 0.85f : 0.22f);
            break;
        case 23:
            mapMotif(jitter);
            for (int i = 0; i < steps; ++i)
                if (!out[i].active && r.coin(0.3f)) {
                    out[i].active = true;
                    out[i].degree = i > 0 && out[i - 1].active ? out[i - 1].degree : pickFrom(c, c.pool, c.poolN);
                }
            break;
        case 24:
            mapMotif(jitter);
            for (int i = 0; i < steps; ++i) out[i].accent = out[i].active && (i % 2 == 0 || r.coin(0.22f));
            break;
        case 25:
            mapMotif(jitter);
            for (int i = 0; i < steps; ++i) {
                out[i].slide = out[i].active && r.coin(0.34f);
                out[i].gate = fmaxf2(out[i].gate, 1.0f);
            }
            break;
        case 26:
            mapMotif(jitter);
            for (int i = 0; i < steps; ++i) {
                out[i].gate = 0.42f;
                out[i].tie = false;
            }
            break;
        case 27:
            mapMotif(jitter);
            for (int i = 0; i < steps; ++i) {
                out[i].gate = 1.08f;
                out[i].tie = out[i].active && r.coin(0.18f);
            }
            break;
        case 29:
            mapMotif(jitter);
            for (int i = 0; i < steps / 2; ++i) out[i].active = false;
            break;
        case 30:
        case 31: {
            const int start = strategy == 31 ? steps / 2 : steps / 3;
            for (int i = 0; i < steps; ++i) {
                const CStep& src = call[(start + i) % steps];
                out[i].active = src.active && r.coin(0.88f);
                out[i].degree = clampi(src.degree + pick(c, {-1, 0, 1}), 0, 10);
            }
            break;
        }
        case 32: mapMotif(kP3); break;
        case 33: mapMotif(kM1); break;
        case 34: mapMotif(kP1); break;
        case 35:
            mapMotif(jitter);
            if (steps >= 2 && out[steps - 2].active) out[steps - 2].degree = pickScaled(c, {1, 3, 5});
            if (out[steps - 1].active) out[steps - 1].degree = stable();
            break;
        case 36:
            for (int i = 0; i < steps; ++i) {
                if (i < steps / 2) {
                    out[i] = call[i];
                } else {
                    out[i] = call[i % motifLen];
                    if (out[i].active) out[i].degree = clampi(out[i].degree + pick(c, {-2, -1, 1, 2}), 0, 10);
                }
            }
            break;
        case 37:
            mapMotif(jitter);
            for (int i = 0; i < steps; ++i) {
                if (i < steps / 2 && out[i].active) out[i].degree = pickScaled(c, {4, 1, 5});
                if (i >= steps / 2 && out[i].active && r.coin(0.55f)) out[i].degree = stable();
            }
            break;
        default: mapMotif(jitter); break;
    }
    for (int i = 0; i < steps; ++i) {
        if (out[i].active && call[i].active && r.coin(sp.tight))
            out[i].degree = clampi((out[i].degree + call[i].degree + 1) / 2, 0, 10);
        const float keepGate = out[i].gate;
        articulation(c, i, out[i].active, i % motifLen, out[i]);
        out[i].gate = keepGate;
    }
    applyEnding(c, out, steps, true);
    postProcess(c, out, steps);
}

// Note -> nearest scale degree (for "Answer mine": the call is read from the track).
int noteToDegree(int note, int root, const uint8_t* deg, int n) {
    int best = 0, bestD = 1000;
    for (int d = -n; d < 4 * n; ++d) {
        int x = degreeNote(root, deg, n, d) - note;
        if (x < 0) x = -x;
        if (x < bestD) {
            bestD = x;
            best = d;
        }
    }
    return best < 0 ? 0 : best;
}

void toMono(const CStep* ph, int n, int root, const uint8_t* deg, int nd, MonoStep* out) {
    for (int i = 0; i < n; ++i) {
        out[i].active = ph[i].active;
        out[i].note = degreeNote(root, deg, nd, ph[i].degree);
        out[i].accent = ph[i].accent;
        out[i].slide = ph[i].slide && i + 1 < n && ph[i + 1].active;
        out[i].gate = ph[i].tie ? 1.1f : ph[i].gate;
    }
}

}  // namespace

const char* crStyleName(int i) { return kStyles[clampi(i, 0, kCrStyleCount - 1)].name; }
const char* crStrategyName(int i) { return kStrategies[clampi(i, 0, kCrStrategyCount - 1)]; }

void genCallResponse(const int16_t* v, Target& t, Rng& rng, Result& r) {
    const int style = v[0], strategy = v[1], layout = v[3];
    uint8_t deg[12];
    const int nd = scaleDegrees(t.scaleMask, deg);
    const int total = t.s1 - t.s0 + 1;
    const bool twoTracks = layout == 2;
    int steps = twoTracks ? total : total / 2;
    if (steps > 64) steps = 64;
    if (steps < 2) {
        std::snprintf(r.msg, sizeof(r.msg), "Select at least 4 steps");
        return;
    }
    Ctx c;
    c.rng = &rng;
    c.sp = profile(style);
    c.scaleLen = nd;
    c.steps = steps;
    c.dens = v[2] > 0 ? v[2] / 100.0f : 0.52f;
    // Note pool "Pool with spice notes": 4 of the first degrees + the middle one.
    {
        int base[7], nb = nd < 7 ? nd : 7;
        for (int i = 0; i < nb; ++i) base[i] = i;
        for (int i = nb - 1; i > 0; --i) {
            const int j = rng.below(i + 1), x = base[i];
            base[i] = base[j];
            base[j] = x;
        }
        c.poolN = 0;
        for (int i = 0; i < (nb < 4 ? nb : 4); ++i) c.pool[c.poolN++] = base[i];
        const int spice = nd / 2 < nd - 1 ? nd / 2 : nd - 1;
        bool dup = false;
        for (int i = 0; i < c.poolN; ++i) dup = dup || c.pool[i] == spice;
        if (!dup) c.pool[c.poolN++] = spice;
    }
    CStep call[64], resp[64];
    Pattern& p = *t.pat;
    const int tr = t.t0;
    if (layout == 1) {
        // "Answer mine": the call is what the first half of the track already plays.
        for (int i = 0; i < steps; ++i) {
            const Step& s = p.tracks[tr].steps[t.s0 + i];
            call[i] = CStep{};
            if (s.note >= 0) {
                call[i].active = true;
                call[i].degree = noteToDegree(s.note, t.rootNote, deg, nd);
                call[i].gate = 1.0f;
            }
        }
    } else {
        buildCall(c, call);
    }
    buildResponse(c, call, strategy, resp);
    MonoStep m[64];
    if (layout != 1) {
        toMono(call, steps, t.rootNote, deg, nd, m);
        writeMono(p, tr, t.s0, m, steps, t.instrument);
    }
    toMono(resp, steps, t.rootNote, deg, nd, m);
    if (twoTracks) {
        const int tr2 = t.t1 > t.t0 ? t.t1 : (tr + 1 < kTrackCount ? tr + 1 : tr);
        writeMono(p, tr2, t.s0, m, steps, t.instrument);
        std::snprintf(r.msg, sizeof(r.msg), "Call T%d, answer T%d", tr + 1, tr2 + 1);
    } else {
        writeMono(p, tr, t.s0 + steps, m, steps, t.instrument);
        std::snprintf(r.msg, sizeof(r.msg), "%s: %d + %d steps", crStrategyName(strategy), steps, steps);
    }
    r.ok = true;
}

}  // namespace gen
}  // namespace ot
