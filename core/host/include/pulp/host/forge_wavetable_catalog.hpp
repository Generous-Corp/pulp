#pragma once

// Wavetable — Forge-facing bake-layer catalog nodes.
//
// This pack exposes the shipped WavetableBank as a fixed four-table source.
// Table construction allocates and therefore happens in the node instance's
// control-thread lifetime; prepare and process only update scalar state.

#include <pulp/host/forge_param_descriptor.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/signal/wavetable.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace pulp::host::wavetable {

inline constexpr const char* kTypeId = "wavetable.oscillator";

inline constexpr state::ParamID kFrequencyHz = 1;
inline constexpr state::ParamID kPosition = 2;
inline constexpr state::ParamID kLevelDb = 3;

struct Instance {
    signal::WavetableBank bank;

    Instance()
        : bank(std::vector<signal::Wavetable>{
              signal::Wavetable::make_sine(),
              signal::Wavetable::make_saw(),
              signal::Wavetable::make_square(),
              signal::Wavetable::make_triangle(),
          }) {}
};

inline CustomNodeType make_wavetable_oscillator_node() {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 0;
    t.num_output_ports = 1;
    t.default_name = "Wavetable Oscillator";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [](void* p, double sample_rate, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->bank.set_sample_rate(static_cast<float>(sample_rate));
        s->bank.set_frequency_immediate(220.0f);
        s->bank.reset();
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->bank.reset(); };

    t.baked_params.push_back({kFrequencyHz, 20.0f, 20000.0f, 220.0f});
    t.baked_params.push_back({kPosition, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kLevelDb, -60.0f, 0.0f, -12.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& /*in*/, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        float* output = out.channel_ptr(0);
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->bank.set_frequency(params.value_at(kFrequencyHz, offset));
            s->bank.set_position(params.value_at(kPosition, offset));
            const float gain = std::pow(10.0f, params.value_at(kLevelDb, offset) / 20.0f);
            output[static_cast<std::size_t>(k)] = s->bank.next() * gain;
        }
    };
    return t;
}

inline ForgeNodeDescriptor descriptor() {
    return {
        "wavetable_oscillator",
        "Wavetable Oscillator",
        "An oscillator that morphs continuously across sine, saw, square, and triangle tables.",
        {},
        {{"default", kTypeId}},
        {
            {"frequency_hz", kFrequencyHz, "Frequency", "Hz",
             "Oscillator frequency.", ForgeParamKind::continuous,
             ForgeParamCurve::logarithmic},
            {"position", kPosition, "Position", "%",
             "Morph position across the wavetable bank.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
            {"level_db", kLevelDb, "Level", "dB",
             "Oscillator output level.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
        },
    };
}

inline constexpr float worst_case_gain() {
    return 1.0f;
}

} // namespace pulp::host::wavetable
