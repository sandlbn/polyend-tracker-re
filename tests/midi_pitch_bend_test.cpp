// 2.x: MIDI input pitch bend (stock ignores it, spec 17 §1.2). Live notes
// only, +-2 semitones [I]: sample instruments bend their track voice, synth
// slots their live voices, MIDI instruments get the bend forwarded on their
// channel. Sequenced notes and the pattern are untouched.
#include <cmath>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "audio/synth_bank.h"
#include "formats/wav.h"

using namespace ot;

static void addWav(Harness& h, const char* path, uint32_t frames) {
    std::vector<int16_t> s(frames);
    for (uint32_t i = 0; i < frames; ++i) s[i] = int16_t((i % 100) * 300 - 15000);
    std::vector<uint8_t> out(frames * 2 + 64);
    size_t n = wavEncode(s.data(), frames, 1, out.data(), out.size());
    out.resize(n);
    h.fs.files[path] = out;
}

static void send(Harness& h, std::initializer_list<uint8_t> b) {
    h.midi.in.insert(h.midi.in.end(), b);
    h.run(5);
    h.audio.pump(2);
}
static void noteOn(Harness& h, uint8_t n, uint8_t st = 0x90) { send(h, {st, n, 100}); }
static void noteOff(Harness& h, uint8_t n, uint8_t st = 0x80) { send(h, {st, n, 0}); }
// 14-bit bend: 0 = -2 semitones, 8192 centre, 16383 = +2 (less one step).
static void bend(Harness& h, int v, uint8_t st = 0xE0) { send(h, {st, uint8_t(v & 127), uint8_t(v >> 7)}); }

static bool near(float a, float b) { return std::fabs(a - b) <= std::fabs(b) * 0.002f + 1e-4f; }

static int sampleTrack(Harness& h, uint8_t note) {
    App& a = *h.app;
    addWav(h, "tone.wav", 40000);
    CHECK(a.importSample("tone.wav", 0));
    a.setCurrentInstrument(0);
    noteOn(h, note);
    return a.auditionTrack(uint16_t(App::kAuditionMidiIn + note));
}

TEST(pitch_bend_bends_a_live_sample_note) {
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    const int t = sampleTrack(h, 60);
    CHECK(t >= 0);
    const float r0 = a.engine.voice(t).rate();
    CHECK(r0 > 0.0f);
    bend(h, 16383);
    CHECK(near(a.engine.voice(t).rate(), r0 * std::exp2(16383.0f / 8192.0f * 2.0f / 12.0f - 2.0f / 12.0f)));
    bend(h, 0);
    CHECK(near(a.engine.voice(t).rate(), r0 * std::exp2(-2.0f / 12.0f)));
    bend(h, 8192);
    CHECK(near(a.engine.voice(t).rate(), r0));
    // A note played while the wheel is held starts bent.
    bend(h, 0);
    noteOn(h, 64);
    const int t2 = a.auditionTrack(uint16_t(App::kAuditionMidiIn + 64));
    CHECK(t2 >= 0 && t2 != t);
    noteOff(h, 64);
    noteOn(h, 60);   // a second 60 on t2: same pitch as the bent first one
    const int t3 = a.auditionTrack(uint16_t(App::kAuditionMidiIn + 60), t2);
    CHECK_EQ(t3, t2);
    CHECK(near(a.engine.voice(t3).rate(), a.engine.voice(t).rate()));
    noteOff(h, 60);
    noteOff(h, 60);
    // A sequenced note on that track does not keep the live bend.
    VoiceNoteOn n;
    n.instrument = 0;
    n.note = 60;
    a.engine.voiceNoteOn(uint8_t(t), n);
    h.audio.pump(2);
    CHECK(near(a.engine.voice(t).rate(), r0));
}

TEST(pitch_bend_follows_the_notes_in_channel) {
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    a.cfg.notesInChannel = 2;   // channel 2 only
    const int t = sampleTrack(h, 60) ;
    CHECK_EQ(t, -1);            // channel 1 note ignored
    noteOn(h, 60, 0x91);
    const int t1 = a.auditionTrack(uint16_t(App::kAuditionMidiIn + 60));
    CHECK(t1 >= 0);
    const float r0 = a.engine.voice(t1).rate();
    bend(h, 0, 0xE0);           // other channel: ignored
    CHECK(near(a.engine.voice(t1).rate(), r0));
    bend(h, 0, 0xE1);
    CHECK(near(a.engine.voice(t1).rate(), r0 * std::exp2(-2.0f / 12.0f)));
}

TEST(pitch_bend_forwarded_to_midi_instruments) {
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    a.cfg.midiOut = 1;
    a.setCurrentInstrument(uint8_t(kFirstMidiInstrument + 2));   // M3: channel 3
    noteOn(h, 60);
    h.midi.sent.clear();
    bend(h, 0x1234);
    const std::vector<uint8_t> want = {0xE2, 0x34, 0x24};
    CHECK(h.midi.sent == want);
    noteOff(h, 60);
    // The wheel moved with no key held still reaches the instrument.
    h.midi.sent.clear();
    bend(h, 8192);
    const std::vector<uint8_t> centre = {0xE2, 0x00, 0x40};
    CHECK(h.midi.sent == centre);
}

TEST(pitch_bend_bends_live_synth_voices) {
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    a.setCurrentInstrument(uint8_t(kFirstSynthInstrument + 1));
    noteOn(h, 60);
    const audio::SynthVoice* v = a.engine.synth().voiceOfTrack(audio::kSynthLiveTrack0);
    CHECK(v != nullptr);
    if (!v) return;
    CHECK(near(v->bend, 0.0f));
    bend(h, 0);
    CHECK(near(v->bend, -2.0f));
    bend(h, 8192);
    CHECK(near(v->bend, 0.0f));
    bend(h, 0);
    noteOn(h, 64);   // starts bent
    const audio::SynthVoice* v2 = a.engine.synth().voiceOfTrack(audio::kSynthLiveTrack0 + 1);
    CHECK(v2 != nullptr);
    if (v2) CHECK(near(v2->bend, -2.0f));
    noteOff(h, 60);
    noteOff(h, 64);
    bend(h, 8192);
}
