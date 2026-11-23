#pragma once

#include "pulp/runtime/interaction_event.hpp"
#include "pulp/runtime/spsc_queue.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pulp::runtime {

/// Bounded producer-to-consumer transport for discrete interactions.
///
/// The producer writes each event to independent UI and audio queues. The
/// queues own their storage, and the consumer methods only copy into caller
/// storage. A source restart, timeout, or queue overflow closes every live
/// object with one cancel event per consumer.
template <std::size_t QueueCapacity = 64, std::size_t MaxObjects = 64>
class InteractionRouter final {
    static_assert(QueueCapacity > 0);
    static_assert(MaxObjects > 0);

  public:
    using Clock = std::uint64_t (*)() noexcept;

    explicit InteractionRouter(std::uint64_t source_timeout_ns = 0,
                               Clock clock = nullptr) noexcept
        : source_timeout_ns_(source_timeout_ns), clock_(clock) {}

    bool publish(const InteractionEvent& event) noexcept {
        if (event.phase == InteractionPhase::invalid)
            return false;

        if (!has_source_) {
            begin_source(event.source_id, event.epoch);
        } else if (event.source_id != source_id_ || event.epoch != epoch_) {
            cancel_source(source_id_, epoch_, event);
            begin_source(event.source_id, event.epoch);
        }

        const bool ui_pushed = ui_queue_.try_push(event);
        const bool audio_pushed = audio_queue_.try_push(event);
        if (!ui_pushed || !audio_pushed) {
            cancel_source(event.source_id, event.epoch, event);
            return false;
        }

        observe(event);
        return true;
    }

    /// Synthesizes source-loss cancels using the injected or configured clock.
    bool check_timeout() noexcept {
        if (clock_ == nullptr)
            return false;
        return check_timeout(clock_());
    }

    bool check_timeout(std::uint64_t now_ns) noexcept {
        if (!has_source_ || source_timeout_ns_ == 0 || last_event_ns_ == 0 ||
            now_ns < last_event_ns_ || now_ns - last_event_ns_ < source_timeout_ns_)
            return false;
        InteractionEvent cause{};
        cause.source_id = source_id_;
        cause.epoch = epoch_;
        cause.provider_time_ns = now_ns;
        cancel_source(source_id_, epoch_, cause);
        last_event_ns_ = now_ns;
        return true;
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

    std::uint64_t ui_overflow_count() const noexcept { return ui_queue_.overflow_count(); }
    std::uint64_t audio_overflow_count() const noexcept { return audio_queue_.overflow_count(); }

    void reset() noexcept {
        source_id_ = 0;
        epoch_ = 0;
        last_event_ns_ = 0;
        has_source_ = false;
        for (auto& object : objects_)
            object = {};
        for (auto& event : pending_ui_)
            event = {};
        for (auto& event : pending_audio_)
            event = {};
        pending_ui_count_ = 0;
        pending_audio_count_ = 0;
    }

  private:
    struct LiveObject {
        std::uint32_t source_id = 0;
        std::uint32_t epoch = 0;
        std::uint32_t object_id = 0;
        bool active = false;
    };

    static constexpr std::uint32_t kNoObject = std::numeric_limits<std::uint32_t>::max();

    void begin_source(std::uint32_t source_id, std::uint32_t epoch) noexcept {
        source_id_ = source_id;
        epoch_ = epoch;
        has_source_ = true;
        last_event_ns_ = 0;
        for (auto& object : objects_)
            object = {};
    }

    std::size_t find_object(std::uint32_t source_id, std::uint32_t epoch,
                            std::uint32_t object_id) const noexcept {
        for (std::size_t i = 0; i < objects_.size(); ++i) {
            const auto& object = objects_[i];
            if (object.active && object.source_id == source_id && object.epoch == epoch &&
                object.object_id == object_id)
                return i;
        }
        return objects_.size();
    }

    std::size_t allocate_object(std::uint32_t source_id, std::uint32_t epoch,
                                std::uint32_t object_id) noexcept {
        for (std::size_t i = 0; i < objects_.size(); ++i) {
            if (!objects_[i].active) {
                objects_[i] = {source_id, epoch, object_id, true};
                return i;
            }
        }
        return objects_.size();
    }

    void observe(const InteractionEvent& event) noexcept {
        last_event_ns_ = event.provider_time_ns;
        auto position = find_object(event.source_id, event.epoch, event.object_id);
        if (event.phase == InteractionPhase::press) {
            if (position == objects_.size())
                (void)allocate_object(event.source_id, event.epoch, event.object_id);
        } else if (event.phase == InteractionPhase::release ||
                   event.phase == InteractionPhase::cancel) {
            if (position != objects_.size())
                objects_[position] = {};
        }
    }

    void cancel_source(std::uint32_t source_id, std::uint32_t epoch,
                       const InteractionEvent& cause) noexcept {
        for (auto& object : objects_) {
            if (!object.active || object.source_id != source_id || object.epoch != epoch)
                continue;
            InteractionEvent cancel = cause;
            cancel.source_id = source_id;
            cancel.epoch = epoch;
            cancel.object_id = object.object_id;
            cancel.phase = InteractionPhase::cancel;
            (void)ui_queue_.try_push(cancel) || remember_pending(pending_ui_, pending_ui_count_, cancel);
            (void)audio_queue_.try_push(cancel) || remember_pending(pending_audio_, pending_audio_count_, cancel);
            object = {};
        }
    }

    static bool pop_pending(std::array<InteractionEvent, MaxObjects>& pending,
                            InteractionEvent& event) noexcept {
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (pending[i].phase != InteractionPhase::cancel)
                continue;
            event = pending[i];
            pending[i] = {};
            return true;
        }
        return false;
    }

    static bool remember_pending(std::array<InteractionEvent, MaxObjects>& pending,
                                 std::size_t& count, const InteractionEvent& event) noexcept {
        for (auto& slot : pending) {
            if (slot.phase == InteractionPhase::invalid) {
                slot = event;
                ++count;
                return true;
            }
        }
        return false;
    }

    SpscQueue<InteractionEvent, QueueCapacity> ui_queue_;
    SpscQueue<InteractionEvent, QueueCapacity> audio_queue_;
    std::array<LiveObject, MaxObjects> objects_{};
    std::array<InteractionEvent, MaxObjects> pending_ui_{};
    std::array<InteractionEvent, MaxObjects> pending_audio_{};
    std::size_t pending_ui_count_ = 0;
    std::size_t pending_audio_count_ = 0;
    std::uint32_t source_id_ = 0;
    std::uint32_t epoch_ = 0;
    std::uint64_t last_event_ns_ = 0;
    std::uint64_t source_timeout_ns_ = 0;
    Clock clock_ = nullptr;
    bool has_source_ = false;
};

} // namespace pulp::runtime
