// Thin hot-reload logic whose editor is an imported design holding a level
// meter. Built against headers only; every pulp symbol, the importer and the
// display registry included, resolves from the host at dlopen. The module
// configures the meter through LevelDisplay, so that cast uses the host's
// typeinfo for the interface.

#include <pulp/format/processor.hpp>
#include <pulp/format/reload/reload_abi.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/data_display.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/view.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>

using namespace pulp;

namespace {

view::View* find_anchor(view::View& root, std::string_view anchor) {
    if (root.anchor_id() == anchor) return &root;
    for (std::size_t i = 0; i < root.child_count(); ++i)
        if (auto* hit = find_anchor(*root.child_at(i), anchor)) return hit;
    return nullptr;
}

class MeterLogic final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {.name = "ReloadMeter", .manufacturer = "Pulp Examples",
                .bundle_id = "com.pulp.reload.meter", .version = "0.1.0",
                .category = format::PluginCategory::Effect,
                .input_buses = {{"In", 2}}, .output_buses = {{"Out", 2}}};
    }
    void define_parameters(state::StateStore& s) override {
        s.add_parameter({.id = 1, .name = "Gain", .unit = "",
                         .range = {0.0f, 2.0f, 1.0f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        const std::size_t ch = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < ch; ++c) {
            auto o = out.channel(c);
            auto i = in.channel(c);
            for (std::size_t n = 0; n < out.num_samples(); ++n) o[n] = i[n];
        }
    }
    std::unique_ptr<view::View> create_view() override {
        view::DesignIR ir;
        ir.root.type = "frame";
        ir.root.stable_anchor_id = "reload-panel";
        ir.root.style.width = 120.0f;
        ir.root.style.height = 120.0f;
        view::IRNode meter;
        meter.type = "frame";
        meter.stable_anchor_id = "reload-meter";
        meter.style.width = 24.0f;
        meter.style.height = 100.0f;
        meter.audio_widget = view::AudioWidgetType::meter;
        ir.root.children.push_back(meter);

        auto root = view::build_native_view_tree(ir, ir.asset_manifest);
        if (!root) return root;
        auto* imported = find_anchor(*root, "reload-meter");
        if (auto* level = imported ? dynamic_cast<view::LevelDisplay*>(imported) : nullptr) {
            level->set_levels(0.5f, 0.75f);
            imported->set_id("module-configured-level");
        }
        return root;
    }
};
} // namespace

PULP_RELOAD_LOGIC(new MeterLogic())
