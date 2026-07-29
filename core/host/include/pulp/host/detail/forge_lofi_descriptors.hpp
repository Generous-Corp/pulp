#pragma once

// Semantic vocabulary transcribed from Forge's catalog-owned registry.

#include <pulp/host/forge_param_descriptor.hpp>

#include <vector>

namespace pulp::host::forge_lofi {

inline ForgeNodeDescriptor delay_line_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "delay_line";
    d.label = "Delay Line";
    d.description = "Create clean echoes and rhythmic repeats with controllable recirculation.";
    d.realizations = {
        {"default", "forge_lofi_delay"},
    };
    d.params = {
        {"time_ms", 1, "Time", "ms", "Time parameter measured in ms. Create clean echoes and rhythmic repeats with controllable recirculation.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"feedback", 2, "Feedback", "%", "Feedback parameter measured in %. Create clean echoes and rhythmic repeats with controllable recirculation.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor svf_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "svf";
    d.label = "SVF";
    d.description = "Shape tone with resonant lowpass, highpass, bandpass, or notch filtering.";
    d.axes = {{"mode", "Mode", "Finite construction modes for SVF.", {{"lowpass", "Lowpass", 0.0f}, {"highpass", "Highpass", 1.0f}, {"bandpass", "Bandpass", 2.0f}, {"notch", "Notch", 3.0f}}}};
    d.realizations = {
        {"lowpass", "forge_lofi_filter"},
        {"highpass", "forge_lofi_filter_highpass"},
        {"bandpass", "forge_lofi_filter_bandpass"},
        {"notch", "forge_lofi_filter_notch"},
    };
    d.params = {
        {"cutoff_hz", 1, "Cutoff", "Hz", "Cutoff parameter measured in Hz. Shape tone with resonant lowpass, highpass, bandpass, or notch filtering.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"resonance", 2, "Resonance", "Q", "Resonance parameter measured in Q. Shape tone with resonant lowpass, highpass, bandpass, or notch filtering.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor waveshaper_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "waveshaper";
    d.label = "Waveshaper";
    d.description = "Add smooth harmonic saturation and tame peaks with a compact nonlinear stage.";
    d.realizations = {
        {"default", "forge_lofi_waveshaper"},
    };
    d.params = {
        {"drive", 1, "Drive", "×", "Drive parameter measured in ×. Add smooth harmonic saturation and tame peaks with a compact nonlinear stage.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor dry_wet_mixer_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "dry_wet_mixer";
    d.label = "Dry Wet Mixer";
    d.description = "Blend parallel dry and processed signal paths under one mix control.";
    d.realizations = {
        {"default", "forge_lofi_drywet"},
    };
    d.params = {
        {"mix", 1, "Mix", "%", "Mix parameter measured in %. Blend parallel dry and processed signal paths under one mix control.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor noise_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "noise";
    d.label = "Noise";
    d.description = "Layer controllable hiss or noise texture into an effect graph.";
    d.realizations = {
        {"default", "forge_lofi_noise"},
    };
    d.params = {
        {"level", 1, "Level", "%", "Level parameter measured in %. Layer controllable hiss or noise texture into an effect graph.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor bitcrush_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "bitcrush";
    d.label = "Bitcrush";
    d.description = "Create lo-fi digital grit through bit-depth and sample-rate reduction.";
    d.realizations = {
        {"default", "forge_lofi_bitcrush"},
    };
    d.params = {
        {"bit_depth", 1, "Bit Depth", "bit", "Bit Depth parameter measured in bit. Create lo-fi digital grit through bit-depth and sample-rate reduction.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"sample_rate_reduction", 2, "Sample Rate Reduction", "×", "Sample Rate Reduction parameter measured in ×. Create lo-fi digital grit through bit-depth and sample-rate reduction.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor trim_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "trim";
    d.label = "Trim";
    d.description = "Set a deliberate gain-staging or output-level adjustment in decibels.";
    d.realizations = {
        {"default", "forge_lofi_trim"},
    };
    d.params = {
        {"gain_db", 1, "Gain", "dB", "Gain parameter measured in dB. Set a deliberate gain-staging or output-level adjustment in decibels.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor ping_pong_delay_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "ping_pong_delay";
    d.label = "Ping Pong Delay";
    d.description = "Bounce echoes across the stereo field for rhythmic width and spatial motion.";
    d.realizations = {
        {"default", "forge_lofi_ping_pong"},
    };
    d.params = {
        {"time_ms", 1, "Time", "ms", "Time parameter measured in ms. Bounce echoes across the stereo field for rhythmic width and spatial motion.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"feedback", 2, "Feedback", "%", "Feedback parameter measured in %. Bounce echoes across the stereo field for rhythmic width and spatial motion.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"width", 3, "Width", "%", "Width parameter measured in %. Bounce echoes across the stereo field for rhythmic width and spatial motion.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor reverb_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "reverb";
    d.label = "Reverb";
    d.description = "Place sounds in a compact algorithmic space with adjustable decay, damping, and wetness.";
    d.realizations = {
        {"default", "forge_lofi_reverb"},
    };
    d.params = {
        {"decay", 1, "Decay", "s", "Decay parameter measured in s. Place sounds in a compact algorithmic space with adjustable decay, damping, and wetness.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"damping", 2, "Damping", "%", "Damping parameter measured in %. Place sounds in a compact algorithmic space with adjustable decay, damping, and wetness.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mix", 3, "Mix", "%", "Mix parameter measured in %. Place sounds in a compact algorithmic space with adjustable decay, damping, and wetness.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor compressor_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "compressor";
    d.label = "Compressor";
    d.description = "Provide straightforward downward compression when simple level control is more useful than coloration.";
    d.realizations = {
        {"default", "forge_lofi_compressor"},
    };
    d.params = {
        {"threshold", 1, "Threshold", "dB", "Threshold parameter measured in dB. Provide straightforward downward compression when simple level control is more useful than coloration.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"ratio", 2, "Ratio", ":1", "Ratio parameter measured in :1. Provide straightforward downward compression when simple level control is more useful than coloration.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"attack", 3, "Attack", "ms", "Attack parameter measured in ms. Provide straightforward downward compression when simple level control is more useful than coloration.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"release", 4, "Release", "ms", "Release parameter measured in ms. Provide straightforward downward compression when simple level control is more useful than coloration.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor gate_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "gate";
    d.label = "Gate";
    d.description = "Suppress low-level bleed and noise while preserving controlled attack, hold, and release behavior.";
    d.realizations = {
        {"default", "forge_lofi_gate"},
    };
    d.params = {
        {"threshold", 1, "Threshold", "dB", "Threshold parameter measured in dB. Suppress low-level bleed and noise while preserving controlled attack, hold, and release behavior.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"attack", 2, "Attack", "ms", "Attack parameter measured in ms. Suppress low-level bleed and noise while preserving controlled attack, hold, and release behavior.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"hold", 3, "Hold", "ms", "Hold parameter measured in ms. Suppress low-level bleed and noise while preserving controlled attack, hold, and release behavior.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"release", 4, "Release", "ms", "Release parameter measured in ms. Suppress low-level bleed and noise while preserving controlled attack, hold, and release behavior.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor lfo_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "lfo";
    d.label = "LFO";
    d.description = "Generate a periodic control signal for graph-level modulation of amplitude, filter, or delay nodes.";
    d.realizations = {
        {"default", "forge_lofi_lfo"},
    };
    d.params = {
        {"rate_hz", 1, "Rate", "Hz", "Rate parameter measured in Hz. Generate a periodic control signal for graph-level modulation of amplitude, filter, or delay nodes.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"depth", 2, "Depth", "%", "Depth parameter measured in %. Generate a periodic control signal for graph-level modulation of amplitude, filter, or delay nodes.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"shape", 3, "Shape", "", "Shape parameter. Generate a periodic control signal for graph-level modulation of amplitude, filter, or delay nodes.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"sine", "Sine", 0.0f}, {"triangle", "Triangle", 1.0f}, {"saw", "Saw", 2.0f}, {"square", "Square", 3.0f}}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor vca_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "vca";
    d.label = "VCA";
    d.description = "Scale an audio signal from a control input for tremolo, pumping, and envelope-shaped level motion.";
    d.realizations = {
        {"default", "forge_lofi_vca"},
    };
    d.params = {
        {"gain", 1, "Gain", "×", "Gain parameter measured in ×. Scale an audio signal from a control input for tremolo, pumping, and envelope-shaped level motion.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor env_follower_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "env_follower";
    d.label = "Env Follower";
    d.description = "Convert input amplitude into a smooth control signal for dynamics and envelope-driven modulation.";
    d.realizations = {
        {"default", "forge_lofi_env_follower"},
    };
    d.params = {
        {"attack_ms", 1, "Attack", "ms", "Attack parameter measured in ms. Convert input amplitude into a smooth control signal for dynamics and envelope-driven modulation.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"release_ms", 2, "Release", "ms", "Release parameter measured in ms. Convert input amplitude into a smooth control signal for dynamics and envelope-driven modulation.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"sensitivity", 3, "Sensitivity", "×", "Sensitivity parameter measured in ×. Convert input amplitude into a smooth control signal for dynamics and envelope-driven modulation.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"invert", 4, "Invert", "", "Invert parameter. Convert input amplitude into a smooth control signal for dynamics and envelope-driven modulation.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor filter_cv_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "filter_cv";
    d.label = "Filter CV";
    d.description = "Modulate resonant lowpass cutoff from a graph control signal for auto-wah and animated filtering.";
    d.realizations = {
        {"default", "forge_lofi_filter_cv"},
    };
    d.params = {
        {"cutoff_hz", 1, "Cutoff", "Hz", "Cutoff parameter measured in Hz. Modulate resonant lowpass cutoff from a graph control signal for auto-wah and animated filtering.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"amount_oct", 2, "Amount Oct", "oct", "Amount Oct parameter measured in oct. Modulate resonant lowpass cutoff from a graph control signal for auto-wah and animated filtering.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"resonance", 3, "Resonance", "Q", "Resonance parameter measured in Q. Modulate resonant lowpass cutoff from a graph control signal for auto-wah and animated filtering.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor delay_cv_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "delay_cv";
    d.label = "Delay CV";
    d.description = "Modulate delay time from a graph control signal for chorus, vibrato, and moving echoes.";
    d.realizations = {
        {"default", "forge_lofi_delay_cv"},
    };
    d.params = {
        {"time_ms", 1, "Time", "ms", "Time parameter measured in ms. Modulate delay time from a graph control signal for chorus, vibrato, and moving echoes.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"depth_ms", 2, "Depth", "ms", "Depth parameter measured in ms. Modulate delay time from a graph control signal for chorus, vibrato, and moving echoes.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"feedback", 3, "Feedback", "%", "Feedback parameter measured in %. Modulate delay time from a graph control signal for chorus, vibrato, and moving echoes.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mix", 4, "Mix", "%", "Mix parameter measured in %. Modulate delay time from a graph control signal for chorus, vibrato, and moving echoes.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor auto_pan_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "auto_pan";
    d.label = "Auto Pan";
    d.description = "Sweep signal energy across the stereo image with equal-power periodic motion.";
    d.realizations = {
        {"default", "forge_lofi_auto_pan"},
    };
    d.params = {
        {"rate_hz", 1, "Rate", "Hz", "Rate parameter measured in Hz. Sweep signal energy across the stereo image with equal-power periodic motion.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"depth", 2, "Depth", "%", "Depth parameter measured in %. Sweep signal energy across the stereo image with equal-power periodic motion.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"shape", 3, "Shape", "", "Shape parameter. Sweep signal energy across the stereo image with equal-power periodic motion.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"sine", "Sine", 0.0f}, {"triangle", "Triangle", 1.0f}, {"saw", "Saw", 2.0f}, {"square", "Square", 3.0f}}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor width_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "width";
    d.label = "Width";
    d.description = "Narrow to mono or widen a stereo signal through mid-side scaling.";
    d.realizations = {
        {"default", "forge_lofi_width"},
    };
    d.params = {
        {"width", 1, "Width", "×", "Width parameter measured in ×. Narrow to mono or widen a stereo signal through mid-side scaling.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor phaser_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "phaser";
    d.label = "Phaser";
    d.description = "Create classic moving notches with an internally modulated feedback allpass chain.";
    d.realizations = {
        {"default", "forge_lofi_phaser"},
    };
    d.params = {
        {"rate_hz", 1, "Rate", "Hz", "Rate parameter measured in Hz. Create classic moving notches with an internally modulated feedback allpass chain.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"depth", 2, "Depth", "%", "Depth parameter measured in %. Create classic moving notches with an internally modulated feedback allpass chain.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"feedback", 3, "Feedback", "%", "Feedback parameter measured in %. Create classic moving notches with an internally modulated feedback allpass chain.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"stages", 4, "Stages", "", "Stages parameter. Create classic moving notches with an internally modulated feedback allpass chain.", ForgeParamKind::stepped, ForgeParamCurve::linear, forge_integer_choices(2, 8), {}},
    };
    return d;
}

inline std::vector<ForgeNodeDescriptor> lofi_descriptors() {
    return {
        delay_line_descriptor(),
        svf_descriptor(),
        waveshaper_descriptor(),
        dry_wet_mixer_descriptor(),
        noise_descriptor(),
        bitcrush_descriptor(),
        trim_descriptor(),
        ping_pong_delay_descriptor(),
        reverb_descriptor(),
        compressor_descriptor(),
        gate_descriptor(),
        lfo_descriptor(),
        vca_descriptor(),
        env_follower_descriptor(),
        filter_cv_descriptor(),
        delay_cv_descriptor(),
        auto_pan_descriptor(),
        width_descriptor(),
        phaser_descriptor(),
    };
}

}  // namespace pulp::host::forge_lofi
