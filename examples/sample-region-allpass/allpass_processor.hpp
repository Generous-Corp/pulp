#pragma once

#include "allpass_graph.hpp"
#include <pulp/format/processor.hpp>
#include <string_view>

namespace pulp::examples {

// A mono Processor whose sample-region topology can be inspected and edited
// through the public prepared-transaction API. The adapter owns parameter values.
class SampleRegionAllpassProcessor final : public format::Processor {
  public:
    explicit SampleRegionAllpassProcessor(std::string_view graph_json = {});
    ~SampleRegionAllpassProcessor() override;

    format::PluginDescriptor descriptor() const override;
    void define_parameters(state::StateStore& store) override;
    void prepare(const format::PrepareContext& context) override;
    void release() override;
    bool is_bus_layout_supported(const BusesLayout& layout) const override;
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                 const format::ProcessContext& context) override;
    int latency_samples() const override {
        return 0;
    }

    host::SignalGraph& graph() noexcept {
        return graph_;
    }
    const host::SignalGraph& graph() const noexcept {
        return graph_;
    }
    const host::SampleRegionParameterContract& parameter_contract() const noexcept {
        return contract_;
    }
    bool ready() const noexcept {
        return ready_;
    }
    const std::string& error() const noexcept {
        return error_;
    }

  private:
    bool make_initial_edit();
    host::SampleRegionParameterContract contract_;
    std::unique_ptr<host::SampleRegionParameterBinding> binding_;
    host::SignalGraph graph_;
    std::unique_ptr<host::SignalGraph::PreparedTopologyEdit> initial_edit_;
    std::string error_;
    int max_buffer_size_ = 0;
    bool initialized_ = false;
    bool ready_ = false;
};

std::unique_ptr<format::Processor> create_sample_region_allpass();

} // namespace pulp::examples
