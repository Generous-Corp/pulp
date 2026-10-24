#pragma once

// The Processor the plugin-state fuzz lane restores into: a handful of
// parameters plus a plugin-owned payload, so both halves of the host-facing
// state envelope are exercised. Shared by the libFuzzer target and the seed
// corpus generator so the seeds restore into the exact store they fuzz.

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace pulp::test::plugin_state_fuzz {

class FuzzProcessor : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "PluginStateFuzz",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.plugin-state-fuzz",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_group({.id = 1, .name = "Main"});
        store.add_parameter({.id = 1,
                             .name = "Gain",
                             .unit = "dB",
                             .range = {-60.0f, 24.0f, 0.0f, 0.1f},
                             .group_id = 1});
        store.add_parameter({.id = 2,
                             .name = "Mix",
                             .unit = "%",
                             .range = {0.0f, 100.0f, 100.0f, 0.1f},
                             .group_id = 1});
        store.add_parameter(
            {.id = 3, .name = "Drive", .range = {0.0f, 1.0f, 0.5f, 0.01f}, .group_id = 1});
        store.add_parameter(
            {.id = 4, .name = "Bypass", .range = {0.0f, 1.0f, 0.0f, 1.0f}, .group_id = 1});
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}

    std::vector<uint8_t> serialize_plugin_state() const override {
        return plugin_state;
    }

    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        plugin_state.assign(data.begin(), data.end());
        return true;
    }

    std::vector<uint8_t> plugin_state;
};

} // namespace pulp::test::plugin_state_fuzz
