// [OT] Z chase (see synth_chase.h).
#include "app/synth_chase.h"

#include <cstring>

#include "app/app.h"
#include "model/fx.h"
#include "sdram_bss.h"

namespace ot {
namespace chase {

namespace {
OT_SDRAM_BSS Memory g_chase;

bool synthInstr(uint8_t in) { return isSynthInstrument(in); }
}  // namespace

Memory& mem() { return g_chase; }

void scan(const Track* const tracks[kTrackCount], int from, int toInclusive, uint16_t used, State& io,
          Summary* sum) {
    uint8_t touched = 0;   // slots an effective Z set
    bool unresolved = false;
    if (from < 0) from = 0;
    if (toInclusive > kStepCount - 1) toInclusive = kStepCount - 1;
    for (int s = from; s <= toInclusive; ++s) {
        for (int t = 0; t < kTrackCount; ++t) {
            const Track* tr = tracks[t];
            if (!tr) continue;
            const Step& st = tr->steps[s];
            for (int c = 0; c < kFxPerStep; ++c) {   // fx[0] = FX-B first, then FX-A (stepStart)
                const StepFx& f = st.fx[c];
                if (!isPresetCmd(f.type, f.value)) continue;
                const uint8_t n = presetCmdIndex(f.value);
                if (n && !((used >> n) & 1)) continue;   // an empty preset: the slot plays on
                uint8_t in = st.instrument;
                if (!synthInstr(in) && st.note < 0) {
                    if (io.last[t] == kNone) {
                        // Before this track's first note: depends on what came before.
                        if (sum) unresolved = true;
                        continue;
                    }
                    if (synthInstr(io.last[t])) in = io.last[t];
                }
                if (!synthInstr(in)) continue;
                const int slot = synthSlotOf(in);
                io.preset[slot] = n;
                touched = uint8_t(touched | (1u << slot));
            }
            if (st.note >= 0) io.last[t] = st.instrument;
        }
    }
    if (!sum) return;
    sum->flags = uint8_t(kValid | (touched ? kHasZ : 0) | (unresolved ? kUnresolved : 0));
    for (int k = 0; k < kSynthSlotCount; ++k) sum->zOut[k] = (touched >> k) & 1 ? io.preset[k] : kNone;
    for (int t = 0; t < kTrackCount; ++t) sum->last[t] = io.last[t];
}

void apply(const Summary& sum, State& io) {
    if (sum.flags & kHasZ)
        for (int k = 0; k < kSynthSlotCount; ++k)
            if (sum.zOut[k] != kNone) io.preset[k] = sum.zOut[k];
    for (int t = 0; t < kTrackCount; ++t)
        if (sum.last[t] != kNone) io.last[t] = sum.last[t];
}

void resetAll() { std::memset(&g_chase, 0, sizeof(g_chase)); }

void invalidate(int num) {
    if (num >= 1 && num <= kPatternCount) g_chase.pat[num - 1].flags = 0;
    ++g_chase.patSerial;
}

void invalidateAll() {
    for (auto& p : g_chase.pat) p.flags = 0;
    ++g_chase.patSerial;
}

uint32_t playlistHash(const uint8_t* playlist, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; ++i) h = (h ^ playlist[i]) * 16777619u;
    return h ^ uint32_t(n);
}

}  // namespace chase

// ---------------------------------------------------------------------------
// App side

uint16_t App::chaseUsed() {
    uint16_t used = 0;
    for (int n = 1; n <= kSynthPresetCount; ++n)
        if (synthPresetUsed(n)) used = uint16_t(used | (1u << n));
    chase::Memory& m = chase::mem();
    if (used != m.used) {   // what a Z does changed: every summary is stale
        m.used = used;
        chase::invalidateAll();
    }
    return used;
}

const chase::Summary& App::chaseSummary(int num, uint16_t used) {
    chase::Summary& sum = chase::mem().pat[num - 1];
    if (sum.flags & chase::kValid) return sum;
    chase::State st;
    chase::startState(st);
    const Track* tracks[kTrackCount] = {};
    const bool present = patternPresent(num);   // else it plays as the default pattern: empty
    if (present)
        for (int t = 0; t < kTrackCount; ++t) tracks[t] = findStoredTrack(num, t);
    chase::scan(tracks, 0, present ? storedStock(num).tracks[0].lastStep : -1, used, st, &sum);
    return sum;
}

void App::chasePrefix(int pos, uint16_t used, chase::State& io) {
    chase::Memory& m = chase::mem();
    if (pos > kSongSlots) pos = kSongSlots;
    const uint32_t hash = chase::playlistHash(project.playlist, pos);
    if (m.prefixValid && m.prefixPos == pos && m.prefixHash == hash && m.prefixSerial == m.patSerial &&
        m.prefixUsed == used) {
        io = m.prefix;
        return;
    }
    chase::startState(io);
    for (int i = 0; i < pos; ++i) {
        const int num = project.playlist[i];
        if (num < 1 || num > kPatternCount) continue;   // an empty entry plays nothing
        const chase::Summary& sum = chaseSummary(num, used);
        if (!(sum.flags & chase::kUnresolved)) {
            chase::apply(sum, io);
            continue;
        }
        const Track* tracks[kTrackCount];
        for (int t = 0; t < kTrackCount; ++t) tracks[t] = findStoredTrack(num, t);
        chase::scan(tracks, 0, storedStock(num).tracks[0].lastStep, used, io);   // unresolved: present
    }
    m.prefix = io;
    m.prefixPos = uint8_t(pos);
    m.prefixHash = hash;
    m.prefixSerial = m.patSerial;
    m.prefixUsed = used;
    m.prefixValid = 1;
}

void App::chaseAt(bool songMode, int songPos, int lastStepInclusive, chase::State& io) {
    const uint16_t used = chaseUsed();
    if (songMode) chasePrefix(songPos, used, io);
    else chase::startState(io);
    if (lastStepInclusive < 0) return;
    const Pattern& p = pattern();
    const Track* tracks[kTrackCount];
    for (int t = 0; t < kTrackCount; ++t) tracks[t] = &p.tracks[t];
    chase::scan(tracks, 0, lastStepInclusive, used, io);
}

void App::serviceSynthChase() {
    if (seq.isPlaying() || rendering_ || usbActive_ || (jobBusy() && jobOp() != JobOp::Autosave)) return;
    chase::Memory& m = chase::mem();
    if (seq.isCountingIn()) {
        // The count-in's start puts the slots on their own patches (audio
        // context), and a Stop zeroes them: applied again once it is over.
        m.stopped.valid = 0;
        return;
    }
    const uint16_t used = chaseUsed();   // first: may drop every summary
    chase::StopKey k;
    std::memset(&k, 0, sizeof(k));
    k.patSerial = m.patSerial;
    k.presetSerial = m.presetSerial;
    k.stopSerial = m.stopSerial;
    k.ticks = uint32_t(seq.tickCount());   // any playback since, also one too short to be seen
    k.used = used;
    // What Play would start on: the Song screen plays the song from its
    // cursor (F2), a position at its step 0, so the positions before it; any
    // other screen the current pattern alone, up to the pattern cursor.
    const int songPos = ui.songCursorPos();
    k.songMode = songPos >= 0 ? 1 : 0;
    k.pos = uint8_t(k.songMode ? songPos : 0);
    k.cursor = int16_t(k.songMode ? -1 : ui.patternCursorStep());
    k.pattern = patternNumber();
    k.playlistHash = k.songMode ? chase::playlistHash(project.playlist, k.pos) : 0;
    k.valid = 1;
    if (std::memcmp(&k, &m.stopped, sizeof(k)) == 0) {
        chaseWarm(used);
        return;
    }
    m.stopped = k;
    chase::State st;
    const int last = pattern().tracks[0].lastStep;
    chaseAt(k.songMode != 0, k.pos, k.cursor > last ? last : k.cursor, st);   // -1: none of the pattern
    for (int s = 0; s < kSynthSlotCount; ++s) {
        engine.synth().presetSelect(s, st.preset[s]);
        m.applied[s] = st.preset[s];
    }
}

void App::chaseReapply(int s) {
    // Stopped, the slot back from something that took it off its preset (a
    // Load Patch preview): what the stopped chase gave it, if still current.
    chase::Memory& m = chase::mem();
    if (s < 0 || s >= kSynthSlotCount || !m.stopped.valid || seq.isPlaying() || seq.isCountingIn()) return;
    engine.synth().presetSelect(s, m.applied[s]);
}

void App::chaseWarm(uint16_t used) {
    chase::Memory& m = chase::mem();
    for (int i = 0, built = 0; i < kPatternCount && built < 2; ++i) {
        const int idx = m.warmNext;
        m.warmNext = uint8_t(idx + 1 >= kPatternCount ? 0 : idx + 1);
        if (m.pat[idx].flags & chase::kValid) continue;
        if (patternPresent(idx + 1)) ++built;   // an absent one costs no scan
        chaseSummary(idx + 1, used);
    }
}

void App::synthChase(bool songMode, int songPos, uint8_t pattern, int lastStepInclusive,
                     uint8_t preset[kSynthSlotCount], uint8_t lastInstr[kTrackCount]) {
    (void)pattern;   // the sequencer's current buffer, which pattern() is
    chase::State st;
    chaseAt(songMode, songPos, lastStepInclusive, st);
    for (int s = 0; s < kSynthSlotCount; ++s) preset[s] = st.preset[s];
    for (int t = 0; t < kTrackCount; ++t) lastInstr[t] = st.last[t];
}

}  // namespace ot
