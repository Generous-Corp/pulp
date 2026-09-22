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

constexpr auto kShaderValueChannelStaleAfter = std::chrono::milliseconds(250);

std::string event_binding_key(std::uint32_t id) {
    return "__value_events__" + std::to_string(id);
}

} // namespace

void WidgetBridge::service_shader_value_bindings() {
    bool any_changed = false;
    for (const auto& [id, state] : widgets_) {
        auto* view = state.view;
        auto* host = dynamic_cast<CustomShaderHost*>(view);
        if (!host || (host->shader_value_bindings().empty() &&
                      !host->shader_scope_binding().has_value()))
            continue;
        for (auto& binding : host->shader_value_bindings()) {
            if (binding.managed_by_param_binding) continue;
            std::uint32_t publish_seq = 0;
            float value = binding.neutral;
            bool found = false;
            visit_value_channels([&](ValueChannelSet* channels) {
                if (!channels) return;
                if (auto* scalar = channels->scalar(binding.channel_name)) {
                    publish_seq = scalar->publish_seq();
                    value = scalar->read();
                    found = true;
                } else if (auto* meter = channels->meter(binding.channel_name)) {
                    publish_seq = meter->publish_seq();
                    value = meter->read().rms[0];
                    found = true;
                }
                for (const auto& info : channels->infos()) {
                    if (info.name == binding.channel_name) {
                        binding.neutral = info.neutral;
                        break;
                    }
                }
            });
            if (binding.channel_name.empty() && !binding.param_name.empty()) {
                state::ParamID param_id = 0;
                if (resolve_param_id(binding.param_name, param_id)) {
                    value = store_.get_normalized(param_id);
                    found = true;
                    // Parameters are host-published state, so they do not
                    // use channel staleness decay.
                    binding.last_publish_at = std::chrono::steady_clock::now();
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (binding.last_publish_at.time_since_epoch().count() == 0)
                binding.last_publish_at = now;
            if (publish_seq != binding.last_publish_seq) {
                binding.last_publish_seq = publish_seq;
                binding.last_publish_at = now;
            }
            if (!found || now - binding.last_publish_at > kShaderValueChannelStaleAfter)
                value = binding.neutral;
            if (host->set_shader_uniform_value(binding.uniform_name, value))
                any_changed = true;
        }
        if (auto& scope = host->shader_scope_binding()) {
            std::uint32_t publish_seq = 0;
            VectorFrame frame{};
            bool found = false;
            visit_value_channels([&](ValueChannelSet* channels) {
                if (!channels) return;
                if (auto* source = channels->vector(scope->channel_name)) {
                    publish_seq = source->publish_seq();
                    frame = source->read();
                    found = true;
                }
                for (const auto& info : channels->infos()) {
                    if (info.name == scope->channel_name) {
                        scope->neutral = info.neutral;
                        break;
                    }
                }
            });
            const auto now = std::chrono::steady_clock::now();
            if (scope->last_publish_at.time_since_epoch().count() == 0)
                scope->last_publish_at = now;
            const bool published = publish_seq != scope->last_publish_seq;
            if (published) {
                scope->last_publish_seq = publish_seq;
                scope->last_publish_at = now;
            }
            const bool live = found && now - scope->last_publish_at <= kShaderValueChannelStaleAfter;
            // A neutral/stale scope still uploads exactly one texel. Zero-sized
            // textures are rejected by several GPU backends and make a shader's
            // sampling contract backend-dependent.
            const bool liveness_changed = !scope->data || scope->data->live != live;
            if (published || liveness_changed) {
                auto data = std::make_shared<canvas::Canvas::ShaderDataTexture>();
                data->name = scope->channel_name;
                const int count = live ? std::clamp(frame.count, 0, VectorFrame::kMaxSamples) : 0;
                data->samples.resize(static_cast<std::size_t>(std::max(1, count)), scope->neutral);
                if (count > 0)
                    std::copy_n(frame.samples.data(), count, data->samples.data());
                // Keep the texture contract explicit: even a stale/empty
                // scope has one neutral texel, so shaders can sample safely.
                // `_count` describes the uploaded payload, not the source's
                // live sample count; stale therefore reports one neutral
                // sample together with `live = 0`.
                data->count = static_cast<std::uint32_t>(std::max(1, count));
                data->live = live;
                data->neutral = scope->neutral;
                data->publish_sequence = publish_seq;
                scope->data = std::move(data);
                any_changed = true;
            }
        }
    }
    if (any_changed) request_repaint();
}

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
    // Shader channel bindings share this frame-tick service even when there
    // are no ordinary widget parameter bindings.
    if (param_bindings_.empty()) {
        service_shader_value_bindings();
        return;
    }

    struct ValueBindingSnapshot {
        float scalar = 0.0f;
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
            } else if (binding.target == ParamBinding::Target::uniform) {
                if (auto* source = channels->scalar(binding.value_channel)) {
                    snapshot.publish_seq = source->publish_seq();
                    snapshot.scalar = source->read();
                    snapshot.found = true;
                } else if (auto* source = channels->meter(binding.value_channel)) {
                    snapshot.publish_seq = source->publish_seq();
                    snapshot.scalar = source->read().rms[0];
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
        if (binding.target == ParamBinding::Target::uniform) {
            auto* host = widget_view ? dynamic_cast<CustomShaderHost*>(widget_view) : nullptr;
            if (!host || binding.uniform_name.empty()) continue;
            auto metadata = std::find_if(
                host->shader_value_bindings().begin(),
                host->shader_value_bindings().end(),
                [&](const auto& item) { return item.uniform_name == binding.uniform_name &&
                                                item.managed_by_param_binding; });
            if (metadata != host->shader_value_bindings().end() &&
                metadata->last_publish_at.time_since_epoch().count() != 0)
                binding.last_publish_at = metadata->last_publish_at;
            float value = binding.neutral;
            if (binding.value_channel.empty()) {
                value = store_.get_normalized(binding.param_id);
            } else {
                auto& snapshot = snapshots[i];
                binding.neutral = snapshot.neutral;
                if (snapshot.generation_identity != 0 &&
                    binding.value_generation_identity != snapshot.generation_identity) {
                    binding.value_generation_identity = snapshot.generation_identity;
                    binding.last_publish_seq = snapshot.publish_seq;
                    binding.last_publish_at = std::chrono::steady_clock::now();
                }
                if (snapshot.found && !WidgetBridge::value_channel_is_stale(binding, snapshot.publish_seq))
                    value = snapshot.scalar;
            }
            value = binding.transform.apply(value);
            if (host->set_shader_uniform_value(binding.uniform_name, value)) any_changed = true;
            if (metadata != host->shader_value_bindings().end()) {
                metadata->last_publish_at = binding.last_publish_at;
                metadata->last_publish_seq = binding.last_publish_seq;
                metadata->neutral = binding.neutral;
            }
            continue;
        }
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
    service_shader_value_bindings();
}

} // namespace pulp::view
