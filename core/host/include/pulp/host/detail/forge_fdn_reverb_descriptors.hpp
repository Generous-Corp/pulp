#pragma once

// Semantic vocabulary transcribed from Forge's catalog-owned registry.

#include <pulp/host/forge_param_descriptor.hpp>

#include <vector>

namespace pulp::host::forge_fdn {

inline ForgeNodeDescriptor reverb_room_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "reverb_room";
    d.label = "Reverb Room";
    d.description = "Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.";
    d.realizations = {
        {"default", "reverb.room"},
    };
    d.params = {
        {"decay", 1, "Decay", "s", "Decay parameter measured in s. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"size", 2, "Size", "%", "Size parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"predelay", 3, "Predelay", "ms", "Predelay parameter measured in ms. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"damp_hi", 4, "Damp Hi", "%", "Damp Hi parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"damp_lo", 5, "Damp Lo", "%", "Damp Lo parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"diffusion", 6, "Diffusion", "%", "Diffusion parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mod", 7, "Mod", "%", "Mod parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"shimmer", 8, "Shimmer", "%", "Shimmer parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"drive", 9, "Drive", "%", "Drive parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"bloom", 10, "Bloom", "%", "Bloom parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"width", 11, "Width", "%", "Width parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tank_rate", 12, "Tank Rate", "", "Tank Rate parameter. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"16000", "16 kHz", 0.0f}, {"20000", "20 kHz", 1.0f}, {"24000", "24 kHz", 2.0f}, {"32000", "32 kHz", 3.0f}, {"44100", "44.1 kHz", 4.0f}, {"48000", "48 kHz", 5.0f}, {"64000", "64 kHz", 6.0f}, {"96000", "96 kHz", 7.0f}}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor reverb_hall_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "reverb_hall";
    d.label = "Reverb Hall";
    d.description = "Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.";
    d.realizations = {
        {"default", "reverb.hall"},
    };
    d.params = {
        {"decay", 1, "Decay", "s", "Decay parameter measured in s. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"size", 2, "Size", "%", "Size parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"predelay", 3, "Predelay", "ms", "Predelay parameter measured in ms. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"damp_hi", 4, "Damp Hi", "%", "Damp Hi parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"damp_lo", 5, "Damp Lo", "%", "Damp Lo parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"diffusion", 6, "Diffusion", "%", "Diffusion parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mod", 7, "Mod", "%", "Mod parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"shimmer", 8, "Shimmer", "%", "Shimmer parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"drive", 9, "Drive", "%", "Drive parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"bloom", 10, "Bloom", "%", "Bloom parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"width", 11, "Width", "%", "Width parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tank_rate", 12, "Tank Rate", "", "Tank Rate parameter. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"16000", "16 kHz", 0.0f}, {"20000", "20 kHz", 1.0f}, {"24000", "24 kHz", 2.0f}, {"32000", "32 kHz", 3.0f}, {"44100", "44.1 kHz", 4.0f}, {"48000", "48 kHz", 5.0f}, {"64000", "64 kHz", 6.0f}, {"96000", "96 kHz", 7.0f}}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor reverb_galaxy_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "reverb_galaxy";
    d.label = "Reverb Galaxy";
    d.description = "Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.";
    d.realizations = {
        {"default", "reverb.galaxy"},
    };
    d.params = {
        {"decay", 1, "Decay", "s", "Decay parameter measured in s. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"size", 2, "Size", "%", "Size parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"predelay", 3, "Predelay", "ms", "Predelay parameter measured in ms. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"damp_hi", 4, "Damp Hi", "%", "Damp Hi parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"damp_lo", 5, "Damp Lo", "%", "Damp Lo parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"diffusion", 6, "Diffusion", "%", "Diffusion parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mod", 7, "Mod", "%", "Mod parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"shimmer", 8, "Shimmer", "%", "Shimmer parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"drive", 9, "Drive", "%", "Drive parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"bloom", 10, "Bloom", "%", "Bloom parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"width", 11, "Width", "%", "Width parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tank_rate", 12, "Tank Rate", "", "Tank Rate parameter. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"16000", "16 kHz", 0.0f}, {"20000", "20 kHz", 1.0f}, {"24000", "24 kHz", 2.0f}, {"32000", "32 kHz", 3.0f}, {"44100", "44.1 kHz", 4.0f}, {"48000", "48 kHz", 5.0f}, {"64000", "64 kHz", 6.0f}, {"96000", "96 kHz", 7.0f}}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor reverb_shimmer_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "reverb_shimmer";
    d.label = "Reverb Shimmer";
    d.description = "Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.";
    d.realizations = {
        {"default", "reverb.shimmer"},
    };
    d.params = {
        {"decay", 1, "Decay", "s", "Decay parameter measured in s. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"size", 2, "Size", "%", "Size parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"predelay", 3, "Predelay", "ms", "Predelay parameter measured in ms. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"damp_hi", 4, "Damp Hi", "%", "Damp Hi parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"damp_lo", 5, "Damp Lo", "%", "Damp Lo parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"diffusion", 6, "Diffusion", "%", "Diffusion parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mod", 7, "Mod", "%", "Mod parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"shimmer", 8, "Shimmer", "%", "Shimmer parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"drive", 9, "Drive", "%", "Drive parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"bloom", 10, "Bloom", "%", "Bloom parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"width", 11, "Width", "%", "Width parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tank_rate", 12, "Tank Rate", "", "Tank Rate parameter. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"16000", "16 kHz", 0.0f}, {"20000", "20 kHz", 1.0f}, {"24000", "24 kHz", 2.0f}, {"32000", "32 kHz", 3.0f}, {"44100", "44.1 kHz", 4.0f}, {"48000", "48 kHz", 5.0f}, {"64000", "64 kHz", 6.0f}, {"96000", "96 kHz", 7.0f}}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor reverb_lofi_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "reverb_lofi";
    d.label = "Reverb Lofi";
    d.description = "Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.";
    d.realizations = {
        {"default", "reverb.lofi"},
    };
    d.params = {
        {"decay", 1, "Decay", "s", "Decay parameter measured in s. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"size", 2, "Size", "%", "Size parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"predelay", 3, "Predelay", "ms", "Predelay parameter measured in ms. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"damp_hi", 4, "Damp Hi", "%", "Damp Hi parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"damp_lo", 5, "Damp Lo", "%", "Damp Lo parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"diffusion", 6, "Diffusion", "%", "Diffusion parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mod", 7, "Mod", "%", "Mod parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"shimmer", 8, "Shimmer", "%", "Shimmer parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"drive", 9, "Drive", "%", "Drive parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"bloom", 10, "Bloom", "%", "Bloom parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"width", 11, "Width", "%", "Width parameter measured in %. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tank_rate", 12, "Tank Rate", "", "Tank Rate parameter. Create a true-stereo multirate feedback-delay-network reverb with mode-specific space and shared deep shaping controls.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"16000", "16 kHz", 0.0f}, {"20000", "20 kHz", 1.0f}, {"24000", "24 kHz", 2.0f}, {"32000", "32 kHz", 3.0f}, {"44100", "44.1 kHz", 4.0f}, {"48000", "48 kHz", 5.0f}, {"64000", "64 kHz", 6.0f}, {"96000", "96 kHz", 7.0f}}, {}},
    };
    return d;
}

inline std::vector<ForgeNodeDescriptor> fdn_reverb_descriptors() {
    return {
        reverb_room_descriptor(),
        reverb_hall_descriptor(),
        reverb_galaxy_descriptor(),
        reverb_shimmer_descriptor(),
        reverb_lofi_descriptor(),
    };
}

}  // namespace pulp::host::forge_fdn
