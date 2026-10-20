#include "ui/screens.h"

#include "app/app.h"
#include "sdram_bss.h"
#include "ui/screen_games.h"

namespace ot {

namespace {
// All screens in one object, created once per Ui (startup only, not in the audio path).
struct ScreenSet {
    ScreenSet(Ui& u, App& a)
        : file(u, a), loader(u, a), pattern(u, a), playback(u, a), song(u, a), instrument(u, a), editor(u, a),
          config(u, a), recorder(u, a), resample(u, a), performance(u, a), mixer(u, a),
#if OT_GAMES
          games(u, a),
#endif
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
#if OT_GAMES
    GamesScreen games;
#endif
    SynthScreen synth;
};
}  // namespace

// The browsers' shared name lists (screens.h browserListMem()).
OT_SDRAM_BSS alignas(8) unsigned char g_browserLists[kBrowserListBytes];

namespace {
// The device's one Ui keeps its screens (~7.5 KB) in the SDRAM instead of on
// the SRAM heap (sdram_bss.h: main-loop data only). A second Ui at the same
// time (host tests) falls back to the heap.
OT_SDRAM_BSS alignas(ScreenSet) unsigned char g_screenMem[sizeof(ScreenSet)];
OT_SDRAM_BSS bool g_screenMemUsed;
}  // namespace

void* createScreens(Ui& ui, App& app, Screen** out) {
    ScreenSet* s;
    if (!g_screenMemUsed) {
        g_screenMemUsed = true;
        s = new (static_cast<void*>(g_screenMem)) ScreenSet(ui, app);
    } else {
        s = new ScreenSet(ui, app);
    }
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
#if OT_GAMES
    out[kModGames] = &s->games;
#endif
    out[kModSynth] = &s->synth;
    return s;
}

void destroyScreens(void* owner, Screen** screens) {
    if (!owner) return;
    auto* s = static_cast<ScreenSet*>(owner);
    if (owner == static_cast<void*>(g_screenMem)) {
        s->~ScreenSet();
        g_screenMemUsed = false;
    } else {
        delete s;
    }
    for (int i = 0; i < kModCount; ++i) screens[i] = nullptr;
}

}  // namespace ot
