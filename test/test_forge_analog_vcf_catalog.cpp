#include <catch2/catch_test_macros.hpp>

#include <pulp/host/forge_analog_vcf_catalog.hpp>
#include <pulp/host/baked_graph_processor.hpp>

#include <array>
#include <cmath>
#include <vector>

namespace lofi = pulp::host::forge_lofi;

TEST_CASE("Forge analog VCF exposes four stable Pulp identities and one param contract",
          "[host][forge][analog-vcf][contract]") {
    using Voicing = pulp::signal::AnalogVcf::Voicing;
    struct Case {
        Voicing voicing;
        const char* type_id;
    };
    constexpr std::array cases{
        Case{Voicing::juno, "vcf.juno"},
        Case{Voicing::jupiter, "vcf.jupiter"},
        Case{Voicing::prophet5, "vcf.prophet5"},
        Case{Voicing::minimoog, "vcf.minimoog"},
    };

    for (const auto& item : cases) {
        const auto node = lofi::make_analog_vcf_node(item.voicing);
        CHECK(node.type_id == item.type_id);
        CHECK(node.lowerable);
        REQUIRE(node.baked_params.size() == 4);
        CHECK(node.baked_params[0].id == lofi::kAnalogVcfCutoff);
        CHECK(node.baked_params[0].min_value == 0.0f);
        CHECK(node.baked_params[0].max_value == 1.0f);
        CHECK(node.baked_params[0].default_value == 0.5f);
        CHECK(node.baked_params[1].id == lofi::kAnalogVcfCutoffMod);
        CHECK(node.baked_params[1].min_value == -5.0f);
        CHECK(node.baked_params[1].max_value == 5.0f);
        CHECK(node.baked_params[2].id == lofi::kAnalogVcfResonance);
        CHECK(node.baked_params[3].id == lofi::kAnalogVcfDriveDb);
        CHECK(node.baked_params[3].min_value == -24.0f);
        CHECK(node.baked_params[3].max_value == 48.0f);
    }
}

TEST_CASE("Forge analog VCF adapter executes through a baked graph with declared latency",
          "[host][forge][analog-vcf][bake][pdc]") {
    constexpr int frames = 256;
    constexpr float cutoff = 0.37f;
    constexpr float cutoff_mod = 0.75f;
    constexpr float resonance = 0.42f;
    constexpr float drive_db = 6.0f;
    const auto type = lofi::make_analog_vcf_node(
        pulp::signal::AnalogVcf::Voicing::juno);
    REQUIRE(type.latency_samples ==
            pulp::signal::AnalogVcf::latency_samples_for_oversampling(2));

    pulp::host::SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(type));
    const auto input = graph.add_input_node(1, "input");
    const auto filter = graph.add_custom_node(type.type_id, type.version, "filter");
    const auto dry = graph.add_gain_node("dry");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, filter, 0));
    REQUIRE(graph.connect(filter, 0, output, 0));
    REQUIRE(graph.connect(input, 0, dry, 0));
    REQUIRE(graph.connect(dry, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, frames));
    REQUIRE(graph.latency_samples() == type.latency_samples);

    auto baked = pulp::host::bake(graph);
    REQUIRE(baked.accepted);
    REQUIRE(baked.processor);
    REQUIRE(baked.processor->latency_samples() == type.latency_samples);

    pulp::format::PrepareContext prepare;
    prepare.sample_rate = 48000.0;
    prepare.max_buffer_size = frames;
    prepare.input_channels = 1;
    prepare.output_channels = 1;
    baked.processor->prepare(prepare);

    auto& baked_graph =
        *static_cast<pulp::host::BakedGraphProcessor*>(baked.processor.get());
    auto injector = baked_graph.claim_param_injection(filter);
    REQUIRE(injector.valid());
    REQUIRE(injector.inject({lofi::kAnalogVcfCutoff, 0, cutoff, 0}) ==
            pulp::host::InjectStatus::Ok);
    REQUIRE(injector.inject({lofi::kAnalogVcfCutoffMod, 0, cutoff_mod, 0}) ==
            pulp::host::InjectStatus::Ok);
    REQUIRE(injector.inject({lofi::kAnalogVcfResonance, 0, resonance, 0}) ==
            pulp::host::InjectStatus::Ok);
    REQUIRE(injector.inject({lofi::kAnalogVcfDriveDb, 0, drive_db, 0}) ==
            pulp::host::InjectStatus::Ok);

    std::vector<float> source(frames, 0.0f);
    source[0] = 0.5f;
    std::vector<float> rendered(frames, 0.0f);
    const float* source_ptrs[] = {source.data()};
    float* rendered_ptrs[] = {rendered.data()};
    pulp::audio::BufferView<const float> in(source_ptrs, 1, source.size());
    pulp::audio::BufferView<float> out(rendered_ptrs, 1, rendered.size());
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    pulp::format::ProcessContext process;
    process.num_samples = frames;
    process.sample_rate = 48000.0;
    baked.processor->process(out, in, midi_in, midi_out, process);

    pulp::signal::AnalogVcf oracle;
    oracle.set_voicing(pulp::signal::AnalogVcf::Voicing::juno);
    oracle.set_sample_rate(48000.0);
    oracle.set_oversampling(2);
    oracle.set_smoothing_time_ms(3.0);
    oracle.reset();
    std::vector<float> expected(frames, 0.0f);
    for (int i = 0; i < frames; ++i) {
        oracle.set_parameters(cutoff, cutoff_mod, resonance, drive_db);
        expected[static_cast<std::size_t>(i)] =
            oracle.process(source[static_cast<std::size_t>(i)]);
        if (i >= type.latency_samples) {
            expected[static_cast<std::size_t>(i)] +=
                source[static_cast<std::size_t>(i - type.latency_samples)];
        }
        REQUIRE(std::isfinite(rendered[static_cast<std::size_t>(i)]));
        REQUIRE(rendered[static_cast<std::size_t>(i)] ==
                expected[static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("Baked-only Custom latency is reported by the baked processor, not live passthrough",
          "[host][forge][analog-vcf][bake][latency]") {
    auto type = lofi::make_analog_vcf_node(
        pulp::signal::AnalogVcf::Voicing::juno);
    type.type_id = "test.baked-only-analog-vcf";
    type.process_instance = {};  // transparent on the live graph by contract

    pulp::host::SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(type));
    const auto input = graph.add_input_node(1, "input");
    const auto filter = graph.add_custom_node(type.type_id, type.version, "filter");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, filter, 0));
    REQUIRE(graph.connect(filter, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 64));
    REQUIRE(graph.latency_samples() == 0);

    auto baked = pulp::host::bake(graph);
    REQUIRE(baked.accepted);
    REQUIRE(baked.processor);
    REQUIRE(baked.processor->latency_samples() == type.latency_samples);
}
