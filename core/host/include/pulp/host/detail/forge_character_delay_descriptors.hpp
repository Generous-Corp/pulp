#pragma once

// Semantic vocabulary transcribed from Forge's catalog-owned registry.

#include <pulp/host/forge_param_descriptor.hpp>

#include <vector>

namespace pulp::host::character_delay {

inline ForgeNodeDescriptor character_delay_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "character_delay";
    d.label = "Character Delay";
    d.description = "Choose clean, vintage, tape, BBD, or diffused delay characters with modulation and feedback effects.";
    d.axes = {{"mode", "Mode", "Finite construction modes for Character Delay.", {{"clean", "Clean", 0.0f}, {"vintage", "Vintage", 1.0f}, {"tape", "Tape", 2.0f}, {"tape_physical", "Tape Physical", 3.0f}, {"bbd", "BBD", 4.0f}, {"diffusion", "Diffusion", 5.0f}}}};
    d.realizations = {
        {"clean", "delay.clean"},
        {"vintage", "delay.vintage"},
        {"tape", "delay.tape"},
        {"tape_physical", "delay.tape_physical"},
        {"bbd", "delay.bbd"},
        {"diffusion", "delay.diffusion"},
    };
    d.params = {
        {"time_ms", 1, "Time", "ms", "Time parameter measured in ms. Choose clean, vintage, tape, BBD, or diffused delay characters with modulation and feedback effects.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"time_offset", 2, "Time Offset", "×", "Time Offset parameter measured in ×. Choose clean, vintage, tape, BBD, or diffused delay characters with modulation and feedback effects.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"feedback", 3, "Feedback", "%", "Feedback parameter measured in %. Choose clean, vintage, tape, BBD, or diffused delay characters with modulation and feedback effects.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"crossfeed", 4, "Crossfeed", "%", "Crossfeed parameter measured in %. Choose clean, vintage, tape, BBD, or diffused delay characters with modulation and feedback effects.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"character_amount", 5, "Character Amount", "%", "Character Amount parameter measured in %. Choose clean, vintage, tape, BBD, or diffused delay characters with modulation and feedback effects.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mod_rate", 6, "Mod Rate", "%", "Mod Rate parameter measured in %. Choose clean, vintage, tape, BBD, or diffused delay characters with modulation and feedback effects.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mod_depth", 7, "Mod Depth", "%", "Mod Depth parameter measured in %. Choose clean, vintage, tape, BBD, or diffused delay characters with modulation and feedback effects.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"duck", 8, "Duck", "%", "Duck parameter measured in %. Choose clean, vintage, tape, BBD, or diffused delay characters with modulation and feedback effects.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"freeze", 9, "Freeze", "", "Freeze parameter. Choose clean, vintage, tape, BBD, or diffused delay characters with modulation and feedback effects.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"reverse", 10, "Reverse", "", "Reverse parameter. Choose clean, vintage, tape, BBD, or diffused delay characters with modulation and feedback effects.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
    };
    return d;
}

inline std::vector<ForgeNodeDescriptor> character_delay_descriptors() {
    return {
        character_delay_descriptor(),
    };
}

}  // namespace pulp::host::character_delay
