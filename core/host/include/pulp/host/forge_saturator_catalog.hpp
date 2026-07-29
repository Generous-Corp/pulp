#pragma once

// Saturation toolkit — bake-layer catalog node.
//
// Wraps pulp::signal::SaturatorT as a lowerable custom node, following the same
// shape as the lo-fi kit's make_waveshaper_node() and the character delay's
// per-realization factory. Where those two differ, this follows the delay: the
// SHAPE is a registration-time realization rather than an injectable param.
//
// That choice is worth stating, because a four-member enum looks like an
// obvious candidate for a stepped knob. Two reasons it is not:
//
//   1. The four curves have genuinely different harmonic signatures and
//      different gain-at-bias behaviour (`cubic_soft` alone clamps its bias, at
//      a different value from the rest). A baked build authored as `sinh_arc`
//      glue should stay glue for the artifact's life, not become a fuzz because
//      a control-thread write moved an enum.
//   2. Crossfading between shapes to make the switch click-free would mean
//      running two shapers, which doubles the cost of the cheapest stage in the
//      catalog to buy a control nobody asked for.
//
// The ALIAS POLICY is likewise a realization, not a param, and for a harder
// reason: it changes `latency_samples()`, and a node whose reported latency
// moves under the audio thread breaks the host's delay compensation.
//
// MONO, one port in and one out. Saturation is a per-sample memoryless function
// with no cross-channel state at all, so a stereo instrument would be exactly
// two independent copies — which is what instancing two nodes already gives,
// without pretending there is a stereo image to preserve.
//
// WET+DRY, unlike the character delay: the module owns its own `mix` because
// the dry path has to be latency-aligned with the wet one, and only the module
// knows what its latency is. Composing an external dry/wet mixer around an
// `oversample_2x` saturator would comb.

#include <pulp/host/forge_param_descriptor.hpp>
#include <pulp/host/signal_graph.hpp>

#include <pulp/signal/saturator.hpp>

#include <cstddef>
#include <cstdint>

namespace pulp::host::saturator {

using Shape = signal::SaturatorShape;
using AliasPolicy = signal::SaturatorAliasPolicy;

// ── Stable type ids ───────────────────────────────────────────────────────
inline constexpr const char* kTanhTypeId = "saturator.tanh";
inline constexpr const char* kAtanTypeId = "saturator.atan";
inline constexpr const char* kCubicTypeId = "saturator.cubic";
inline constexpr const char* kSinhArcTypeId = "saturator.sinh_arc";

// ── Injectable param ids ──────────────────────────────────────────────────
// Node-local; the framework namespaces per node so two nodes never collide.
inline constexpr state::ParamID kDriveDb = 1;      // dB
inline constexpr state::ParamID kBias = 2;         // −1..+1 normalized
inline constexpr state::ParamID kTonePreHz = 3;    // Hz, 0 = off
inline constexpr state::ParamID kMix = 4;          // 0..1
inline constexpr state::ParamID kOutputTrimDb = 5; // dB
inline constexpr state::ParamID kToneTracking = 6; // stepped 0/1
inline constexpr state::ParamID kToneDeHz = 7;     // Hz, used when tracking is off
inline constexpr state::ParamID kPreBoostDb = 8;   // dB

struct SaturatorInstance {
    signal::Saturator saturator;
};

inline const char* saturator_type_id(Shape shape) {
    switch (shape) {
    case Shape::atan_soft:
        return kAtanTypeId;
    case Shape::cubic_soft:
        return kCubicTypeId;
    case Shape::sinh_arc:
        return kSinhArcTypeId;
    case Shape::tanh_soft:
    default:
        return kTanhTypeId;
    }
}

inline const char* saturator_default_name(Shape shape) {
    switch (shape) {
    case Shape::atan_soft:
        return "Saturator (Arctan)";
    case Shape::cubic_soft:
        return "Saturator (Cubic)";
    case Shape::sinh_arc:
        return "Saturator (Sinh-Arc)";
    case Shape::tanh_soft:
    default:
        return "Saturator (Tanh)";
    }
}

inline const char* alias_policy_id(AliasPolicy policy) noexcept {
    switch (policy) {
    case AliasPolicy::oversample_2x:
        return "x2";
    case AliasPolicy::off:
        return "off";
    case AliasPolicy::adaa:
    default:
        return "adaa";
    }
}

inline Shape normalized_shape(Shape shape) noexcept {
    switch (shape) {
    case Shape::tanh_soft:
    case Shape::atan_soft:
    case Shape::cubic_soft:
    case Shape::sinh_arc:
        return shape;
    default:
        return Shape::tanh_soft;
    }
}

inline AliasPolicy normalized_alias_policy(AliasPolicy policy) noexcept {
    switch (policy) {
    case AliasPolicy::adaa:
    case AliasPolicy::oversample_2x:
    case AliasPolicy::off:
        return policy;
    default:
        return AliasPolicy::adaa;
    }
}

/// The largest gain this realization can present, for the Forge registry's
/// `worst_case_gain` row (series law 8).
///
/// Exactly 1.0 while bias is zero, because every shape is globally compressive
/// past the origin. With bias it is `1 / f'(b)`, which the DSP computes and its
/// test suite asserts across the full drive/bias grid. This function reports the
/// bound at the node's own bias CEILING, since a baked param can be automated
/// anywhere in its declared range and the registry entry has to cover all of it.
inline float saturator_worst_case_gain(Shape shape) {
    signal::Saturator probe;
    probe.set_shape(shape);
    probe.set_bias(1.0); // clamped per shape by the DSP
    return static_cast<float>(probe.worst_case_gain());
}

/// One factory, four registered realizations.
///
/// `alias_policy` is a construction-time choice because it determines
/// `latency_samples()`; see the header note.
inline CustomNodeType make_saturator_node(Shape shape,
                                          AliasPolicy alias_policy = AliasPolicy::adaa) {
    const Shape fixed_shape = normalized_shape(shape);
    const AliasPolicy fixed_alias_policy = normalized_alias_policy(alias_policy);
    CustomNodeType t;
    t.type_id = saturator_type_id(fixed_shape);
    if (fixed_alias_policy != AliasPolicy::adaa)
        t.type_id += std::string{"."} + alias_policy_id(fixed_alias_policy);
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = saturator_default_name(fixed_shape);
    t.lowerable = true;
    t.latency_samples = [fixed_shape, fixed_alias_policy](double sample_rate) {
        signal::Saturator probe;
        probe.set_shape(fixed_shape);
        probe.set_alias_policy(fixed_alias_policy);
        probe.prepare(sample_rate);
        return probe.latency_samples();
    };

    t.create = []() -> void* { return new SaturatorInstance{}; };
    t.destroy = [](void* p) { delete static_cast<SaturatorInstance*>(p); };
    t.prepare = [fixed_shape, fixed_alias_policy](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<SaturatorInstance*>(p);
        s->saturator.set_shape(fixed_shape);
        s->saturator.set_alias_policy(fixed_alias_policy);
        s->saturator.prepare(sr);
    };
    t.reset = [](void* p) { static_cast<SaturatorInstance*>(p)->saturator.reset(); };

    // Ranges and defaults are the module's canonical contract, in REAL units;
    // the Forge layer mirrors them and owns the display curve. Every row here
    // is a design-parameter declaration per the series contract.
    using Sat = signal::Saturator;
    t.baked_params.push_back({kDriveDb, static_cast<float>(Sat::kDriveDbMin),
                              static_cast<float>(Sat::kDriveDbMax),
                              static_cast<float>(Sat::kDriveDbDefault)});
    t.baked_params.push_back({kBias, -1.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kTonePreHz, 0.0f, static_cast<float>(Sat::kTonePreHzMax),
                              static_cast<float>(Sat::kTonePreHzDefault)});
    t.baked_params.push_back({kMix, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back({kOutputTrimDb, -static_cast<float>(Sat::kOutputTrimDbMax),
                              static_cast<float>(Sat::kOutputTrimDbMax), 0.0f});
    t.baked_params.push_back({kToneTracking, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back({kToneDeHz, 0.0f, static_cast<float>(Sat::kTonePreHzMax),
                              static_cast<float>(Sat::kTonePreHzDefault)});
    t.baked_params.push_back({kPreBoostDb, 0.0f, static_cast<float>(Sat::kPreBoostDbMax),
                              static_cast<float>(Sat::kPreBoostDbDefault)});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<SaturatorInstance*>(p);
        const float* input = in.channel_ptr(0);
        float* output = out.channel_ptr(0);

        // Sample-at-a-time so every knob is sample-accurate. The drive and bias
        // setters recompute three doubles; the tone setter redesigns a biquad,
        // which is why the tone corner is expected to be an automation-rare
        // control rather than a per-sample sweep. Both are still cheaper than
        // the transcendental the shaper itself evaluates.
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->saturator.set_drive_db(params.value_at(kDriveDb, offset));
            s->saturator.set_bias(params.value_at(kBias, offset));
            s->saturator.set_tone_pre_hz(params.value_at(kTonePreHz, offset));
            s->saturator.set_tone_tracking(params.value_at(kToneTracking, offset) >= 0.5f);
            s->saturator.set_tone_de_hz(params.value_at(kToneDeHz, offset));
            s->saturator.set_pre_boost_db(params.value_at(kPreBoostDb, offset));
            s->saturator.set_mix(params.value_at(kMix, offset));
            s->saturator.set_output_trim_db(params.value_at(kOutputTrimDb, offset));
            output[static_cast<std::size_t>(k)] =
                s->saturator.process(input[static_cast<std::size_t>(k)]);
        }
    };
    return t;
}

inline ForgeNodeDescriptor saturator_descriptor() {
    return {
        "saturator",
        "Saturator",
        "Memoryless waveshaping with four harmonic curves and a construction-time "
        "anti-aliasing policy.",
        {{"shape",
          "Shape",
          "Selects the fixed transfer curve and harmonic character.",
          {{"tanh", "Tanh", 0.0f},
           {"atan", "Arctan", 1.0f},
           {"cubic", "Cubic", 2.0f},
           {"sinh_arc", "Sinh-Arc", 3.0f}}},
         {"alias_policy",
          "Alias Policy",
          "Selects antiderivative anti-aliasing, two-times oversampling, or the raw curve.",
          {{"adaa", "ADAA", 0.0f}, {"x2", "2x Oversampling", 1.0f}, {"off", "Off", 2.0f}}}},
        {{"tanh_adaa", "saturator.tanh", {{"shape", "tanh"}, {"alias_policy", "adaa"}}},
         {"tanh_x2", "saturator.tanh.x2", {{"shape", "tanh"}, {"alias_policy", "x2"}}},
         {"tanh_off", "saturator.tanh.off", {{"shape", "tanh"}, {"alias_policy", "off"}}},
         {"atan_adaa", "saturator.atan", {{"shape", "atan"}, {"alias_policy", "adaa"}}},
         {"atan_x2", "saturator.atan.x2", {{"shape", "atan"}, {"alias_policy", "x2"}}},
         {"atan_off", "saturator.atan.off", {{"shape", "atan"}, {"alias_policy", "off"}}},
         {"cubic_adaa", "saturator.cubic", {{"shape", "cubic"}, {"alias_policy", "adaa"}}},
         {"cubic_x2", "saturator.cubic.x2", {{"shape", "cubic"}, {"alias_policy", "x2"}}},
         {"cubic_off", "saturator.cubic.off", {{"shape", "cubic"}, {"alias_policy", "off"}}},
         {"sinh_arc_adaa",
          "saturator.sinh_arc",
          {{"shape", "sinh_arc"}, {"alias_policy", "adaa"}}},
         {"sinh_arc_x2",
          "saturator.sinh_arc.x2",
          {{"shape", "sinh_arc"}, {"alias_policy", "x2"}}},
         {"sinh_arc_off",
          "saturator.sinh_arc.off",
          {{"shape", "sinh_arc"}, {"alias_policy", "off"}}}},
        {{"drive_db",
          kDriveDb,
          "Drive",
          "dB",
          "Input gain applied before the transfer curve.",
          ForgeParamKind::continuous,
          ForgeParamCurve::linear,
          {},
          {}},
         {"bias",
          kBias,
          "Bias",
          "",
          "Offsets the transfer curve to introduce asymmetric harmonics.",
          ForgeParamKind::continuous,
          ForgeParamCurve::linear,
          {},
          {}},
         {"tone_pre_hz",
          kTonePreHz,
          "Pre Tone",
          "Hz",
          "Sets the pre-emphasis corner; zero disables the stage.",
          ForgeParamKind::continuous,
          ForgeParamCurve::logarithmic,
          {},
          {}},
         {"mix",
          kMix,
          "Mix",
          "%",
          "Blends the latency-aligned dry and saturated signals.",
          ForgeParamKind::continuous,
          ForgeParamCurve::linear,
          {},
          {}},
         {"output_trim_db",
          kOutputTrimDb,
          "Output Trim",
          "dB",
          "Applies gain after the wet/dry blend.",
          ForgeParamKind::continuous,
          ForgeParamCurve::linear,
          {},
          {}},
         {"tone_tracking",
          kToneTracking,
          "Tone Tracking",
          "",
          "Makes the de-emphasis corner track the pre-emphasis corner.",
          ForgeParamKind::stepped,
          ForgeParamCurve::linear,
          {{"off", "Off", 0.0f}, {"on", "On", 1.0f}},
          {}},
         {"tone_de_hz",
          kToneDeHz,
          "De-emphasis",
          "Hz",
          "Sets the independent post-shaper de-emphasis corner when tracking is off.",
          ForgeParamKind::continuous,
          ForgeParamCurve::logarithmic,
          {},
          {}},
         {"pre_boost_db",
          kPreBoostDb,
          "Pre Boost",
          "dB",
          "Boosts the pre-emphasis stage before nonlinear shaping.",
          ForgeParamKind::continuous,
          ForgeParamCurve::linear,
          {},
          {}}},
    };
}

} // namespace pulp::host::saturator
