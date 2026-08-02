#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

namespace pulp::inspect::detail {

enum class EventQueuePushResult {
    Queued,
    DroppedLossy,
    ReliableOverflow,
};

template <typename Value>
class BoundedEventQueue {
public:
    explicit BoundedEventQueue(std::size_t capacity)
        : capacity_(capacity) {}

    EventQueuePushResult push(Value value, bool lossy) {
        if (entries_.size() >= capacity_) {
            const auto replaceable = std::find_if(
                entries_.begin(), entries_.end(),
                [](const auto& entry) { return entry.lossy; });
            if (replaceable != entries_.end()) {
                entries_.erase(replaceable);
            } else if (lossy) {
                return EventQueuePushResult::DroppedLossy;
            } else {
                return EventQueuePushResult::ReliableOverflow;
            }
        }
        entries_.push_back(Entry{std::move(value), lossy});
        return EventQueuePushResult::Queued;
    }

    std::optional<Value> take_front() {
        if (entries_.empty())
            return std::nullopt;
        auto value = std::move(entries_.front().value);
        entries_.pop_front();
        return value;
    }

    bool empty() const { return entries_.empty(); }
    std::size_t size() const { return entries_.size(); }
    void clear() { entries_.clear(); }

private:
    struct Entry {
        Value value;
        bool lossy = false;
    };

    std::size_t capacity_;
    std::deque<Entry> entries_;
};

} // namespace pulp::inspect::detail
