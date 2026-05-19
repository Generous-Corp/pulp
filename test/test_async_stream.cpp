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
        if (read_io_error_) return StreamResult::fail(StreamError::IoError);
        if (read_zero_ok_count_ > 0) {
            --read_zero_ok_count_;
            return StreamResult::make(0);
        }
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
        if (write_would_block_count_ > 0) {
            --write_would_block_count_;
            return StreamResult::fail(StreamError::WouldBlock);
        }
        if (write_io_error_) return StreamResult::fail(StreamError::IoError);
        if (write_io_error_after_bytes_ >= 0 &&
            static_cast<int>(written_.size()) >= write_io_error_after_bytes_) {
            return StreamResult::fail(StreamError::IoError);
        }

        auto n = size;
        if (write_chunk_limit_ > 0) n = std::min(n, write_chunk_limit_);
        if (write_io_error_after_bytes_ >= 0) {
            auto remaining = static_cast<std::size_t>(
                write_io_error_after_bytes_ - static_cast<int>(written_.size()));
            n = std::min(n, remaining);
        }
        if (n == 0) return StreamResult::fail(StreamError::IoError);

        written_.insert(written_.end(), buf, buf + n);
        return StreamResult::make(n);
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

    void set_read_zero_ok_count(int count) {
        std::lock_guard<std::mutex> lock(mutex_);
        read_zero_ok_count_ = count;
    }

    void set_read_io_error(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        read_io_error_ = enabled;
    }

    void set_write_chunk_limit(std::size_t limit) {
        std::lock_guard<std::mutex> lock(mutex_);
        write_chunk_limit_ = limit;
    }

    void set_write_would_block_count(int count) {
        std::lock_guard<std::mutex> lock(mutex_);
        write_would_block_count_ = count;
    }

    void set_write_io_error(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        write_io_error_ = enabled;
    }

    void set_write_io_error_after_bytes(int bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        write_io_error_after_bytes_ = bytes;
    }

    std::vector<std::uint8_t> captured_writes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return written_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::vector<std::uint8_t>> read_queue_;
    std::vector<std::uint8_t> written_;
    std::size_t write_chunk_limit_ = 0;
    int write_would_block_count_ = 0;
    int write_io_error_after_bytes_ = -1;
    int read_zero_ok_count_ = 0;
    bool read_io_error_ = false;
    bool write_io_error_ = false;
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

TEST_CASE("AsyncStream rejects null non-empty writes via callback",
          "[async_stream][coverage][phase3]") {
    auto backing = std::make_unique<TestStream>();

    AsyncStream::Options opts;
    opts.auto_read = false;
    AsyncStream stream(std::move(backing), opts);

    std::atomic<int> callbacks{0};
    std::atomic<std::size_t> bytes{99};
    std::atomic<StreamError> error{StreamError::Ok};

    REQUIRE(stream.write_async(nullptr, 3, [&](std::size_t n, StreamError err) {
        bytes.store(n);
        error.store(err);
        callbacks.fetch_add(1);
    }));

    REQUIRE(callbacks.load() == 1);
    REQUIRE(bytes.load() == 0);
    REQUIRE(error.load() == StreamError::Invalid);
    REQUIRE(stream.pending_write_bytes() == 0);
}

TEST_CASE("AsyncStream start after stop refreshes cancellation state",
          "[async_stream][coverage][phase3]") {
    auto backing = std::make_unique<TestStream>();
    auto* raw = backing.get();

    AsyncStream::Options opts;
    opts.auto_read = false;
    AsyncStream stream(std::move(backing), opts);

    stream.start();
    stream.stop();

    std::atomic<int> callbacks{0};
    std::atomic<std::size_t> bytes{0};
    std::atomic<StreamError> error{StreamError::Closed};
    const std::uint8_t payload[] = {'r', 'e', 's', 't', 'a', 'r', 't'};

    stream.start();
    REQUIRE(stream.write_async(payload, sizeof(payload),
                               [&](std::size_t n, StreamError err) {
                                   bytes.store(n);
                                   error.store(err);
                                   callbacks.fetch_add(1);
                               }));

    REQUIRE(wait_until([&] { return callbacks.load() == 1; }));
    REQUIRE(bytes.load() == sizeof(payload));
    REQUIRE(error.load() == StreamError::Ok);
    REQUIRE(raw->captured_writes() ==
            std::vector<std::uint8_t>{'r', 'e', 's', 't', 'a', 'r', 't'});
}

TEST_CASE("AsyncStream write on a closed backing stream completes without queueing",
          "[async_stream][coverage][phase3]") {
    auto backing = std::make_unique<TestStream>();
    auto* raw = backing.get();
    raw->close();

    AsyncStream::Options opts;
    opts.auto_read = false;
    AsyncStream stream(std::move(backing), opts);

    std::atomic<int> completions{0};
    std::atomic<std::size_t> bytes{99};
    std::atomic<StreamError> error{StreamError::Ok};
    const std::uint8_t payload[] = {'x', 'y'};

    REQUIRE(stream.write_async(payload, sizeof(payload),
                               [&](std::size_t n, StreamError err) {
                                   bytes.store(n);
                                   error.store(err);
                                   completions.fetch_add(1);
                               }));

    REQUIRE(completions.load() == 1);
    REQUIRE(bytes.load() == 0);
    REQUIRE(error.load() == StreamError::Closed);
    REQUIRE(stream.pending_write_bytes() == 0);
}

TEST_CASE("AsyncStream write without a backing stream completes as closed",
          "[async_stream][coverage][phase3]") {
    AsyncStream::Options opts;
    opts.auto_read = false;
    AsyncStream stream(nullptr, opts);

    std::atomic<int> completions{0};
    std::atomic<std::size_t> bytes{99};
    std::atomic<StreamError> error{StreamError::Ok};
    const std::uint8_t payload[] = {'n', 'o'};

    REQUIRE(stream.stream() == nullptr);
    REQUIRE(stream.write_async(payload, sizeof(payload),
                               [&](std::size_t n, StreamError err) {
                                   bytes.store(n);
                                   error.store(err);
                                   completions.fetch_add(1);
                               }));

    REQUIRE(completions.load() == 1);
    REQUIRE(bytes.load() == 0);
    REQUIRE(error.load() == StreamError::Closed);
    REQUIRE(stream.pending_write_bytes() == 0);
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

TEST_CASE("AsyncStream repeated start and stop keep close callback single-shot",
          "[async_stream][coverage][phase3]") {
    auto backing = std::make_unique<TestStream>();

    AsyncStream::Options opts;
    opts.auto_read = false;
    AsyncStream stream(std::move(backing), opts);

    std::atomic<int> closes{0};
    stream.on_close([&] { closes.fetch_add(1); });

    stream.stop();
    REQUIRE(closes.load() == 0);

    stream.start();
    stream.start();
    stream.stop();
    stream.stop();

    REQUIRE(closes.load() == 1);
}

TEST_CASE("AsyncStream retries WouldBlock and partial writes before draining",
          "[async_stream][coverage][phase3]") {
    auto backing = std::make_unique<TestStream>();
    auto* raw = backing.get();
    raw->set_write_chunk_limit(2);
    raw->set_write_would_block_count(1);

    AsyncStream::Options opts;
    opts.auto_read = false;
    AsyncStream stream(std::move(backing), opts);

    std::atomic<int> completions{0};
    std::atomic<int> drains{0};
    std::atomic<std::size_t> total_bytes{0};
    std::atomic<StreamError> last_error{StreamError::Closed};
    const std::uint8_t second[] = {'e', 'f'};

    stream.on_drain([&] {
        auto previous = drains.fetch_add(1);
        if (previous == 0) {
            REQUIRE(stream.write_async(second, sizeof(second),
                                       [&](std::size_t n, StreamError err) {
                                           total_bytes.fetch_add(n);
                                           last_error.store(err);
                                           completions.fetch_add(1);
                                       }));
        }
    });

    stream.start();
    const std::uint8_t first[] = {'a', 'b', 'c', 'd'};
    REQUIRE(stream.write_async(first, sizeof(first),
                               [&](std::size_t n, StreamError err) {
                                   total_bytes.fetch_add(n);
                                   last_error.store(err);
                                   completions.fetch_add(1);
                               }));

    REQUIRE(wait_until([&] { return completions.load() == 2 && drains.load() == 2; }));
    REQUIRE(total_bytes.load() == 6);
    REQUIRE(last_error.load() == StreamError::Ok);
    REQUIRE(stream.pending_write_bytes() == 0);

    auto captured = raw->captured_writes();
    REQUIRE(captured == std::vector<std::uint8_t>{'a', 'b', 'c', 'd', 'e', 'f'});
}

TEST_CASE("AsyncStream reports partial write errors and stops writer",
          "[async_stream][coverage][phase3]") {
    auto backing = std::make_unique<TestStream>();
    auto* raw = backing.get();
    raw->set_write_chunk_limit(2);
    raw->set_write_io_error_after_bytes(2);

    AsyncStream::Options opts;
    opts.auto_read = false;
    AsyncStream stream(std::move(backing), opts);

    std::atomic<bool> write_done{false};
    std::atomic<bool> error_seen{false};
    std::atomic<std::size_t> callback_bytes{99};
    std::atomic<StreamError> callback_error{StreamError::Ok};
    std::atomic<StreamError> stream_error{StreamError::Ok};

    stream.on_error([&](StreamError err) {
        stream_error.store(err);
        error_seen.store(true);
    });

    stream.start();
    const std::uint8_t payload[] = {'f', 'a', 'i', 'l'};
    REQUIRE(stream.write_async(payload, sizeof(payload),
                               [&](std::size_t n, StreamError err) {
                                   callback_bytes.store(n);
                                   callback_error.store(err);
                                   write_done.store(true);
                               }));

    REQUIRE(wait_until([&] { return write_done.load() && error_seen.load(); }));
    REQUIRE(callback_bytes.load() == 2);
    REQUIRE(callback_error.load() == StreamError::IoError);
    REQUIRE(stream_error.load() == StreamError::IoError);
    REQUIRE(stream.pending_write_bytes() == 0);

    auto captured = raw->captured_writes();
    REQUIRE(captured == std::vector<std::uint8_t>{'f', 'a'});
}

TEST_CASE("AsyncStream read loop backs off on zero-byte reads before data",
          "[async_stream][coverage][phase3]") {
    auto backing = std::make_unique<TestStream>();
    auto* raw = backing.get();
    raw->set_read_zero_ok_count(3);

    AsyncStream::Options opts;
    opts.read_chunk = 2;
    AsyncStream stream(std::move(backing), opts);

    std::mutex m;
    std::vector<std::uint8_t> received;
    stream.on_data([&](const std::uint8_t* data, std::size_t n) {
        std::lock_guard<std::mutex> lock(m);
        received.insert(received.end(), data, data + n);
    });

    raw->push_read({1, 2, 3});
    stream.start();

    REQUIRE(wait_until([&] {
        std::lock_guard<std::mutex> lock(m);
        return received.size() == 3;
    }));

    std::lock_guard<std::mutex> lock(m);
    REQUIRE(received == std::vector<std::uint8_t>{1, 2, 3});
}

TEST_CASE("AsyncStream read errors fire on_error and close fires once on stop",
          "[async_stream][coverage][phase3]") {
    auto backing = std::make_unique<TestStream>();
    auto* raw = backing.get();
    raw->set_read_io_error(true);

    AsyncStream stream(std::move(backing));

    std::atomic<int> errors{0};
    std::atomic<int> closes{0};
    std::atomic<StreamError> last_error{StreamError::Ok};
    stream.on_error([&](StreamError err) {
        last_error.store(err);
        errors.fetch_add(1);
    });
    stream.on_close([&] { closes.fetch_add(1); });

    stream.start();
    REQUIRE(wait_until([&] { return errors.load() == 1; }));
    REQUIRE(last_error.load() == StreamError::IoError);

    stream.stop();
    stream.stop();
    REQUIRE(closes.load() == 1);
}
