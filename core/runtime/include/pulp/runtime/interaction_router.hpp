#pragma once

#include "pulp/runtime/interaction_event.hpp"
#include "pulp/runtime/spsc_queue.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace pulp::runtime {

template <std::size_t QueueCapacity = 64, std::size_t MaxObjects = 64>
class InteractionRouter final {
    static_assert(QueueCapacity > 0);
    static_assert(MaxObjects > 0);

  public:
    using Clock = std::uint64_t (*)() noexcept;

    explicit InteractionRouter(std::uint64_t source_timeout_ns = 0, Clock clock = nullptr) noexcept
        : source_timeout_ns_(source_timeout_ns), clock_(clock) {}

    bool publish(const InteractionEvent& event) noexcept {
        typename InteractionTracker<MaxObjects>::Result output;
        const auto verdict = tracker_.submit(event, output);
        if (verdict != InteractionVerdict::accepted)
            return false;
        if (event.provider_time_ns != 0)
            last_event_ns_ = event.provider_time_ns;
        else if (clock_ != nullptr)
            last_event_ns_ = clock_();
        bool delivered = true;
        for (const auto& item : output.events_view()) {
            if (fan_out(item))
                continue;
            delivered = false;
            if (item.phase == InteractionPhase::cancel)
                continue;
            const auto cancelled = tracker_.reset();
            for (const auto& cancel : cancelled.events_view())
                (void)fan_out(cancel);
        }
        return delivered;
    }

    bool check_timeout() noexcept {
        if (clock_ == nullptr)
            return false;
        return check_timeout(clock_());
    }

    bool check_timeout(std::uint64_t now_ns) noexcept {
        if (source_timeout_ns_ == 0 || last_event_ns_ == 0 || now_ns < last_event_ns_ ||
            now_ns - last_event_ns_ < source_timeout_ns_)
            return false;
        const auto output = tracker_.advance(now_ns - last_event_ns_, source_timeout_ns_);
        last_event_ns_ = now_ns;
        bool cancelled = false;
        for (const auto& item : output.events_view()) {
            cancelled = true;
            (void)fan_out(item);
        }
        return cancelled;
    }

    bool try_pop_ui(InteractionEvent& event) noexcept {
        if (ui_queue_.try_pop(event))
            return true;
        return pop_pending(pending_ui_, event);
    }

    bool try_pop_audio(InteractionEvent& event) noexcept {
        if (audio_queue_.try_pop(event))
            return true;
        return pop_pending(pending_audio_, event);
    }

    std::uint64_t ui_overflow_count() const noexcept {
        return ui_queue_.overflow_count();
    }
    std::uint64_t audio_overflow_count() const noexcept {
        return audio_queue_.overflow_count();
    }

    void reset() noexcept {
        const auto output = tracker_.reset();
        for (const auto& item : output.events_view())
            (void)fan_out(item);
        last_event_ns_ = 0;
    }

  private:
    bool fan_out(const InteractionEvent& event) noexcept {
        const bool ui_pushed = ui_queue_.try_push(event);
        const bool audio_pushed = audio_queue_.try_push(event);
        if (event.phase == InteractionPhase::cancel) {
            if (!ui_pushed)
                (void)remember_pending(pending_ui_, event);
            if (!audio_pushed)
                (void)remember_pending(pending_audio_, event);
        }
        return ui_pushed && audio_pushed;
    }

    static bool pop_pending(std::array<InteractionEvent, MaxObjects>& pending,
                            InteractionEvent& event) noexcept {
        for (auto& slot : pending) {
            if (slot.phase != InteractionPhase::cancel)
                continue;
            event = slot;
            slot = {};
            return true;
        }
        return false;
    }

    static bool remember_pending(std::array<InteractionEvent, MaxObjects>& pending,
                                 const InteractionEvent& event) noexcept {
        for (auto& slot : pending) {
            if (slot.phase == InteractionPhase::cancel && slot.object_id == event.object_id)
                return true;
        }
        for (auto& slot : pending) {
            if (slot.phase == InteractionPhase::invalid) {
                slot = event;
                return true;
            }
        }
        return false;
    }

    InteractionTracker<MaxObjects> tracker_;
    SpscQueue<InteractionEvent, QueueCapacity> ui_queue_;
    SpscQueue<InteractionEvent, QueueCapacity> audio_queue_;
    std::array<InteractionEvent, MaxObjects> pending_ui_{};
    std::array<InteractionEvent, MaxObjects> pending_audio_{};
    std::uint64_t last_event_ns_ = 0;
    std::uint64_t source_timeout_ns_ = 0;
    Clock clock_ = nullptr;
};

} // namespace pulp::runtime
