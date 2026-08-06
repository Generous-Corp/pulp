// widget_bridge/value_channel_binding_service.cpp - frame-tick value-channel delivery.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

#include "bridge_dispatch.hpp"

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace pulp::view {
namespace {

std::string event_binding_key(std::uint32_t id) {
    return "__value_events__" + std::to_string(id);
}

} // namespace

std::size_t WidgetBridge::event_binding_count() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(event_bindings_.begin(), event_bindings_.end(),
                      [](const EventBinding& binding) { return binding.id != 0; }));
}

void WidgetBridge::service_event_bindings() {
    if (event_bindings_.empty()) return;

    struct PendingEventDispatch {
        std::uint32_t id;
        EventFrame frame;
    };
    std::vector<PendingEventDispatch> pending;
    pending.reserve(event_bindings_.size());
    visit_value_channels([&](ValueChannelSet* channels) {
        if (channels == nullptr) return;
        const auto generation_identity = channels->generation_identity();
        const std::size_t count = event_bindings_.size();
        for (std::size_t i = 0; i < count; ++i) {
            auto& binding = event_bindings_[i];
            if (binding.id == 0) continue;
            auto* source = channels->events(binding.channel_name);
            if (source == nullptr) continue;
            const auto frame = source->read();
            const bool replacement =
                binding.value_generation_identity != generation_identity;
            binding.value_generation_identity = generation_identity;
            if (!replacement && frame.publication == binding.last_publication)
                continue;
            binding.last_publication = frame.publication;
            if (std::clamp(frame.count, 0, EventFrame::kMaxEvents) == 0) continue;
            pending.push_back(PendingEventDispatch{binding.id, frame});
        }
    });

    const bool reentrant = in_event_dispatch_;
    in_event_dispatch_ = true;
    for (const auto& item : pending) {
        const auto id = item.id;
        const auto& frame = item.frame;
        const auto still_bound = std::find_if(
            event_bindings_.begin(), event_bindings_.end(),
            [id](const EventBinding& binding) { return binding.id == id; });
        if (still_bound == event_bindings_.end()) continue;
        const int event_count = std::clamp(frame.count, 0, EventFrame::kMaxEvents);

        auto payload = choc::value::createEmptyArray();
        for (int event_index = 0; event_index < event_count; ++event_index) {
            auto occurrence = choc::value::createObject("ValueEvent");
            occurrence.addMember(
                "frameIndex",
                static_cast<std::int64_t>(frame.events[event_index].frame_index));
            occurrence.addMember("value", frame.events[event_index].value);
            payload.addArrayElement(std::move(occurrence));
        }
        safe_dispatch_eval(
            callback_alive_, &engine_,
            "__dispatch__(" + js_string_literal(event_binding_key(id)) +
                ", 'events', " + choc::json::toString(payload, false) + ")",
            "value event binding");
    }
    in_event_dispatch_ = reentrant;
    if (!in_event_dispatch_) {
        std::erase_if(event_bindings_,
                      [](const EventBinding& binding) { return binding.id == 0; });
    }
}

void WidgetBridge::service_param_bindings() {
    if (param_bindings_.empty()) return;

    struct ValueBindingSnapshot {
        MeterFrame meter{};
        VectorFrame vector{};
        std::uint32_t publish_seq = 0;
        std::uint64_t generation_identity = 0;
        float neutral = 0.0f;
        bool found = false;
    };
    // Seed each snapshot from its binding so a temporarily unavailable lease
    // preserves the channel's declared neutral during processor replacement.
    std::vector<ValueBindingSnapshot> snapshots(param_bindings_.size());
    for (std::size_t i = 0; i < param_bindings_.size(); ++i)
        snapshots[i].neutral = param_bindings_[i].neutral;

    // Allocate all snapshot storage before taking the processor-generation
    // lease. The callback performs only bounded lookups and lock-free reads.
    visit_value_channels([&](ValueChannelSet* channels) {
        if (channels == nullptr) return;
        const auto generation_identity = channels->generation_identity();
        for (std::size_t i = 0; i < param_bindings_.size(); ++i) {
            const auto& binding = param_bindings_[i];
            if (binding.value_channel.empty()) continue;
            auto& snapshot = snapshots[i];
            snapshot.generation_identity = generation_identity;
            for (const auto& info : channels->infos()) {
                if (info.name == binding.value_channel) {
                    snapshot.neutral = info.neutral;
                    break;
                }
            }
            if (binding.target == ParamBinding::Target::scope) {
                if (auto* source = channels->vector(binding.value_channel)) {
                    snapshot.publish_seq = source->publish_seq();
                    snapshot.vector = source->read();
                    snapshot.found = true;
                }
            } else if (auto* source = channels->meter(binding.value_channel)) {
                snapshot.publish_seq = source->publish_seq();
                snapshot.meter = source->read();
                snapshot.found = true;
            }
        }
    });

    bool any_changed = false;
    for (std::size_t i = 0; i < param_bindings_.size(); ++i) {
        auto& binding = param_bindings_[i];
        View* widget_view = widget(binding.widget_id);
        if (!widget_view) continue;
        // Precedence: the binding owns the widget's value except while the user
        // is dragging it. Reassert the source on the first frame after release.
        if (widget_view->is_gesture_active()) {
            binding.last_applied = std::numeric_limits<float>::quiet_NaN();
            continue;
        }
        if (binding.value_channel.empty()) {
            if (apply_param_binding(binding, widget_view)) any_changed = true;
            continue;
        }

        auto& snapshot = snapshots[i];
        binding.neutral = snapshot.neutral;
        if (snapshot.generation_identity != 0 &&
            binding.value_generation_identity != snapshot.generation_identity) {
            binding.value_generation_identity = snapshot.generation_identity;
            binding.last_publish_seq = snapshot.publish_seq;
            binding.last_publish_at = std::chrono::steady_clock::now();
        }
        const bool changed = binding.target == ParamBinding::Target::scope
                                 ? apply_scope_binding(
                                       binding, widget_view,
                                       snapshot.found ? &snapshot.vector : nullptr,
                                       snapshot.publish_seq)
                                 : apply_param_binding(
                                       binding, widget_view,
                                       snapshot.found ? &snapshot.meter : nullptr,
                                       snapshot.publish_seq);
        if (changed) any_changed = true;
    }
    if (any_changed) request_repaint();
}

} // namespace pulp::view
