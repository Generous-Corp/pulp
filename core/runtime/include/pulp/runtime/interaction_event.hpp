#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace pulp::runtime {

// Provider-neutral phases for bounded interactions such as a press, pluck,
// drag, strum, drawing trace, or tracked-object release.
enum class InteractionPhase : std::uint8_t { invalid, press, hold, move, release, cancel };

struct InteractionEvent {
    std::uint32_t source_id = 0;
    std::uint32_t epoch = 0;
    // Sequence is monotonic within a source and epoch.
    std::uint64_t sequence = 0;
    std::uint64_t provider_time_ns = 0;
    std::uint32_t object_id = 0;
    // hold is a provider keep-alive. It does not imply a new musical attack.
    InteractionPhase phase = InteractionPhase::invalid;
    // Coordinates are finite and normalized to the provider's declared 0..1
    // surface, with origin and y direction documented by that provider.
    float x = 0.0f;
    float y = 0.0f;
    float confidence = 0.0f;
};

static_assert(std::is_trivially_copyable_v<InteractionEvent>);

template <std::size_t Capacity = 16> struct InteractionBatch {
    static_assert(Capacity >= 1 && Capacity <= 64);
    std::array<InteractionEvent, Capacity> events{};
    std::uint8_t count = 0;

    bool append(const InteractionEvent& event) noexcept {
        if (count >= Capacity)
            return false;
        events[count++] = event;
        return true;
    }

    std::span<const InteractionEvent> events_view() const noexcept {
        const auto bounded_count = count > Capacity ? Capacity : count;
        return {events.data(), bounded_count};
    }
};

enum class InteractionVerdict : std::uint8_t {
    accepted,
    unchanged,
    rejected_source,
    rejected_order,
    rejected_malformed,
    rejected_confidence,
    rejected_capacity,
};

template <std::size_t MaxObjects = 16> class InteractionTracker {
  public:
    static_assert(MaxObjects >= 1 && MaxObjects < 64);
    using Result = InteractionBatch<MaxObjects + 1>;

    InteractionVerdict submit(const InteractionEvent& event, Result& output) noexcept {
        output = {};
        if (source_seen_ && event.source_id != source_id_) return InteractionVerdict::rejected_source;
        if (source_seen_ && event.epoch == epoch_ && event.sequence <= sequence_)
            return event.sequence == sequence_ ? InteractionVerdict::unchanged
                                                : InteractionVerdict::rejected_order;
        if (event.phase == InteractionPhase::invalid || !std::isfinite(event.x) ||
            !std::isfinite(event.y) || !std::isfinite(event.confidence) ||
            event.x < 0.0f || event.x > 1.0f || event.y < 0.0f || event.y > 1.0f)
            return InteractionVerdict::rejected_malformed;
        if (event.confidence < 0.0f || event.confidence > 1.0f)
            return InteractionVerdict::rejected_confidence;

        if (!source_seen_) {
            source_id_ = event.source_id;
            source_seen_ = true;
        }
        if (event.epoch != epoch_) {
            cancel_active(output, event);
            epoch_ = event.epoch;
            active_ = {};
        }
        epoch_ = event.epoch;
        sequence_ = event.sequence;
        age_samples_ = 0;

        const auto slot = find(event.object_id);
        if (event.phase == InteractionPhase::press) {
            if (slot >= 0) output.append(cancel_for(active_[slot], event));
            const auto free_slot = slot >= 0 ? slot : find_free();
            if (free_slot < 0) return InteractionVerdict::rejected_capacity;
            active_[free_slot] = event;
            active_[free_slot].phase = InteractionPhase::press;
            output.append(event);
            return InteractionVerdict::accepted;
        }
        if (slot < 0) return InteractionVerdict::rejected_order;
        if (event.phase == InteractionPhase::release || event.phase == InteractionPhase::cancel)
            active_[slot] = {};
        else
            active_[slot] = event;
        output.append(event);
        return InteractionVerdict::accepted;
    }

    Result advance(std::uint64_t samples, std::uint64_t max_age_samples) noexcept {
        Result output;
        if (max_age_samples == 0 || active_count() == 0) return output;
        age_samples_ = UINT64_MAX - age_samples_ < samples ? UINT64_MAX : age_samples_ + samples;
        if (age_samples_ < max_age_samples) return output;
        InteractionEvent reason{};
        reason.source_id = source_id_;
        reason.epoch = epoch_;
        reason.sequence = sequence_;
        reason.phase = InteractionPhase::cancel;
        cancel_active(output, reason);
        age_samples_ = 0;
        return output;
    }

    Result reset() noexcept {
        Result output;
        InteractionEvent reason{};
        reason.source_id = source_id_;
        reason.epoch = epoch_;
        reason.sequence = sequence_;
        reason.phase = InteractionPhase::cancel;
        cancel_active(output, reason);
        active_ = {};
        age_samples_ = 0;
        return output;
    }

    std::size_t active_count() const noexcept {
        std::size_t count = 0;
        for (const auto& event : active_)
            if (event.phase != InteractionPhase::invalid) ++count;
        return count;
    }

  private:
    int find(std::uint32_t object_id) const noexcept {
        for (std::size_t i = 0; i < active_.size(); ++i)
            if (active_[i].phase != InteractionPhase::invalid && active_[i].object_id == object_id)
                return static_cast<int>(i);
        return -1;
    }

    int find_free() const noexcept {
        for (std::size_t i = 0; i < active_.size(); ++i)
            if (active_[i].phase == InteractionPhase::invalid) return static_cast<int>(i);
        return -1;
    }

    static InteractionEvent cancel_for(const InteractionEvent& active,
                                       const InteractionEvent& reason) noexcept {
        auto cancel = reason;
        cancel.object_id = active.object_id;
        cancel.x = active.x;
        cancel.y = active.y;
        cancel.confidence = active.confidence;
        cancel.phase = InteractionPhase::cancel;
        return cancel;
    }

    void cancel_active(Result& output, const InteractionEvent& reason) noexcept {
        for (auto& active : active_)
            if (active.phase != InteractionPhase::invalid) {
                output.append(cancel_for(active, reason));
                active = {};
            }
    }

    std::array<InteractionEvent, MaxObjects> active_{};
    std::uint32_t source_id_ = 0, epoch_ = 0;
    std::uint64_t sequence_ = 0, age_samples_ = 0;
    bool source_seen_ = false;
};

} // namespace pulp::runtime
