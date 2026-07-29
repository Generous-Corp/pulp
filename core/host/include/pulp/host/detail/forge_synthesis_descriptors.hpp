#pragma once

// Semantic vocabulary transcribed from Forge's catalog-owned registry.

#include <pulp/host/forge_param_descriptor.hpp>

#include <vector>

namespace pulp::host::synthesis {

inline ForgeNodeDescriptor additive_bank_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "additive_bank";
    d.label = "Additive Bank";
    d.description = "Synthesize organ-like or bell-like additive tones from control or trigger input.";
    d.axes = {{"mode", "Mode", "Finite construction modes for Additive Bank.", {{"organ", "Organ", 0.0f}, {"bell", "Bell", 1.0f}}}};
    d.realizations = {
        {"organ", "synthesis.additive_organ"},
        {"bell", "synthesis.additive_bell"},
    };
    d.params = {
        {"fundamental_hz", 1, "Fundamental", "Hz", "Fundamental parameter measured in Hz. Synthesize organ-like or bell-like additive tones from control or trigger input.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"partial_count", 2, "Partial Count", "", "Partial Count parameter. Synthesize organ-like or bell-like additive tones from control or trigger input.", ForgeParamKind::stepped, ForgeParamCurve::linear, forge_integer_choices(1, 128), {}},
        {"inharmonicity", 3, "Inharmonicity", "", "Inharmonicity parameter. Synthesize organ-like or bell-like additive tones from control or trigger input.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {"organ"}},
        {"spectral_tilt", 4, "Spectral Tilt", "dB/oct", "Spectral Tilt parameter measured in dB/oct. Synthesize organ-like or bell-like additive tones from control or trigger input.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"master_gain_db", 5, "Master Gain", "dB", "Master Gain parameter measured in dB. Synthesize organ-like or bell-like additive tones from control or trigger input.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"envelope_mode", 6, "Envelope Mode", "", "Envelope Mode parameter. Synthesize organ-like or bell-like additive tones from control or trigger input.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"shared_ar", "Shared AR", 0.0f}, {"per_partial_decay", "Per-Partial Decay", 1.0f}}, {}},
        {"retrig_phase", 7, "Retrig Phase", "", "Retrig Phase parameter. Synthesize organ-like or bell-like additive tones from control or trigger input.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"hard_reset", "Hard Reset", 0.0f}, {"free_run", "Free Run", 1.0f}, {"seeded_random", "Seeded Random", 2.0f}}, {}},
        {"attack_ms", 8, "Attack", "ms", "Attack parameter measured in ms. Synthesize organ-like or bell-like additive tones from control or trigger input.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"release_ms", 9, "Release", "ms", "Release parameter measured in ms. Synthesize organ-like or bell-like additive tones from control or trigger input.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"detune_cents", 10, "Detune", "cent", "Detune parameter measured in cent. Synthesize organ-like or bell-like additive tones from control or trigger input.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor vocoder_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "vocoder";
    d.label = "Vocoder";
    d.description = "Imprint a modulator's spectral envelope onto an external or internal carrier for vocoded speech and textures.";
    d.realizations = {
        {"default", "synthesis.vocoder"},
    };
    d.params = {
        {"band_count", 1, "Band Count", "", "Band Count parameter. Imprint a modulator's spectral envelope onto an external or internal carrier for vocoded speech and textures.", ForgeParamKind::stepped, ForgeParamCurve::linear, forge_integer_choices(10, 20), {}},
        {"freq_lo_hz", 2, "Freq Lo", "Hz", "Freq Lo parameter measured in Hz. Imprint a modulator's spectral envelope onto an external or internal carrier for vocoded speech and textures.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"freq_hi_hz", 3, "Freq Hi", "Hz", "Freq Hi parameter measured in Hz. Imprint a modulator's spectral envelope onto an external or internal carrier for vocoded speech and textures.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"carrier_source", 4, "Carrier Source", "", "Carrier Source parameter. Imprint a modulator's spectral envelope onto an external or internal carrier for vocoded speech and textures.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"external", "External", 0.0f}, {"internal", "Internal", 1.0f}}, {}},
        {"internal_wave", 5, "Internal Wave", "", "Internal Wave parameter. Imprint a modulator's spectral envelope onto an external or internal carrier for vocoded speech and textures.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"saw", "Saw", 0.0f}, {"pulse", "Pulse", 1.0f}}, {}},
        {"internal_pw", 6, "Internal Pw", "", "Internal Pw parameter. Imprint a modulator's spectral envelope onto an external or internal carrier for vocoded speech and textures.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"carrier_pitch_hz", 7, "Carrier Pitch", "Hz", "Carrier Pitch parameter measured in Hz. Imprint a modulator's spectral envelope onto an external or internal carrier for vocoded speech and textures.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"noise_mix", 8, "Noise Mix", "", "Noise Mix parameter. Imprint a modulator's spectral envelope onto an external or internal carrier for vocoded speech and textures.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"attack_ms", 9, "Attack", "ms", "Attack parameter measured in ms. Imprint a modulator's spectral envelope onto an external or internal carrier for vocoded speech and textures.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"release_ms", 10, "Release", "ms", "Release parameter measured in ms. Imprint a modulator's spectral envelope onto an external or internal carrier for vocoded speech and textures.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"unvoiced_sensitivity", 11, "Unvoiced Sensitivity", "", "Unvoiced Sensitivity parameter. Imprint a modulator's spectral envelope onto an external or internal carrier for vocoded speech and textures.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"sibilance_mix", 12, "Sibilance Mix", "", "Sibilance Mix parameter. Imprint a modulator's spectral envelope onto an external or internal carrier for vocoded speech and textures.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"formant_shift_st", 13, "Formant Shift St", "st", "Formant Shift St parameter measured in st. Imprint a modulator's spectral envelope onto an external or internal carrier for vocoded speech and textures.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"formant_freeze", 14, "Formant Freeze", "", "Formant Freeze parameter. Imprint a modulator's spectral envelope onto an external or internal carrier for vocoded speech and textures.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"output_trim_db", 15, "Output Trim", "dB", "Output Trim parameter measured in dB. Imprint a modulator's spectral envelope onto an external or internal carrier for vocoded speech and textures.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"dry_wet", 16, "Dry Wet", "", "Dry Wet parameter. Imprint a modulator's spectral envelope onto an external or internal carrier for vocoded speech and textures.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor cyclic_stretch_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "cyclic_stretch";
    d.label = "Cyclic Stretch";
    d.description = "Capture and cyclically time-stretch short or long frames for frozen rhythmic textures.";
    d.axes = {{"mode", "Mode", "Finite construction modes for Cyclic Stretch.", {{"short", "Short", 0.0f}, {"long", "Long", 1.0f}}}};
    d.realizations = {
        {"short", "synthesis.cyclic_stretch_short"},
        {"long", "synthesis.cyclic_stretch_long"},
    };
    d.params = {
        {"stretch", 1, "Stretch", "x", "Stretch parameter measured in x. Capture and cyclically time-stretch short or long frames for frozen rhythmic textures.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"capture_ms", 2, "Capture", "ms", "Capture parameter measured in ms. Capture and cyclically time-stretch short or long frames for frozen rhythmic textures.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"crossfade_shape", 3, "Crossfade Shape", "", "Crossfade Shape parameter. Capture and cyclically time-stretch short or long frames for frozen rhythmic textures.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mix", 4, "Mix", "%", "Mix parameter measured in %. Capture and cyclically time-stretch short or long frames for frozen rhythmic textures.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_db", 5, "Output", "dB", "Output parameter measured in dB. Capture and cyclically time-stretch short or long frames for frozen rhythmic textures.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor granular_live_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "granular_live";
    d.label = "Granular Live";
    d.description = "Resynthesize live input as stereo grains with density, pitch, position, spray, and jitter control.";
    d.realizations = {
        {"default", "synthesis.granular_live"},
    };
    d.params = {
        {"density_hz", 1, "Density", "Hz", "Density parameter measured in Hz. Resynthesize live input as stereo grains with density, pitch, position, spray, and jitter control.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"grain_ms", 2, "Grain", "ms", "Grain parameter measured in ms. Resynthesize live input as stereo grains with density, pitch, position, spray, and jitter control.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"position", 3, "Position", "", "Position parameter. Resynthesize live input as stereo grains with density, pitch, position, spray, and jitter control.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"position_spray_ms", 4, "Position Spray", "ms", "Position Spray parameter measured in ms. Resynthesize live input as stereo grains with density, pitch, position, spray, and jitter control.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pitch_st", 5, "Pitch St", "st", "Pitch St parameter measured in st. Resynthesize live input as stereo grains with density, pitch, position, spray, and jitter control.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pitch_spray_st", 6, "Pitch Spray St", "st", "Pitch Spray St parameter measured in st. Resynthesize live input as stereo grains with density, pitch, position, spray, and jitter control.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pan_spray", 7, "Pan Spray", "", "Pan Spray parameter. Resynthesize live input as stereo grains with density, pitch, position, spray, and jitter control.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"jitter", 8, "Jitter", "", "Jitter parameter. Resynthesize live input as stereo grains with density, pitch, position, spray, and jitter control.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"level_db", 9, "Level", "dB", "Level parameter measured in dB. Resynthesize live input as stereo grains with density, pitch, position, spray, and jitter control.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mix", 10, "Mix", "", "Mix parameter. Resynthesize live input as stereo grains with density, pitch, position, spray, and jitter control.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline std::vector<ForgeNodeDescriptor> synthesis_descriptors() {
    return {
        additive_bank_descriptor(),
        vocoder_descriptor(),
        cyclic_stretch_descriptor(),
        granular_live_descriptor(),
    };
}

}  // namespace pulp::host::synthesis
