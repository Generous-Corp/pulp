#pragma once

// Sample-accurate parameter-event queue used for per-block automation.
//
// Ordering contract: events are sorted by sample_offset ascending before
// being handed to consumers. Callers that append events unordered must call
// sort() before passing the queue on. sort() is a stable insertion sort:
// equal sample_offsets keep their push order. It is tuned for the common case
// where hosts deliver automation already in (or near) sample-offset order —
// O(n) on sorted input, O(n*shift) in general, and O(n^2) only on a fully
// reversed adversarial flood. n is bounded by kCapacity, so even the worst
// case is a bounded, allocation-free pass on the audio thread; there is no
// unbounded blowup. If a future audio-rate source can deliver large reverse-
// ordered batches, switch to a counting/bucket sort keyed on sample_offset
// (offsets are bounded by the block size) rather than raising kCapacity.
//
// Realtime contract: fixed-capacity storage only. push(), clear(), sort(),
// iteration, and events() do not allocate; overflow is reported through
// overflowed() / dropped_event_count() and the extra event is dropped.

#include <pulp/state/parameter.hpp>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace pulp::state {

struct ParameterEvent {
    ParamID param_id = 0;
    int32_t sample_offset = 0; // 0..num_samples-1 within the current block
    float value = 0.0f;        // plain parameter domain
    int32_t ramp_duration_sample_frames = 0;
};

struct ParameterEventQueueTelemetry {
    std::size_t size = 0;
    std::size_t capacity = 0;
    std::uint64_t overflow_count = 0;
};

class ParameterEventQueue {
public:
    static constexpr std::size_t kCapacity = 1024;

    ParameterEventQueue() = default;

    bool push(const ParameterEvent& e) {
        if (size_ >= events_.size()) {
            record_drop();
            return false;
        }
        events_[size_++] = e;
        return true;
    }

    void clear() {
        size_ = 0;
        dropped_events_ = 0;
    }
    bool empty() const { return size_ == 0; }
    std::size_t size() const { return size_; }
    constexpr std::size_t capacity() const { return kCapacity; }
    bool overflowed() const { return dropped_events_ != 0; }
    std::uint32_t dropped_event_count() const { return dropped_events_; }
    std::uint64_t overflow_count() const {
        return overflow_count_.load(std::memory_order_relaxed);
    }
    void reset_overflow_count() {
        overflow_count_.store(0, std::memory_order_relaxed);
    }

    ParameterEventQueueTelemetry telemetry() const {
        return {
            .size = size_,
            .capacity = kCapacity,
            .overflow_count = overflow_count(),
        };
    }

    void sort() {
        for (std::size_t i = 1; i < size_; ++i) {
            auto current = events_[i];
            auto j = i;
            while (j > 0 && events_[j - 1].sample_offset > current.sample_offset) {
                events_[j] = events_[j - 1];
                --j;
            }
            events_[j] = current;
        }
    }

    using iterator = std::array<ParameterEvent, kCapacity>::iterator;
    using const_iterator = std::array<ParameterEvent, kCapacity>::const_iterator;

    iterator begin() { return events_.begin(); }
    iterator end() { return events_.begin() + static_cast<std::ptrdiff_t>(size_); }
    const_iterator begin() const { return events_.begin(); }
    const_iterator end() const { return events_.begin() + static_cast<std::ptrdiff_t>(size_); }

    std::span<const ParameterEvent> events() const {
        return {events_.data(), size_};
    }

private:
    void record_drop() {
        if (dropped_events_ < std::numeric_limits<std::uint32_t>::max()) {
            ++dropped_events_;
        }
        overflow_count_.fetch_add(1, std::memory_order_relaxed);
    }

    std::array<ParameterEvent, kCapacity> events_{};
    std::size_t size_ = 0;
    std::uint32_t dropped_events_ = 0;
    std::atomic<std::uint64_t> overflow_count_{0};
};

} // namespace pulp::state
