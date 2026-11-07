// [3.x] Z chase (see synth_chase.h): main loop only.
#include "app/synth_chase.h"

#include <cstring>

#include "app/app.h"
#include "model/fx.h"
#include "sdram_bss.h"

namespace ot {

namespace {

using chase::State;
using chase::Summary;

OT_SDRAM_BSS chase::Memory g_chase;   // main-loop data: zeroed by otAppMain(), reset in App::init()

inline bool synth(uint8_t i) { return isSynthInstrument(i); }

void ownState(State& st) {
    std::memset(st.preset, 0, sizeof st.preset);
    std::memset(st.last, chase::kUnknown, sizeof st.last);
}

// Steps 0..lastStep of `tracks` (nullptr = an empty track) played from `st`.
// The sequencer's order is step by step, tracks 0..15, FX-B then FX-A; a
// track's no-note Z depends only on that track's own notes, so the tracks
// are read one after the other and the latest Z per slot (in play order)
// wins. Returns kHasZ when a Z took effect, kUnresolved when a no-note Z met
// a track whose last instrument `st` does not know.
uint8_t scanSteps(const Track* const* tracks, int lastStep, uint16_t used, State& st) {
    uint8_t out = 0;
    int order[kSynthSlotCount] = {-1, -1, -1};
    uint8_t val[kSynthSlotCount] = {};
    static_assert(kSynthSlotCount == 3, "order[] names each slot");
    for (int t = 0; t < kTrackCount; ++t) {
        const Track* tk = tracks[t];
        if (!tk) continue;
        uint8_t last = st.last[t];
        for (int i = 0; i <= lastStep; ++i) {
            const Step& s = tk->steps[i];
            for (int c = 0; c < 2; ++c) {   // model fx[0] = FX-B, evaluated first
                const StepFx f = s.fx[c];
                if (!isPresetCmd(f.type, f.value)) continue;
                uint8_t in = s.instrument;
                if (!synth(in) && s.note < 0) {
                    if (last == chase::kUnknown) out = uint8_t(out | chase::kUnresolved);
                    else in = last;
                }
                if (!synth(in)) continue;
                const uint8_t n = presetCmdIndex(f.value);
                if (n && !((used >> n) & 1u)) continue;   // an empty preset: the slot plays on
                const int slot = synthSlotOf(in);
                const int key = (i * kTrackCount + t) * 2 + c;
                if (key > order[slot]) {
                    order[slot] = key;
                    val[slot] = n;
                }
            }
            if (s.note >= 0) last = synth(s.instrument) ? s.instrument : chase::kNonSynth;
        }
        st.last[t] = last;
    }
    for (int s = 0; s < kSynthSlotCount; ++s)
        if (order[s] >= 0) {
            st.preset[s] = val[s];
            out = uint8_t(out | chase::kHasZ);
        }
    return out;
}

uint8_t zNibble(const Summary& m, int s) {
    const int k = s + 1;   // nibble 0 holds the flags
    return uint8_t((m.zf[k >> 1] >> ((k & 1) * 4)) & 0x0F);
}
void setZNibble(Summary& m, int s, uint8_t v) {
    const int k = s + 1;
    const int sh = (k & 1) * 4;
    m.zf[k >> 1] = uint8_t((m.zf[k >> 1] & ~(0x0F << sh)) | ((v & 0x0F) << sh));
}
uint8_t lastCode(const Summary& m, int t) {
    const int b = t * 3;
    const unsigned w = unsigned(m.last[b >> 3]) | (unsigned((b >> 3) + 1 < 6 ? m.last[(b >> 3) + 1] : 0) << 8);
    return uint8_t((w >> (b & 7)) & 7u);
}
void setLastCode(Summary& m, int t, uint8_t v) {
    for (int k = 0; k < 3; ++k) {
        const int b = t * 3 + k;
        if ((v >> k) & 1u) m.last[b >> 3] = uint8_t(m.last[b >> 3] | (1u << (b & 7)));
        else m.last[b >> 3] = uint8_t(m.last[b >> 3] & ~(1u << (b & 7)));
    }
}

// The tracks the sequencer would play for stored pattern num (loadPattern):
// nullptr for an ext track without a block (empty) and for every track of a
// pattern that is not present (the default pattern). Returns the last step.
int storedTracks(const App& a, int num, const Track* tracks[kTrackCount]) {
    const bool present = a.patternPresent(num);
    for (int t = 0; t < kTrackCount; ++t) tracks[t] = present ? a.findStoredTrack(num, t) : nullptr;
    if (!present) return -1;
    const int li = a.storedStock(num).tracks[0].lastStep;
    return li < 0 ? 0 : (li >= kStepCount ? kStepCount - 1 : li);
}

void summarise(const App& a, int num, uint16_t used) {
    Summary& m = g_chase.pat[num - 1];
    std::memset(&m, 0, sizeof m);
    const Track* tracks[kTrackCount];
    const int li = storedTracks(a, num, tracks);
    State st;
    std::memset(st.preset, chase::kNoZ, sizeof st.preset);
    std::memset(st.last, chase::kUnknown, sizeof st.last);
    const uint8_t flags = li >= 0 ? scanSteps(tracks, li, used, st) : 0;
    m.zf[0] = uint8_t(chase::kValid | flags);
    for (int s = 0; s < kSynthSlotCount; ++s) setZNibble(m, s, st.preset[s]);
    for (int t = 0; t < kTrackCount; ++t) {
        const uint8_t l = st.last[t];
        setLastCode(m, t, l == chase::kUnknown ? 0 : (synth(l) ? uint8_t(2 + synthSlotOf(l)) : 1));
    }
}

// A whole pass of stored pattern num from `st`.
void playStored(const App& a, int num, uint16_t used, State& st) {
    if (num < 1 || num > kPatternCount) return;
    Summary& m = g_chase.pat[num - 1];
    if (!(m.zf[0] & chase::kValid)) summarise(a, num, used);
    if (m.zf[0] & chase::kUnresolved) {   // depends on what came before: read it again
        const Track* tracks[kTrackCount];
        const int li = storedTracks(a, num, tracks);
        if (li >= 0) scanSteps(tracks, li, used, st);
        return;
    }
    if (m.zf[0] & chase::kHasZ)
        for (int s = 0; s < kSynthSlotCount; ++s)
            if (zNibble(m, s) != chase::kNoZ) st.preset[s] = zNibble(m, s);
    for (int t = 0; t < kTrackCount; ++t) {
        const uint8_t c = lastCode(m, t);
        if (c == 1) st.last[t] = chase::kNonSynth;
        else if (c >= 2) st.last[t] = uint8_t(kFirstSynthInstrument + c - 2);
    }
}

// Playlist positions 0..pos-1, whole (an empty entry is skipped).
void playPrefix(const App& a, int pos, uint16_t used, State& st) {
    if (pos > kSongSlots) pos = kSongSlots;
    for (int i = 0; i < pos; ++i)
        if (const uint8_t num = a.project.playlist[i]) playStored(a, num, used, st);
}

// Steps 0..lastStep of the working pattern (the sequencer's current buffer).
void playCurrent(App& a, int lastStep, uint16_t used, State& st) {
    if (lastStep < 0) return;
    Pattern& p = a.pattern();
    const int li = p.tracks[0].lastStep;
    if (lastStep > li) lastStep = li;
    if (lastStep >= kStepCount) lastStep = kStepCount - 1;
    const Track* tracks[kTrackCount];
    for (int t = 0; t < kTrackCount; ++t) tracks[t] = &p.tracks[t];
    scanSteps(tracks, lastStep, used, st);
}

uint16_t usedPresets(const App& a) {
    uint16_t m = 0;
    for (int n = 1; n <= kSynthPresetCount; ++n)
        if (a.synthPresetUsed(n)) m = uint16_t(m | (1u << n));
    return m;
}

}  // namespace

namespace chase {
Memory& mem() { return g_chase; }
}  // namespace chase

void App::resetSynthChase() {
    std::memset(&g_chase, 0, sizeof g_chase);   // every slot on its own patch, as the engine starts
}

void App::chaseInvalidate(int num) {
    if (num < 1 || num > kPatternCount) return;
    g_chase.pat[num - 1].zf[0] = 0;
    ++g_chase.gen;
}

void App::chaseInvalidateAll() {
    for (Summary& m : g_chase.pat) m.zf[0] = 0;
    ++g_chase.gen;
}

void App::chaseTouch() { ++g_chase.gen; }

void App::chaseForget() {
    g_chase.keyValid = 0;
    std::memset(g_chase.applied, 0xFF, sizeof g_chase.applied);
}

bool App::synthChaseSummarised(int num) const {
    return num >= 1 && num <= kPatternCount && (g_chase.pat[num - 1].zf[0] & chase::kValid);
}

void App::synthChase(bool songMode, int songPos, uint8_t patternNum, int lastStep, uint8_t preset[kSynthSlotCount],
                     uint8_t lastInstr[kTrackCount]) {
    (void)patternNum;   // the sequencer's current buffer is pattern()
    const uint16_t used = usedPresets(*this);
    State st;
    ownState(st);
    if (songMode) playPrefix(*this, songPos, used, st);
    playCurrent(*this, lastStep, used, st);
    std::memcpy(preset, st.preset, sizeof st.preset);
    std::memcpy(lastInstr, st.last, sizeof st.last);
}

}  // namespace ot
