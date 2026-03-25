#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/seqlock.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <thread>
#include <atomic>

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

TEST_CASE("SeqLock concurrent stress test", "[runtime][seqlock]") {
    SeqLock<TransportState> lock;
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
    TripleBuffer<TransportState> buf;
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
