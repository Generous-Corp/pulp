#pragma once

// Semantic vocabulary transcribed from Forge's catalog-owned registry.

#include <pulp/host/forge_param_descriptor.hpp>

#include <vector>

namespace pulp::host::modulation {

inline ForgeNodeDescriptor frequency_shifter_ssb_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "frequency_shifter_ssb";
    d.label = "Frequency Shifter SSB";
    d.description = "Shift every frequency by a fixed amount for metallic, inharmonic, and stereo-spread effects.";
    d.realizations = {
        {"default", "modulation.frequency_shifter_ssb"},
    };
    d.params = {
        {"shift_hz", 1, "Shift", "Hz", "Shift parameter measured in Hz. Shift every frequency by a fixed amount for metallic, inharmonic, and stereo-spread effects.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"feedback", 2, "Feedback", "", "Feedback parameter. Shift every frequency by a fixed amount for metallic, inharmonic, and stereo-spread effects.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"feedback_delay_ms", 3, "Feedback Delay", "ms", "Feedback Delay parameter measured in ms. Shift every frequency by a fixed amount for metallic, inharmonic, and stereo-spread effects.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"mix", 4, "Mix", "%", "Mix parameter measured in %. Shift every frequency by a fixed amount for metallic, inharmonic, and stereo-spread effects.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"shift_mode", 5, "Shift Mode", "", "Shift Mode parameter. Shift every frequency by a fixed amount for metallic, inharmonic, and stereo-spread effects.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"up", "Up", 0.0f}, {"down", "Down", 1.0f}, {"dual_mono", "Dual Mono", 2.0f}, {"stereo_split", "Stereo Split", 3.0f}}, {}},
        {"stereo_spread", 6, "Stereo Spread", "", "Stereo Spread parameter. Shift every frequency by a fixed amount for metallic, inharmonic, and stereo-spread effects.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor chorus_family_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "chorus_family";
    d.label = "Chorus Family";
    d.description = "Create classic CE-2, Juno, Dimension-D, or tri-chorus ensemble width with optional BBD color.";
    d.axes = {{"mode", "Mode", "Finite construction modes for Chorus Family.", {{"ce2", "Ce2", 0.0f}, {"juno_i", "Juno I", 1.0f}, {"juno_ii", "Juno Ii", 2.0f}, {"juno_i_plus_ii", "Juno I Plus Ii", 3.0f}, {"dimension_d", "Dimension D", 4.0f}, {"tri_chorus", "Tri Chorus", 5.0f}, {"ce2_bbd", "Ce2 BBD", 6.0f}, {"juno_i_bbd", "Juno I BBD", 7.0f}, {"juno_ii_bbd", "Juno Ii BBD", 8.0f}, {"juno_i_plus_ii_bbd", "Juno I Plus Ii BBD", 9.0f}, {"dimension_d_bbd", "Dimension D BBD", 10.0f}, {"tri_chorus_bbd", "Tri Chorus BBD", 11.0f}}}};
    d.realizations = {
        {"ce2", "modulation.chorus.ce2"},
        {"juno_i", "modulation.chorus.juno_i"},
        {"juno_ii", "modulation.chorus.juno_ii"},
        {"juno_i_plus_ii", "modulation.chorus.juno_i_plus_ii"},
        {"dimension_d", "modulation.chorus.dimension_d"},
        {"tri_chorus", "modulation.chorus.tri_chorus"},
        {"ce2_bbd", "modulation.chorus.ce2.bbd"},
        {"juno_i_bbd", "modulation.chorus.juno_i.bbd"},
        {"juno_ii_bbd", "modulation.chorus.juno_ii.bbd"},
        {"juno_i_plus_ii_bbd", "modulation.chorus.juno_i_plus_ii.bbd"},
        {"dimension_d_bbd", "modulation.chorus.dimension_d.bbd"},
        {"tri_chorus_bbd", "modulation.chorus.tri_chorus.bbd"},
    };
    d.params = {
        {"rate_hz", 1, "Rate", "Hz", "Rate parameter measured in Hz. Create classic CE-2, Juno, Dimension-D, or tri-chorus ensemble width with optional BBD color.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"depth", 2, "Depth", "%", "Depth parameter measured in %. Create classic CE-2, Juno, Dimension-D, or tri-chorus ensemble width with optional BBD color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mix", 3, "Mix", "%", "Mix parameter measured in %. Create classic CE-2, Juno, Dimension-D, or tri-chorus ensemble width with optional BBD color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"stereo_width", 4, "Stereo Width", "%", "Stereo Width parameter measured in %. Create classic CE-2, Juno, Dimension-D, or tri-chorus ensemble width with optional BBD color.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor phaser_stages_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "phaser_stages";
    d.label = "Phaser Stages";
    d.description = "Create stereo phasing with a selectable allpass-stage count and detailed sweep geometry.";
    d.axes = {{"mode", "Mode", "Finite construction modes for Phaser Stages.", {{"4", "4", 0.0f}, {"6", "6", 1.0f}, {"8", "8", 2.0f}, {"10", "10", 3.0f}, {"12", "12", 4.0f}}}};
    d.realizations = {
        {"4", "modulation.phaser.4"},
        {"6", "modulation.phaser.6"},
        {"8", "modulation.phaser.8"},
        {"10", "modulation.phaser.10"},
        {"12", "modulation.phaser.12"},
    };
    d.params = {
        {"rate_hz", 1, "Rate", "Hz", "Rate parameter measured in Hz. Create stereo phasing with a selectable allpass-stage count and detailed sweep geometry.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"depth", 2, "Depth", "%", "Depth parameter measured in %. Create stereo phasing with a selectable allpass-stage count and detailed sweep geometry.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"center_hz", 3, "Center", "Hz", "Center parameter measured in Hz. Create stereo phasing with a selectable allpass-stage count and detailed sweep geometry.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"feedback", 4, "Feedback", "", "Feedback parameter. Create stereo phasing with a selectable allpass-stage count and detailed sweep geometry.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mix", 5, "Mix", "%", "Mix parameter measured in %. Create stereo phasing with a selectable allpass-stage count and detailed sweep geometry.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"stereo_spread", 6, "Stereo Spread", "", "Stereo Spread parameter. Create stereo phasing with a selectable allpass-stage count and detailed sweep geometry.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"stagger_ratio", 7, "Stagger Ratio", "", "Stagger Ratio parameter. Create stereo phasing with a selectable allpass-stage count and detailed sweep geometry.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"wave", 8, "Wave", "", "Wave parameter. Create stereo phasing with a selectable allpass-stage count and detailed sweep geometry.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"sine", "Sine", 0.0f}, {"triangle", "Triangle", 1.0f}, {"saw_up", "Saw Up", 2.0f}, {"saw_down", "Saw Down", 3.0f}, {"square", "Square", 4.0f}, {"sample_hold", "Sample & Hold", 5.0f}, {"smooth_random", "Smooth Random", 6.0f}}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor vibrato_delay_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "vibrato_delay";
    d.label = "Vibrato Delay";
    d.description = "Create true pitch vibrato through a short modulated delay line.";
    d.axes = {{"mode", "Mode", "Finite construction modes for Vibrato Delay.", {{"4", "4", 0.0f}, {"8", "8", 1.0f}}}};
    d.realizations = {
        {"4", "modulation.vibrato.delay.40"},
        {"8", "modulation.vibrato.delay.80"},
    };
    d.params = {
        {"rate_hz", 1, "Rate", "Hz", "Rate parameter measured in Hz. Create true pitch vibrato through a short modulated delay line.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"depth_cents", 2, "Depth", "cent", "Depth parameter measured in cent. Create true pitch vibrato through a short modulated delay line.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"delay_ms", 3, "Delay", "ms", "Delay parameter measured in ms. Create true pitch vibrato through a short modulated delay line.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fade_in_ms", 4, "Fade In", "ms", "Fade In parameter measured in ms. Create true pitch vibrato through a short modulated delay line.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor vibrato_phase_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "vibrato_phase";
    d.label = "Vibrato Phase";
    d.description = "Create phase-network vibrato with selectable stage depth and resonant motion.";
    d.axes = {{"mode", "Mode", "Finite construction modes for Vibrato Phase.", {{"1", "1", 0.0f}, {"2", "2", 1.0f}, {"3", "3", 2.0f}, {"4", "4", 3.0f}}}};
    d.realizations = {
        {"1", "modulation.vibrato.phase.1"},
        {"2", "modulation.vibrato.phase.2"},
        {"3", "modulation.vibrato.phase.3"},
        {"4", "modulation.vibrato.phase.4"},
    };
    d.params = {
        {"rate_hz", 1, "Rate", "Hz", "Rate parameter measured in Hz. Create phase-network vibrato with selectable stage depth and resonant motion.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"depth", 2, "Depth", "%", "Depth parameter measured in %. Create phase-network vibrato with selectable stage depth and resonant motion.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"center_hz", 3, "Center", "Hz", "Center parameter measured in Hz. Create phase-network vibrato with selectable stage depth and resonant motion.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"mix", 4, "Mix", "%", "Mix parameter measured in %. Create phase-network vibrato with selectable stage depth and resonant motion.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor univibe_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "univibe";
    d.label = "Univibe";
    d.description = "Add asymmetrical lamp-and-photocell-style chorus or vibrato swirl.";
    d.realizations = {
        {"default", "modulation.vibrato.univibe"},
    };
    d.params = {
        {"rate_hz", 1, "Rate", "Hz", "Rate parameter measured in Hz. Add asymmetrical lamp-and-photocell-style chorus or vibrato swirl.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"depth", 2, "Depth", "%", "Depth parameter measured in %. Add asymmetrical lamp-and-photocell-style chorus or vibrato swirl.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mode", 3, "Mode", "", "Mode parameter. Add asymmetrical lamp-and-photocell-style chorus or vibrato swirl.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"vibrato", "Vibrato", 0.0f}, {"chorus", "Chorus", 1.0f}}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor flanger_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "flanger";
    d.label = "Flanger";
    d.description = "Create classic, barber-pole, or through-zero comb sweeps with feedback and stereo spread.";
    d.axes = {{"mode", "Mode", "Finite construction modes for Flanger.", {{"classic", "Classic", 0.0f}, {"barberpole", "Barberpole", 1.0f}, {"through_zero", "Through Zero", 2.0f}, {"through_zero_1ms", "Through Zero 1ms", 3.0f}, {"through_zero_2ms", "Through Zero 2ms", 4.0f}, {"through_zero_8ms", "Through Zero 8ms", 5.0f}}}};
    d.realizations = {
        {"classic", "modulation.flanger"},
        {"barberpole", "modulation.flanger.barberpole"},
        {"through_zero", "modulation.flanger.through_zero.4010000000000000"},
        {"through_zero_1ms", "modulation.flanger.through_zero.3ff0000000000000"},
        {"through_zero_2ms", "modulation.flanger.through_zero.4000000000000000"},
        {"through_zero_8ms", "modulation.flanger.through_zero.4020000000000000"},
    };
    d.params = {
        {"rate_hz", 1, "Rate", "Hz", "Rate parameter measured in Hz. Create classic, barber-pole, or through-zero comb sweeps with feedback and stereo spread.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"depth_ms", 2, "Depth", "ms", "Depth parameter measured in ms. Create classic, barber-pole, or through-zero comb sweeps with feedback and stereo spread.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"center_ms", 3, "Center", "ms", "Center parameter measured in ms. Create classic, barber-pole, or through-zero comb sweeps with feedback and stereo spread.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"feedback", 5, "Feedback", "", "Feedback parameter. Create classic, barber-pole, or through-zero comb sweeps with feedback and stereo spread.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mix", 6, "Mix", "", "Mix parameter. Create classic, barber-pole, or through-zero comb sweeps with feedback and stereo spread.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"spread_deg", 7, "Spread Deg", "", "Spread Deg parameter. Create classic, barber-pole, or through-zero comb sweeps with feedback and stereo spread.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"polarity", 9, "Polarity", "", "Polarity parameter. Create classic, barber-pole, or through-zero comb sweeps with feedback and stereo spread.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"positive", "Positive", 0.0f}, {"negative", "Negative", 1.0f}}, {}},
        {"engine", 10, "Engine", "", "Engine parameter. Create classic, barber-pole, or through-zero comb sweeps with feedback and stereo spread.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"clean", "Clean", 0.0f}, {"bbd", "BBD", 1.0f}}, {}},
        {"barberpole_hz", 11, "Barberpole", "Hz", "Barberpole parameter measured in Hz. Create classic, barber-pole, or through-zero comb sweeps with feedback and stereo spread.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"wave", 12, "Wave", "", "Wave parameter. Create classic, barber-pole, or through-zero comb sweeps with feedback and stereo spread.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"sine", "Sine", 0.0f}, {"triangle", "Triangle", 1.0f}, {"saw_up", "Saw Up", 2.0f}, {"saw_down", "Saw Down", 3.0f}, {"square", "Square", 4.0f}, {"sample_hold", "Sample & Hold", 5.0f}, {"smooth_random", "Smooth Random", 6.0f}}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor leslie_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "leslie";
    d.label = "Leslie";
    d.description = "Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.";
    d.realizations = {
        {"default", "modulation.leslie"},
    };
    d.params = {
        {"speed", 1, "Speed", "", "Speed parameter. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"stop", "Stop", 0.0f}, {"chorale", "Chorale", 1.0f}, {"tremolo", "Tremolo", 2.0f}}, {}},
        {"horn_fast_hz", 2, "Horn Fast", "Hz", "Horn Fast parameter measured in Hz. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"horn_slow_hz", 3, "Horn Slow", "Hz", "Horn Slow parameter measured in Hz. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"drum_fast_hz", 4, "Drum Fast", "Hz", "Drum Fast parameter measured in Hz. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"drum_slow_hz", 5, "Drum Slow", "Hz", "Drum Slow parameter measured in Hz. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"horn_accel_s", 6, "Horn Accel S", "s", "Horn Accel S parameter measured in s. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"drum_accel_s", 7, "Drum Accel S", "s", "Drum Accel S parameter measured in s. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"crossover_hz", 8, "Crossover", "Hz", "Crossover parameter measured in Hz. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"horn_radius_m", 9, "Horn Radius M", "", "Horn Radius M parameter. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"drum_radius_m", 10, "Drum Radius M", "", "Drum Radius M parameter. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"am_depth", 11, "Am Depth", "", "Am Depth parameter. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"dir_depth_db", 12, "Dir Depth", "dB", "Dir Depth parameter measured in dB. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"dir_corner_hz", 13, "Dir Corner", "Hz", "Dir Corner parameter measured in Hz. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"drum_dir_depth_db", 14, "Drum Dir Depth", "dB", "Drum Dir Depth parameter measured in dB. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"d_bias_ms", 15, "D Bias", "ms", "D Bias parameter measured in ms. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mic_angle_deg", 16, "Mic Angle Deg", "deg", "Mic Angle Deg parameter measured in deg. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mic_distance_m", 17, "Mic Distance M", "", "Mic Distance M parameter. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"reflection_db", 18, "Reflection", "dB", "Reflection parameter measured in dB. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"reflections", 19, "Reflections", "", "Reflections parameter. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::stepped, ForgeParamCurve::linear, forge_integer_choices(1, 4), {}},
        {"reflection_delay_ms", 20, "Reflection Delay", "ms", "Reflection Delay parameter measured in ms. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"reflection_spacing_ms", 21, "Reflection Spacing", "ms", "Reflection Spacing parameter measured in ms. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"reflection_corner_hz", 22, "Reflection Corner", "Hz", "Reflection Corner parameter measured in Hz. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"drift_cents", 23, "Drift", "cent", "Drift parameter measured in cent. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"mix", 24, "Mix", "", "Mix parameter. Model a rotating horn-and-drum speaker with independent inertia, microphones, reflections, and speed.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor scanner_vibrato_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "scanner_vibrato";
    d.label = "Scanner Vibrato";
    d.description = "Reproduce organ scanner vibrato and chorus from a tapped delay-line scan.";
    d.realizations = {
        {"default", "modulation.scanner_vibrato"},
    };
    d.params = {
        {"mode", 1, "Mode", "", "Mode parameter. Reproduce organ scanner vibrato and chorus from a tapped delay-line scan.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"v1", "V1", 1.0f}, {"v2", "V2", 2.0f}, {"v3", "V3", 3.0f}, {"c1", "C1", 4.0f}, {"c2", "C2", 5.0f}, {"c3", "C3", 6.0f}}, {}},
        {"scan_hz", 2, "Scan", "Hz", "Scan parameter measured in Hz. Reproduce organ scanner vibrato and chorus from a tapped delay-line scan.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"line_ms", 3, "Line", "ms", "Line parameter measured in ms. Reproduce organ scanner vibrato and chorus from a tapped delay-line scan.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"v1", 4, "V1", "", "V1 parameter. Reproduce organ scanner vibrato and chorus from a tapped delay-line scan.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"v2", 5, "V2", "", "V2 parameter. Reproduce organ scanner vibrato and chorus from a tapped delay-line scan.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"v3", 6, "V3", "", "V3 parameter. Reproduce organ scanner vibrato and chorus from a tapped delay-line scan.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"chorus_mix", 7, "Chorus Mix", "", "Chorus Mix parameter. Reproduce organ scanner vibrato and chorus from a tapped delay-line scan.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline std::vector<ForgeNodeDescriptor> effect_modulation_descriptors() {
    return {
        frequency_shifter_ssb_descriptor(),
        chorus_family_descriptor(),
        phaser_stages_descriptor(),
        vibrato_delay_descriptor(),
        vibrato_phase_descriptor(),
        univibe_descriptor(),
        flanger_descriptor(),
        leslie_descriptor(),
        scanner_vibrato_descriptor(),
    };
}

}  // namespace pulp::host::modulation
