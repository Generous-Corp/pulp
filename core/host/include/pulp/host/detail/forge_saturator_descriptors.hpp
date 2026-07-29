#pragma once

// Semantic vocabulary transcribed from Forge's catalog-owned registry.

#include <pulp/host/forge_param_descriptor.hpp>

#include <vector>

namespace pulp::host::saturator {

inline ForgeNodeDescriptor saturator_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "saturator";
    d.label = "Saturator";
    d.description = "Apply selectable soft-saturation curves for warmth, glue, or aggressive harmonic color.";
    d.axes = {{"mode", "Mode", "Finite construction modes for Saturator.", {{"tanh", "Tanh", 0.0f}, {"atan", "Atan", 1.0f}, {"cubic", "Cubic", 2.0f}, {"sinh_arc", "Sinh Arc", 3.0f}}}};
    d.realizations = {
        {"tanh", "saturator.tanh"},
        {"atan", "saturator.atan"},
        {"cubic", "saturator.cubic"},
        {"sinh_arc", "saturator.sinh_arc"},
    };
    d.params = {
        {"drive_db", 1, "Drive", "dB", "Drive parameter measured in dB. Apply selectable soft-saturation curves for warmth, glue, or aggressive harmonic color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"bias", 2, "Bias", "%", "Bias parameter measured in %. Apply selectable soft-saturation curves for warmth, glue, or aggressive harmonic color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tone_pre_hz", 3, "Tone Pre", "Hz", "Tone Pre parameter measured in Hz. Apply selectable soft-saturation curves for warmth, glue, or aggressive harmonic color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mix", 4, "Mix", "%", "Mix parameter measured in %. Apply selectable soft-saturation curves for warmth, glue, or aggressive harmonic color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_trim_db", 5, "Output Trim", "dB", "Output Trim parameter measured in dB. Apply selectable soft-saturation curves for warmth, glue, or aggressive harmonic color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tone_tracking", 6, "Tone Tracking", "", "Tone Tracking parameter. Apply selectable soft-saturation curves for warmth, glue, or aggressive harmonic color.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"tone_de_hz", 7, "Tone De", "Hz", "Tone De parameter measured in Hz. Apply selectable soft-saturation curves for warmth, glue, or aggressive harmonic color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pre_boost_db", 8, "Pre Boost", "dB", "Pre Boost parameter measured in dB. Apply selectable soft-saturation curves for warmth, glue, or aggressive harmonic color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline std::vector<ForgeNodeDescriptor> saturator_descriptors() {
    return {
        saturator_descriptor(),
    };
}

}  // namespace pulp::host::saturator
