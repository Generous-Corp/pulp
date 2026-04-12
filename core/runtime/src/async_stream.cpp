#include <pulp/runtime/async_stream.hpp>

#include <chrono>
#include <utility>

namespace pulp::runtime {

// ─── CancellationToken ────────────────────────────────────────────────────

CancellationToken::CancellationToken() : state_(std::make_shared<State>()) {}

void CancellationToken::cancel() {
    if (state_) state_->cancelled.store(true, std::memory_order_release);
}

bool CancellationToken::is_cancelled() const {
    return state_ && state_->cancelled.load(std::memory_order_acquire);
}

// ─── AsyncStream ──────────────────────────────────────────────────────────

AsyncStream::AsyncStream(std::unique_ptr<Stream> stream, Options options)
    : stream_(std::move(stream)),
      options_(options) {}

AsyncStream::~AsyncStream() {
    stop();
}

void AsyncStream::on_data(AsyncDataCallback cb) { on_data_ = std::move(cb); }
void AsyncStream::on_error(AsyncErrorCallback cb) { on_error_ = std::move(cb); }
void AsyncStream::on_close(AsyncCloseCallback cb) { on_close_ = std::move(cb); }
void AsyncStream::on_drain(AsyncCloseCallback cb) { on_drain_ = std::move(cb); }

void AsyncStream::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;
    running_ = true;
    close_fired_ = false;
    worker_ = std::thread([this] { worker_main(); });
}

void AsyncStream::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !worker_.joinable()) return;
        running_ = false;
    }
    cv_.notify_all();
    token_.cancel();
    if (worker_.joinable()) worker_.join();
    fire_close();
}

void AsyncStream::cancel() {
    token_.cancel();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    cv_.notify_all();
}

bool AsyncStream::write_async(const std::uint8_t* data, std::size_t size,
                              AsyncWriteCallback callback) {
    if (size == 0) {
        if (callback) dispatch([cb = std::move(callback)] { cb(0, StreamError::Ok); });
        return true;
    }

    PendingWrite pw;
    pw.data.assign(data, data + size);
    pw.callback = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (token_.is_cancelled() || !stream_ || !stream_->is_open()) {
            auto cb = std::move(pw.callback);
            if (cb) dispatch([cb = std::move(cb)] { cb(0, StreamError::Closed); });
            return true;
        }
        if (pending_bytes_ + size > options_.write_high_water) {
            return false;  // backpressure
        }
        pending_bytes_ += size;
        write_queue_.push_back(std::move(pw));
    }
    cv_.notify_all();
    return true;
}

std::size_t AsyncStream::pending_write_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_bytes_;
}

void AsyncStream::dispatch(std::function<void()> task) {
    if (options_.executor) {
        options_.executor(std::move(task));
    } else {
        task();
    }
}

void AsyncStream::fire_close() {
    AsyncCloseCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (close_fired_) return;
        close_fired_ = true;
        cb = on_close_;
    }
    if (cb) dispatch(std::move(cb));
}

void AsyncStream::fire_drain_if_idle_locked() {
    if (pending_bytes_ == 0 && on_drain_) {
        auto cb = on_drain_;
        dispatch(std::move(cb));
    }
}

void AsyncStream::worker_main() {
    std::vector<std::uint8_t> read_buf(options_.read_chunk);

    while (true) {
        // Check cancellation / running state.
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!running_ || token_.is_cancelled()) break;
        }

        bool did_work = false;

        // Drain one pending write, if any.
        PendingWrite to_write;
        bool have_write = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!write_queue_.empty()) {
                to_write = std::move(write_queue_.front());
                write_queue_.erase(write_queue_.begin());
                have_write = true;
            }
        }

        if (have_write) {
            StreamError last_err = StreamError::Ok;
            while (to_write.written < to_write.data.size()) {
                if (token_.is_cancelled()) { last_err = StreamError::Closed; break; }
                auto remaining = to_write.data.size() - to_write.written;
                auto result = stream_->write(
                    to_write.data.data() + to_write.written, remaining);
                if (!result.ok()) {
                    last_err = result.error;
                    if (last_err == StreamError::WouldBlock) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    break;
                }
                if (result.bytes == 0) { last_err = StreamError::Closed; break; }
                to_write.written += result.bytes;
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                pending_bytes_ -= to_write.data.size();
                fire_drain_if_idle_locked();
            }

            if (to_write.callback) {
                auto cb = std::move(to_write.callback);
                auto wrote = to_write.written;
                dispatch([cb = std::move(cb), wrote, last_err] { cb(wrote, last_err); });
            }
            if (last_err != StreamError::Ok && last_err != StreamError::WouldBlock) {
                if (on_error_) {
                    auto cb = on_error_;
                    dispatch([cb = std::move(cb), last_err] { cb(last_err); });
                }
                break;
            }
            did_work = true;
        }

        // Try a read if auto_read is on.
        if (options_.auto_read && stream_ && stream_->is_open()) {
            auto r = stream_->read(read_buf.data(), read_buf.size());
            if (r.ok() && r.bytes > 0) {
                if (on_data_) {
                    auto cb = on_data_;
                    std::vector<std::uint8_t> copy(read_buf.begin(),
                                                   read_buf.begin() + r.bytes);
                    dispatch([cb = std::move(cb), payload = std::move(copy)] {
                        cb(payload.data(), payload.size());
                    });
                }
                did_work = true;
            } else if (r.closed()) {
                break;  // stream closed cleanly; exit worker → fire_close
            } else if (r.error == StreamError::IoError) {
                if (on_error_) {
                    auto cb = on_error_;
                    dispatch([cb = std::move(cb)] { cb(StreamError::IoError); });
                }
                break;
            }
            // WouldBlock / Ok-with-0-bytes falls through to the wait below.
        }

        if (!did_work) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(10),
                         [this] { return !running_ || !write_queue_.empty(); });
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    fire_close();
}

}  // namespace pulp::runtime
