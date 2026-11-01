#include "ui/screens.h"

#include "app/app.h"
#include "sdram_bss.h"

namespace ot {

namespace {
// All screens in one object, created once per Ui (startup only, not in the audio path).
struct ScreenSet {
    ScreenSet(Ui& u, App& a)
        : file(u, a), loader(u, a), pattern(u, a), playback(u, a), song(u, a), instrument(u, a), editor(u, a),
          config(u, a), recorder(u, a), resample(u, a), performance(u, a), mixer(u, a),
          synth(u, a) {}
    FileScreen file;
    LoaderScreen loader;
    PatternScreen pattern;
    PlaybackScreen playback;
    SongScreen song;
    InstrumentScreen instrument;
    EditorScreen editor;
    ConfigScreen config;
    RecorderScreen recorder;
    ResampleScreen resample;   // [I]
    PerformanceScreen performance;
    MixerScreen mixer;
    SynthScreen synth;
};
}  // namespace

// The browsers' shared name lists (screens.h browserListMem()).
OT_SDRAM_BSS alignas(kBrowserListAlign) unsigned char g_browserLists[kBrowserListBytes];

namespace {
// The device's one Ui keeps its screens (~7.5 KB) in the SDRAM instead of on
// the SRAM heap (sdram_bss.h: main-loop data only). A second Ui at the same
// time (host tests) falls back to the heap.
OT_SDRAM_BSS alignas(ScreenSet) unsigned char g_screenMem[sizeof(ScreenSet)];
OT_SDRAM_BSS bool g_screenMemUsed;
}  // namespace

void* createScreens(Ui& ui, App& app, Screen** out) {
    void* mem = g_screenMem;
    if (g_screenMemUsed) mem = ::operator new(sizeof(ScreenSet));
    else g_screenMemUsed = true;
    ScreenSet* s = new (mem) ScreenSet(ui, app);
    out[kModFile] = &s->file;
    out[kModLoader] = &s->loader;
    out[kModPattern] = &s->pattern;
    out[kModPlayback] = &s->playback;
    out[kModSong] = &s->song;
    out[kModInstrument] = &s->instrument;
    out[kModEditor] = &s->editor;
    out[kModConfig] = &s->config;
    out[kModRecorder] = &s->recorder;
    out[kModResample] = &s->resample;
    out[kModPerformance] = &s->performance;
    out[kModMaster] = &s->mixer;
    out[kModSynth] = &s->synth;
    return s;
}

void destroyScreens(void* owner, Screen** screens) {
    if (!owner) return;
    static_cast<ScreenSet*>(owner)->~ScreenSet();
    if (owner == static_cast<void*>(g_screenMem)) g_screenMemUsed = false;
    else ::operator delete(owner);
    for (int i = 0; i < kModCount; ++i) screens[i] = nullptr;
}

}  // namespace ot
