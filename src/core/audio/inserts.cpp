// Per-instance effects (audio/inserts.h): the defaults and the master-insert wrapper.
#include "audio/inserts.h"

#include <cstring>

#include "audio/ring_io.h"

namespace ot {
namespace audio {

namespace {
int16_t* gWindow = nullptr;
}  // namespace

int16_t* effectWindow() { return gWindow; }
void setEffectWindow(int16_t* window) { gWindow = window; }

void IEffect::valueText(int i, uint8_t v, char* out, int cap) const {
    if (!out || cap <= 0) return;
    const EffectParam& p = param(i);
    char num[4];
    const char* s = num;
    if (p.names) {
        s = p.names[v > p.max ? p.max : v];
    } else {
        int k = 0;
        if (v >= 100) num[k++] = char('0' + v / 100);
        if (v >= 10) num[k++] = char('0' + v / 10 % 10);
        num[k++] = char('0' + v % 10);
        num[k] = '\0';
    }
    int k = 0;
    for (; k < cap - 1 && s[k]; ++k) out[k] = s[k];
    out[k] = '\0';
}

EffectSettings effectDefaults(const IEffect& fx) {
    EffectSettings s = {};
    const int n = fx.paramCount() < kEffectMaxParams ? fx.paramCount() : kEffectMaxParams;
    for (int i = 0; i < n; ++i) s.v[i] = fx.param(i).def;
    return s;
}

void EffectInsert::set(const EffectSettings& s, bool on) {
    const uint8_t next = uint8_t(cur_ ^ 1u);
    pub_[next] = Pub{s, on};
    __atomic_store_n(&cur_, next, __ATOMIC_RELEASE);
}

void EffectInsert::reset() {
    std::memset(state_, 0, fx_.stateBytes());
    if (work_) std::memset(work_, 0, fx_.workBytes());
    fx_.reset(state_, work_);
}

bool EffectInsert::process(float* l, float* r, uint32_t n) {
    const Pub p = pub_[__atomic_load_n(&cur_, __ATOMIC_ACQUIRE)];
    if (!slow_) return fx_.process(state_, work_, p.set, p.on, l, r, n);
    // The line comes in from the slow memory, the block runs on the copy, the
    // copy goes back (one span each way).
    int16_t* const w = gWindow;
    int16_t* const line = static_cast<int16_t*>(work_);
    const uint32_t n16 = fx_.workBytes() / sizeof(int16_t);
    if (!w || !line || n16 > kEffectWorkMax / 2) return false;
    spanIn(line, w, n16);
    const bool live = fx_.process(state_, w, p.set, p.on, l, r, n);
    spanOut(line, w, n16);
    return live;
}

}  // namespace audio
}  // namespace ot
