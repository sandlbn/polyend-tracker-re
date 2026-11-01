// Master insert slot (audio/inserts.h, docs/PLAN_3X_M2.md M-C): a module's
// effect on the master bus, after the master sum and before the limiter.
// Not called while the bus is silent (unless it reports a tail); none bound
// = the engine renders exactly as before.
#include "test.h"

#include <cmath>
#include <memory>
#include <vector>

#include "audio/engine.h"

using namespace ot;
using namespace ot::audio;

namespace {

struct Rig {
    std::vector<int16_t> fx = std::vector<int16_t>(AudioEngine::kFxMemorySamples);
    std::vector<int16_t> pool = std::vector<int16_t>(1u << 20);
    std::unique_ptr<Instrument[]> ins = std::unique_ptr<Instrument[]>(new Instrument[kSampleInstrumentCount]);
    std::unique_ptr<AudioEngine> eng = std::unique_ptr<AudioEngine>(new AudioEngine());
    Rig() {
        CHECK(eng->init(fx.data(), fx.size(), pool.data(), pool.size(), ins.get()));
        std::vector<int16_t> d(20000);
        for (size_t i = 0; i < d.size(); ++i) d[i] = int16_t(12000.0f * std::sin(0.05f * float(i)));
        ins[0].lengthFrames = uint32_t(d.size());
        ins[0].playMode = uint8_t(PlayMode::OneShot);
        CHECK(eng->bank().store(0, d.data(), uint32_t(d.size()), false));
    }
    void note() {
        VoiceNoteOn n;
        n.instrument = 0;
        n.note = 60;
        eng->voiceNoteOn(0, n);
    }
    std::vector<int16_t> render(int blocks) {
        std::vector<int16_t> out;
        int16_t b[kBlock * 2];
        for (int i = 0; i < blocks; ++i) {
            eng->renderLive(b);
            out.insert(out.end(), b, b + kBlock * 2);
        }
        return out;
    }
};

// Halves the bus and counts calls; reports a tail for `tail` blocks after
// the last loud one.
struct HalfInsert : IMasterInsert {
    int calls = 0, tail = 0, left = 0;
    float peakIn = 0.0f;
    bool process(float* l, float* r, uint32_t n) override {
        ++calls;
        float p = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            p = std::fmax(p, std::fabs(l[i]));
            l[i] *= 0.5f;
            r[i] *= 0.5f;
        }
        peakIn = std::fmax(peakIn, p);
        left = p > 0.0f ? tail : (left > 0 ? left - 1 : 0);
        return left > 0;
    }
};

double rms(const std::vector<int16_t>& x) {
    double s = 0;
    for (int16_t v : x) s += double(v) * v;
    return std::sqrt(s / double(x.size()));
}

}  // namespace

TEST(master_insert_none_bound_is_unchanged) {
    Rig a, b;
    a.note();
    b.note();
    CHECK(b.eng->masterInsert() == nullptr);
    CHECK(a.render(40) == b.render(40));
}

TEST(master_insert_processes_the_master_bus) {
    Rig plain, half;
    HalfInsert ins;
    half.eng->setMasterInsert(&ins);
    plain.note();
    half.note();
    const std::vector<int16_t> p = plain.render(40), h = half.render(40);
    CHECK(ins.calls > 0);
    CHECK(ins.peakIn > 0.0f);
    const double ratio = rms(h) / rms(p);
    CHECK(ratio > 0.45 && ratio < 0.55);   // before the limiter: half the level
}

TEST(master_insert_skipped_while_silent_but_tail_plays_out) {
    Rig r;
    HalfInsert ins;
    r.eng->setMasterInsert(&ins);
    r.render(20);
    CHECK_EQ(ins.calls, 0);   // nothing on the bus: never called
    ins.tail = 5;
    r.note();
    r.render(200);            // the one-shot ends inside this run
    const int afterSound = ins.calls;
    CHECK(afterSound > 0);
    r.render(50);
    // At most the tail blocks after the sound ended, then silence again.
    CHECK(ins.calls - afterSound <= 5);
    const int quiet = ins.calls;
    r.render(20);
    CHECK_EQ(ins.calls, quiet);
    r.eng->setMasterInsert(nullptr);
    r.note();
    r.render(10);
    CHECK_EQ(ins.calls, quiet);
}

TEST(master_inserts_run_as_a_chain_in_order) {
    Rig r;
    HalfInsert first, second;
    IMasterInsert* chain[2] = {&first, &second};
    r.eng->setMasterInserts(chain, 2);
    CHECK(r.eng->masterInsert(0) == &first && r.eng->masterInsert(1) == &second);
    r.note();
    r.render(10);
    CHECK(first.calls > 0 && first.calls == second.calls);
    // The second sees the first's output: half the level.
    CHECK(second.peakIn > 0.0f && std::fabs(second.peakIn - 0.5f * first.peakIn) < 1e-4f);
    r.eng->setMasterInserts(nullptr, 0);
    CHECK(r.eng->masterInsert(0) == nullptr);
}

namespace {
// Records the tempo it is given (IMasterInsert::setTempo, docs/PLAN_TECHNO.md E0).
struct TempoInsert : IMasterInsert {
    std::vector<float> tempos;
    bool process(float*, float*, uint32_t) override { return false; }
    void setTempo(float bpm) override { tempos.push_back(bpm); }
};
}  // namespace

// The chain hears the song tempo once when it is bound and on every change,
// whether or not the bus carries sound; an insert without setTempo is fine.
TEST(master_inserts_get_the_tempo_on_change) {
    Rig r;
    TempoInsert t;
    HalfInsert plain;
    IMasterInsert* chain[2] = {&plain, &t};
    r.eng->setMasterInserts(chain, 2);
    r.render(3);
    CHECK_EQ(t.tempos.size(), 1u);
    CHECK_NEAR(t.tempos[0], r.eng->tempo(), 1e-6);
    r.eng->engineBlockUpdate(140.0f);
    r.render(3);
    CHECK_EQ(t.tempos.size(), 2u);
    CHECK_NEAR(t.tempos.back(), 140.0f, 1e-6);
    r.eng->engineBlockUpdate(140.0f);   // unchanged: no call
    r.render(2);
    CHECK_EQ(t.tempos.size(), 2u);
    r.eng->engineBlockUpdate(97.5f);
    r.note();
    r.render(2);
    CHECK_EQ(t.tempos.size(), 3u);
    CHECK_NEAR(t.tempos.back(), 97.5f, 1e-6);
    // Rebinding the chain hands the tempo over again.
    r.eng->setMasterInserts(chain, 2);
    r.render(1);
    CHECK_EQ(t.tempos.size(), 4u);
    CHECK_NEAR(t.tempos.back(), 97.5f, 1e-6);
}
