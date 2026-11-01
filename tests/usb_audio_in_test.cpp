// [I] USB audio computer -> Tracker (#97): the ring and its feedback loop
// against a simulated computer (core/usb/usb_audio_in.h).
#include "test.h"

#include "usb/usb_audio_in.h"

using namespace ot;
using namespace ot::usbaudio;

namespace {

UsbAudioIn g_in;   // .bss, like on the device

constexpr double kCodecRate = 44117.647;

// The computer: one packet per 1 ms USB frame, sized from the feedback it
// last read (re-read every `refresh` frames, the endpoint's bRefresh), as
// Linux's snd-usb-audio does with an asynchronous OUT endpoint. Frames carry
// a counter (L = n, R = ~n) so the test can check nothing was lost.
struct Sim {
    UsbAudioIn& in;
    double hostPpm = 0.0, codecPpm = 0.0;
    int refresh = 32;
    bool followFeedback = true;
    double tHost = 0.0, tBlock = 0.0;   // next USB frame / next audio block, seconds
    uint32_t acc = 0, fbHost = kFeedbackNominal;
    long frame = 0;
    uint16_t sent = 0, expect = 0;
    bool sending = true, synced = false;
    long blocks = 0, silent = 0, gaps = 0;
    uint32_t minFill = ~0u, maxFill = 0;

    explicit Sim(UsbAudioIn& i) : in(i) {}

    void hostFrame() {
        if (frame++ % refresh == 0) fbHost = in.feedback();
        const uint32_t step = followFeedback ? fbHost : 722534;   // 44.1 frames / ms
        acc += step;
        const uint32_t n = acc >> 14;
        acc &= 0x3FFF;
        int16_t pkt[2 * 64];
        for (uint32_t i = 0; i < n; ++i, ++sent) {
            pkt[2 * i] = int16_t(sent);
            pkt[2 * i + 1] = int16_t(~sent);
        }
        if (sending) in.receive(pkt, n);
    }

    void audioBlock(bool measure) {
        int16_t out[2 * kBlockFrames];
        const int16_t* b = in.block(out);
        ++blocks;
        if (!b) {
            ++silent;
            synced = false;
            return;
        }
        CHECK(b == out);
        // A discontinuity counts once, then the check follows the stream again.
        bool ok = true;
        if (!synced) expect = uint16_t(out[0]);   // (re)start: any position
        for (uint32_t i = 0; i < kBlockFrames; ++i, ++expect) {
            const uint16_t v = uint16_t(out[2 * i]);
            ok = ok && uint16_t(out[2 * i + 1]) == uint16_t(~v);   // L and R from the same frame
            if (v != expect) {
                ok = false;
                expect = v;
            }
        }
        if (!ok) ++gaps;
        synced = true;
        if (measure) {
            const uint32_t f = in.fill();
            minFill = f < minFill ? f : minFill;
            maxFill = f > maxFill ? f : maxFill;
        }
    }

    // Runs `seconds`; statistics only after `settle` seconds.
    void run(double seconds, double settle = 0.0) {
        const double frameS = 1e-3 * (1.0 + hostPpm * 1e-6);
        const double blockS = double(kBlockFrames) / (kCodecRate * (1.0 + codecPpm * 1e-6));
        const double end = tHost + seconds, from = tHost + settle;
        while (tHost < end || tBlock < end) {
            if (tHost <= tBlock) {
                hostFrame();
                tHost += frameS;
            } else {
                audioBlock(tBlock >= from);
                tBlock += blockS;
            }
        }
    }
};

void reset() { g_in = UsbAudioIn{}; }

}  // namespace

TEST(silent_without_packets) {
    reset();
    int16_t out[2 * kBlockFrames];
    for (int i = 0; i < 10; ++i) CHECK(g_in.block(out) == nullptr);
    CHECK(!g_in.playing());
    CHECK_EQ(g_in.feedback(), kFeedbackNominal);
}

TEST(primes_then_plays_in_order) {
    reset();
    Sim s(g_in);
    s.run(0.2);
    CHECK(g_in.playing());
    CHECK(s.blocks - s.silent > 50);
    CHECK_EQ(s.gaps, 0L);
    // Start latency: the first block waits for target + one block.
    CHECK(s.silent <= 4);
}

// The case the loop exists for: the codec's 44117.6 Hz against a computer
// clocked for 44100, with crystal offsets both ways, for ten minutes.
TEST(locks_and_holds_ten_minutes) {
    const double ppm[][2] = {{0, 0}, {100, -100}, {-100, 100}, {50, 50}};
    for (const auto& p : ppm) {
        for (int refresh : {1, 32}) {
            reset();
            Sim s(g_in);
            s.hostPpm = p[0];
            s.codecPpm = p[1];
            s.refresh = refresh;
            s.run(600.0, 5.0);
            CHECK_EQ(g_in.underruns(), 0u);
            CHECK_EQ(g_in.dropped(), 0u);
            CHECK_EQ(s.gaps, 0L);
            // After settling the fill stays close to the target: never near
            // empty (an under-run) or the ring's end.
            CHECK(s.minFill >= kTarget - 80);
            CHECK(s.maxFill <= kTarget + 80);
            const int32_t d = int32_t(g_in.feedback()) - int32_t(kFeedbackNominal);
            CHECK(d > -int32_t(kFeedbackSpan) && d < int32_t(kFeedbackSpan));
        }
    }
}

TEST(stop_and_restart) {
    reset();
    Sim s(g_in);
    s.run(1.0);
    CHECK(g_in.playing());
    s.sending = false;
    s.run(0.05);
    CHECK(!g_in.playing());
    CHECK_EQ(g_in.fill(), 0u);
    const long silent = s.silent;
    s.sending = true;
    s.run(2.0, 0.5);
    CHECK(g_in.playing());
    CHECK(s.silent - silent <= 8);
    CHECK_EQ(s.gaps, 0L);
    CHECK_EQ(g_in.underruns(), 0u);
}

// A computer that ignores the feedback (macOS caps it below our rate): the
// ring drains, under-runs and recovers; never stuck, never out of order.
TEST(feedback_ignored_recovers) {
    reset();
    Sim s(g_in);
    s.followFeedback = false;
    s.run(120.0);
    CHECK(g_in.underruns() > 0u);
    CHECK(g_in.playing() || s.silent > 0);
    CHECK_EQ(s.gaps, 0L);
}

// The audio blocks pause (a project load) while the computer keeps sending:
// the ring fills; afterwards it skips back to the target instead of playing
// ~10 ms late, and keeps playing in order.
TEST(pause_skips_back_to_target) {
    reset();
    Sim s(g_in);
    s.run(2.0);
    CHECK(g_in.playing());
    for (int i = 0; i < 30; ++i) s.hostFrame();   // 30 ms of packets, no audio block
    CHECK(g_in.fill() > kTarget + kBlockFrames + kSkipAbove);
    s.tBlock = s.tHost;
    s.run(0.01);
    CHECK(g_in.fill() <= kTarget + kBlockFrames);
    CHECK(g_in.dropped() > 0u);
    // The ring overflowed during the pause: one hole where the frames that
    // didn't fit were, nothing else.
    CHECK(s.gaps <= 2);
    const long gaps = s.gaps;
    s.minFill = ~0u;
    s.maxFill = 0;
    s.run(10.0, 2.0);
    CHECK_EQ(s.gaps, gaps);
    CHECK_EQ(g_in.underruns(), 0u);
    CHECK(s.maxFill <= kTarget + 80);
}

TEST(overflow_drops_newest) {
    reset();
    int16_t pkt[2 * 45] = {};
    for (int i = 0; i < 20; ++i) g_in.receive(pkt, 45);
    CHECK_EQ(g_in.fill(), kRingFrames);
    CHECK_EQ(g_in.dropped(), 20u * 45u - kRingFrames);
}
