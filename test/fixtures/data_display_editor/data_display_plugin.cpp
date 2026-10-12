// A plug-in whose editor is an imported design containing a level meter. The
// editor reaches the meter only through the importer, so whether it is a real
// Meter depends on the audio displays having been registered by the time the
// editor is created.

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/view.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>

namespace {

pulp::view::DesignIR meter_panel() {
    using namespace pulp::view;
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "panel";
    ir.root.style.width = 160.0f;
    ir.root.style.height = 120.0f;
    ir.root.layout.direction = LayoutDirection::row;

    IRNode meter;
    meter.type = "frame";
    meter.stable_anchor_id = "output-meter";
    meter.style.width = 24.0f;
    meter.style.height = 100.0f;
    meter.audio_widget = AudioWidgetType::meter;
    meter.attributes["value"] = "0.5";
    ir.root.children.push_back(std::move(meter));
    return ir;
}

class DataDisplayFixture final : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "PulpDataDisplayFixture", .manufacturer = "Pulp",
                .bundle_id = "dev.pulp.test.data-display-fixture", .version = "1.0.0",
                .category = pulp::format::PluginCategory::Effect,
                .input_buses = {{"In", 2}}, .output_buses = {{"Out", 2}}};
    }
    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({.id = 1, .name = "Gain", .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 0.0f}});
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        const std::size_t channels = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < channels; ++c) {
            auto o = out.channel(c);
            auto i = in.channel(c);
            for (std::size_t n = 0; n < out.num_samples(); ++n) o[n] = i[n];
        }
    }
    std::unique_ptr<pulp::view::View> create_view() override {
        const auto ir = meter_panel();
        return pulp::view::build_native_view_tree(ir, ir.asset_manifest);
    }
};

} // namespace

std::unique_ptr<pulp::format::Processor> make_data_display_fixture() {
    return std::make_unique<DataDisplayFixture>();
}
