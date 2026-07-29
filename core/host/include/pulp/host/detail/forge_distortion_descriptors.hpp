#pragma once

// Semantic vocabulary transcribed from Forge's catalog-owned registry.

#include <pulp/host/forge_param_descriptor.hpp>

#include <vector>

namespace pulp::host::distortion {

inline ForgeNodeDescriptor distortion_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "distortion";
    d.label = "Distortion";
    d.description = "Create circuit-style diode clipping with selectable topology, tone shaping, and symmetry.";
    d.axes = {
        {"topology", "Topology", "Diode placement in the clipping circuit.",
         {{"to_ground", "To Ground", 0.0f}, {"in_loop", "In Loop", 1.0f}}},
        {"oversampling", "Oversampling", "Fixed antialiasing tier and latency.",
         {{"x1", "1x", 1.0f}, {"x2", "2x", 2.0f}, {"x4", "4x", 4.0f},
          {"x8", "8x", 8.0f}}},
    };
    d.realizations = {
        {"to_ground_x1", "distortion.to_ground.x1"},
        {"to_ground_x2", "distortion.to_ground.x2"},
        {"to_ground_x4", "distortion.to_ground.x4"},
        {"to_ground_x8", "distortion.to_ground.x8"},
        {"in_loop_x1", "distortion.in_loop.x1"},
        {"in_loop_x2", "distortion.in_loop.x2"},
        {"in_loop_x4", "distortion.in_loop.x4"},
        {"in_loop_x8", "distortion.in_loop.x8"},
    };
    d.params = {
        {"drive_db", 1, "Drive", "dB", "Drive parameter measured in dB. Create circuit-style diode clipping with selectable topology, tone shaping, and symmetry.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"symmetry", 2, "Symmetry", "%", "Symmetry parameter measured in %. Create circuit-style diode clipping with selectable topology, tone shaping, and symmetry.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pre_tone_hz", 4, "Pre Tone", "Hz", "Pre Tone parameter measured in Hz. Create circuit-style diode clipping with selectable topology, tone shaping, and symmetry.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"pre_gain_db", 8, "Pre Gain", "dB", "Pre Gain parameter measured in dB. Create circuit-style diode clipping with selectable topology, tone shaping, and symmetry.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"post_tone_hz", 5, "Post Tone", "Hz", "Post Tone parameter measured in Hz. Create circuit-style diode clipping with selectable topology, tone shaping, and symmetry.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"tone_mix", 6, "Tone Mix", "%", "Tone Mix parameter measured in %. Create circuit-style diode clipping with selectable topology, tone shaping, and symmetry.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_db", 7, "Output", "dB", "Output parameter measured in dB. Create circuit-style diode clipping with selectable topology, tone shaping, and symmetry.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"diode_model", 3, "Diode Model", "", "Diode Model parameter. Create circuit-style diode clipping with selectable topology, tone shaping, and symmetry.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"silicon", "Silicon", 0.0f}, {"germanium", "Germanium", 1.0f}, {"led", "LED", 2.0f}}, {}},
    };
    return d;
}

inline std::vector<ForgeNodeDescriptor> distortion_descriptors() {
    return {
        distortion_descriptor(),
    };
}

}  // namespace pulp::host::distortion
