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
            update_ramps_to(events_[next_event_].sample_offset);
            apply_event(events_[next_event_]);
            ++next_event_;
        }

        update_ramps_to(sample_offset);
        position_ = sample_offset;
    }

    float value(ParamID id) const {
        if (const auto* entry = find_entry(id)) return entry->value;
        return store_ ? store_->get_value(id) : 0.0f;
    }

    float value_at(ParamID id, int32_t sample_offset) const {
        if (const auto* entry = find_entry(id)) {
            return value_for_entry_at(*entry, sample_offset);
        }
        return store_ ? store_->get_value(id) : 0.0f;
    }

    bool is_ramping(ParamID id) const {
        if (const auto* entry = find_entry(id)) return entry->ramping;
        return false;
    }

    // In-flight ramp introspection, for continuing a ramp across block
    // boundaries (a caller that owns the cursor's lifetime can read the active
    // ramp's target + block-relative end offset and re-seed the next block).
    // Meaningful only while is_ramping(id) is true; both return a benign
    // fallback for an untracked or non-ramping param.
    float ramp_target(ParamID id) const {
        if (const auto* entry = find_entry(id)) return entry->ramp_target_value;
        return value(id);
    }
    int32_t ramp_end_offset(ParamID id) const {
        if (const auto* entry = find_entry(id)) return entry->ramp_end_sample_offset;
        return 0;
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
        float ramp_start_value = 0.0f;
        float ramp_target_value = 0.0f;
        int32_t ramp_start_sample_offset = 0;
        int32_t ramp_end_sample_offset = 0;
        bool ramping = false;
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
            entry->ramping = false;
            return;
        }
        if (entry_count_ >= entries_.size()) return;
        const float clamped = clamp_value(id, value);
        entries_[entry_count_++] = Entry{
            .param_id = id,
            .value = clamped,
            .ramp_start_value = clamped,
            .ramp_target_value = clamped,
        };
    }

    void ensure_event_entry(ParamID id) {
        if (find_entry(id) != nullptr) return;
        if (!is_registered(id)) return;
        add_or_update(id, store_->get_value(id), false);
    }

    float value_for_entry_at(const Entry& entry, int32_t sample_offset) const {
        if (!entry.ramping) return entry.value;
        if (sample_offset <= entry.ramp_start_sample_offset) {
            return entry.ramp_start_value;
        }
        if (sample_offset >= entry.ramp_end_sample_offset) {
            return entry.ramp_target_value;
        }

        const auto elapsed = static_cast<float>(sample_offset - entry.ramp_start_sample_offset);
        const auto duration = static_cast<float>(entry.ramp_end_sample_offset
                                                 - entry.ramp_start_sample_offset);
        const float t = duration > 0.0f ? (elapsed / duration) : 1.0f;
        return entry.ramp_start_value + (entry.ramp_target_value - entry.ramp_start_value) * t;
    }

    void update_ramps_to(int32_t sample_offset) {
        for (std::size_t i = 0; i < entry_count_; ++i) {
            auto& entry = entries_[i];
            if (!entry.ramping) continue;
            entry.value = value_for_entry_at(entry, sample_offset);
            if (sample_offset >= entry.ramp_end_sample_offset) {
                entry.value = entry.ramp_target_value;
                entry.ramping = false;
            }
        }
    }

    int32_t ramp_end_for(const ParameterEvent& event) const {
        if (event.ramp_duration_sample_frames <= 0) return event.sample_offset;
        const auto max = std::numeric_limits<int32_t>::max();
        if (event.sample_offset > max - event.ramp_duration_sample_frames) return max;
        return event.sample_offset + event.ramp_duration_sample_frames;
    }

    void apply_event(const ParameterEvent& event) {
        if (!is_registered(event.param_id)) return;
        ensure_event_entry(event.param_id);
        auto* entry = find_entry(event.param_id);
        if (!entry) return;

        const float target = clamp_value(event.param_id, event.value);
        if (event.ramp_duration_sample_frames <= 0) {
            entry->value = target;
            entry->ramping = false;
            return;
        }

        entry->ramp_start_value = entry->value;
        entry->ramp_target_value = target;
        entry->ramp_start_sample_offset = event.sample_offset;
        entry->ramp_end_sample_offset = ramp_end_for(event);
        entry->ramping = entry->ramp_end_sample_offset > entry->ramp_start_sample_offset;
        if (!entry->ramping) {
            entry->value = target;
        }
    }

    const StateStore* store_ = nullptr;
    std::span<const ParameterEvent> events_;
    std::size_t next_event_ = 0;
    std::array<Entry, ParameterEventQueue::kCapacity> entries_{};
    std::size_t entry_count_ = 0;
    int32_t position_ = std::numeric_limits<int32_t>::min();
};

} // namespace pulp::state
