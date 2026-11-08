// Tracker Mini fixtures (spec 21 §3, §4; docs/MINI_PLAN.md §1 tests).
//
// 1. Generated Mini projects (tests/fixtures/mini, gen_mini_fixtures.py):
//    project.mt and 0x3030 patterns load and round-trip byte-exact, with
//    synth steps on instruments 64..66, slot records and names 13..16.
// 2. Every pattern of the SD-card backup projects, fixtures/stock-project
//    and the demo card (read in place, skipped when absent): the 16-track
//    decoder gives tracks 0..11 identical to the stock decoder and tracks
//    12..15 empty; re-encoding in the stock layout gives the bytes the stock
//    encoder writes (and the original file for a 0x242C v5 file). Every
//    project.mt passes the slot check unchanged.
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "test.h"

#include "formats/bytes.h"
#include "formats/pattern_mtp.h"
#include "formats/project_mt.h"
#include "mini_golden.h"

using namespace ot;
namespace sfs = std::filesystem;

namespace {

std::string legacyRoot() {
    if (const char* e = std::getenv("OT_LEGACY_FIXTURES")) return e;
    return minigold::fixturesDir() + "/../../../fixtures";
}

std::vector<sfs::path> filesNamed(const sfs::path& root, const char* ext, const char* exact = nullptr) {
    std::vector<sfs::path> out;
    std::error_code ec;
    if (!sfs::exists(root, ec)) return out;
    for (auto it = sfs::recursive_directory_iterator(root, sfs::directory_options::follow_directory_symlink, ec);
         it != sfs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const sfs::path& p = it->path();
        if (exact ? p.filename() == exact : p.extension() == ext) out.push_back(p);
    }
    std::sort(out.begin(), out.end());
    return out;
}

Pattern g_a, g_b;
PatternStock g_s;
uint8_t g_out[mtp::kMiniFileSize], g_out2[mtp::kMiniFileSize];
uint8_t g_mt[mt::kFileSize];
Project g_p;

}  // namespace

TEST(generated_mini_projects_round_trip) {
    const sfs::path root = sfs::path(minigold::fixturesDir()) / "mini";
    const std::vector<sfs::path> projects = filesNamed(root, nullptr, "project.mt");
    CHECK_EQ(projects.size(), size_t(2));
    int patterns = 0, synthSteps = 0, extSynthSteps = 0;
    for (const sfs::path& mtPath : projects) {
        const std::vector<uint8_t> f = minigold::readFile(mtPath.string());
        CHECK_EQ(f.size(), size_t(mt::kFileSize));
        ProjectLoadInfo li;
        CHECK(decodeProject(f.data(), f.size(), g_p, &li) == FmtStatus::Ok);
        CHECK_EQ(li.structVersion, 17);
        CHECK_EQ(li.storedCrc, li.computedCrc);
        for (int s = 0; s < kSynthSlotCount; ++s) CHECK(g_p.synthSlotValid(s));
        CHECK_EQ(sanitizeSynthSlots(g_p), 0);
        encodeProject(g_p, g_mt);
        CHECK(std::memcmp(g_mt, f.data(), mt::kFileSize) == 0);

        const std::string name = mtPath.parent_path().filename().string();
        CHECK_STR(g_p.name, name.c_str());
        char buf[16];
        if (name == "Mini Synth Song") {
            const SynthSlotRecord r0 = g_p.synthSlot(0), r1 = g_p.synthSlot(1), r2 = g_p.synthSlot(2);
            CHECK_EQ(r0.model, uint32_t(SynthModel::PERC));
            CHECK_NEAR(r0.gainDb, -3.5, 0);
            CHECK_EQ(r0.polyphony, 3u);
            CHECK_NEAR(r0.pan, -25.0, 0);
            CHECK_STR(r0.patchName, "Kit 808");
            CHECK_NEAR(r0.reverbSend, 12.5, 0);
            CHECK_EQ(r0.macros[3], 127);
            CHECK_EQ(r1.model, uint32_t(SynthModel::ACD));
            CHECK_STR(r1.patchName, "Acid Line");
            CHECK_NEAR(r1.delaySend, 30.0, 0);
            CHECK_EQ(r2.model, uint32_t(SynthModel::VAP));
            CHECK_NEAR(r2.gainDb, -96.0, 0);
            const char* names[4] = {"Bass", "Lead", "Perc", "Pad"};
            for (int t = 0; t < 4; ++t) {
                g_p.trackName(12 + t, buf, sizeof(buf));
                CHECK_STR(buf, names[t]);
            }
            CHECK(!g_p.trackMuted(12) && g_p.trackMuted(13) && !g_p.trackMuted(14) && g_p.trackMuted(15));
        } else {
            const SynthSlotRecord r0 = g_p.synthSlot(0), r1 = g_p.synthSlot(1);
            CHECK_EQ(r0.model, uint32_t(SynthModel::WTFM));
            CHECK_EQ(r0.polyphony, 8u);
            CHECK_STR(r0.patchName, "");
            CHECK_EQ(r1.model, uint32_t(SynthModel::FAT));
            CHECK_EQ(r1.polyphony, 0u);
            CHECK_EQ(std::strlen(r1.patchName), size_t(31));
            g_p.trackName(12, buf, sizeof(buf));
            CHECK_STR(buf, "Midi 13");
            g_p.trackName(14, buf, sizeof(buf));
            CHECK_STR(buf, "");
            for (int t = 12; t < 16; ++t) CHECK(g_p.trackMuted(t));
        }

        for (const sfs::path& pp : filesNamed(mtPath.parent_path() / "patterns", ".mtp")) {
            const std::vector<uint8_t> pf = minigold::readFile(pp.string());
            CHECK_EQ(pf.size(), size_t(mtp::kMiniFileSize));
            int num = 0;
            CHECK(std::sscanf(pp.filename().string().c_str(), "pattern_%d.mtp", &num) == 1);
            PatternLoadInfo info;
            decodePattern16(pf.data(), pf.size(), g_a, nullptr, &info);
            CHECK(!info.upgraded && !info.upperTracksReset);
            CHECK_EQ(info.storedCrc, info.computedCrc);
            minigold::goldenPattern(g_b, num);
            bool same = true;
            for (int t = 0; t < kTrackCount; ++t) {
                same = same && minigold::sameTrack(g_a.tracks[t], g_b.tracks[t]);
                for (const Step& st : g_a.tracks[t].steps) {
                    synthSteps += isSynthSlotInstrument(st.instrument);
                    if (t >= kStockTrackCount) extSynthSteps += isSynthSlotInstrument(st.instrument);
                }
            }
            CHECK(same);
            CHECK_EQ(encodePattern16(g_a, g_out), size_t(mtp::kMiniFileSize));
            CHECK(std::memcmp(g_out, pf.data(), mtp::kMiniFileSize) == 0);
            // Split form (resident stock tracks + ext pool) gives the same bytes.
            Track ext[kExtTrackCount];
            decodePattern16(pf.data(), pf.size(), g_s, ext);
            const Track* ep[kExtTrackCount] = {&ext[0], &ext[1], &ext[2], &ext[3]};
            encodePattern16(g_s, ep, g_out2);
            CHECK(std::memcmp(g_out2, pf.data(), mtp::kMiniFileSize) == 0);
            ++patterns;
        }
    }
    CHECK_EQ(patterns, 3);
    CHECK(synthSteps >= 40);
    CHECK(extSynthSteps > 10);
    std::printf("  mini fixtures: %zu projects, %d patterns, %d synth steps (%d on tracks 13-16)\n", projects.size(),
                patterns, synthSteps, extSynthSteps);
}

TEST(stock_patterns_upgrade_to_16_tracks) {
    const std::string root = legacyRoot();
    std::vector<sfs::path> files;
    for (const char* sub : {"sdcard-backup-2026-10-15/Projects", "stock-project", "demo-sd"}) {
        const std::vector<sfs::path> v = filesNamed(sfs::path(root) / sub, ".mtp");
        files.insert(files.end(), v.begin(), v.end());
    }
    if (files.empty()) {
        std::printf("  legacy fixtures absent (%s), skipped\n", root.c_str());
        return;
    }
    int n = 0, stock242c = 0, identical = 0;
    for (const sfs::path& p : files) {
        const std::vector<uint8_t> f = minigold::readFile(p.string());
        PatternLoadInfo si, mi;
        decodePattern(f.data(), f.size(), g_s, nullptr, &si);
        minigold::goldenPattern(g_a, 3);   // garbage in the target
        decodePattern16(f.data(), f.size(), g_a, nullptr, &mi);
        bool ok = mi.upgraded == si.upgraded;
        for (int t = 0; t < kStockTrackCount; ++t) ok = ok && minigold::sameTrack(g_a.tracks[t], g_s.tracks[t]);
        for (int t = kStockTrackCount; t < kTrackCount; ++t)
            ok = ok && g_a.tracks[t].blank() && g_a.tracks[t].lastStep == g_a.tracks[0].lastStep;
        // Stock-layout re-encode == what the 12-track encoder writes.
        encodePattern(g_s, g_out);
        CHECK_EQ(encodePattern16(g_a, g_out2, MtpLayout::Stock12), size_t(mtp::kStockFileSize));
        ok = ok && std::memcmp(g_out, g_out2, mtp::kStockFileSize) == 0;
        if (f.size() == mtp::kStockFileSize && si.structVersion == 5 && si.sizeField == mtp::kStockFileSize) {
            ++stock242c;
            CHECK_EQ(mi.firstResetTrack, 12);
            if (si.storedCrc == si.computedCrc) {
                ok = ok && std::memcmp(g_out2, f.data(), f.size()) == 0;
                ++identical;
            }
        } else {
            CHECK_EQ(mi.firstResetTrack, 8);
        }
        if (!ok) std::printf("  mismatch: %s\n", p.string().c_str());
        CHECK(ok);
        ++n;
    }
    std::printf("  %d stock patterns: tracks 0-11 identical, 12-15 empty; %d 0x242C files (%d byte-identical)\n", n,
                stock242c, identical);
}

TEST(stock_projects_pass_slot_check) {
    const std::string root = legacyRoot();
    std::vector<sfs::path> files;
    for (const char* sub : {"sdcard-backup-2026-10-15/Projects", "stock-project", "demo-sd"}) {
        const std::vector<sfs::path> v = filesNamed(sfs::path(root) / sub, nullptr, "project.mt");
        files.insert(files.end(), v.begin(), v.end());
    }
    if (files.empty()) return;
    int n = 0, v17 = 0;
    for (const sfs::path& p : files) {
        const std::vector<uint8_t> f = minigold::readFile(p.string());
        ProjectLoadInfo li;
        if (decodeProject(f.data(), f.size(), g_p, &li) != FmtStatus::Ok) continue;
        ++n;
        // The slot area of a stock file is zero after load (valid, untouched).
        bool zero = true;
        for (uint32_t o = mt::kSynthSlots; o < mt::kSynthSlots + 3 * mt::kSynthSlotSize; ++o) zero = zero && g_p.u8(o) == 0;
        CHECK(zero);
        CHECK_EQ(sanitizeSynthSlots(g_p), 0);
        if (li.structVersion == 17 && f.size() == mt::kFileSize) {
            // Values and song blocks survive a re-encode byte for byte.
            ++v17;
            encodeProject(g_p, g_mt);
            CHECK(std::memcmp(g_mt + 0x10, f.data() + 0x10, 0x800) == 0);
        }
    }
    std::printf("  %d stock projects pass the slot check (%d structure-17 files byte-identical)\n", n, v17);
}
