#include <pulp/host/forge_distortion_catalog.hpp>

namespace pulp::host::distortion {

int oversample_factor(OversampleTier tier) {
    switch (tier) {
        case OversampleTier::x2: return 2;
        case OversampleTier::x4: return 4;
        case OversampleTier::x8: return 8;
        case OversampleTier::x1:
        default: return 1;
    }
}

const char* distortion_type_id(Topology topology, OversampleTier tier) {
    if (topology == Topology::to_ground) {
        switch (tier) {
            case OversampleTier::x2: return kToGroundX2TypeId;
            case OversampleTier::x4: return kToGroundX4TypeId;
            case OversampleTier::x8: return kToGroundX8TypeId;
            case OversampleTier::x1:
            default: return kToGroundX1TypeId;
        }
    }
    switch (tier) {
        case OversampleTier::x2: return kInLoopX2TypeId;
        case OversampleTier::x4: return kInLoopX4TypeId;
        case OversampleTier::x8: return kInLoopX8TypeId;
        case OversampleTier::x1:
        default: return kInLoopX1TypeId;
    }
}

const char* distortion_default_name(Topology topology) {
    return topology == Topology::to_ground ? "Distortion" : "Overdrive";
}

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// The clipper cannot amplify: at zero signal the diodes are an open circuit
/// and the stage's gain is its linear `Rf/Rin`; as they conduct they shunt
/// feedback current and gain only falls. The module's own suite asserts that
/// monotonic non-increase across a swept-amplitude sweep. So the node's bound
/// is the DRIVE and OUTPUT trims either side of it, which are plain gains:
/// `10^((drive_max + pre_gain_max + output_max)/20)`.
///
/// The drive is included because it sits BEFORE the clipper, where at small
/// signals — below the diode knee — it passes through un-clipped. The pre-tone
/// shelf can add another 12 dB above its corner, so it belongs in the same
/// composed bound. A bound that omitted either pre-clip gain would be wrong for
/// exactly the quiet material a path-gain lint most needs to reason about.
float distortion_worst_case_gain() {
    return static_cast<float>(signal::units::db_to_linear(
        static_cast<double>(kDriveDbMax + signal::ToneStack::kPreGainDbMax + kOutputDbMax)));
}

/// One factory, eight registered realizations (2 topologies × 4 tiers).
CustomNodeType make_distortion_node(Topology topology,
                                    OversampleTier tier) {
    CustomNodeType t;
    t.type_id = distortion_type_id(topology, tier);
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = distortion_default_name(topology);
    t.lowerable = true;
    t.latency_samples = [tier](double sample_rate) {
        if (tier == OversampleTier::x1) return 0;
        using Os = signal::OversamplerT<float>;
        Os probe;
        probe.set_kind(Os::Kind::linear_phase_fir);
        probe.set_quality(Os::Quality::standard);
        const int factor = oversample_factor(tier);
        probe.set_factor(factor == 2 ? Os::Factor::x2
                         : factor == 4 ? Os::Factor::x4
                                       : Os::Factor::x8);
        probe.set_sample_rate(static_cast<float>(sample_rate));
        return probe.latency_samples();
    };

    t.create = []() -> void* { return new DistortionInstance{}; };
    t.destroy = [](void* p) { delete static_cast<DistortionInstance*>(p); };
    t.prepare = [tier](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<DistortionInstance*>(p);
        const int factor = oversample_factor(tier);
        const double inner_rate = sr * factor;

        s->loop_clipper.set_topology(signal::ClipperTopology::in_loop);
        // ×1 has NO antialiasing. It used to enable ADAA on both clippers,
        // which diverged to 5.8e25 on a full-scale input — see the note on
        // `DiodeClipperT` for why that path was structurally unsound rather
        // than merely buggy. Oversampling is how this node antialiases; ×1 is
        // the zero-latency tier and is honest about costing you that.
        s->ground_clipper.prepare(inner_rate);
        s->loop_clipper.prepare(inner_rate);
        s->tone.prepare(inner_rate);

        using Os = signal::OversamplerT<float>;
        if (factor > 1) {
            s->oversampler.set_kind(Os::Kind::linear_phase_fir);
            s->oversampler.set_quality(Os::Quality::standard);
            s->oversampler.set_factor(factor == 2   ? Os::Factor::x2
                                      : factor == 4 ? Os::Factor::x4
                                                    : Os::Factor::x8);
            s->oversampler.set_sample_rate(static_cast<float>(sr));
            s->latency = s->oversampler.latency_samples();
        } else {
            s->latency = 0;
        }
    };
    t.reset = [](void* p) {
        auto* s = static_cast<DistortionInstance*>(p);
        s->ground_clipper.reset();
        s->loop_clipper.reset();
        s->tone.reset();
        s->oversampler.reset();
    };

    // Real units; each row is a design-parameter declaration per the series
    // contract.
    t.baked_params.push_back({kDriveDb, kDriveDbMin, kDriveDbMax, kDriveDbDefault});
    t.baked_params.push_back({kSymmetry, -1.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kDiodeModel, 0.0f, 2.0f, 0.0f});
    t.baked_params.push_back({kPreToneHz, 200.0f, 8000.0f, 720.0f});
    t.baked_params.push_back({kPreGainDb,
                              static_cast<float>(-signal::ToneStack::kPreGainDbMax),
                              static_cast<float>(signal::ToneStack::kPreGainDbMax), 0.0f});
    t.baked_params.push_back({kPostToneHz, 500.0f, 12000.0f, 4000.0f});
    t.baked_params.push_back({kToneMix, 0.0f, 1.0f, 0.5f});
    t.baked_params.push_back({kOutputDb, kOutputDbMin, kOutputDbMax, 0.0f});

    t.process_instance_baked_param = [tier, topology](void* p, audio::BufferView<float>& out,
                                            const audio::BufferView<const float>& in, int n,
                                            const BakedParamView& params) {
        auto* s = static_cast<DistortionInstance*>(p);
        const float* input = in.channel_ptr(0);
        float* output = out.channel_ptr(0);
        const int factor = oversample_factor(tier);

        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->drive = static_cast<float>(
                signal::units::db_to_linear(params.value_at(kDriveDb, offset)));
            s->output = static_cast<float>(
                signal::units::db_to_linear(params.value_at(kOutputDb, offset)));
            const auto model = static_cast<DiodeModel>(
                static_cast<int>(params.value_at(kDiodeModel, offset) + 0.5f));
            const float symmetry = params.value_at(kSymmetry, offset);
            s->ground_clipper.set_symmetry(symmetry);
            s->ground_clipper.set_diode_model(model);
            s->loop_clipper.set_symmetry(symmetry);
            s->loop_clipper.set_diode_model(model);
            s->tone.set_pre_tone_hz(params.value_at(kPreToneHz, offset));
            s->tone.set_pre_gain_db(params.value_at(kPreGainDb, offset));
            s->tone.set_post_tone_hz(params.value_at(kPostToneHz, offset));
            s->tone.set_tone_mix(params.value_at(kToneMix, offset));

            // The one nonlinear chain, evaluated either directly (×1, where the
            // clipper's own ADAA is the antialiasing) or inside the oversampler.
            const bool ground = topology == Topology::to_ground;
            const auto stage = [s, ground](float x) {
                const float driven = s->tone.process_pre(x * s->drive);
                const float clipped =
                    ground ? s->ground_clipper.process(driven) : s->loop_clipper.process(driven);
                return s->tone.process_post(clipped);
            };

            const float x = input[static_cast<std::size_t>(k)];
            const float wet = factor == 1 ? stage(x) : s->oversampler.process(x, stage);
            output[static_cast<std::size_t>(k)] = wet * s->output;
        }
    };
    return t;
}

ForgeNodeDescriptor distortion_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "distortion";
    d.label = "Distortion";
    d.description = "Circuit-modeled diode distortion with selectable clipping topology and "
                    "antialiasing quality.";
    d.axes = {
        {"topology",
         "Topology",
         "Whether the diode pair clips to ground or sits in an amplifier feedback loop.",
         {{"to_ground", "To Ground", 0.0f}, {"in_loop", "In Loop", 1.0f}}},
        {"oversampling",
         "Oversampling",
         "Antialiasing quality and its corresponding fixed latency.",
         {{"x1", "1x", 1.0f}, {"x2", "2x", 2.0f}, {"x4", "4x", 4.0f}, {"x8", "8x", 8.0f}}},
    };
    d.realizations = {
        {"to_ground_x1", kToGroundX1TypeId,
         {{"topology", "to_ground"}, {"oversampling", "x1"}}},
        {"to_ground_x2", kToGroundX2TypeId,
         {{"topology", "to_ground"}, {"oversampling", "x2"}}},
        {"to_ground_x4", kToGroundX4TypeId,
         {{"topology", "to_ground"}, {"oversampling", "x4"}}},
        {"to_ground_x8", kToGroundX8TypeId,
         {{"topology", "to_ground"}, {"oversampling", "x8"}}},
        {"in_loop_x1", kInLoopX1TypeId,
         {{"topology", "in_loop"}, {"oversampling", "x1"}}},
        {"in_loop_x2", kInLoopX2TypeId,
         {{"topology", "in_loop"}, {"oversampling", "x2"}}},
        {"in_loop_x4", kInLoopX4TypeId,
         {{"topology", "in_loop"}, {"oversampling", "x4"}}},
        {"in_loop_x8", kInLoopX8TypeId,
         {{"topology", "in_loop"}, {"oversampling", "x8"}}},
    };
    d.params = {
        {"drive_db", kDriveDb, "Drive", "dB", "Gain driving the diode clipper.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"symmetry", kSymmetry, "Symmetry", "",
         "Offsets positive and negative clipping thresholds for asymmetric harmonics.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"diode_model",
         kDiodeModel,
         "Diode",
         "",
         "Diode material and its characteristic forward-voltage curve.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"silicon", "Silicon", 0.0f}, {"germanium", "Germanium", 1.0f}, {"led", "LED", 2.0f}}},
        {"pre_tone_hz", kPreToneHz, "Pre Tone", "Hz",
         "Tone-shaping frequency before the clipping stage.", ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic},
        {"pre_gain_db", kPreGainDb, "Pre Gain", "dB",
         "Input trim before the pre-tone and drive stages.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"post_tone_hz", kPostToneHz, "Post Tone", "Hz",
         "Tone-shaping frequency after the clipping stage.", ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic},
        {"tone_mix", kToneMix, "Tone Mix", "%",
         "Blend between the pre- and post-clipping tone paths.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"output_db", kOutputDb, "Output", "dB", "Output trim after distortion and tone shaping.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
    };
    return d;
}

/// The node's reported latency for a tier, as the composed oversampler reports
/// it. Zero at ×1 — the ADAA path has no filter — and the exact integer group
/// delay of the cascaded half-band pairs above that. Reported, never estimated
/// (series law 5).
int distortion_latency_samples(OversampleTier tier, double sample_rate) {
    if (tier == OversampleTier::x1) return 0;
    using Os = signal::OversamplerT<float>;
    Os probe;
    probe.set_kind(Os::Kind::linear_phase_fir);
    probe.set_quality(Os::Quality::standard);
    const int factor = oversample_factor(tier);
    probe.set_factor(factor == 2 ? Os::Factor::x2 : factor == 4 ? Os::Factor::x4 : Os::Factor::x8);
    probe.set_sample_rate(static_cast<float>(sample_rate));
    return probe.latency_samples();
}

}  // namespace pulp::host::distortion
