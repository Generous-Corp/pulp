#pragma once

// An editor-less gain: no create_view() override, so the plug-in has no editor
// and pulp_add_plugin(NO_EDITOR) can link it against the audio half of the
// format layer only.

#include <pulp/format/processor.hpp>

#include <cmath>
#include <memory>

namespace pulp::test_fixtures {

class NoEditorGain final : public format::Processor {
  public:
    static constexpr state::ParamID kGain = 1;

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "Pulp No Editor Link Floor",
            .manufacturer = "Pulp",
            .bundle_id = "dev.pulp.test.no-editor-link-floor",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter(
            {.id = kGain, .name = "Gain", .unit = "dB", .range = {-60.0f, 12.0f, 0.0f}});
    }

    void prepare(const format::PrepareContext&) override {}

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        const float gain = std::pow(10.0f, state().get_value(kGain) / 20.0f);
        for (std::size_t c = 0; c < output.num_channels(); ++c) {
            for (std::size_t i = 0; i < output.num_samples(); ++i) {
                output.channel(c)[i] = c < input.num_channels() ? input.channel(c)[i] * gain : 0.0f;
            }
        }
    }
};

inline std::unique_ptr<format::Processor> create_no_editor_gain() {
    return std::make_unique<NoEditorGain>();
}

} // namespace pulp::test_fixtures
