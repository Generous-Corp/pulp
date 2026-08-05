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
#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/value_channel_json.hpp>
#include <pulp/view/value_channel_set.hpp>
#include "api_registry.hpp"
#include "bridge_dispatch.hpp"

#include <pulp/state/param_json.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
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
        BridgeCallbackScope scope(alive);
        if (!alive || !alive->load(std::memory_order_acquire)) return;
        begin_param_gesture(widget_id);
    };
    auto end = [this, alive, widget_id] {
        BridgeCallbackScope scope(alive);
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

void WidgetBridge::defer_all_param_gesture_routes() {
    for (auto& [widget_id, route] : param_gesture_routes_) {
        (void)widget_id;
        if (!route->active) continue;
        store_.defer_gesture_release(route->active_param_id);
        route->active = false;
    }
    param_gesture_routes_.clear();
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

bool WidgetBridge::transform_requests_derivation(const choc::value::Value* v) {
    if (!v || !v->isObject()) return false;
    const auto& o = *v;
    if (!o["fromParam"].getWithDefault<bool>(false)) return false;
    // `fromParam` and an explicit db-family key are mutually exclusive: naming
    // any of db/dbMin/dbMax means the author is describing the mapping
    // themselves, and silently replacing what they wrote would be worse than
    // ignoring the flag. (dbMin/dbMax without `db: true` is inert in this API —
    // the mapping stays off — which is pre-existing behaviour, not a new trap.)
    return !(o.hasObjectMember("db") || o.hasObjectMember("dbMin") ||
             o.hasObjectMember("dbMax"));
}

// Parse the optional `{db, dbMin, dbMax, scale, offset, min, max, clamp}`
// transform object handed to bindWidgetToParam / bindMeter. Absent OR `{}`
// both yield the default: identity scale/offset with a [0,1] clamp (the value
// domain every binding target shares — Knob/Fader/Meter are normalized and a
// RangeSlider treats the result as a fraction of its own range). Provide `min`
// / `max` to widen or narrow the clamp, or `clamp:false` to disable it.
// `{fromParam: true}` fills db/dbMin/dbMax from the parameter instead — see
// transform_requests_derivation above and derive_binding_transform below.
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

namespace {
/// Returns the channel name for a `value:<name>` source, or empty for a plain
/// parameter name.
std::string_view value_channel_name(std::string_view source) {
    constexpr std::string_view kPrefix = "value:";
    if (source.size() <= kPrefix.size() || source.substr(0, kPrefix.size()) != kPrefix)
        return {};
    return source.substr(kPrefix.size());
}
} // namespace

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

void WidgetBridge::derive_binding_transform(ParamBinding& b, View* w) {
    b.derive_from_param = false;  // one shot, whatever we conclude below
    const auto* info = store_.info(b.param_id);
    if (info == nullptr) return;

    // Only widgets that present the param in REAL units need a derived
    // transform. A Knob/Fader/Toggle/ProgressBar is a [0,1] surface, and the
    // store's normalized value is already the right thing to show there — it is
    // the position the host's automation lane shows. Deriving for those would
    // move the control off the host's curve, so `fromParam` deliberately
    // leaves them alone.
    const bool real_units = b.target == ParamBinding::Target::meter ||
                            dynamic_cast<RangeSlider*>(w) != nullptr;
    if (!real_units) return;

    // Map the real value linearly across the param's own range. For a
    // RangeSlider this is the fix for skew: the normalized value is curved, so
    // treating it as a fraction of the slider's range put a 1 kHz cutoff at
    // ~10 kHz on a 20 Hz..20 kHz range. For a dB meter it replaces hand-copied
    // dbMin/dbMax with the range the parameter already declares.
    b.transform.db = true;
    b.transform.db_min = info->range.min;
    b.transform.db_max = info->range.max;
}


/// How long a value channel may go without a publish before a bound view falls
/// back to its neutral. Pulp meters already carry UI-side ballistics, so this
/// only has to outlast a normal render gap, not smooth anything.
constexpr auto kValueChannelStaleAfter = std::chrono::milliseconds(250);

bool WidgetBridge::value_channel_is_stale(ParamBinding& b, std::uint32_t seq) {
    const auto now = std::chrono::steady_clock::now();
    if (seq != b.last_publish_seq) {
        b.last_publish_seq = seq;
        b.last_publish_at = now;
        return false;
    }
    return now - b.last_publish_at > kValueChannelStaleAfter;
}

bool WidgetBridge::apply_scope_binding(ParamBinding& b, View* w,
                                       const VectorFrame* value_frame,
                                       std::uint32_t publish_seq) {
    const bool stale = value_frame == nullptr ||
                       value_channel_is_stale(b, publish_seq);
    const int count = stale ? 0 : value_frame->count;
    static constexpr std::array<float, 1> kEmptyFrame{};
    const float* samples = value_frame ? value_frame->samples.data()
                                       : kEmptyFrame.data();
    // A stale scope renders empty rather than holding the last block on screen,
    // which would read as a frozen display of live audio.
    if (auto* spectrum = dynamic_cast<SpectrumView*>(w)) {
        spectrum->set_spectrum(samples, static_cast<size_t>(count));
        return true;
    }
    if (auto* wave = dynamic_cast<WaveformView*>(w)) {
        wave->set_data(samples, static_cast<size_t>(count));
        return true;
    }
    return false;
}

bool WidgetBridge::apply_param_binding(ParamBinding& b, View* w,
                                       const MeterFrame* value_frame,
                                       std::uint32_t publish_seq) {
    if (b.derive_from_param) derive_binding_transform(b, w);

    // A scope pushes a whole block, so it shares none of the scalar path below.
    if (b.target == ParamBinding::Target::scope) return false;

    // Staleness: a value channel that has stopped PUBLISHING decays to its
    // declared neutral, so a meter drops to rest when audio stops instead of
    // freezing on its last reading. Watching the publish sequence rather than
    // the value is what lets a genuinely static-but-live signal keep reading.
    if (!b.value_channel.empty() &&
        (value_frame == nullptr || value_channel_is_stale(b, publish_seq))) {
        if (b.last_applied == b.neutral) return false;
        b.last_applied = b.neutral;
        if (auto* m = dynamic_cast<Meter*>(w)) m->set_level(b.neutral, b.neutral);
        return true;
    }
    // A value channel already publishes in the domain the widget wants, so the
    // transform applies to it directly — there is no normalized/real split to
    // choose between as there is for a parameter.
    const float src = !b.value_channel.empty()
                          ? value_frame->rms[0]
                          : (b.transform.db ? store_.get_value(b.param_id)
                                            : store_.get_normalized(b.param_id));
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
    } else if (auto* seg = dynamic_cast<SegmentedControl*>(w)) {
        seg->set_selected_silent(selector_segment_index(
            target, static_cast<int>(seg->segments().size())));
    } else if (auto* st = dynamic_cast<Stepper*>(w)) {
        st->set_value(stepper_plain_value(target, st->minimum(), st->maximum(),
                                          st->step()));
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

namespace {
// Subscriptions are not widgets, but they reuse __callbacks__/__dispatch__ so
// they inherit its handler-exception containment for free. The '__param__'
// prefix cannot collide with a widget id: ids come from the author's markup,
// and a leading double underscore is reserved by the preamble.
std::string param_subscription_key(std::uint32_t id) {
    return "__param__" + std::to_string(id);
}
} // namespace

std::size_t WidgetBridge::param_subscription_count() const noexcept {
    // Tombstoned entries (id == 0) are logically gone the moment a handler
    // unsubscribes; they only survive until the pass compacts them.
    return static_cast<std::size_t>(
        std::count_if(param_subscriptions_.begin(), param_subscriptions_.end(),
                      [](const ParamSubscription& s) { return s.id != 0; }));
}


void WidgetBridge::service_param_subscriptions() {
    if (param_subscriptions_.empty()) return;

    // A handler is free to subscribe or unsubscribe while it runs — "unsubscribe
    // after the first change" is an ordinary thing to write. Both mutate
    // param_subscriptions_, so this loop must survive the vector growing,
    // reallocating, or having an element cancelled underneath it:
    //   * index-based, bounded by the size at entry, so subscriptions added by
    //     a handler are picked up next frame rather than during this pass;
    //   * no reference to an element is held across the dispatch, because a
    //     push_back can reallocate the buffer;
    //   * cancellation during dispatch tombstones (id = 0) instead of erasing,
    //     and the compaction below is the only place elements are removed.
    const bool reentrant = in_param_dispatch_;
    in_param_dispatch_ = true;
    const std::size_t count = param_subscriptions_.size();
    for (std::size_t i = 0; i < count && i < param_subscriptions_.size(); ++i) {
        const auto id = param_subscriptions_[i].id;
        if (id == 0) continue;  // cancelled by an earlier handler this pass
        const auto param_id = param_subscriptions_[i].param_id;
        const float value = store_.get_value(param_id);
        const float modulated = store_.get_modulated(param_id);
        // A NaN value never compares equal, so without this it would dispatch
        // every frame forever. service_param_bindings() refuses NaN for the
        // same reason.
        if (std::isnan(value) || std::isnan(modulated)) continue;
        // Modulation is part of the change signal, not just the payload: a CLAP
        // host can move the modulated value while the base is static, and a UI
        // drawing the modulated position has to see that.
        if (value == param_subscriptions_[i].last_value &&
            modulated == param_subscriptions_[i].last_modulated)
            continue;

        // The value is snapshotted once per frame and compared against the
        // previous snapshot. That is what bounds a handler which writes its own
        // param: its write is not re-read here, so it surfaces as an ordinary
        // change on the next frame — one dispatch per frame, never recursion.
        param_subscriptions_[i].last_value = value;
        param_subscriptions_[i].last_modulated = modulated;

        const auto* info = store_.info(param_id);
        if (info == nullptr) continue;
        // Same serializer as getParamMetadata and the inspector, so a
        // subscriber and a poller cannot disagree about a param's shape.
        const auto payload = choc::json::toString(state::param_snapshot_to_value(store_, *info));
        safe_dispatch_eval(callback_alive_, &engine_,
                           "__dispatch__(" + js_string_literal(param_subscription_key(id)) +
                               ", 'paramchange', " + payload + ")",
                           "param change subscription");
        // param_subscriptions_[i] may have been reallocated or tombstoned by
        // the handler — do not touch it again.
    }
    in_param_dispatch_ = reentrant;
    if (!in_param_dispatch_) {
        std::erase_if(param_subscriptions_, [](const auto& s) { return s.id == 0; });
    }
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
        case BindingOutcome::unknown_value_channel:
            return "no value channel with that name is declared";
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
            target == ParamBinding::Target::scope
                ? (dynamic_cast<SpectrumView*>(view) != nullptr ||
                   dynamic_cast<WaveformView*>(view) != nullptr)
            : target == ParamBinding::Target::meter
                ? dynamic_cast<Meter*>(view) != nullptr
                : dynamic_cast<Knob*>(view) != nullptr ||
                      dynamic_cast<Fader*>(view) != nullptr ||
                      dynamic_cast<RangeSlider*>(view) != nullptr ||
                      dynamic_cast<Toggle*>(view) != nullptr ||
                      dynamic_cast<SegmentedControl*>(view) != nullptr ||
                      dynamic_cast<Stepper*>(view) != nullptr ||
                      dynamic_cast<ProgressBar*>(view) != nullptr;
        if (!supported) return fail(BindingOutcome::incompatible_widget);
    }

    // A `value:<name>` source names one of the processor's declared value
    // channels rather than a parameter. The prefix is mandatory and there is no
    // fallback between the two namespaces on purpose: silently resolving a
    // typo'd channel to a same-named param would bind a meter to the wrong
    // thing and look like it worked.
    std::string value_channel;
    bool value_channel_found = false;
    float neutral = 0.0f;
    state::ParamID id = 0;
    if (const auto channel = value_channel_name(param_name); !channel.empty()) {
        // Shape follows the target: a scope wants a vector channel, everything
        // else a meter channel. A shape mismatch is a MISS, not a coercion —
        // binding a scope to a meter would otherwise render a plausible wrong
        // picture instead of reporting that the channel is the wrong kind.
        visit_value_channels([&](ValueChannelSet* channels) {
            if (channels == nullptr) return;
            value_channel_found = target == ParamBinding::Target::scope
                                      ? channels->vector(channel) != nullptr
                                      : channels->meter(channel) != nullptr;
            for (const auto& info : channels->infos()) {
                if (info.name == channel) { neutral = info.neutral; break; }
            }
        });
        if (!value_channel_found)
            return fail(BindingOutcome::unknown_value_channel);
        value_channel = std::string(channel);
    } else if (target == ParamBinding::Target::scope) {
        // A scope has no parameter equivalent — there is no store shape that
        // carries a block of samples.
        return fail(BindingOutcome::unknown_value_channel);
    } else if (!resolve_param_id(param_name, id)) {
        return fail(BindingOutcome::unknown_param);
    }

    ParamBinding binding;
    binding.widget_id = widget_id;
    binding.param_id = id;
    binding.value_channel = std::move(value_channel);
    binding.neutral = neutral;
    binding.target = target;
    visit_value_channels([&](ValueChannelSet* channels) {
        if (channels == nullptr || binding.value_channel.empty()) return;
        if (binding.target == ParamBinding::Target::scope) {
            if (auto* source = channels->vector(binding.value_channel))
                binding.last_publish_seq = source->publish_seq();
        } else if (auto* source = channels->meter(binding.value_channel)) {
            binding.last_publish_seq = source->publish_seq();
        }
    });
    binding.last_publish_at = std::chrono::steady_clock::now();
    binding.transform = parse_transform(transform);
    binding.derive_from_param = transform_requests_derivation(transform);

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
    // onParamChanged/offParamChanged are the public API, but they take a JS
    // function and CHOC's NativeFunction cannot carry a JSValue argument — the
    // same constraint that makes setTimeout a JS shim over __scheduleTimer__.
    // So the handler stays in JS (in __callbacks__, which gives it
    // __dispatch__'s exception containment for free) and only the name/id
    // crosses to native.
    self.engine_.evaluate(
        "function onParamChanged(name, fn) {"
        "  if (typeof fn !== 'function') return 0;"
        "  var id = __subscribeParam__(name);"
        "  if (id > 0) __callbacks__['__param__' + id + ':paramchange'] = fn;"
        "  return id;"
        "}"
        "function offParamChanged(id) {"
        "  if (!__unsubscribeParam__(id)) return false;"
        "  delete __callbacks__['__param__' + id + ':paramchange'];"
        "  return true;"
        "}"
        "function bindEvents(source, fn) {"
        "  if (typeof fn !== 'function') return 0;"
        "  var id = __bindEvents__(source);"
        "  if (id > 0) __callbacks__['__value_events__' + id + ':events'] = fn;"
        "  return id;"
        "}"
        "function unbindEvents(id) {"
        "  if (!__unbindEvents__(id)) return false;"
        "  delete __callbacks__['__value_events__' + id + ':events'];"
        "  return true;"
        "}"
    );

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
    // __subscribeParam__(name) -> subscription id, or 0 if the param is unknown.
    // The public onParamChanged() shim above owns the callback half.
    //
    // Delivery is async on the frame tick and coalesced: a handler sees the
    // value it would have read with getParam, never a burst of intermediate
    // ones. It is deliberately origin-blind — a host automation move, a native
    // gesture and this UI's own setParam all arrive the same way — so a UI
    // written against it stays correct when the host is driving.
    //
    // Subscribing does NOT fire. The current value is already readable via
    // getParam; firing on subscribe would make every subscriber handle a
    // synthetic "change" that did not happen.
    register_bridge_function(api, "__subscribeParam__", [&self](choc::javascript::ArgumentList args) {
        const auto name = args.get<std::string>(0, "");
        if (name.empty()) return choc::value::createInt64(0);
        state::ParamID param_id = 0;
        if (!self.resolve_param_id(name, param_id)) return choc::value::createInt64(0);

        ParamSubscription sub;
        sub.id = self.next_param_subscription_id_++;
        sub.param_id = param_id;
        sub.param_name = name;
        // Seed from the store so the first frame is not reported as a change.
        sub.last_value = self.store_.get_value(param_id);
        sub.last_modulated = self.store_.get_modulated(param_id);
        self.param_subscriptions_.push_back(std::move(sub));
        return choc::value::createInt64(static_cast<std::int64_t>(
            self.param_subscriptions_.back().id));
    });

    // __unsubscribeParam__(id) -> true if a subscription was removed.
    //
    // Unknown or already-removed ids return false rather than throwing: a view
    // tearing down should be able to unsubscribe unconditionally.
    register_bridge_function(api, "__unsubscribeParam__", [&self](choc::javascript::ArgumentList args) {
        const auto id = static_cast<std::uint32_t>(args.get<int64_t>(0, 0));
        if (id == 0) return choc::value::createBool(false);
        auto& subs = self.param_subscriptions_;
        const auto it = std::find_if(subs.begin(), subs.end(),
                                     [id](const auto& s) { return s.id == id; });
        if (it == subs.end()) return choc::value::createBool(false);
        if (self.in_param_dispatch_) {
            // Called from inside a handler: erasing now would invalidate the
            // servicing loop's position. Tombstone; it compacts after the pass.
            it->id = 0;
        } else {
            subs.erase(it);
        }
        return choc::value::createBool(true);
    });

    // bindEvents("value:<name>", handler) delivers one array per newly
    // published non-empty EventFrame. The JS shim retains the function; native
    // code retains only a channel name and callback id.
    register_bridge_function(api, "__bindEvents__", [&self](choc::javascript::ArgumentList args) {
        const auto source_name = args.get<std::string>(0, "");
        const auto channel_name = value_channel_name(source_name);
        if (channel_name.empty()) return choc::value::createInt64(0);
        bool found = false;
        std::uint32_t publication = 0;
        std::uint64_t generation_identity = 0;
        self.visit_value_channels([&](ValueChannelSet* channels) {
            if (channels == nullptr) return;
            if (auto* source = channels->events(channel_name)) {
                publication = source->read().publication;
                generation_identity = channels->generation_identity();
                found = true;
            }
        });
        if (!found) return choc::value::createInt64(0);

        WidgetBridge::EventBinding binding;
        binding.id = self.next_event_binding_id_++;
        binding.channel_name = std::string(channel_name);
        binding.last_publication = publication;
        binding.value_generation_identity = generation_identity;
        self.event_bindings_.push_back(binding);
        return choc::value::createInt64(static_cast<std::int64_t>(binding.id));
    });

    register_bridge_function(api, "__unbindEvents__", [&self](choc::javascript::ArgumentList args) {
        const auto id = static_cast<std::uint32_t>(args.get<int64_t>(0, 0));
        if (id == 0) return choc::value::createBool(false);
        auto& bindings = self.event_bindings_;
        const auto it = std::find_if(bindings.begin(), bindings.end(),
                                     [id](const auto& binding) {
                                         return binding.id == id;
                                     });
        if (it == bindings.end()) return choc::value::createBool(false);
        if (self.in_event_dispatch_) {
            it->id = 0;
        } else {
            bindings.erase(it);
        }
        return choc::value::createBool(true);
    });

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

    // bindScope(widgetId, "value:<name>") -> bind a SpectrumView / WaveformView
    // to a vector value channel. Scope sources are value channels only: there is
    // no parameter shape that carries a block of samples.
    register_bridge_function(api, "bindScope", [&self](choc::javascript::ArgumentList args) {
        return choc::value::createBool(
            self.add_param_binding(args.get<std::string>(0, ""),
                              args.get<std::string>(1, ""),
                              WidgetBridge::ParamBinding::Target::scope,
                              args.numArgs > 2 ? args[2] : nullptr));
    });

    // listValueChannels() -> [{name, unit, shape, neutral}], or [] when the
    // processor declares none. This is what lets a UI discover what it can bind
    // instead of hard-coding names that silently stop resolving when the
    // processor is edited — the same drift `getParamMetadata` removed for
    // parameters. `shape` tells a caller which binder applies: `meter` for
    // bindMeter, `vector` for bindScope, `events` for bindEvents.
    register_bridge_function(api, "listValueChannels", [&self](choc::javascript::ArgumentList) {
        // Shared with the inspector's State.getValueChannels — one serializer,
        // so the two descriptions of the same channels cannot disagree.
        std::vector<ValueChannelInfo> infos;
        self.visit_value_channels([&](ValueChannelSet* channels) {
            if (channels != nullptr) infos = channels->infos();
        });
        return value_channels_to_value(infos);
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
