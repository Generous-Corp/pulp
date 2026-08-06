#pragma once

// Multiband — Forge-facing bake-layer catalog nodes.
//
// The shipped multiband contract is composition: a Linkwitz-Riley crossover
// followed by independent shipped Compressor instances. This two-band node is
// that composition made directly lowerable, not a parallel DSP implementation.

#include <pulp/host/forge_param_descriptor.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/signal/compressor.hpp>
#include <pulp/signal/linkwitz_riley.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pulp::host::multiband {

inline constexpr const char* kTypeId = "multiband.compressor";

inline constexpr state::ParamID kCrossoverHz = 1;
inline constexpr state::ParamID kLowThresholdDb = 2;
inline constexpr state::ParamID kLowRatio = 3;
inline constexpr state::ParamID kHighThresholdDb = 4;
inline constexpr state::ParamID kHighRatio = 5;
inline constexpr state::ParamID kAttackMs = 6;
inline constexpr state::ParamID kReleaseMs = 7;
inline constexpr state::ParamID kLowMakeupDb = 8;
inline constexpr state::ParamID kHighMakeupDb = 9;

struct Instance {
    signal::LinkwitzRiley crossover;
    signal::Compressor low;
    signal::Compressor high;
    float sample_rate = 48000.0f;
};

namespace detail {

inline signal::Compressor::Params compressor_params(float threshold_db, float ratio,
                                                    float attack_ms, float release_ms,
                                                    float makeup_db) {
    signal::Compressor::Params p;
    p.threshold_db = threshold_db;
    p.ratio = std::max(1.0f, ratio);
    p.attack_ms = attack_ms;
    p.release_ms = release_ms;
    p.knee_db = 6.0f;
    p.makeup_db = makeup_db;
    return p;
}

} // namespace detail

inline CustomNodeType make_multiband_compressor_node() {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Multiband Compressor";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [](void* p, double sample_rate, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->sample_rate = static_cast<float>(sample_rate);
        s->crossover.set_frequency(800.0f, s->sample_rate);
        s->low.set_sample_rate(s->sample_rate);
        s->high.set_sample_rate(s->sample_rate);
        s->crossover.reset();
        s->low.reset();
        s->high.reset();
    };
    t.reset = [](void* p) {
        auto* s = static_cast<Instance*>(p);
        s->crossover.reset();
        s->low.reset();
        s->high.reset();
    };

    t.baked_params.push_back({kCrossoverHz, 80.0f, 8000.0f, 800.0f});
    t.baked_params.push_back({kLowThresholdDb, -60.0f, 0.0f, -18.0f});
    t.baked_params.push_back({kLowRatio, 1.0f, 20.0f, 4.0f});
    t.baked_params.push_back({kHighThresholdDb, -60.0f, 0.0f, -18.0f});
    t.baked_params.push_back({kHighRatio, 1.0f, 20.0f, 4.0f});
    t.baked_params.push_back({kAttackMs, 0.1f, 100.0f, 10.0f});
    t.baked_params.push_back({kReleaseMs, 10.0f, 1000.0f, 150.0f});
    t.baked_params.push_back({kLowMakeupDb, -24.0f, 12.0f, 0.0f});
    t.baked_params.push_back({kHighMakeupDb, -24.0f, 12.0f, 0.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* input = in.channel_ptr(0);
        float* output = out.channel_ptr(0);

        s->crossover.set_frequency(params.value_at(kCrossoverHz, 0), s->sample_rate);
        s->low.set_params(detail::compressor_params(
            params.value_at(kLowThresholdDb, 0), params.value_at(kLowRatio, 0),
            params.value_at(kAttackMs, 0), params.value_at(kReleaseMs, 0),
            params.value_at(kLowMakeupDb, 0)));
        s->high.set_params(detail::compressor_params(
            params.value_at(kHighThresholdDb, 0), params.value_at(kHighRatio, 0),
            params.value_at(kAttackMs, 0), params.value_at(kReleaseMs, 0),
            params.value_at(kHighMakeupDb, 0)));

        for (int k = 0; k < n; ++k) {
            const auto split = s->crossover.process(input[static_cast<std::size_t>(k)]);
            output[static_cast<std::size_t>(k)] =
                s->low.process(split.low) + s->high.process(split.high);
        }
    };
    return t;
}

inline ForgeNodeDescriptor descriptor() {
    return {
        "multiband_compressor",
        "Multiband Compressor",
        "A two-band compressor with independent low- and high-band dynamics.",
        {},
        {{"default", kTypeId}},
        {
            {"crossover_hz", kCrossoverHz, "Crossover", "Hz",
             "Frequency that divides the low and high compression bands.",
             ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
            {"low_threshold_db", kLowThresholdDb, "Low Threshold", "dB",
             "Level above which the low band is compressed.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
            {"low_ratio", kLowRatio, "Low Ratio", ":1",
             "Gain-reduction ratio for the low band.", ForgeParamKind::continuous,
             ForgeParamCurve::logarithmic},
            {"high_threshold_db", kHighThresholdDb, "High Threshold", "dB",
             "Level above which the high band is compressed.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
            {"high_ratio", kHighRatio, "High Ratio", ":1",
             "Gain-reduction ratio for the high band.", ForgeParamKind::continuous,
             ForgeParamCurve::logarithmic},
            {"attack_ms", kAttackMs, "Attack", "ms",
             "Time for compression to engage in both bands.", ForgeParamKind::continuous,
             ForgeParamCurve::logarithmic},
            {"release_ms", kReleaseMs, "Release", "ms",
             "Time for compression to recover in both bands.", ForgeParamKind::continuous,
             ForgeParamCurve::logarithmic},
            {"low_makeup_db", kLowMakeupDb, "Low Makeup", "dB",
             "Output gain applied after low-band compression.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
            {"high_makeup_db", kHighMakeupDb, "High Makeup", "dB",
             "Output gain applied after high-band compression.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
        },
    };
}

inline float worst_case_gain() {
    return 2.0f * std::pow(10.0f, 12.0f / 20.0f);
}

} // namespace pulp::host::multiband
