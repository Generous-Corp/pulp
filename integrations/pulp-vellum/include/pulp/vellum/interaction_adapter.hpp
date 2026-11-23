#pragma once

#include <pulp/runtime/interaction_event.hpp>

#include <vellum/view/pointer_dispatch.hpp>
#include <vellum/view/view.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace pulp::vellum {

/// Routes bounded runtime interactions into one Vellum editor root.
///
/// Object IDs are kept as independent pointer sessions. The adapter owns only
/// fixed-capacity storage. Vellum's event type has no
/// fields for hold, velocity, or confidence, so hold is delivered as a drag
/// tick and those values are not represented in the Vellum event.
///
/// deliver() returns true only when at least one event was routed.
class InteractionAdapter final {
  public:
    static constexpr std::size_t kMaxPointers = 64;

    void set_view_metrics(float width, float height, float backing_scale = 1.0f) noexcept {
        view_width_ = width;
        view_height_ = height;
        backing_scale_ = backing_scale;
    }

    template <std::size_t Capacity>
    bool deliver(::vellum::view::View& root,
                 const runtime::InteractionBatch<Capacity>& batch) noexcept {
        static_assert(Capacity <= kMaxPointers);
        const auto events = batch.events_view();
        if (events.empty())
            return false;
        // Events arrive in the tracker's order, so the epoch is followed event
        // by event: a batch may end one epoch (its cancels) and start the next.
        // Another source, and a non-cancel event from an older epoch, are stale.
        // A caller may hand over a batch out of order. Order it by (epoch,
        // sequence) with a stable insertion sort, so events with equal keys,
        // such as a tracker cancel followed by the press that caused it, keep
        // their relative order.
        std::array<runtime::InteractionEvent, Capacity> ordered{};
        std::size_t count = 0;
        for (const auto& event : events) {
            std::size_t at = count;
            while (at > 0 && (ordered[at - 1].epoch > event.epoch ||
                              (ordered[at - 1].epoch == event.epoch &&
                               ordered[at - 1].sequence > event.sequence))) {
                ordered[at] = ordered[at - 1];
                --at;
            }
            ordered[at] = event;
            ++count;
        }
        bool delivered = false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& event = ordered[i];
            if (!has_source_) {
                source_id_ = event.source_id;
                epoch_ = event.epoch;
                has_source_ = true;
            }
            if (event.source_id != source_id_)
                continue;
            if (event.epoch < epoch_ && event.phase != runtime::InteractionPhase::cancel)
                continue;
            if (event.epoch > epoch_)
                epoch_ = event.epoch;
            delivered = route(root, event) || delivered;
        }
        return delivered;
    }

    void reset() noexcept {
        for (auto& pointer : pointers_)
            pointer = {};
        source_id_ = 0;
        epoch_ = 0;
        has_source_ = false;
    }

  private:
    struct Pointer {
        std::uint32_t object_id = 0;
        bool active = false;
    };

    std::size_t find_pointer(std::uint32_t object_id) const noexcept {
        for (std::size_t i = 0; i < pointers_.size(); ++i)
            if (pointers_[i].active && pointers_[i].object_id == object_id)
                return i;
        return pointers_.size();
    }

    std::size_t allocate_pointer(std::uint32_t object_id) noexcept {
        for (std::size_t i = 0; i < pointers_.size(); ++i) {
            if (!pointers_[i].active) {
                pointers_[i] = {object_id, true};
                return i;
            }
        }
        return pointers_.size();
    }

    bool route(::vellum::view::View& root, const runtime::InteractionEvent& event) noexcept {
        using runtime::InteractionPhase;
        const auto existing = find_pointer(event.object_id);
        std::size_t slot = existing;
        if (event.phase == InteractionPhase::press) {
            if (slot == pointers_.size())
                slot = allocate_pointer(event.object_id);
            if (slot == pointers_.size())
                return false;
        } else if (slot == pointers_.size()) {
            return false;
        }

        const auto pointer_id = static_cast<int>(slot);
        const ::vellum::view::Point point{event.x * view_width_ * backing_scale_,
                                          event.y * view_height_ * backing_scale_};
        const ::vellum::view::PointerAttributes attributes{
            .type = ::vellum::view::PointerType::touch, .pressure = 0.5f, .pointer_id = pointer_id};

        switch (event.phase) {
        case InteractionPhase::invalid:
            return false;
        case InteractionPhase::press:
            return ::vellum::view::deliver_mouse_down(
                root, &root, point, 0, 1, true, ::vellum::view::MouseButton::left, {}, attributes);
        case InteractionPhase::hold:
        case InteractionPhase::move:
            ::vellum::view::deliver_mouse_drag(root, &root, point, 0, 1,
                                               ::vellum::view::MouseButton::left, attributes);
            return true;
        case InteractionPhase::release:
            ::vellum::view::deliver_mouse_up(root, &root, point, 0, 1, {},
                                             ::vellum::view::MouseButton::left, attributes);
            pointers_[slot] = {};
            return true;
        case InteractionPhase::cancel:
            ::vellum::view::deliver_mouse_cancel(root, &root, point, 0, 1,
                                                 ::vellum::view::MouseButton::left, attributes);
            pointers_[slot] = {};
            return true;
        }
        return false;
    }

    std::array<Pointer, kMaxPointers> pointers_{};
    std::uint32_t source_id_ = 0;
    std::uint32_t epoch_ = 0;
    bool has_source_ = false;
    float view_width_ = 1.0f;
    float view_height_ = 1.0f;
    float backing_scale_ = 1.0f;
};

} // namespace pulp::vellum
