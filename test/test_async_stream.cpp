#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/async_stream.hpp>
#include <pulp/runtime/stream.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace pulp::runtime;
using namespace std::chrono_literals;

namespace {

// Test-only Stream that lets the test drive read availability and observe
// writes. Synchronization is internal so the worker thread can safely block
// waiting for data.
class TestStream : public Stream {
public:
    StreamResult read(std::uint8_t* buf, std::size_t size) override {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!open_) return StreamResult::fail(StreamError::Closed);
        if (read_queue_.empty()) return StreamResult::fail(StreamError::WouldBlock);
        auto& chunk = read_queue_.front();
        auto n = std::min(size, chunk.size());
        std::memcpy(buf, chunk.data(), n);
        if (n == chunk.size()) {
            read_queue_.erase(read_queue_.begin());
        } else {
            chunk.erase(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
        }
        return StreamResult::make(n);
    }

    StreamResult write(const std::uint8_t* buf, std::size_t size) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return StreamResult::fail(StreamError::Closed);
        written_.insert(written_.end(), buf, buf + size);
        return StreamResult::make(size);
    }

    void close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = false;
    }

    bool is_open() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return open_;
    }

    void push_read(std::vector<std::uint8_t> data) {
        std::lock_guard<std::mutex> lock(mutex_);
        read_queue_.push_back(std::move(data));
    }

    std::vector<std::uint8_t> captured_writes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return written_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::vector<std::uint8_t>> read_queue_;
    std::vector<std::uint8_t> written_;
    bool open_ = true;
};

template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds budget = 2s) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

}  // namespace

TEST_CASE("AsyncStream delivers read data via callback", "[async_stream]") {
    auto backing = std::make_unique<TestStream>();
    auto* raw = backing.get();

    AsyncStream stream(std::move(backing));

    std::mutex m;
    std::vector<std::uint8_t> received;
    stream.on_data([&](const std::uint8_t* data, std::size_t n) {
        std::lock_guard<std::mutex> lock(m);
        received.insert(received.end(), data, data + n);
    });

    stream.start();
    raw->push_read({1, 2, 3, 4});
    raw->push_read({5, 6});

    REQUIRE(wait_until([&] {
        std::lock_guard<std::mutex> lock(m);
        return received.size() >= 6;
    }));

    std::lock_guard<std::mutex> lock(m);
    REQUIRE(received == std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6});
}

TEST_CASE("AsyncStream write flushes through to Stream", "[async_stream]") {
    auto backing = std::make_unique<TestStream>();
    auto* raw = backing.get();

    AsyncStream::Options opts;
    opts.auto_read = false;
    AsyncStream stream(std::move(backing), opts);

    std::atomic<std::size_t> written_bytes{0};
    std::atomic<bool> done{false};
    stream.start();
    REQUIRE(stream.write_async(reinterpret_cast<const std::uint8_t*>("hello"), 5,
                               [&](std::size_t n, StreamError err) {
                                   REQUIRE(err == StreamError::Ok);
                                   written_bytes.store(n);
                                   done.store(true);
                               }));

    REQUIRE(wait_until([&] { return done.load(); }));
    REQUIRE(written_bytes.load() == 5);
    auto captured = raw->captured_writes();
    REQUIRE(captured.size() == 5);
    REQUIRE(std::memcmp(captured.data(), "hello", 5) == 0);
}

TEST_CASE("AsyncStream honors backpressure high-water", "[async_stream]") {
    auto backing = std::make_unique<TestStream>();
    AsyncStream::Options opts;
    opts.auto_read = false;
    opts.write_high_water = 8;
    AsyncStream stream(std::move(backing), opts);
    // Do not start the worker — writes remain queued so we can observe
    // the high-water behavior directly.

    std::uint8_t payload[8] = {};
    REQUIRE(stream.write_async(payload, 8));
    REQUIRE_FALSE(stream.write_async(payload, 1));  // would exceed high-water
    REQUIRE(stream.pending_write_bytes() == 8);
}

TEST_CASE("AsyncStream cancel drains pending writes with Closed", "[async_stream]") {
    auto backing = std::make_unique<TestStream>();
    AsyncStream::Options opts;
    opts.auto_read = false;
    AsyncStream stream(std::move(backing), opts);

    std::atomic<int> completions{0};
    std::atomic<StreamError> last_err{StreamError::Ok};

    std::uint8_t payload[4] = {};
    stream.start();
    stream.cancel();  // cancel before writing

    REQUIRE(stream.write_async(payload, 4, [&](std::size_t, StreamError err) {
        last_err.store(err);
        completions.fetch_add(1);
    }));

    REQUIRE(wait_until([&] { return completions.load() == 1; }));
    REQUIRE(last_err.load() == StreamError::Closed);
}

TEST_CASE("AsyncStream cancel drains queue even before worker starts", "[async_stream]") {
    auto backing = std::make_unique<TestStream>();
    AsyncStream::Options opts;
    opts.auto_read = false;
    AsyncStream stream(std::move(backing), opts);

    std::atomic<int> completions{0};
    std::atomic<StreamError> last_err{StreamError::Ok};

    // Queue writes *without* starting the worker, then cancel. The previous
    // implementation dropped these callbacks; now they must all complete
    // with Closed.
    std::uint8_t payload[4] = {};
    REQUIRE(stream.write_async(payload, 4, [&](std::size_t, StreamError err) {
        last_err.store(err);
        completions.fetch_add(1);
    }));
    REQUIRE(stream.write_async(payload, 4, [&](std::size_t, StreamError err) {
        last_err.store(err);
        completions.fetch_add(1);
    }));

    stream.cancel();

    REQUIRE(wait_until([&] { return completions.load() == 2; }));
    REQUIRE(last_err.load() == StreamError::Closed);
    REQUIRE(stream.pending_write_bytes() == 0);
}

TEST_CASE("AsyncStream zero-byte write dispatches completion without worker", "[async_stream]") {
    auto backing = std::make_unique<TestStream>();

    std::mutex m;
    std::vector<std::function<void()>> queued;
    AsyncStream::Options opts;
    opts.auto_read = false;
    opts.executor = [&](std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(m);
        queued.push_back(std::move(fn));
    };
    AsyncStream stream(std::move(backing), opts);

    std::atomic<bool> done{false};
    std::atomic<std::size_t> bytes{99};
    std::atomic<StreamError> error{StreamError::Closed};
    REQUIRE(stream.write_async(nullptr, 0, [&](std::size_t n, StreamError err) {
        bytes.store(n);
        error.store(err);
        done.store(true);
    }));
    REQUIRE(stream.pending_write_bytes() == 0);
    REQUIRE_FALSE(done.load());

    std::vector<std::function<void()>> to_run;
    {
        std::lock_guard<std::mutex> lock(m);
        REQUIRE(queued.size() == 1);
        to_run.swap(queued);
    }
    to_run.front()();

    REQUIRE(done.load());
    REQUIRE(bytes.load() == 0);
    REQUIRE(error.load() == StreamError::Ok);
}

TEST_CASE("CancellationToken sharing and idempotent cancel", "[async_stream]") {
    CancellationToken token;
    CancellationToken copy = token;
    CancellationToken other;

    REQUIRE(token.shares(copy));
    REQUIRE_FALSE(token.shares(other));
    REQUIRE_FALSE(copy.is_cancelled());

    token.cancel();
    token.cancel();
    REQUIRE(token.is_cancelled());
    REQUIRE(copy.is_cancelled());
    REQUIRE_FALSE(other.is_cancelled());
}

TEST_CASE("AsyncStream executor routes callbacks off worker", "[async_stream]") {
    auto backing = std::make_unique<TestStream>();
    auto* raw = backing.get();

    std::mutex m;
    std::vector<std::function<void()>> queued;
    AsyncStream::Options opts;
    opts.executor = [&](std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(m);
        queued.push_back(std::move(fn));
    };
    AsyncStream stream(std::move(backing), opts);

    std::atomic<std::size_t> received{0};
    stream.on_data([&](const std::uint8_t*, std::size_t n) { received.fetch_add(n); });

    stream.start();
    raw->push_read({9, 9, 9});

    REQUIRE(wait_until([&] {
        std::lock_guard<std::mutex> lock(m);
        return !queued.empty();
    }));

    // Data callback was enqueued, not invoked directly.
    REQUIRE(received.load() == 0);

    // Drain the executor queue manually, as a real event loop would.
    std::vector<std::function<void()>> to_run;
    {
        std::lock_guard<std::mutex> lock(m);
        to_run.swap(queued);
    }
    for (auto& fn : to_run) fn();

    REQUIRE(received.load() == 3);
}
