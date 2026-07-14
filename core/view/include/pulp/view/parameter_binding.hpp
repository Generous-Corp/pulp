#pragma once

/// @file parameter_binding.hpp
/// Two-way binding between a standard widget and a store parameter, with
/// host automation gestures. One call replaces the hand-written
/// widget-callback ↔ parameter-listener pair an editor would otherwise
/// maintain per control.
///
/// `bind_parameter(widget, store, id)` wires:
///   - widget interaction → host gesture (begin on press, end on release)
///     so DAWs record and play back the control's automation;
///   - widget value changes → `store.set_normalized(id, …)`;
///   - store changes (e.g. automation playback, preset load) → the widget's
///     displayed value.
///
/// It returns a `ParameterBinding` that owns the store listener. The binding
/// is lifetime-scoped: keep it alive for as long as the widget is bound
/// (typically as a member of the editor, so it dies with the widget), and
/// dropping it unbinds — there is no separate detach call.
///
/// Without this, a widget changes the parameter audibly but the host does
/// not record the move, because the raw value write is not bracketed in a
/// gesture. See pulp::state::ParameterEdit for the lower-level primitive
/// used by bespoke (non-widget) controls.
///
/// UI-thread only.

#include <pulp/state/parameter.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>       // ComboBox
#include <pulp/view/design_frame_view.hpp>   // DesignStepper
#include <utility>

namespace pulp::view {

/// RAII owner of a widget↔parameter binding's store listener. Move-only;
/// keep it alive for the binding's lifetime.
class ParameterBinding {
public:
    ParameterBinding() = default;
    explicit ParameterBinding(state::ListenerToken token) : token_(std::move(token)) {}
    ParameterBinding(ParameterBinding&&) noexcept = default;
    ParameterBinding& operator=(ParameterBinding&&) noexcept = default;
    ParameterBinding(const ParameterBinding&) = delete;
    ParameterBinding& operator=(const ParameterBinding&) = delete;

private:
    state::ListenerToken token_;
};

/// Knob ↔ parameter (normalized).
[[nodiscard]] inline ParameterBinding
bind_parameter(Knob& knob, state::StateStore& store, state::ParamID id) {
    knob.set_value(store.get_normalized(id));
    knob.on_gesture_begin = [&store, id] { store.begin_gesture(id); };
    knob.on_change = [&store, id](float v) { store.set_normalized(id, v); };
    knob.on_gesture_end = [&store, id] { store.end_gesture(id); };
    return ParameterBinding(store.add_listener(
        [&knob, &store, id](state::ParamID changed, float) {
            if (changed == id) knob.set_value(store.get_normalized(id));
        },
        state::ListenerThread::Main));
}

/// Fader ↔ parameter (normalized).
[[nodiscard]] inline ParameterBinding
bind_parameter(Fader& fader, state::StateStore& store, state::ParamID id) {
    fader.set_value(store.get_normalized(id));
    fader.on_gesture_begin = [&store, id] { store.begin_gesture(id); };
    fader.on_change = [&store, id](float v) { store.set_normalized(id, v); };
    fader.on_gesture_end = [&store, id] { store.end_gesture(id); };
    return ParameterBinding(store.add_listener(
        [&fader, &store, id](state::ParamID changed, float) {
            if (changed == id) fader.set_value(store.get_normalized(id));
        },
        state::ListenerThread::Main));
}

/// XY pad ↔ two parameters (normalized). A single drag drives both.
[[nodiscard]] inline ParameterBinding
bind_parameter(XYPad& pad, state::StateStore& store, state::ParamID x_id, state::ParamID y_id) {
    pad.on_gesture_begin = [&store, x_id, y_id] {
        store.begin_gesture(x_id);
        store.begin_gesture(y_id);
    };
    pad.on_change = [&store, x_id, y_id](float x, float y) {
        store.set_normalized(x_id, x);
        store.set_normalized(y_id, y);
    };
    pad.on_gesture_end = [&store, x_id, y_id] {
        store.end_gesture(x_id);
        store.end_gesture(y_id);
    };
    return ParameterBinding(store.add_listener(
        [&pad, &store, x_id, y_id](state::ParamID changed, float) {
            if (changed == x_id) pad.set_x(store.get_normalized(x_id));
            if (changed == y_id) pad.set_y(store.get_normalized(y_id));
        },
        state::ListenerThread::Main));
}

/// Range slider ↔ parameter (normalized).
[[nodiscard]] inline ParameterBinding
bind_parameter(RangeSlider& slider, state::StateStore& store, state::ParamID id) {
    slider.set_value(store.get_normalized(id));
    slider.on_gesture_begin = [&store, id] { store.begin_gesture(id); };
    slider.on_change = [&store, id](float v) { store.set_normalized(id, v); };
    slider.on_gesture_end = [&store, id] { store.end_gesture(id); };
    return ParameterBinding(store.add_listener(
        [&slider, &store, id](state::ParamID changed, float) {
            if (changed == id) slider.set_value(store.get_normalized(id));
        },
        state::ListenerThread::Main));
}

/// ComboBox ↔ stepped/index parameter. A selection is a one-shot gesture so
/// the host records the discrete change; automation playback drives the
/// selection back (via set_selected_silent, so it does not re-fire on_change).
[[nodiscard]] inline ParameterBinding
bind_parameter(ComboBox& combo, state::StateStore& store, state::ParamID id) {
    combo.set_selected_silent(static_cast<int>(store.get_value(id)));
    combo.on_change = [&store, id](int index) {
        store.begin_gesture(id);
        store.set_value(id, static_cast<float>(index));
        store.end_gesture(id);
    };
    return ParameterBinding(store.add_listener(
        [&combo, &store, id](state::ParamID changed, float) {
            if (changed == id) combo.set_selected_silent(static_cast<int>(store.get_value(id)));
        },
        state::ListenerThread::Main));
}

/// DesignStepper ↔ stepped/index parameter (same contract as ComboBox):
/// user steps are one-shot gestures; automation playback drives the selection
/// via set_selected_silent (which does not re-fire on_select).
[[nodiscard]] inline ParameterBinding
bind_parameter(DesignStepper& stepper, state::StateStore& store, state::ParamID id) {
    stepper.set_selected_silent(static_cast<int>(store.get_value(id)));
    stepper.on_select = [&store, id](int index) {
        store.begin_gesture(id);
        store.set_value(id, static_cast<float>(index));
        store.end_gesture(id);
    };
    return ParameterBinding(store.add_listener(
        [&stepper, &store, id](state::ParamID changed, float) {
            if (changed == id) stepper.set_selected_silent(static_cast<int>(store.get_value(id)));
        },
        state::ListenerThread::Main));
}

/// Toggle (switch) ↔ parameter, as a one-shot gesture per flip.
[[nodiscard]] inline ParameterBinding
bind_parameter(Toggle& toggle, state::StateStore& store, state::ParamID id) {
    toggle.set_on(store.get_value(id) >= 0.5f);
    toggle.on_toggle = [&store, id](bool on) {
        store.begin_gesture(id);
        store.set_value(id, on ? 1.0f : 0.0f);
        store.end_gesture(id);
    };
    return ParameterBinding(store.add_listener(
        [&toggle, &store, id](state::ParamID changed, float) {
            if (changed == id) toggle.set_on(store.get_value(id) >= 0.5f);
        },
        state::ListenerThread::Main));
}

/// Checkbox ↔ parameter, as a one-shot gesture per flip.
[[nodiscard]] inline ParameterBinding
bind_parameter(Checkbox& box, state::StateStore& store, state::ParamID id) {
    box.set_checked(store.get_value(id) >= 0.5f);
    box.on_change = [&store, id](bool on) {
        store.begin_gesture(id);
        store.set_value(id, on ? 1.0f : 0.0f);
        store.end_gesture(id);
    };
    return ParameterBinding(store.add_listener(
        [&box, &store, id](state::ParamID changed, float) {
            if (changed == id) box.set_checked(store.get_value(id) >= 0.5f);
        },
        state::ListenerThread::Main));
}

/// Toggle button ↔ parameter. A press is a complete one-shot gesture so
/// the host records the discrete change.
[[nodiscard]] inline ParameterBinding
bind_parameter(ToggleButton& button, state::StateStore& store, state::ParamID id) {
    button.set_on(store.get_value(id) >= 0.5f);
    button.on_toggle = [&store, id](bool on) {
        store.begin_gesture(id);
        store.set_value(id, on ? 1.0f : 0.0f);
        store.end_gesture(id);
    };
    return ParameterBinding(store.add_listener(
        [&button, &store, id](state::ParamID changed, float) {
            if (changed == id) button.set_on(store.get_value(id) >= 0.5f);
        },
        state::ListenerThread::Main));
}

} // namespace pulp::view
