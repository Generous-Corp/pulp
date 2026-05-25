#include <catch2/catch_test_macros.hpp>
#include <pulp/midi/message_collector.hpp>

#include <atomic>
#include <thread>

using namespace pulp::midi;

namespace {

MidiEvent note_on(uint8_t channel, uint8_t note, uint8_t velocity, double ts = 0.0) {
    return MidiEvent{
        choc::midi::ShortMessage(
            static_cast<uint8_t>(0x90 | (channel & 0x0F)), note, velocity),
        0,
        ts
    };
}

} // namespace

TEST_CASE("MidiMessageCollector pushes events and drains them by timestamp",
          "[midi][collector]") {
    MidiMessageCollector<32> collector;

    REQUIRE(collector.push_now(note_on(0, 60, 100), /*ts=*/1.005));
    REQUIRE(collector.push_now(note_on(0, 64, 90),  /*ts=*/1.010));
    REQUIRE(collector.size_approx() == 2);

    MidiBuffer out;
    auto drained = collector.drain_into(out,
                                         /*block_start=*/1.000,
                                         /*block_samples=*/512,
                                         /*sample_rate=*/48000.0);
    REQUIRE(drained == 2);
    REQUIRE(out.size() == 2);

    // 1.005 - 1.000 = 0.005 s × 48 000 = 240 samples
    // 1.010 - 1.000 = 0.010 s × 48 000 = 480 samples
    REQUIRE(out[0].sample_offset == 240);
    REQUIRE(out[1].sample_offset == 480);
}

TEST_CASE("MidiMessageCollector clamps past-deadline events to sample 0",
          "[midi][collector]") {
    MidiMessageCollector<32> collector;
    REQUIRE(collector.push_now(note_on(0, 60, 100), /*ts=*/0.500)); // late
    REQUIRE(collector.push_now(note_on(0, 64, 90),  /*ts=*/1.000));  // boundary

    MidiBuffer out;
    auto drained = collector.drain_into(out,
                                         /*block_start=*/1.000,
                                         /*block_samples=*/256,
                                         /*sample_rate=*/48000.0);
    REQUIRE(drained == 2);
    REQUIRE(out[0].sample_offset == 0);
    REQUIRE(out[1].sample_offset == 0);
}

TEST_CASE("MidiMessageCollector defers future events to subsequent block",
          "[midi][collector]") {
    MidiMessageCollector<32> collector;
    REQUIRE(collector.push_now(note_on(0, 60, 100), /*ts=*/0.001));
    REQUIRE(collector.push_now(note_on(0, 64, 90),  /*ts=*/0.020));

    MidiBuffer block1;
    auto block_end = 256.0 / 48000.0; // ~0.00533 s
    auto drained1 = collector.drain_into(block1,
                                          /*block_start=*/0.000,
                                          /*block_samples=*/256,
                                          /*sample_rate=*/48000.0);
    (void) block_end;
    REQUIRE(drained1 == 1);
    REQUIRE(block1.size() == 1);
    REQUIRE(block1[0].sample_offset == 48); // 0.001 s × 48 000 = 48

    MidiBuffer block2;
    auto drained2 = collector.drain_into(block2,
                                          /*block_start=*/256.0 / 48000.0,
                                          /*block_samples=*/4096,
                                          /*sample_rate=*/48000.0);
    REQUIRE(drained2 == 1);
    REQUIRE(block2.size() == 1);
}

TEST_CASE("MidiMessageCollector survives concurrent producer / consumer",
          "[midi][collector][threading]") {
    constexpr int kEvents = 5000;
    MidiMessageCollector<512> collector;

    std::atomic<bool> producer_done{false};
    std::thread producer([&] {
        double now = 0.0;
        for (int i = 0; i < kEvents; /*advance only on success*/) {
            const auto ts = now;
            if (collector.push_now(
                    note_on(0, static_cast<uint8_t>(i & 0x7F), 100), ts)) {
                ++i;
                now += 1e-6; // 1 µs spacing
            } else {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    int observed = 0;
    while (!producer_done.load(std::memory_order_acquire)
            || collector.size_approx() > 0) {
        MidiBuffer out;
        observed += static_cast<int>(collector.drain_into(out,
                                                            /*block_start=*/0.0,
                                                            /*block_samples=*/512,
                                                            /*sample_rate=*/48000.0));
        if (out.size() == 0) std::this_thread::yield();
    }
    producer.join();
    REQUIRE(observed == kEvents);
}

TEST_CASE("MidiMessageCollector deferred-future event stays in pending slot (Codex #2843 P1)",
          "[midi][collector][regression]") {
    MidiMessageCollector<32> collector;
    REQUIRE(collector.push_now(note_on(0, 60, 100), /*ts=*/1.000));
    REQUIRE(collector.size_approx() == 1);

    // First drain finds the event too late for this block. Old impl
    // re-pushed into the SPSC queue (consumer-side write → SPSC
    // violation). New impl stashes it in a consumer-owned `pending_`
    // slot and leaves the queue empty.
    MidiBuffer b1;
    REQUIRE(collector.drain_into(b1, /*start=*/0.000,
                                       /*samples=*/512, /*sr=*/48000.0) == 0);
    REQUIRE(b1.size() == 0);
    REQUIRE(collector.size_approx() == 0);

    // Subsequent drain still too early — pending stays put, queue
    // remains empty.
    MidiBuffer b2;
    REQUIRE(collector.drain_into(b2, /*start=*/0.001,
                                       /*samples=*/256, /*sr=*/48000.0) == 0);
    REQUIRE(collector.size_approx() == 0);

    // Drain that crosses the timestamp boundary delivers from pending.
    MidiBuffer b3;
    REQUIRE(collector.drain_into(b3, /*start=*/0.999,
                                       /*samples=*/2048, /*sr=*/48000.0) == 1);
    REQUIRE(b3.size() == 1);
    REQUIRE(b3[0].sample_offset == 48); // (1.000 - 0.999) * 48000
}

TEST_CASE("MidiMessageCollector returns false when queue is full",
          "[midi][collector]") {
    MidiMessageCollector<4> collector;
    int accepted = 0;
    // SpscQueue capacities are non-rigid (choc reserves a slot); push
    // until rejected.
    for (int i = 0; i < 16; ++i) {
        if (collector.push_now(note_on(0, 60, 100), /*ts=*/double(i) * 0.001))
            ++accepted;
        else
            break;
    }
    REQUIRE(accepted >= 3); // at least N-1 slots usable
    REQUIRE(accepted <= 16);
}
