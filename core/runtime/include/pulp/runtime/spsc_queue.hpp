#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <choc/containers/choc_SingleReaderSingleWriterFIFO.h>

namespace pulp::runtime {

struct SpscQueueTelemetry {
    std::size_t size_approx = 0;
    std::size_t capacity = 0;
    std::uint64_t overflow_count = 0;
};

// Single-producer/single-consumer lock-free queue. Either endpoint may be the
// realtime thread: audio -> UI telemetry and UI -> audio commands are both
// supported, provided there is exactly one producer and one consumer.
// Wraps choc::fifo::SingleReaderSingleWriterFIFO
template<typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity > 0, "Capacity must be > 0");

public:
    SpscQueue() {
        fifo_.reset(Capacity);
    }

    // Producer: push an item. Returns false if full.
    bool try_push(const T& item) {
        const bool pushed = fifo_.push(item);
        if (!pushed) overflow_count_.fetch_add(1, std::memory_order_relaxed);
        return pushed;
    }

    bool try_push(T&& item) {
        const bool pushed = fifo_.push(std::move(item));
        if (!pushed) overflow_count_.fetch_add(1, std::memory_order_relaxed);
        return pushed;
    }

    // Consumer: pop an item into reusable storage.
    bool try_pop(T& item) {
        return fifo_.pop(item);
    }

    // Consumer: pop an item. Returns nullopt if empty.
    std::optional<T> try_pop() {
        T item{};
        if (try_pop(item)) {
            return item;
        }
        return std::nullopt;
    }

    bool empty() const {
        return fifo_.getUsedSlots() == 0;
    }

    std::size_t size_approx() const {
        return fifo_.getUsedSlots();
    }

    std::uint64_t overflow_count() const {
        return overflow_count_.load(std::memory_order_relaxed);
    }

    void reset_overflow_count() {
        overflow_count_.store(0, std::memory_order_relaxed);
    }

    SpscQueueTelemetry telemetry() const {
        return {
            .size_approx = size_approx(),
            .capacity = Capacity,
            .overflow_count = overflow_count(),
        };
    }

    static constexpr std::size_t capacity() { return Capacity; }

private:
    choc::fifo::SingleReaderSingleWriterFIFO<T> fifo_;
    std::atomic<std::uint64_t> overflow_count_{0};
};

} // namespace pulp::runtime
