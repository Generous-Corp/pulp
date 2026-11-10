#pragma once

// The metal-probe plug-in: a gain whose editor is a native Metal surface. The
// editor's clear colour follows the gain, and a triangle follows the pointer.

#include "metal_probe_ui.hpp"

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <memory>

namespace pulp_native_metal_probe {

inline constexpr ::pulp::state::ParamID kGain = 1;

class MetalProbeProcessor final : public ::pulp::format::Processor {
  public:
    ::pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "Pulp Native Metal Probe",
            .manufacturer = "Pulp",
            .bundle_id = "dev.pulp.native.metal-probe",
            .version = "1.0.0",
            .category = ::pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(::pulp::state::StateStore& store) override {
        ::pulp::state::ParamInfo gain;
        gain.id = kGain;
        gain.name = "Gain";
        gain.range = {0.0f, 1.0f, 0.5f};
        store.add_parameter(gain);
    }

    void prepare(const ::pulp::format::PrepareContext&) override {}

    void process(::pulp::audio::BufferView<float>& output,
                 const ::pulp::audio::BufferView<const float>& input, ::pulp::midi::MidiBuffer&,
                 ::pulp::midi::MidiBuffer&, const ::pulp::format::ProcessContext&) override {
        const float gain = state().get_value(kGain);
        for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
            auto out = output.channel(ch);
            if (ch >= input.num_channels()) {
                for (auto& s : out)
                    s = 0.0f;
                continue;
            }
            auto in = input.channel(ch);
            for (std::size_t i = 0; i < out.size() && i < in.size(); ++i)
                out[i] = in[i] * gain;
        }
    }
};

inline std::unique_ptr<::pulp::format::Processor> create_metal_probe() {
    return std::make_unique<MetalProbeProcessor>();
}

} // namespace pulp_native_metal_probe
