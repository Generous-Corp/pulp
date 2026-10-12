#include "../core/format/src/projection_capability.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/clap_entry.hpp>
#include <pulp/host/baked_graph_processor.hpp>

#include <array>
#include <cmath>
#include <memory>
#include <string_view>
#include <vector>

namespace {

std::unique_ptr<pulp::format::Processor> create_baked_projection() {
    return std::make_unique<pulp::host::BakedGraphProcessor>(
        std::vector<pulp::host::GraphNode>{}, std::vector<pulp::host::Connection>{}, 1, 1,
        "DSPX baked CLAP", "com.pulp.test.dspx07.baked-clap");
}

} // namespace

PULP_CLAP_PLUGIN(create_baked_projection)

TEST_CASE("DSPX-07 baked processor reaches the real CLAP adapter lifecycle", "[dspx][clap]") {
    using namespace pulp::format;
    REQUIRE(projection_capability(ProjectionSurface::clap, true, true).supported());
    // AUv2/v3 share the accepted baked-graph admission contract exercised by
    // the native AU lifecycle tests in this change.
    REQUIRE(projection_capability(ProjectionSurface::au, true, true).supported());

    REQUIRE(clap_entry.init("dspx07-baked-clap"));
    const auto* factory =
        static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    REQUIRE(factory->get_plugin_count(factory) == 1);
    const auto* descriptor = factory->get_plugin_descriptor(factory, 0);
    REQUIRE(descriptor != nullptr);
    REQUIRE(std::string_view(descriptor->id) == "com.pulp.test.dspx07.baked-clap");

    const auto* plugin = factory->create_plugin(factory, nullptr, descriptor->id);
    REQUIRE(plugin != nullptr);
    REQUIRE(plugin->init(plugin));

    const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    REQUIRE(ports != nullptr);
    REQUIRE(ports->count(plugin, true) == 1);
    REQUIRE(ports->count(plugin, false) == 1);

    REQUIRE(plugin->activate(plugin, 48000.0, 1, 64));
    REQUIRE(plugin->start_processing(plugin));

    std::array<float, 64> input{};
    std::array<float, 64> output{};
    float* inputs[] = {input.data()};
    float* outputs[] = {output.data()};
    clap_audio_buffer_t in_bus{inputs, nullptr, 1, 0, 0};
    clap_audio_buffer_t out_bus{outputs, nullptr, 1, 0, 0};
    clap_process_t process{};
    process.steady_time = 0;
    process.frames_count = 64;
    process.audio_inputs = &in_bus;
    process.audio_outputs = &out_bus;
    REQUIRE(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR);
    for (const float sample : output)
        REQUIRE(std::isfinite(sample));

    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    clap_entry.deinit();
}
