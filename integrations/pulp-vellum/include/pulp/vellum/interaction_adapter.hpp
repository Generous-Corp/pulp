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
/// Object IDs are kept as independent pointer sessions. Events older than the
/// current epoch are ignored, and a newer epoch closes the previous sessions.
/// The adapter owns only fixed-capacity storage. Vellum's event type has no
/// fields for hold, velocity, or confidence, so hold is delivered as a drag
/// tick and those values are not represented in the Vellum event.
///
/// A batch is bound to the source of its first event: events from any other
/// source in the same batch are dropped without routing. deliver() returns true
/// only when at least one event was routed, so a false result can be a
/// legitimately dropped batch (stale epoch, other source, or a pointer that is
/// not active) and is not an error.
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
        std::array<std::size_t, kMaxPointers> order{};
        std::size_t ordered = 0;
        bool delivered = false;
        if (batch.count == 0)
            return false;
        const auto events = batch.events_view();
        if (events.empty())
            return false;
        const auto batch_source = events[0].source_id;
        std::uint32_t batch_epoch = events[0].epoch;
        for (std::size_t i = 1; i < events.size(); ++i) {
            if (events[i].source_id == batch_source && events[i].epoch > batch_epoch)
                batch_epoch = events[i].epoch;
        }
        if (!has_source_ || source_id_ != batch_source || !has_epoch_ || batch_epoch > epoch_) {
            if (has_source_)
                cancel_active(root);
            source_id_ = batch_source;
            has_source_ = true;
            epoch_ = batch_epoch;
            has_epoch_ = true;
            for (auto& pointer : pointers_)
                pointer = {};
        }

        for (std::size_t i = 0; i < events.size(); ++i) {
            const auto& event = events[i];
            if (event.source_id != source_id_ || event.epoch != epoch_)
                continue;
            std::size_t position = ordered;
            while (position > 0 && events[order[position - 1]].sequence > event.sequence) {
                order[position] = order[position - 1];
                --position;
            }
            order[position] = i;
            ++ordered;
        }

        for (std::size_t i = 0; i < ordered; ++i) {
            const auto& event = events[order[i]];
            if (route(root, event))
                delivered = true;
        }
        return delivered;
    }

    void reset() noexcept {
        source_id_ = 0;
        has_source_ = false;
        epoch_ = 0;
        has_epoch_ = false;
        for (auto& pointer : pointers_)
            pointer = {};
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

    void cancel_active(::vellum::view::View& root) noexcept {
        runtime::InteractionEvent cancel{};
        cancel.phase = runtime::InteractionPhase::cancel;
        for (const auto& pointer : pointers_) {
            if (!pointer.active)
                continue;
            cancel.object_id = pointer.object_id;
            (void)route(root, cancel);
        }
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
    bool has_epoch_ = false;
    float view_width_ = 1.0f;
    float view_height_ = 1.0f;
    float backing_scale_ = 1.0f;
};

} // namespace pulp::vellum
