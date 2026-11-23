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
// Appended after cancel so the existing wire values (press=0 .. cancel=4) never change.
enum class InteractionPhase : std::uint8_t { press, hold, move, release, cancel, invalid };

struct InteractionEvent {
    std::uint32_t source_id = 0;
    std::uint32_t epoch = 0;
    // Sequence is monotonic within a source and epoch.
    std::uint64_t sequence = 0;
    std::uint64_t provider_time_ns = 0;
    std::uint32_t object_id = 0;
    // hold is a provider keep-alive. It does not imply a new musical attack.
    InteractionPhase phase = InteractionPhase::invalid;
    std::array<std::uint8_t, 3> reserved{};
    // Coordinates are finite and normalized to the provider's declared 0..1
    // surface, with origin and y direction documented by that provider.
    float x = 0.0f;
    float y = 0.0f;
    // Strike or contact strength, normalized to 0..1. Motion velocity is a
    // separate derived value and is not part of this wire contract yet.
    float strength = 0.0f;
    float confidence = 0.0f;
};

static_assert(std::is_trivially_copyable_v<InteractionEvent>);
static_assert(std::is_standard_layout_v<InteractionEvent>);
static_assert(sizeof(InteractionEvent) == 48);
static_assert(offsetof(InteractionEvent, x) == 32);
static_assert(offsetof(InteractionEvent, y) == 36);
static_assert(offsetof(InteractionEvent, strength) == 40);
static_assert(offsetof(InteractionEvent, confidence) == 44);

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
        if (source_seen_ && event.source_id != source_id_)
            return InteractionVerdict::rejected_source;
        if (source_seen_ && event.epoch < epoch_)
            return InteractionVerdict::rejected_order;
        if (source_seen_ && event.epoch == epoch_ && event.sequence <= sequence_)
            return event.sequence == sequence_ ? InteractionVerdict::unchanged
                                               : InteractionVerdict::rejected_order;
        const auto terminal_slot = find(event.object_id);
        if (terminal_slot >= 0 &&
            (event.phase == InteractionPhase::release || event.phase == InteractionPhase::cancel)) {
            auto terminal = event;
            if (!std::isfinite(terminal.x) || terminal.x < 0.0f || terminal.x > 1.0f)
                terminal.x = active_[terminal_slot].x;
            if (!std::isfinite(terminal.y) || terminal.y < 0.0f || terminal.y > 1.0f)
                terminal.y = active_[terminal_slot].y;
            if (!std::isfinite(terminal.strength) || terminal.strength < 0.0f ||
                terminal.strength > 1.0f)
                terminal.strength = active_[terminal_slot].strength;
            if (!std::isfinite(terminal.confidence) || terminal.confidence < 0.0f ||
                terminal.confidence > 1.0f)
                terminal.confidence = active_[terminal_slot].confidence;
            if (!source_seen_)
                source_id_ = terminal.source_id, source_seen_ = true;
            epoch_ = terminal.epoch;
            sequence_ = terminal.sequence;
            active_[terminal_slot] = {};
            ages_[terminal_slot] = 0;
            output.append(terminal);
            return InteractionVerdict::accepted;
        }
        if (event.phase == InteractionPhase::invalid || !std::isfinite(event.x) ||
            !std::isfinite(event.y) || !std::isfinite(event.confidence) || event.x < 0.0f ||
            event.x > 1.0f || event.y < 0.0f || event.y > 1.0f)
            return InteractionVerdict::rejected_malformed;
        if (event.confidence < 0.0f || event.confidence > 1.0f || !std::isfinite(event.strength) ||
            event.strength < 0.0f || event.strength > 1.0f)
            return InteractionVerdict::rejected_confidence;

        if (!source_seen_) {
            source_id_ = event.source_id;
            source_seen_ = true;
        }
        if (event.epoch > epoch_) {
            cancel_active(output, event);
            epoch_ = event.epoch;
            active_ = {};
            ages_ = {};
        }
        epoch_ = event.epoch;
        sequence_ = event.sequence;

        const auto slot = find(event.object_id);
        if (event.phase == InteractionPhase::press) {
            if (slot >= 0)
                output.append(cancel_for(active_[slot], event));
            const auto free_slot = slot >= 0 ? slot : find_free();
            if (free_slot < 0)
                return InteractionVerdict::rejected_capacity;
            active_[free_slot] = event;
            active_[free_slot].phase = InteractionPhase::press;
            ages_[free_slot] = 0;
            output.append(event);
            return InteractionVerdict::accepted;
        }
        if (slot < 0)
            return output.count != 0 ? InteractionVerdict::accepted
                                     : InteractionVerdict::rejected_order;
        ages_[slot] = 0;
        if (event.phase == InteractionPhase::release || event.phase == InteractionPhase::cancel)
            active_[slot] = {}, ages_[slot] = 0;
        else
            active_[slot] = event;
        output.append(event);
        return InteractionVerdict::accepted;
    }

    Result advance(std::uint64_t samples, std::uint64_t max_age_samples) noexcept {
        Result output;
        if (max_age_samples == 0 || active_count() == 0)
            return output;
        InteractionEvent reason{};
        reason.source_id = source_id_;
        reason.epoch = epoch_;
        reason.sequence = sequence_;
        reason.phase = InteractionPhase::cancel;
        for (std::size_t i = 0; i < active_.size(); ++i) {
            if (active_[i].phase == InteractionPhase::invalid)
                continue;
            ages_[i] = UINT64_MAX - ages_[i] < samples ? UINT64_MAX : ages_[i] + samples;
            if (ages_[i] >= max_age_samples) {
                output.append(cancel_for(active_[i], reason));
                active_[i] = {};
                ages_[i] = 0;
            }
        }
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
        ages_ = {};
        source_seen_ = false;
        source_id_ = 0;
        epoch_ = 0;
        sequence_ = 0;
        return output;
    }

    std::size_t active_count() const noexcept {
        std::size_t count = 0;
        for (const auto& event : active_)
            if (event.phase != InteractionPhase::invalid)
                ++count;
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
            if (active_[i].phase == InteractionPhase::invalid)
                return static_cast<int>(i);
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
        for (std::size_t i = 0; i < active_.size(); ++i)
            if (active_[i].phase != InteractionPhase::invalid) {
                output.append(cancel_for(active_[i], reason));
                active_[i] = {};
                ages_[i] = 0;
            }
    }

    std::array<InteractionEvent, MaxObjects> active_{};
    std::array<std::uint64_t, MaxObjects> ages_{};
    std::uint32_t source_id_ = 0, epoch_ = 0;
    std::uint64_t sequence_ = 0;
    bool source_seen_ = false;
};

} // namespace pulp::runtime
