#pragma once

#include <pulp/format/processor.hpp>

#include <cmath>
#include <memory>

namespace pulp::sdk_smoke {

enum ParamIds : state::ParamID {
    kGain = 1,
};

class SmokeProcessor final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override
    {
        return {
            .name = "PulpSDKSmokeProbe",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.sdksmoke",
            .version = "0.1.0",
            .category = format::PluginCategory::Effect,
        };
    }

    void define_parameters(state::StateStore& store) override
    {
        store.add_parameter({
            .id = kGain,
            .name = "Gain",
            .unit = "dB",
            .range = {-24.0f, 24.0f, 0.0f, 0.1f},
        });
    }

    void prepare(const format::PrepareContext&) override {}

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext&) override
    {
        const float gain = std::pow(10.0f, state().get_value(kGain) / 20.0f);
        const std::size_t channels = std::min(output.num_channels(), input.num_channels());

        for (std::size_t ch = 0; ch < channels; ++ch) {
            for (std::size_t i = 0; i < output.num_samples(); ++i) {
                output.channel(ch)[i] = input.channel(ch)[i] * gain;
            }
        }
    }
};

inline std::unique_ptr<format::Processor> create_processor()
{
    return std::make_unique<SmokeProcessor>();
}

} // namespace pulp::sdk_smoke
