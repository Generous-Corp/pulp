#pragma once

// A processor with a recording editor factory, built by every AU adapter type
// in pulp-test-au-v2-contract-adapters.

#include "../support/recording_contract_editor.hpp"

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <memory>

namespace pulp::test_fixtures {

inline constexpr state::ParamID kContractGain = 1;

class AuContractProcessor final : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override {
        format::PluginDescriptor d;
        d.name = "Contract AU Adapters";
        d.manufacturer = "Pulp";
        d.bundle_id = "dev.pulp.test.au-contract-adapters";
        d.version = "1.0.0";
        d.input_buses = {{"Audio In", 2}};
        d.output_buses = {{"Audio Out", 2}};
        return d;
    }
    void define_parameters(state::StateStore& store) override {
        state::ParamInfo info;
        info.id = kContractGain;
        info.name = "Gain";
        info.range = {0.0f, 1.0f, 0.5f};
        store.add_parameter(info);
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        out.clear();
    }
    format::EditorFactory* editor_factory() override {
        return &factory_;
    }

  private:
    test_support::RecordingContractFactory factory_;
};

inline std::unique_ptr<format::Processor> create_au_contract_processor() {
    return std::make_unique<AuContractProcessor>();
}

} // namespace pulp::test_fixtures
