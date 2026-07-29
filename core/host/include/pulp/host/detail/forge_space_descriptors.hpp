#pragma once

// Semantic vocabulary transcribed from Forge's catalog-owned registry.

#include <pulp/host/forge_param_descriptor.hpp>

#include <vector>

namespace pulp::host::space {

inline ForgeNodeDescriptor convolution_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "convolution";
    d.label = "Convolution";
    d.description = "Reproduce a measured room, plate, chamber, or hardware response from a bound impulse-response asset.";
    d.realizations = {
        {"default", "space.convolution_reverb"},
    };
    d.params = {
        {"ir_gain_db", 1, "IR Gain", "dB", "IR Gain parameter measured in dB. Reproduce a measured room, plate, chamber, or hardware response from a bound impulse-response asset.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"predelay_ms", 2, "Predelay", "ms", "Predelay parameter measured in ms. Reproduce a measured room, plate, chamber, or hardware response from a bound impulse-response asset.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"wet_pct", 3, "Wet Pct", "%", "Wet Pct parameter measured in %. Reproduce a measured room, plate, chamber, or hardware response from a bound impulse-response asset.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"dry_pct", 4, "Dry Pct", "%", "Dry Pct parameter measured in %. Reproduce a measured room, plate, chamber, or hardware response from a bound impulse-response asset.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"width_pct", 5, "Width Pct", "%", "Width Pct parameter measured in %. Reproduce a measured room, plate, chamber, or hardware response from a bound impulse-response asset.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"lowcut_hz", 6, "Lowcut", "Hz", "Lowcut parameter measured in Hz. Reproduce a measured room, plate, chamber, or hardware response from a bound impulse-response asset.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"highcut_hz", 7, "Highcut", "Hz", "Highcut parameter measured in Hz. Reproduce a measured room, plate, chamber, or hardware response from a bound impulse-response asset.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor nonlin_ambience_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "nonlin_ambience";
    d.label = "Nonlin Ambience";
    d.description = "Create gated, reverse-like, and nonlinear ambience envelopes with vintage converter color.";
    d.realizations = {
        {"default", "space.nonlin_ambience"},
    };
    d.params = {
        {"program", 1, "Program", "", "Program parameter. Create gated, reverse-like, and nonlinear ambience envelopes with vintage converter color.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"ambience", "Ambience", 0.0f}, {"gated", "Gated", 1.0f}, {"reverse", "Reverse", 2.0f}, {"nonlin2", "NonLin 2", 3.0f}}, {}},
        {"length_ms", 2, "Length", "ms", "Length parameter measured in ms. Create gated, reverse-like, and nonlinear ambience envelopes with vintage converter color.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"predelay_ms", 3, "Predelay", "ms", "Predelay parameter measured in ms. Create gated, reverse-like, and nonlinear ambience envelopes with vintage converter color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"density_pct", 4, "Density Pct", "%", "Density Pct parameter measured in %. Create gated, reverse-like, and nonlinear ambience envelopes with vintage converter color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"density_growth", 5, "Density Growth", "", "Density Growth parameter. Create gated, reverse-like, and nonlinear ambience envelopes with vintage converter color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"gate_hold_pct", 6, "Gate Hold Pct", "%", "Gate Hold Pct parameter measured in %. Create gated, reverse-like, and nonlinear ambience envelopes with vintage converter color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"attack_pct", 7, "Attack Pct", "%", "Attack Pct parameter measured in %. Create gated, reverse-like, and nonlinear ambience envelopes with vintage converter color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"diffusion", 8, "Diffusion", "", "Diffusion parameter. Create gated, reverse-like, and nonlinear ambience envelopes with vintage converter color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tone", 9, "Tone", "", "Tone parameter. Create gated, reverse-like, and nonlinear ambience envelopes with vintage converter color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"hf_damp_hz", 10, "Hf Damp", "Hz", "Hf Damp parameter measured in Hz. Create gated, reverse-like, and nonlinear ambience envelopes with vintage converter color.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"width_pct", 11, "Width Pct", "%", "Width Pct parameter measured in %. Create gated, reverse-like, and nonlinear ambience envelopes with vintage converter color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"converter_amount", 12, "Converter Amount", "", "Converter Amount parameter. Create gated, reverse-like, and nonlinear ambience envelopes with vintage converter color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_gain_db", 13, "Output Gain", "dB", "Output Gain parameter measured in dB. Create gated, reverse-like, and nonlinear ambience envelopes with vintage converter color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mix_pct", 14, "Mix Pct", "%", "Mix Pct parameter measured in %. Create gated, reverse-like, and nonlinear ambience envelopes with vintage converter color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor speaker_cabinet_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "speaker_cabinet";
    d.label = "Speaker Cabinet";
    d.description = "Shape tone through a physical speaker, enclosure, breakup, microphone, and diffraction model.";
    d.realizations = {
        {"default", "space.speaker_cabinet"},
    };
    d.params = {
        {"driver", 1, "Driver", "", "Driver parameter. Shape tone through a physical speaker, enclosure, breakup, microphone, and diffraction model.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"brit_12_ceramic", "Brit 12 Ceramic", 0.0f}, {"amer_12_ceramic", "Amer 12 Ceramic", 1.0f}, {"alnico_12", "Alnico 12", 2.0f}, {"brit_10", "Brit 10", 3.0f}, {"bass_15", "Bass 15", 4.0f}}, {}},
        {"box", 2, "Box", "", "Box parameter. Shape tone through a physical speaker, enclosure, breakup, microphone, and diffraction model.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"sealed", "Sealed", 0.0f}, {"open_back", "Open Back", 1.0f}}, {}},
        {"volume_l", 3, "Volume L", "L", "Volume L parameter measured in L. Shape tone through a physical speaker, enclosure, breakup, microphone, and diffraction model.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"resonance_trim_st", 4, "Resonance Trim St", "st", "Resonance Trim St parameter measured in st. Shape tone through a physical speaker, enclosure, breakup, microphone, and diffraction model.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"resonance_q", 5, "Resonance Q", "", "Resonance Q parameter. Shape tone through a physical speaker, enclosure, breakup, microphone, and diffraction model.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"breakup_pct", 6, "Breakup Pct", "%", "Breakup Pct parameter measured in %. Shape tone through a physical speaker, enclosure, breakup, microphone, and diffraction model.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"treble_hz", 7, "Treble", "Hz", "Treble parameter measured in Hz. Shape tone through a physical speaker, enclosure, breakup, microphone, and diffraction model.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"drive_db", 8, "Drive", "dB", "Drive parameter measured in dB. Shape tone through a physical speaker, enclosure, breakup, microphone, and diffraction model.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"compression_pct", 9, "Compression Pct", "%", "Compression Pct parameter measured in %. Shape tone through a physical speaker, enclosure, breakup, microphone, and diffraction model.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mic_distance_cm", 10, "Mic Distance Cm", "cm", "Mic Distance Cm parameter measured in cm. Shape tone through a physical speaker, enclosure, breakup, microphone, and diffraction model.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"mic_position_pct", 11, "Mic Position Pct", "%", "Mic Position Pct parameter measured in %. Shape tone through a physical speaker, enclosure, breakup, microphone, and diffraction model.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mic_axis_deg", 12, "Mic Axis Deg", "deg", "Mic Axis Deg parameter measured in deg. Shape tone through a physical speaker, enclosure, breakup, microphone, and diffraction model.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"diffraction_pct", 13, "Diffraction Pct", "%", "Diffraction Pct parameter measured in %. Shape tone through a physical speaker, enclosure, breakup, microphone, and diffraction model.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_trim_db", 14, "Output Trim", "dB", "Output Trim parameter measured in dB. Shape tone through a physical speaker, enclosure, breakup, microphone, and diffraction model.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline std::vector<ForgeNodeDescriptor> space_descriptors() {
    return {
        convolution_descriptor(),
        nonlin_ambience_descriptor(),
        speaker_cabinet_descriptor(),
    };
}

}  // namespace pulp::host::space
