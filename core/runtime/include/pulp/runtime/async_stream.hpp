#pragma once

// AsyncStream — non-blocking wrapper around a synchronous Stream.
//
// AsyncStream owns a worker thread that pumps I/O on a Stream while the
// caller continues on its own thread. Read data and completion callbacks
// are delivered either on the worker thread (default) or dispatched into a
// Pulp EventLoop when one is supplied.
//
// The wrapper adds three things the raw Stream does not:
//   - asynchronous callbacks for read data, errors, and close;
//   - a bounded write queue with explicit backpressure (`write` returns
//     false when the pending queue exceeds a high-water mark);
//   - cancellation — destruction, `cancel()`, or `close()` stops all
//     in-flight work and fires pending completion callbacks with a
//     Cancelled error.
//
// This is layer 2 of the Stream hierarchy. Layer 1 (`Stream`) provides a
// synchronous interface; layer 3 adds network/HTTP-specific streams that
// are already `Stream` subclasses and therefore automatically work with
// `AsyncStream` without further adapter code.

#include <pulp/runtime/stream.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pulp::runtime {

/// Executor hook: a function that schedules work on some thread. Callers
/// pass a wrapper around their own event loop's `dispatch` — for example
/// `[loop](auto fn) { loop->dispatch(std::move(fn)); }`. When null, the
/// AsyncStream runs callbacks on its worker thread.
using AsyncExecutor = std::function<void(std::function<void()>)>;

/// Simple cancellation token, shared across async operations. Instances can
/// be cheaply copied (they share state) and safely used from any thread.
class CancellationToken {
public:
    CancellationToken();

    /// Signal cancellation. Idempotent.
    void cancel();

    /// True once `cancel()` has been called.
    bool is_cancelled() const;

    /// True if this token is the same underlying state as `other`.
    bool shares(const CancellationToken& other) const {
        return state_.get() == other.state_.get();
    }

private:
    struct State { std::atomic<bool> cancelled{false}; };
    std::shared_ptr<State> state_;
};

/// Completion callback for `write_async`. Receives the number of bytes
/// written and an error code; `error == Ok` means the full payload
/// succeeded (partial writes retry internally until the Stream reports a
/// non-Ok error).
using AsyncWriteCallback = std::function<void(std::size_t bytes, StreamError error)>;

/// Incoming-data callback. Called whenever the worker reads bytes from the
/// underlying Stream. `data` is valid only for the duration of the call.
using AsyncDataCallback = std::function<void(const std::uint8_t* data, std::size_t size)>;

/// Error callback. Fired once per async operation when an error terminates
/// the stream or a write. `Cancelled` is reported via the dedicated close
/// callback instead.
using AsyncErrorCallback = std::function<void(StreamError error)>;

/// Close callback. Fired once when the stream is no longer usable — either
/// because the underlying Stream closed, because `cancel()` was invoked, or
/// because the AsyncStream is being destroyed.
using AsyncCloseCallback = std::function<void()>;

struct AsyncStreamOptions {
    /// Read chunk size (bytes) used by the background worker.
    std::size_t read_chunk = 4096;

    /// High-water mark for pending write bytes. `write_async` returns
    /// false when adding the new payload would push the total beyond
    /// this limit — callers should wait for the `on_drain` callback
    /// before retrying.
    std::size_t write_high_water = 1 << 20;  // 1 MiB

    /// When true, the worker reads from the stream eagerly and fires
    /// `on_data` callbacks. Set to false for write-only streams.
    bool auto_read = true;

    /// Optional executor used to dispatch completion/data/error
    /// callbacks onto the caller's thread. When empty, callbacks run
    /// on the AsyncStream's worker thread. Typically wired to a
    /// pulp::events::EventLoop via `[loop](auto fn){loop->dispatch(std::move(fn));}`.
    AsyncExecutor executor;
};

class AsyncStream {
public:
    using Options = AsyncStreamOptions;

    explicit AsyncStream(std::unique_ptr<Stream> stream, Options options = {});
    ~AsyncStream();

    AsyncStream(const AsyncStream&) = delete;
    AsyncStream& operator=(const AsyncStream&) = delete;

    /// Attach callbacks. Must be called before `start()` so the worker has
    /// somewhere to send events. Safe to overwrite between `stop()` and
    /// `start()`; not safe to reassign while the worker is running.
    void on_data(AsyncDataCallback cb);
    void on_error(AsyncErrorCallback cb);
    void on_close(AsyncCloseCallback cb);
    void on_drain(AsyncCloseCallback cb);  ///< fires when pending writes hit 0

    /// Start the worker thread. No-op if already running.
    void start();

    /// Stop the worker thread and wait for it to exit. Fires `on_close`
    /// exactly once if it has not fired already.
    void stop();

    /// Request cancellation. Pending writes complete with
    /// `StreamError::Closed`; the worker exits at its next scheduled point.
    void cancel();

    /// Queue an async write. Returns false if the write would exceed the
    /// configured high-water mark (backpressure). The caller should wait
    /// for `on_drain` before trying again.
    [[nodiscard]] bool write_async(const std::uint8_t* data, std::size_t size,
                                   AsyncWriteCallback callback = {});

    /// Bytes currently queued for writing.
    std::size_t pending_write_bytes() const;

    /// Cancellation token shared with this stream's worker. Callers can
    /// pass this to user code that wants to observe cancellation alongside
    /// the stream lifetime.
    CancellationToken cancellation_token() const { return token_; }

    /// Underlying Stream, for advanced callers. Not thread-safe with the
    /// worker; only touch from the worker thread or after `stop()`.
    Stream* stream() { return stream_.get(); }

private:
    struct PendingWrite {
        std::vector<std::uint8_t> data;
        std::size_t written = 0;
        AsyncWriteCallback callback;
    };

    void write_loop();
    void read_loop();
    void dispatch(std::function<void()> task);
    void fire_close();
    void drain_queue_as_closed();  ///< completes queued writes with StreamError::Closed

    std::unique_ptr<Stream> stream_;
    Options options_;
    CancellationToken token_;

    AsyncDataCallback on_data_;
    AsyncErrorCallback on_error_;
    AsyncCloseCallback on_close_;
    AsyncCloseCallback on_drain_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<PendingWrite> write_queue_;
    std::size_t pending_bytes_ = 0;
    bool running_ = false;
    bool close_fired_ = false;
    std::thread writer_thread_;
    std::thread reader_thread_;
};

}  // namespace pulp::runtime
