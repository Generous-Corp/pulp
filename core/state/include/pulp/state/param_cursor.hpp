#pragma once

// Block-local parameter cursor for sample-accurate automation.
//
// The cursor advances over a sorted ParameterEventQueue and maintains a
// fixed-size value view for the parameters touched by the block. It never
// mutates StateStore, allocates heap storage, or invokes listeners.

#include <pulp/state/parameter_event_queue.hpp>
#include <pulp/state/store.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace pulp::state {

struct ParamSnapshotEntry {
    ParamID param_id = 0;
    float value = 0.0f;
};

class ParamCursor {
public:
    ParamCursor(const StateStore& store,
                const ParameterEventQueue* events,
                std::span<const ParamSnapshotEntry> initial_values = {})
        : store_(&store)
        , events_(events ? events->events() : std::span<const ParameterEvent>{})
    {
        for (const auto& initial : initial_values) {
            add_or_update(initial.param_id, initial.value, true);
        }

        for (const auto& event : events_) {
            ensure_event_entry(event.param_id);
        }
    }

    void advance_to(int32_t sample_offset) {
        if (sample_offset < position_) return;

        while (next_event_ < events_.size() &&
               events_[next_event_].sample_offset <= sample_offset) {
            apply_event(events_[next_event_]);
            ++next_event_;
        }

        position_ = sample_offset;
    }

    float value(ParamID id) const {
        if (const auto* entry = find_entry(id)) return entry->value;
        return store_ ? store_->get_value(id) : 0.0f;
    }

    bool is_tracked(ParamID id) const {
        return find_entry(id) != nullptr;
    }

    std::size_t tracked_param_count() const { return entry_count_; }
    int32_t position() const { return position_; }

private:
    struct Entry {
        ParamID param_id = 0;
        float value = 0.0f;
    };

    Entry* find_entry(ParamID id) {
        for (std::size_t i = 0; i < entry_count_; ++i) {
            if (entries_[i].param_id == id) return &entries_[i];
        }
        return nullptr;
    }

    const Entry* find_entry(ParamID id) const {
        for (std::size_t i = 0; i < entry_count_; ++i) {
            if (entries_[i].param_id == id) return &entries_[i];
        }
        return nullptr;
    }

    float clamp_value(ParamID id, float value) const {
        if (store_) {
            if (const auto* info = store_->info(id)) {
                return std::clamp(value, info->range.min, info->range.max);
            }
        }
        return value;
    }

    bool is_registered(ParamID id) const {
        return store_ && store_->info(id) != nullptr;
    }

    void add_or_update(ParamID id, float value, bool allow_unregistered) {
        if (!allow_unregistered && !is_registered(id)) return;
        if (auto* entry = find_entry(id)) {
            entry->value = clamp_value(id, value);
            return;
        }
        if (entry_count_ >= entries_.size()) return;
        entries_[entry_count_++] = Entry{id, clamp_value(id, value)};
    }

    void ensure_event_entry(ParamID id) {
        if (find_entry(id) != nullptr) return;
        if (!is_registered(id)) return;
        add_or_update(id, store_->get_value(id), false);
    }

    void apply_event(const ParameterEvent& event) {
        if (find_entry(event.param_id) == nullptr && !is_registered(event.param_id)) {
            return;
        }
        add_or_update(event.param_id, event.value, false);
    }

    const StateStore* store_ = nullptr;
    std::span<const ParameterEvent> events_;
    std::size_t next_event_ = 0;
    std::array<Entry, ParameterEventQueue::kCapacity> entries_{};
    std::size_t entry_count_ = 0;
    int32_t position_ = std::numeric_limits<int32_t>::min();
};

} // namespace pulp::state
