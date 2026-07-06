// widget_bridge/state_binding_api.cpp - parameter state registrations for WidgetBridge.
//
// Two families live here:
//   * Imperative param access — getParam / setParam (pull-based; JS reads/writes
//     the atomic param store on demand).
//   * Declarative native→widget bindings — bindWidgetToParam / bindMeter /
//     unbindWidget. Registered ONCE from JS, after which C++ pushes the store
//     value onto the widget every frame off the host FrameClock with zero
//     per-frame JS crossing (service_param_bindings, below). This is the native
//     replacement for a requestAnimationFrame metering loop.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include "api_registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace pulp::view {

// ── Transform mini-spec ──────────────────────────────────────────────────
// Applied to the source before it reaches the widget, in order:
//   optional dB→linear map → scale → offset → optional clamp.
float WidgetBridge::BindingTransform::apply(float v) const {
    float out = v;
    if (db) {
        const float span = db_max - db_min;
        out = span != 0.0f ? (v - db_min) / span : 0.0f;
    }
    out = out * scale + offset;
    if (clamp) out = std::clamp(out, clamp_min, clamp_max);
    return out;
}

// Parse the optional `{db, dbMin, dbMax, scale, offset, min, max, clamp}`
// transform object handed to bindWidgetToParam / bindMeter. Absent OR `{}`
// both yield the default: identity scale/offset with a [0,1] clamp (the value
// domain every binding target shares — Knob/Fader/Meter are normalized and a
// RangeSlider treats the result as a fraction of its own range). Provide `min`
// / `max` to widen or narrow the clamp, or `clamp:false` to disable it.
WidgetBridge::BindingTransform WidgetBridge::parse_transform(const choc::value::Value* v) {
    BindingTransform t;
    if (!v || !v->isObject()) return t;
    const auto& o = *v;
    t.db = o["db"].getWithDefault<bool>(false);
    t.db_min = static_cast<float>(o["dbMin"].getWithDefault<double>(t.db_min));
    t.db_max = static_cast<float>(o["dbMax"].getWithDefault<double>(t.db_max));
    t.scale = static_cast<float>(o["scale"].getWithDefault<double>(t.scale));
    t.offset = static_cast<float>(o["offset"].getWithDefault<double>(t.offset));
    t.clamp = o["clamp"].getWithDefault<bool>(t.clamp);
    t.clamp_min = static_cast<float>(o["min"].getWithDefault<double>(t.clamp_min));
    t.clamp_max = static_cast<float>(o["max"].getWithDefault<double>(t.clamp_max));
    return t;
}

bool WidgetBridge::resolve_param_id(const std::string& name, state::ParamID& out) const {
    for (std::size_t i = 0; i < store_.param_count(); ++i) {
        const auto* info = &store_.all_params()[i];
        if (info && info->name == name) {
            out = info->id;
            return true;
        }
    }
    return false;
}

bool WidgetBridge::apply_param_binding(ParamBinding& b, View* w) {
    // dB transforms operate on the raw param value; everything else on the
    // store's already-normalized [0,1] value (the 1:1 common case).
    const float src = b.transform.db ? store_.get_value(b.param_id)
                                     : store_.get_normalized(b.param_id);
    const float target = b.transform.apply(src);
    if (std::isnan(target)) return false;  // never churn on a NaN source

    const bool changed = std::isnan(b.last_applied) || target != b.last_applied;

    if (b.target == ParamBinding::Target::meter) {
        // Meter::set_level repaints unconditionally — push ONLY on change so a
        // static source doesn't spin the paint loop every vsync.
        if (!changed) return false;
        b.last_applied = target;
        if (auto* m = dynamic_cast<Meter*>(w)) m->set_level(target, target);
        return true;
    }

    // Value widgets: re-assert the store value EVERY frame so the binding
    // strictly owns it (a stray setValue is corrected next frame). Their own
    // set_value is a guarded no-op that self-repaints only on a real change, so
    // this is cheap on a static source. Track whether a widget type actually
    // matched so an unbindable target neither claims a repaint nor churns.
    b.last_applied = target;
    bool matched = true;
    if (auto* k = dynamic_cast<Knob*>(w)) {
        k->set_value(target);
    } else if (auto* f = dynamic_cast<Fader*>(w)) {
        f->set_value(target);
    } else if (auto* r = dynamic_cast<RangeSlider*>(w)) {
        // RangeSlider works in real units [min,max]; treat the transformed
        // [0,1] value as a fraction of its own range so a non-normalized slider
        // (e.g. 20 Hz..20 kHz) tracks the param across its full travel.
        const float lo = r->min_value();
        const float hi = r->max_value();
        r->set_value(lo + std::clamp(target, 0.0f, 1.0f) * (hi - lo));
    } else if (auto* t = dynamic_cast<Toggle*>(w)) {
        t->set_on(target > 0.5f);
    } else if (auto* p = dynamic_cast<ProgressBar*>(w)) {
        // ProgressBar::set_progress does NOT self-repaint; the caller schedules
        // one when `changed`.
        p->set_progress(target);
    } else {
        matched = false;
    }
    return matched && changed;
}

void WidgetBridge::prune_dangling_bindings() {
    if (param_bindings_.empty()) return;
    param_bindings_.erase(
        std::remove_if(param_bindings_.begin(), param_bindings_.end(),
                       [this](const ParamBinding& b) {
                           return widgets_.find(b.widget_id) == widgets_.end();
                       }),
        param_bindings_.end());
}

void WidgetBridge::service_param_bindings() {
    if (param_bindings_.empty()) return;
    bool any_changed = false;
    for (auto& b : param_bindings_) {
        View* w = widget(b.widget_id);
        if (!w) continue;
        // Precedence: the binding owns the widget's value EXCEPT while the user
        // is dragging it — then the gesture wins. Invalidate last_applied so the
        // store value re-asserts on the first frame after the drag ends.
        if (w->is_gesture_active()) {
            b.last_applied = std::numeric_limits<float>::quiet_NaN();
            continue;
        }
        if (apply_param_binding(b, w)) any_changed = true;
    }
    if (any_changed) request_repaint();
}

bool WidgetBridge::add_param_binding(const std::string& widget_id,
                                     const std::string& param_name,
                                     ParamBinding::Target target,
                                     const choc::value::Value* transform) {
    if (widget_id.empty() || param_name.empty()) return false;
    state::ParamID id = 0;
    if (!resolve_param_id(param_name, id)) return false;

    ParamBinding binding;
    binding.widget_id = widget_id;
    binding.param_id = id;
    binding.target = target;
    binding.transform = parse_transform(transform);

    // Re-binding a widget replaces the prior binding (a widget has one source).
    for (auto& existing : param_bindings_) {
        if (existing.widget_id == widget_id) {
            existing = std::move(binding);
            return true;
        }
    }
    param_bindings_.push_back(std::move(binding));
    return true;
}

void WidgetBridge::register_state_binding_api() {
    BridgeApiContext api{engine_};

    // getParam(name) -> get parameter value from store (normalized)
    register_bridge_function(api, "getParam", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");

        for (size_t i = 0; i < store_.param_count(); ++i) {
            auto* info = &store_.all_params()[i];
            if (info && info->name == name) {
                return choc::value::createFloat64(store_.get_normalized(info->id));
            }
        }
        return choc::value::createFloat64(0);
    });

    // setParam(name, normalized_value) -> set parameter in store
    register_bridge_function(api, "setParam", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto value = args.get<double>(1, 0);

        for (size_t i = 0; i < store_.param_count(); ++i) {
            auto* info = &store_.all_params()[i];
            if (info && info->name == name) {
                store_.set_normalized(info->id, static_cast<float>(value));
                break;
            }
        }
        return choc::value::Value();
    });

    // bindWidgetToParam(widgetId, paramName, transform?) -> bind a value widget
    // (knob / fader / slider / toggle / progress) to a param. Registered once;
    // C++ then pushes the transformed store value every frame with no per-frame
    // JS crossing. Returns true if the param exists and the binding was set.
    register_bridge_function(api, "bindWidgetToParam", [this](choc::javascript::ArgumentList args) {
        return choc::value::createBool(
            add_param_binding(args.get<std::string>(0, ""),
                              args.get<std::string>(1, ""),
                              ParamBinding::Target::value,
                              args.numArgs > 2 ? args[2] : nullptr));
    });

    // bindMeter(widgetId, source, transform?) -> bind a Meter widget to a param
    // (the source drives both the rms and peak fill). Same native-push contract.
    register_bridge_function(api, "bindMeter", [this](choc::javascript::ArgumentList args) {
        return choc::value::createBool(
            add_param_binding(args.get<std::string>(0, ""),
                              args.get<std::string>(1, ""),
                              ParamBinding::Target::meter,
                              args.numArgs > 2 ? args[2] : nullptr));
    });

    // unbindWidget(widgetId) -> remove any binding(s) for that widget. Returns
    // the number of bindings removed.
    register_bridge_function(api, "unbindWidget", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        const auto before = param_bindings_.size();
        param_bindings_.erase(
            std::remove_if(param_bindings_.begin(), param_bindings_.end(),
                           [&](const ParamBinding& b) { return b.widget_id == id; }),
            param_bindings_.end());
        return choc::value::createInt64(
            static_cast<int64_t>(before - param_bindings_.size()));
    });
}

} // namespace pulp::view
