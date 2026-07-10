// Tests for runtime::Slot<T> and runtime::Handoff<T>.
//
// The contract under test is one sentence: the audio thread never allocates,
// never blocks, and never frees. So the tests instrument destruction and assert
// on WHICH THREAD it happens, not merely that it happens.

#include <pulp/runtime/slot.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using pulp::runtime::Handoff;
using pulp::runtime::Slot;

namespace {

/// Records the thread that destroyed it, so a test can prove reclamation did
/// not happen on the reader/audio thread.
struct Tracked {
    explicit Tracked(int v) : value(v) { ++live(); }
    ~Tracked() {
        destroyed_on().store(std::this_thread::get_id());
        ++destroyed();
        --live();
    }

    int value;

    static std::atomic<int>& live() { static std::atomic<int> n{0}; return n; }
    static std::atomic<int>& destroyed() { static std::atomic<int> n{0}; return n; }
    static std::atomic<std::thread::id>& destroyed_on() {
        static std::atomic<std::thread::id> id{};
        return id;
    }
    static void reset() {
        live().store(0);
        destroyed().store(0);
        destroyed_on().store(std::thread::id{});
    }
};

}  // namespace

// ─────────────────────────── Slot<T> ───────────────────────────

TEST_CASE("Slot read() before any publish yields a falsy guard", "[runtime][slot]") {
    Slot<Tracked> slot;
    REQUIRE_FALSE(slot.has_value());
    auto pin = slot.read();
    REQUIRE_FALSE(static_cast<bool>(pin));
    REQUIRE(pin.get() == nullptr);
}

TEST_CASE("Slot publishes a value readers can see", "[runtime][slot]") {
    Tracked::reset();
    Slot<Tracked> slot;
    slot.publish(std::make_unique<Tracked>(42));
    REQUIRE(slot.has_value());

    auto pin = slot.read();
    REQUIRE(static_cast<bool>(pin));
    REQUIRE(pin->value == 42);
    REQUIRE((*pin).value == 42);
}

TEST_CASE("Slot does not reclaim while a reader is pinned", "[runtime][slot]") {
    Tracked::reset();
    Slot<Tracked> slot;
    slot.publish(std::make_unique<Tracked>(1));

    {
        auto pin = slot.read();           // pinned
        REQUIRE(pin->value == 1);

        slot.publish(std::make_unique<Tracked>(2));   // displaces #1
        REQUIRE(slot.retired_count() == 1);           // parked, NOT freed
        REQUIRE(Tracked::destroyed().load() == 0);

        REQUIRE(pin->value == 1);                     // old value still valid
    }                                                 // pin released

    slot.reclaim_if_quiescent();
    REQUIRE(slot.retired_count() == 0);
    REQUIRE(Tracked::destroyed().load() == 1);
}

TEST_CASE("Slot reclaims on the publisher thread, never the reader", "[runtime][slot]") {
    Tracked::reset();
    Slot<Tracked> slot;
    slot.publish(std::make_unique<Tracked>(1));

    const auto publisher_id = std::this_thread::get_id();

    // Catch2 assertion macros are not thread-safe; the worker records into an
    // atomic and the main thread asserts after the join.
    std::atomic<int> seen{-1};
    std::thread reader([&] {
        auto pin = slot.read();
        seen.store(pin ? pin->value : -1);
    });
    reader.join();
    REQUIRE(seen.load() == 1);

    slot.publish(std::make_unique<Tracked>(2));  // publisher thread reclaims #1 here
    REQUIRE(Tracked::destroyed().load() == 1);
    REQUIRE(Tracked::destroyed_on().load() == publisher_id);
}

TEST_CASE("Slot survives an N-reader hammer against a hot publisher", "[runtime][slot]") {
    Tracked::reset();
    {
        Slot<Tracked> slot;
        slot.publish(std::make_unique<Tracked>(0));

        constexpr int kReaders = 4;
        constexpr int kPublishes = 2000;
        std::atomic<bool> stop{false};
        std::atomic<long long> reads{0};
        std::atomic<int> corrupt{0};

        std::vector<std::thread> readers;
        readers.reserve(kReaders);
        for (int i = 0; i < kReaders; ++i) {
            readers.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    if (auto pin = slot.read()) {
                        // Any published value is in range. A torn/freed read
                        // would show up here (and under ASan/TSan as a fault).
                        const int v = pin->value;
                        if (v < 0 || v > kPublishes) corrupt.fetch_add(1);
                        reads.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (int i = 1; i <= kPublishes; ++i)
            slot.publish(std::make_unique<Tracked>(i));

        stop.store(true);
        for (auto& t : readers) t.join();

        REQUIRE(corrupt.load() == 0);
        REQUIRE(reads.load() > 0);

        slot.wait_and_clear();
        REQUIRE(slot.retired_count() == 0);
    }
    // The Slot destructor released the live value too.
    REQUIRE(Tracked::live().load() == 0);
}

TEST_CASE("Slot pins lifetime, not constness — the interior stays mutable",
          "[runtime][slot]") {
    // SignalGraph's CompiledGraph is published through a Slot and written
    // through the pin every block (scratch buffers, MIDI mailboxes, telemetry).
    // A pin must therefore hand back a mutable pointer.
    struct Interior { int scratch = 0; };
    Slot<Interior> slot;
    slot.publish(std::make_unique<Interior>());

    {
        auto pin = slot.read();
        REQUIRE(pin);
        pin->scratch = 7;                  // writing through the pin must compile
        static_assert(std::is_same_v<decltype(pin.get()), Interior*>);
    }
    REQUIRE(slot.read()->scratch == 7);
}

TEST_CASE("Slot<const T> hands back a const pointer", "[runtime][slot]") {
    Slot<const int> slot;
    slot.publish(std::make_unique<const int>(5));
    auto pin = slot.read();
    REQUIRE(pin);
    REQUIRE(*pin == 5);
    static_assert(std::is_same_v<decltype(pin.get()), const int*>);
}

TEST_CASE("Slot unpublish drops the value and reclaims it off the reader",
          "[runtime][slot]") {
    Tracked::reset();
    Slot<Tracked> slot;
    slot.publish(std::make_unique<Tracked>(1));
    REQUIRE(slot.has_value());

    {
        auto pin = slot.read();       // pinned: cannot reclaim yet
        slot.unpublish();
        REQUIRE_FALSE(slot.has_value());
        REQUIRE_FALSE(slot.read());   // new readers see nothing
        REQUIRE(pin->value == 1);     // the old pin stays valid
        REQUIRE(Tracked::destroyed().load() == 0);
    }
    slot.reclaim_if_quiescent();
    REQUIRE(Tracked::destroyed().load() == 1);
    REQUIRE(slot.retired_count() == 0);
}

TEST_CASE("Slot::live() exposes the published shared_ptr to the publisher",
          "[runtime][slot]") {
    Tracked::reset();
    Slot<Tracked> slot;
    REQUIRE(slot.live() == nullptr);
    slot.publish(std::make_unique<Tracked>(3));
    REQUIRE(slot.live() != nullptr);
    REQUIRE(slot.live()->value == 3);
}

// ─────────────────────────── Handoff<T> ───────────────────────────

TEST_CASE("Handoff consume yields the published value", "[runtime][handoff]") {
    Tracked::reset();
    Handoff<Tracked> h;
    REQUIRE_FALSE(h.has_pending());

    REQUIRE(h.publish(std::make_unique<Tracked>(7)));
    REQUIRE(h.has_pending());

    auto got = h.try_consume();
    REQUIRE(got != nullptr);
    REQUIRE(got->value == 7);
    REQUIRE_FALSE(h.has_pending());
    REQUIRE(h.try_consume() == nullptr);
}

TEST_CASE("Handoff publish rejects null and frees an unconsumed value off-RT",
          "[runtime][handoff]") {
    Tracked::reset();
    const auto producer_id = std::this_thread::get_id();
    {
        Handoff<Tracked> h;
        REQUIRE_FALSE(h.publish(nullptr));

        REQUIRE(h.publish(std::make_unique<Tracked>(1)));
        REQUIRE(h.publish(std::make_unique<Tracked>(2)));  // displaces #1 inline

        REQUIRE(Tracked::destroyed().load() == 1);
        REQUIRE(Tracked::destroyed_on().load() == producer_id);

        auto got = h.try_consume();
        REQUIRE(got->value == 2);
    }
    REQUIRE(Tracked::live().load() == 0);
}

TEST_CASE("Handoff retire parks the displaced value; consumer never frees",
          "[runtime][handoff]") {
    Tracked::reset();
    Handoff<Tracked> h;
    h.publish(std::make_unique<Tracked>(1));
    auto current = h.try_consume();
    REQUIRE(current->value == 1);

    h.publish(std::make_unique<Tracked>(2));
    REQUIRE(h.has_retire_capacity());
    auto next = h.try_consume();
    auto displaced = std::exchange(current, std::move(next));

    const int before = Tracked::destroyed().load();
    REQUIRE(h.retire(displaced));
    REQUIRE(displaced == nullptr);                     // ownership moved to the ring
    REQUIRE(Tracked::destroyed().load() == before);    // nothing freed on this path
    REQUIRE(h.has_retired());

    REQUIRE(h.drain_retired() == 1);                   // freed here, off-RT
    REQUIRE(Tracked::destroyed().load() == before + 1);
    REQUIRE_FALSE(h.has_retired());
}

TEST_CASE("Handoff retire fails closed when the ring is full", "[runtime][handoff]") {
    Tracked::reset();
    Handoff<Tracked, 2> h;
    REQUIRE(h.retire_capacity() == 2);

    std::vector<std::unique_ptr<Tracked>> parked;
    for (int i = 0; i < 2; ++i) {
        REQUIRE(h.has_retire_capacity());
        auto p = std::make_unique<Tracked>(i);
        REQUIRE(h.retire(p));
        REQUIRE(p == nullptr);
    }

    REQUIRE_FALSE(h.has_retire_capacity());
    auto overflow = std::make_unique<Tracked>(99);
    REQUIRE_FALSE(h.retire(overflow));
    REQUIRE(overflow != nullptr);          // caller STILL owns it — no leak, no free
    REQUIRE(overflow->value == 99);

    REQUIRE(h.drain_retired() == 2);
    REQUIRE(h.has_retire_capacity());
}

TEST_CASE("Handoff retire(null) is a no-op that succeeds", "[runtime][handoff]") {
    Handoff<Tracked> h;
    std::unique_ptr<Tracked> empty;
    REQUIRE(h.retire(empty));
    REQUIRE_FALSE(h.has_retired());
}

TEST_CASE("Handoff survives a producer/consumer hammer with no audio-thread free",
          "[runtime][handoff]") {
    Tracked::reset();
    const auto producer_id = std::this_thread::get_id();
    {
        Handoff<Tracked> h;
        std::atomic<bool> stop{false};
        std::atomic<int> consumed{0};
        std::atomic<int> corrupt{0};
        constexpr int kPublishes = 5000;

        // The consumer must never pop the retire ring: it is SPSC, and the
        // producer is its only reader. So the consumer parks its final value
        // here for the main thread to reclaim after the join.
        std::unique_ptr<Tracked> leftover;

        // Consumer plays the audio thread: consume + retire only, never free.
        std::thread consumer([&] {
            std::unique_ptr<Tracked> current;
            while (!stop.load(std::memory_order_relaxed)) {
                if (h.has_pending() && h.has_retire_capacity()) {
                    if (auto next = h.try_consume()) {
                        if (next->value < 0 || next->value > kPublishes) corrupt.fetch_add(1);
                        auto displaced = std::exchange(current, std::move(next));
                        if (!h.retire(displaced)) {
                            // Ring full despite the gate: keep it, never free here.
                            current = std::move(displaced);
                        }
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            leftover = std::move(current);  // handed off, not destroyed
        });

        for (int i = 1; i <= kPublishes; ++i) {
            h.publish(std::make_unique<Tracked>(i));
            h.drain_retired();  // producer reclaims
        }
        stop.store(true);
        consumer.join();
        h.drain_retired();
        leftover.reset();  // freed on the main (producer) thread

        REQUIRE(corrupt.load() == 0);
        REQUIRE(consumed.load() > 0);
        // Every destructor that ran, ran on the producer thread. The consumer
        // thread only ever moved pointers.
        REQUIRE(Tracked::destroyed_on().load() == producer_id);
    }
    REQUIRE(Tracked::live().load() == 0);
}
