#pragma once

/// @file param_attachment.hpp
/// Widget-to-parameter binding helpers with automatic normalization and gestures.

#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/state/binding.hpp>
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

    toggle->set_on(binding.get_normalized() > 0.5f);

    auto param_id = id;
    toggle->on_toggle = [&store, param_id](bool on) {
        store.set_normalized(param_id, on ? 1.0f : 0.0f);
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
        store.set_value(param_id, static_cast<float>(index));
    };

    return {std::move(combo), std::move(binding)};
}

/// Poll all bindings in a vector to sync with host automation.
inline void poll_bindings(std::vector<state::Binding>& bindings) {
    for (auto& b : bindings) b.poll();
}

} // namespace pulp::view
