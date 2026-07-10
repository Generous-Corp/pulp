#pragma once

#include <pulp/runtime/async_stream.hpp>
#include <pulp/runtime/spsc_queue.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace pulp::runtime {

enum class BackgroundJobPriority {
    low = 0,
    normal = 1,
    high = 2,
};

struct BackgroundJobProgress {
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
    std::string label;
};

class BackgroundJobContext {
public:
    BackgroundJobContext(CancellationToken cancellation,
                         std::function<void(BackgroundJobProgress)> progress);

    bool is_cancelled() const;
    const CancellationToken& cancellation_token() const { return cancellation_; }
    void publish_progress(BackgroundJobProgress progress) const;

private:
    CancellationToken cancellation_;
    std::function<void(BackgroundJobProgress)> progress_;
};

using BackgroundJobFunction = std::function<void(BackgroundJobContext&)>;

class BackgroundJobHandle {
public:
    BackgroundJobHandle() = default;

    void cancel() const;
    bool valid() const;
    bool is_cancelled() const;
    bool is_finished() const;
    void wait() const;
    std::optional<BackgroundJobProgress> latest_progress() const;

private:
    struct State {
        explicit State(CancellationToken token_in) : token(std::move(token_in)) {}

        mutable std::mutex mutex;
        std::condition_variable cv;
        CancellationToken token;
        std::optional<BackgroundJobProgress> progress;
        bool started = false;
        bool finished = false;
    };

    explicit BackgroundJobHandle(std::shared_ptr<State> state);

    std::shared_ptr<State> state_;

    friend class BackgroundJobService;
};

struct BackgroundJobOptions {
    std::string name;
    BackgroundJobPriority priority = BackgroundJobPriority::normal;
};

class BackgroundJobService {
public:
    BackgroundJobService();
    ~BackgroundJobService();

    BackgroundJobService(const BackgroundJobService&) = delete;
    BackgroundJobService& operator=(const BackgroundJobService&) = delete;

    [[nodiscard]] BackgroundJobHandle submit(BackgroundJobOptions options,
                                             BackgroundJobFunction job);
    void cancel_all();
    void wait_all();
    std::size_t pending_count() const;

private:
    struct QueuedJob {
        BackgroundJobOptions options;
        BackgroundJobFunction job;
        std::shared_ptr<BackgroundJobHandle::State> state;
        std::uint64_t sequence = 0;
    };

    void worker_loop();
    void prune_finished_jobs_locked();
    static bool has_higher_priority(const QueuedJob& a, const QueuedJob& b);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<QueuedJob> queue_;
    std::vector<std::shared_ptr<BackgroundJobHandle::State>> all_jobs_;
    bool stopping_ = false;
    std::thread worker_;
};

enum class RealtimeResourceSlotPublishStatus : std::uint8_t {
    Published,
    RejectedEmptyResource,
    RejectedRetireQueueFull,
};

constexpr bool realtime_resource_slot_publish_succeeded(
    RealtimeResourceSlotPublishStatus status) noexcept {
    return status == RealtimeResourceSlotPublishStatus::Published;
}

/// Control-thread owner for resources prepared off the audio thread and read
/// from the audio thread through an atomic raw pointer.
///
/// A single non-RT owner serializes `publish*()` and `reclaim_retired()`. The
/// audio thread only calls `get()`, which is a single acquire load and does not
/// allocate, lock, wait, or take ownership. Because `get()` returns an unpinned
/// raw pointer, the owner must call `reclaim_retired()` only after a host/block
/// boundary or other grace point proves callbacks that may have loaded an old
/// pointer have left it.
template <typename T, std::size_t RetireCapacity>
class RealtimeResourceSlot {
    static_assert(RetireCapacity > 0, "RetireCapacity must be > 0");

public:
    RealtimeResourceSlot() = default;
    ~RealtimeResourceSlot() { reclaim_retired(); }

    RealtimeResourceSlot(const RealtimeResourceSlot&) = delete;
    RealtimeResourceSlot& operator=(const RealtimeResourceSlot&) = delete;

    const T* get() const noexcept {
        return current_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool publish(std::unique_ptr<const T> next) {
        return realtime_resource_slot_publish_succeeded(
            publish_with_status(std::move(next)));
    }

    [[nodiscard]] RealtimeResourceSlotPublishStatus publish_with_status(
        std::unique_ptr<const T> next) {
        if (!next) return RealtimeResourceSlotPublishStatus::RejectedEmptyResource;

        auto previous = std::move(active_);
        if (previous && !retired_.try_push(std::move(previous))) {
            active_ = std::move(previous);
            retire_overflow_count_.fetch_add(1, std::memory_order_relaxed);
            return RealtimeResourceSlotPublishStatus::RejectedRetireQueueFull;
        }

        const T* next_ptr = next.get();
        active_ = std::move(next);
        current_.store(next_ptr, std::memory_order_release);
        return RealtimeResourceSlotPublishStatus::Published;
    }

    std::size_t reclaim_retired(std::size_t max_to_reclaim = RetireCapacity) {
        std::size_t reclaimed = 0;
        while (reclaimed < max_to_reclaim) {
            auto retired = retired_.try_pop();
            if (!retired) break;
            ++reclaimed;
        }
        return reclaimed;
    }

    std::size_t retired_count_approx() const {
        return retired_.size_approx();
    }

    std::uint64_t retire_overflow_count() const {
        return retire_overflow_count_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<const T*> current_{nullptr};
    std::unique_ptr<const T> active_;
    SpscQueue<std::unique_ptr<const T>, RetireCapacity> retired_;
    std::atomic<std::uint64_t> retire_overflow_count_{0};
};

} // namespace pulp::runtime
