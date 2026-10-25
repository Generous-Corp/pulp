#pragma once

// Sample-accurate parameter-event queue used for per-block automation.
//
// Ordering contract: events are sorted by sample_offset ascending before
// being handed to consumers. Callers that append events unordered must call
// sort() before passing the queue on. sort() is stable: equal sample_offsets
// keep their push order. Input already in time order is detected in one pass.
// Anything else -- notably VST3, which delivers one time-ordered queue per
// parameter, so a block's events arrive as one sorted run per parameter --
// goes through stable_sort_in_place (stable_sort.hpp), O(n log^2 n) and
// allocation-free. An insertion sort over those
// interleaved runs was quadratic: 32 parameters with 16 points each cost
// about 28 us per block. An adapter that knows its block size passes scratch
// to the two-argument sort(), a stable counting sort on the offset.
//
// Realtime contract: fixed-capacity storage only. push(), clear(), sort(),
// iteration, and events() do not allocate; overflow is reported through
// overflowed() / dropped_event_count() and the extra event is dropped.

#include <pulp/state/parameter.hpp>
#include <pulp/state/stable_sort.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace pulp::state {

struct ParameterEvent {
    ParamID param_id = 0;
    int32_t sample_offset = 0; // 0..num_samples-1 within the current block
    float value = 0.0f;        // plain parameter domain
    int32_t ramp_duration_sample_frames = 0;
};

/// Timestamped host modulation event. Unlike StateStore's block-scoped
/// modulation offset, this preserves the CLAP event sample position so a
/// processor can apply modulation at the exact frame it was received.
struct ModulationEvent {
    ParamID param_id = 0;
    int32_t sample_offset = 0;
    float amount = 0.0f;
};

class ModulationEventQueue {
public:
    static constexpr std::size_t kCapacity = 1024;
    bool push(const ModulationEvent& e) {
        if (size_ >= events_.size()) { ++dropped_events_; return false; }
        events_[size_++] = e; return true;
    }
    void clear() { size_ = 0; dropped_events_ = 0; }
    std::size_t size() const { return size_; }
    bool overflowed() const { return dropped_events_ != 0; }
    std::span<const ModulationEvent> events() const { return {events_.data(), size_}; }
    void sort() {
        for (std::size_t i = 1; i < size_; ++i) {
            auto current = events_[i]; auto j = i;
            while (j > 0 && events_[j - 1].sample_offset > current.sample_offset) {
                events_[j] = events_[j - 1]; --j;
            }
            events_[j] = current;
        }
    }
private:
    std::array<ModulationEvent, kCapacity> events_{};
    std::size_t size_ = 0;
    std::uint32_t dropped_events_ = 0;
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
        stable_sort_in_place(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(size_),
                             [](const ParameterEvent& a, const ParameterEvent& b) {
                                 return a.sample_offset < b.sample_offset;
                             });
    }

    /// sort() for a caller that can lend scratch storage, typically sized once
    /// at prepare time: a stable counting sort on sample_offset, O(n + range),
    /// when every offset lies in [0, offset_counts.size()) and `scratch` holds
    /// size() events; otherwise sort(). Allocation-free either way.
    void sort(std::span<ParameterEvent> scratch, std::span<std::uint32_t> offset_counts) {
        const auto first = events_.begin();
        const auto last = events_.begin() + static_cast<std::ptrdiff_t>(size_);
        if (std::is_sorted(first, last, [](const ParameterEvent& a, const ParameterEvent& b) {
                return a.sample_offset < b.sample_offset;
            }))
            return;
        if (!stable_counting_sort(
                first, last, [](const ParameterEvent& e) { return e.sample_offset; }, scratch,
                offset_counts))
            sort();
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
