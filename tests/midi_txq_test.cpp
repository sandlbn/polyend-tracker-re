// MIDI TX queue (src/hal/midi_txq.h): bounded pumping and the MIDI clock
// latency the telemetry line reports (DIN clock out jitter, user report:
// clock to a Mother-32 / DFAM wobbles on screen changes and pad auditions).
#include "test.h"

#include <vector>

#include "hal/midi_txq.h"

using ot::hal::MidiTxQueue;

TEST(pump_hands_out_at_most_room_bytes_in_order) {
    static MidiTxQueue q;
    q = MidiTxQueue{};
    const uint8_t note[3] = {0x90, 60, 100};
    CHECK(q.push(note, 3, 0));
    std::vector<uint8_t> out;
    auto put = [&](uint8_t b) { out.push_back(b); };
    CHECK_EQ(q.pump(0, 0, put), 0u);
    CHECK_EQ(q.pump(2, 0, put), 2u);
    CHECK_EQ(q.pump(8, 0, put), 1u);
    CHECK(out == std::vector<uint8_t>({0x90, 60, 100}));
    CHECK_EQ(q.used(), 0u);
}

TEST(push_refuses_what_does_not_fit) {
    static MidiTxQueue q;
    q = MidiTxQueue{};
    static uint8_t big[MidiTxQueue::kSize];
    CHECK(q.push(big, MidiTxQueue::kSize, 0));
    const uint8_t one = 0xF8;
    CHECK(!q.push(&one, 1, 0));
    CHECK_EQ(q.used(), MidiTxQueue::kSize);
}

TEST(clock_latency_is_measured_from_queue_to_port) {
    static MidiTxQueue q;
    q = MidiTxQueue{};
    auto put = [](uint8_t) {};
    const uint8_t clk = 0xF8;
    CHECK(q.push(&clk, 1, 1000));
    q.pump(4, 1500, put);   // sent at the block end: 500 us
    CHECK(q.push(&clk, 1, 2000));
    CHECK(q.push(&clk, 1, 2100));
    q.pump(4, 32000, put);  // held by a 30 ms main-loop pass
    uint32_t late = 0, n = 0;
    q.takeClockStats(&late, &n);
    CHECK_EQ(late, 30000u);
    CHECK_EQ(n, 3u);
    q.takeClockStats(&late, &n);
    CHECK_EQ(late, 0u);
    CHECK_EQ(n, 0u);
}

TEST(clock_latency_counts_the_bytes_ahead_on_the_wire) {
    static MidiTxQueue q;
    q = MidiTxQueue{};
    auto put = [](uint8_t) {};
    const uint8_t msg[4] = {0x90, 60, 100, 0xF8};   // a note, then the clock
    CHECK(q.push(msg, 4, 1000));
    // Pumped at once, but 10 bytes already in the UART ring and the note's
    // 3 before it: the clock leaves 13 bytes (13 x 320 us) later.
    q.pump(64, 1000, put, 10, 320);
    uint32_t late = 0, n = 0;
    q.takeClockStats(&late, &n);
    CHECK_EQ(late, 13u * 320u);
    CHECK_EQ(n, 1u);
}

TEST(clock_queued_after_the_pump_read_the_time_counts_as_not_late) {
    // The pump reads the time, then an ISR queues a clock before the pump
    // masks IRQs: that clock's stamp is newer than the pump's time (device
    // log: "late max 4294967.279 ms").
    static MidiTxQueue q;
    q = MidiTxQueue{};
    auto put = [](uint8_t) {};
    const uint8_t clk = 0xF8;
    CHECK(q.push(&clk, 1, 5017));
    q.pump(64, 5000, put);
    uint32_t late = 0, n = 0;
    q.takeClockStats(&late, &n);
    CHECK_EQ(late, 0u);
    CHECK_EQ(n, 1u);
}
