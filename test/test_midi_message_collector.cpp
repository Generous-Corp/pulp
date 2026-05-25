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
    // violation). New impl stashes it in the consumer-owned pending
    // ring and leaves the queue empty.
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

TEST_CASE("MidiMessageCollector drains queue even when pending holds a far-future event (Codex #2845 P1)",
          "[midi][collector][regression]") {
    MidiMessageCollector<32> collector;

    // Push a far-future event first; it will land in pending after
    // the first drain.
    REQUIRE(collector.push_now(note_on(0, 60, 100), /*ts=*/10.000));
    MidiBuffer b1;
    REQUIRE(collector.drain_into(b1, /*start=*/0.000,
                                       /*samples=*/256, /*sr=*/48000.0) == 0);
    REQUIRE(collector.size_approx() == 0); // future event is in pending ring

    // Producer now pushes an event whose timestamp fits the next block.
    // Old impl: early-return on pending → queue NOT scanned → event lost.
    // New impl: pending is in the future, but the queue is still drained;
    // the in-block event must be delivered.
    REQUIRE(collector.push_now(note_on(0, 64, 90), /*ts=*/0.002));
    MidiBuffer b2;
    auto drained = collector.drain_into(b2, /*start=*/0.001,
                                              /*samples=*/256, /*sr=*/48000.0);
    REQUIRE(drained == 1);
    REQUIRE(b2.size() == 1);
    REQUIRE(b2[0].sample_offset == 48); // (0.002 - 0.001) * 48000
    REQUIRE(b2[0].message.getChannel0to15() == 0);
    REQUIRE(b2[0].message.getNoteNumber() == 64);

    // Far-future event still pending.
    MidiBuffer b3;
    REQUIRE(collector.drain_into(b3, /*start=*/0.5,
                                       /*samples=*/256, /*sr=*/48000.0) == 0);

    // Eventually crossing 10.000 s delivers it.
    MidiBuffer b4;
    REQUIRE(collector.drain_into(b4, /*start=*/9.999,
                                       /*samples=*/4800, /*sr=*/48000.0) == 1);
    REQUIRE(b4.size() == 1);
    REQUIRE(b4[0].message.getNoteNumber() == 60);
}

TEST_CASE("MidiMessageCollector pending ring handles multiple out-of-order future events",
          "[midi][collector][regression]") {
    MidiMessageCollector<32> collector;
    // Producer pushes 3 future events out of order.
    REQUIRE(collector.push_now(note_on(0, 60, 100), /*ts=*/5.000));
    REQUIRE(collector.push_now(note_on(0, 64, 100), /*ts=*/3.000));
    REQUIRE(collector.push_now(note_on(0, 67, 100), /*ts=*/4.000));

    // First drain at t=0: nothing fits, all three end up in pending ring.
    MidiBuffer b1;
    REQUIRE(collector.drain_into(b1, /*start=*/0.0,
                                       /*samples=*/256, /*sr=*/48000.0) == 0);

    // Drain at t=3.0: only the ts=3.0 event fits.
    MidiBuffer b2;
    REQUIRE(collector.drain_into(b2, /*start=*/3.0,
                                       /*samples=*/256, /*sr=*/48000.0) == 1);
    REQUIRE(b2[0].message.getNoteNumber() == 64);

    // Drain at t=4.0: ts=4.0 event fits.
    MidiBuffer b3;
    REQUIRE(collector.drain_into(b3, /*start=*/4.0,
                                       /*samples=*/256, /*sr=*/48000.0) == 1);
    REQUIRE(b3[0].message.getNoteNumber() == 67);

    // Drain at t=5.0: ts=5.0 event fits.
    MidiBuffer b4;
    REQUIRE(collector.drain_into(b4, /*start=*/5.0,
                                       /*samples=*/256, /*sr=*/48000.0) == 1);
    REQUIRE(b4[0].message.getNoteNumber() == 60);
}

TEST_CASE("MidiMessageCollector absorbs realistic future bursts within pending ring (Codex #2853 P1)",
          "[midi][collector][regression]") {
    MidiMessageCollector<128> collector;
    // Push 24 events all in the future — well within the 64-slot
    // pending ring. Earlier impl variants either silently dropped late
    // entries or starved later in-block events; current impl stashes
    // every future event in the consumer-owned ring with zero loss.
    constexpr int kEvents = 24;
    for (int i = 0; i < kEvents; ++i) {
        REQUIRE(collector.push_now(note_on(0, static_cast<uint8_t>(60 + i), 100),
                                    /*ts=*/10.0 + double(i) * 0.001));
    }

    // First drain at t=0: nothing fits. All 24 events land in the
    // pending ring with no loss because 24 <= pending_capacity() (64).
    MidiBuffer b1;
    REQUIRE(collector.drain_into(b1, /*start=*/0.0,
                                       /*samples=*/256, /*sr=*/48000.0) == 0);
    REQUIRE(collector.dropped_future() == 0);

    // Drain across multiple subsequent blocks until everything lands.
    int total_delivered = 0;
    for (int i = 0; i < 8 && total_delivered < kEvents; ++i) {
        MidiBuffer extra;
        total_delivered += static_cast<int>(
            collector.drain_into(extra, /*start=*/10.0,
                                         /*samples=*/static_cast<int>(0.5 * 48000),
                                         /*sr=*/48000.0));
    }
    // Zero loss — all events eventually delivered.
    REQUIRE(total_delivered == kEvents);
    REQUIRE(collector.dropped_future() == 0);
}

TEST_CASE("MidiMessageCollector keeps draining queue for in-block events even after a stash (Codex #2856 P1)",
          "[midi][collector][regression]") {
    MidiMessageCollector<128> collector;

    // Push 10 FUTURE events first, then 1 IN-BLOCK event behind them in
    // FIFO order. Earlier impl `break`-ed after a stash failure (which
    // was easy to hit with the 8-slot ring), starving the in-block
    // event. With the larger ring AND continue-drain semantics, every
    // future event stashes cleanly and the in-block event still gets
    // delivered in the SAME block.
    for (int i = 0; i < 10; ++i) {
        REQUIRE(collector.push_now(note_on(0, static_cast<uint8_t>(60 + i), 100),
                                    /*ts=*/10.0 + double(i) * 0.001));
    }
    REQUIRE(collector.push_now(note_on(0, 42, 100), /*ts=*/0.001));

    MidiBuffer b;
    auto drained = collector.drain_into(b, /*start=*/0.0,
                                          /*samples=*/256, /*sr=*/48000.0);
    REQUIRE(drained == 1);
    REQUIRE(b.size() == 1);
    REQUIRE(b[0].message.getNoteNumber() == 42);
    REQUIRE(b[0].sample_offset == 48); // (0.001 - 0.0) * 48000
    REQUIRE(collector.dropped_future() == 0);

    // Drain later blocks — all 10 future events should land.
    int delivered = 0;
    for (int i = 0; i < 8 && delivered < 10; ++i) {
        MidiBuffer extra;
        delivered += static_cast<int>(
            collector.drain_into(extra, /*start=*/10.0,
                                         /*samples=*/static_cast<int>(0.5 * 48000),
                                         /*sr=*/48000.0));
    }
    REQUIRE(delivered == 10);
    REQUIRE(collector.dropped_future() == 0);
}

TEST_CASE("MidiMessageCollector drops surplus future events when pending ring is genuinely saturated",
          "[midi][collector][regression]") {
    MidiMessageCollector<256> collector;
    // Push enough future events to overflow the 64-slot pending ring.
    constexpr int kRingPlusOne = 65;
    for (int i = 0; i < kRingPlusOne; ++i) {
        REQUIRE(collector.push_now(note_on(0, static_cast<uint8_t>(i & 0x7F), 100),
                                    /*ts=*/10.0 + double(i) * 0.001));
    }

    MidiBuffer b;
    REQUIRE(collector.drain_into(b, /*start=*/0.0,
                                       /*samples=*/256, /*sr=*/48000.0) == 0);
    REQUIRE(b.size() == 0);
    REQUIRE(collector.dropped_future() == 1); // exactly one beyond ring capacity

    // The 64 events that fit the ring should be deliverable.
    int delivered = 0;
    for (int i = 0; i < 8 && delivered < 64; ++i) {
        MidiBuffer extra;
        delivered += static_cast<int>(
            collector.drain_into(extra, /*start=*/10.0,
                                         /*samples=*/static_cast<int>(0.5 * 48000),
                                         /*sr=*/48000.0));
    }
    REQUIRE(delivered == 64);
    REQUIRE(collector.dropped_future() == 1); // counter monotonic
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
