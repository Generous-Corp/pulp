#pragma once

#include <cstdint>
#include <string>

/// @file binding_diagnostics.hpp
/// Why a widget→parameter binding attempt did or did not take effect.
///
/// `bindWidgetToParam` and `bindMeter` answer with a bool, and generated UI
/// scripts do not check it. Without a recorded reason, a rejected binding
/// produces a control that renders correctly and does nothing — a defect only a
/// person poking at the running plugin will find. These types let the bridge
/// keep the reason so a diagnostic can name the dead control instead.

namespace pulp::view {

/// What a binding drives on its widget.
enum class BindingTarget : std::uint8_t {
    value,  ///< the widget's primary value (Knob, Fader, RangeSlider, Toggle, ProgressBar)
    meter,  ///< a Meter's level
    scope,  ///< a block of samples for a SpectrumView / WaveformView
};

/// The outcome of one `bindWidgetToParam` / `bindMeter` call.
enum class BindingOutcome : std::uint8_t {
    ok,                       ///< bound; the widget exists and accepts this target
    deferred_widget_missing,  ///< bound; the view does not exist YET, which is legal
    replaced_prior_binding,   ///< bound, displacing an earlier binding for this widget
    empty_widget_id,          ///< ─── below here nothing was bound ───
    empty_param_name,
    null_widget,          ///< the id maps to a null view
    incompatible_widget,  ///< the widget cannot accept this target (e.g. a meter on a Knob)
    unknown_param,        ///< the store declares no parameter with that name
    /// A `value:<name>` source named no channel the processor declares — or the
    /// processor declares none at all, e.g. a UI bound against the wrong build.
    unknown_value_channel,
};

/// True for the three outcomes that produced a live binding.
///
/// `deferred_widget_missing` counts as bound on purpose: scripts legitimately
/// register a binding before creating the view, so treating it as a failure
/// would report correct code as broken — the fastest way for a diagnostic to
/// lose its audience.
constexpr bool is_bound(BindingOutcome o) noexcept {
    return o == BindingOutcome::ok || o == BindingOutcome::deferred_widget_missing ||
           o == BindingOutcome::replaced_prior_binding;
}

/// Human-readable reason, for diagnostics and test failure messages.
const char* describe(BindingOutcome o) noexcept;

/// One recorded attempt, successful or not.
struct BindingAttempt {
    std::string widget_id;
    std::string param_name;
    BindingTarget target = BindingTarget::value;
    BindingOutcome outcome = BindingOutcome::ok;
};

}  // namespace pulp::view
