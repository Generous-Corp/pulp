#pragma once

#include <pulp/format/processor.hpp>

#include <memory>

namespace pulp_rmlui_metal_demo {

class Processor final : public ::pulp::format::Processor {
  public:
    ::pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "Pulp + RmlUi + Metal",
                .manufacturer = "Pulp",
                .bundle_id = "dev.pulp.rmlui.metal-demo",
                .version = "1.0.0",
                .category = ::pulp::format::PluginCategory::Effect,
                .input_buses = {{"Audio In", 2}},
                .output_buses = {{"Audio Out", 2}}};
    }

    void define_parameters(::pulp::state::StateStore&) override {}
    void prepare(const ::pulp::format::PrepareContext&) override {}
    void process(::pulp::audio::BufferView<float>& output,
                 const ::pulp::audio::BufferView<const float>& input, ::pulp::midi::MidiBuffer&,
                 ::pulp::midi::MidiBuffer&, const ::pulp::format::ProcessContext&) override {
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel) {
            auto out = output.channel(channel);
            if (channel >= input.num_channels()) {
                for (auto& sample : out)
                    sample = 0.0f;
                continue;
            }
            const auto in = input.channel(channel);
            for (std::size_t i = 0; i < out.size() && i < in.size(); ++i)
                out[i] = in[i];
        }
    }

    ::pulp::format::EditorFactory* editor_factory() override;
};

inline std::unique_ptr<::pulp::format::Processor> create_processor() {
    return std::make_unique<Processor>();
}

} // namespace pulp_rmlui_metal_demo
