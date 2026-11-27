#include "processor.hpp"

#include <pulp/format/processor.hpp>

namespace pulp::test_fixtures {

namespace {

class Auv3EditorProcessor final : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "Pulp AUv3 Vellum Editor Fixture",
            .manufacturer = "Pulp",
            .bundle_id = "dev.pulp.test.auv3-vellum-editor",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        output.clear();
    }
};

} // namespace

std::unique_ptr<format::Processor> create_auv3_editor_fixture() {
    return std::make_unique<Auv3EditorProcessor>();
}

} // namespace pulp::test_fixtures
