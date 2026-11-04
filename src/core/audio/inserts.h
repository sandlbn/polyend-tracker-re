// Effect slots for modules (docs/PLAN_3X.md §2.2, docs/PLAN_3X_M2.md M-C).
// Module-facing: only <cstdint>. Audio context, like ISynthEngine: no
// allocation, no I/O, no locks; parameters are read once per block (a module
// publishes them from the main loop) and ramped across the block.
#pragma once

#include <cstdint>

namespace ot {
namespace audio {

// After the master sum (dry + delay / reverb returns + line in, saturation
// applied), before the limiter. Up to AudioEngine::kMaxMasterInserts per
// image, run in profile order (a chain).
class IMasterInsert {
public:
    // Process the stereo master bus in place (n = kBlock frames). Called while
    // the bus carries sound or the previous call returned true; return true
    // while the insert still has a tail to play out (a delay line).
    virtual bool process(float* l, float* r, uint32_t n) = 0;
    // [3.x] E0: the song tempo in BPM (the sequencer's smoothed tempo), before
    // process() in the block it changes, and once when the chain is bound;
    // also while the bus is silent (tempo-synced delays). Optional.
    virtual void setTempo(float bpm) { (void)bpm; }
};

// [3.x] Per-instance effects: one effect kind, several instances (a synth
// slot insert each, or a master insert), the caller owning each instance's
// memory.
constexpr int kEffectMaxParams = 6;       // F2..F7 of a settings view
constexpr uint32_t kEffectStateMax = 64;  // per-instance state, bytes
constexpr uint32_t kEffectWorkMax = 512;  // per-instance delay memory, bytes

// The stored values of one instance, one byte each (0..param(i).max); unused 0.
struct EffectSettings {
    uint8_t v[kEffectMaxParams];
};

// One setting as a settings view and the project file see it.
struct EffectParam {
    const char* label;           // bottom-bar label, <= 11 characters ("Mode", "Tone")
    uint8_t max;                 // values 0..max (<= 100; an enum: count - 1)
    uint8_t def;                 // the default
    const char* const* names;    // an enum's max + 1 value names, or nullptr (a number)
};

// A stateless singleton per effect kind, like ISynthEngine. The caller owns
// `state` (stateBytes(), 8-aligned) and `work` (workBytes(), 4-aligned); all
// zero is fresh. Audio context: no allocation, no I/O, no locks. The caller
// keeps `work` between blocks and may hold it in slow memory, presenting a
// copy for the block, so an effect touches `work` only inside process() and
// keeps no pointer into it.
class IEffect {
public:
    virtual const char* id() const = 0;       // 4 characters, unique in a profile ("BBDC")
    virtual const char* name() const = 0;     // the list entry, <= 8 characters ("Chorus")
    virtual uint32_t stateBytes() const = 0;  // <= kEffectStateMax
    virtual uint32_t workBytes() const { return 0; }   // <= kEffectWorkMax; 0: none
    virtual int paramCount() const = 0;       // 1..kEffectMaxParams
    virtual const EffectParam& param(int i) const = 0;
    // Value text of setting i: the enum name (clamped to the last) or the
    // number. Main loop.
    virtual void valueText(int i, uint8_t v, char* out, int cap) const;
    // Frames of silent input after which the output is silent again (a delay
    // line's tail): a caller stops processing a silent bus after that many.
    virtual uint32_t tailFrames() const { return 0; }
    // One block in place (n = kBlock). on = false fades to bypass and returns
    // false once l / r pass untouched; on = true returns true. Settings are
    // read once per block and ramped across it (no step between blocks).
    virtual bool process(void* state, void* work, const EffectSettings& s, bool on, float* l, float* r,
                         uint32_t n) = 0;
    // Back to fresh after the caller zeroed state and work (an effect with
    // derived values seeds them here).
    virtual void reset(void* state, void* work) {
        (void)state;
        (void)work;
    }
    virtual void setTempo(float bpm) { (void)bpm; }
};

// Every setting at its default.
EffectSettings effectDefaults(const IEffect& fx);

// The shared SRAM window for slow work memory (kEffectWorkMax bytes, free
// while the master chain runs; the engine sets it before audio starts).
int16_t* effectWindow();
void setEffectWindow(int16_t* window);

// One IEffect instance as a master insert: its state, its work (static
// storage of the module) and the published settings (main loop -> audio,
// taken at the next block).
class EffectInsert : public IMasterInsert {
public:
    // init: the settings before the first set() (off).
    constexpr EffectInsert(IEffect& fx, void* state, void* work, const EffectSettings& init = {})
        : fx_(fx), state_(state), work_(work), pub_{{init, false}, {init, false}} {}
    void set(const EffectSettings& s, bool on);   // main loop
    EffectSettings settings() const { return pub_[cur_].set; }
    bool on() const { return pub_[cur_].on; }
    // Work in slow memory (the SDRAM), moved into effectWindow() and back once
    // per block; from attach() (main loop, before audio).
    void setSlowWork(void* work) {
        work_ = work;
        slow_ = true;
    }
    void reset();   // zero state and work, then IEffect::reset; before audio or in it
    bool process(float* l, float* r, uint32_t n) override;
    void setTempo(float bpm) override { fx_.setTempo(bpm); }
    IEffect& effect() const { return fx_; }

private:
    struct Pub {
        EffectSettings set;
        bool on;
    };
    IEffect& fx_;
    void* state_;
    void* work_;
    Pub pub_[2];
    volatile uint8_t cur_ = 0;
    bool slow_ = false;
};

}  // namespace audio
}  // namespace ot
