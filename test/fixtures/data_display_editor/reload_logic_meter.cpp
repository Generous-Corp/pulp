// Thin hot-reload logic whose editor holds a level meter. Built against headers
// only; every pulp symbol, the display registry included, resolves from the
// host at dlopen. The meter is made through the view-core registry and
// configured through LevelDisplay inside this module, so the cast below uses
// the host's typeinfo for the interface.

#include <pulp/format/processor.hpp>
#include <pulp/format/reload/reload_abi.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/data_display.hpp>
#include <pulp/view/view.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>

using namespace pulp;

namespace {
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
        auto root = std::make_unique<view::View>();
        root->set_bounds({0.0f, 0.0f, 120.0f, 120.0f});
        auto meter = view::make_data_display("pulp.level");
        if (!meter) {
            root->set_id("meter-unavailable");
            return root;
        }
        auto* level = dynamic_cast<view::LevelDisplay*>(meter.get());
        if (!level) {
            root->set_id("meter-not-level-display");
            return root;
        }
        level->set_levels(0.5f, 0.75f);
        meter->set_id("reload-meter");
        root->set_id("meter-ready");
        root->add_child(std::move(meter));
        return root;
    }
};
} // namespace

PULP_RELOAD_LOGIC(new MeterLogic())
