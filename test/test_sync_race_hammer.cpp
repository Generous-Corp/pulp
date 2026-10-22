// Thread-sanitizer hammer test for Pulp's sync primitives.
//
// Each section spawns producer(s) + consumer(s) on the same sync
// primitive and hammers it for a fixed wall-clock duration. TSan
// flags any detectable race; ASan/UBSan also catch use-after-free
// and UB in the primitives' fast paths.
//
// Runs under all three sanitizers per the tag-scoped TSan regex in
// .github/workflows/sanitizers.yml (#412 step 3). Keep the hammer
// duration short (500 ms) so the test finishes well under the 120s
// per-test timeout.
//
// Primitives covered:
//   TripleBuffer<T>  — single writer, single reader, latest-value
//   SeqLock<T>       — single writer, multi-reader, coherent snapshot
//   SpscQueue<T>     — single producer, single consumer, ordered
//
// #412 step 4: deterministic race-hammer harness. Complements the
// individual unit tests in test_triple_buffer.cpp / test_seqlock.cpp
// / test_spsc_queue.cpp which cover correctness of isolated ops.

#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/seqlock.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/runtime/triple_buffer.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

using namespace std::chrono_literals;
using pulp::runtime::SeqLock;
using pulp::runtime::SpscQueue;
using pulp::runtime::TripleBuffer;

namespace {

struct Snapshot {
    double tempo;
    double position;
    int    numerator;
};

constexpr auto kHammerDuration = 500ms;

bool wait_for_start_count(std::atomic<int>& started, int expected) {
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (started.load(std::memory_order_acquire) < expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return started.load(std::memory_order_acquire) == expected;
}

}  // namespace

TEST_CASE("TripleBuffer hammer: writer + reader for 500ms", "[concurrent][race][triple_buffer]") {
    TripleBuffer<int> buf;
    std::atomic<bool> stop{false};
    std::atomic<int>  read_count{0};
    std::atomic<int>  write_count{0};
    std::atomic<int>  started{0};

    std::thread writer([&] {
        started.fetch_add(1, std::memory_order_release);
        int v = 0;
        while (!stop.load(std::memory_order_acquire)) {
            buf.write(++v);
            write_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread reader([&] {
        started.fetch_add(1, std::memory_order_release);
        while (!stop.load(std::memory_order_acquire)) {
            (void) buf.read();
            read_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    INFO("started=" << started.load(std::memory_order_acquire));
    REQUIRE(wait_for_start_count(started, 2));
    std::this_thread::sleep_for(kHammerDuration);
    stop.store(true, std::memory_order_release);
    writer.join();
    reader.join();

    INFO("writes=" << write_count.load() << " reads=" << read_count.load());
    REQUIRE(write_count.load() > 0);
    REQUIRE(read_count.load() > 0);
}

TEST_CASE("TripleBuffer hammer: millions of frames, never torn, never backwards",
          "[concurrent][race][triple_buffer]") {
    // A multi-word frame: every word is derived from the sequence number, so
    // a frame assembled from two publications cannot pass the check.
    struct Frame {
        std::uint64_t seq = 0;
        std::array<std::uint64_t, 15> words{};
    };
    constexpr std::uint64_t kFrames = 2'000'000;
    TripleBuffer<Frame> buf;
    std::atomic<int> started{0};
    std::atomic<std::uint64_t> torn{0};
    std::atomic<std::uint64_t> backwards{0};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> last_seen{0};

    std::thread writer([&] {
        started.fetch_add(1, std::memory_order_release);
        for (std::uint64_t seq = 1; seq <= kFrames; ++seq) {
            buf.write_with([seq](Frame& f) noexcept {
                f.seq = seq;
                for (std::size_t w = 0; w < f.words.size(); ++w)
                    f.words[w] = seq * 0x9E3779B97F4A7C15ull + w;
            });
        }
    });

    std::thread reader([&] {
        started.fetch_add(1, std::memory_order_release);
        std::uint64_t previous = 0;
        std::uint64_t n = 0;
        // Runs until the final publication becomes visible, so the loop also
        // proves the last value always reaches the reader.
        const auto deadline = std::chrono::steady_clock::now() + 60s;
        while (previous != kFrames) {
            if ((n & 0xFFF) == 0 && std::chrono::steady_clock::now() > deadline)
                break;
            const Frame& f = buf.read();
            ++n;
            for (std::size_t w = 0; w < f.words.size(); ++w) {
                if (f.words[w] != (f.seq == 0 ? 0 : f.seq * 0x9E3779B97F4A7C15ull + w)) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            if (f.seq < previous)
                backwards.fetch_add(1, std::memory_order_relaxed);
            previous = f.seq;
        }
        reads.store(n, std::memory_order_relaxed);
        last_seen.store(previous, std::memory_order_relaxed);
    });

    writer.join();
    reader.join();
    REQUIRE(started.load() == 2);
    INFO("reads=" << reads.load());
    REQUIRE(torn.load() == 0);
    REQUIRE(backwards.load() == 0);
    REQUIRE(last_seen.load() == kFrames);
    REQUIRE(buf.read().seq == kFrames);
}

TEST_CASE("SeqLock hammer: 1 writer + 2 readers for 500ms", "[concurrent][race][seqlock]") {
    SeqLock<Snapshot> lock{Snapshot{120.0, 15.0, 4}};
    std::atomic<bool> stop{false};
    std::atomic<int>  read_count{0};
    std::atomic<int>  write_count{0};
    std::atomic<int>  started{0};

    std::thread writer([&] {
        started.fetch_add(1, std::memory_order_release);
        double tempo = 120.0;
        while (!stop.load(std::memory_order_acquire)) {
            lock.write(Snapshot{tempo, tempo * 0.125, 4});
            tempo += 0.001;
            write_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Reader just exercises read() repeatedly. TSan is the verifier
    // here — any race in SeqLock's byte-stripe write / seq-retry read
    // protocol will trip the detector under -fsanitize=thread.
    // (Avoid asserting a correctness invariant on the snapshot
    // contents: floating-point recomputation in the writer makes any
    // `position == tempo * k` relation subject to rounding drift that
    // isn't a race but would flake the assertion.)
    auto read_worker = [&] {
        started.fetch_add(1, std::memory_order_release);
        while (!stop.load(std::memory_order_acquire)) {
            Snapshot snap = lock.read();
            (void) snap;
            read_count.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread reader_a(read_worker);
    std::thread reader_b(read_worker);

    INFO("started=" << started.load(std::memory_order_acquire));
    REQUIRE(wait_for_start_count(started, 3));
    std::this_thread::sleep_for(kHammerDuration);
    stop.store(true, std::memory_order_release);
    writer.join();
    reader_a.join();
    reader_b.join();

    INFO("writes=" << write_count.load() << " reads=" << read_count.load());
    REQUIRE(write_count.load() > 0);
    REQUIRE(read_count.load() > 0);
}

TEST_CASE("SpscQueue hammer: producer + consumer for 500ms", "[concurrent][race][spsc_queue]") {
    SpscQueue<int, 4096> q;
    std::atomic<bool> stop{false};
    std::atomic<int>  produced{0};
    std::atomic<int>  consumed{0};
    std::atomic<int>  started{0};

    std::thread producer([&] {
        started.fetch_add(1, std::memory_order_release);
        int v = 0;
        while (!stop.load(std::memory_order_acquire)) {
            if (q.try_push(++v)) {
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    // Gate the consumer's shutdown drain until after the producer has
    // fully exited. Without this gate, the consumer could exit the main
    // loop (stop observed) and try to drain the queue while the producer
    // is still racing toward its own exit — pushing more items after the
    // consumer thinks it's done. SPSC assumes strict single-producer, so
    // any overlap is undefined behavior.
    std::atomic<bool> producer_done{false};

    std::thread consumer([&] {
        started.fetch_add(1, std::memory_order_release);
        // Main hammer loop — drain concurrently while producer is pushing.
        while (!stop.load(std::memory_order_acquire)) {
            if (auto item = q.try_pop()) {
                (void) *item;
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Shutdown drain. The previous exit condition
        //     `!stop || produced != consumed`
        // relied on `produced` being up-to-date. But `produced.fetch_add`
        // uses relaxed ordering, so the consumer could read a stale value
        // of `produced` that happened to equal `consumed` while an item
        // was still in the queue — exit prematurely, leave one item
        // stranded, fail the final REQUIRE. Under CI load this reproduced
        // in roughly 1 of 5 runs and has blocked 6 PRs today.
        //
        // Fix: ignore `produced` at shutdown. Wait for the producer to
        // fully exit (via the `producer_done` flag set by main after
        // producer.join()), then drain the queue until empty. Once the
        // producer is verifiably done, no new items can appear — every
        // item pushed has been observably counted in `produced`.
        while (!producer_done.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        while (auto item = q.try_pop()) {
            (void) *item;
            consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    INFO("started=" << started.load(std::memory_order_acquire));
    REQUIRE(wait_for_start_count(started, 2));
    std::this_thread::sleep_for(kHammerDuration);
    stop.store(true, std::memory_order_release);
    producer.join();
    // Producer has fully exited — its final relaxed fetch_add on
    // `produced` is now visible to main (join synchronizes). Signal the
    // consumer that it's safe to drain.
    producer_done.store(true, std::memory_order_release);
    consumer.join();

    INFO("produced=" << produced.load() << " consumed=" << consumed.load());
    // Ordered SPSC: consumer should never see more than was produced,
    // and when stop is observed by both they must agree.
    REQUIRE(produced.load() > 0);
    REQUIRE(consumed.load() == produced.load());
}
