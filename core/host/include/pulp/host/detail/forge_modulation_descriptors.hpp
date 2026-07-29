#pragma once

// Semantic vocabulary transcribed from Forge's catalog-owned registry.

#include <pulp/host/forge_param_descriptor.hpp>

#include <vector>

namespace pulp::host::forge_modulation {

inline ForgeNodeDescriptor mod_lfo_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "mod_lfo";
    d.label = "Mod LFO";
    d.description = "Generate flexible delayed or fading periodic and random-blend modulation signals.";
    d.realizations = {
        {"default", "forge_mod_lfo"},
    };
    d.params = {
        {"rate_hz", 1, "Rate", "Hz", "Rate parameter measured in Hz. Generate flexible delayed or fading periodic and random-blend modulation signals.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"depth", 2, "Depth", "%", "Depth parameter measured in %. Generate flexible delayed or fading periodic and random-blend modulation signals.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"wave", 3, "Wave", "", "Wave parameter. Generate flexible delayed or fading periodic and random-blend modulation signals.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"sine", "Sine", 0.0f}, {"triangle", "Triangle", 1.0f}, {"saw_up", "Saw Up", 2.0f}, {"saw_down", "Saw Down", 3.0f}, {"square", "Square", 4.0f}, {"sample_hold", "Sample & Hold", 5.0f}, {"smooth_random", "Smooth Random", 6.0f}}, {}},
        {"pulse_width", 4, "Pulse Width", "", "Pulse Width parameter. Generate flexible delayed or fading periodic and random-blend modulation signals.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"random_blend", 5, "Random Blend", "", "Random Blend parameter. Generate flexible delayed or fading periodic and random-blend modulation signals.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"delay_ms", 6, "Delay", "ms", "Delay parameter measured in ms. Generate flexible delayed or fading periodic and random-blend modulation signals.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fade_in_ms", 7, "Fade In", "ms", "Fade In parameter measured in ms. Generate flexible delayed or fading periodic and random-blend modulation signals.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"shape_morph", 8, "Shape Morph", "", "Shape Morph parameter. Generate flexible delayed or fading periodic and random-blend modulation signals.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"morph_enabled", 9, "Morph Enabled", "", "Morph Enabled parameter. Generate flexible delayed or fading periodic and random-blend modulation signals.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"triangle_bias", 10, "Triangle Bias", "", "Triangle Bias parameter. Generate flexible delayed or fading periodic and random-blend modulation signals.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"random_segments", 11, "Random Segments", "", "Random Segments parameter. Generate flexible delayed or fading periodic and random-blend modulation signals.", ForgeParamKind::stepped, ForgeParamCurve::linear, forge_integer_choices(1, 16), {}},
        {"phase_degrees", 12, "Phase Degrees", "deg", "Phase Degrees parameter measured in deg. Generate flexible delayed or fading periodic and random-blend modulation signals.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fade_out_ms", 13, "Fade Out", "ms", "Fade Out parameter measured in ms. Generate flexible delayed or fading periodic and random-blend modulation signals.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fade_quadratic", 14, "Fade Quadratic", "", "Fade Quadratic parameter. Generate flexible delayed or fading periodic and random-blend modulation signals.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"repeat_count", 15, "Repeat Count", "", "Repeat Count parameter. Generate flexible delayed or fading periodic and random-blend modulation signals.", ForgeParamKind::stepped, ForgeParamCurve::linear, forge_integer_choices(0, 128), {}},
    };
    return d;
}

inline ForgeNodeDescriptor lpg_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "lpg";
    d.label = "LPG";
    d.description = "Shape struck, organic plucks with coupled low-pass filtering and amplitude decay.";
    d.realizations = {
        {"default", "forge_mod_lpg"},
    };
    d.params = {
        {"decay_ms", 1, "Decay", "ms", "Decay parameter measured in ms. Shape struck, organic plucks with coupled low-pass filtering and amplitude decay.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"colour", 2, "Colour", "", "Colour parameter. Shape struck, organic plucks with coupled low-pass filtering and amplitude decay.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"droop", 3, "Droop", "", "Droop parameter. Shape struck, organic plucks with coupled low-pass filtering and amplitude decay.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"brightness_hz", 4, "Brightness", "Hz", "Brightness parameter measured in Hz. Shape struck, organic plucks with coupled low-pass filtering and amplitude decay.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"struck", 5, "Struck", "", "Struck parameter. Shape struck, organic plucks with coupled low-pass filtering and amplitude decay.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"rise_ms", 6, "Rise", "ms", "Rise parameter measured in ms. Shape struck, organic plucks with coupled low-pass filtering and amplitude decay.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"darkness_hz", 7, "Darkness", "Hz", "Darkness parameter measured in Hz. Shape struck, organic plucks with coupled low-pass filtering and amplitude decay.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"strike_threshold", 8, "Strike Threshold", "%", "Strike Threshold parameter measured in %. Shape struck, organic plucks with coupled low-pass filtering and amplitude decay.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"refractory_ms", 9, "Refractory", "ms", "Refractory parameter measured in ms. Shape struck, organic plucks with coupled low-pass filtering and amplitude decay.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor slew_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "slew";
    d.label = "Slew";
    d.description = "Smooth abrupt control or audio changes with independent rise and fall timing.";
    d.realizations = {
        {"default", "forge_mod_slew"},
    };
    d.params = {
        {"rise_ms", 1, "Rise", "ms", "Rise parameter measured in ms. Smooth abrupt control or audio changes with independent rise and fall timing.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fall_ms", 2, "Fall", "ms", "Fall parameter measured in ms. Smooth abrupt control or audio changes with independent rise and fall timing.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"curved", 3, "Curved", "", "Curved parameter. Smooth abrupt control or audio changes with independent rise and fall timing.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"linear", "Linear", 0.0f}, {"curved", "Curved", 1.0f}}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor transient_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "transient";
    d.label = "Transient";
    d.description = "Extract attack-versus-body motion as a control signal from incoming transients.";
    d.realizations = {
        {"default", "forge_mod_transient"},
    };
    d.params = {
        {"fast_ms", 1, "Fast", "ms", "Fast parameter measured in ms. Extract attack-versus-body motion as a control signal from incoming transients.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"slow_ms", 2, "Slow", "ms", "Slow parameter measured in ms. Extract attack-versus-body motion as a control signal from incoming transients.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"sensitivity", 3, "Sensitivity", "x", "Sensitivity parameter measured in x. Extract attack-versus-body motion as a control signal from incoming transients.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"invert", 4, "Invert", "", "Invert parameter. Extract attack-versus-body motion as a control signal from incoming transients.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor trig_env_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "trig_env";
    d.label = "Trig Env";
    d.description = "Turn trigger-like input events into attack-hold-decay control envelopes.";
    d.realizations = {
        {"default", "forge_mod_env"},
    };
    d.params = {
        {"attack_ms", 1, "Attack", "ms", "Attack parameter measured in ms. Turn trigger-like input events into attack-hold-decay control envelopes.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"hold_ms", 2, "Hold", "ms", "Hold parameter measured in ms. Turn trigger-like input events into attack-hold-decay control envelopes.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"decay_ms", 3, "Decay", "ms", "Decay parameter measured in ms. Turn trigger-like input events into attack-hold-decay control envelopes.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"curve", 4, "Curve", "", "Curve parameter. Turn trigger-like input events into attack-hold-decay control envelopes.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"threshold", 5, "Threshold", "", "Threshold parameter. Turn trigger-like input events into attack-hold-decay control envelopes.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"delay_ms", 6, "Delay", "ms", "Delay parameter measured in ms. Turn trigger-like input events into attack-hold-decay control envelopes.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"depth", 7, "Depth", "%", "Depth parameter measured in %. Turn trigger-like input events into attack-hold-decay control envelopes.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"loop", 8, "Loop", "", "Loop parameter. Turn trigger-like input events into attack-hold-decay control envelopes.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"loop_count", 9, "Loop Count", "", "Loop Count parameter. Turn trigger-like input events into attack-hold-decay control envelopes.", ForgeParamKind::stepped, ForgeParamCurve::linear, forge_integer_choices(0, 128), {}},
        {"refractory_ms", 10, "Refractory", "ms", "Refractory parameter measured in ms. Turn trigger-like input events into attack-hold-decay control envelopes.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_sensitive", 11, "Velocity Sensitive", "", "Velocity Sensitive parameter. Turn trigger-like input events into attack-hold-decay control envelopes.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"independent_curves", 12, "Independent Curves", "", "Independent Curves parameter. Turn trigger-like input events into attack-hold-decay control envelopes.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"attack_curve", 13, "Attack Curve", "", "Attack Curve parameter. Turn trigger-like input events into attack-hold-decay control envelopes.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"decay_curve", 14, "Decay Curve", "", "Decay Curve parameter. Turn trigger-like input events into attack-hold-decay control envelopes.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline std::vector<ForgeNodeDescriptor> modulation_descriptors() {
    return {
        mod_lfo_descriptor(),
        lpg_descriptor(),
        slew_descriptor(),
        transient_descriptor(),
        trig_env_descriptor(),
    };
}

}  // namespace pulp::host::forge_modulation
