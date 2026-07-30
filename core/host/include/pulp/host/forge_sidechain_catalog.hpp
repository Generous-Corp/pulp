#pragma once

// Sidechain — Forge-facing bake-layer catalog nodes.
//
// Port 0 is the audible signal and port 1 is the external detector/key. The
// implementation is the shipped Compressor::process_with_sidechain path.

#include <pulp/host/forge_param_descriptor.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/signal/compressor.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pulp::host::sidechain {

inline constexpr const char* kTypeId = "sidechain.compressor";
inline constexpr PortIndex kSignalPort = 0;
inline constexpr PortIndex kKeyPort = 1;

inline constexpr state::ParamID kThresholdDb = 1;
inline constexpr state::ParamID kRatio = 2;
inline constexpr state::ParamID kAttackMs = 3;
inline constexpr state::ParamID kReleaseMs = 4;
inline constexpr state::ParamID kKneeDb = 5;
inline constexpr state::ParamID kMakeupDb = 6;
inline constexpr state::ParamID kKeyHpfHz = 7;

struct Instance {
    signal::Compressor compressor;
    float key_hpf_hz = -1.0f;
};

inline CustomNodeType make_sidechain_compressor_node() {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 2;
    t.num_output_ports = 1;
    t.default_name = "Sidechain Compressor";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [](void* p, double sample_rate, int /*max_block*/) {
        static_cast<Instance*>(p)->compressor.set_sample_rate(static_cast<float>(sample_rate));
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->compressor.reset(); };

    t.baked_params.push_back({kThresholdDb, -60.0f, 0.0f, -18.0f});
    t.baked_params.push_back({kRatio, 1.0f, 20.0f, 4.0f});
    t.baked_params.push_back({kAttackMs, 0.1f, 100.0f, 10.0f});
    t.baked_params.push_back({kReleaseMs, 10.0f, 1000.0f, 150.0f});
    t.baked_params.push_back({kKneeDb, 0.0f, 24.0f, 6.0f});
    t.baked_params.push_back({kMakeupDb, -24.0f, 12.0f, 0.0f});
    t.baked_params.push_back({kKeyHpfHz, 0.0f, 2000.0f, 0.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* main_input = in.channel_ptr(kSignalPort);
        const float* key = in.channel_ptr(kKeyPort);
        float* output = out.channel_ptr(0);

        signal::Compressor::Params cp;
        cp.threshold_db = params.value_at(kThresholdDb, 0);
        cp.ratio = std::max(1.0f, params.value_at(kRatio, 0));
        cp.attack_ms = params.value_at(kAttackMs, 0);
        cp.release_ms = params.value_at(kReleaseMs, 0);
        cp.knee_db = params.value_at(kKneeDb, 0);
        cp.makeup_db = params.value_at(kMakeupDb, 0);
        s->compressor.set_params(cp);
        const float key_hpf_hz = params.value_at(kKeyHpfHz, 0);
        if (key_hpf_hz != s->key_hpf_hz) {
            s->key_hpf_hz = key_hpf_hz;
            s->compressor.set_sidechain_hpf_hz(key_hpf_hz);
        }

        for (int k = 0; k < n; ++k) {
            output[static_cast<std::size_t>(k)] = s->compressor.process_with_sidechain(
                main_input[static_cast<std::size_t>(k)], key[static_cast<std::size_t>(k)]);
        }
    };
    return t;
}

inline ForgeNodeDescriptor descriptor() {
    return {
        "sidechain_compressor",
        "Sidechain Compressor",
        "A compressor whose gain reduction follows a separate external key signal.",
        {},
        {{"default", kTypeId}},
        {
            {"threshold_db", kThresholdDb, "Threshold", "dB",
             "Key-signal level above which gain reduction begins.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"ratio", kRatio, "Ratio", ":1",
             "Gain-reduction ratio above threshold.", ForgeParamKind::continuous,
             ForgeParamCurve::logarithmic},
            {"attack_ms", kAttackMs, "Attack", "ms",
             "Time for compression to engage.", ForgeParamKind::continuous,
             ForgeParamCurve::logarithmic},
            {"release_ms", kReleaseMs, "Release", "ms",
             "Time for compression to recover.", ForgeParamKind::continuous,
             ForgeParamCurve::logarithmic},
            {"knee_db", kKneeDb, "Knee", "dB",
             "Width of the soft-knee transition.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
            {"makeup_db", kMakeupDb, "Makeup", "dB",
             "Output gain applied after compression.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
            {"key_hpf_hz", kKeyHpfHz, "Key HPF", "Hz",
             "High-pass cutoff that reduces low-frequency key sensitivity; zero disables it.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
        },
    };
}

inline float worst_case_gain() {
    return std::pow(10.0f, 12.0f / 20.0f);
}

} // namespace pulp::host::sidechain
