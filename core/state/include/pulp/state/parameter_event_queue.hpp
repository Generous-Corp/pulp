#pragma once

// Sample-accurate parameter-event queue used for per-block automation.
//
// Ordering contract: events are sorted by sample_offset ascending before
// being handed to consumers. Callers that append events unordered must call
// sort() before passing the queue on.

#include <pulp/state/parameter.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pulp::state {

struct ParameterEvent {
    ParamID param_id = 0;
    int32_t sample_offset = 0; // 0..num_samples-1 within the current block
    float value = 0.0f;        // plain parameter domain
    int32_t ramp_duration_sample_frames = 0;
};

class ParameterEventQueue {
public:
    static constexpr std::size_t kCapacity = 1024;

    ParameterEventQueue() = default;

    bool push(const ParameterEvent& e) {
        if (size_ >= events_.size()) return false;
        events_[size_++] = e;
        return true;
    }

    void clear() { size_ = 0; }
    bool empty() const { return size_ == 0; }
    std::size_t size() const { return size_; }
    constexpr std::size_t capacity() const { return kCapacity; }

    void sort() {
        std::sort(begin(), end(),
            [](const ParameterEvent& a, const ParameterEvent& b) {
                return a.sample_offset < b.sample_offset;
            });
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
    std::array<ParameterEvent, kCapacity> events_{};
    std::size_t size_ = 0;
};

} // namespace pulp::state
