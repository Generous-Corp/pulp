// Behavioral-probe hot-reload logic fixture for test_reload_transaction.cpp.
//
// Same parameter contract as the compatible fixture (one "Gain" param, id 1,
// range 0..2), so it passes the load / ABI / fingerprint / contract gates — but
// its process() emits NaN unconditionally (even on the silence probe block). The
// transaction's pre-commit behavioral probe (item 1.10) must catch this and
// reject the swap, keeping the live DSP unchanged.

#include <pulp/format/processor.hpp>
#include <pulp/format/reload/reload_abi.hpp>
#include <pulp/state/store.hpp>

#include <cstddef>
#include <limits>

using namespace pulp;

namespace {
class NanGain final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {.name = "ReloadGain", .manufacturer = "Pulp Examples",
                .bundle_id = "com.pulp.reload.gain", .version = "0.3.0",
                .category = format::PluginCategory::Effect,
                .input_buses = {{"In", 2}}, .output_buses = {{"Out", 2}}};
    }
    void define_parameters(state::StateStore& s) override {
        s.add_parameter({.id = 1, .name = "Gain", .unit = "",
                         .range = {0.0f, 2.0f, 1.0f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        for (std::size_t c = 0; c < out.num_channels(); ++c) {
            auto o = out.channel(c);
            for (std::size_t n = 0; n < out.num_samples(); ++n) o[n] = nan;  // buggy build
        }
    }
};
}  // namespace

PULP_RELOAD_LOGIC(new NanGain())
