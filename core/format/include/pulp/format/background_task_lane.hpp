#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>

#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/runtime/triple_buffer.hpp>

namespace pulp::format {

enum class BackgroundTaskPolicy { Ordered, Latest };

/// One bounded, pre-warmed audio-to-worker task lane.
///
/// start()/stop() are control-thread operations. try_spawn() is lock-free and
/// allocation-free after start. A lane has one consumer, so its handler is
/// never re-entered; independent lanes run concurrently. Latest policy
/// coalesces bursts to the newest trivially-copyable task.
template <typename Task, std::size_t Capacity = 64>
class BackgroundTaskLane {
    static_assert(std::is_trivially_copyable_v<Task>,
                  "RT task payloads must be trivially copyable");
public:
    using Handler = void (*)(void*, const Task&) noexcept;

    BackgroundTaskLane() = default;
    ~BackgroundTaskLane() { stop(); }
    BackgroundTaskLane(const BackgroundTaskLane&) = delete;
    BackgroundTaskLane& operator=(const BackgroundTaskLane&) = delete;

    bool start(Handler handler, void* context = nullptr,
               BackgroundTaskPolicy policy = BackgroundTaskPolicy::Ordered) {
        if (!handler || running_.load(std::memory_order_acquire)) return false;
        handler_ = handler;
        context_ = context;
        policy_ = policy;
        stopping_.store(false, std::memory_order_release);
        try {
            worker_ = std::thread([this] { worker_loop(); });
        } catch (...) {
            handler_ = nullptr;
            return false;
        }
        running_.store(true, std::memory_order_release);
        return true;
    }

    void stop() noexcept {
        stopping_.store(true, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
        running_.store(false, std::memory_order_release);
    }

    bool try_spawn(const Task& task) noexcept {
        if (!running_.load(std::memory_order_acquire)) return false;
        if (policy_ == BackgroundTaskPolicy::Latest) {
            latest_.write(task);
            latest_generation_.fetch_add(1, std::memory_order_release);
            return true;
        }
        return ordered_.try_push(task);
    }

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    std::uint64_t overflow_count() const noexcept { return ordered_.overflow_count(); }

private:
    void worker_loop() noexcept {
        std::uint64_t consumed_generation = 0;
        while (!stopping_.load(std::memory_order_acquire)) {
            bool handled = false;
            if (policy_ == BackgroundTaskPolicy::Latest) {
                const auto generation = latest_generation_.load(std::memory_order_acquire);
                if (generation != consumed_generation) {
                    handler_(context_, latest_.read());
                    consumed_generation = generation;
                    handled = true;
                }
            } else if (auto task = ordered_.try_pop()) {
                handler_(context_, *task);
                handled = true;
            }
            if (!handled) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (policy_ == BackgroundTaskPolicy::Ordered) {
            while (auto task = ordered_.try_pop()) handler_(context_, *task);
        }
    }

    runtime::SpscQueue<Task, Capacity> ordered_;
    runtime::TripleBuffer<Task> latest_;
    std::atomic<std::uint64_t> latest_generation_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    Handler handler_ = nullptr;
    void* context_ = nullptr;
    BackgroundTaskPolicy policy_ = BackgroundTaskPolicy::Ordered;
    std::thread worker_;
};

} // namespace pulp::format
