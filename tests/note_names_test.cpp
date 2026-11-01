// [I] Config > General > Note names (not in stock): sharps, flats, or Auto
// from the project key; the extension-record byte, the Config texts and the
// pattern grid.
#include <string>

#include "test.h"
#include "ui_fakes.h"
#include "nvstore_fake.h"

#include "app/project_fields.h"
#include "ui/notes.h"
#include "ui/screens.h"

using namespace ot;

namespace {
std::string nn(int n) {
    char b[4];
    noteName(int8_t(n), b);
    return b;
}
ConfigScreen& cfgScreen(Harness& h) { return *static_cast<ConfigScreen*>(h.app->ui.screen(kModConfig)); }
bool hasText(Harness& h, const char* s) {
    for (auto& l : h.disp.last)
        if (l.rfind("TEXT", 0) == 0 && l.find(std::string("\"") + s + "\"") != std::string::npos) return true;
    return false;
}
constexpr uint8_t kMinor = 1, kMajor = 2;   // scale indices (kScaleNames)
}  // namespace

TEST(note_names_sharps_and_flats) {
    setNoteSpelling(kSpellSharps, 0, 0);
    CHECK(nn(61) == "C#5");
    CHECK(nn(58) == "A#4");
    CHECK(nn(60) == "C5");
    setNoteSpelling(kSpellFlats, 0, 0);
    CHECK(nn(61) == "Db5");
    CHECK(nn(58) == "Bb4");
    CHECK(nn(66) == "Gb5");
    CHECK(nn(60) == "C5");     // naturals unchanged
    CHECK(nn(127) == "G0");    // octave digit wraps as before
    CHECK(nn(-1) == "---");
    CHECK(nn(-4) == "OFF");
    CHECK_STR(pitchClassName(70), "Bb");
    setNoteSpelling(kSpellSharps, 0, 0);
    CHECK_STR(pitchClassName(70), "A#");
}

TEST(note_names_auto_follows_key) {
    CHECK(!keyUsesFlats(0, 0));          // C chromatic
    CHECK(!keyUsesFlats(0, kMajor));     // C major
    CHECK(!keyUsesFlats(7, kMajor));     // G major
    CHECK(keyUsesFlats(5, kMajor));      // F major
    CHECK(keyUsesFlats(10, kMajor));     // Bb major
    CHECK(keyUsesFlats(1, kMajor));      // Db major
    CHECK(!keyUsesFlats(6, kMajor));     // F# major
    CHECK(keyUsesFlats(2, kMinor));      // D minor (relative F major)
    CHECK(keyUsesFlats(7, kMinor));      // G minor (Bb major)
    CHECK(!keyUsesFlats(9, kMinor));     // A minor (C major)
    CHECK(!keyUsesFlats(4, kMinor));     // E minor (G major)
    CHECK(keyUsesFlats(5 + 24, kMajor)); // root option C1..C4: pitch class only
    setNoteSpelling(kSpellAuto, 5, kMajor);
    CHECK(nn(70) == "Bb5");
    setNoteSpelling(kSpellAuto, 7, kMajor);
    CHECK(nn(66) == "F#5");
    setNoteSpelling(kSpellSharps, 0, 0);
}

// Auto on modes and the other 7-note scales: whichever of sharps / flats
// names the scale with each letter once (or with fewer repeats) wins; ties
// keep the key-signature rule above.
TEST(note_names_auto_spells_modes) {
    auto scaleIndex = [](const char* name) {
        for (int i = 0; i < kScaleCount; ++i)
            if (std::string(scaleName(uint8_t(i))) == name) return uint8_t(i);
        CHECK(false);
        return uint8_t(0);
    };
    CHECK(keyUsesFlats(0, scaleIndex("Mixlydian")));    // C D E F G A Bb
    CHECK(keyUsesFlats(9, scaleIndex("Phrygian")));     // A Bb C D E F G
    CHECK(keyUsesFlats(4, scaleIndex("Locrian")));      // E F G A Bb C D
    CHECK(!keyUsesFlats(2, scaleIndex("Melo Min")));    // D E F G A B C#
    CHECK(keyUsesFlats(0, scaleIndex("PhrygDom")));     // C Db E F G Ab Bb
    CHECK(!keyUsesFlats(6, kMajor));                    // F# major: a tie, sharps
    CHECK(!keyUsesFlats(9, scaleIndex("Penta Min")));   // A minor pentatonic: a tie, sharps
    CHECK(keyUsesFlats(2, scaleIndex("Penta Min")));    // D minor pentatonic: a tie, flats
    // Every 7-note scale and root: never more repeated letters than the
    // other spelling would give.
    for (int sc = 0; sc < kScaleCount; ++sc) {
        int notes = 0;
        for (int i = 0; i < 12; ++i) notes += (scaleMask(uint8_t(sc)) >> i) & 1;
        if (notes != 7) continue;
        for (int root = 0; root < 12; ++root) {
            int rep[2];
            for (int f = 0; f < 2; ++f) {
                setNoteSpelling(f ? kSpellFlats : kSpellSharps, 0, 0);
                int seen = 0;
                rep[f] = 0;
                for (int i = 0; i < 12; ++i) {
                    if (!((scaleMask(uint8_t(sc)) >> i) & 1)) continue;
                    const int bit = 1 << (pitchClassName(root + i)[0] - 'A');
                    rep[f] += (seen & bit) ? 1 : 0;
                    seen |= bit;
                }
            }
            CHECK(rep[keyUsesFlats(root, uint8_t(sc)) ? 1 : 0] <= rep[keyUsesFlats(root, uint8_t(sc)) ? 0 : 1]);
        }
    }
    setNoteSpelling(kSpellSharps, 0, 0);
}

TEST(note_names_config_record) {
    FakeNvStore nv;
    Config c;
    CHECK_EQ(c.noteNames, 0);
    c.noteNames = kSpellAuto;   // a non-default spelling alone writes the record
    CHECK(c.save(nv));
    CHECK_EQ(nv.mem[0xF4 + 4], 28);                  // payload length
    CHECK_EQ(nv.mem[0xF4 + 5 + 27], kSpellAuto);     // payload byte 28
    Config r;
    CHECK(r.load(nv));
    CHECK_EQ(r.noteNames, kSpellAuto);
    // A record from before the byte (payload 27) keeps Sharps. Its checksum
    // sits at offset 32; with these fields it is 1, a valid spelling, so only
    // the payload length keeps it from being read as Flats.
    Config o;
    o.synthVoices = 1;
    o.streamThreshold = 0;
    o.exportRate = 0;
    uint8_t ext[Config::kExtSize];
    o.toExt(ext);
    ext[4] = 27;
    uint8_t sum = 0;
    for (int i = 0; i < 27; ++i) sum = uint8_t(sum + ext[5 + i]);
    ext[5 + 27] = sum;
    CHECK_EQ(ext[32], 1);
    Config k0;
    k0.noteNames = kSpellFlats;
    CHECK(k0.fromExt(ext, 5 + 27 + 1));
    CHECK_EQ(k0.synthVoices, 1);
    CHECK_EQ(k0.noteNames, 0);
    // Out of range -> Sharps.
    c.noteNames = 3;
    c.sanitize();
    CHECK_EQ(c.noteNames, 0);
    // File fallback.
    FakeStorage fs;
    c.noteNames = kSpellFlats;
    CHECK(c.save(fs));
    Config k;
    CHECK(k.load(fs));
    CHECK_EQ(k.noteNames, kSpellFlats);
}

TEST(note_names_config_screen_and_pattern) {
    Harness h;
    App& a = *h.app;
    ConfigScreen& c = cfgScreen(h);
    char t[48];
    c.optionText(1, 17, t, sizeof t);
    CHECK_STR(t, "Sharps");
    // Root note option 1 = C#1 with sharps.
    a.project.setU8(pf::kRootNote, 1);
    h.run(20);
    c.optionText(0, 3, t, sizeof t);
    CHECK_STR(t, "C#1");
    // Flats: Root note and the pattern grid follow.
    a.cfg.noteNames = kSpellFlats;
    a.pattern().tracks[0].steps[0].note = 61;
    h.app->ui.dirty();
    h.run(40);
    c.optionText(1, 17, t, sizeof t);
    CHECK_STR(t, "Flats");
    c.optionText(0, 3, t, sizeof t);
    CHECK_STR(t, "Db1");
    CHECK(hasText(h, "Db5"));
    CHECK(!hasText(h, "C#5"));
    // Auto: G major sharps, F major flats.
    a.cfg.noteNames = kSpellAuto;
    a.project.setU8(pf::kScale, kMajor);
    a.project.setU8(pf::kRootNote, 7);
    h.run(40);
    CHECK(!noteSpellingFlats());
    CHECK(hasText(h, "C#5"));
    a.project.setU8(pf::kRootNote, 5);
    h.run(40);
    CHECK(noteSpellingFlats());
    CHECK(hasText(h, "Db5"));
    setNoteSpelling(kSpellSharps, 0, 0);
}
