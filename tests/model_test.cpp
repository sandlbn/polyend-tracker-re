// Data model tests (spec 02 §3.3, §4.2, §4.3, §6.2).
#include "test.h"

#include "model/constants.h"
#include "model/fx.h"
#include "model/instrument.h"
#include "model/pattern.h"
#include "model/project.h"

using namespace ot;

TEST(constants) {
    CHECK_EQ(kTrackCount, 16);
    CHECK_EQ(kStockTrackCount, 12);
    CHECK_EQ(kMidiTrackStart, 8);
    CHECK_EQ(kStepCount, 128);
    CHECK_EQ(kInstrumentCount, 64);
    CHECK_EQ(int(kNoteEmpty), -1);
    CHECK_EQ(int(kNoteFade), -2);
    CHECK_EQ(int(kNoteCut), -3);
    CHECK_EQ(int(kNoteOff), -4);
    CHECK(isSampleInstrument(47));
    CHECK(!isSampleInstrument(48));
    CHECK(isMidiInstrument(48));
    CHECK(isMidiInstrument(63));
    CHECK(!isMidiInstrument(64));
    CHECK_EQ(kSampleBankBytes, 11796480u);
}

TEST(fx_table) {
    CHECK_EQ(kFxCount, 43);
    CHECK_STR(fxInfo(FxId::None).name, "None");
    CHECK_STR(fxInfo(FxId::Tempo).name, "Tempo");
    CHECK_EQ(fxInfo(FxId::Tempo).minValue, 4);
    CHECK_EQ(fxInfo(FxId::Tempo).maxValue, 200);
    CHECK_EQ(fxInfo(FxId::Tempo).defaultValue, 60);
    CHECK_EQ(fxInfo(FxId::Swing).defaultValue, 50);
    CHECK_EQ(fxInfo(FxId::Roll).defaultValue, 1);
    CHECK_EQ(fxInfo(FxId::MicroTune).maxValue, 198);
    CHECK_EQ(fxInfo(FxId::BitDepth).minValue, 1);
    CHECK_EQ(fxInfo(FxId::BitDepth).maxValue, 16);
    CHECK_STR(fxInfo(FxId::SlideDown).name, "Slide Down");
    CHECK_EQ(fxInfo(FxId::SlideDown).symbol, 'J');
    CHECK_EQ(fxInfo(FxId::Volume).symbol, 'V');
    CHECK_STR(fxInfo(uint8_t(200)).name, "None");
    CHECK(fxIdValid(42));
    CHECK(!fxIdValid(43));
    // Menu order is a permutation of 0..42.
    bool seen[kFxCount] = {};
    for (int i = 0; i < kFxCount; ++i) {
        CHECK(kFxMenuOrder[i] < kFxCount);
        seen[kFxMenuOrder[i]] = true;
    }
    for (bool s : seen) CHECK(s);
    CHECK_EQ(kFxMenuOrder[2], 18);
    CHECK_EQ(kFxMenuOrder[42], 13);
}

TEST(step_clear_keeps_values) {
    Step s;
    s.note = 60;
    s.instrument = 5;
    s.fx[0] = {18, 77};
    s.fx[1] = {31, 12};
    s.clear();
    CHECK_EQ(s.note, kNoteEmpty);
    CHECK_EQ(s.instrument, 0);
    CHECK_EQ(s.fx[0].type, 0);
    CHECK_EQ(s.fx[1].type, 0);
    CHECK_EQ(s.fx[0].value, 77);  // firmware clear leaves values (spec 02 §4.2)
    CHECK_EQ(s.fx[1].value, 12);
}

TEST(pattern_default) {
    static Pattern p;
    p.tracks[3].steps[9].note = 5;
    p.tracks[3].lastStep = 7;
    p.setDefault();
    for (auto& t : p.tracks) {
        CHECK_EQ(t.lastStep, 31);
        CHECK_EQ(t.length(), 32);
    }
    CHECK_EQ(p.tracks[3].steps[9].note, kNoteEmpty);
}

TEST(instrument_defaults) {
    Instrument ins;
    CHECK_EQ(ins.active, 1);
    CHECK_EQ(ins.loopPoint1, 1);
    CHECK_EQ(ins.loopPoint2, 65534);
    CHECK_EQ(ins.endPoint, 65535);
    CHECK_EQ(ins.volume, 50);
    CHECK_EQ(ins.panning, 50);
    CHECK_EQ(ins.grainLength, 441);
    CHECK_EQ(ins.bitDepth, 16);
    CHECK_EQ(ins.wavetableWindowSize, 2048);
    CHECK_EQ(ins.env[0].enabled, 1);
    CHECK_EQ(ins.env[0].attackMs, 0);
    for (int i = 1; i < kAutomationCount; ++i) {
        CHECK_EQ(ins.env[i].enabled, 0);
        CHECK_EQ(ins.env[i].attackMs, 3000);
    }
    CHECK_EQ(ins.env[3].releaseMs, 1000);
    CHECK_EQ(ins.lfo[2].shape, uint8_t(LfoShape::Triangle));
    CHECK_NEAR(ins.lfo[2].amount, 0.5, 0);
    CHECK_NEAR(ins.cutoff, 1.0, 0);
}

TEST(project_defaults_and_accessors) {
    static Project p;
    CHECK_EQ(p.playlist[0], 1);
    CHECK_EQ(p.playlist[1], 0);
    CHECK_EQ(p.currentPattern(), 1);
    CHECK_NEAR(p.tempo(), 130.0, 0);
    CHECK_EQ(p.defaultPatternLength(), 32);
    CHECK_EQ(p.beatSliceReference(), 24);
    CHECK_STR(p.name, "New Project");
    CHECK_STR(p.folder, "");
    char buf[32];
    CHECK(p.trackName(0, buf, sizeof(buf)));
    CHECK_STR(buf, "Track 1");
    CHECK(p.trackName(7, buf, sizeof(buf)));
    CHECK_STR(buf, "Track 8");
    CHECK(p.trackName(8, buf, sizeof(buf)));
    CHECK_STR(buf, "Midi 9");
    CHECK(p.trackName(15, buf, sizeof(buf)));
    CHECK_STR(buf, "Midi 16");
    CHECK(!p.trackName(16, buf, sizeof(buf)));
    // 9..16 slots hold 7 chars max.
    p.setTrackName(9, "LongName123");
    p.trackName(9, buf, sizeof(buf));
    CHECK_STR(buf, "LongNam");
    p.trackName(10, buf, sizeof(buf));
    CHECK_STR(buf, "Midi 11");
    MidiInstrument m = p.midiInstrument(5);
    CHECK_EQ(m.channel, 5);
    CHECK_EQ(m.velocity, 127);
    CHECK_EQ(m.cc[0], 1);
    CHECK_EQ(m.cc[5], 128);
    p.setTempo(99.5f);
    CHECK_NEAR(p.f32(mt::kTempo), 99.5, 0);
    p.setName("0123456789012345678901234567890123456789");
    CHECK_EQ(std::strlen(p.name), 32u);
}

TEST(recents_policy) {
    static Recents r;
    r.touch("Projects/A");
    r.touch("Projects/B");
    r.touch("Projects/A");
    CHECK_STR(r.paths[0], "Projects/A");
    CHECK_STR(r.paths[1], "Projects/B");
    CHECK_STR(r.paths[2], "");
    for (int i = 0; i < 12; ++i) {
        char b[32];
        std::snprintf(b, sizeof(b), "P/%d", i);
        r.touch(b);
    }
    CHECK_STR(r.paths[0], "P/11");
    CHECK_STR(r.paths[9], "P/2");
    char name[64];
    Recents::displayName("Projects/My Song", name, sizeof(name));
    CHECK_STR(name, "My Song");
    Recents::displayName("Projects/abcdefghijklmnopqrstuvwxyz0123456789/", name, sizeof(name));
    CHECK_STR(name, "abcdefghijklmnopqrstuvwxyz012345");
}

TEST(pattern_names) {
    static PatternNames n;
    n.set(5, "Intro");
    CHECK_STR(n.get(5), "Intro");
    n.set(6, "0123456789012345678901234567890123");
    CHECK_EQ(std::strlen(n.get(6)), 30u);
    CHECK_STR(n.get(300), "");
}

TEST(preset_cmd_helpers) {
    // A pattern's Z command is FX type 0 with value 0xD0 + n; id 43 exists only in the lists.
    CHECK(!fxIdValid(kFxSynthPreset));
    CHECK_EQ(kFxListCount, kFxCount + 1);
    CHECK_EQ(int(fxInfo(kFxSynthPreset).symbol), int('Z'));
    CHECK_EQ(int(kFxMenuOrder[kFxListCount - 1]), int(kFxSynthPreset));
    CHECK_EQ(int(kFxMenuOrder[kFxCount - 1]), int(uint8_t(FxId::BreakPattern)));   // stock list unchanged
    for (int n = 0; n <= kSynthPresetCount; ++n) {
        const uint8_t v = uint8_t(kFxPresetCmdBase + n);
        CHECK(isPresetCmd(0, v));
        CHECK_EQ(int(presetCmdIndex(v)), n);
        CHECK_EQ(int(fxLogicalType(0, v)), int(kFxSynthPreset));
        CHECK_EQ(int(fxLogicalValue(0, v)), n);
    }
    CHECK(!isPresetCmd(0, uint8_t(kFxPresetCmdBase - 1)));
    CHECK(!isPresetCmd(0, uint8_t(kFxPresetCmdBase + kSynthPresetCount + 1)));
    CHECK(!isPresetCmd(uint8_t(FxId::SlideUp), kFxPresetCmdBase));   // a real Slide value
    CHECK_EQ(int(fxLogicalType(uint8_t(FxId::SlideUp), 210)), int(uint8_t(FxId::SlideUp)));
    CHECK_EQ(int(fxLogicalValue(uint8_t(FxId::SlideUp), 210)), 210);
}

TEST(step_clear_leaves_no_preset_cmd_behind) {
    Step s;
    s.fx[0] = StepFx{0, uint8_t(kFxPresetCmdBase + 3)};
    s.fx[1] = StepFx{uint8_t(FxId::SlideUp), 210};   // cleared, 210 under type 0 would read as a Z
    s.clear();
    CHECK(!isPresetCmd(s.fx[0].type, s.fx[0].value));
    CHECK(!isPresetCmd(s.fx[1].type, s.fx[1].value));
    Step o;
    o.fx[0] = StepFx{uint8_t(FxId::Volume), 77};
    o.clear();
    CHECK_EQ(int(o.fx[0].value), 77);   // any other value stays, as the stock clear leaves it
}

TEST(track_blank_counts_preset_cmds) {
    Track t;
    t.clear(15);
    CHECK(t.blank());
    t.steps[3].fx[1] = StepFx{0, uint8_t(kFxPresetCmdBase + 1)};
    CHECK(!t.blank());
    t.steps[3].fx[1] = StepFx{0, 50};   // a stale value is still nothing
    CHECK(t.blank());
}
