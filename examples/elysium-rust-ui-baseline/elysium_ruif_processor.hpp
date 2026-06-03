#pragma once

#include <pulp/format/processor.hpp>

#include <memory>

namespace pulp::examples {

class ElysiumRuifProcessor final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override;
    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext&) override {}
    void release() override {}
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext&) override;

    std::pair<uint32_t, uint32_t> editor_size() const override { return {1000, 600}; }
    std::unique_ptr<view::View> create_view() override;
};

std::unique_ptr<format::Processor> create_elysium_ruif_processor();
std::unique_ptr<format::Processor> create_elysium_ruif_rust_provider_processor();

}  // namespace pulp::examples
