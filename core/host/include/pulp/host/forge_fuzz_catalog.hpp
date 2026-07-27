#pragma once

// FuzzPair bake catalog. Device chemistry and oversampling are realizations:
// both are circuit identity, and oversampling changes reported latency.

#include <pulp/host/signal_graph.hpp>
#include <pulp/signal/fuzz_pair.hpp>

#include <cstdint>

namespace pulp::host::fuzz {

using Device = signal::FuzzDevice;
inline constexpr state::ParamID kFuzz = 1;
inline constexpr state::ParamID kBiasStarve = 2;
inline constexpr state::ParamID kSourceKohm = 3;
inline constexpr state::ParamID kOutputDb = 4;
inline constexpr state::ParamID kMix = 5;
inline constexpr state::ParamID kDriftEnabled = 6;
inline constexpr std::uint32_t kDeterministicDriftSeed = 0x46555a5au;

struct Instance { signal::FuzzPair fuzz; };

inline const char* type_id(Device device, bool oversampled) noexcept {
    if (device == Device::silicon) return oversampled ? "fuzz.silicon.x4" : "fuzz.silicon.x1";
    return oversampled ? "fuzz.germanium.x4" : "fuzz.germanium.x1";
}

/// Tested small-signal feedback bound for the Forge registry.
///
/// The DSP suite sweeps the complete parameter grid and proves that the binding
/// point is silicon, maximum fuzz, healthy bias, and minimum source impedance.
/// Evaluate that realized endpoint instead of restating the 0.94 design ceiling:
/// the accepted 0.1 kohm source floor makes the measured maximum slightly lower.
inline float worst_case_gain() {
    signal::FuzzPair probe;
    probe.set_device(Device::silicon);
    probe.set_fuzz(1.0);
    probe.set_bias_starve(0.0);
    probe.set_source_impedance_kohm(0.1);
    return static_cast<float>(probe.loop_gain());
}

inline CustomNodeType make_fuzz_node(Device device, bool oversampled = true) {
    CustomNodeType t;
    t.type_id = type_id(device, oversampled);
    t.version = 1;
    t.num_input_ports = t.num_output_ports = 1;
    t.default_name = device == Device::silicon ? "Fuzz (Silicon)" : "Fuzz (Germanium)";
    t.lowerable = true;
    t.latency_samples = [oversampled](double sample_rate) {
        signal::FuzzPair probe;
        probe.set_oversampling_enabled(oversampled);
        probe.prepare(sample_rate);
        return probe.latency_samples();
    };
    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [device, oversampled](void* p, double sr, int) {
        auto& f = static_cast<Instance*>(p)->fuzz;
        f.set_device(device);
        f.set_oversampling_enabled(oversampled);
        f.set_seed(kDeterministicDriftSeed);
        f.set_drift_enabled(false);
        f.prepare(sr);
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->fuzz.reset(); };
    t.baked_params = {{kFuzz, 0.0f, 1.0f, 0.65f},
                      {kBiasStarve, 0.0f, 1.0f, 0.0f},
                      {kSourceKohm, 0.1f, 1000.0f, 10.0f},
                      {kOutputDb, -24.0f, 24.0f, 0.0f},
                      {kMix, 0.0f, 1.0f, 1.0f},
                      {kDriftEnabled, 0.0f, 1.0f, 0.0f}};
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto& f = static_cast<Instance*>(p)->fuzz;
        const float* input = in.channel_ptr(0);
        float* output = out.channel_ptr(0);
        for (int i = 0; i < n; ++i) {
            const auto offset = static_cast<std::int32_t>(i);
            f.set_fuzz(params.value_at(kFuzz, offset));
            f.set_bias_starve(params.value_at(kBiasStarve, offset));
            f.set_source_impedance_kohm(params.value_at(kSourceKohm, offset));
            f.set_output_level_db(params.value_at(kOutputDb, offset));
            f.set_mix(params.value_at(kMix, offset));
            f.set_drift_enabled(params.value_at(kDriftEnabled, offset) >= 0.5f);
            output[static_cast<std::size_t>(i)] = f.process(input[static_cast<std::size_t>(i)]);
        }
    };
    return t;
}

inline int latency_samples(bool oversampled, double sample_rate = 48000.0) {
    signal::FuzzPair probe;
    probe.set_oversampling_enabled(oversampled);
    probe.prepare(sample_rate);
    return probe.latency_samples();
}

}  // namespace pulp::host::fuzz
