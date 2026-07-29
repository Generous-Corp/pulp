#pragma once

#include <pulp/host/forge_effect_modulation_catalog.hpp>
#include <pulp/host/forge_param_descriptor.hpp>

#include <array>
#include <string>

namespace pulp::host::modulation {

inline ForgeNodeDescriptor frequency_shifter_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "frequency_shifter";
    d.label = "Frequency Shifter";
    d.description = "True-stereo single-sideband frequency shifter with delayed feedback.";
    d.realizations = {{"default", kSsbFrequencyShifterTypeId}};
    d.params = {
        {"shift_hz", kShiftHz, "Shift", "Hz",
         "Signed frequency translation; unlike pitch shifting, every partial moves "
         "by the same number of hertz.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"feedback", kFeedback, "Feedback", "%", "Amount of shifted output returned to the input.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"feedback_delay_ms", kFeedbackDelayMs, "Feedback Delay", "ms",
         "Delay within the frequency-shifted feedback loop.", ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic},
        {"mix", kMix, "Mix", "%", "Blend between dry and shifted signals.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"shift_mode",
         kShiftMode,
         "Mode",
         "",
         "Sideband direction and stereo routing.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"up", "Up", kModeUp},
          {"down", "Down", kModeDown},
          {"dual_mono", "Dual Mono", kModeDualMono},
          {"stereo_split", "Stereo Split", kModeStereoSplit}}},
        {"stereo_spread", kStereoSpread, "Stereo Spread", "%",
         "Separation of the left and right frequency shifts.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
    };
    return d;
}

}  // namespace pulp::host::modulation

namespace pulp::host::modulation::chorus {

inline ForgeNodeDescriptor chorus_descriptor() {
    static const std::array<std::string, 12> ids = {
        chorus_type_id(Voicing::ce2, JunoMode::mode_I, false),
        chorus_type_id(Voicing::ce2, JunoMode::mode_I, true),
        chorus_type_id(Voicing::juno_ensemble, JunoMode::mode_I, false),
        chorus_type_id(Voicing::juno_ensemble, JunoMode::mode_I, true),
        chorus_type_id(Voicing::juno_ensemble, JunoMode::mode_II, false),
        chorus_type_id(Voicing::juno_ensemble, JunoMode::mode_II, true),
        chorus_type_id(Voicing::juno_ensemble, JunoMode::mode_I_plus_II, false),
        chorus_type_id(Voicing::juno_ensemble, JunoMode::mode_I_plus_II, true),
        chorus_type_id(Voicing::dimension_d, JunoMode::mode_I, false),
        chorus_type_id(Voicing::dimension_d, JunoMode::mode_I, true),
        chorus_type_id(Voicing::tri_chorus, JunoMode::mode_I, false),
        chorus_type_id(Voicing::tri_chorus, JunoMode::mode_I, true),
    };
    ForgeNodeDescriptor d;
    d.key = "chorus";
    d.label = "Chorus";
    d.description = "Stereo ensemble chorus spanning compact pedal, Juno, Dimension D, and "
                    "three-voice architectures.";
    d.axes = {
        {"voicing",
         "Voicing",
         "Voice count and stereo recombination architecture.",
         {{"ce2", "CE-2", 0.0f},
          {"juno_i", "Juno I", 1.0f},
          {"juno_ii", "Juno II", 2.0f},
          {"juno_i_ii", "Juno I+II", 3.0f},
          {"dimension_d", "Dimension D", 4.0f},
          {"tri_chorus", "Tri Chorus", 5.0f}}},
        {"bbd_color",
         "BBD Color",
         "Adds companding, clock jitter, and bucket-brigade coloration.",
         {{"clean", "Clean", 0.0f}, {"bbd", "BBD", 1.0f}}},
    };
    d.realizations = {
        {"ce2_clean", ids[0], {{"voicing", "ce2"}, {"bbd_color", "clean"}}},
        {"ce2_bbd", ids[1], {{"voicing", "ce2"}, {"bbd_color", "bbd"}}},
        {"juno_i_clean", ids[2], {{"voicing", "juno_i"}, {"bbd_color", "clean"}}},
        {"juno_i_bbd", ids[3], {{"voicing", "juno_i"}, {"bbd_color", "bbd"}}},
        {"juno_ii_clean", ids[4], {{"voicing", "juno_ii"}, {"bbd_color", "clean"}}},
        {"juno_ii_bbd", ids[5], {{"voicing", "juno_ii"}, {"bbd_color", "bbd"}}},
        {"juno_i_ii_clean", ids[6], {{"voicing", "juno_i_ii"}, {"bbd_color", "clean"}}},
        {"juno_i_ii_bbd", ids[7], {{"voicing", "juno_i_ii"}, {"bbd_color", "bbd"}}},
        {"dimension_d_clean", ids[8],
         {{"voicing", "dimension_d"}, {"bbd_color", "clean"}}},
        {"dimension_d_bbd", ids[9], {{"voicing", "dimension_d"}, {"bbd_color", "bbd"}}},
        {"tri_chorus_clean", ids[10],
         {{"voicing", "tri_chorus"}, {"bbd_color", "clean"}}},
        {"tri_chorus_bbd", ids[11], {{"voicing", "tri_chorus"}, {"bbd_color", "bbd"}}},
    };
    d.params = {
        {"rate_hz", kRateHz, "Rate", "Hz", "Chorus modulation rate.", ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic},
        {"depth", kDepth, "Depth", "%", "Chorus modulation depth.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"mix", kMix, "Mix", "%", "Blend between dry and chorused signals.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"stereo_width", kStereoWidth, "Stereo Width", "%",
         "Separation of the ensemble voices across the stereo field.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
    };
    return d;
}

}  // namespace pulp::host::modulation::chorus

namespace pulp::host::modulation::phaser {

inline ForgeNodeDescriptor phaser_descriptor() {
    static const std::array<std::string, 5> ids = {
        phaser_type_id(4),  phaser_type_id(6),  phaser_type_id(8),
        phaser_type_id(10), phaser_type_id(12),
    };
    ForgeNodeDescriptor d;
    d.key = "phaser_stages";
    d.label = "Phaser";
    d.description = "Stereo swept all-pass phaser with selectable even stage counts.";
    d.axes = {{"stages",
               "Stages",
               "Number of all-pass stages, which sets the notch count.",
               {{"four", "4", 4.0f},
                {"six", "6", 6.0f},
                {"eight", "8", 8.0f},
                {"ten", "10", 10.0f},
                {"twelve", "12", 12.0f}}}};
    d.realizations = {
        {"four", ids[0], {{"stages", "four"}}},
        {"six", ids[1], {{"stages", "six"}}},
        {"eight", ids[2], {{"stages", "eight"}}},
        {"ten", ids[3], {{"stages", "ten"}}},
        {"twelve", ids[4], {{"stages", "twelve"}}}};
    d.params = {
        {"rate_hz", kRateHz, "Rate", "Hz", "Notch-sweep rate.", ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic},
        {"depth", kDepth, "Depth", "%", "Sweep depth.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"center_hz", kCenterHz, "Center", "Hz", "Center of the swept frequency range.",
         ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
        {"feedback", kFeedback, "Feedback", "%",
         "Signed all-pass feedback that sharpens and colors the notches.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"mix", kMix, "Mix", "%", "Blend between dry and phase-shifted signals.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"stereo_spread", kStereoSpread, "Stereo Spread", "cycle",
         "Phase offset between left and right modulation.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"stagger_ratio", kStaggerRatio, "Stagger", "x",
         "Spacing ratio between successive all-pass corners.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"wave",
         kWave,
         "Wave",
         "",
         "Modulation waveform.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"sine", "Sine", kWaveSine},
          {"triangle", "Triangle", kWaveTriangle},
          {"saw_up", "Saw Up", kWaveSawUp},
          {"saw_down", "Saw Down", kWaveSawDown},
          {"square", "Square", kWaveSquare},
          {"sample_hold", "Sample & Hold", kWaveSampleHold},
          {"smooth_random", "Smooth Random", kWaveSmoothRandom}}},
    };
    return d;
}

}  // namespace pulp::host::modulation::phaser

namespace pulp::host::modulation::vibrato::delay_line {

inline ForgeNodeDescriptor delay_vibrato_descriptor() {
    static const std::string id = delay_vibrato_type_id(4.0f);
    ForgeNodeDescriptor d;
    d.key = "delay_vibrato";
    d.label = "Delay Vibrato";
    d.description = "True pitch vibrato produced by a continuously moving delay tap.";
    d.realizations = {{"default", id}};
    d.params = {
        {"rate_hz", kRateHz, "Rate", "Hz", "Pitch-modulation rate.", ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic},
        {"depth_cents", kDepthCents, "Depth", "cent", "Peak pitch excursion.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"delay_ms", kDelayMs, "Onset Delay", "ms", "Wait before vibrato begins.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"fade_in_ms", kFadeInMs, "Fade In", "ms", "Time for vibrato depth to reach its target.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
    };
    return d;
}

}  // namespace pulp::host::modulation::vibrato::delay_line

namespace pulp::host::modulation::vibrato::phase {

inline ForgeNodeDescriptor phase_vibrato_descriptor() {
    static const std::array<std::string, 4> ids = {
        phase_vibrato_type_id(1),
        phase_vibrato_type_id(2),
        phase_vibrato_type_id(3),
        phase_vibrato_type_id(4),
    };
    ForgeNodeDescriptor d;
    d.key = "phase_vibrato";
    d.label = "Phase Vibrato";
    d.description = "Magnatone-style pitch animation from a swept all-pass cascade.";
    d.axes = {
        {"stages",
         "Stages",
         "Length of the all-pass cascade.",
         {{"one", "1", 1.0f}, {"two", "2", 2.0f}, {"three", "3", 3.0f}, {"four", "4", 4.0f}}}};
    d.realizations = {{"one", ids[0], {{"stages", "one"}}},
                      {"two", ids[1], {{"stages", "two"}}},
                      {"three", ids[2], {{"stages", "three"}}},
                      {"four", ids[3], {{"stages", "four"}}}};
    d.params = {
        {"rate_hz", kRateHz, "Rate", "Hz", "Phase-sweep rate.", ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic},
        {"depth", kDepth, "Depth", "%", "Phase-sweep depth.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"center_hz", kCenterHz, "Center", "Hz", "Center frequency of the swept all-pass corners.",
         ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
        {"mix", kMix, "Mix", "%", "Blend between dry and phase-shifted signals.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
    };
    return d;
}

}  // namespace pulp::host::modulation::vibrato::phase

namespace pulp::host::modulation::vibrato::univibe {

inline ForgeNodeDescriptor univibe_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "univibe";
    d.label = "Univibe";
    d.description = "Lamp-and-photocell four-stage modulation with vibrato and chorus modes.";
    d.realizations = {{"default", kTypeId}};
    d.params = {
        {"rate_hz", kRateHz, "Rate", "Hz", "Lamp modulation rate.", ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic},
        {"depth", kDepth, "Depth", "%", "Photocell sweep depth.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"mode",
         kMode,
         "Mode",
         "",
         "Vibrato is phase-shifted only; chorus mixes it with the direct path.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"vibrato", "Vibrato", kModeVibrato}, {"chorus", "Chorus", kModeChorus}}},
    };
    return d;
}

}  // namespace pulp::host::modulation::vibrato::univibe
