#pragma once

#include <pulp/host/forge_lofi_catalog.hpp>
#include <pulp/host/forge_param_descriptor.hpp>

#include <string_view>
#include <utility>
#include <vector>

namespace pulp::host::forge_lofi {

namespace descriptor_detail {

inline ForgeNodeDescriptor single(std::string_view key, std::string_view label,
                                  std::string_view description, std::string_view type_id,
                                  std::vector<ForgeParamDescriptor> params) {
    return {key, label, description, {}, {{"default", type_id}}, std::move(params)};
}

inline std::vector<ForgeParamChoice> switch_choices() {
    return {{"off", "Off", 0.0f}, {"on", "On", 1.0f}};
}

inline std::vector<ForgeParamChoice> lfo_shape_choices() {
    return {{"sine", "Sine", 0.0f},
            {"triangle", "Triangle", 1.0f},
            {"saw", "Saw", 2.0f},
            {"square", "Square", 3.0f}};
}

} // namespace descriptor_detail

inline ForgeNodeDescriptor delay_descriptor() {
    return descriptor_detail::single("lofi_delay", "Lo-Fi Delay",
                                     "Simple feedback delay for compact lo-fi patches.",
                                     kDelayTypeId,
                                     {{"time_ms", kDelayTimeMs, "Time", "ms", "Delay time.",
                                       ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
                                      {"feedback", kDelayFeedback, "Feedback", "%",
                                       "Amount of delayed output returned to the input.",
                                       ForgeParamKind::continuous, ForgeParamCurve::linear}});
}

inline ForgeNodeDescriptor filter_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "lofi_filter";
    d.label = "Lo-Fi Filter";
    d.description = "State-variable tone filter with low-pass, high-pass, band-pass, and notch "
                    "realizations.";
    d.axes = {{"mode",
               "Mode",
               "Filter response topology.",
               {{"lowpass", "Low-Pass", 0.0f},
                {"highpass", "High-Pass", 1.0f},
                {"bandpass", "Band-Pass", 2.0f},
                {"notch", "Notch", 3.0f}}}};
    d.realizations = {{"lowpass", kFilterTypeId, {{"mode", "lowpass"}}},
                      {"highpass", kFilterHighpassTypeId, {{"mode", "highpass"}}},
                      {"bandpass", kFilterBandpassTypeId, {{"mode", "bandpass"}}},
                      {"notch", kFilterNotchTypeId, {{"mode", "notch"}}}};
    d.params = {
        {"cutoff_hz", kFilterCutoffHz, "Cutoff", "Hz",
         "Cutoff or center frequency of the selected response.", ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic},
        {"resonance", kFilterResonance, "Resonance", "Q",
         "Emphasis and selectivity around the cutoff frequency.", ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic},
    };
    return d;
}

inline ForgeNodeDescriptor waveshaper_descriptor() {
    return descriptor_detail::single(
        "lofi_waveshaper", "Lo-Fi Waveshaper",
        "Tanh saturation for adding progressively denser harmonics.", kWaveshaperTypeId,
        {{"drive", kWaveshaperDrive, "Drive", "x", "Linear gain applied into the saturation curve.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic}});
}

inline ForgeNodeDescriptor drywet_descriptor() {
    return descriptor_detail::single(
        "lofi_drywet", "Dry/Wet", "Two-input equal-power dry/wet mixer.", kDryWetTypeId,
        {{"mix", kDryWetMix, "Mix", "%", "Blend from the dry input to the wet input.",
          ForgeParamKind::continuous, ForgeParamCurve::linear}});
}

inline ForgeNodeDescriptor noise_descriptor() {
    return descriptor_detail::single(
        "lofi_noise", "Noise", "Deterministic white-noise generator.", kNoiseTypeId,
        {{"level", kNoiseLevel, "Level", "%", "Amplitude of the generated noise.",
          ForgeParamKind::continuous, ForgeParamCurve::linear}});
}

inline ForgeNodeDescriptor bitcrush_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "lofi_bitcrush";
    d.label = "Bit Crusher";
    d.description = "Reduces amplitude resolution and sample-update rate.";
    d.axes = {{"quantization", "Quantization",
               "Fixed output-correctness policy selected when the node is built.",
               {{"legacy", "Legacy", 0.0f},
                {"tpdf", "TPDF", 1.0f},
                {"tpdf_first", "TPDF + First-Order Shaping", 2.0f},
                {"tpdf_second", "TPDF + Second-Order Shaping", 3.0f}}}};
    d.realizations = {{"legacy", kBitcrushTypeId, {{"quantization", "legacy"}}},
                      {"tpdf", kBitcrushTpdfTypeId, {{"quantization", "tpdf"}}},
                      {"tpdf_first", kBitcrushTpdfFirstTypeId,
                       {{"quantization", "tpdf_first"}}},
                      {"tpdf_second", kBitcrushTpdfSecondTypeId,
                       {{"quantization", "tpdf_second"}}}};
    d.params = {{"bit_depth", kBitcrushBitDepth, "Bit Depth", "bit",
                 "Number of quantization bits retained.", ForgeParamKind::continuous,
                 ForgeParamCurve::linear},
                {"rate_division", kBitcrushRateDiv, "Rate Division", "x",
                 "Number of input samples held for each reduced-rate output sample.",
                 ForgeParamKind::continuous, ForgeParamCurve::logarithmic}};
    return d;
}

inline ForgeNodeDescriptor trim_descriptor() {
    return descriptor_detail::single(
        "lofi_trim", "Trim", "Transparent gain trim.", kTrimTypeId,
        {{"gain_db", kTrimGainDb, "Gain", "dB", "Output gain adjustment.",
          ForgeParamKind::continuous, ForgeParamCurve::linear}});
}

inline ForgeNodeDescriptor ping_pong_descriptor() {
    return descriptor_detail::single(
        "lofi_ping_pong", "Ping-Pong Delay",
        "Stereo cross-feedback delay that alternates echoes between channels.", kPingPongTypeId,
        {{"time_ms", kPingPongTimeMs, "Time", "ms", "Delay time.", ForgeParamKind::continuous,
          ForgeParamCurve::logarithmic},
         {"feedback", kPingPongFeedback, "Feedback", "%",
          "Amount of delayed output returned through the cross-channel loop.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"width", kPingPongWidth, "Width", "%", "Stereo separation of the alternating echoes.",
          ForgeParamKind::continuous, ForgeParamCurve::linear}});
}

inline ForgeNodeDescriptor reverb_descriptor() {
    return descriptor_detail::single(
        "lofi_reverb", "Lo-Fi Reverb", "Compact colored reverb for short rooms and smeared tails.",
        kReverbTypeId,
        {{"decay", kReverbDecay, "Decay", "s", "Reverberation decay time.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"damping", kReverbDamping, "Damping", "%", "High-frequency loss inside the reverb.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"mix", kReverbMix, "Mix", "%", "Blend between dry input and reverb output.",
          ForgeParamKind::continuous, ForgeParamCurve::linear}});
}

inline ForgeNodeDescriptor compressor_descriptor() {
    return descriptor_detail::single(
        "lofi_compressor", "Lo-Fi Compressor",
        "Compact dynamics compressor for controlling peaks and adding density.", kCompressorTypeId,
        {{"threshold_db", kCompThresholdDb, "Threshold", "dB",
          "Level above which gain reduction begins.", ForgeParamKind::continuous,
          ForgeParamCurve::linear},
         {"ratio", kCompRatio, "Ratio", ":1", "Gain-reduction ratio above the threshold.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"attack_ms", kCompAttackMs, "Attack", "ms", "Time for compression to engage.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"release_ms", kCompReleaseMs, "Release", "ms", "Time for compression to recover.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic}});
}

inline ForgeNodeDescriptor gate_descriptor() {
    return descriptor_detail::single(
        "lofi_gate", "Lo-Fi Gate", "Level gate with explicit attack, hold, and release timing.",
        kGateTypeId,
        {{"threshold_db", kGateThresholdDb, "Threshold", "dB", "Level below which the gate closes.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"attack_ms", kGateAttackMs, "Attack", "ms", "Time for the gate to open.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"hold_ms", kGateHoldMs, "Hold", "ms",
          "Minimum time the gate remains open after the signal falls.", ForgeParamKind::continuous,
          ForgeParamCurve::linear},
         {"release_ms", kGateReleaseMs, "Release", "ms", "Time for the gate to close.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic}});
}

inline ForgeNodeDescriptor lfo_descriptor() {
    return descriptor_detail::single(
        "lofi_lfo", "LFO", "Low-frequency modulation source.", kLfoTypeId,
        {{"rate_hz", kLfoRateHz, "Rate", "Hz", "Oscillation rate.", ForgeParamKind::continuous,
          ForgeParamCurve::logarithmic},
         {"depth", kLfoDepth, "Depth", "%", "Output modulation amplitude.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"shape", kLfoShape, "Shape", "", "Oscillator waveform.", ForgeParamKind::stepped,
          ForgeParamCurve::linear, descriptor_detail::lfo_shape_choices()}});
}

inline ForgeNodeDescriptor vca_descriptor() {
    return descriptor_detail::single(
        "lofi_vca", "VCA", "Control-voltage-scaled amplifier.", kVcaTypeId,
        {{"gain", kVcaGain, "Gain", "%", "Base gain multiplied by the control input.",
          ForgeParamKind::continuous, ForgeParamCurve::linear}});
}

inline ForgeNodeDescriptor env_follower_descriptor() {
    return descriptor_detail::single(
        "lofi_env_follower", "Envelope Follower",
        "Extracts a smooth control envelope from an audio signal.", kEnvFollowerTypeId,
        {{"attack_ms", kEnvAttackMs, "Attack", "ms", "Rise time of the detected envelope.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"release_ms", kEnvReleaseMs, "Release", "ms", "Fall time of the detected envelope.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"sensitivity", kEnvSensitivity, "Sensitivity", "x",
          "Gain applied before envelope detection.", ForgeParamKind::continuous,
          ForgeParamCurve::logarithmic},
         {"invert", kEnvInvert, "Invert", "", "Inverts the envelope for ducking-style control.",
          ForgeParamKind::stepped, ForgeParamCurve::linear, descriptor_detail::switch_choices()}});
}

inline ForgeNodeDescriptor filter_cv_descriptor() {
    return descriptor_detail::single("lofi_filter_cv", "CV Filter",
                                     "Low-pass filter whose cutoff is driven by a control input.",
                                     kFilterCvTypeId,
                                     {{"base_hz", kFilterCvBaseHz, "Base Cutoff", "Hz",
                                       "Cutoff frequency before control-voltage modulation.",
                                       ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
                                      {"amount_oct", kFilterCvAmountOct, "CV Amount", "oct",
                                       "Cutoff modulation depth in octaves.",
                                       ForgeParamKind::continuous, ForgeParamCurve::linear},
                                      {"resonance", kFilterCvResonance, "Resonance", "Q",
                                       "Emphasis around the modulated cutoff.",
                                       ForgeParamKind::continuous, ForgeParamCurve::logarithmic}});
}

inline ForgeNodeDescriptor delay_cv_descriptor() {
    return descriptor_detail::single(
        "lofi_delay_cv", "CV Delay", "Short modulated delay driven by a control input.",
        kDelayCvTypeId,
        {{"base_ms", kDelayCvBaseMs, "Base Time", "ms",
          "Delay time before control-voltage modulation.", ForgeParamKind::continuous,
          ForgeParamCurve::logarithmic},
         {"depth_ms", kDelayCvDepthMs, "CV Depth", "ms", "Delay-time modulation depth.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"feedback", kDelayCvFeedback, "Feedback", "%",
          "Amount of delayed output returned to the input.", ForgeParamKind::continuous,
          ForgeParamCurve::linear},
         {"mix", kDelayCvMix, "Mix", "%", "Blend between dry and delayed signals.",
          ForgeParamKind::continuous, ForgeParamCurve::linear}});
}

inline ForgeNodeDescriptor auto_pan_descriptor() {
    return descriptor_detail::single(
        "lofi_auto_pan", "Auto Pan",
        "Stereo panner animated by an internal low-frequency oscillator.", kAutoPanTypeId,
        {{"rate_hz", kAutoPanRateHz, "Rate", "Hz", "Panning oscillation rate.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"depth", kAutoPanDepth, "Depth", "%", "Width of the panning motion.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"shape", kAutoPanShape, "Shape", "", "Panning oscillator waveform.",
          ForgeParamKind::stepped, ForgeParamCurve::linear,
          descriptor_detail::lfo_shape_choices()}});
}

inline ForgeNodeDescriptor width_descriptor() {
    return descriptor_detail::single(
        "lofi_width", "Stereo Width",
        "Mid/side stereo-width control from mono through exaggerated width.", kWidthTypeId,
        {{"width", kWidthAmount, "Width", "x", "Gain applied to the stereo side component.",
          ForgeParamKind::continuous, ForgeParamCurve::linear}});
}

inline ForgeNodeDescriptor phaser_descriptor() {
    return descriptor_detail::single(
        "lofi_phaser", "Lo-Fi Phaser",
        "Feedback all-pass phaser with a variable number of moving notches.", kPhaserTypeId,
        {{"rate_hz", kPhaserRateHz, "Rate", "Hz", "Notch-sweep rate.", ForgeParamKind::continuous,
          ForgeParamCurve::logarithmic},
         {"depth", kPhaserDepth, "Depth", "%", "Notch-sweep depth.", ForgeParamKind::continuous,
          ForgeParamCurve::linear},
         {"feedback", kPhaserFeedback, "Feedback", "%",
          "All-pass feedback that sharpens the notches.", ForgeParamKind::continuous,
          ForgeParamCurve::linear},
         {"stages",
          kPhaserStages,
          "Stages",
          "",
          "Number of all-pass stages.",
          ForgeParamKind::stepped,
          ForgeParamCurve::linear,
          {{"two", "2", 2.0f},
           {"three", "3", 3.0f},
           {"four", "4", 4.0f},
           {"five", "5", 5.0f},
           {"six", "6", 6.0f},
           {"seven", "7", 7.0f},
           {"eight", "8", 8.0f}}}});
}

}  // namespace pulp::host::forge_lofi
