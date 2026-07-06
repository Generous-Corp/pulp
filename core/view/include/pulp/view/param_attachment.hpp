#pragma once

/// @file param_attachment.hpp
/// Widget-to-parameter binding helpers with automatic normalization and gestures.

#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/state/binding.hpp>
#include <pulp/state/external_binding.hpp>
#include <memory>

namespace pulp::view {

/// Attach a Knob widget to a parameter. Handles normalization and gestures.
///
/// @code
/// auto [knob, binding] = attach_knob(store, kCutoff, 60);
/// root.add_child(std::move(knob));
/// // Call binding.poll() periodically to sync host automation
/// @endcode
inline std::pair<std::unique_ptr<Knob>, state::Binding>
attach_knob(state::StateStore& store, state::ParamID id, float size = 60.0f) {
    state::Binding binding(store, id);
    auto knob = std::make_unique<Knob>();
    auto* info = binding.info();

    if (info) {
        knob->set_label(info->name);
        knob->set_id(info->name);

        auto unit = info->unit;
        auto range = info->range;
        knob->set_format([unit, range](float norm) {
            float val = range.denormalize(norm);
            char buf[32];
            if (std::abs(val) >= 100) std::snprintf(buf, sizeof(buf), "%.0f", val);
            else if (std::abs(val) >= 10) std::snprintf(buf, sizeof(buf), "%.1f", val);
            else std::snprintf(buf, sizeof(buf), "%.2f", val);
            return std::string(buf) + (unit.empty() ? "" : " " + unit);
        });
    }

    knob->set_value(binding.get_normalized());
    knob->flex().preferred_width = size;
    knob->flex().preferred_height = size;

    // Wire change callback
    auto param_id = id;
    knob->on_change = [&store, param_id](float norm) {
        store.set_normalized(param_id, norm);
    };
    // Bracket the drag in a host gesture so DAWs RECORD automation from UI
    // moves: begin → value changes → end. The store's gesture callbacks map to
    // the host (AU Begin/EndParameterChangeGesture, VST3 begin/endEdit, CLAP
    // gesture events). Without this a knob emits value changes with no "touch"
    // and hosts like Logic don't capture them into an automation lane.
    knob->on_gesture_begin = [&store, param_id]() { store.begin_gesture(param_id); };
    knob->on_gesture_end   = [&store, param_id]() { store.end_gesture(param_id); };

    return {std::move(knob), std::move(binding)};
}

/// Attach a Fader widget to a parameter.
inline std::pair<std::unique_ptr<Fader>, state::Binding>
attach_fader(state::StateStore& store, state::ParamID id) {
    state::Binding binding(store, id);
    auto fader = std::make_unique<Fader>();
    auto* info = binding.info();

    if (info) {
        fader->set_label(info->name);
        fader->set_id(info->name);
    }

    fader->set_value(binding.get_normalized());

    auto param_id = id;
    fader->on_change = [&store, param_id](float norm) {
        store.set_normalized(param_id, norm);
    };
    // Gesture-bracket the drag so hosts record UI automation (see attach_knob).
    fader->on_gesture_begin = [&store, param_id]() { store.begin_gesture(param_id); };
    fader->on_gesture_end   = [&store, param_id]() { store.end_gesture(param_id); };

    return {std::move(fader), std::move(binding)};
}

/// Attach a Toggle widget to a boolean parameter.
inline std::pair<std::unique_ptr<Toggle>, state::Binding>
attach_toggle(state::StateStore& store, state::ParamID id) {
    state::Binding binding(store, id);
    auto toggle = std::make_unique<Toggle>();
    auto* info = binding.info();

    if (info) {
        toggle->set_label(info->name);
        toggle->set_id(info->name);
    }

    // Snap (don't animate) the initial seed: the editor must paint the stored
    // state on the first frame — a freshly opened plugin, or a single headless
    // screenshot, never advances the animation clock.
    toggle->set_on(binding.get_normalized() > 0.5f, /*animate=*/false);

    auto param_id = id;
    toggle->on_toggle = [&store, param_id](bool on) {
        // Discrete change bracketed in a one-shot gesture so hosts record it.
        store.begin_gesture(param_id);
        store.set_normalized(param_id, on ? 1.0f : 0.0f);
        store.end_gesture(param_id);
    };

    return {std::move(toggle), std::move(binding)};
}

/// Attach a ComboBox to an integer/stepped parameter.
inline std::pair<std::unique_ptr<ComboBox>, state::Binding>
attach_combo(state::StateStore& store, state::ParamID id,
             std::vector<std::string> items) {
    state::Binding binding(store, id);
    auto combo = std::make_unique<ComboBox>();
    auto* info = binding.info();

    if (info) combo->set_id(info->name);
    combo->set_items(std::move(items));
    combo->set_selected(static_cast<int>(binding.get()));

    auto param_id = id;
    combo->on_change = [&store, param_id](int index) {
        // Discrete change bracketed in a one-shot gesture so hosts record it.
        store.begin_gesture(param_id);
        store.set_value(param_id, static_cast<float>(index));
        store.end_gesture(param_id);
    };

    return {std::move(combo), std::move(binding)};
}

/// Poll all bindings in a vector to sync with host automation.
///
/// @note Soft-deprecated alongside `Binding::poll()`. Prefer the
///       VBlank-locked dirty-flag pattern: have parameter changes call
///       `WindowHost::mark_dirty()` so the UI repaints once on the next
///       vsync instead of polling every binding on a timer. This helper
///       remains for hosts without a vblank-driven `RenderLoop`.
inline void poll_bindings(std::vector<state::Binding>& bindings) {
    for (auto& b : bindings) b.poll();
}

// ── External (lambda-backed) attachments ─────────────────────────────────────
// The same widgets, bound to an arbitrary get/set source instead of a StateStore
// parameter — e.g. a host-side value in an embedding app. The widget writes
// through the config's setter; the returned ExternalBinding is the host-side
// handle for poll()/notify()/gestures/on_change. See state::ExternalBinding.

inline std::pair<std::unique_ptr<Knob>, state::ExternalBinding>
attach_knob(state::ExternalBindingConfig cfg, float size = 60.0f) {
    auto setter = cfg.set;
    auto range = cfg.range;
    auto begin = cfg.begin_gesture;
    auto end = cfg.end_gesture;
    std::string name = cfg.name;
    std::string unit = cfg.unit;

    state::ExternalBinding binding(std::move(cfg));
    auto knob = std::make_unique<Knob>();
    if (!name.empty()) {
        knob->set_label(name);
        knob->set_id(name);
    }
    knob->set_format([unit, range](float norm) {
        float val = range.denormalize(norm);
        char buf[32];
        if (std::abs(val) >= 100) std::snprintf(buf, sizeof(buf), "%.0f", val);
        else if (std::abs(val) >= 10) std::snprintf(buf, sizeof(buf), "%.1f", val);
        else std::snprintf(buf, sizeof(buf), "%.2f", val);
        return std::string(buf) + (unit.empty() ? "" : " " + unit);
    });
    knob->set_value(binding.get_normalized());
    knob->flex().preferred_width = size;
    knob->flex().preferred_height = size;
    knob->on_change = [setter, range](float norm) {
        if (setter) setter(range.denormalize(norm));
    };
    // Bracket the drag in the host's gesture callbacks so DAWs record UI moves
    // as automation (same rationale as the StateStore overload above).
    knob->on_gesture_begin = [begin]() { if (begin) begin(); };
    knob->on_gesture_end   = [end]()   { if (end) end(); };
    return {std::move(knob), std::move(binding)};
}

inline std::pair<std::unique_ptr<Fader>, state::ExternalBinding>
attach_fader(state::ExternalBindingConfig cfg) {
    auto setter = cfg.set;
    auto range = cfg.range;
    auto begin = cfg.begin_gesture;
    auto end = cfg.end_gesture;
    std::string name = cfg.name;

    state::ExternalBinding binding(std::move(cfg));
    auto fader = std::make_unique<Fader>();
    if (!name.empty()) {
        fader->set_label(name);
        fader->set_id(name);
    }
    fader->set_value(binding.get_normalized());
    fader->on_change = [setter, range](float norm) {
        if (setter) setter(range.denormalize(norm));
    };
    fader->on_gesture_begin = [begin]() { if (begin) begin(); };
    fader->on_gesture_end   = [end]()   { if (end) end(); };
    return {std::move(fader), std::move(binding)};
}

inline std::pair<std::unique_ptr<Toggle>, state::ExternalBinding>
attach_toggle(state::ExternalBindingConfig cfg) {
    auto setter = cfg.set;
    auto range = cfg.range;
    auto begin = cfg.begin_gesture;
    auto end = cfg.end_gesture;
    std::string name = cfg.name;

    state::ExternalBinding binding(std::move(cfg));
    auto toggle = std::make_unique<Toggle>();
    if (!name.empty()) {
        toggle->set_label(name);
        toggle->set_id(name);
    }
    // Snap (don't animate) the initial seed so the first frame / a headless
    // screenshot paints the stored state (matches the StateStore overload).
    toggle->set_on(binding.get_normalized() > 0.5f, /*animate=*/false);
    toggle->on_toggle = [setter, range, begin, end](bool on) {
        // Discrete change bracketed in a one-shot gesture so hosts record it.
        if (begin) begin();
        if (setter) setter(range.denormalize(on ? 1.0f : 0.0f));
        if (end) end();
    };
    return {std::move(toggle), std::move(binding)};
}

} // namespace pulp::view
