#pragma once
#include <pulp/format/processor.hpp>
#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/host/graph_patch.hpp>
#include <pulp/state/param_cursor.hpp>

class PatchGain final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "PatchGain";
        d.manufacturer = "Example";
        d.bundle_id = "com.example.PatchGain";
        d.version = "1";
        d.accepts_midi = true;
        d.produces_midi = true;
        d.input_buses = {{"In", 1, false}};
        d.output_buses = {{"Out", 1, false}};
        return d;
    }
    void define_parameters(pulp::state::StateStore& s) override {
        s.add_parameter({.id = 7, .name = "level", .range = {0, 2, 1}});
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in, pulp::midi::MidiBuffer& midi,
                 pulp::midi::MidiBuffer& midi_out, const pulp::format::ProcessContext&) override {
        pulp::state::ParamCursor parameters(state(), param_events());
        for (std::size_t i = 0; i < out.num_samples(); ++i) {
            parameters.advance_to(static_cast<int32_t>(i));
            out.channel(0)[i] =
                in.channel(0)[i] * parameters.value(7) + (midi.empty() ? 0.0f : 0.125f);
        }
        for (const auto& event : midi)
            midi_out.add(event);
    }
};
inline void register_patch_gain(pulp::host::GraphPatchCatalog& catalog) {
    pulp::host::ProcessorRestoreDescriptor descriptor;
    descriptor.type = {"com.example.PatchGain", 1};
    descriptor.input_channels = 1;
    descriptor.output_channels = 1;
    descriptor.create = [] {
        return pulp::format::ProcessorNodeInstance::create(std::make_unique<PatchGain>());
    };
    if (catalog.register_processor(std::move(descriptor)) !=
        pulp::host::ProcessorRestoreCode::Restored)
        std::terminate();
}
