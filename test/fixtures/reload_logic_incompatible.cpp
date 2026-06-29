// Incompatible hot-reload logic fixture for test_reload_transaction.cpp.
//
// Adds a second parameter (id 2, "Mix") the live plugin doesn't have, so its
// parameter contract diverges — the reload transaction must reject it at the
// contract gate, before any audio-visible swap. Built as a MODULE and dlopen'd.

#include <pulp/format/processor.hpp>
#include <pulp/format/reload/reload_abi.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <cstddef>

using namespace pulp;

namespace {
class IncompatibleGain final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {.name = "ReloadGainPlus", .manufacturer = "Pulp Examples",
                .bundle_id = "com.pulp.reload.gain", .version = "0.3.0",
                .category = format::PluginCategory::Effect,
                .input_buses = {{"In", 2}}, .output_buses = {{"Out", 2}}};
    }
    void define_parameters(state::StateStore& s) override {
        s.add_parameter({.id = 1, .name = "Gain", .unit = "",
                         .range = {0.0f, 2.0f, 1.0f, 0.0f}});
        // Extra parameter the live plugin doesn't have -> contract mismatch.
        s.add_parameter({.id = 2, .name = "Mix", .unit = "%",
                         .range = {0.0f, 100.0f, 50.0f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const float g = state().get_value(1) * 4.0f;  // distinct, but must never run
        const std::size_t ch = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < ch; ++c) {
            auto o = out.channel(c);
            auto i = in.channel(c);
            for (std::size_t n = 0; n < out.num_samples(); ++n) o[n] = i[n] * g;
        }
    }
};
}  // namespace

PULP_RELOAD_LOGIC(new IncompatibleGain())
