// A real LV2 module, built into a real bundle, so the build's own TTL emission
// can be read back the way a host reads it.
//
// It exists because the generator and the reader were only ever tested apart:
// test_lv2_adapter.cpp asserts the generated strings and
// test_lv2_host_discovery.cpp parses hand-authored TTL. Both stay green if the
// two disagree — which is precisely the shape of a bundle no host can load.
// This module is the one artifact both halves touch.

#include <pulp/format/lv2_entry.hpp>
#include <pulp/format/processor.hpp>

#include <cstddef>
#include <memory>

namespace {

using namespace pulp::format;

constexpr int kFixtureGainParam = 1;
constexpr int kFixtureMixParam = 2;

class Lv2TtlFixtureProcessor final : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor desc;
        desc.name = "Pulp TTL Fixture";
        desc.manufacturer = "PulpTest";
        desc.bundle_id = "com.pulp.test.lv2-ttl-fixture";
        desc.version = "1.0.0";
        desc.category = PluginCategory::Effect;
        // Stereo in and out with two parameters: enough shape that the control
        // ports must follow the audio ports and hold declaration order. A
        // mono/no-parameter plugin would prove nothing about the ordering the
        // manifest and connect_port() have to share.
        desc.input_buses = {{"Audio In", 2}};
        desc.output_buses = {{"Audio Out", 2}};
        return desc;
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = kFixtureGainParam,
            .name = "Gain",
            .unit = "",
            .range = {0.0f, 2.0f, 1.0f, 0.01f},
        });
        store.add_parameter({
            .id = kFixtureMixParam,
            .name = "Mix",
            .unit = "",
            .range = {0.0f, 1.0f, 0.5f, 0.01f},
        });
    }

    void prepare(const PrepareContext&) override {}
    void release() override {}

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const ProcessContext&) override {
        // Passthrough. What this module DECLARES is the subject; it only has to
        // be a working plugin so a host load is a real load.
        const std::size_t channels = output.num_channels();
        const std::size_t samples = output.num_samples();
        for (std::size_t c = 0; c < channels; ++c) {
            float* dst = output.channel_ptr(c);
            const bool has_in = c < input.num_channels();
            const float* src = has_in ? input.channel_ptr(c) : nullptr;
            for (std::size_t n = 0; n < samples; ++n)
                dst[n] = has_in ? src[n] : 0.0f;
        }
    }
};

std::unique_ptr<Processor> make_lv2_ttl_fixture() {
    return std::make_unique<Lv2TtlFixtureProcessor>();
}

} // namespace

PULP_LV2_PLUGIN(make_lv2_ttl_fixture, "urn:pulp:test:lv2-ttl-fixture")
