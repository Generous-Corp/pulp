#pragma once

// Forge-facing adapter for Pulp's analog-modelled VCF.
//
// The filter engine, voicing laws, and measured calibration tables belong to
// pulp::signal. This header owns only the bakeable CustomNodeType identities
// and parameter bridge needed by Forge (and any other graph host). Keeping that
// boundary in a dedicated header prevents the general lo-fi catalog from
// becoming the owner of analog DSP policy.

#include <pulp/host/signal_graph.hpp>
#include <pulp/signal/analog_vcf.hpp>

#include <cstddef>
#include <cstdint>

namespace pulp::host::forge_lofi {

inline constexpr const char* kAnalogVcfJunoTypeId = "vcf.juno";
inline constexpr const char* kAnalogVcfJupiterTypeId = "vcf.jupiter";
inline constexpr const char* kAnalogVcfProphet5TypeId = "vcf.prophet5";
inline constexpr const char* kAnalogVcfMinimoogTypeId = "vcf.minimoog";

inline constexpr state::ParamID kAnalogVcfCutoff = 1;  // panel knob, not Hz
inline constexpr state::ParamID kAnalogVcfCutoffMod = 2;  // octave modulation
inline constexpr state::ParamID kAnalogVcfResonance = 3;
inline constexpr state::ParamID kAnalogVcfDriveDb = 4;

// Oversampling changes the realized topology and fixed latency, so it is an
// adapter construction policy rather than an injectable musical parameter.
// Keep prepare and the published latency contract tied to this one value.
inline constexpr int kAnalogVcfOversampling = 2;

struct AnalogVcfInstance {
    explicit AnalogVcfInstance(signal::AnalogVcf::Voicing selected) noexcept {
        filter.set_voicing(selected);
    }

    signal::AnalogVcf filter;
};

struct AnalogVcfIdentity {
    const char* type_id;
    const char* name;
};

inline AnalogVcfIdentity analog_vcf_identity(
    signal::AnalogVcf::Voicing voicing) noexcept {
    switch (voicing) {
        case signal::AnalogVcf::Voicing::juno:
            return {kAnalogVcfJunoTypeId, "Juno VCF"};
        case signal::AnalogVcf::Voicing::jupiter:
            return {kAnalogVcfJupiterTypeId, "Jupiter-8 VCF"};
        case signal::AnalogVcf::Voicing::prophet5:
            return {kAnalogVcfProphet5TypeId, "Prophet-5 VCF"};
        case signal::AnalogVcf::Voicing::minimoog:
            return {kAnalogVcfMinimoogTypeId, "Minimoog VCF"};
    }
    return {kAnalogVcfJunoTypeId, "Analog VCF"};
}

inline CustomNodeType make_analog_vcf_node(signal::AnalogVcf::Voicing voicing) {
    const AnalogVcfIdentity identity = analog_vcf_identity(voicing);
    CustomNodeType type;
    type.type_id = identity.type_id;
    type.version = 1;
    type.num_input_ports = 1;
    type.num_output_ports = 1;
    type.default_name = identity.name;
    // Rate-independent: the OTA cascade's half-band group delay is a function of
    // the oversampling factor alone, so the callback ignores the sample rate.
    type.latency_samples = [](double) {
        return signal::AnalogVcf::latency_samples_for_oversampling(
            kAnalogVcfOversampling);
    };
    type.lowerable = true;
    type.create = [voicing]() -> void* { return new AnalogVcfInstance(voicing); };
    type.destroy = [](void* p) { delete static_cast<AnalogVcfInstance*>(p); };
    type.prepare = [](void* p, double sample_rate, int /*max_block*/) {
        auto* instance = static_cast<AnalogVcfInstance*>(p);
        instance->filter.set_sample_rate(sample_rate);
        instance->filter.set_oversampling(kAnalogVcfOversampling);
        instance->filter.set_smoothing_time_ms(3.0);
        instance->filter.reset();
    };
    type.reset = [](void* p) { static_cast<AnalogVcfInstance*>(p)->filter.reset(); };
    type.process_instance =
        [](void* p, audio::BufferView<float>& output,
           const audio::BufferView<const float>& input, int num_samples) {
            auto* instance = static_cast<AnalogVcfInstance*>(p);
            const float* input_samples = input.channel_ptr(0);
            float* output_samples = output.channel_ptr(0);
            for (int i = 0; i < num_samples; ++i)
                output_samples[i] = instance->filter.process(input_samples[i]);
        };
    type.baked_params.push_back({kAnalogVcfCutoff, 0.0f, 1.0f, 0.5f});
    type.baked_params.push_back({kAnalogVcfCutoffMod, -5.0f, 5.0f, 0.0f});
    type.baked_params.push_back({kAnalogVcfResonance, 0.0f, 1.0f, 0.0f});
    type.baked_params.push_back({kAnalogVcfDriveDb, -24.0f, 48.0f, 0.0f});
    type.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& output,
           const audio::BufferView<const float>& input, int num_samples,
           const BakedParamView& params) {
            auto* instance = static_cast<AnalogVcfInstance*>(p);
            const float* input_samples = input.channel_ptr(0);
            float* output_samples = output.channel_ptr(0);
            for (int sample = 0; sample < num_samples; ++sample) {
                const auto offset = static_cast<std::int32_t>(sample);
                instance->filter.set_parameters(
                    params.value_at(kAnalogVcfCutoff, offset),
                    params.value_at(kAnalogVcfCutoffMod, offset),
                    params.value_at(kAnalogVcfResonance, offset),
                    params.value_at(kAnalogVcfDriveDb, offset));
                output_samples[static_cast<std::size_t>(sample)] =
                    instance->filter.process(
                        input_samples[static_cast<std::size_t>(sample)]);
            }
        };
    return type;
}

}  // namespace pulp::host::forge_lofi
