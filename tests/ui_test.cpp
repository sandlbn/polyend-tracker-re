// Whole-app tests: boot, pattern editing, clipboard, undo, save/load, screens.
#include <algorithm>
#include <cmath>

#include "test.h"
#include "ui_fakes.h"

#include "app/project_fields.h"
#include "formats/file_io.h"
#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;

static PatternScreen& pe(Harness& h) { return *static_cast<PatternScreen*>(h.app->ui.screen(kModPattern)); }

static void addWav(Harness& h, const char* path, uint32_t frames) {
    std::vector<int16_t> s(frames);
    for (uint32_t i = 0; i < frames; ++i) s[i] = int16_t((i % 100) * 300 - 15000);
    std::vector<uint8_t> out(frames * 2 + 64);
    size_t n = wavEncode(s.data(), frames, 1, out.data(), out.size());
    out.resize(n);
    h.fs.files[path] = out;
}

TEST(boot_creates_workspace) {
    Harness h;
    CHECK(h.fs.dirs.count("Workspace"));
    CHECK(h.fs.dirs.count("Workspace/patterns"));
    CHECK(h.fs.dirs.count("Workspace/instruments"));
    CHECK(h.fs.files.count("Workspace/project.mt"));
    CHECK(h.fs.files.count("Workspace/patterns/pattern_01.mtp"));
    CHECK(h.fs.files.count("OpenTracker/config.bin"));
    CHECK(h.app->ui.active() == kModPattern);
    h.run(50);
    CHECK(h.disp.swaps > 0);
    CHECK(h.disp.lastHasText("Pattern 1"));
    CHECK(std::strcmp(h.app->project.name, "New Project") == 0);
}

TEST(boot_resumes_existing_workspace) {
    Harness h;
    h.tap(btn::Rec);
    h.pad(0);
    h.app->autosave();
    // A second app on the same card loads the workspace.
    Harness h2(false);
    h2.fs = h.fs;
    h2.app->boot();
    CHECK(h2.app->pattern().tracks[0].steps[0].note == h.app->pattern().tracks[0].steps[0].note);
    CHECK(h2.app->pattern().tracks[0].steps[0].note >= 0);
}

TEST(pattern_note_entry_from_pads) {
    Harness h;
    App& a = *h.app;
    CHECK(!a.editMode);
    h.pad(5);   // not editing: audition only
    CHECK(a.pattern().tracks[0].steps[0].note == kNoteEmpty);
    h.tap(btn::Rec);
    CHECK(a.editMode);
    // Step jump 1 via F3 hold + pad 1.
    h.down(btn::F3);
    h.pad(1);
    h.up(btn::F3);
    CHECK(a.stepJump() == 1);
    const uint8_t n0 = a.padNote(36);
    h.pad(36);   // bottom-left pad = first note of the scale map
    CHECK(a.pattern().tracks[0].steps[0].note == int8_t(n0));
    CHECK(a.pattern().tracks[0].steps[0].instrument == 0);
    CHECK(pe(h).step == 1);
    h.pad(37);
    CHECK(a.pattern().tracks[0].steps[1].note == int8_t(a.padNote(37)));
    CHECK(pe(h).step == 2);
    // Default map: chromatic from root C3 + 12 (spec 05 §4.1).
    CHECK(n0 == 36);
    CHECK(a.padNote(37) == 37);
    // Encoder edits the note under the cursor.
    h.tap(btn::Up);
    h.tap(btn::Up);
    CHECK(pe(h).step == 0);
    h.enc(2);
    CHECK(a.pattern().tracks[0].steps[0].note == 38);
    // Delete clears and advances.
    h.tap(btn::Delete);
    CHECK(a.pattern().tracks[0].steps[0].note == kNoteEmpty);
    CHECK(pe(h).step == 1);
    // Instrument column: pad sets instrument.
    h.tap(btn::Instr);
    h.pad(4);
    CHECK(a.pattern().tracks[0].steps[1].instrument == 4);
    CHECK(a.currentInstrument() == 4);
}

TEST(copy_paste_and_selection) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Rec);
    h.down(btn::F3);
    h.pad(1);
    h.up(btn::F3);
    h.pad(36);
    h.pad(38);
    h.pad(40);
    // Select steps 0..2 on track 0: cursor to 0, Shift+Down x2.
    h.tap(btn::Up);
    h.tap(btn::Up);
    h.tap(btn::Up);
    CHECK(pe(h).step == 0);
    h.down(btn::Shift);
    h.tap(btn::Down);
    h.tap(btn::Down);
    h.up(btn::Shift);
    SelRect r = pe(h).target();
    CHECK(r.s0 == 0 && r.s1 == 2 && r.t0 == 0 && r.t1 == 0);
    h.tap(btn::Copy);
    CHECK(a.clipboardValid());
    // Move to track 1 step 8 and paste.
    h.tap(btn::Right);
    for (int i = 0; i < 6; ++i) h.tap(btn::Down);
    CHECK(pe(h).track == 1);
    const int dst = pe(h).step;
    h.down(btn::Shift);
    h.tap(btn::Copy);
    h.up(btn::Shift);
    CHECK(a.pattern().tracks[1].steps[dst].note == 36);
    CHECK(a.pattern().tracks[1].steps[dst + 1].note == 38);
    CHECK(a.pattern().tracks[1].steps[dst + 2].note == 40);
    // Cursor advanced by rows-1 + jump.
    CHECK(pe(h).step == dst + 3);
}

TEST(undo_redo) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Rec);
    h.pad(36);
    CHECK(a.pattern().tracks[0].steps[0].note == 36);
    h.tap(btn::F6);   // Undo on release
    CHECK(a.pattern().tracks[0].steps[0].note == kNoteEmpty);
    h.tap(btn::F7);   // Redo
    CHECK(a.pattern().tracks[0].steps[0].note == 36);
    // Encoder while F6 is held: - undo, + redo; release does nothing extra.
    h.down(btn::F6);
    h.enc(-1);
    CHECK(a.pattern().tracks[0].steps[0].note == kNoteEmpty);
    h.enc(1);
    CHECK(a.pattern().tracks[0].steps[0].note == 36);
    h.up(btn::F6);
    CHECK(a.pattern().tracks[0].steps[0].note == 36);
    // Nothing more to redo -> notification, no change.
    h.tap(btn::F7);
    CHECK(a.pattern().tracks[0].steps[0].note == 36);
    CHECK(h.disp.lastHasText("No redo data."));
}

TEST(pattern_length_and_ops) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Rec);
    h.down(btn::F2);
    h.pad(3);   // length 16
    h.up(btn::F2);
    CHECK(pops::length(a.pattern()) == 16);
    h.pad(36);
    h.tap(btn::F8);   // page 1
    h.tap(btn::F3);   // duplicate
    CHECK(pops::length(a.pattern()) == 32);
    CHECK(a.pattern().tracks[0].steps[16].note == 36);
    h.tap(btn::F5);   // shrink
    CHECK(pops::length(a.pattern()) == 16);
    h.tap(btn::F4);   // expand
    CHECK(pops::length(a.pattern()) == 32);
    CHECK(a.pattern().tracks[0].steps[0].note == 36);
    CHECK(a.pattern().tracks[0].steps[16].note == 36);
}

TEST(pattern_switch_and_mute) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Rec);
    h.pad(36);
    h.down(btn::F1);
    h.enc(1);
    h.up(btn::F1);
    CHECK(a.patternNumber() == 2);
    CHECK(a.pattern().tracks[0].steps[0].note == kNoteEmpty);
    h.down(btn::F1);
    h.pad(0);
    h.up(btn::F1);
    CHECK(a.patternNumber() == 1);
    CHECK(a.pattern().tracks[0].steps[0].note == 36);
    // Shift + F3 toggles mute of track 3.
    h.down(btn::Shift);
    h.tap(btn::F3);
    h.up(btn::Shift);
    CHECK(a.trackMuted(2));
    CHECK(a.project.u8(pf::kMute0 + 2) == 1);
}

TEST(playback_drives_sequencer) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Play);
    CHECK(a.seq.isPlaying());
    h.audio.pump(400);   // ~1.16 s at 130 BPM ~ 10 steps
    h.run(30);
    CHECK(a.seq.step(0) > 3);
    CHECK(pe(h).step == a.seq.step(0));
    h.tap(btn::Play);
    CHECK(!a.seq.isPlaying());
}

TEST(save_load_round_trip) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Rec);
    h.pad(36);
    h.tap(btn::File);
    CHECK(a.ui.active() == kModFile);
    h.tap(btn::F5);   // Save As -> Save browser
    h.tap(btn::F8);   // Save -> keyboard
    CHECK(a.ui.kb.isOpen());
    for (int i = 0; i < 12; ++i) h.tap(btn::Delete);   // clear "New Project"
    h.type("song1");
    CHECK(std::strcmp(a.ui.kb.text(), "song1") == 0);
    h.tap(btn::F8);
    CHECK(!a.ui.kb.isOpen());
    CHECK(h.fs.files.count("Projects/song1/project.mt"));
    CHECK(h.fs.files.count("Projects/song1/patterns/pattern_01.mtp"));
    CHECK(!a.unsavedChanges());
    CHECK(std::strcmp(a.project.name, "song1") == 0);
    // New project clears the pattern.
    h.tap(btn::F1);
    CHECK(a.pattern().tracks[0].steps[0].note == kNoteEmpty);
    // Open it again from the list.
    h.tap(btn::F2);
    FileScreen& fsn = *static_cast<FileScreen*>(a.ui.screen(kModFile));
    CHECK(fsn.count == 1);
    h.tap(btn::F8);
    CHECK(a.ui.active() == kModPattern);
    CHECK(a.pattern().tracks[0].steps[0].note == 36);
    CHECK(std::strcmp(a.project.name, "song1") == 0);
    CHECK(h.fs.files.count("Projects/recents"));
}

TEST(autosave_writes_workspace) {
    Harness h;
    App& a = *h.app;
    auto before = h.fs.files["Workspace/patterns/pattern_01.mtp"];
    h.tap(btn::Rec);
    h.pad(36);
    h.run(11000);
    CHECK(h.fs.files["Workspace/patterns/pattern_01.mtp"] != before);
    Pattern p;
    loadPatternFile(h.fs, "Workspace/patterns/pattern_01.mtp", p);
    CHECK(p.tracks[0].steps[0].note == 36);
    (void)a;
}

TEST(screen_switching_and_dl_budget) {
    Harness h;
    App& a = *h.app;
    const uint8_t keys[] = {btn::File, btn::SampleLoader, btn::SamplePlayback, btn::Song, btn::InstrParams,
                            btn::SampleEditor, btn::Config, btn::SampleRecorder, btn::Performance, btn::Master,
                            btn::Pattern};
    const int mods[] = {kModFile, kModLoader, kModPlayback, kModSong, kModInstrument, kModEditor,
                        kModConfig, kModRecorder, kModPerformance, kModMaster, kModPattern};
    for (size_t i = 0; i < sizeof(keys); ++i) {
        h.tap(keys[i]);
        h.run(40);
        CHECK(a.ui.active() == mods[i]);
        CHECK(a.ui.lastDlWords() < 2048);
    }
    // Master key cycles pages; Instr key toggles instrument pages.
    h.tap(btn::Master);
    MixerScreen& mx = *static_cast<MixerScreen*>(a.ui.screen(kModMaster));
    CHECK(mx.page == 0);
    h.tap(btn::Master);
    CHECK(mx.page == 1);
    h.tap(btn::Master);
    CHECK(mx.page == 2);
    h.tap(btn::Master);
    CHECK(mx.page == 0);
}

TEST(song_screen_edit) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Song);
    SongScreen& s = *static_cast<SongScreen*>(a.ui.screen(kModSong));
    CHECK(s.length() == 1);
    h.tap(btn::Rec);
    h.tap(btn::F4);   // add slot (same pattern)
    CHECK(s.length() == 2);
    h.tap(btn::F7);   // pattern +1
    CHECK(a.project.playlist[1] == 2);
    h.down(btn::Shift);
    h.tap(btn::F4);   // add empty-pattern slot
    h.up(btn::Shift);
    CHECK(s.length() == 3);
    CHECK(a.project.playlist[2] == 1 || a.project.playlist[2] >= 2);
    h.tap(btn::F5);
    CHECK(s.length() == 2);
    // Tempo: focus tempo with F8, encoder +5 BPM.
    h.tap(btn::F8);
    h.enc(5);
    CHECK(int(a.project.tempo()) == 135);
}

TEST(instrument_params_edit) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::InstrParams);
    h.tap(btn::F1);
    h.enc(10);
    CHECK(a.instruments[0].volume == 60);
    h.tap(btn::Delete);
    CHECK(a.instruments[0].volume == 50);
    h.tap(btn::F3);
    h.enc(10);   // +-10 -> octave
    CHECK(a.instruments[0].tune == 12);
}

TEST(sample_loader_import) {
    Harness h;
    App& a = *h.app;
    h.fs.mkdir("Samples");
    addWav(h, "Samples/kick.wav", 4410);
    addWav(h, "Samples/snare.wav", 2205);
    h.tap(btn::SampleLoader);
    LoaderScreen& l = *static_cast<LoaderScreen*>(a.ui.screen(kModLoader));
    // Root lists folders first.
    int idx = -1;
    for (int i = 0; i < l.count; ++i)
        if (!std::strcmp(l.entries[i], "Samples")) idx = i;
    CHECK(idx >= 0);
    while (l.sdCursor < idx) h.tap(btn::Down);
    h.tap(btn::F1);   // Enter folder
    CHECK(std::strcmp(l.dir, "Samples") == 0);
    CHECK(l.count == 3);   // "..", kick, snare
    h.tap(btn::Down);
    h.tap(btn::F1);   // add kick into slot 1
    CHECK(a.instrPresent[0]);
    CHECK(a.bank.frames(0) == 4410);
    CHECK(std::strcmp(a.instruments[0].name, "kick") == 0);
    // Audition through the engine produces sound.
    a.previewOn(0, 0, 60, 127);   // pad 0 -> track voice
    CHECK(h.audio.pump(4) > 1000);
    a.previewAllOff();
    // Autosave writes the instrument file.
    a.autosave();
    CHECK(h.fs.files.count("Workspace/instruments/instrument_01.pti"));
    // Delete via the instrument list.
    h.tap(btn::Right);
    h.tap(btn::Delete);
    CHECK(a.ui.askOpen());
    h.tap(btn::F8);
    CHECK(!a.instrPresent[0]);
    a.autosave();
    CHECK(!h.fs.files.count("Workspace/instruments/instrument_01.pti"));
}

TEST(config_apply_and_firmware_update) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Config);
    ConfigScreen& c = *static_cast<ConfigScreen*>(a.ui.screen(kModConfig));
    // General > Pads preview.
    h.tap(btn::Down);
    CHECK(c.category == 1);
    h.tap(btn::Right);
    for (int i = 0; i < 6; ++i) h.tap(btn::Down);   // after 2.x Export theme
    h.tap(btn::F8);
    CHECK(c.editing);
    h.tap(btn::Up);   // pattern(2) -> track(1)
    CHECK(a.cfg.padsPreview == 2);   // not stored until Apply
    h.tap(btn::F8);
    CHECK(a.cfg.padsPreview == 1);
    h.tap(btn::Pattern);   // leaving Config saves
    Config chk;
    CHECK(chk.load(h.fs));
    CHECK(chk.padsPreview == 1);
    // Firmware update: list /Firmware, select, confirm.
    h.fs.mkdir("Firmware");
    h.fs.files["Firmware/PolyendTracker_1.9.2.ptf"] = {1, 2, 3};
    h.fs.files["Firmware/readme.txt"] = {1};
    h.tap(btn::Config);
    for (int i = 0; i < 7 && c.category != 5; ++i) h.tap(btn::Down);   // Firmware (Info is last)
    CHECK(c.category == 5);
    h.tap(btn::Right);
    h.tap(btn::F8);
    CHECK(c.editing);
    h.tap(btn::F8);
    CHECK(a.ui.askOpen());
    h.tap(btn::F8);
    CHECK(Harness::fwRequested == "PolyendTracker_1.9.2.ptf");
}

TEST(export_pattern_wav) {
    Harness h;
    App& a = *h.app;
    addWav(h, "click.wav", 2000);
    CHECK(a.importSample("click.wav", 0));
    h.tap(btn::Rec);
    h.pad(36);
    h.tap(btn::File);
    h.tap(btn::F6);
    h.tap(btn::F3);   // export pattern
    std::string found;
    for (auto& f : h.fs.files)
        if (f.first.compare(0, 7, "Export/") == 0) found = f.first;
    CHECK(!found.empty());
    WavInfo info;
    const auto& w = h.fs.files[found];
    CHECK(wavParseHeader(w.data(), w.size(), info));
    CHECK(info.channels == 2);
    // 32 steps at 130 BPM ~ 3.7 s plus tail.
    CHECK(info.dataSize / 4 > 44100 * 3);
    int peak = 0;
    for (size_t i = 44; i + 1 < w.size(); i += 2) {
        int v = std::abs(int16_t(w[i] | (w[i + 1] << 8)));
        if (v > peak) peak = v;
    }
    CHECK(peak > 1000);
    CHECK(info.sampleRate == 44100);   // [I] resampled (stock 44117, spec 04 §6.1)
    CHECK(!a.rendering());
}

TEST(engine_pad_audition_and_playback) {
    Harness h;
    App& a = *h.app;
    addWav(h, "tone.wav", 20000);
    CHECK(a.importSample("tone.wav", 0));
    CHECK(a.engine.bank().loaded(0));
    // Pad press in the pattern editor auditions on a track voice, starting at
    // the cursor track (spec 07 Q7); the ninth player stays silent.
    audio::Voice& pv = a.engine.voice(0);
    CHECK(!pv.active());
    h.push(InputEvent::kPadDown, 36);
    CHECK(pv.active());
    CHECK(!a.engine.metronomeActive());
    CHECK(h.audio.pump(8) > 1000);
    h.push(InputEvent::kPadUp, 36);
    for (int i = 0; i < 4000 && pv.active(); ++i) h.audio.pump(1);   // envelope release
    CHECK(!pv.active());
    CHECK(h.audio.pump(2) == 0);
    // Entered note plays on track voice 0 through the mixer.
    h.tap(btn::Rec);
    h.pad(36);
    h.tap(btn::Rec);
    h.tap(btn::Play);
    int peak = h.audio.pump(4);
    CHECK(a.engine.voice(0).active());
    CHECK(peak > 1000 || h.audio.pump(16) > 1000);
    CHECK(a.engine.trackPeak(0) > 0.0f);
    // Track mute reaches the engine.
    a.setTrackMute(0, true);
    h.audio.pump(1);
    CHECK(a.engine.settings().trackMute[0]);
    a.setTrackMute(0, false);
    h.tap(btn::Play);
    // Master volume edit in the Mixer reaches the engine.
    h.tap(btn::Master);
    h.enc(-11);
    h.audio.pump(1);
    CHECK(a.cfg.masterVolume == 70);
    CHECK(a.engine.settings().masterVolume == 70);
    // Reverb size edit (Master > Reverb > Size).
    h.tap(btn::F2);
    h.enc(10);
    h.audio.pump(1);
    CHECK(std::fabs(a.engine.settings().reverbSize - a.project.f32(pf::kReverbSize)) < 1e-6f);
    CHECK(std::fabs(a.engine.settings().reverbSize - 0.6f) < 1e-3f);
}

TEST(export_pattern_stems) {
    Harness h;
    App& a = *h.app;
    addWav(h, "click.wav", 2000);
    CHECK(a.importSample("click.wav", 0));
    h.tap(btn::Rec);
    h.pad(36);
    h.tap(btn::File);
    h.tap(btn::F6);
    h.tap(btn::F4);   // export pattern stems
    static const char* kStems[14] = {"track1", "track2", "track3", "track4", "track5", "track6", "track7",
                                     "track8", "reverb", "delay",  "master", "synth_1", "synth_2", "synth_3"};   // Mini synth_%d (spec 21 §2)
    std::string dir;
    for (auto& f : h.fs.files)
        if (f.first.find("_S0") != std::string::npos) dir = f.first.substr(0, f.first.rfind('/'));
    CHECK(!dir.empty());
    int count = 0;
    for (auto& f : h.fs.files)
        if (f.first.compare(0, dir.size() + 1, dir + "/") == 0) ++count;
    CHECK(count == 14);
    for (const char* n : kStems) {
        const std::string p = dir + "/" + n + ".wav";
        CHECK(h.fs.files.count(p));
        WavInfo info;
        const auto& w = h.fs.files[p];
        CHECK(wavParseHeader(w.data(), w.size(), info));
        CHECK(info.channels == 2);
        CHECK(info.sampleRate == 44100);   // [I] resampled (stock 44117, spec 04 §6.1)
        CHECK(info.dataSize / 4 > 44100 * 3);
    }
    auto peakOf = [&](const char* n) {
        const auto& w = h.fs.files[dir + "/" + n + ".wav"];
        int peak = 0;
        for (size_t i = 44; i + 1 < w.size(); i += 2) peak = std::max(peak, std::abs(int16_t(w[i] | (w[i + 1] << 8))));
        return peak;
    };
    CHECK(peakOf("track1") > 1000);
    CHECK(peakOf("master") > 1000);
    CHECK(peakOf("track2") == 0);
    CHECK(!a.rendering());
}

TEST(keyboard_and_pattern_name) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::F8);
    h.tap(btn::F8);   // page 2
    h.tap(btn::F1);
    CHECK(a.ui.kb.isOpen());
    h.type("intro");
    h.tap(btn::F8);
    CHECK(std::strcmp(a.names.get(1), "intro") == 0);
    h.run(30);
    CHECK(h.disp.lastHasText("intro"));
}

TEST(pad_leds_follow_edit_state) {
    Harness h;
    h.tap(btn::Rec);
    h.tap(btn::Instr);
    h.pad(7);   // writes instrument 8 and advances (spec 16 §A.5)
    CHECK_EQ(pe(h).step, 1);
    h.tap(btn::Up);
    h.run(30);
    CHECK(h.leds.level[7] > 0);
}


// spec 07 Q7: pad audition on the track voices.
TEST(audition_track_voice_allocation) {
    Harness h;
    App& a = *h.app;
    addWav(h, "tone.wav", 20000);
    CHECK(a.importSample("tone.wav", 0));
    a.setCurrentInstrument(0);
    a.editorTrack = 2;
    // One voice per held pad, starting at the cursor track, wrapping; sample
    // instruments only on tracks 0..7, so at most 8.
    const int expect[8] = {2, 3, 4, 5, 6, 7, 0, 1};
    for (int p = 0; p < 8; ++p) CHECK_EQ(a.previewOn(uint16_t(p), 0, 60, 127), expect[p]);
    CHECK_EQ(a.previewOn(8, 0, 60, 127), -1);
    for (int t = 0; t < 8; ++t) CHECK(a.engine.voice(t).active());
    CHECK(!a.engine.metronomeActive());   // not the ninth player
    // Release: normal envelope release, the track becomes free again.
    a.previewOff(3);
    CHECK_EQ(a.auditionTrack(3), -1);
    CHECK(a.engine.voice(5).envelopeLevel() > 0.0f || a.engine.voice(5).active());
    CHECK_EQ(a.previewOn(9, 0, 60, 127), 5);
    a.previewAllOff();
    // Muted tracks are skipped.
    a.setTrackMute(2, true);
    CHECK_EQ(a.previewOn(0, 0, 60, 127), 3);
    a.setTrackMute(2, false);
    a.previewAllOff();
    // Sample Loader preview: fixed id 0x60, at most one voice.
    CHECK_EQ(a.previewOn(App::kAuditionLoader, 0, 60, 127), 2);
    CHECK_EQ(a.previewOn(App::kAuditionLoader, 0, 60, 127), 2);
    CHECK_EQ(a.auditionTrack(App::kAuditionLoader), 2);
    a.previewOff(App::kAuditionLoader);
    CHECK_EQ(a.auditionTrack(App::kAuditionLoader), -1);
    // Forced track (pattern-editor note entry in edit mode).
    CHECK_EQ(a.previewOn(5, 0, 60, 127, 6), 6);
    a.previewAllOff();
}

// USER_REPORTS_R2 §1b: on Sample Playback each Beat Slice pad plays its own
// slice whether the audition voice is idle or still sounding.
TEST(sample_playback_pads_play_their_own_slice) {
    Harness h;
    App& a = *h.app;
    addWav(h, "loop.wav", 40000);
    CHECK(a.importSample("loop.wav", 0));
    a.setCurrentInstrument(0);
    Instrument& in = a.instruments[0];
    in.playMode = uint8_t(PlayMode::BeatSlice);
    in.sliceCount = 4;
    for (int k = 0; k < 4; ++k) in.slices[k] = uint16_t(k * 16384);
    in.selectedSlice = 3;
    h.tap(btn::SamplePlayback);
    const uint32_t frames = a.engine.bank().frames(0);
    auto sliceOf = [&](int32_t pos) { return pos < 0 ? -1 : int(int64_t(pos) * 4 / int64_t(frames)); };
    auto head = [&](uint8_t pad) {
        const int t = a.auditionTrack(pad);
        return t < 0 ? -1 : sliceOf(a.engine.voice(t).playhead());
    };
    auto drain = [&] {
        for (int i = 0; i < 4000; ++i) {
            bool any = false;
            for (int t = 0; t < 8; ++t) any = any || a.engine.voice(t).active();
            if (!any) break;
            h.audio.pump(1);
        }
    };
    const uint8_t order[] = {3, 0, 2};
    for (uint8_t p : order) {   // idle voice between presses
        drain();
        h.push(InputEvent::kPadDown, p);
        CHECK_EQ(head(p), int(p));
        CHECK_EQ(int(in.selectedSlice), int(p));
        h.audio.pump(2);
        CHECK_EQ(head(p), int(p));
        h.push(InputEvent::kPadUp, p);
    }
    // Sounding voice: pad 2 is still releasing on the same track when pad 1 hits.
    h.push(InputEvent::kPadDown, 1);
    h.audio.pump(8);   // forced fade, then the deferred start
    CHECK_EQ(head(1), 1);
    CHECK_EQ(int(in.selectedSlice), 1);
    h.push(InputEvent::kPadUp, 1);
}

// USER_REPORTS_R2 §1c: Sample Playback draws the auditioned voice's play
// position (spec 05 §6, redraw every >= 5 ms) in every play mode.
TEST(sample_playback_playhead) {
    Harness h;
    App& a = *h.app;
    addWav(h, "long.wav", 40000);
    CHECK(a.importSample("long.wav", 0));
    a.setCurrentInstrument(0);
    Instrument& in = a.instruments[0];
    h.tap(btn::SamplePlayback);
    auto* s = static_cast<PlaybackScreen*>(a.ui.screen(kModPlayback));
    const uint32_t len = a.bank.frames(0);
    auto expectX = [&](int32_t pos) {
        const uint32_t m = uint32_t(uint64_t(pos) * 65535u / len);
        return int(float(m) / 65535.0f * 599.0f + 0.5f);   // full view, kWaveX 0, 600 px
    };
    CHECK_EQ(s->drawnHeadX(), -1);
    h.push(InputEvent::kPadDown, 0);
    const int t = a.auditionTrack(0);
    CHECK(t >= 0);
    h.audio.pump(1);
    h.run(40);   // >= 16 ms frame limit
    const int32_t p1 = a.engine.voicePlayhead(t);
    CHECK(p1 >= 0);
    CHECK_EQ(s->drawnHeadX(), expectX(p1));
    h.audio.pump(60);   // ~7700 frames later
    h.run(40);   // >= 16 ms frame limit
    const int32_t p2 = a.engine.voicePlayhead(t);
    CHECK(p2 > p1 + 5000);
    CHECK_EQ(s->drawnHeadX(), expectX(p2));
    CHECK(s->drawnHeadX() > expectX(p1));
    h.push(InputEvent::kPadUp, 0);
    for (int i = 0; i < 4000 && a.engine.voice(t).active(); ++i) h.audio.pump(1);
    h.audio.pump(1);
    h.run(40);   // >= 16 ms frame limit
    CHECK_EQ(a.engine.voicePlayhead(t), -1);
    CHECK_EQ(s->drawnHeadX(), -1);
    // Beat Slice: the line starts at the pad's slice.
    in.playMode = uint8_t(PlayMode::BeatSlice);
    in.sliceCount = 4;
    for (int k = 0; k < 4; ++k) in.slices[k] = uint16_t(k * 16384);
    h.tap(btn::SamplePlayback);
    h.run(20);
    h.push(InputEvent::kPadDown, 2);
    h.audio.pump(1);
    h.run(40);   // >= 16 ms frame limit
    const int x = s->drawnHeadX();
    CHECK(x >= 300 && x < 320);   // slice 2 starts at x ~300
    h.push(InputEvent::kPadUp, 2);
}

// USER_REPORTS_R2 §4: while a song plays, Up/Down browse the song list; the
// cursor re-joins the playing slot only when that slot changes.
TEST(song_cursor_free_between_slot_changes) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Song);
    SongScreen& s = *static_cast<SongScreen*>(a.ui.screen(kModSong));
    h.tap(btn::Rec);
    h.tap(btn::F4);
    h.tap(btn::F4);
    h.tap(btn::Rec);
    CHECK_EQ(s.length(), 3);
    CHECK(!s.edit);
    s.cursor = 0;
    h.tap(btn::Play);
    CHECK(a.seq.isPlaying());
    CHECK(a.seq.isSongMode());
    auto playTo = [&](int slot) {
        for (int i = 0; i < 20000 && a.seq.songPosition() != slot; ++i) h.audio.pump(1);
        CHECK_EQ(a.seq.songPosition(), slot);
        h.run(150);
    };
    h.run(150);
    CHECK_EQ(s.cursor, 0);
    playTo(1);
    CHECK_EQ(s.cursor, 1);   // follows the new slot
    h.tap(btn::Down);
    CHECK_EQ(s.cursor, 2);
    h.run(150);
    h.audio.pump(10);
    h.run(150);
    CHECK_EQ(s.cursor, 2);   // stays while slot 1 keeps playing
    h.tap(btn::Up);
    h.tap(btn::Up);
    h.run(300);
    CHECK_EQ(s.cursor, 0);
    CHECK_EQ(a.seq.songPosition(), 1);
    playTo(2);
    CHECK_EQ(s.cursor, 2);   // the next slot change pulls it back
    CHECK(a.seq.isPlaying());
    h.tap(btn::Play);
}

TEST(output_unmuted_only_after_the_startup_load) {
    // USER_REPORTS_R3 R3-4 ("big crack in the headphones during start"): stock
    // keeps the audio mute line (pin 21) low through start-up and drives it
    // high only when the start-up project has loaded (spec 05 §1.9 state 3 ->
    // 4, spec 01 §5.4); we unmuted as soon as audio started.
    Harness h(false);
    CHECK(h.audio.started);
    CHECK(!h.audio.outputOn());   // audio running, output still muted
    h.app->boot();
    h.run(20);
    CHECK(!h.audio.outputOn());   // #63: not yet, the output settles first
    h.run(400);
    CHECK(!h.audio.outputOn());
    h.run(200);
    CHECK(h.audio.outputOn());    // unmuted 500 ms after the load
}

TEST(output_stays_on_through_later_project_loads) {
    // #63: a click when "Opening project" appears and another when it
    // finishes. Stock mutes pin 21 around the export render (spec 04 §6.1),
    // not around project loads; after the start-up unmute the line stays high.
    Harness h;
    h.run(600);
    CHECK(h.audio.outputOn());
    const size_t n = h.audio.outputLog.size();
    h.app->autosave();
    CHECK(h.app->startLoadWorkspace());   // a later load (Open goes the same way)
    for (int i = 0; i < 400 && h.app->jobBusy(); ++i) h.run(10);
    CHECK(!h.app->jobBusy());
    CHECK_EQ(h.audio.outputLog.size(), n);   // the mute line never toggled
    CHECK(h.audio.outputOn());
}
