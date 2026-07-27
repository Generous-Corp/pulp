#pragma once

// Distortion — bake-layer catalog node.
//
// Composes the circuit-modelled clipper family into one node:
//
//   input trim → pre-tone → drive → CLIPPER → post-tone → output trim
//
// with the clipper (and only the clipper) running inside the house
// oversampling wrapper. The tone stages ride at the oversampled rate too, but
// only because they sit between the rails of one prepare/process pair — they are
// linear and carry no antialiasing requirement of their own.
//
// TOPOLOGY IS A REGISTRATION-TIME REALIZATION, like the saturator's curve and
// unlike the compressor's detector mode. `to_ground` and `in_loop` are different
// circuits, not different coefficients: one clips a fixed voltage after a gain
// stage, the other clips inside a feedback loop whose capacitor makes the clip
// frequency-dependent. A baked build authored as an overdrive should stay an
// overdrive.
//
// THE OVERSAMPLING TIER IS ALSO A REALIZATION, and for the harder reason: it
// determines `latency_samples()`. A node whose reported latency moves under the
// audio thread breaks the host's delay compensation. ×1 is the zero-latency
// no antialiasing; ×2/×4/×8 report the composed oversampler's own exact group delay.
//
// MONO, one port in and one out. Every stage here is per-sample with no
// cross-channel state, so a stereo instrument would be two independent copies —
// which is what instancing two nodes already gives.

#include <pulp/host/signal_graph.hpp>

#include <pulp/signal/distortion.hpp>
#include <pulp/signal/oversampling.hpp>
#include <pulp/signal/units.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace pulp::host::distortion {

using DiodeModel = signal::DiodeModel;
using Topology = signal::ClipperTopology;

/// Oversampling tiers this node registers. ×1 is the zero-latency tier
/// and does NOT antialias.
enum class OversampleTier : std::uint8_t { x1, x2, x4, x8 };

// ── Stable type ids ───────────────────────────────────────────────────────
// One id per (topology, tier) pair, because both are frozen at registration.
inline constexpr const char* kToGroundX1TypeId = "distortion.to_ground.x1";
inline constexpr const char* kToGroundX2TypeId = "distortion.to_ground.x2";
inline constexpr const char* kToGroundX4TypeId = "distortion.to_ground.x4";
inline constexpr const char* kToGroundX8TypeId = "distortion.to_ground.x8";
inline constexpr const char* kInLoopX1TypeId = "distortion.in_loop.x1";
inline constexpr const char* kInLoopX2TypeId = "distortion.in_loop.x2";
inline constexpr const char* kInLoopX4TypeId = "distortion.in_loop.x4";
inline constexpr const char* kInLoopX8TypeId = "distortion.in_loop.x8";

// ── Injectable param ids ──────────────────────────────────────────────────
inline constexpr state::ParamID kDriveDb = 1;       // dB
inline constexpr state::ParamID kSymmetry = 2;      // −1..+1
inline constexpr state::ParamID kDiodeModel = 3;    // stepped 0=Si, 1=Ge, 2=LED
inline constexpr state::ParamID kPreToneHz = 4;     // Hz
inline constexpr state::ParamID kPostToneHz = 5;    // Hz
inline constexpr state::ParamID kToneMix = 6;       // 0..1
inline constexpr state::ParamID kOutputDb = 7;      // dB
// Added after the original seven IDs shipped on the feature branch. Keep it at
// 8 so saved graphs retain the meaning of every existing parameter ID.
inline constexpr state::ParamID kPreGainDb = 8;     // dB

/// Drive range. The primary "how hard" control.
/// [design parameter] default 12 dB, range 0 .. 40 dB.
inline constexpr float kDriveDbMin = 0.0f;
inline constexpr float kDriveDbMax = 40.0f;
inline constexpr float kDriveDbDefault = 12.0f;

/// Output trim. [design parameter] default 0 dB, range −24 .. +12 dB.
inline constexpr float kOutputDbMin = -24.0f;
inline constexpr float kOutputDbMax = 12.0f;

struct DistortionInstance {
    // Two clipper realizations, one selected per registration.
    //
    // `to_ground` uses DiodeClipperT DIRECTLY rather than FeedbackClipperT's
    // to-ground mode, because that mode bundles the op-amp's own Rf/Rin gain —
    // 10.85× at the defaults. Bundling it here would double-count against the
    // node's `drive_db`: the minimum drive setting would already be 20.7 dB of
    // gain, so the node would arrive fully clipped at drive 0 and the knob
    // would do nothing but change how square the result is. The spec's chain is
    // `drive → clipper`, and this is that chain.
    signal::DiodeClipper ground_clipper;
    signal::FeedbackClipper loop_clipper;
    signal::ToneStack tone;
    signal::OversamplerT<float> oversampler;
    float drive = 1.0f;
    float output = 1.0f;
    int latency = 0;
};

inline int oversample_factor(OversampleTier tier) {
    switch (tier) {
        case OversampleTier::x2: return 2;
        case OversampleTier::x4: return 4;
        case OversampleTier::x8: return 8;
        case OversampleTier::x1:
        default: return 1;
    }
}

inline const char* distortion_type_id(Topology topology, OversampleTier tier) {
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

inline const char* distortion_default_name(Topology topology) {
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
inline float distortion_worst_case_gain() {
    return static_cast<float>(signal::units::db_to_linear(
        static_cast<double>(kDriveDbMax + signal::ToneStack::kPreGainDbMax + kOutputDbMax)));
}

/// One factory, eight registered realizations (2 topologies × 4 tiers).
inline CustomNodeType make_distortion_node(Topology topology,
                                           OversampleTier tier = OversampleTier::x4) {
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

/// The node's reported latency for a tier, as the composed oversampler reports
/// it. Zero at ×1 — the ADAA path has no filter — and the exact integer group
/// delay of the cascaded half-band pairs above that. Reported, never estimated
/// (series law 5).
inline int distortion_latency_samples(OversampleTier tier, double sample_rate) {
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
