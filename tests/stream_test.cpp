// SD sample streaming, engine side (audio/stream_source.*, docs/
// PLAN_SD_STREAMING.md P1): a streamed instrument (resident head + loop head,
// the rest through the track's ring) renders bit for bit like the same
// instrument in memory while the stream service keeps up -- mono and stereo,
// one-shot and forward loop, at several pitches; without the service the
// voice reads silence past its head and counts underruns; a note that cannot
// stream (other play modes) is not played from a partial sample.
#include "test.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "audio/engine.h"
#include "audio/stream_source.h"

using namespace ot;
using namespace ot::audio;

namespace {

constexpr uint32_t kLen = 300000;   // ~6.8 s

std::vector<int16_t> signal(uint32_t len, uint32_t seed) {
    std::vector<int16_t> d(len);
    uint32_t x = seed;
    for (uint32_t i = 0; i < len; ++i) {
        x = x * 1664525u + 1013904223u;
        d[i] = int16_t(int32_t(x >> 16) - 32768);   // full-band noise: any misplaced frame shows
    }
    return d;
}

// The card, on the host: reads from the full sample in memory.
struct MemReader : IStreamReader {
    const int16_t* l = nullptr;
    const int16_t* r = nullptr;
    int reads = 0;
    bool read(const StreamSource&, uint32_t frame, uint32_t n, int16_t* dl, int16_t* dr) override {
        std::memcpy(dl, l + frame, n * 2);
        if (dr && r) std::memcpy(dr, r + frame, n * 2);
        ++reads;
        return true;
    }
};

struct Rig {
    std::vector<int16_t> fx = std::vector<int16_t>(AudioEngine::kFxMemorySamples);
    std::vector<int16_t> pool = std::vector<int16_t>(2u << 20);
    std::unique_ptr<Instrument[]> ins = std::unique_ptr<Instrument[]>(new Instrument[kSampleInstrumentCount]);
    std::unique_ptr<AudioEngine> eng = std::unique_ptr<AudioEngine>(new AudioEngine());
    std::vector<int16_t> ringMem;
    StreamRing rings[kVoices];
    StreamSource src;
    MemReader reader;
    Rig() { CHECK(eng->init(fx.data(), fx.size(), pool.data(), pool.size(), ins.get())); }
    void load(const std::vector<int16_t>& l, const std::vector<int16_t>* r, PlayMode m, uint16_t lp1 = 0,
              uint16_t lp2 = 65535) {
        ins[0].lengthFrames = uint32_t(l.size());
        ins[0].playMode = uint8_t(m);
        ins[0].loopPoint1 = lp1;
        ins[0].loopPoint2 = lp2;
        std::vector<int16_t> both = l;
        if (r) both.insert(both.end(), r->begin(), r->end());
        CHECK(eng->bank().store(0, both.data(), uint32_t(l.size()), r != nullptr));
    }
    // Stream instrument 0: the head from the bank, the loop head at loop point 1.
    void stream(const std::vector<int16_t>& l, const std::vector<int16_t>* r) {
        ringMem.assign(size_t(kVoices) * 2 * kStreamRingFrames, 0);
        for (int v = 0; v < kVoices; ++v) {
            rings[v] = StreamRing{};
            rings[v].bufL = ringMem.data() + size_t(2 * v) * kStreamRingFrames;
            rings[v].bufR = rings[v].bufL + kStreamRingFrames;
        }
        eng->setStreamRings(rings);
        src = StreamSource{};
        src.headL = eng->bank().left(0);
        src.headR = r ? eng->bank().right(0) : nullptr;
        src.headFrames = kStreamHeadFrames;
        src.frames = uint32_t(l.size());
        const uint32_t ls = uint32_t(uint64_t(l.size()) * ins[0].loopPoint1 / 65535u);
        if (ins[0].playMode == uint8_t(PlayMode::ForwardLoop) && ls >= kStreamHeadFrames) {
            src.loopStart = ls;
            src.loopFrames = kStreamHeadFrames;
            src.loopL = eng->bank().left(0) + ls;
            src.loopR = r ? eng->bank().right(0) + ls : nullptr;
        }
        eng->setStreamSource(0, &src);
        reader.l = l.data();
        reader.r = r ? r->data() : nullptr;
    }
    void note(uint8_t n) {
        VoiceNoteOn on;
        on.instrument = 0;
        on.note = n;
        eng->voiceNoteOn(0, on);
    }
    std::vector<int16_t> render(int blocks, bool service) {
        std::vector<int16_t> out;
        int16_t b[kBlock * 2];
        for (int i = 0; i < blocks; ++i) {
            if (service && eng->streamRings()) streamService(eng->streamRings(), kVoices, reader, 2);
            eng->renderLive(b);
            out.insert(out.end(), b, b + kBlock * 2);
        }
        return out;
    }
};

// RAM vs streamed: the same note, the same output.
void sameAsRam(PlayMode m, bool stereo, uint8_t note, int blocks, uint16_t lp1 = 0, uint16_t lp2 = 65535) {
    const std::vector<int16_t> l = signal(kLen, 7), r = signal(kLen, 99);
    Rig ram, st;
    ram.load(l, stereo ? &r : nullptr, m, lp1, lp2);
    st.load(l, stereo ? &r : nullptr, m, lp1, lp2);
    st.stream(l, stereo ? &r : nullptr);
    ram.note(note);
    st.note(note);
    const std::vector<int16_t> a = ram.render(blocks, false), b = st.render(blocks, true);
    size_t firstDiff = a.size();
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) {
            firstDiff = i;
            break;
        }
    CHECK(firstDiff == a.size());
    if (firstDiff != a.size())
        std::printf("       mode %d stereo %d note %d: first difference at frame %zu (underruns %u)\n", int(m),
                    int(stereo), int(note), firstDiff / 2, unsigned(st.rings[0].underruns));
    CHECK_EQ(st.rings[0].underruns, 0u);
    CHECK(st.reader.reads > 0);
}

}  // namespace

TEST(stream_one_shot_matches_ram) {
    sameAsRam(PlayMode::OneShot, false, 60, 2600);   // rate 1: the whole sample and past its end
    sameAsRam(PlayMode::OneShot, false, 72, 1300);   // rate 2
    sameAsRam(PlayMode::OneShot, false, 79, 900);    // ~rate 3
    sameAsRam(PlayMode::OneShot, true, 60, 2600);    // planar stereo
    sameAsRam(PlayMode::OneShot, true, 84, 700);     // stereo, rate 4
}

TEST(stream_forward_loop_matches_ram) {
    // Loop region far past the head: the loop head covers each wrap.
    sameAsRam(PlayMode::ForwardLoop, false, 60, 4000, 30000, 50000);
    sameAsRam(PlayMode::ForwardLoop, true, 67, 4000, 30000, 50000);
    // Loop inside the head.
    sameAsRam(PlayMode::ForwardLoop, false, 60, 1500, 1000, 4000);
}

TEST(stream_without_service_underruns_after_the_head) {
    const std::vector<int16_t> l = signal(kLen, 3);
    Rig st;
    st.load(l, nullptr, PlayMode::OneShot);
    st.stream(l, nullptr);
    st.note(60);
    const std::vector<int16_t> out = st.render(600, false);   // ~76,800 frames: past the head
    // The head plays (the first ~32768 frames are not silent) ...
    bool head = false;
    for (size_t i = 0; i < 2 * 30000; ++i) head = head || out[i] != 0;
    CHECK(head);
    // ... then the ring is empty: silence and counted underruns, never a crash.
    bool quiet = true;
    for (size_t i = 2 * 40000; i < 2 * 70000; ++i) quiet = quiet && out[i] == 0;
    CHECK(quiet);
    CHECK(st.rings[0].underruns > 0);
}

TEST(stream_not_played_in_other_play_modes) {
    const std::vector<int16_t> l = signal(kLen, 5);
    Rig st;
    st.load(l, nullptr, PlayMode::Granular);
    st.stream(l, nullptr);
    st.note(60);
    const std::vector<int16_t> out = st.render(20, true);
    for (int16_t v : out) CHECK(v == 0);
    CHECK_EQ(st.reader.reads, 0);
}

TEST(stream_ring_follows_a_retrigger) {
    // A new note restarts the stream (new generation): still identical to RAM.
    const std::vector<int16_t> l = signal(kLen, 11);
    Rig ram, st;
    ram.load(l, nullptr, PlayMode::OneShot);
    st.load(l, nullptr, PlayMode::OneShot);
    st.stream(l, nullptr);
    ram.note(60);
    st.note(60);
    std::vector<int16_t> a = ram.render(500, false), b = st.render(500, true);
    ram.note(64);
    st.note(64);
    const std::vector<int16_t> a2 = ram.render(800, false), b2 = st.render(800, true);
    CHECK(a == b);
    CHECK(a2 == b2);
    CHECK_EQ(st.rings[0].underruns, 0u);
}

TEST(stream_underrun_fades_instead_of_clicking) {
    // A late card: the service stops, the ring runs dry, then the service
    // resumes. The output never jumps by more than a fade step's worth: the
    // gap decays to silence and the return ramps up (no full-scale step).
    std::vector<int16_t> l(kLen);
    for (uint32_t i = 0; i < kLen; ++i) l[i] = 20000;   // DC: a cut to 0 would be a 20000 step
    Rig st;
    st.load(l, nullptr, PlayMode::OneShot);
    st.stream(l, nullptr);
    st.note(60);
    std::vector<int16_t> out = st.render(200, true);    // in the ring, beyond the head
    const std::vector<int16_t> dry = st.render(1000, false);   // the ring (~1.5 s ahead) runs out
    const uint32_t under = st.rings[0].underruns;
    const std::vector<int16_t> back = st.render(400, true);    // the card is back
    out.insert(out.end(), dry.begin(), dry.end());
    out.insert(out.end(), back.begin(), back.end());
    CHECK(under > 0);
    int maxStep = 0;
    bool silent = false, loudAgain = false;
    for (size_t i = 2 * 1000; i < out.size(); i += 2) {   // past the note's own start
        const int d = std::abs(int(out[i]) - int(out[i - 2]));
        if (d > maxStep) maxStep = d;
        if (out[i] == 0 && i > 2 * 30000) silent = true;
        if (silent && out[i] == out[2 * 20000]) loudAgain = true;
    }
    CHECK(silent);
    CHECK(loudAgain);
    CHECK(out[2 * 20000] != 0);
    CHECK(maxStep < 2000);   // a 32-frame ramp: ~20000 / 32 per frame (x the gain)
    if (maxStep >= 2000) std::printf("       largest step %d\n", maxStep);
}
