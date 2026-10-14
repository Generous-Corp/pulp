// Out-of-line Forge tape catalog factories.
//
// Keep the public declarations and stable catalog contracts in the header;
// implementation-heavy TapeMachine construction belongs in one translation unit.
#include <pulp/host/forge_tape_catalog.hpp>

namespace pulp::host::tape {

float tape_machine_insertion_gain_bound(Archetype archetype, double sample_rate) {
    signal::TapeMachine probe;
    probe.set_archetype(archetype);
    probe.prepare(sample_rate);
    return static_cast<float>(probe.worst_case_insertion_gain());
}

CustomNodeType make_tape_machine_node(Archetype archetype, double speed_ips,
                                      bool pre_echo_enabled) {
    using Machine = signal::TapeMachine;
    const Archetype fixed_archetype = normalized_archetype(archetype);
    const signal::tape::ArchetypePreset preset = signal::tape::archetype_preset(fixed_archetype);
    const double requested_speed =
        std::isfinite(speed_ips) && speed_ips > 0.0 ? speed_ips : preset.default_speed_ips;
    const double speed = signal::tape::snap_speed_ips(fixed_archetype, requested_speed);
    const CurveRange curves = curve_range(fixed_archetype);
    const bool curve_is_variable = curves.min_curve != curves.max_curve;

    CustomNodeType t;
    t.type_id = tape_type_id(fixed_archetype);
    if (speed != preset.default_speed_ips || pre_echo_enabled) {
        t.type_id += ".speed_" + detail::realization_real_token(speed);
        if (pre_echo_enabled)
            t.type_id += ".pre_echo";
    }
    t.version = 1;
    t.num_input_ports = 2;
    t.num_output_ports = 2;
    t.default_name = tape_default_name(fixed_archetype);
    t.lowerable = true;
    t.latency_samples = [fixed_archetype, speed, pre_echo_enabled](double sample_rate) {
        signal::TapeMachine probe;
        probe.set_archetype(fixed_archetype);
        probe.set_speed_ips(speed);
        probe.prepare(sample_rate);
        probe.set_print_through(static_cast<float>(probe.print_through_db()),
                                static_cast<float>(probe.print_offset_ms()), pre_echo_enabled);
        return probe.latency_samples();
    };

    t.create = []() -> void* { return new TapeMachineInstance{}; };
    t.destroy = [](void* p) { delete static_cast<TapeMachineInstance*>(p); };
    t.prepare = [fixed_archetype, speed, pre_echo_enabled](void* p, double sr, int /*max_block*/) {
        auto* instance = static_cast<TapeMachineInstance*>(p);
        instance->machine.set_archetype(fixed_archetype);
        instance->machine.set_speed_ips(speed);
        instance->machine.prepare(sr);
        // Pre-echo is a realization, so it is set once here and never touched
        // again — that is what keeps `latency_samples()` still.
        instance->machine.set_print_through(
            static_cast<float>(instance->machine.print_through_db()),
            static_cast<float>(instance->machine.print_offset_ms()), pre_echo_enabled);
        instance->invalidate_cache();
    };
    t.reset = [](void* p) { static_cast<TapeMachineInstance*>(p)->machine.reset(); };

    // Ranges and defaults are the module's canonical contract, in REAL units;
    // the Forge layer mirrors them and owns the display curve. Every row here is
    // a design-parameter declaration per the series contract.
    if (curve_is_variable) {
        t.baked_params.push_back({kEqCurve, static_cast<float>(curves.min_curve),
                                  static_cast<float>(curves.max_curve),
                                  static_cast<float>(preset.default_curve)});
    }
    t.baked_params.push_back({kBias, static_cast<float>(Machine::kBiasMin),
                              static_cast<float>(Machine::kBiasMax),
                              static_cast<float>(Machine::kBiasDefault)});
    t.baked_params.push_back({kDrive, 0.0f, 1.0f, static_cast<float>(Machine::kDriveDefault)});
    t.baked_params.push_back({kAge, 0.0f, 1.0f, static_cast<float>(preset.age01)});
    t.baked_params.push_back({kCrosstalkDb, static_cast<float>(Machine::kCrosstalkDbMin),
                              static_cast<float>(Machine::kCrosstalkDbMax),
                              static_cast<float>(preset.crosstalk_db)});
    t.baked_params.push_back({kCompanding, 0.0f, 1.0f, preset.companding ? 1.0f : 0.0f});
    t.baked_params.push_back(
        {kPrintThroughDb, static_cast<float>(Machine::kPrintThroughDbMin),
         static_cast<float>(Machine::kPrintThroughDbMax),
         static_cast<float>(signal::tape::age_print_through_db(preset.age01))});
    // With pre-echo disabled the offset is latency-neutral. A pre-echo
    // realization fixes it at construction because it contributes to latency,
    // so exposing a frozen row there would advertise a capability that cannot
    // do anything.
    const float print_offset_default = static_cast<float>(Machine::kPrintThroughOffsetMsDefault);
    if (!pre_echo_enabled) {
        t.baked_params.push_back(
            {kPrintOffsetMs, static_cast<float>(Machine::kPrintThroughOffsetMsMin),
             static_cast<float>(Machine::kPrintThroughOffsetMsMax), print_offset_default});
    }
    t.baked_params.push_back({kMix, 0.0f, 1.0f, 1.0f});

    t.process_instance_baked_param = [pre_echo_enabled,
                                      curve_is_variable](void* p, audio::BufferView<float>& out,
                                                         const audio::BufferView<const float>& in,
                                                         int n, const BakedParamView& params) {
        auto* instance = static_cast<TapeMachineInstance*>(p);
        auto& machine = instance->machine;

        // Read at offset 0 and apply once. See the header note on why this node
        // does not write its setters per sample.
        auto changed = [](float& held, float fresh) {
            if (held == fresh)
                return false;
            held = fresh;
            return true;
        };

        if (curve_is_variable && changed(instance->curve, params.value_at(kEqCurve, 0)))
            machine.set_eq_curve(
                static_cast<Curve>(static_cast<std::uint8_t>(std::lround(instance->curve))));
        if (changed(instance->drive, params.value_at(kDrive, 0)))
            machine.set_drive(instance->drive);

        // Age first, because `set_age` WRITES the print-through level (that is
        // what "storage print grows with age" means) and shifts the effective
        // bias. Applying it after either would let the age axis silently
        // overwrite an explicit parameter — so age goes first, and a block in
        // which it moved must re-apply the print level even if the print
        // parameter itself did not change.
        const bool age_moved = changed(instance->age, params.value_at(kAge, 0));
        if (age_moved)
            machine.set_age(instance->age);

        if (changed(instance->bias, params.value_at(kBias, 0)) || age_moved)
            machine.set_bias(instance->bias);
        if (changed(instance->crosstalk_db, params.value_at(kCrosstalkDb, 0)))
            machine.set_crosstalk_db(instance->crosstalk_db);
        if (changed(instance->companding, params.value_at(kCompanding, 0)))
            machine.set_companding_enabled(instance->companding >= 0.5f);

        const bool print_moved = changed(instance->print_db, params.value_at(kPrintThroughDb, 0));
        const float print_offset_ms =
            pre_echo_enabled ? static_cast<float>(Machine::kPrintThroughOffsetMsDefault)
                             : params.value_at(kPrintOffsetMs, 0);
        const bool offset_moved = changed(instance->print_offset_ms, print_offset_ms);
        if (print_moved || offset_moved || age_moved)
            machine.set_print_through(instance->print_db, instance->print_offset_ms,
                                      pre_echo_enabled);

        if (changed(instance->mix, params.value_at(kMix, 0)))
            machine.set_mix(instance->mix);

        // Safe to write straight into the output view even when the executor
        // aliases it onto the input: `process()` reads both input samples for a
        // frame before writing either output sample of that frame, and never
        // reads an input frame it has already passed.
        machine.process(in.channel_ptr(0), in.channel_ptr(1), out.channel_ptr(0),
                        out.channel_ptr(1), n);
    };
    return t;
}

ForgeNodeDescriptor descriptor() {
    ForgeNodeDescriptor d{
        "tape_machine",
        "Tape Machine",
        "Models open-reel and cassette recording paths with bias, saturation, wear, crosstalk, "
        "companding, and print-through.",
        {{"archetype",
          "Archetype",
          "Selects the fixed machine lineage.",
          {{"ampex", "Ampex-class Open Reel", 0},
           {"studer", "Studer-class Open Reel", 1},
           {"cassette", "Cassette Deck", 2}}}},
        {},
        {
            {"eq_curve",
             kEqCurve,
             "EQ Curve",
             "",
             "Selects the recording equalization standard.",
             ForgeParamKind::stepped,
             ForgeParamCurve::linear,
             {{"nab",
               "NAB",
               static_cast<float>(Curve::nab),
               {"studer_7_5ips", "studer_7_5ips_pre_echo", "studer", "studer_pre_echo",
                "studer_30ips", "studer_30ips_pre_echo"}},
              {"iec_ccir",
               "IEC/CCIR",
               static_cast<float>(Curve::iec_ccir),
               {"studer_7_5ips", "studer_7_5ips_pre_echo", "studer", "studer_pre_echo",
                "studer_30ips", "studer_30ips_pre_echo"}},
              {"cassette_type_one",
               "Cassette Type One",
               static_cast<float>(Curve::cassette_type1),
               {"cassette", "cassette_pre_echo"}},
              {"cassette_type_two",
               "Cassette Type Two",
               static_cast<float>(Curve::cassette_type2),
               {"cassette", "cassette_pre_echo"}}},
             {"studer_7_5ips", "studer_7_5ips_pre_echo", "studer", "studer_pre_echo",
              "studer_30ips", "studer_30ips_pre_echo", "cassette", "cassette_pre_echo"}},
            {"bias", kBias, "Bias", "", "Moves tape bias below or above the calibrated point.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"drive", kDrive, "Drive", "%", "Sets record-level saturation.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"age", kAge, "Age", "%", "Adds wear, loss, and instability.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"crosstalk_db", kCrosstalkDb, "Crosstalk", "dB", "Sets channel bleed.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"companding",
             kCompanding,
             "Companding",
             "",
             "Enables the machine's encode/decode compression path.",
             ForgeParamKind::stepped,
             ForgeParamCurve::linear,
             {{"off", "Off", 0}, {"on", "On", 1}}},
            {"print_through_db", kPrintThroughDb, "Print Through", "dB",
             "Sets magnetic echo level.", ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"print_offset_ms",
             kPrintOffsetMs,
             "Print Offset",
             "ms",
             "Sets magnetic echo delay.",
             ForgeParamKind::continuous,
             ForgeParamCurve::logarithmic,
             {},
             {"ampex_7_5ips", "ampex", "studer_7_5ips", "studer", "studer_30ips", "cassette"}},
            {"mix", kMix, "Mix", "%", "Blends dry and tape paths.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
        }};
    d.axes.push_back({"speed_ips",
                      "Tape Speed",
                      "Fixed transport speed in inches per second.",
                      {{"1_875", "1.875 ips", 1.875f},
                       {"7_5", "7.5 ips", 7.5f},
                       {"15", "15 ips", 15.0f},
                       {"30", "30 ips", 30.0f}}});
    d.axes.push_back({"pre_echo",
                      "Pre-Echo",
                      "Whether print-through is rendered before the source event.",
                      {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}});
    const auto add = [&](std::string mode, Archetype archetype, double speed, bool pre_echo,
                         std::string_view archetype_token, std::string_view speed_token) {
        d.realizations.push_back({std::move(mode),
                                  make_tape_machine_node(archetype, speed, pre_echo).type_id,
                                  {{"archetype", archetype_token},
                                   {"speed_ips", speed_token},
                                   {"pre_echo", pre_echo ? "on" : "off"}}});
    };
    add("ampex_7_5ips", Archetype::ampex_350_440, 7.5, false, "ampex", "7_5");
    add("ampex_7_5ips_pre_echo", Archetype::ampex_350_440, 7.5, true, "ampex", "7_5");
    add("ampex", Archetype::ampex_350_440, 15.0, false, "ampex", "15");
    add("ampex_pre_echo", Archetype::ampex_350_440, 15.0, true, "ampex", "15");
    add("studer_7_5ips", Archetype::studer_a800, 7.5, false, "studer", "7_5");
    add("studer_7_5ips_pre_echo", Archetype::studer_a800, 7.5, true, "studer", "7_5");
    add("studer", Archetype::studer_a800, 15.0, false, "studer", "15");
    add("studer_pre_echo", Archetype::studer_a800, 15.0, true, "studer", "15");
    add("studer_30ips", Archetype::studer_a800, 30.0, false, "studer", "30");
    add("studer_30ips_pre_echo", Archetype::studer_a800, 30.0, true, "studer", "30");
    add("cassette", Archetype::cassette_deck, 1.875, false, "cassette", "1_875");
    add("cassette_pre_echo", Archetype::cassette_deck, 1.875, true, "cassette", "1_875");
    return d;
}

} // namespace pulp::host::tape
