#pragma once

#include "pulp/runtime/interaction_event.hpp"
#include "pulp/runtime/spsc_queue.hpp"

#include <array>
#include <atomic>
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

    /// Cancels that could not be kept because every pending slot was in use.
    std::uint64_t pending_cancel_dropped_count() const noexcept {
        return pending_dropped_.load(std::memory_order_relaxed);
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

    // One overflowed cancel per object, kept until its consumer pops it. The
    // producer thread is the only writer of `event` (and only while `ready` is
    // false); the consumer thread only reads it while `ready` is true and then
    // hands the slot back by clearing `ready` (release). The event bytes are
    // never touched by the consumer, so no slot is ever read and written at once.
    struct PendingSlot {
        InteractionEvent event{};
        std::atomic<bool> ready{false};
    };
    using PendingArray = std::array<PendingSlot, MaxObjects>;

    static bool pop_pending(PendingArray& pending, InteractionEvent& event) noexcept {
        for (auto& slot : pending) {
            if (!slot.ready.load(std::memory_order_acquire))
                continue;
            event = slot.event;
            slot.ready.store(false, std::memory_order_release);
            return true;
        }
        return false;
    }

    // Producer thread only.
    bool remember_pending(PendingArray& pending, const InteractionEvent& event) noexcept {
        PendingSlot* free_slot = nullptr;
        for (auto& slot : pending) {
            if (slot.ready.load(std::memory_order_acquire)) {
                if (slot.event.object_id == event.object_id)
                    return true;
            } else if (free_slot == nullptr) {
                free_slot = &slot;
            }
        }
        if (free_slot == nullptr) {
            pending_dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        free_slot->event = event;
        free_slot->ready.store(true, std::memory_order_release);
        return true;
    }

    InteractionTracker<MaxObjects> tracker_;
    SpscQueue<InteractionEvent, QueueCapacity> ui_queue_;
    SpscQueue<InteractionEvent, QueueCapacity> audio_queue_;
    PendingArray pending_ui_{};
    PendingArray pending_audio_{};
    std::atomic<std::uint64_t> pending_dropped_{0};
    std::uint64_t last_event_ns_ = 0;
    std::uint64_t source_timeout_ns_ = 0;
    Clock clock_ = nullptr;
};

} // namespace pulp::runtime
