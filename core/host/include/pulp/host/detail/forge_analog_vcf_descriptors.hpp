#pragma once

// Semantic vocabulary transcribed from Forge's catalog-owned registry.

#include <pulp/host/forge_param_descriptor.hpp>

#include <vector>

namespace pulp::host::forge_lofi {

inline ForgeNodeDescriptor analog_vcf_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "analog_vcf";
    d.label = "Analog VCF";
    d.description = "Add characterful resonant subtractive filtering in classic analog-synth voicings.";
    d.axes = {{"mode", "Mode", "Finite construction modes for Analog VCF.", {{"juno", "Juno", 0.0f}, {"jupiter", "Jupiter", 1.0f}, {"prophet5", "Prophet5", 2.0f}, {"minimoog", "Minimoog", 3.0f}}}};
    d.realizations = {
        {"juno", "vcf.juno"},
        {"jupiter", "vcf.jupiter"},
        {"prophet5", "vcf.prophet5"},
        {"minimoog", "vcf.minimoog"},
    };
    d.params = {
        {"cutoff", 1, "Cutoff", "%", "Cutoff parameter measured in %. Add characterful resonant subtractive filtering in classic analog-synth voicings.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"cutoff_mod", 2, "Cutoff Mod", "oct", "Cutoff Mod parameter measured in oct. Add characterful resonant subtractive filtering in classic analog-synth voicings.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"resonance", 3, "Resonance", "%", "Resonance parameter measured in %. Add characterful resonant subtractive filtering in classic analog-synth voicings.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"drive", 4, "Drive", "dB", "Drive parameter measured in dB. Add characterful resonant subtractive filtering in classic analog-synth voicings.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline std::vector<ForgeNodeDescriptor> analog_vcf_descriptors() {
    return {
        analog_vcf_descriptor(),
    };
}

}  // namespace pulp::host::forge_lofi
