#pragma once

// Semantic vocabulary transcribed from Forge's catalog-owned registry.

#include <pulp/host/forge_param_descriptor.hpp>

#include <vector>

namespace pulp::host::sequencing {

inline ForgeNodeDescriptor stage_seq_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "stage_seq";
    d.label = "Stage Seq";
    d.description = "Generate staged control values and gates from an incoming clock for stepped "
                    "modulation patterns.";
    d.realizations = {
        {"default", "sequencing.stage_seq"},
    };
    d.params = {
        {"run",
         1,
         "Run",
         "",
         "Run parameter. Generate staged control values and gates from an incoming clock for "
         "stepped modulation patterns.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"off", "Off", 0.0f}, {"on", "On", 1.0f}},
         {}},
        {"num_stages",
         2,
         "Num Stages",
         "",
         "Num Stages parameter. Generate staged control values and gates from an incoming clock "
         "for stepped modulation patterns.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         forge_integer_choices(1, 16),
         {}},
        {"direction",
         3,
         "Direction",
         "",
         "Direction parameter. Generate staged control values and gates from an incoming clock for "
         "stepped modulation patterns.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"forward", "Forward", 0.0f},
          {"reverse", "Reverse", 1.0f},
          {"ping_pong", "Ping Pong", 2.0f},
          {"random", "Random", 3.0f}},
         {}},
        {"slide_ms",
         4,
         "Slide",
         "ms",
         "Slide parameter measured in ms. Generate staged control values and gates from an "
         "incoming clock for stepped modulation patterns.",
         ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic,
         {},
         {}},
        {"repeat_duty_pct",
         5,
         "Repeat Duty Pct",
         "%",
         "Repeat Duty Pct parameter measured in %. Generate staged control values and gates from "
         "an incoming clock for stepped modulation patterns.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
    };
    return d;
}

inline ForgeNodeDescriptor cartesian_walk_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "cartesian_walk";
    d.label = "Cartesian Walk";
    d.description = "Walk a two-dimensional value grid from clock and direction inputs to generate "
                    "coordinated control patterns.";
    d.axes = {{"mode",
               "Mode",
               "Finite construction modes for Cartesian Walk.",
               {{"independent", "Independent", 0.0f}, {"row_major", "Row Major", 1.0f}}}};
    d.realizations = {
        {"independent", "sequencing.cartesian_walk"},
        {"row_major", "sequencing.cartesian_walk_row_major"},
    };
    d.params = {
        {"run",
         1,
         "Run",
         "",
         "Run parameter. Walk a two-dimensional value grid from clock and direction inputs to "
         "generate coordinated control patterns.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"off", "Off", 0.0f}, {"on", "On", 1.0f}},
         {}},
        {"grid_w",
         2,
         "Grid W",
         "",
         "Grid W parameter. Walk a two-dimensional value grid from clock and direction inputs to "
         "generate coordinated control patterns.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         forge_integer_choices(1, 8),
         {}},
        {"grid_h",
         3,
         "Grid H",
         "",
         "Grid H parameter. Walk a two-dimensional value grid from clock and direction inputs to "
         "generate coordinated control patterns.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         forge_integer_choices(1, 8),
         {}},
        {"x_offset",
         4,
         "X Offset",
         "",
         "X Offset parameter. Walk a two-dimensional value grid from clock and direction inputs to "
         "generate coordinated control patterns.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         forge_integer_choices(-8, 8),
         {}},
        {"y_offset",
         5,
         "Y Offset",
         "",
         "Y Offset parameter. Walk a two-dimensional value grid from clock and direction inputs to "
         "generate coordinated control patterns.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         forge_integer_choices(-8, 8),
         {}},
    };
    return d;
}

inline ForgeNodeDescriptor rungler_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "rungler";
    d.label = "Rungler";
    d.description = "Generate looping pseudo-random stepped control and gate patterns from clocked "
                    "shift-register feedback.";
    d.realizations = {
        {"default", "sequencing.rungler"},
    };
    d.params = {
        {"run",
         1,
         "Run",
         "",
         "Run parameter. Generate looping pseudo-random stepped control and gate patterns from "
         "clocked shift-register feedback.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"off", "Off", 0.0f}, {"on", "On", 1.0f}},
         {}},
        {"dac_bits",
         2,
         "Dac Bits",
         "",
         "Dac Bits parameter. Generate looping pseudo-random stepped control and gate patterns "
         "from clocked shift-register feedback.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         forge_integer_choices(1, 4),
         {}},
        {"feedback_tap",
         3,
         "Feedback Tap",
         "",
         "Feedback Tap parameter. Generate looping pseudo-random stepped control and gate patterns "
         "from clocked shift-register feedback.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         forge_integer_choices(0, 6),
         {}},
        {"range_v",
         4,
         "Range V",
         "V",
         "Range V parameter measured in V. Generate looping pseudo-random stepped control and gate "
         "patterns from clocked shift-register feedback.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"external_data",
         5,
         "External Data",
         "",
         "External Data parameter. Generate looping pseudo-random stepped control and gate "
         "patterns from clocked shift-register feedback.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"off", "Off", 0.0f}, {"on", "On", 1.0f}},
         {}},
        {"data_in",
         6,
         "Data In",
         "",
         "Data In parameter. Generate looping pseudo-random stepped control and gate patterns from "
         "clocked shift-register feedback.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"zero", "Zero", 0.0f}, {"one", "One", 1.0f}},
         {}},
    };
    return d;
}

inline ForgeNodeDescriptor quantize_scale_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "quantize_scale";
    d.label = "Quantize Scale";
    d.description =
        "Snap continuous control or pitch values to an equal division or authored musical scale.";
    d.realizations = {
        {"default", "sequencing.quantize_scale"},
    };
    d.params = {
        {"mode",
         1,
         "Mode",
         "",
         "Mode parameter. Snap continuous control or pitch values to an equal division or authored "
         "musical scale.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"edo", "EDO", 0.0f}, {"scale_mask", "Scale Mask", 1.0f}},
         {}},
        {"edo_n",
         2,
         "Edo N",
         "",
         "Edo N parameter. Snap continuous control or pitch values to an equal division or "
         "authored musical scale.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         forge_integer_choices(1, 48),
         {}},
        {"scale_mask",
         3,
         "Scale Mask",
         "",
         "Scale Mask combines pitch-class bits C through B. Snap continuous control or pitch "
         "values to an equal division or authored musical scale.",
         ForgeParamKind::bitmask,
         ForgeParamCurve::linear,
         {{"c", "C", 1.0f},
          {"c_sharp", "C Sharp", 2.0f},
          {"d", "D", 4.0f},
          {"d_sharp", "D Sharp", 8.0f},
          {"e", "E", 16.0f},
          {"f", "F", 32.0f},
          {"f_sharp", "F Sharp", 64.0f},
          {"g", "G", 128.0f},
          {"g_sharp", "G Sharp", 256.0f},
          {"a", "A", 512.0f},
          {"a_sharp", "A Sharp", 1024.0f},
          {"b", "B", 2048.0f}},
         {}},
        {"root_pc",
         4,
         "Root Pc",
         "",
         "Root Pc parameter. Snap continuous control or pitch values to an equal division or "
         "authored musical scale.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"c", "C", 0.0f},
          {"c_sharp", "C Sharp", 1.0f},
          {"d", "D", 2.0f},
          {"d_sharp", "D Sharp", 3.0f},
          {"e", "E", 4.0f},
          {"f", "F", 5.0f},
          {"f_sharp", "F Sharp", 6.0f},
          {"g", "G", 7.0f},
          {"g_sharp", "G Sharp", 8.0f},
          {"a", "A", 9.0f},
          {"a_sharp", "A Sharp", 10.0f},
          {"b", "B", 11.0f}},
         {}},
        {"hysteresis_cents",
         5,
         "Hysteresis",
         "cent",
         "Hysteresis parameter measured in cent. Snap continuous control or pitch values to an "
         "equal division or authored musical scale.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"reset",
         6,
         "Reset",
         "",
         "Reset parameter. Snap continuous control or pitch values to an equal division or "
         "authored musical scale.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"off", "Off", 0.0f}, {"on", "On", 1.0f}},
         {}},
    };
    return d;
}

inline ForgeNodeDescriptor gate_logic_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "gate_logic";
    d.label = "Gate Logic";
    d.description =
        "Combine two gate streams with Boolean logic and expose complementary event outputs.";
    d.realizations = {
        {"default", "sequencing.gate_logic"},
    };
    d.params = {
        {"op",
         1,
         "Op",
         "",
         "Op parameter. Combine two gate streams with Boolean logic and expose complementary event "
         "outputs.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"and", "AND", 0.0f},
          {"or", "OR", 1.0f},
          {"xor", "XOR", 2.0f},
          {"nand", "NAND", 3.0f},
          {"nor", "NOR", 4.0f},
          {"xnor", "XNOR", 5.0f}},
         {}},
    };
    return d;
}

inline ForgeNodeDescriptor prob_gate_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "prob_gate";
    d.label = "Prob Gate";
    d.description = "Pass or suppress incoming gates with repeatable probability for controlled "
                    "rhythmic variation.";
    d.realizations = {
        {"default", "sequencing.prob_gate"},
    };
    d.params = {
        {"probability_pct",
         1,
         "Probability Pct",
         "%",
         "Probability Pct parameter measured in %. Pass or suppress incoming gates with repeatable "
         "probability for controlled rhythmic variation.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"reset",
         2,
         "Reset",
         "",
         "Reset parameter. Pass or suppress incoming gates with repeatable probability for "
         "controlled rhythmic variation.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"off", "Off", 0.0f}, {"on", "On", 1.0f}},
         {}},
    };
    return d;
}

inline std::vector<ForgeNodeDescriptor> sequencing_descriptors() {
    return {
        stage_seq_descriptor(),      cartesian_walk_descriptor(), rungler_descriptor(),
        quantize_scale_descriptor(), gate_logic_descriptor(),     prob_gate_descriptor(),
    };
}

} // namespace pulp::host::sequencing
