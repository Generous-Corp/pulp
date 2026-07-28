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

#include <pulp/state/param_json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulp::view {

void WidgetBridge::begin_param_gesture(const std::string& widget_id) {
    auto it = param_gesture_routes_.find(widget_id);
    if (it == param_gesture_routes_.end()) return;
    auto& route = it->second;
    if (!route || route->active) return;
    const auto binding = std::find_if(
        param_bindings_.begin(), param_bindings_.end(),
        [&](const ParamBinding& candidate) {
            return candidate.widget_id == widget_id &&
                   candidate.target == ParamBinding::Target::value;
        });
    if (binding == param_bindings_.end()) return;
    route->active_param_id = binding->param_id;
    route->active = true;
    try {
        store_.acquire_gesture(route->active_param_id);
    } catch (...) {
        // StateStore records the lease and open gesture before invoking its
        // host callback. Preserve the route so release still balances it,
        // but never unwind through the widget's input dispatch.
    }
}

void WidgetBridge::end_param_gesture(const std::string& widget_id) {
    auto it = param_gesture_routes_.find(widget_id);
    if (it == param_gesture_routes_.end()) return;
    try {
        finish_param_gesture_route(it->second);
    } catch (...) {
        // StateStore removes the open gesture before invoking its host
        // callback. Keep input dispatch noexcept after that external call.
    }
}

void WidgetBridge::wire_parameter_gestures(const std::string& widget_id,
                                           View* widget) {
    if (!widget) return;

    release_param_gesture_route(widget_id);
    param_gesture_routes_[widget_id] = std::make_shared<ParamGestureRoute>();
    auto alive = callback_alive_;
    auto begin = [this, alive, widget_id] {
        if (!alive || !alive->load(std::memory_order_acquire)) return;
        begin_param_gesture(widget_id);
    };
    auto end = [this, alive, widget_id] {
        if (!alive || !alive->load(std::memory_order_acquire)) return;
        end_param_gesture(widget_id);
    };
    if (auto* knob = dynamic_cast<Knob*>(widget)) {
        knob->on_gesture_begin = begin;
        knob->on_gesture_end = end;
    } else if (auto* fader = dynamic_cast<Fader*>(widget)) {
        fader->on_gesture_begin = begin;
        fader->on_gesture_end = end;
    } else if (auto* slider = dynamic_cast<RangeSlider*>(widget)) {
        slider->on_gesture_begin = begin;
        slider->on_gesture_end = end;
    }
}

void WidgetBridge::finish_param_gesture_route(
    const std::shared_ptr<ParamGestureRoute>& route) {
    if (!route || !route->active) return;
    const state::ParamID id = route->active_param_id;
    route->active = false;
    store_.release_gesture(id);
}

void WidgetBridge::release_param_gesture_route(
    const std::string& widget_id) noexcept {
    auto route = param_gesture_routes_.find(widget_id);
    if (route == param_gesture_routes_.end()) return;
    auto detached = std::move(route->second);
    param_gesture_routes_.erase(route);
    try {
        finish_param_gesture_route(detached);
    } catch (...) {
        // StateStore removes the open gesture before invoking the host callback.
        // Teardown must remain noexcept even if that external callback throws.
    }
}

void WidgetBridge::release_all_param_gesture_routes() noexcept {
    auto routes = std::move(param_gesture_routes_);
    param_gesture_routes_.clear();

    for (auto& [widget_id, route] : routes) {
        (void)widget_id;
        if (!route->active) continue;
        const state::ParamID id = route->active_param_id;
        route->active = false;
        try {
            store_.release_gesture(id);
        } catch (...) {
            // The StateStore has already removed the open gesture. Continue
            // releasing the remaining routes and preserve noexcept teardown.
        }
    }
}

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

const char* describe(BindingOutcome o) noexcept {
    switch (o) {
        case BindingOutcome::ok: return "bound";
        case BindingOutcome::deferred_widget_missing:
            return "bound; widget not created yet";
        case BindingOutcome::replaced_prior_binding:
            return "bound, replacing this widget's previous binding";
        case BindingOutcome::empty_widget_id: return "widget id was empty";
        case BindingOutcome::empty_param_name: return "parameter name was empty";
        case BindingOutcome::null_widget: return "widget id maps to a null view";
        case BindingOutcome::incompatible_widget:
            return "widget type cannot accept this binding target";
        case BindingOutcome::unknown_param:
            return "no parameter with that name is declared";
    }
    return "unknown outcome";
}

bool WidgetBridge::record_binding_attempt(const std::string& widget_id,
                                          const std::string& param_name,
                                          BindingTarget target,
                                          BindingOutcome outcome) {
    binding_attempts_.push_back(BindingAttempt{widget_id, param_name, target, outcome});
    return is_bound(outcome);
}

std::vector<std::string> WidgetBridge::unbound_params() const {
    std::unordered_set<std::string> reached;
    for (const auto& attempt : binding_attempts_) {
        if (is_bound(attempt.outcome)) reached.insert(attempt.param_name);
    }

    std::vector<std::string> out;
    for (std::size_t i = 0; i < store_.param_count(); ++i) {
        const auto* info = &store_.all_params()[i];
        if (!info) continue;
        // The host surfaces these itself; omitting them from a plugin's own UI
        // is a choice, not a hole.
        if (info->designation != state::ParamDesignation::None) continue;
        if (info->is_trigger) continue;
        if (reached.count(info->name) == 0) out.push_back(info->name);
    }
    return out;
}

bool WidgetBridge::add_param_binding(const std::string& widget_id,
                                     const std::string& param_name,
                                     ParamBinding::Target target,
                                     const choc::value::Value* transform) {
    // Every early return below records why, because the bool these functions
    // return is dropped on the floor by every generated UI script.
    const auto fail = [&](BindingOutcome o) {
        return record_binding_attempt(widget_id, param_name, target, o);
    };

    if (widget_id.empty()) return fail(BindingOutcome::empty_widget_id);
    if (param_name.empty()) return fail(BindingOutcome::empty_param_name);
    const auto widget = widgets_.find(widget_id);
    const bool widget_present = widget != widgets_.end();
    if (widget_present) {
        if (widget->second == nullptr) return fail(BindingOutcome::null_widget);

        // Record only bindings the native per-frame service can actually
        // apply. A custom Canvas may still use getParam/setParam from its own
        // pointer handlers, but counting it here would claim a declarative
        // control exists while apply_param_binding() silently has no
        // compatible value surface. A missing widget remains valid because
        // scripts may intentionally register bindings before creating views.
        View* const view = widget->second;
        const bool supported =
            target == ParamBinding::Target::meter
                ? dynamic_cast<Meter*>(view) != nullptr
                : dynamic_cast<Knob*>(view) != nullptr ||
                      dynamic_cast<Fader*>(view) != nullptr ||
                      dynamic_cast<RangeSlider*>(view) != nullptr ||
                      dynamic_cast<Toggle*>(view) != nullptr ||
                      dynamic_cast<ProgressBar*>(view) != nullptr;
        if (!supported) return fail(BindingOutcome::incompatible_widget);
    }

    state::ParamID id = 0;
    if (!resolve_param_id(param_name, id)) return fail(BindingOutcome::unknown_param);

    ParamBinding binding;
    binding.widget_id = widget_id;
    binding.param_id = id;
    binding.target = target;
    binding.transform = parse_transform(transform);

    // Re-binding a widget replaces the prior binding (a widget has one source).
    for (auto& existing : param_bindings_) {
        if (existing.widget_id == widget_id) {
            existing = std::move(binding);
            return record_binding_attempt(widget_id, param_name, target,
                                          BindingOutcome::replaced_prior_binding);
        }
    }
    param_bindings_.push_back(std::move(binding));
    return record_binding_attempt(
        widget_id, param_name, target,
        widget_present ? BindingOutcome::ok : BindingOutcome::deferred_widget_missing);
}

void BridgeRegistrars::register_state_binding_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // getParam(name) -> get parameter value from store (normalized)
    // getParamMetadata(name) -> the parameter's STATIC shape, or undefined.
    //
    // This is what a UI needs to build a correct control: real min/max, the
    // unit, the default, the curve. Without it an author retypes all of that in
    // JavaScript and the two drift. Serialized by pulp::state::param_json — the
    // same code the inspector uses, so the two payloads cannot disagree.
    // Static only, so a caller can fetch once and cache; live value stays on
    // getParam.
    register_bridge_function(api, "getParamMetadata", [&self](choc::javascript::ArgumentList args) {
        const auto name = args.get<std::string>(0, "");
        for (std::size_t i = 0; i < self.store_.param_count(); ++i) {
            const auto* info = &self.store_.all_params()[i];
            if (info && info->name == name) return state::param_metadata_to_value(*info);
        }
        return choc::value::Value();  // undefined in JS — an unknown name is not an empty object
    });

    // formatParamValue(name, value, isNormalized?) -> display string.
    // Honours the author's to_string, then an enum label, then unit-suffixed
    // numeric — the same order the host and the inspector use, so a UI never
    // shows a different string than the DAW.
    register_bridge_function(api, "formatParamValue", [&self](choc::javascript::ArgumentList args) {
        const auto name = args.get<std::string>(0, "");
        const auto raw = static_cast<float>(args.get<double>(1, 0.0));
        const bool normalized = args.get<bool>(2, false);
        for (std::size_t i = 0; i < self.store_.param_count(); ++i) {
            const auto* info = &self.store_.all_params()[i];
            if (!info || info->name != name) continue;
            const float value = normalized ? info->range.denormalize(raw) : raw;
            return choc::value::createString(state::param_display_text(*info, value));
        }
        return choc::value::Value();
    });

    // parseParamValue(name, text) -> {ok, value, normalized} or undefined.
    //
    // `ok` is the point: a caller must be able to tell "banana" from a genuine
    // zero. Returning a bare number would let a click-to-type field store a
    // value the user never entered.
    register_bridge_function(api, "parseParamValue", [&self](choc::javascript::ArgumentList args) {
        const auto name = args.get<std::string>(0, "");
        const auto text = args.get<std::string>(1, "");
        for (std::size_t i = 0; i < self.store_.param_count(); ++i) {
            const auto* info = &self.store_.all_params()[i];
            if (!info || info->name != name) continue;
            float parsed = 0.0f;
            const bool ok = state::param_parse_display_text(*info, text, parsed);
            auto out = choc::value::createObject("");
            out.addMember("ok", choc::value::createBool(ok));
            out.addMember("value", choc::value::createFloat64(ok ? parsed : 0.0f));
            out.addMember("normalized",
                          choc::value::createFloat64(ok ? info->range.normalize(parsed) : 0.0f));
            return out;
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "getParam", [&self](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");

        for (size_t i = 0; i < self.store_.param_count(); ++i) {
            auto* info = &self.store_.all_params()[i];
            if (info && info->name == name) {
                return choc::value::createFloat64(self.store_.get_normalized(info->id));
            }
        }
        return choc::value::createFloat64(0);
    });

    // setParam(name, normalized_value) -> set parameter in store
    register_bridge_function(api, "setParam", [&self](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto value = args.get<double>(1, 0);

        for (size_t i = 0; i < self.store_.param_count(); ++i) {
            auto* info = &self.store_.all_params()[i];
            if (info && info->name == name) {
                self.store_.set_normalized(info->id, static_cast<float>(value));
                break;
            }
        }
        return choc::value::Value();
    });

    // bindWidgetToParam(widgetId, paramName, transform?) -> bind a value widget
    // (knob / fader / slider / toggle / progress) to a param. Registered once;
    // C++ then pushes the transformed store value every frame with no per-frame
    // JS crossing. Gesture-capable controls also forward their native begin/end
    // lifecycle to StateStore; a JS change handler can therefore write values
    // without losing host automation touch/release or undo grouping. Returns
    // true if the param exists and the binding was set.
    register_bridge_function(api, "bindWidgetToParam", [&self](choc::javascript::ArgumentList args) {
        return choc::value::createBool(
            self.add_param_binding(args.get<std::string>(0, ""),
                              args.get<std::string>(1, ""),
                              WidgetBridge::ParamBinding::Target::value,
                              args.numArgs > 2 ? args[2] : nullptr));
    });

    // bindMeter(widgetId, source, transform?) -> bind a Meter widget to a param
    // (the source drives both the rms and peak fill). Same native-push contract.
    register_bridge_function(api, "bindMeter", [&self](choc::javascript::ArgumentList args) {
        return choc::value::createBool(
            self.add_param_binding(args.get<std::string>(0, ""),
                              args.get<std::string>(1, ""),
                              WidgetBridge::ParamBinding::Target::meter,
                              args.numArgs > 2 ? args[2] : nullptr));
    });

    // unbindWidget(widgetId) -> remove any binding(s) for that widget. Returns
    // the number of bindings removed.
    register_bridge_function(api, "unbindWidget", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        const auto before = self.param_bindings_.size();
        self.param_bindings_.erase(
            std::remove_if(self.param_bindings_.begin(), self.param_bindings_.end(),
                           [&](const WidgetBridge::ParamBinding& b) { return b.widget_id == id; }),
            self.param_bindings_.end());
        return choc::value::createInt64(
            static_cast<int64_t>(before - self.param_bindings_.size()));
    });
}

} // namespace pulp::view
