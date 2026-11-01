// OpenTracker synth preset pack (tools/make_patches, PRESETS.md): every
// preset is built through the App and written with its own Save As on a fake
// card, then read back through the firmware's Load Patch path
// (App::loadSynthLibraryPatch): the file must decode, carry the engine id of
// its model folder, validate, load into the slot byte for byte as saved, and
// re-encode to the same bytes. Each loaded patch is then played through a
// real SynthBank slot at the default slot record (-9 dB, sends off, macros
// at their page defaults: 63 bipolar = the patch as saved): a few notes
// (ACD, FAT / WTFM Poly 1), 4-voice chords and single notes (VAP, FAT / WTFM Poly 4),
// every drum and the whole kit at once (PERC; every drum of a kit
// must also be audible on its own). It must be audible (peak >
// -30 dBFS) and stay at least 1 dB below full scale. Macros M1..M6 at both extremes must stay finite and
// below full scale too. When the release folder (releases/OpenTracker-Presets)
// exists, its files must be the generator's bytes (for the models it has).
// 3.x: the pack holds the models of the profile's modules (presets.cpp); the
// JU6 module engine (JU6S, Patches/JU6/) plays 4-voice chords and single
// notes like VAP, the mono D-Type (DTY1, Patches/DTYPE/) a few notes at
// Poly 1 like ACD, the duophonic A-Type (ATY1, Patches/ATYPE/) the mono
// notes plus a Duo pair from two tracks, the CS-Type (CST1, Patches/CSTYPE/)
// 4-voice chords like VAP (its Poly 1 patches the mono notes), the T-Type kits (TTY1, Patches/TTYPE/)
// every drum alone, then eight drums at once, at Poly 1; this test runs in every profile with a preset module
// (profile.mk PRESET_MODS).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "../tools/make_patches/presets.cpp"
#include "audio/synth_bank.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "modules/synth_models.h"

using namespace ot;
using namespace ot::audio;

namespace {

std::string libPath(const presets::Preset& p) {
    return std::string("Patches/") + presets::folderOf(p) + "/" + p.name + ".patch";
}

// The slot on the preset's engine (a module engine keeps the record's model).
void setEngine(App& a, int s, const presets::Preset& p) {
    a.setSynthModel(s, int(p.model));
    if (p.engine) a.setSynthEngine(s, p.engine);
}

std::vector<uint8_t> encode(const SynthPatch& p) {
    std::vector<uint8_t> out(kPatchMaxFileSize);
    size_t n = 0;
    CHECK(encodePatch(p, out.data(), out.size(), &n) == PatchStatus::kOk);
    out.resize(n);
    return out;
}

// One slot of a real engine in a bare bank (the SynthBank render path of
// tests/synth_levels_test.cpp).
struct Slot {
    SynthPatch patch;
    Governor gov;
    SynthBank bank;
    SynthMixInputs in;
    SynthSlotRecord rec;
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    float peak = 0.0f;
    long clips = 0;
    bool finite = true;
    Slot(const SynthPatch& p, SynthModel m, int poly, int macroValue = -1, int32_t engine = 0) : patch(p) {
        gov.setBudget(8);
        bank.setGovernor(&gov);
        bank.setStartsPerBlock(0);
        bank.bindPatch(0, &patch);
        rec = SynthSlotRecord::defaults(0);
        rec.model = uint32_t(m);
        rec.polyphony = uint32_t(poly);
        rec.delaySend = 0.0f;
        rec.reverbSend = 0.0f;
        for (int k = 0; k < int(mt::kSynthMacroCount); ++k)
            rec.macros[k] = synthMacroDefault(patch.macros[k].bipolar);
        if (macroValue >= 0)
            for (int k = 0; k < kSynthSeqMacros; ++k) rec.macros[k] = uint8_t(macroValue);
        bank.setSlotRecord(0, rec);
        bank.setSlotEngine(0, engine);
        block();
    }
    void block() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        bank.render(in, L, R, D, V, nullptr);
    }
    void on(int track, int note) {
        SynthNoteOn n;
        n.slot = 0;
        n.note = uint8_t(note);
        n.volume = -1;
        bank.noteOn(track, n);
    }
    void off(int track) { bank.noteOff(track, NoteOffMode::Off); }
    void run(int n) {
        for (int b = 0; b < n; ++b) {
            block();
            for (uint32_t i = 0; i < kBlock; ++i) {
                if (!std::isfinite(L[i]) || !std::isfinite(R[i])) finite = false;
                const float a = std::max(std::fabs(L[i]), std::fabs(R[i]));
                peak = std::max(peak, a);
                if (a >= 1.0f) ++clips;
            }
        }
    }
};

double dbfs(float x) { return 20.0 * std::log10(x > 1e-9f ? double(x) : 1e-9); }

constexpr int kBlocksPerSec = 345;   // 44.1 kHz / 128

// The test performance of one preset; returns the slot (peak, clips). FAT
// and WTFM presets play as ACD (mono, Poly 1) or as VAP (chords, Poly 4);
// JU6 presets always as VAP (4-voice chords, whatever their suggested Poly),
// D-Type presets as ACD (mono), A-Type as ACD then two tracks held together
// (Duo), CS-Type as VAP or, at Poly 1, as ACD.
void perform(Slot& s, const presets::Preset& p, int poly) {
    SynthModel m = p.model;
    if (p.engine == presets::kJu6Engine) m = SynthModel::VAP;
    else if (p.engine == presets::kDtypeEngine || p.engine == presets::kAtypeEngine) m = SynthModel::ACD;
    else if (p.engine == presets::kCstypeEngine) m = poly > 1 ? SynthModel::VAP : SynthModel::ACD;
    else if (m == SynthModel::FAT || m == SynthModel::WTFM) m = poly > 1 ? SynthModel::VAP : SynthModel::ACD;
    if (p.engine == presets::kTtypeEngine) {
        // Every drum alone (both hats, crash and ride, a tom of each group,
        // a low tom transposed), then eight drums in one block (twice).
        const int notes[14] = {60, 61, 62, 63, 64, 67, 68, 69, 70, 71, 48, 53, 59, 40};
        for (int n : notes) {
            s.on(0, n);
            s.run(kBlocksPerSec / 2);
        }
        for (int rep = 0; rep < 2; ++rep) {
            const int kit[8] = {60, 62, 63, 67, 69, 48, 52, 56};
            for (int k = 0; k < 8; ++k) s.on(k, kit[k]);
            s.run(kBlocksPerSec);
        }
    } else if (m == SynthModel::ACD) {
        const int notes[5] = {36, 43, 48, 60, 72};
        for (int n : notes) {
            s.on(0, n);
            s.run(kBlocksPerSec / 2);
            s.off(0);
            s.run(kBlocksPerSec / 4);
        }
        s.run(kBlocksPerSec);
        if (p.engine == presets::kAtypeEngine) {   // a Duo pair: two tracks, then either released
            const int pairs[2][2] = {{43, 50}, {55, 67}};
            for (int k = 0; k < 2; ++k) {
                s.on(0, pairs[k][0]);
                s.on(1, pairs[k][1]);
                s.run(kBlocksPerSec);
                s.off(k);
                s.run(kBlocksPerSec / 2);
                s.off(1 - k);
                s.run(kBlocksPerSec / 2);
            }
        }
    } else if (m == SynthModel::VAP) {
        // 4-voice chords held 2 s (slow pads reach full level), then single
        // notes low and high.
        const int chords[3][4] = {{48, 55, 60, 64}, {57, 60, 64, 67}, {50, 57, 62, 65}};
        for (const auto& ch : chords) {
            for (int k = 0; k < 4; ++k) s.on(k, ch[k]);
            s.run(2 * kBlocksPerSec);
            for (int k = 0; k < 4; ++k) s.off(k);
            s.run(kBlocksPerSec);
        }
        const int notes[3] = {36, 60, 84};
        for (int n : notes) {
            s.on(0, n);
            s.run(kBlocksPerSec);
            s.off(0);
            s.run(kBlocksPerSec / 2);
        }
    } else {
        // Every drum alone, then the whole kit in one block (twice).
        const int notes[8] = {60, 62, 64, 67, 69, 71, 48, 55};
        for (int n : notes) {
            s.on(0, n);
            s.run(kBlocksPerSec / 2);
        }
        for (int rep = 0; rep < 2; ++rep) {
            const int kit[5] = {60, 62, 67, 69, 71};
            for (int k = 0; k < 5; ++k) s.on(k, kit[k]);
            s.run(kBlocksPerSec);
        }
    }
    s.run(kBlocksPerSec);
}

int polyFor(const presets::Preset& p) {
    if (p.engine == presets::kTtypeEngine) return 1;   // one voice owns the kit
    if (p.engine == presets::kJu6Engine) return 4;
    if (p.engine == presets::kDtypeEngine || p.engine == presets::kAtypeEngine) return 1;
    if (p.engine == presets::kCstypeEngine) return p.playPoly;
    if (p.model == SynthModel::FAT || p.model == SynthModel::WTFM) return p.playPoly;
    return p.model == SynthModel::VAP ? 4 : 1;
}

// Generated pack (name -> bytes) and the loaded patches, built once.
struct Pack {
    std::vector<std::vector<uint8_t>> files;
    std::vector<SynthPatch> loaded;
};
Pack& pack() {
    static Pack p;
    return p;
}

}  // namespace

TEST(presets_names_counts_and_macros) {
    int acd = 0, fat = 0, vap = 0, wtfm = 0, perc = 0, ju6 = 0, dtype = 0, cstype = 0, atype = 0, ttype = 0;
    std::vector<std::string> names;
    for (const presets::Preset& p : presets::all()) {
        const int32_t e = presets::engineOf(p);
        acd += e == kSynthEngineAcd;
        fat += e == kSynthEngineFat;
        vap += e == kSynthEngineVap;
        wtfm += e == kSynthEngineWtfm;
        perc += e == kSynthEnginePerc;
        ju6 += e == presets::kJu6Engine;
        dtype += e == presets::kDtypeEngine;
        cstype += e == presets::kCstypeEngine;
        atype += e == presets::kAtypeEngine;
        ttype += e == presets::kTtypeEngine;
        if (e == presets::kDtypeEngine || e == presets::kAtypeEngine) CHECK_EQ(p.playPoly, 1);   // mono / Duo
        if (e == presets::kTtypeEngine) {   // a kit: Poly 1, "OT Kit " names
            CHECK_EQ(p.playPoly, 1);
            CHECK(std::strncmp(p.name, "OT Kit ", 7) == 0);
        }
        CHECK(p.engine == 0 || p.engine >= 0x80);
        CHECK(std::strncmp(p.name, "OT ", 3) == 0);
        CHECK(std::strlen(p.name) <= 24);
        CHECK(std::strchr(p.name, '/') == nullptr);
        CHECK(std::find(names.begin(), names.end(), p.name) == names.end());
        names.push_back(p.name);
        CHECK_EQ(int(p.macros.size()), 6);
        for (const presets::Macro& m : p.macros) {
            CHECK(std::strlen(m.name) > 0 && std::strlen(m.name) <= size_t(kPatchMacroNameMax));
            CHECK(!m.targets.empty() && m.targets.size() <= size_t(kPatchMacroTargets));
        }
    }
    std::printf("  presets: ACD %d, FAT %d, VAP %d, WTFM %d, PERC %d, JU6 %d, D-Type %d, CS-Type %d, A-Type %d, T-Type %d\n", acd,
                fat, vap, wtfm, perc, ju6, dtype, cstype, atype, ttype);
#if defined(OT_MOD_ACD1)
    CHECK(acd >= 10);
#endif
#if defined(OT_MOD_FAT1)
    CHECK(fat >= 8);
#endif
#if defined(OT_MOD_VAP1)
    CHECK(vap >= 14);
#endif
#if defined(OT_MOD_WTF1)
    CHECK(wtfm >= 30);
#endif
#if defined(OT_MOD_PRC1)
    CHECK(perc >= 16);
#endif
#if defined(OT_MOD_JU6S)
    CHECK(ju6 >= 12);
#endif
#if defined(OT_MOD_DTY1)
    CHECK(dtype >= 12);
#endif
#if defined(OT_MOD_CST1)
    CHECK(cstype >= 12);
#endif
#if defined(OT_MOD_ATY1)
    CHECK(atype >= 12);
#endif
#if defined(OT_MOD_TTY1)
    CHECK(ttype >= 12);
#endif
    (void)ttype;
}

TEST(presets_save_and_load_through_the_app) {
    Harness h;
    App& a = *h.app;
    Pack& pk = pack();
    for (const presets::Preset& p : presets::all()) {
        std::string err;
        const bool ok = presets::buildAndSave(a, 0, p, err);
        if (!ok) std::printf("       %s\n", err.c_str());
        CHECK(ok);
        const std::vector<uint8_t> built = encode(a.synthPatch(0));
        const auto it = h.fs.files.find(libPath(p));
        CHECK(it != h.fs.files.end());
        if (it == h.fs.files.end()) continue;
        const std::vector<uint8_t> file = it->second;
        CHECK(file == built);
        // The file on its own: decodes, engine id of its folder, valid.
        SynthPatch dec;
        CHECK(decodePatch(file.data(), file.size(), &dec) == PatchStatus::kOk);
        CHECK_EQ(dec.engine, presets::engineOf(p));
        CHECK_EQ(validatePatch(dec, nullptr, 0), 0);
        // Load Patch: the slot on the model's Init, then the library patch.
        setEngine(a, 0, p);
        a.initSynthPatch(0);
        CHECK(a.loadSynthLibraryPatch(0, p.name));
        CHECK(a.synthPatchSource(0) == App::PatchSource::Library);
        const SynthSlotRecord rec = a.synthSlot(0);
        CHECK_EQ(rec.model, uint32_t(p.model));
        CHECK_STR(rec.patchName, p.name);
        CHECK_EQ(a.synthPatch(0).engine, presets::engineOf(p));
        CHECK_EQ(a.slotEngineId(0), presets::engineOf(p));
        CHECK(encode(a.synthPatch(0)) == file);
        // Macro page values: bipolar macros at 63 (the patch as saved).
        for (int k = 0; k < kSynthSeqMacros; ++k)
            CHECK_EQ(int(rec.macros[k]), a.synthPatch(0).macros[k].bipolar ? 63 : 0);
        // Another engine's folder does not have it.
        a.setSynthModel(0, int(p.model == SynthModel::VAP && !p.engine ? SynthModel::ACD : SynthModel::VAP));
        CHECK(!a.loadSynthLibraryPatch(0, p.name));
        setEngine(a, 0, p);
        CHECK(a.loadSynthLibraryPatch(0, p.name));
        pk.files.push_back(file);
        pk.loaded.push_back(a.synthPatch(0));
    }
    CHECK_EQ(pk.loaded.size(), presets::all().size());
    // Nothing else under Patches/ (no temporary files left).
    int n = 0;
    for (const auto& f : h.fs.files) n += f.first.compare(0, 8, "Patches/") == 0;
    CHECK_EQ(n, int(presets::all().size()));
}

TEST(presets_render_audible_and_never_full_scale) {
    const Pack& pk = pack();
    CHECK_EQ(pk.loaded.size(), presets::all().size());
    float loud = 0.0f, quiet = 1e9f;
    std::string loudName, quietName;
    for (size_t i = 0; i < pk.loaded.size(); ++i) {
        const presets::Preset& p = presets::all()[i];
        Slot s(pk.loaded[i], p.model, polyFor(p), -1, p.engine);
        perform(s, p, polyFor(p));
        std::printf("  %-5s %-22s peak %6.1f dBFS\n", presets::folderOf(p), p.name, dbfs(s.peak));
        CHECK(s.finite);
        CHECK(dbfs(s.peak) > -30.0);
        CHECK(dbfs(s.peak) < -1.0);   // headroom: as saved, the loudest phrase stays 1 dB below full scale
        CHECK_EQ(s.clips, 0L);
        if (s.peak > loud) loud = s.peak, loudName = p.name;
        if (s.peak < quiet) quiet = s.peak, quietName = p.name;
    }
    std::printf("  loudest %s %.1f dBFS, quietest %s %.1f dBFS\n", loudName.c_str(), dbfs(loud), quietName.c_str(),
                dbfs(quiet));
}

// PERC: every drum of every kit on its own (kick, snare, closed and open
// hat, cymbal, perc, a low and a high tom) is audible and below full scale.
TEST(presets_perc_every_drum_audible) {
    const Pack& pk = pack();
    for (size_t i = 0; i < pk.loaded.size(); ++i) {
        const presets::Preset& p = presets::all()[i];
        if (p.model != SynthModel::PERC || p.engine) continue;   // (T-Type kits keep PERC as the record's model)
        const int notes[8] = {60, 62, 64, 67, 69, 71, 45, 57};
        std::string line;
        for (int n : notes) {
            Slot s(pk.loaded[i], p.model, 1);
            s.on(0, n);
            s.run(kBlocksPerSec);
            char b[16];
            std::snprintf(b, sizeof b, " %6.1f", dbfs(s.peak));
            line += b;
            if (dbfs(s.peak) <= -30.0 || dbfs(s.peak) >= -1.0) std::printf("       %s note %d\n", p.name, n);
            CHECK(dbfs(s.peak) > -30.0);
            CHECK(dbfs(s.peak) < -1.0);
            CHECK_EQ(s.clips, 0L);
        }
        std::printf("  %-22s%s\n", p.name, line.c_str());
    }
}

// T-Type: every drum of every kit on its own (kick, rim, snare, clap, closed
// and open hat, cowbell, crash, ride, a tom of each group) is audible and
// below full scale.
TEST(presets_ttype_every_drum_audible) {
    const Pack& pk = pack();
    for (size_t i = 0; i < pk.loaded.size(); ++i) {
        const presets::Preset& p = presets::all()[i];
        if (p.engine != presets::kTtypeEngine) continue;
        const int notes[12] = {60, 61, 62, 63, 64, 67, 68, 69, 70, 49, 54, 58};
        std::string line;
        for (int n : notes) {
            Slot s(pk.loaded[i], p.model, 1, -1, p.engine);
            s.on(0, n);
            s.run(kBlocksPerSec);
            char b[16];
            std::snprintf(b, sizeof b, " %6.1f", dbfs(s.peak));
            line += b;
            if (dbfs(s.peak) <= -30.0 || dbfs(s.peak) >= -1.0) std::printf("       %s note %d\n", p.name, n);
            CHECK(dbfs(s.peak) > -30.0);
            CHECK(dbfs(s.peak) < -1.0);
            CHECK_EQ(s.clips, 0L);
        }
        std::printf("  %-22s%s\n", p.name, line.c_str());
    }
}

TEST(presets_macro_extremes_stay_in_bounds) {
    const Pack& pk = pack();
    for (size_t i = 0; i < pk.loaded.size(); ++i) {
        const presets::Preset& p = presets::all()[i];
        for (int v : {0, 127}) {
            Slot s(pk.loaded[i], p.model, polyFor(p), v, p.engine);
            perform(s, p, polyFor(p));
            if (!s.finite || s.clips)
                std::printf("       %s macros %d: peak %.1f dBFS, %ld clipped\n", p.name, v, dbfs(s.peak), s.clips);
            CHECK(s.finite);
            CHECK_EQ(s.clips, 0L);
        }
    }
}

TEST(presets_release_folder_matches_the_generator) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path("../../releases/OpenTracker-Presets");
    if (!fs::exists(dir / "Patches")) {
        std::printf("  (no release folder: skipped)\n");
        return;
    }
    // A pack released before a model had presets (FAT: 2.1) has no folder
    // for it: the models it has must match the generator.
    const Pack& pk = pack();
    int files = 0, want = 0;
    auto released = [&](const presets::Preset& p) {
        return fs::is_directory(dir / "Patches" / presets::folderOf(p));
    };
    // Only the folders of the models in this build (profile nofm has no WTFM).
    std::vector<std::string> folders;
    for (const presets::Preset& p : presets::all())
        if (released(p) && std::find(folders.begin(), folders.end(), presets::folderOf(p)) == folders.end())
            folders.push_back(presets::folderOf(p));
    for (const std::string& d : folders)
        for (const auto& e : fs::recursive_directory_iterator(dir / "Patches" / d)) files += e.is_regular_file();
    for (const presets::Preset& p : presets::all()) want += released(p);
    CHECK_EQ(files, want);
    for (size_t i = 0; i < pk.files.size(); ++i) {
        if (!released(presets::all()[i])) continue;
        std::ifstream f(dir / libPath(presets::all()[i]), std::ios::binary);
        CHECK(f.good());
        const std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(b == pk.files[i]);
    }
}
