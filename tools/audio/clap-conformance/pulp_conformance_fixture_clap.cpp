// SPDX-License-Identifier: MIT
//
// A Pulp-built CLAP plug-in (generated entry, real adapter) with a static
// descriptor, two parameters and a stereo pass-through. The conformance checker
// must pass it.

#include <pulp/format/clap_entry.hpp>
#include <pulp/format/processor.hpp>

#include <memory>

namespace {

pulp::format::PluginDescriptor fixture_descriptor() {
    return {
        .name = "Pulp Conformance Fixture",
        .manufacturer = "Pulp",
        .bundle_id = "dev.pulp.test.conformance-pulp",
        .version = "1.0.0",
        .category = pulp::format::PluginCategory::Effect,
        .input_buses = {{"In", 2}},
        .output_buses = {{"Out", 2}},
    };
}

class FixtureProcessor : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        return fixture_descriptor();
    }
    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter(
            {.id = 1, .name = "Gain", .unit = "", .range = {0.0f, 1.0f, 0.5f, 0.0f}});
        store.add_parameter(
            {.id = 2, .name = "Mix", .unit = "", .range = {0.0f, 1.0f, 0.5f, 0.0f}});
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        for (std::size_t c = 0; c < out.num_channels(); ++c)
            for (std::size_t n = 0; n < out.num_samples(); ++n)
                out.channel_ptr(c)[n] = c < in.num_channels() ? in.channel_ptr(c)[n] : 0.0f;
    }
    void process(pulp::format::ProcessBuffers& audio, pulp::midi::MidiBuffer& mi,
                 pulp::midi::MidiBuffer& mo, const pulp::format::ProcessContext& ctx) override {
        auto* o = audio.main_output();
        if (!o)
            return;
        if (auto* i = audio.main_input()) {
            process(*o, *i, mi, mo, ctx);
        } else {
            for (std::size_t c = 0; c < o->num_channels(); ++c)
                for (std::size_t n = 0; n < o->num_samples(); ++n)
                    o->channel_ptr(c)[n] = 0.0f;
        }
    }
};

std::unique_ptr<pulp::format::Processor> create_fixture() {
    return std::make_unique<FixtureProcessor>();
}

} // namespace

PULP_CLAP_PLUGIN_WITH_DESCRIPTOR(create_fixture, fixture_descriptor)
