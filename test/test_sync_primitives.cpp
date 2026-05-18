#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/seqlock.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <thread>
#include <atomic>
#include <cmath>

using namespace pulp::runtime;

// ── SeqLock tests ─────────────────────────────────────────────────────────

struct TransportState {
    double tempo = 120.0;
    double beat_position = 0.0;
    int time_sig_num = 4;
    int time_sig_den = 4;
};

TEST_CASE("SeqLock basic read/write", "[runtime][seqlock]") {
    SeqLock<TransportState> lock;

    TransportState st;
    st.tempo = 140.0;
    st.beat_position = 3.5;
    st.time_sig_num = 3;
    st.time_sig_den = 8;
    lock.write(st);

    auto result = lock.read();
    REQUIRE(result.tempo == 140.0);
    REQUIRE(result.beat_position == 3.5);
    REQUIRE(result.time_sig_num == 3);
    REQUIRE(result.time_sig_den == 8);
}

TEST_CASE("SeqLock explicit initial value is readable before first write",
          "[runtime][seqlock][coverage][phase3]") {
    TransportState init;
    init.tempo = 96.0;
    init.beat_position = 12.25;
    init.time_sig_num = 7;
    init.time_sig_den = 8;

    SeqLock<TransportState> lock(init);
    auto result = lock.read();

    REQUIRE(result.tempo == 96.0);
    REQUIRE(result.beat_position == 12.25);
    REQUIRE(result.time_sig_num == 7);
    REQUIRE(result.time_sig_den == 8);
}

TEST_CASE("SeqLock concurrent stress test", "[runtime][seqlock]") {
    TransportState init;
    init.beat_position = init.tempo * 0.5;
    SeqLock<TransportState> lock(init);
    std::atomic<bool> running{true};
    std::atomic<int> torn_reads{0};

    // Writer thread: rapidly update all fields together
    std::thread writer([&] {
        for (int i = 0; i < 100000; ++i) {
            TransportState st;
            st.tempo = static_cast<double>(i);
            st.beat_position = static_cast<double>(i) * 0.5;
            st.time_sig_num = i % 7 + 1;
            st.time_sig_den = 4;
            lock.write(st);
        }
        running = false;
    });

    // Reader thread: verify consistency
    std::thread reader([&] {
        while (running.load()) {
            auto st = lock.read();
            // Verify coherence: beat_position should be tempo * 0.5
            double expected_beat = st.tempo * 0.5;
            if (std::abs(st.beat_position - expected_beat) > 0.001) {
                torn_reads.fetch_add(1);
            }
        }
    });

    writer.join();
    reader.join();

    REQUIRE(torn_reads.load() == 0);
}

// ── TripleBuffer tests ────────────────────────────────────────────────────

TEST_CASE("TripleBuffer basic read/write", "[runtime][triple_buffer]") {
    TripleBuffer<int> buf(0);

    buf.write(42);
    REQUIRE(buf.read() == 42);

    buf.write(100);
    REQUIRE(buf.read() == 100);
}

TEST_CASE("TripleBuffer default and initial reads are stable without dirty swaps",
          "[runtime][triple_buffer][coverage][phase3]") {
    TripleBuffer<int> default_buf;
    REQUIRE(default_buf.read() == 0);
    REQUIRE(default_buf.read() == 0);

    struct Snapshot {
        float peak_l = 0.0f;
        float peak_r = 0.0f;
        int block_count = 0;
    };

    Snapshot initial;
    initial.peak_l = 0.75f;
    initial.peak_r = 0.5f;
    initial.block_count = 12;

    TripleBuffer<Snapshot> initialized(initial);
    const auto& first = initialized.read();
    REQUIRE(first.peak_l == 0.75f);
    REQUIRE(first.peak_r == 0.5f);
    REQUIRE(first.block_count == 12);

    const auto& second = initialized.read();
    REQUIRE(second.peak_l == 0.75f);
    REQUIRE(second.peak_r == 0.5f);
    REQUIRE(second.block_count == 12);
}

TEST_CASE("TripleBuffer reader gets latest value", "[runtime][triple_buffer]") {
    TripleBuffer<int> buf(0);

    // Write multiple values before reading
    buf.write(1);
    buf.write(2);
    buf.write(3);

    // Reader should get the latest
    REQUIRE(buf.read() == 3);
}

TEST_CASE("TripleBuffer concurrent stress test", "[runtime][triple_buffer]") {
    // Initialize with coherent state so early reads before first write are valid
    TransportState init;
    init.tempo = 0.0;
    init.beat_position = 0.0;
    init.time_sig_num = 1;
    init.time_sig_den = 4;
    TripleBuffer<TransportState> buf(init);
    std::atomic<bool> running{true};
    std::atomic<int> bad_reads{0};

    std::thread writer([&] {
        for (int i = 0; i < 100000; ++i) {
            TransportState st;
            st.tempo = static_cast<double>(i);
            st.beat_position = static_cast<double>(i) * 0.5;
            st.time_sig_num = i % 7 + 1;
            st.time_sig_den = 4;
            buf.write(st);
        }
        running = false;
    });

    std::thread reader([&] {
        while (running.load()) {
            auto& st = buf.read();
            // Coherence check
            double expected_beat = st.tempo * 0.5;
            if (std::abs(st.beat_position - expected_beat) > 0.001) {
                bad_reads.fetch_add(1);
            }
        }
    });

    writer.join();
    reader.join();

    REQUIRE(bad_reads.load() == 0);
}

// ── SpscQueue tests ───────────────────────────────────────────────────────

TEST_CASE("SpscQueue reports capacity, empty state, and FIFO order",
          "[runtime][spsc_queue][coverage]") {
    SpscQueue<int, 4> queue;

    REQUIRE(queue.capacity() == 4);
    REQUIRE(queue.empty());
    REQUIRE(queue.size_approx() == 0);
    REQUIRE_FALSE(queue.try_pop().has_value());

    REQUIRE(queue.try_push(10));
    REQUIRE(queue.try_push(20));
    REQUIRE_FALSE(queue.empty());
    REQUIRE(queue.size_approx() == 2);

    auto first = queue.try_pop();
    auto second = queue.try_pop();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(*first == 10);
    REQUIRE(*second == 20);
    REQUIRE(queue.empty());
}

TEST_CASE("SpscQueue rejects pushes when full and accepts after pop",
          "[runtime][spsc_queue][coverage]") {
    SpscQueue<int, 2> queue;

    REQUIRE(queue.try_push(1));
    REQUIRE(queue.try_push(2));
    REQUIRE_FALSE(queue.try_push(3));
    REQUIRE(queue.size_approx() == 2);

    auto popped = queue.try_pop();
    REQUIRE(popped.has_value());
    REQUIRE(*popped == 1);

    REQUIRE(queue.try_push(3));
    REQUIRE(*queue.try_pop() == 2);
    REQUIRE(*queue.try_pop() == 3);
    REQUIRE_FALSE(queue.try_pop().has_value());
}

TEST_CASE("SpscQueue supports rvalue item pushes",
          "[runtime][spsc_queue][coverage]") {
    struct MoveTracked {
        int value = 0;
    };

    SpscQueue<MoveTracked, 2> queue;
    REQUIRE(queue.try_push(MoveTracked{42}));

    auto item = queue.try_pop();
    REQUIRE(item.has_value());
    REQUIRE(item->value == 42);
    REQUIRE(queue.empty());
}

// ── Composed integration test ────────────────────────────────────────────
// Simulates the real audio→UI pipeline: audio thread writes to both
// TripleBuffer (meter data) and SpscQueue (events) while UI thread reads both.

struct MeterData {
    float peak_l = 0.0f;
    float peak_r = 0.0f;
    int block_count = 0;
};

struct UIEvent {
    int type = 0;     // 0=param_change, 1=note_on, 2=note_off
    int param_id = 0;
    float value = 0.0f;
};

TEST_CASE("Composed: TripleBuffer + SpscQueue concurrent pipeline", "[runtime][integration]") {
    TripleBuffer<MeterData> meter_buf;
    SpscQueue<UIEvent, 256> event_queue;
    std::atomic<bool> running{true};
    std::atomic<int> coherence_errors{0};
    std::atomic<int> events_received{0};

    // Audio thread: writes meter data and pushes events
    std::thread audio_thread([&] {
        for (int i = 0; i < 100000; ++i) {
            // Write coherent meter data
            MeterData m;
            m.peak_l = static_cast<float>(i) * 0.001f;
            m.peak_r = m.peak_l * 0.5f; // invariant: peak_r == peak_l * 0.5
            m.block_count = i;
            meter_buf.write(m);

            // Push events periodically
            if (i % 100 == 0) {
                UIEvent ev;
                ev.type = i % 3;
                ev.param_id = i / 100;
                ev.value = static_cast<float>(i);
                event_queue.try_push(ev); // may fail if full, that's OK
            }
        }
        running = false;
    });

    // UI thread: reads meter data and pops events
    std::thread ui_thread([&] {
        while (running.load(std::memory_order_relaxed)) {
            // Read latest meter data
            auto& m = meter_buf.read();
            // Check coherence: peak_r should be peak_l * 0.5
            if (m.block_count > 0) {
                float expected_r = m.peak_l * 0.5f;
                if (std::abs(m.peak_r - expected_r) > 0.001f) {
                    coherence_errors.fetch_add(1);
                }
            }

            // Drain event queue
            while (auto ev = event_queue.try_pop()) {
                events_received.fetch_add(1);
            }
        }

        // Final drain
        while (auto ev = event_queue.try_pop()) {
            events_received.fetch_add(1);
        }
    });

    audio_thread.join();
    ui_thread.join();

    REQUIRE(coherence_errors.load() == 0);
    REQUIRE(events_received.load() > 0); // should have received some events
}

TEST_CASE("Composed: SeqLock + TripleBuffer simultaneous access", "[runtime][integration]") {
    TransportState init;
    init.beat_position = init.tempo * 0.5;
    SeqLock<TransportState> transport(init);
    TripleBuffer<MeterData> meters;
    std::atomic<bool> running{true};
    std::atomic<int> bad_reads{0};

    // Audio thread: writes both transport and meter data
    std::thread audio_thread([&] {
        for (int i = 0; i < 100000; ++i) {
            TransportState ts;
            ts.tempo = 120.0 + static_cast<double>(i % 60);
            ts.beat_position = ts.tempo * 0.5;
            ts.time_sig_num = 4;
            ts.time_sig_den = 4;
            transport.write(ts);

            MeterData m;
            m.peak_l = static_cast<float>(i % 1000) * 0.001f;
            m.peak_r = m.peak_l;
            m.block_count = i;
            meters.write(m);
        }
        running = false;
    });

    // UI thread: reads both
    std::thread ui_thread([&] {
        while (running.load(std::memory_order_relaxed)) {
            auto ts = transport.read();
            if (std::abs(ts.beat_position - ts.tempo * 0.5) > 0.001) {
                bad_reads.fetch_add(1);
            }

            auto& m = meters.read();
            if (m.block_count > 0 && std::abs(m.peak_l - m.peak_r) > 0.001f) {
                bad_reads.fetch_add(1);
            }
        }
    });

    audio_thread.join();
    ui_thread.join();

    REQUIRE(bad_reads.load() == 0);
}
