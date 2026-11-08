// [I] Songs 23-30 "OT Pool N - <synth>" (shared SRAM_L pool measurement,
// profiles/pooltest.toml). Included by testsongs.cpp inside its anonymous
// namespace (it uses Gen from there).
//
// One synth in Synth 1 with its Init patch (the patch the bench-m4 module
// benches also run, so tools/synth_hot.py's hot lists cover it), polyphony 1,
// re-struck every 4 steps so one voice always sounds; the telemetry's
// cyc_per_voice is then that synth's voice. PERC hits six drums at once.
// No samples, no sends. 120 BPM, one 16-step pattern. Synths 2 and 3 are
// on the same engine (silent): with the shared fast pool (OT_HOT_POOL) the
// measured synth then has the fast block to itself.

namespace pooltest {

constexpr int kSynth1 = kSynthBase + 0;

bool poolSong(Gen& g, int32_t engineId) {
    g.tempo = 120.0f;
    if (!g.begin(false)) return false;
    for (int s = 0; s < 3; ++s) g.app.setSynthEngine(s, engineId);
    SynthSlotRecord r = g.app.synthSlot(0);
    r.polyphony = 1;
    r.delaySend = 0.0f;
    r.reverbSend = 0.0f;
    g.app.setSynthSlotRecord(0, r);
    trackNames(g, {"Synth 1"});
    g.beginPattern(1, 16, "Held note");
    for (int s = 0; s < 16; s += 4) {
        if (engineId == kSynthEnginePerc) {
            const int drums[6] = {60, 62, 64, 69, 71, 48};   // kick snare hat cymbal perc tom
            for (int k = 0; k < 6; ++k) g.note(k, s, drums[k], kSynth1);
        } else {
            g.note(0, s, 60, kSynth1);
        }
    }
    g.endPattern();
    g.playlist({1});
    return g.ok;
}

}  // namespace pooltest
