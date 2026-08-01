#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
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

    EventQueuePushResult push(Value value, bool lossy,
                              bool* evicted_lossy = nullptr) {
        if (evicted_lossy)
            *evicted_lossy = false;
        if (entries_.size() >= capacity_) {
            const auto replaceable = std::find_if(
                entries_.begin(), entries_.end(),
                [](const auto& entry) { return entry.lossy; });
            if (replaceable != entries_.end()) {
                entries_.erase(replaceable);
                if (evicted_lossy)
                    *evicted_lossy = true;
            } else if (lossy) {
                return EventQueuePushResult::DroppedLossy;
            } else {
                return EventQueuePushResult::ReliableOverflow;
            }
        }
        entries_.push_back(Entry{std::move(value), lossy, {}});
        return EventQueuePushResult::Queued;
    }

    /// Push into a loss-accounted queue. A lossy value may replace only an
    /// older value carrying the same owner key; otherwise the new value is the
    /// one reported dropped. Reliable values never silently evict another
    /// owner's lossy data.
    EventQueuePushResult push_isolated(Value value, bool lossy,
                                       std::string owner,
                                       bool* evicted_same_owner = nullptr) {
        if (evicted_same_owner)
            *evicted_same_owner = false;
        if (entries_.size() >= capacity_) {
            if (!lossy)
                return EventQueuePushResult::ReliableOverflow;
            const auto replaceable = std::find_if(
                entries_.begin(), entries_.end(),
                [&](const auto& entry) {
                    return entry.lossy && entry.owner == owner;
                });
            if (replaceable == entries_.end())
                return EventQueuePushResult::DroppedLossy;
            entries_.erase(replaceable);
            if (evicted_same_owner)
                *evicted_same_owner = true;
        }
        entries_.push_back(Entry{std::move(value), lossy, std::move(owner)});
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
        std::string owner;
    };

    std::size_t capacity_;
    std::deque<Entry> entries_;
};

} // namespace pulp::inspect::detail
