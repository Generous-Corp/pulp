// Tests for pulp::runtime::AbstractFifo — wrap-around correctness,
// capacity/empty/full boundaries, prepare/finish contract, and an SPSC
// hammer that drives the producer + consumer on separate threads to assert
// every item arrives in order with no loss or duplication.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <vector>

#include <pulp/runtime/abstract_fifo.hpp>

using pulp::runtime::AbstractFifo;

namespace {

// Convenience: write `count` items (values starting at `seed`) into `buf` via
// the fifo, return how many were actually written (free space may clamp it).
int write_via_fifo(AbstractFifo& fifo, std::vector<int>& buf,
                   int count, int seed) {
    int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
    fifo.prepare_to_write(count, s1, n1, s2, n2);
    for (int i = 0; i < n1; ++i) buf[s1 + i] = seed + i;
    for (int i = 0; i < n2; ++i) buf[s2 + i] = seed + n1 + i;
    fifo.finish_write(n1 + n2);
    return n1 + n2;
}

// Convenience: read up to `count` items into `out`. Returns # read.
int read_via_fifo(AbstractFifo& fifo, const std::vector<int>& buf,
                  std::vector<int>& out, int count) {
    int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
    fifo.prepare_to_read(count, s1, n1, s2, n2);
    for (int i = 0; i < n1; ++i) out.push_back(buf[s1 + i]);
    for (int i = 0; i < n2; ++i) out.push_back(buf[s2 + i]);
    fifo.finish_read(n1 + n2);
    return n1 + n2;
}

}  // namespace

TEST_CASE("AbstractFifo starts empty with full free_space minus reserved slot",
          "[runtime][abstract_fifo]") {
    AbstractFifo fifo(8);
    REQUIRE(fifo.capacity() == 8);
    REQUIRE(fifo.num_ready() == 0);
    REQUIRE(fifo.free_space() == 7);  // one slot reserved as sentinel
}

TEST_CASE("AbstractFifo prepare_to_write within contiguous range",
          "[runtime][abstract_fifo]") {
    AbstractFifo fifo(8);
    int s1 = -1, n1 = -1, s2 = -1, n2 = -1;
    fifo.prepare_to_write(3, s1, n1, s2, n2);
    REQUIRE(s1 == 0);
    REQUIRE(n1 == 3);
    REQUIRE(n2 == 0);
}

TEST_CASE("AbstractFifo prepare_to_write wraps around the buffer end",
          "[runtime][abstract_fifo]") {
    AbstractFifo fifo(8);
    std::vector<int> buf(8, 0);

    // Push the write cursor to position 6, then drain reads so free space
    // is the full capacity-1 again but the write cursor is at offset 6.
    REQUIRE(write_via_fifo(fifo, buf, 6, 1000) == 6);
    std::vector<int> out;
    REQUIRE(read_via_fifo(fifo, buf, out, 6) == 6);
    REQUIRE(fifo.num_ready() == 0);
    REQUIRE(fifo.free_space() == 7);

    int s1 = -1, n1 = -1, s2 = -1, n2 = -1;
    fifo.prepare_to_write(5, s1, n1, s2, n2);
    REQUIRE(s1 == 6);
    REQUIRE(n1 == 2);   // slots 6, 7
    REQUIRE(s2 == 0);
    REQUIRE(n2 == 3);   // wraps to 0, 1, 2
}

TEST_CASE("AbstractFifo full state clamps writes to zero",
          "[runtime][abstract_fifo]") {
    AbstractFifo fifo(4);
    std::vector<int> buf(4, 0);
    REQUIRE(write_via_fifo(fifo, buf, 3, 100) == 3);
    REQUIRE(fifo.num_ready() == 3);
    REQUIRE(fifo.free_space() == 0);

    int s1 = -1, n1 = -1, s2 = -1, n2 = -1;
    fifo.prepare_to_write(2, s1, n1, s2, n2);
    REQUIRE(n1 == 0);
    REQUIRE(n2 == 0);
}

TEST_CASE("AbstractFifo prepare_to_write clamps overshoot to free_space",
          "[runtime][abstract_fifo]") {
    AbstractFifo fifo(8);
    std::vector<int> buf(8, 0);
    REQUIRE(write_via_fifo(fifo, buf, 100, 0) == 7);  // capacity-1
}

TEST_CASE("AbstractFifo empty state clamps reads to zero",
          "[runtime][abstract_fifo]") {
    AbstractFifo fifo(8);
    int s1 = -1, n1 = -1, s2 = -1, n2 = -1;
    fifo.prepare_to_read(4, s1, n1, s2, n2);
    REQUIRE(n1 == 0);
    REQUIRE(n2 == 0);
}

TEST_CASE("AbstractFifo write+read round-trip preserves order across a wrap",
          "[runtime][abstract_fifo]") {
    AbstractFifo fifo(6);
    std::vector<int> buf(6, 0);
    std::vector<int> out;

    // Write 5, read 5 — cursors now both at 5.
    REQUIRE(write_via_fifo(fifo, buf, 5, 10) == 5);
    REQUIRE(read_via_fifo(fifo, buf, out, 5) == 5);

    // Write 4 more — this MUST wrap (only 1 slot at the tail).
    REQUIRE(write_via_fifo(fifo, buf, 4, 100) == 4);
    REQUIRE(fifo.num_ready() == 4);

    out.clear();
    REQUIRE(read_via_fifo(fifo, buf, out, 4) == 4);
    REQUIRE(out == std::vector<int>{100, 101, 102, 103});
}

TEST_CASE("AbstractFifo finish_* with non-positive args is a no-op",
          "[runtime][abstract_fifo]") {
    AbstractFifo fifo(4);
    fifo.finish_write(0);
    fifo.finish_write(-5);
    fifo.finish_read(0);
    fifo.finish_read(-5);
    REQUIRE(fifo.num_ready() == 0);
    REQUIRE(fifo.free_space() == 3);
}

TEST_CASE("AbstractFifo reset returns cursors to zero",
          "[runtime][abstract_fifo]") {
    AbstractFifo fifo(8);
    std::vector<int> buf(8, 0);
    REQUIRE(write_via_fifo(fifo, buf, 4, 1) == 4);
    REQUIRE(fifo.num_ready() == 4);
    fifo.reset();
    REQUIRE(fifo.num_ready() == 0);
    REQUIRE(fifo.free_space() == 7);
}

TEST_CASE("AbstractFifo SPSC hammer — one producer, one consumer",
          "[runtime][abstract_fifo][hammer]") {
    constexpr int kCapacity = 64;
    constexpr int kTotal = 200'000;

    AbstractFifo fifo(kCapacity);
    std::vector<int> buf(kCapacity, 0);
    std::atomic<bool> producer_done{false};

    std::vector<int> consumed;
    consumed.reserve(kTotal);

    std::thread producer([&] {
        int sent = 0;
        while (sent < kTotal) {
            const int want = std::min(17, kTotal - sent);  // odd chunk → forces wraps
            int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
            fifo.prepare_to_write(want, s1, n1, s2, n2);
            for (int i = 0; i < n1; ++i) buf[s1 + i] = sent + i;
            for (int i = 0; i < n2; ++i) buf[s2 + i] = sent + n1 + i;
            fifo.finish_write(n1 + n2);
            sent += n1 + n2;
            if (n1 + n2 == 0) std::this_thread::yield();
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        while (true) {
            int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
            fifo.prepare_to_read(23, s1, n1, s2, n2);  // different chunk → tests both wrap sides
            for (int i = 0; i < n1; ++i) consumed.push_back(buf[s1 + i]);
            for (int i = 0; i < n2; ++i) consumed.push_back(buf[s2 + i]);
            fifo.finish_read(n1 + n2);
            if (n1 + n2 == 0) {
                if (producer_done.load(std::memory_order_acquire)
                    && fifo.num_ready() == 0) {
                    break;
                }
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    REQUIRE(static_cast<int>(consumed.size()) == kTotal);
    for (int i = 0; i < kTotal; ++i) {
        REQUIRE(consumed[i] == i);
    }
}
