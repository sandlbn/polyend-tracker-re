// [I] Resample take diagnostics (app/resample.h): the per-take stats the
// audio side counts, App's report of each finished or cancelled take, and
// the "resample:" diag log line.
#include <cstring>
#include <string>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "formats/wav.h"

using namespace ot;

namespace {

// Same project as resample_test.cpp: a saw on track 1 every step, 120 BPM.
void setup(Harness& h) {
    App& a = *h.app;
    std::vector<int16_t> s(3000);
    for (uint32_t i = 0; i < s.size(); ++i) s[i] = int16_t((i % 100) * 300 - 15000);
    std::vector<uint8_t> out(s.size() * 2 + 64);
    out.resize(wavEncode(s.data(), uint32_t(s.size()), 1, out.data(), out.size()));
    h.fs.files["saw.wav"] = out;
    CHECK(a.importSample("saw.wav", 0));
    for (int st = 0; st < 64; ++st) {
        a.pattern().tracks[0].steps[st].note = 60;
        a.pattern().tracks[0].steps[st].instrument = 0;
    }
    a.pattern().tracks[0].lastStep = 63;
    a.patternEdited();
    a.project.setTempo(120.0f);
}

constexpr double kBlockS = 128.0 / 44122.6484;
int blocksFor(double seconds) { return int(seconds / kBlockS + 0.5); }

void tap(Resampler& r, float level, uint32_t n = 128) {
    std::vector<float> l(n, level), rr(n, -level);
    r.tapBlock(l.data(), rr.data(), n);
}

}  // namespace

TEST(resampler_take_stats) {
    std::vector<int16_t> area(2 * 4096);
    Resampler r;
    r.arm(area.data(), uint32_t(area.size()), 0);
    tap(r, 0.5f);                       // armed: nothing counted
    CHECK_EQ(r.stats().blocks, 0u);
    r.onBlock(true, true);              // bar start: recording
    tap(r, 0.25f);
    tap(r, 0.0f);
    tap(r, 0.0f);
    tap(r, 0.0f);
    tap(r, 0.5f);                       // the negative right channel counts too
    tap(r, 0.0f);
    CHECK_EQ(r.stats().blocks, 6u);
    CHECK_EQ(r.stats().silentBlocks, 4u);
    CHECK_EQ(r.stats().longestSilent, 3u);
    CHECK_EQ(r.stats().peak, 16383u);   // 0.5 * 32767
    // A new take starts from zero.
    r.reset();
    r.arm(area.data(), uint32_t(area.size()), 0);
    r.onBlock(true, true);
    tap(r, 1.5f);                       // clipped: left 32767, right -32768
    CHECK_EQ(r.stats().blocks, 1u);
    CHECK_EQ(r.stats().silentBlocks, 0u);
    CHECK_EQ(r.stats().peak, 32768u);
    // Full area: the partial block counts, nothing after it.
    tap(r, 0.1f, 4096);
    CHECK(r.state() == Resampler::kDone);
    const uint32_t blocks = r.stats().blocks;
    tap(r, 0.1f);
    CHECK_EQ(r.stats().blocks, blocks);
}

TEST(resample_report_per_take) {
    Harness h;
    App& a = *h.app;
    setup(h);
    CHECK_EQ(a.lastResample.seq, 0u);
    // Cancelled while armed: no take, no report.
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    CHECK(a.resampleToggle() == App::ResampleAction::kCancelled);
    CHECK_EQ(a.lastResample.seq, 0u);
    // A 1-bar take of the master.
    a.resampleCfg.length = 1;
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    a.seq.play(0);
    h.audio.pump(blocksFor(2.2));
    h.run(5);
    const ResampleReport& r = a.lastResample;
    CHECK_EQ(r.seq, 1u);
    CHECK_EQ(r.outcome, ResampleReport::kOk);
    CHECK_EQ(r.slot, 1);
    CHECK_EQ(r.source, 0);
    CHECK_EQ(r.end, Resampler::kEndBars);
    CHECK_EQ(r.frames, a.bank.frames(1));
    CHECK_EQ(r.stats.blocks, (r.frames + 127) / 128);
    CHECK(r.stats.silentBlocks < r.stats.blocks);
    CHECK(r.stats.peak > 1000);
    // Cancelled while recording (Shift + Sample Recorder twice is a stop, so
    // cancel through a new project, which clears the reserved memory).
    a.resampleCfg.length = 0;   // manual
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    h.audio.pump(blocksFor(2.2));
    CHECK(a.resampler.state() == Resampler::kRecording);
    CHECK(a.newProject());
    h.run(5);
    CHECK_EQ(a.lastResample.seq, 2u);
    CHECK_EQ(a.lastResample.outcome, ResampleReport::kCancelled);
    CHECK(a.lastResample.frames > 0u);
}

TEST(resample_report_line) {
    ResampleReport r;
    r.seq = 3;
    r.source = audio::AudioEngine::kTapLineIn;
    r.end = Resampler::kEndStopped;
    r.outcome = ResampleReport::kOk;
    r.slot = 12;
    r.frames = 176490;   // 4.00 s
    r.stats.blocks = 1379;
    r.stats.silentBlocks = 40;
    r.stats.longestSilent = 22;
    r.stats.peak = 16383;
    char b[256];
    const size_t n = formatResampleReport(b, sizeof b, r, 1, 22);
    CHECK_EQ(n, std::strlen(b));
    CHECK_STR(b, "resample: take 3 source line-in end stopped -> instrument 13 | 176490 frames (4.00 s) blocks 1379 "
                 "silent 40 longest 22 peak 16383 (49%) | rx restarts +1 stale +22");
    r.source = 0;
    r.outcome = ResampleReport::kEmpty;
    formatResampleReport(b, sizeof b, r, 0, 0);
    CHECK(std::strstr(b, "source master end stopped -> nothing recorded |") != nullptr);
    r.source = 4;
    r.outcome = ResampleReport::kCancelled;
    formatResampleReport(b, sizeof b, r, 0, 0);
    CHECK(std::strstr(b, "source track4 end stopped -> cancelled |") != nullptr);
    char small[20];
    CHECK_EQ(formatResampleReport(small, sizeof small, r, 0, 0), 19u);
    CHECK_EQ(std::strlen(small), 19u);
}
