// Pattern generators (src/core/gen): determinism, ranges, FX use, edge cases.
#include "test.h"

#include "gen/gen.h"
#include "gen/gen_internal.h"
#include "model/fx.h"
#include "sequencer/seq_tables.h"

using namespace ot;

namespace {

Pattern g_pat;

gen::Target target(int s0, int s1, int t0 = 0, int t1 = 0) {
    g_pat.setDefault();
    for (auto& tr : g_pat.tracks) tr.lastStep = 63;
    gen::Target t;
    t.pat = &g_pat;
    t.s0 = s0;
    t.s1 = s1;
    t.t0 = t0;
    t.t1 = t1;
    t.instrument = 64;   // Synth 1
    t.rootNote = 36;
    t.scaleMask = 0x0AD;  // 0, 2, 3, 5, 7
    t.entropy = 12345;
    return t;
}

uint16_t g_slices[16];
gen::Target slicedTarget(int s0, int s1) {
    gen::Target t = target(s0, s1);
    for (int i = 0; i < 16; ++i) g_slices[i] = uint16_t(i * 4096);
    t.slices.count = 16;
    t.slices.start = g_slices;
    t.slices.beatSlice = true;
    t.slices.sampleBeats = 4.0f;
    t.instrument = 3;
    return t;
}

void defaults(int k, int16_t* v) {
    for (int i = 0; i < gen::kParams; ++i) v[i] = gen::param(k, i, v).def;
    // BBCut A / B depend on the procedure (v[0]): read again after it is set.
    for (int i = 0; i < gen::kParams; ++i) v[i] = gen::param(k, i, v).def;
}

// Every FX type and value on a step is valid for the sequencer.
bool stepValid(const Step& s) {
    if (s.note < 0 && s.note != kNoteEmpty && s.note != kNoteOff) return false;
    for (const StepFx& f : s.fx) {
        if (f.type == 0) continue;
        if (!fxIdValid(f.type)) return false;
        if (f.value < seq::fxMin(f.type) || f.value > seq::fxMax(f.type)) return false;
    }
    return true;
}

bool patternValid() {
    for (const Track& tr : g_pat.tracks)
        for (const Step& s : tr.steps)
            if (!stepValid(s)) return false;
    return true;
}

int notesIn(int t, int s0, int s1) {
    int n = 0;
    for (int s = s0; s <= s1; ++s) n += g_pat.tracks[t].steps[s].note >= 0;
    return n;
}

}  // namespace

TEST(table_complete) {
    for (int k = 0; k < gen::kKindCount; ++k) {
        const gen::KindDesc& d = gen::kind(k);
        CHECK(d.name != nullptr);
        CHECK(d.p[gen::kParams - 1].name != nullptr);   // Seed is always last
        int16_t v[gen::kParams] = {};
        defaults(k, v);
        for (int i = 0; i < gen::kParams; ++i) {
            const gen::ParamDesc& p = gen::param(k, i, v);
            if (!p.name) continue;
            CHECK(p.min <= p.def && p.def <= p.max);
        }
    }
    CHECK_EQ(gen::kind(gen::kCallResponse).p[0].labels != nullptr, true);
}

TEST(acid_deterministic_and_valid) {
    int16_t v[gen::kParams];
    defaults(gen::kAcid, v);
    v[5] = 42;
    gen::Target t = target(0, 15);
    gen::Result r = gen::generate(gen::kAcid, v, t);
    CHECK(r.ok);
    CHECK(patternValid());
    Pattern first = g_pat;
    t = target(0, 15);
    gen::generate(gen::kAcid, v, t);
    bool same = true;
    for (int s = 0; s < 16; ++s) same = same && first.tracks[0].steps[s] == g_pat.tracks[0].steps[s];
    CHECK(same);
    CHECK(notesIn(0, 0, 15) >= 1);
    CHECK_EQ(notesIn(0, 16, 63), 0);   // outside the range untouched
    // Notes are in the scale.
    for (int s = 0; s < 16; ++s) {
        const Step& st = g_pat.tracks[0].steps[s];
        if (st.note < 0) continue;
        const int pc = ((st.note - 36) % 12 + 12) % 12;
        CHECK((0x0AD >> pc) & 1);
        CHECK_EQ(int(st.instrument), 64);
    }
}

TEST(acid_density_extremes) {
    int16_t v[gen::kParams];
    defaults(gen::kAcid, v);
    v[0] = 0;   // density 0: still one note
    v[5] = 7;
    gen::Target t = target(0, 31);
    gen::generate(gen::kAcid, v, t);
    CHECK_EQ(notesIn(0, 0, 31), 1);
    v[0] = 100;
    v[2] = 100;   // slide everything
    t = target(0, 31);
    gen::generate(gen::kAcid, v, t);
    CHECK_EQ(notesIn(0, 0, 31), 32);
    int glides = 0;
    for (int s = 0; s < 32; ++s)
        for (const StepFx& f : g_pat.tracks[0].steps[s].fx) glides += f.type == gen::kFxGlide;
    CHECK(glides >= 30);
    CHECK(patternValid());
}

TEST(callresponse_all_strategies) {
    int16_t v[gen::kParams];
    defaults(gen::kCallResponse, v);
    for (int style = 0; style < gen::kCrStyleCount; style += 5)
        for (int st = 0; st < gen::kCrStrategyCount; ++st) {
            v[0] = int16_t(style);
            v[1] = int16_t(st);
            v[5] = int16_t(1 + st);
            gen::Target t = target(0, 31);
            gen::Result r = gen::generate(gen::kCallResponse, v, t);
            CHECK(r.ok);
            CHECK(patternValid());
            CHECK(notesIn(0, 0, 15) >= 1);
        }
}

TEST(callresponse_layouts) {
    int16_t v[gen::kParams];
    defaults(gen::kCallResponse, v);
    v[5] = 9;
    // Two tracks: the answer goes to the next track.
    v[3] = 2;
    gen::Target t = target(0, 15, 2, 2);
    gen::generate(gen::kCallResponse, v, t);
    CHECK(notesIn(2, 0, 15) >= 1);
    CHECK(notesIn(3, 0, 15) >= 1);
    // Answer mine: the first half is left as written.
    v[3] = 1;
    t = target(0, 15);
    for (int s = 0; s < 8; s += 2) {
        g_pat.tracks[0].steps[s].note = int8_t(36 + s);
        g_pat.tracks[0].steps[s].instrument = 64;
    }
    gen::generate(gen::kCallResponse, v, t);
    CHECK_EQ(int(g_pat.tracks[0].steps[0].note), 36);
    CHECK_EQ(int(g_pat.tracks[0].steps[2].note), 38);
    CHECK(notesIn(0, 8, 15) >= 1);
    // Too short a selection.
    v[3] = 0;
    t = target(0, 2);
    CHECK(!gen::generate(gen::kCallResponse, v, t).ok);
}

TEST(bbcut_every_proc) {
    int16_t v[gen::kParams];
    for (int proc = 0; proc < gen::kBBProcCount; ++proc) {
        v[0] = int16_t(proc);
        defaults(gen::kBBCut, v);
        v[0] = int16_t(proc);
        for (int i = 1; i < 3; ++i) v[i] = gen::param(gen::kBBCut, i, v).def;
        v[5] = int16_t(100 + proc);
        gen::Target t = slicedTarget(0, 63);
        gen::Result r = gen::generate(gen::kBBCut, v, t);
        CHECK(r.ok);
        CHECK(patternValid());
        CHECK(notesIn(0, 0, 63) >= 4);
        // Beat Slice hits: notes 24..39 (16 slices).
        for (int s = 0; s < 64; ++s) {
            const int n = g_pat.tracks[0].steps[s].note;
            if (n >= 0) CHECK(n >= 24 && n < 40);
        }
    }
}

TEST(bbcut_slice_mode_and_errors) {
    int16_t v[gen::kParams] = {};
    defaults(gen::kBBCut, v);
    v[5] = 5;
    gen::Target t = slicedTarget(0, 31);
    t.slices.beatSlice = false;   // Slice mode: unity note + Slice FX
    CHECK(gen::generate(gen::kBBCut, v, t).ok);
    int sliceFx = 0;
    for (int s = 0; s < 32; ++s)
        for (const StepFx& f : g_pat.tracks[0].steps[s].fx) sliceFx += f.type == gen::kFxSliceId;
    CHECK(sliceFx >= 4);
    CHECK(patternValid());
    t = target(0, 31);   // no slices
    gen::Result r = gen::generate(gen::kBBCut, v, t);
    CHECK(!r.ok);
    CHECK(std::strstr(r.msg, "slice") != nullptr);
}

TEST(slice_for_offset) {
    uint16_t st[4] = {0, 16384, 32768, 49152};
    gen::Slices s;
    s.count = 4;
    s.start = st;
    CHECK_EQ(gen::sliceForOffset(s, 0.0f), 0);
    CHECK_EQ(gen::sliceForOffset(s, 0.26f), 1);
    CHECK_EQ(gen::sliceForOffset(s, 0.5f), 2);
    CHECK_EQ(gen::sliceForOffset(s, 0.99f), 3);
    CHECK_EQ(gen::sliceForOffset(s, 1.0f), 0);   // wraps
}

TEST(roll_value) {
    CHECK_EQ(int(gen::rollValue(1)), 1);
    CHECK_EQ(int(gen::rollValue(4)), 4);
    CHECK_EQ(int(gen::rollValue(16)), 8);
    CHECK_EQ(int(gen::rollValue(5)), 5);   // nearest in log: 6 per step (vs 4)
    CHECK_EQ(int(gen::rollValue(2, 1)), 11);
}

TEST(turnaround_styles) {
    int16_t v[gen::kParams];
    defaults(gen::kTurnaround, v);
    for (int style = 0; style < 6; ++style) {
        gen::Target t = target(0, 63);
        for (int s = 0; s < 64; s += 2) {
            g_pat.tracks[0].steps[s].note = 48;
            g_pat.tracks[0].steps[s].instrument = 1;
        }
        v[2] = int16_t(style);
        v[5] = int16_t(3 + style);
        gen::Result r = gen::generate(gen::kTurnaround, v, t);
        CHECK(r.ok);
        CHECK(patternValid());
    }
    // Drop: the bar end (step 31 with 2-bar phrase, budget 4) is silenced.
    gen::Target t = target(0, 63);
    for (int s = 0; s < 64; ++s) g_pat.tracks[0].steps[s].note = 48;
    v[0] = 1;
    v[1] = 4;
    v[2] = 2;
    v[3] = 100;
    v[5] = 11;
    gen::generate(gen::kTurnaround, v, t);
    CHECK_EQ(int(g_pat.tracks[0].steps[28].note), int(kNoteOff));
    CHECK_EQ(int(g_pat.tracks[0].steps[60].note), int(kNoteOff));
    CHECK_EQ(int(g_pat.tracks[0].steps[10].note), 48);
}

TEST(genotype_identity_and_vocabulary) {
    int16_t v[gen::kParams];
    defaults(gen::kGenotype, v);
    gen::Target t = target(0, 15);
    for (int s = 0; s < 16; ++s) {
        g_pat.tracks[0].steps[s].note = int8_t(s % 4 == 0 ? 36 : (s % 4 == 2 ? 43 : kNoteEmpty));
        g_pat.tracks[0].steps[s].instrument = s % 4 == 1 || s % 4 == 3 ? 0 : 5;
    }
    Pattern orig = g_pat;
    v[1] = 100;   // identity 100: unchanged
    v[5] = 3;
    gen::generate(gen::kGenotype, v, t);
    bool same = true;
    for (int s = 0; s < 16; ++s) same = same && orig.tracks[0].steps[s] == g_pat.tracks[0].steps[s];
    CHECK(same);
    for (int mode = 0; mode < 3; ++mode) {
        g_pat = orig;
        v[0] = int16_t(mode);
        v[1] = 0;
        v[3] = 80;
        v[5] = int16_t(20 + mode);
        gen::generate(gen::kGenotype, v, t);
        // Every output step is one of the learnt steps.
        for (int s = 0; s < 16; ++s) {
            bool known = false;
            for (int k = 0; k < 16; ++k) known = known || g_pat.tracks[0].steps[s] == orig.tracks[0].steps[k];
            CHECK(known);
        }
    }
}

TEST(scale_helpers) {
    uint8_t d[12];
    CHECK_EQ(gen::scaleDegrees(0, d), 7);   // empty -> minor
    CHECK_EQ(gen::scaleDegrees(0xFFF, d), 12);
    const int n = gen::scaleDegrees(0x0AD, d);
    CHECK_EQ(n, 5);
    CHECK_EQ(gen::degreeNote(36, d, n, 0), 36);
    CHECK_EQ(gen::degreeNote(36, d, n, 5), 48);
    CHECK_EQ(gen::degreeNote(36, d, n, -1), 31);
}



TEST(acid_mutate_changes_some_steps) {
    int16_t v[gen::kParams];
    defaults(gen::kAcid, v);
    v[5] = 17;
    gen::Target t = target(0, 31);
    gen::Rng rng(99);
    gen::MonoStep a[32], b[32];
    gen::acidPhrase(v, t, rng, a, 32);
    for (int i = 0; i < 32; ++i) b[i] = a[i];
    gen::acidMutate(v, t, rng, b, 32, 0.0f);
    int diff = 0;
    for (int i = 0; i < 32; ++i) diff += a[i].active != b[i].active || a[i].note != b[i].note;
    CHECK_EQ(diff, 0);   // amount 0: unchanged (Lock)
    gen::acidMutate(v, t, rng, b, 32, 0.5f);
    diff = 0;
    for (int i = 0; i < 32; ++i) diff += a[i].active != b[i].active || a[i].note != b[i].note;
    CHECK(diff > 0 && diff < 32);
    // The generate's phrase is rebuilt from Result::rngSeed.
    t = target(0, 31);
    gen::Result r = gen::generate(gen::kAcid, v, t);
    gen::Rng again(r.rngSeed);
    gen::acidPhrase(v, t, again, b, 32);
    bool same = true;
    for (int i = 0; i < 32; ++i)
        same = same && (b[i].active ? g_pat.tracks[0].steps[i].note == b[i].note : g_pat.tracks[0].steps[i].note < 0);
    CHECK(same);
}

TEST(bb_play_tables) {
    uint8_t tab[128][3];
    for (int proc = 0; proc < gen::kBBProcCount; ++proc) {
        gen::bbPlayTable(proc, 1234u + uint32_t(proc), tab);
        int rolls = 0, jumps = 0;
        for (int k = 0; k < 128; ++k) {
            CHECK(tab[k][0] < 128);
            CHECK(tab[k][2] <= 15);
            rolls += tab[k][2] != 0;
            if (k > 0) {
                const int prev = (tab[k - 1][0] * 16 / 128 + tab[k - 1][1]) % 16;
                const int cur = (tab[k][0] * 16 / 128 + tab[k][1]) % 16;
                jumps += cur != (prev + 1) % 16;
            }
        }
        (void)rolls;
        if (proc != 7) CHECK(jumps > 0);   // rearranges something (TimelineCut only regroups)
    }
}

TEST(station_digits) {
    int16_t v[gen::kParams];
    defaults(gen::kStation, v);
    gen::Target t = target(0, 63);
    t.text = "JUNGLE";
    t.pi = 0xC204;
    t.freq10 = 981;
    v[0] = 0;   // RDS name
    v[2] = 0;   // 1/16
    v[3] = 0;   // no groups
    v[4] = 1;
    gen::Result r = gen::generate(gen::kStation, v, t);
    CHECK(r.ok);
    // J=10 U=21 N=14 G=7 L=12 E=5 -> 1 0 2 1 1 4 7 1 2 5 as scale degrees.
    static const int kDig[10] = {1, 0, 2, 1, 1, 4, 7, 1, 2, 5};
    uint8_t deg[12];
    const int nd = gen::scaleDegrees(0x0AD, deg);
    for (int i = 0; i < 10; ++i) CHECK_EQ(int(g_pat.tracks[0].steps[i].note), gen::degreeNote(36, deg, nd, kDig[i]));
    CHECK_EQ(int(g_pat.tracks[0].steps[10].note), int(kNoteEmpty));
    CHECK(patternValid());
    // Slices mode with a 16-slice Beat Slice instrument: notes 24 + digit; fives.
    t = slicedTarget(0, 63);
    t.freq10 = 981;
    v[0] = 2;
    v[1] = 1;
    v[3] = 1;
    v[4] = 2;
    CHECK(gen::generate(gen::kStation, v, t).ok);
    CHECK_EQ(int(g_pat.tracks[0].steps[0].note), 24 + 9);
    CHECK_EQ(int(g_pat.tracks[0].steps[1].note), 24 + 8);
    CHECK_EQ(int(g_pat.tracks[0].steps[2].note), 24 + 1);
    CHECK(patternValid());
    // Slices with too few slices: refused.
    t = slicedTarget(0, 63);
    t.slices.count = 8;
    CHECK(!gen::generate(gen::kStation, v, t).ok);
}
