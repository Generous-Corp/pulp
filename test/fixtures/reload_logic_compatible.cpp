// Compatible hot-reload logic fixture for test_reload_transaction.cpp.
//
// Same parameter contract as the test's live plugin (one "Gain" param, id 1,
// range 0..2), but a DIFFERENT DSP behavior — it applies 2x the gain — so a
// successful reload is observable in the output. Exports the reload ABI via
// PULP_RELOAD_LOGIC; built as a MODULE and dlopen'd by the transaction.

#include <pulp/format/processor.hpp>
#include <pulp/format/reload/reload_abi.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <cstddef>

using namespace pulp;

namespace {
class CompatibleGain final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {.name = "ReloadGain", .manufacturer = "Pulp Examples",
                .bundle_id = "com.pulp.reload.gain", .version = "0.2.0",
                .category = format::PluginCategory::Effect,
                .input_buses = {{"In", 2}}, .output_buses = {{"Out", 2}}};
    }
    void define_parameters(state::StateStore& s) override {
        s.add_parameter({.id = 1, .name = "Gain", .unit = "",
                         .range = {0.0f, 2.0f, 1.0f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const float g = state().get_value(1) * 2.0f;  // reloaded behavior: 2x gain
        const std::size_t ch = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < ch; ++c) {
            auto o = out.channel(c);
            auto i = in.channel(c);
            for (std::size_t n = 0; n < out.num_samples(); ++n) o[n] = i[n] * g;
        }
    }
};
}  // namespace

PULP_RELOAD_LOGIC(new CompatibleGain())
