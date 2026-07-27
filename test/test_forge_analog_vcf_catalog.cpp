#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/host/forge_analog_vcf_catalog.hpp>
#include <pulp/host/baked_graph_processor.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace lofi = pulp::host::forge_lofi;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;

struct BakedVcfFixture {
    pulp::host::SignalGraph graph;
    pulp::host::LowerResult result;
    pulp::host::NodeId filter = 0;

    explicit BakedVcfFixture(pulp::signal::AnalogVcf::Voicing voicing) {
        const auto type = lofi::make_analog_vcf_node(voicing);
        REQUIRE(graph.register_custom_node_type(type));
        const auto input = graph.add_input_node(1, "input");
        filter = graph.add_custom_node(type.type_id, type.version, "filter");
        const auto output = graph.add_output_node(1, "output");
        REQUIRE(graph.connect(input, 0, filter, 0));
        REQUIRE(graph.connect(filter, 0, output, 0));
        graph.set_canonical_executor_routing_enabled(true);
        REQUIRE(graph.prepare(kSampleRate, kBlockSize));

        result = pulp::host::bake(graph);
        REQUIRE(result.accepted);
        REQUIRE(result.processor);
        pulp::format::PrepareContext prepare;
        prepare.sample_rate = kSampleRate;
        prepare.max_buffer_size = kBlockSize;
        prepare.input_channels = 1;
        prepare.output_channels = 1;
        result.processor->prepare(prepare);
    }

    pulp::host::BakedGraphProcessor& processor() {
        return *static_cast<pulp::host::BakedGraphProcessor*>(result.processor.get());
    }
};

std::vector<float> render_block(pulp::format::Processor& processor,
                                const std::vector<float>& input) {
    std::vector<float> output(input.size(), 0.0f);
    const float* input_ptr = input.data();
    float* output_ptr = output.data();
    pulp::audio::BufferView<const float> in(&input_ptr, 1,
                                            static_cast<std::uint32_t>(input.size()));
    pulp::audio::BufferView<float> out(&output_ptr, 1,
                                      static_cast<std::uint32_t>(output.size()));
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    pulp::format::ProcessContext process;
    process.num_samples = static_cast<int>(input.size());
    process.sample_rate = kSampleRate;
    processor.process(out, in, midi_in, midi_out, process);
    return output;
}

void inject_controls(pulp::host::ParamInjector& injector,
                     const std::array<float, 4>& values) {
    constexpr std::array ids{
        lofi::kAnalogVcfCutoff,
        lofi::kAnalogVcfCutoffMod,
        lofi::kAnalogVcfResonance,
        lofi::kAnalogVcfDriveDb,
    };
    for (std::size_t i = 0; i < ids.size(); ++i) {
        REQUIRE(injector.inject({ids[i], 0, values[i], 0}) ==
                pulp::host::InjectStatus::Ok);
    }
}

}  // namespace

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
        CHECK(node.version == 1);
        CHECK(node.num_input_ports == 1);
        CHECK(node.num_output_ports == 1);
        CHECK(node.lowerable);
        CHECK(static_cast<bool>(node.create));
        CHECK(static_cast<bool>(node.process_instance));
        CHECK(static_cast<bool>(node.process_instance_baked_param));
        REQUIRE(node.baked_params.size() == 4);
        constexpr std::array expected{
            pulp::host::CustomNodeBakedParam{lofi::kAnalogVcfCutoff, 0.0f, 1.0f, 0.5f},
            pulp::host::CustomNodeBakedParam{lofi::kAnalogVcfCutoffMod, -5.0f, 5.0f, 0.0f},
            pulp::host::CustomNodeBakedParam{lofi::kAnalogVcfResonance, 0.0f, 1.0f, 0.0f},
            pulp::host::CustomNodeBakedParam{lofi::kAnalogVcfDriveDb, -24.0f, 48.0f, 0.0f},
        };
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CHECK(node.baked_params[i].id == expected[i].id);
            CHECK(node.baked_params[i].min_value == expected[i].min_value);
            CHECK(node.baked_params[i].max_value == expected[i].max_value);
            CHECK(node.baked_params[i].default_value == expected[i].default_value);
            for (std::size_t j = i + 1; j < expected.size(); ++j)
                CHECK(node.baked_params[i].id != node.baked_params[j].id);
        }
    }
}

TEST_CASE("Forge analog VCF baked render is deterministic across identical instances",
          "[host][forge][analog-vcf][determinism]") {
    // Minimoog carries the catalog's deterministic stochastic drift state;
    // exact agreement here proves independent bake/create/prepare lifecycles
    // seed and advance that hidden state identically.
    BakedVcfFixture first(pulp::signal::AnalogVcf::Voicing::minimoog);
    BakedVcfFixture second(pulp::signal::AnalogVcf::Voicing::minimoog);
    auto first_injector = first.processor().claim_param_injection(first.filter);
    auto second_injector = second.processor().claim_param_injection(second.filter);
    REQUIRE(first_injector.valid());
    REQUIRE(second_injector.valid());
    constexpr std::array controls{0.63f, -0.75f, 0.90f, 9.0f};
    inject_controls(first_injector, controls);
    inject_controls(second_injector, controls);

    std::vector<float> input(kBlockSize);
    for (int i = 0; i < kBlockSize; ++i)
        input[static_cast<std::size_t>(i)] =
            0.2f * std::sin(0.071f * static_cast<float>(i));
    for (int block = 0; block < 64; ++block) {
        const auto first_output = render_block(*first.result.processor, input);
        const auto second_output = render_block(*second.result.processor, input);
        CHECK(first_output == second_output);
        CHECK(std::any_of(first_output.begin(), first_output.end(),
                          [](float sample) { return sample != 0.0f; }));
    }
}

TEST_CASE("Forge analog VCF maps non-finite injection to declared defaults",
          "[host][forge][analog-vcf][non-finite]") {
    BakedVcfFixture defaults(pulp::signal::AnalogVcf::Voicing::jupiter);
    BakedVcfFixture hostile(pulp::signal::AnalogVcf::Voicing::jupiter);
    auto injector = hostile.processor().claim_param_injection(hostile.filter);
    REQUIRE(injector.valid());
    inject_controls(injector,
                    {std::numeric_limits<float>::quiet_NaN(),
                     std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity(),
                     std::numeric_limits<float>::quiet_NaN()});

    std::vector<float> input(kBlockSize, 0.125f);
    input[0] = 0.5f;
    const auto expected = render_block(*defaults.result.processor, input);
    const auto actual = render_block(*hostile.result.processor, input);
    REQUIRE(actual == expected);
    for (const float sample : actual) CHECK(std::isfinite(sample));
}

TEST_CASE("Forge analog VCF injection and steady-state render allocate nothing",
          "[host][forge][analog-vcf][rt]") {
    constexpr std::array voicings{
        pulp::signal::AnalogVcf::Voicing::juno,
        pulp::signal::AnalogVcf::Voicing::jupiter,
        pulp::signal::AnalogVcf::Voicing::prophet5,
        pulp::signal::AnalogVcf::Voicing::minimoog,
    };
    for (const auto voicing : voicings) {
        BakedVcfFixture fixture(voicing);
        auto injector = fixture.processor().claim_param_injection(fixture.filter);
        REQUIRE(injector.valid());

        std::array<float, kBlockSize> input{};
        std::array<float, kBlockSize> output{};
        input[0] = 0.5f;
        const float* input_ptr = input.data();
        float* output_ptr = output.data();
        pulp::audio::BufferView<const float> in(&input_ptr, 1, kBlockSize);
        pulp::audio::BufferView<float> out(&output_ptr, 1, kBlockSize);
        pulp::midi::MidiBuffer midi_in;
        pulp::midi::MidiBuffer midi_out;
        pulp::format::ProcessContext process;
        process.num_samples = kBlockSize;
        process.sample_rate = kSampleRate;

        REQUIRE(injector.inject({lofi::kAnalogVcfCutoff, 0, 0.4f, 0}) ==
                pulp::host::InjectStatus::Ok);
        fixture.result.processor->process(out, in, midi_in, midi_out, process);

        pulp::host::InjectStatus status = pulp::host::InjectStatus::InvalidHandle;
        std::size_t allocation_count = 0;
        std::size_t allocated_bytes = 0;
        {
            pulp::test::RtAllocationProbe probe;
            status = injector.inject({lofi::kAnalogVcfCutoff, 0, 0.7f, 32});
            for (int block = 0; block < 8; ++block)
                fixture.result.processor->process(out, in, midi_in, midi_out, process);
            allocation_count = probe.allocation_count();
            allocated_bytes = probe.allocated_bytes();
        }
        CHECK(status == pulp::host::InjectStatus::Ok);
        CHECK(allocation_count == 0);
        CHECK(allocated_bytes == 0);
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
