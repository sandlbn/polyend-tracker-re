// Display performance path (docs/PLAN_2X.md performance workstream "Display"):
// cached display-list fragments (Gfx::cached, CMD_APPEND model), the
// non-blocking swap and the upload reduction on the Pattern screen.
//
// FragDisplay records like RecordingDisplay and models fragments the way the
// FT81x does: fragmentBegin/End store the commands drawn in between, an
// append re-emits them into the frame. The expanded frame (appends replaced
// by their content) must equal the frame drawn without fragments, after
// dropping redundant COLOR commands (a block starts with an explicit colour).
#include <algorithm>

#include "test.h"
#include "ui_fakes.h"

using namespace ot;

namespace {

class FragDisplay : public RecordingDisplay {
public:
    bool fragments = true;
    bool ready = true;
    uint32_t maxLines = 100000;      // capacity of one slot (refusal tests)
    int appends = 0, begins = 0;
    std::vector<std::string> slot[kFragmentSlots];
    bool frameReady() override { return ready; }
    bool fragmentsSupported() const override { return fragments; }
    void dlStart() override {
        RecordingDisplay::dlStart();
        appends = 0;
        capSlot = -1;
    }
    void fragmentBegin(uint8_t s) override {
        CHECK(capSlot < 0);
        ++begins;
        capSlot = s;
        capStart = cur.size();
    }
    bool fragmentEnd(uint8_t s) override {
        CHECK(capSlot == int(s));
        capSlot = -1;
        slot[s].assign(cur.begin() + long(capStart), cur.end());
        if (slot[s].empty() || slot[s].size() > maxLines) {
            slot[s].clear();
            return false;
        }
        return true;
    }
    void fragmentAppend(uint8_t s) override {
        CHECK(!slot[s].empty());
        ++appends;
        cur.insert(cur.end(), slot[s].begin(), slot[s].end());
    }
    uint32_t fragmentWords(uint8_t s) const override { return uint32_t(slot[s].size()); }

private:
    int capSlot = -1;
    size_t capStart = 0;
};

// Drops COLOR lines that repeat the colour already in effect.
std::vector<std::string> normalized(const std::vector<std::string>& in) {
    std::vector<std::string> out;
    std::string color;
    for (const std::string& l : in) {
        if (l.compare(0, 6, "COLOR ") == 0) {
            if (l == color) continue;
            color = l;
        }
        out.push_back(l);
    }
    return out;
}

struct Rig {
    Harness h;
    FragDisplay fd;
    Rig() {
        h.app->ui.attach(&fd, &h.leds);
        // Some pattern content in the first tracks.
        h.tap(btn::Rec);
        for (int p = 24; p < 30; ++p) {
            h.pad(uint8_t(p));
            h.tap(btn::Down);
        }
        h.tap(btn::Right);
        h.pad(31);
        h.tap(btn::Rec);
    }
    // Time with audio running (the sequencer advances in the audio callback).
    uint64_t acc = 0;
    void play(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10) {
            acc += 10u * 44100u;
            int blocks = 0;
            while (acc >= 128000u) {
                acc -= 128000u;
                ++blocks;
            }
            h.audio.pump(blocks);
            h.run(10);
        }
    }
    // One frame drawn inline, then with fragments (probe, capture, append):
    // the expanded command streams must be the same.
    void compareNow(const char* what) {
        Ui& ui = h.app->ui;
        ui.fragments.enabled = false;
        ui.render();
        const std::vector<std::string> a = normalized(fd.last);
        const uint32_t inlineWords = ui.lastUploadWords();
        ui.fragments.enabled = true;
        ui.fragments.reset();
        for (int i = 0; i < 4; ++i) ui.render();   // probes until stable, capture, append
        const std::vector<std::string> b = normalized(fd.last);
        CHECK(fd.appends > 0);
        CHECK(ui.lastUploadWords() < inlineWords);
        CHECK(ui.lastDlWords() < 2048);
        CHECK(a == b);
        if (a != b) {
            std::fprintf(stderr, "  %s: inline %zu vs cached %zu lines\n", what, a.size(), b.size());
            for (size_t i = 0; i < a.size() && i < b.size(); ++i)
                if (a[i] != b[i]) {
                    std::fprintf(stderr, "  first difference at %zu: '%s' vs '%s'\n", i, a[i].c_str(), b[i].c_str());
                    break;
                }
        }
    }
};

}  // namespace

TEST(fragments_match_inline_on_every_screen) {
    Rig r;
    const uint8_t keys[] = {btn::Pattern, btn::Song, btn::InstrParams, btn::SampleLoader, btn::Master,
                            btn::File, btn::Config, btn::SamplePlayback, btn::Performance, btn::SampleEditor,
                            btn::SampleRecorder};
    for (uint8_t k : keys) {
        r.h.tap(k);
        r.h.run(100);
        r.compareNow("screen");
    }
}

// 1.9.5 merge: the Pattern grid is drawn in colour passes across all rows
// (screen_pattern_draw.cpp), so the M0 per-row fragments are gone; the
// title chrome, the bottom bar labels, the header tabs and the separators
// are still appended from RAM_G (docs/CHANGES_2X.md).
TEST(pattern_playback_edit_mode_appends_chrome) {
    Rig r;
    Ui& ui = r.h.app->ui;
    r.h.tap(btn::Pattern);
    r.h.tap(btn::Rec);      // edit mode: the grid stands still while playing
    r.h.tap(btn::Play);
    // Inline cost of a playback frame.
    CHECK(r.h.app->seq.isPlaying());
    ui.fragments.enabled = false;
    r.play(300);
    const uint32_t inlineWords = ui.lastUploadWords();
    ui.fragments.enabled = true;
    ui.fragments.reset();
    uint32_t maxWords = 0;
    r.play(500);             // warm-up: blocks captured from their third frame on
    const uint32_t f0 = ui.frames();
    const uint32_t h0 = ui.fragments.hits;
    for (int i = 0; i < 20; ++i) {
        r.play(50);
        maxWords = std::max(maxWords, ui.lastUploadWords());
    }
    CHECK(ui.frames() > f0);
    CHECK(ui.fragments.hits > h0);
    std::printf("  edit-mode playback: inline %u words, cached max %u words\n", unsigned(inlineWords),
                unsigned(maxWords));
    CHECK(maxWords < inlineWords);
    r.compareNow("edit playback");
    r.h.tap(btn::Play);
}

TEST(pattern_playback_follow_matches_inline) {
    Rig r;
    r.h.tap(btn::Pattern);
    r.h.tap(btn::Play);     // cursor follows the playhead: the grid scrolls
    for (int i = 0; i < 6; ++i) {
        r.play(130);
        r.compareNow("follow playback");
    }
    r.h.tap(btn::Play);
}

TEST(follow_playback_appends_static_chrome) {
    Rig r;
    Ui& ui = r.h.app->ui;
    r.h.tap(btn::Pattern);
    r.h.tap(btn::Play);
    r.play(2000);
    CHECK(ui.fragments.hits > 0);          // title, headers, separators, bar
    CHECK(ui.fragments.captures < ui.frames());
    r.h.tap(btn::Play);
}

// A block whose content changes every frame goes hot: after kHotChanges
// changed probes it is drawn inline without hashing for kHotSkip frames, and
// it is never captured.
TEST(changing_block_goes_hot_and_skips_hashing) {
    FragDisplay fd;
    FragmentCache cache;
    for (int f = 0; f < 40; ++f) {
        Gfx g(fd, &cache);
        g.beginFrame(0);
        g.cached(kFragPatSeparators, [&] { g.fill(f, 0, 10, 10, 0xFFFFFF); });
        g.endFrame();
    }
    CHECK(cache.skips > 0);
    CHECK_EQ(cache.captures, 0u);
    CHECK_EQ(fd.begins, 0);
}

TEST(refused_fragment_is_not_captured_again) {
    Rig r;
    Ui& ui = r.h.app->ui;
    r.fd.maxLines = 1;       // every fragment is too large
    r.h.tap(btn::Song);
    for (int i = 0; i < 6; ++i) {
        ui.dirty();
        r.h.run(20);
    }
    const int begins = r.fd.begins;
    for (int i = 0; i < 6; ++i) {
        ui.dirty();
        r.h.run(20);
    }
    CHECK_EQ(r.fd.begins, begins);   // the refused hash is remembered
    CHECK_EQ(r.fd.appends, 0);
}

TEST(busy_display_defers_the_frame) {
    Rig r;
    Ui& ui = r.h.app->ui;
    r.h.run(100);
    const uint32_t f0 = ui.frames(), d0 = ui.framesDeferred();
    r.fd.ready = false;
    ui.dirty();
    r.h.run(100);
    CHECK_EQ(ui.frames(), f0);           // nothing drawn while busy, no spinning
    CHECK(ui.framesDeferred() > d0);
    r.fd.ready = true;
    r.h.run(20);
    CHECK(ui.frames() > f0);             // the deferred frame follows
}

TEST(no_fragments_without_display_support) {
    Rig r;
    Ui& ui = r.h.app->ui;
    r.fd.fragments = false;
    r.fd.begins = 0;
    r.h.tap(btn::Pattern);
    r.h.run(500);
    CHECK_EQ(r.fd.begins, 0);
    CHECK_EQ(r.fd.appends, 0);
    CHECK_EQ(ui.lastUploadWords(), ui.lastDlWords());
}

// RAM_DL guard (1.9.5) with fragments: words appended with CMD_APPEND fill
// RAM_DL like inline ones, so the guard counts them (Gfx::dlTotalWords). A
// limit between the uploaded and the total words must trip the guard.
TEST(dl_guard_counts_appended_fragment_words) {
    Rig r;
    Ui& ui = r.h.app->ui;
    r.h.tap(btn::Pattern);
    r.h.run(100);
    for (int i = 0; i < 4; ++i) ui.render();   // probes, capture, append
    CHECK(r.fd.appends > 0);
    const uint32_t total = ui.lastDlWords(), upload = ui.lastUploadWords();
    std::printf("  pattern: %u DL words, %u uploaded\n", unsigned(total), unsigned(upload));
    CHECK(total > upload + 40);
    const uint32_t over0 = ui.overBudgetFrames();
    ui.setDlFrameLimit(total + 20);
    ui.render();
    CHECK_EQ(ui.overBudgetFrames(), over0);
    Harness::dlOverflowFatal = false;   // the cut is expected here
    ui.setDlFrameLimit((total + upload) / 2);
    ui.render();
    Harness::dlOverflowFatal = true;
    CHECK(ui.overBudgetFrames() > over0);
    CHECK(ui.lastDlWords() <= (total + upload) / 2);
    ui.setDlFrameLimit(Ui::kDlFrameWords);
}
