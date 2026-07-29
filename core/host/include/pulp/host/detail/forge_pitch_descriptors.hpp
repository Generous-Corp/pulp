#pragma once

// Semantic vocabulary transcribed from Forge's catalog-owned registry.

#include <pulp/host/forge_param_descriptor.hpp>

#include <vector>

namespace pulp::host::pitch {

inline ForgeNodeDescriptor pitch_shifter_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "pitch_shifter";
    d.label = "Pitch Shifter";
    d.description = "Perform expressive pedal, interval, detune, or dive-style pitch shifting with "
                    "glide control.";
    d.realizations = {
        {"default", "pitch.whammy"},
    };
    d.params = {
        {"pedal",
         1,
         "Pedal",
         "",
         "Pedal parameter. Perform expressive pedal, interval, detune, or dive-style pitch "
         "shifting with glide control.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"pedal_mode",
         2,
         "Pedal Mode",
         "",
         "Pedal Mode parameter. Perform expressive pedal, interval, detune, or dive-style pitch "
         "shifting with glide control.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"whammy", "Whammy", 0.0f},
          {"harmony", "Harmony", 1.0f},
          {"detune", "Detune", 2.0f},
          {"dive", "Dive", 3.0f}},
         {}},
        {"shift_source",
         3,
         "Shift Source",
         "",
         "Shift Source parameter. Perform expressive pedal, interval, detune, or dive-style pitch "
         "shifting with glide control.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"pedal", "Pedal", 0.0f}, {"direct", "Direct", 1.0f}},
         {}},
        {"shift_semitones",
         4,
         "Shift Semitones",
         "st",
         "Shift Semitones parameter measured in st. Perform expressive pedal, interval, detune, or "
         "dive-style pitch shifting with glide control.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"heel_semitones",
         5,
         "Heel Semitones",
         "st",
         "Heel Semitones parameter measured in st. Perform expressive pedal, interval, detune, or "
         "dive-style pitch shifting with glide control.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"toe_semitones",
         6,
         "Toe Semitones",
         "st",
         "Toe Semitones parameter measured in st. Perform expressive pedal, interval, detune, or "
         "dive-style pitch shifting with glide control.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"interval_a_semitones",
         7,
         "Interval A Semitones",
         "st",
         "Interval A Semitones parameter measured in st. Perform expressive pedal, interval, "
         "detune, or dive-style pitch shifting with glide control.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"interval_b_semitones",
         8,
         "Interval B Semitones",
         "st",
         "Interval B Semitones parameter measured in st. Perform expressive pedal, interval, "
         "detune, or dive-style pitch shifting with glide control.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"detune_cents",
         9,
         "Detune",
         "cent",
         "Detune parameter measured in cent. Perform expressive pedal, interval, detune, or "
         "dive-style pitch shifting with glide control.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"dive_floor_semitones",
         10,
         "Dive Floor Semitones",
         "st",
         "Dive Floor Semitones parameter measured in st. Perform expressive pedal, interval, "
         "detune, or dive-style pitch shifting with glide control.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"glide_up_ms",
         11,
         "Glide Up",
         "ms",
         "Glide Up parameter measured in ms. Perform expressive pedal, interval, detune, or "
         "dive-style pitch shifting with glide control.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"glide_down_ms",
         12,
         "Glide Down",
         "ms",
         "Glide Down parameter measured in ms. Perform expressive pedal, interval, detune, or "
         "dive-style pitch shifting with glide control.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"mix",
         13,
         "Mix",
         "",
         "Mix parameter. Perform expressive pedal, interval, detune, or dive-style pitch shifting "
         "with glide control.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"detents",
         14,
         "Detents",
         "",
         "Detents parameter. Perform expressive pedal, interval, detune, or dive-style pitch "
         "shifting with glide control.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"off", "Off", 0.0f}, {"on", "On", 1.0f}},
         {}},
        {"interpolation",
         15,
         "Interpolation",
         "",
         "Interpolation parameter. Perform expressive pedal, interval, detune, or dive-style pitch "
         "shifting with glide control.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"linear", "Linear", 0.0f}, {"cubic", "Cubic", 1.0f}},
         {}},
        {"drift_depth",
         16,
         "Drift Depth",
         "",
         "Drift Depth parameter. Perform expressive pedal, interval, detune, or dive-style pitch "
         "shifting with glide control.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
    };
    return d;
}

inline ForgeNodeDescriptor harmony_engine_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "harmony_engine";
    d.label = "Harmony Engine";
    d.description = "Generate one or two pitch-shifted harmony voices in a chosen key and scale.";
    d.realizations = {
        {"default", "pitch.harmony_engine"},
    };
    d.params = {
        {"key",
         1,
         "Key",
         "",
         "Key parameter. Generate one or two pitch-shifted harmony voices in a chosen key and "
         "scale.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"c", "C", 0.0f},
          {"c_sharp", "C sharp", 1.0f},
          {"d", "D", 2.0f},
          {"d_sharp", "D sharp", 3.0f},
          {"e", "E", 4.0f},
          {"f", "F", 5.0f},
          {"f_sharp", "F sharp", 6.0f},
          {"g", "G", 7.0f},
          {"g_sharp", "G sharp", 8.0f},
          {"a", "A", 9.0f},
          {"a_sharp", "A sharp", 10.0f},
          {"b", "B", 11.0f}},
         {}},
        {"scale",
         2,
         "Scale",
         "",
         "Scale parameter. Generate one or two pitch-shifted harmony voices in a chosen key and "
         "scale.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"major", "Major", 0.0f},
          {"natural_minor", "Natural Minor", 1.0f},
          {"dorian", "Dorian", 2.0f},
          {"phrygian", "Phrygian", 3.0f},
          {"lydian", "Lydian", 4.0f},
          {"mixolydian", "Mixolydian", 5.0f},
          {"harmonic_minor", "Harmonic Minor", 6.0f},
          {"melodic_minor", "Melodic Minor", 7.0f},
          {"major_pentatonic", "Major Pentatonic", 8.0f},
          {"minor_pentatonic", "Minor Pentatonic", 9.0f}},
         {}},
        {"voice1_interval",
         3,
         "Voice1 Interval",
         "scale steps",
         "Voice1 Interval selects an integer number of scale steps. Generate one or two "
         "pitch-shifted harmony voices in a chosen key and scale.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         forge_integer_choices(-14, 14),
         {}},
        {"voice1_detune",
         4,
         "Voice1 Detune",
         "cent",
         "Voice1 Detune parameter measured in cent. Generate one or two pitch-shifted harmony "
         "voices in a chosen key and scale.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"voice1_level_db",
         5,
         "Voice1 Level",
         "dB",
         "Voice1 Level parameter measured in dB. Generate one or two pitch-shifted harmony voices "
         "in a chosen key and scale.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"voice2_enable",
         6,
         "Voice2 Enable",
         "",
         "Voice2 Enable parameter. Generate one or two pitch-shifted harmony voices in a chosen "
         "key and scale.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"off", "Off", 0.0f}, {"on", "On", 1.0f}},
         {}},
        {"voice2_interval",
         7,
         "Voice2 Interval",
         "scale steps",
         "Voice2 Interval selects an integer number of scale steps. Generate one or two "
         "pitch-shifted harmony voices in a chosen key and scale.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         forge_integer_choices(-14, 14),
         {}},
        {"voice2_detune",
         8,
         "Voice2 Detune",
         "cent",
         "Voice2 Detune parameter measured in cent. Generate one or two pitch-shifted harmony "
         "voices in a chosen key and scale.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"voice2_level_db",
         9,
         "Voice2 Level",
         "dB",
         "Voice2 Level parameter measured in dB. Generate one or two pitch-shifted harmony voices "
         "in a chosen key and scale.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"glide_ms",
         10,
         "Glide",
         "ms",
         "Glide parameter measured in ms. Generate one or two pitch-shifted harmony voices in a "
         "chosen key and scale.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"dry_level_db",
         11,
         "Dry Level",
         "dB",
         "Dry Level parameter measured in dB. Generate one or two pitch-shifted harmony voices in "
         "a chosen key and scale.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"humanize_cents",
         12,
         "Humanize",
         "cent",
         "Humanize parameter measured in cent. Generate one or two pitch-shifted harmony voices in "
         "a chosen key and scale.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
    };
    return d;
}

inline std::vector<ForgeNodeDescriptor> pitch_descriptors() {
    return {
        pitch_shifter_descriptor(),
        harmony_engine_descriptor(),
    };
}

} // namespace pulp::host::pitch
