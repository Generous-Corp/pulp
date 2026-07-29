#pragma once

// Semantic vocabulary transcribed from Forge's catalog-owned registry.

#include <pulp/host/forge_param_descriptor.hpp>

#include <vector>

namespace pulp::host::dynamics {

inline ForgeNodeDescriptor compressor_ff_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "compressor_ff";
    d.label = "Compressor Feedforward";
    d.description = "Control stereo dynamics transparently with modern feedforward compression and linked detection.";
    d.axes = {{"mode", "Mode", "Finite construction modes for Compressor Feedforward.", {{"zero_latency", "Zero Latency", 0.0f}, {"lookahead_3ms", "Lookahead 3ms", 1.0f}, {"lookahead_10ms", "Lookahead 10ms", 2.0f}}}};
    d.realizations = {
        {"zero_latency", "dynamics.feedforward_compressor"},
        {"lookahead_3ms", "dynamics.feedforward_compressor.la_4008000000000000"},
        {"lookahead_10ms", "dynamics.feedforward_compressor.la_4024000000000000"},
    };
    d.params = {
        {"threshold_db", 1, "Threshold", "dB", "Threshold parameter measured in dB. Control stereo dynamics transparently with modern feedforward compression and linked detection.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"ratio", 2, "Ratio", ":1", "Ratio parameter measured in :1. Control stereo dynamics transparently with modern feedforward compression and linked detection.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"knee_db", 3, "Knee", "dB", "Knee parameter measured in dB. Control stereo dynamics transparently with modern feedforward compression and linked detection.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"attack_ms", 4, "Attack", "ms", "Attack parameter measured in ms. Control stereo dynamics transparently with modern feedforward compression and linked detection.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"release_ms", 5, "Release", "ms", "Release parameter measured in ms. Control stereo dynamics transparently with modern feedforward compression and linked detection.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"makeup_db", 10, "Makeup", "dB", "Makeup parameter measured in dB. Control stereo dynamics transparently with modern feedforward compression and linked detection.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"stereo_link", 12, "Stereo Link", "%", "Stereo Link parameter measured in %. Control stereo dynamics transparently with modern feedforward compression and linked detection.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"detector_mode", 6, "Detector Mode", "", "Detector Mode parameter. Control stereo dynamics transparently with modern feedforward compression and linked detection.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"peak", "Peak", 0.0f}, {"rms", "RMS", 1.0f}}, {}},
        {"rms_window_ms", 7, "Rms Window", "ms", "Rms Window parameter measured in ms. Control stereo dynamics transparently with modern feedforward compression and linked detection.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"program_dependent", 9, "Program Dependent", "", "Program Dependent parameter. Control stereo dynamics transparently with modern feedforward compression and linked detection.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"auto_makeup", 11, "Auto Makeup", "", "Auto Makeup parameter. Control stereo dynamics transparently with modern feedforward compression and linked detection.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor compressor_vca_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "compressor_vca";
    d.label = "Compressor VCA";
    d.description = "Apply fast, clean VCA compression for bus control, punch, and firm level containment.";
    d.axes = {{"mode", "Mode", "Finite construction modes for Compressor VCA.", {{"zero_latency_k4", "Zero Latency K4", 0.0f}, {"lookahead_3ms_k4", "Lookahead 3ms K4", 1.0f}, {"lookahead_10ms_k4", "Lookahead 10ms K4", 2.0f}, {"zero_latency_k2", "Zero Latency K2", 3.0f}, {"zero_latency_k8", "Zero Latency K8", 4.0f}}}};
    d.realizations = {
        {"zero_latency_k4", "dynamics.vca_compressor"},
        {"lookahead_3ms_k4", "dynamics.vca_compressor.la_4008000000000000.ark_4010000000000000"},
        {"lookahead_10ms_k4", "dynamics.vca_compressor.la_4024000000000000.ark_4010000000000000"},
        {"zero_latency_k2", "dynamics.vca_compressor.la_0000000000000000.ark_4000000000000000"},
        {"zero_latency_k8", "dynamics.vca_compressor.la_0000000000000000.ark_4020000000000000"},
    };
    d.params = {
        {"threshold_db", 1, "Threshold", "dB", "Threshold parameter measured in dB. Apply fast, clean VCA compression for bus control, punch, and firm level containment.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"ratio", 2, "Ratio", ":1", "Ratio parameter measured in :1. Apply fast, clean VCA compression for bus control, punch, and firm level containment.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"knee_db", 3, "Knee", "dB", "Knee parameter measured in dB. Apply fast, clean VCA compression for bus control, punch, and firm level containment.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"time_ms", 4, "Time", "ms", "Time parameter measured in ms. Apply fast, clean VCA compression for bus control, punch, and firm level containment.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"makeup_db", 5, "Makeup", "dB", "Makeup parameter measured in dB. Apply fast, clean VCA compression for bus control, punch, and firm level containment.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mix", 6, "Mix", "%", "Mix parameter measured in %. Apply fast, clean VCA compression for bus control, punch, and firm level containment.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"negative_ratio", 7, "Negative Ratio", "", "Negative Ratio parameter. Apply fast, clean VCA compression for bus control, punch, and firm level containment.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"infinity_plus", "Infinity+", 1.0f}}, {}},
        {"negative_ratio_amount", 8, "Negative Ratio Amount", ":1", "Negative Ratio Amount parameter measured in :1. Apply fast, clean VCA compression for bus control, punch, and firm level containment.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"ceiling_db", 9, "Ceiling", "dB", "Ceiling parameter measured in dB. Apply fast, clean VCA compression for bus control, punch, and firm level containment.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor compressor_fet_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "compressor_fet";
    d.label = "Compressor FET";
    d.description = "Add fast FET compression for assertive transient shaping and energetic color.";
    d.realizations = {
        {"default", "dynamics.fet_compressor"},
    };
    d.params = {
        {"input_gain_db", 1, "Input Gain", "dB", "Input Gain parameter measured in dB. Add fast FET compression for assertive transient shaping and energetic color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_gain_db", 2, "Output Gain", "dB", "Output Gain parameter measured in dB. Add fast FET compression for assertive transient shaping and energetic color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"attack_us", 4, "Attack Us", "us", "Attack Us parameter measured in us. Add fast FET compression for assertive transient shaping and energetic color.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"release_ms", 5, "Release", "ms", "Release parameter measured in ms. Add fast FET compression for assertive transient shaping and energetic color.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"knee_db", 6, "Knee", "dB", "Knee parameter measured in dB. Add fast FET compression for assertive transient shaping and energetic color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"transformer_amount", 7, "Transformer Amount", "%", "Transformer Amount parameter measured in %. Add fast FET compression for assertive transient shaping and energetic color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mix", 8, "Mix", "%", "Mix parameter measured in %. Add fast FET compression for assertive transient shaping and energetic color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"ratio", 3, "Ratio", "", "Ratio parameter. Add fast FET compression for assertive transient shaping and energetic color.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"4_to_1", "4:1", 0.0f}, {"8_to_1", "8:1", 1.0f}, {"12_to_1", "12:1", 2.0f}, {"20_to_1", "20:1", 3.0f}, {"all_buttons_in", "All Buttons In", 4.0f}}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor compressor_diode_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "compressor_diode";
    d.label = "Compressor Diode";
    d.description = "Use diode-bridge compression for weighty, program-dependent dynamics and vintage glue.";
    d.axes = {{"mode", "Mode", "Finite construction modes for Compressor Diode.", {{"feedback", "Feedback", 0.0f}, {"feedforward", "Feedforward", 1.0f}}}};
    d.realizations = {
        {"feedback", "dynamics.diode_bridge_compressor"},
        {"feedforward", "dynamics.diode_bridge_compressor_feedforward"},
    };
    d.params = {
        {"threshold_db", 1, "Threshold", "dB", "Threshold parameter measured in dB. Use diode-bridge compression for weighty, program-dependent dynamics and vintage glue.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"ratio", 2, "Ratio", ":1", "Ratio parameter measured in :1. Use diode-bridge compression for weighty, program-dependent dynamics and vintage glue.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"knee_db", 3, "Knee", "dB", "Knee parameter measured in dB. Use diode-bridge compression for weighty, program-dependent dynamics and vintage glue.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"attack_ms", 4, "Attack", "ms", "Attack parameter measured in ms. Use diode-bridge compression for weighty, program-dependent dynamics and vintage glue.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"release_ms", 5, "Release", "ms", "Release parameter measured in ms. Use diode-bridge compression for weighty, program-dependent dynamics and vintage glue.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"makeup_db", 6, "Makeup", "dB", "Makeup parameter measured in dB. Use diode-bridge compression for weighty, program-dependent dynamics and vintage glue.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"character", 7, "Character", "%", "Character parameter measured in %. Use diode-bridge compression for weighty, program-dependent dynamics and vintage glue.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mix", 8, "Mix", "%", "Mix parameter measured in %. Use diode-bridge compression for weighty, program-dependent dynamics and vintage glue.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"sc_hpf_hz", 9, "Sc Hpf", "Hz", "Sc Hpf parameter measured in Hz. Use diode-bridge compression for weighty, program-dependent dynamics and vintage glue.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"auto_release", 10, "Auto Release", "", "Auto Release parameter. Use diode-bridge compression for weighty, program-dependent dynamics and vintage glue.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
    };
    return d;
}

inline std::vector<ForgeNodeDescriptor> dynamics_descriptors() {
    return {
        compressor_ff_descriptor(),
        compressor_vca_descriptor(),
        compressor_fet_descriptor(),
        compressor_diode_descriptor(),
    };
}

}  // namespace pulp::host::dynamics
