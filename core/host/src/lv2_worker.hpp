#pragma once

// The host half of the LV2 worker extension (LV2_WORKER__schedule).
//
// A plugin calls schedule_work() from run() to get non-realtime work done; the
// host calls the plugin's work() on another thread, and the plugin's respond()
// calls from inside work() are handed back to work_response() on the audio
// thread after a later run(). Messages are variable-sized, so both directions
// travel through a single-producer single-consumer byte ring that never
// allocates or locks.
//
// Offline rendering has no deadline but does need determinism: a response that
// lands one block later on a slow machine changes the render. So in
// synchronous mode work() runs inside schedule_work(), and its response is
// delivered after the same run(). A request already queued for the thread is
// never overtaken: work() is never called concurrently with itself.

#include <lv2/core/lv2.h>
#include <lv2/worker/worker.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <semaphore>
#include <thread>
#include <vector>

namespace pulp::host::detail {

/// Byte ring of length-prefixed messages. One producer thread, one consumer
/// thread. Capacity is rounded up to a power of two.
class Lv2MessageRing {
  public:
    explicit Lv2MessageRing(std::size_t capacity) {
        std::size_t size = 64;
        while (size < capacity)
            size <<= 1;
        buffer_.assign(size, 0);
        mask_ = size - 1;
    }

    std::size_t capacity() const noexcept {
        return buffer_.size();
    }

    /// Producer. False when the message does not fit; nothing is written.
    bool write(const void* data, std::uint32_t size) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        // Acquire pairs with the consumer's release of tail_: the bytes it has
        // finished reading may now be overwritten.
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t needed = sizeof(size) + size;
        if (buffer_.size() - (head - tail) < needed)
            return false;
        copy_in(head, &size, sizeof(size));
        copy_in(head + sizeof(size), data, size);
        // Release publishes the message bytes before the new head.
        head_.store(head + needed, std::memory_order_release);
        return true;
    }

    /// Consumer. Reads the next message into `out` (resized only when it is
    /// too small, which a caller avoids by reserving capacity()). False when
    /// the ring is empty.
    bool read(std::vector<std::uint8_t>& out, std::uint32_t& size) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (head == tail)
            return false;
        copy_out(tail, &size, sizeof(size));
        if (out.size() < size)
            return false; // Cannot happen when `out` holds capacity() bytes.
        copy_out(tail + sizeof(size), out.data(), size);
        tail_.store(tail + sizeof(size) + size, std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

  private:
    void copy_in(std::size_t at, const void* src, std::size_t n) noexcept {
        const auto* bytes = static_cast<const std::uint8_t*>(src);
        for (std::size_t i = 0; i < n; ++i)
            buffer_[(at + i) & mask_] = bytes[i];
    }
    void copy_out(std::size_t at, void* dst, std::size_t n) const noexcept {
        auto* bytes = static_cast<std::uint8_t*>(dst);
        for (std::size_t i = 0; i < n; ++i)
            bytes[i] = buffer_[(at + i) & mask_];
    }

    std::vector<std::uint8_t> buffer_;
    std::size_t mask_ = 0;
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
};

class Lv2Worker {
  public:
    static constexpr std::size_t kRingBytes = 1u << 16;

    Lv2Worker() : requests_(kRingBytes), responses_(kRingBytes) {
        schedule_.handle = this;
        schedule_.schedule_work = &Lv2Worker::schedule_trampoline;
        audio_scratch_.assign(requests_.capacity(), 0);
    }

    ~Lv2Worker() {
        stop();
    }

    Lv2Worker(const Lv2Worker&) = delete;
    Lv2Worker& operator=(const Lv2Worker&) = delete;

    /// The feature data a plugin is instantiated with. Valid for this object's
    /// lifetime; schedule_work() before start() is refused.
    LV2_Worker_Schedule* schedule_feature() noexcept {
        return &schedule_;
    }

    /// Control thread, after instantiate(). Starts the worker thread.
    void start(const LV2_Worker_Interface* iface, LV2_Handle handle) {
        stop();
        if (!iface || !iface->work)
            return;
        iface_ = iface;
        handle_ = handle;
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { thread_main(); });
    }

    /// Control thread, before cleanup(). Joins the worker thread; queued
    /// requests that were not yet worked are discarded.
    void stop() {
        if (thread_.joinable()) {
            running_.store(false, std::memory_order_release);
            wake_.release();
            thread_.join();
        }
        iface_ = nullptr;
        handle_ = nullptr;
        std::uint32_t size = 0;
        while (requests_.read(audio_scratch_, size)) {
        }
        while (responses_.read(audio_scratch_, size)) {
        }
        in_flight_.store(0, std::memory_order_relaxed);
    }

    bool active() const noexcept {
        return iface_ != nullptr;
    }

    /// Audio thread, before run(). Synchronous for an offline block.
    void set_synchronous(bool synchronous) noexcept {
        synchronous_ = synchronous;
    }

    /// Audio thread, after run(): hand every response to work_response(), then
    /// call end_run().
    void deliver_responses() noexcept {
        if (!iface_)
            return;
        std::uint32_t size = 0;
        while (responses_.read(audio_scratch_, size)) {
            if (iface_->work_response)
                iface_->work_response(handle_, size, audio_scratch_.data());
        }
        if (iface_->end_run)
            iface_->end_run(handle_);
    }

    /// Requests handed to the worker thread that it has not finished.
    std::uint32_t in_flight() const noexcept {
        return in_flight_.load(std::memory_order_acquire);
    }

  private:
    static LV2_Worker_Status schedule_trampoline(LV2_Worker_Schedule_Handle handle,
                                                 std::uint32_t size, const void* data) {
        return static_cast<Lv2Worker*>(handle)->schedule(size, data);
    }

    static LV2_Worker_Status respond_trampoline(LV2_Worker_Respond_Handle handle,
                                                std::uint32_t size, const void* data) {
        auto* self = static_cast<Lv2Worker*>(handle);
        return self->responses_.write(data, size) ? LV2_WORKER_SUCCESS : LV2_WORKER_ERR_NO_SPACE;
    }

    LV2_Worker_Status schedule(std::uint32_t size, const void* data) noexcept {
        if (!iface_)
            return LV2_WORKER_ERR_UNKNOWN;
        if (synchronous_ && in_flight_.load(std::memory_order_acquire) == 0)
            return iface_->work(handle_, &Lv2Worker::respond_trampoline, this, size, data);
        if (!requests_.write(data, size))
            return LV2_WORKER_ERR_NO_SPACE;
        // Release: the worker thread that sees the count also sees the request.
        in_flight_.fetch_add(1, std::memory_order_release);
        wake_.release();
        return LV2_WORKER_SUCCESS;
    }

    void thread_main() {
        std::vector<std::uint8_t> scratch(requests_.capacity(), 0);
        while (true) {
            wake_.acquire();
            if (!running_.load(std::memory_order_acquire))
                return;
            std::uint32_t size = 0;
            while (requests_.read(scratch, size)) {
                iface_->work(handle_, &Lv2Worker::respond_trampoline, this, size, scratch.data());
                in_flight_.fetch_sub(1, std::memory_order_release);
            }
        }
    }

    LV2_Worker_Schedule schedule_{};
    const LV2_Worker_Interface* iface_ = nullptr;
    LV2_Handle handle_ = nullptr;
    Lv2MessageRing requests_;
    Lv2MessageRing responses_;
    std::vector<std::uint8_t> audio_scratch_;
    std::counting_semaphore<> wake_{0};
    std::atomic<bool> running_{false};
    std::atomic<std::uint32_t> in_flight_{0};
    bool synchronous_ = false;
    std::thread thread_;
};

} // namespace pulp::host::detail
