// Per-instance effects (audio/inserts.h IEffect, EffectInsert): the contract on
// a host-only fake effect, the master-insert wrapper, and the module host's
// effect registry.
#include "test.h"

#include <cstring>
#include <vector>

#include "audio/audio_tables.h"
#include "audio/inserts.h"
#include "modules/module.h"
#if __has_include("ot_profile.h")
#include "ot_profile.h"
#endif

using namespace ot;
using namespace ot::audio;

namespace {

// A gain with a one-block ramp: on -> gain (1 + v[0] / 100), off -> back to 1, then false.
struct FakeState {
    float g;   // the gain the last block ended on (0: fresh = 1)
};
const char* const kShapes[] = {"Soft", "Hard"};
class FakeEffect final : public IEffect {
public:
    FakeEffect(const char* id, uint32_t state = sizeof(FakeState), uint32_t work = 16) : id_(id), state_(state), work_(work) {}
    const char* id() const override { return id_; }
    const char* name() const override { return "Fake"; }
    uint32_t stateBytes() const override { return state_; }
    uint32_t workBytes() const override { return work_; }
    int paramCount() const override { return 2; }
    const EffectParam& param(int i) const override {
        static const EffectParam p[2] = {{"Boost", 100, 30, nullptr}, {"Shape", 1, 1, kShapes}};
        return p[i];
    }
    bool process(void* state, void* work, const EffectSettings& s, bool on, float* l, float* r, uint32_t n) override {
        FakeState& st = *static_cast<FakeState*>(state);
        uint8_t* w = static_cast<uint8_t*>(work);
        const float g0 = st.g == 0.0f ? 1.0f : st.g;
        const float g1 = on ? 1.0f + float(s.v[0]) * 0.01f : 1.0f;
        if (!on && g0 == 1.0f) return false;   // through: untouched
        for (uint32_t i = 0; i < n; ++i) {
            const float g = g0 + (g1 - g0) * float(i + 1) / float(n);
            l[i] *= g;
            r[i] *= g;
        }
        ++w[0];   // the work memory is the caller's: it must persist between blocks
        st.g = g1;
        return on || g1 != 1.0f;
    }

private:
    const char* id_;
    uint32_t state_, work_;
};

void tone(float* l, float* r, uint32_t n, uint32_t k) {
    for (uint32_t i = 0; i < n; ++i) l[i] = r[i] = float((i * 7 + k * 13) % 50) * 0.01f - 0.25f;
}

}  // namespace

TEST(effect_defaults_and_value_text) {
    FakeEffect fx("TFX1");
    const EffectSettings d = effectDefaults(fx);
    CHECK_EQ(int(d.v[0]), 30);
    CHECK_EQ(int(d.v[1]), 1);
    for (int i = 2; i < kEffectMaxParams; ++i) CHECK_EQ(int(d.v[i]), 0);
    char t[16];
    fx.valueText(0, 42, t, sizeof(t));
    CHECK_STR(t, "42");
    fx.valueText(1, 0, t, sizeof(t));
    CHECK_STR(t, "Soft");
    fx.valueText(1, 9, t, sizeof(t));   // past the names: clamped to the last
    CHECK_STR(t, "Hard");
    fx.valueText(0, 100, t, 3);         // cut to the buffer, NUL-terminated
    CHECK_STR(t, "10");
}

TEST(effect_insert_equals_the_bare_effect) {
    // The master-insert wrapper runs the effect on its own state and work: bit-exact with
    // the bare call on the same input, a set() taking effect at the next block.
    FakeEffect fx("TFX1");
    alignas(8) static uint8_t st1[kEffectStateMax], st2[kEffectStateMax];
    alignas(8) static uint8_t w1[16], w2[16];
    std::memset(st1, 0, sizeof(st1));
    std::memset(st2, 0, sizeof(st2));
    std::memset(w1, 0, sizeof(w1));
    std::memset(w2, 0, sizeof(w2));
    EffectInsert ins(fx, st1, w1);
    EffectSettings s = effectDefaults(fx);
    ins.set(s, true);
    CHECK(ins.on());
    CHECK_EQ(int(ins.settings().v[0]), 30);
    float a[2][kBlock], b[2][kBlock];
    bool same = true;
    for (uint32_t k = 0; k < 12; ++k) {
        const bool on = k < 8;
        if (k == 4) {
            s.v[0] = 80;
            ins.set(s, true);
        }
        if (k == 8) ins.set(s, false);
        tone(a[0], a[1], kBlock, k);
        tone(b[0], b[1], kBlock, k);
        const bool ra = ins.process(a[0], a[1], kBlock);
        const bool rb = fx.process(st2, w2, s, on, b[0], b[1], kBlock);
        same = same && ra == rb && std::memcmp(a, b, sizeof(a)) == 0;
    }
    CHECK(same);
    CHECK(!ins.on());
    CHECK_EQ(int(w1[0]), int(w2[0]));   // the work memory persisted across the blocks
    // Off and faded: false, the bus untouched.
    tone(a[0], a[1], kBlock, 99);
    tone(b[0], b[1], kBlock, 99);
    CHECK(!ins.process(a[0], a[1], kBlock));
    CHECK(std::memcmp(a, b, sizeof(a)) == 0);
    // reset(): state and work back to zero (fresh).
    ins.reset();
    bool zero = true;
    for (uint8_t v : st1) zero = zero && v == 0;
    for (uint8_t v : w1) zero = zero && v == 0;
    CHECK(zero);
}

// Slow work memory (a module's line in the SDRAM): the wrapper moves it into the
// shared effect window for each block and back. Bit-exact with work in place.
TEST(effect_insert_slow_work_equals_in_place) {
    FakeEffect fx("TFX1");
    alignas(8) static uint8_t st1[kEffectStateMax], st2[kEffectStateMax];
    alignas(8) static int16_t inPlace[8], slow[8], window[kEffectWorkMax / 2];
    std::memset(st1, 0, sizeof(st1));
    std::memset(st2, 0, sizeof(st2));
    std::memset(inPlace, 0, sizeof(inPlace));
    std::memset(slow, 0, sizeof(slow));
    int16_t* const saved = effectWindow();
    setEffectWindow(window);
    EffectInsert a(fx, st1, inPlace), b(fx, st2, nullptr);
    b.setSlowWork(slow);
    const EffectSettings s = effectDefaults(fx);
    a.set(s, true);
    b.set(s, true);
    float x[2][kBlock], y[2][kBlock];
    bool same = true;
    for (uint32_t k = 0; k < 10; ++k) {
        tone(x[0], x[1], kBlock, k);
        tone(y[0], y[1], kBlock, k);
        same = same && a.process(x[0], x[1], kBlock) == b.process(y[0], y[1], kBlock) && std::memcmp(x, y, sizeof(x)) == 0;
        std::memset(window, 0x5A, sizeof(window));   // the window is shared: anything may be in it between blocks
    }
    CHECK(same);
    CHECK(std::memcmp(inPlace, slow, sizeof(slow)) == 0);   // the line holds the same as the in-place one
    CHECK_EQ(int(reinterpret_cast<uint8_t*>(slow)[0]), 10);
    b.reset();
    bool zero = true;
    for (int16_t v : slow) zero = zero && v == 0;
    CHECK(zero);
    setEffectWindow(saved);
}

// The registry: effects of the profile's modules (kCapEffect, IModule::effect(i)), in
// module order, by id; a module repeating an effect id or over the limits is refused.
namespace {
FakeEffect gFxA("TFXA"), gFxB("TFXB"), gFxDup("TFXA"), gFxBig("TFXC", kEffectStateMax + 8);
class FxModule final : public mod::IModule {
public:
    FxModule(const char* id, IEffect* a, IEffect* b) : a_(a), b_(b) {
        std::memcpy(info_.id, id, 5);
        info_.api = mod::kModuleApi;
        info_.version = 100;
        info_.name = "Effects";
        info_.caps = mod::kCapEffect;
        info_.maxUnits = 1;
    }
    const mod::ModuleInfo& info() const override { return info_; }
    bool attach(mod::ModuleHost&) override { return true; }
    IEffect* effect(int i) override { return i == 0 ? a_ : i == 1 ? b_ : nullptr; }

private:
    mod::ModuleInfo info_ = {};
    IEffect* a_;
    IEffect* b_;
};
FxModule gFxMod("TEF1", &gFxA, &gFxB), gFxModDup("TEF2", &gFxDup, nullptr), gFxModBig("TEF3", &gFxBig, nullptr);
}  // namespace

TEST(module_host_registers_effects) {
    mod::ModuleHost& h = mod::ModuleHost::instance();
    h.init();
    const int base = h.effectCount();
    CHECK(h.effectAt(base) == nullptr);
    CHECK(h.addForTest(gFxMod));
    CHECK_EQ(h.effectCount(), base + 2);
    CHECK(h.effectAt(base) == &gFxA);
    CHECK(h.effectAt(base + 1) == &gFxB);
    CHECK(h.effectAt(-1) == nullptr);
    CHECK(h.effectById("TFXB") == &gFxB);
    CHECK(h.effectById("NONE") == nullptr);
    CHECK(!h.addForTest(gFxModDup));   // TFXA again
    CHECK(!h.addForTest(gFxModBig));   // state over kEffectStateMax
    CHECK_EQ(h.effectCount(), base + 2);
}

#if defined(OT_MOD_BBDC) && OT_MOD_BBDC
// BBDC provides the chorus as an effect: Mode I / II / I+II, Mix, Width, Tone.
TEST(bbdc_provides_the_chorus_effect) {
    IEffect* fx = mod::ModuleHost::instance().effectById("BBDC");
    CHECK(fx != nullptr);
    if (!fx) return;
    CHECK_STR(fx->name(), "Chorus");
    CHECK(fx->stateBytes() <= kEffectStateMax);
    CHECK(fx->workBytes() > 0 && fx->workBytes() <= kEffectWorkMax);
    CHECK(fx->tailFrames() > 0);
    CHECK_EQ(fx->paramCount(), 4);
    CHECK_STR(fx->param(0).label, "Mode");
}
#endif
