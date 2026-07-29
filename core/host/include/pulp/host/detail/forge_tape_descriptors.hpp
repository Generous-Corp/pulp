#pragma once

// Semantic vocabulary transcribed from Forge's catalog-owned registry.

#include <pulp/host/forge_param_descriptor.hpp>

#include <vector>

namespace pulp::host::tape {

inline ForgeNodeDescriptor tape_machine_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "tape_machine";
    d.label = "Tape Machine";
    d.description =
        "Impart tape saturation, record/repro EQ, age, crosstalk, and print-through character.";
    d.axes = {{"mode",
               "Mode",
               "Finite construction modes for Tape Machine.",
               {{"ampex_7_5ips", "Ampex 7 5ips", 0.0f},
                {"ampex_7_5ips_pre_echo", "Ampex 7 5ips Pre Echo", 1.0f},
                {"ampex", "Ampex", 2.0f},
                {"ampex_pre_echo", "Ampex Pre Echo", 3.0f},
                {"studer_7_5ips", "Studer 7 5ips", 4.0f},
                {"studer_7_5ips_pre_echo", "Studer 7 5ips Pre Echo", 5.0f},
                {"studer", "Studer", 6.0f},
                {"studer_pre_echo", "Studer Pre Echo", 7.0f},
                {"studer_30ips", "Studer 30ips", 8.0f},
                {"studer_30ips_pre_echo", "Studer 30ips Pre Echo", 9.0f},
                {"cassette", "Cassette", 10.0f},
                {"cassette_pre_echo", "Cassette Pre Echo", 11.0f}}}};
    d.realizations = {
        {"ampex_7_5ips", "tape.ampex350_440.speed_401e000000000000"},
        {"ampex_7_5ips_pre_echo", "tape.ampex350_440.speed_401e000000000000.pre_echo"},
        {"ampex", "tape.ampex350_440"},
        {"ampex_pre_echo", "tape.ampex350_440.speed_402e000000000000.pre_echo"},
        {"studer_7_5ips", "tape.studer_a800.speed_401e000000000000"},
        {"studer_7_5ips_pre_echo", "tape.studer_a800.speed_401e000000000000.pre_echo"},
        {"studer", "tape.studer_a800"},
        {"studer_pre_echo", "tape.studer_a800.speed_402e000000000000.pre_echo"},
        {"studer_30ips", "tape.studer_a800.speed_403e000000000000"},
        {"studer_30ips_pre_echo", "tape.studer_a800.speed_403e000000000000.pre_echo"},
        {"cassette", "tape.cassette"},
        {"cassette_pre_echo", "tape.cassette.speed_3ffe000000000000.pre_echo"},
    };
    d.params = {
        {"bias",
         2,
         "Bias",
         "%",
         "Bias parameter measured in %. Impart tape saturation, record/repro EQ, age, crosstalk, "
         "and print-through character.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"drive",
         3,
         "Drive",
         "%",
         "Drive parameter measured in %. Impart tape saturation, record/repro EQ, age, crosstalk, "
         "and print-through character.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"age",
         4,
         "Age",
         "%",
         "Age parameter measured in %. Impart tape saturation, record/repro EQ, age, crosstalk, "
         "and print-through character.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"crosstalk_db",
         5,
         "Crosstalk",
         "dB",
         "Crosstalk parameter measured in dB. Impart tape saturation, record/repro EQ, age, "
         "crosstalk, and print-through character.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"print_through_db",
         7,
         "Print Through",
         "dB",
         "Print Through parameter measured in dB. Impart tape saturation, record/repro EQ, age, "
         "crosstalk, and print-through character.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"print_offset_ms",
         8,
         "Print Offset",
         "ms",
         "Print Offset parameter measured in ms. Impart tape saturation, record/repro EQ, age, "
         "crosstalk, and print-through character.",
         ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic,
         {},
         {"ampex_7_5ips", "ampex", "studer_7_5ips", "studer", "studer_30ips", "cassette"}},
        {"mix",
         9,
         "Mix",
         "%",
         "Mix parameter measured in %. Impart tape saturation, record/repro EQ, age, crosstalk, "
         "and print-through character.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {}},
        {"eq_curve",
         1,
         "EQ Curve",
         "",
         "EQ Curve parameter. Impart tape saturation, record/repro EQ, age, crosstalk, and "
         "print-through character.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"nab",
           "NAB",
           0.0f,
           {"studer_7_5ips", "studer_7_5ips_pre_echo", "studer", "studer_pre_echo", "studer_30ips",
            "studer_30ips_pre_echo"}},
          {"iec_ccir",
           "IEC/CCIR",
           1.0f,
           {"studer_7_5ips", "studer_7_5ips_pre_echo", "studer", "studer_pre_echo", "studer_30ips",
            "studer_30ips_pre_echo"}},
          {"cassette_type1", "Cassette Type I", 2.0f, {"cassette", "cassette_pre_echo"}},
          {"cassette_type2", "Cassette Type II", 3.0f, {"cassette", "cassette_pre_echo"}}},
         {"studer_7_5ips", "studer_7_5ips_pre_echo", "studer", "studer_pre_echo", "studer_30ips",
          "studer_30ips_pre_echo", "cassette", "cassette_pre_echo"}},
        {"companding",
         6,
         "Companding",
         "",
         "Companding parameter. Impart tape saturation, record/repro EQ, age, crosstalk, and "
         "print-through character.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"off", "Off", 0.0f}, {"on", "On", 1.0f}},
         {}},
    };
    return d;
}

inline std::vector<ForgeNodeDescriptor> tape_descriptors() {
    return {
        tape_machine_descriptor(),
    };
}

} // namespace pulp::host::tape
