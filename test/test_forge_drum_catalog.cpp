#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/forge_drum_catalog.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace drum = pulp::host::forge_drum;
using namespace pulp::host;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kFrames = 128;

struct Fixture {
    SignalGraph graph;
    LowerResult lowered;
    NodeId node = 0;

    explicit Fixture(const CustomNodeType& type) {
        REQUIRE(graph.register_custom_node_type(type));
        node = graph.add_custom_node(type.type_id, type.version, "Drum");
        const auto output = graph.add_output_node(2, "Output");
        REQUIRE(graph.connect(node, 0, output, 0));
        REQUIRE(graph.connect(node, 1, output, 1));
        graph.set_canonical_executor_routing_enabled(true);
        REQUIRE(graph.prepare(kSampleRate, kFrames));
        lowered = bake(graph);
        REQUIRE(lowered.accepted);
        REQUIRE(lowered.processor);

        pulp::format::PrepareContext context;
        context.sample_rate = kSampleRate;
        context.max_buffer_size = kFrames;
        context.input_channels = 1;
        context.output_channels = 2;
        lowered.processor->prepare(context);
    }

    BakedGraphProcessor& baked() {
        return *static_cast<BakedGraphProcessor*>(lowered.processor.get());
    }
};

std::array<std::vector<float>, 2> render(Fixture& fixture) {
    std::vector<float> input(kFrames, 0.0f);
    const float* input_pointer = input.data();
    std::array<std::vector<float>, 2> output{std::vector<float>(kFrames, 0.0f),
                                             std::vector<float>(kFrames, 0.0f)};
    float* output_pointers[] = {output[0].data(), output[1].data()};
    pulp::audio::BufferView<const float> input_view(&input_pointer, 1, kFrames);
    pulp::audio::BufferView<float> output_view(output_pointers, 2, kFrames);
    pulp::midi::MidiBuffer midi_input;
    pulp::midi::MidiBuffer midi_output;
    pulp::format::ProcessContext context;
    context.sample_rate = kSampleRate;
    context.num_samples = kFrames;
    fixture.lowered.processor->process(output_view, input_view, midi_input, midi_output, context);
    return output;
}

void inject(Fixture& fixture, pulp::state::ParamID id, float value) {
    auto injector = fixture.baked().claim_param_injection(fixture.node);
    REQUIRE(injector.valid());
    REQUIRE(injector.inject({id, 0, value, 0}) == pulp::host::InjectStatus::Ok);
}

double energy(const std::array<std::vector<float>, 2>& block) {
    double sum = 0.0;
    for (const auto& channel : block)
        for (const float sample : channel)
            sum += sample * sample;
    return sum;
}

const CustomNodeBakedParam& parameter(const CustomNodeType& type, pulp::state::ParamID id) {
    const auto found = std::find_if(type.baked_params.begin(), type.baked_params.end(),
                                    [id](const auto& item) { return item.id == id; });
    REQUIRE(found != type.baked_params.end());
    return *found;
}

} // namespace

TEST_CASE("Forge drum catalog registers every available stable engine identity",
          "[host][forge][drum][contract]") {
    using pulp::signal::drum::EngineId;
    constexpr std::array cases{
        std::pair{EngineId::kick_oscillator, drum::kKickOscillatorTypeId},
        std::pair{EngineId::kick_resonant, drum::kKickResonantTypeId},
        std::pair{EngineId::kick_circuit, drum::kKickCircuitTypeId},
        std::pair{EngineId::snare, drum::kSnareTypeId},
        std::pair{EngineId::hat, drum::kHatTypeId},
        std::pair{EngineId::clap, drum::kClapTypeId},
        std::pair{EngineId::tom_generic, drum::kTomGenericTypeId},
        std::pair{EngineId::tom_simmons, drum::kTomSimmonsTypeId},
        std::pair{EngineId::cymbal_comb, drum::kCymbalTypeId},
        std::pair{EngineId::membrane_modal, drum::kMembraneTypeId},
        std::pair{EngineId::string_karplus_strong, drum::kStringTypeId},
        std::pair{EngineId::zap_cz, drum::kZapTypeId},
        std::pair{EngineId::fm2, drum::kFm2TypeId},
        std::pair{EngineId::fm6, drum::kFm6TypeId},
        std::pair{EngineId::fm8, drum::kFm8TypeId},
    };
    for (const auto& [engine, id] : cases) {
        const auto type = drum::make_drum_node(engine);
        CHECK(type.type_id == id);
        CHECK(type.lowerable);
        CHECK(type.num_input_ports == 0);
        CHECK(type.num_output_ports == 2);
        REQUIRE(type.create);
        REQUIRE(type.process_instance_baked_param);
        REQUIRE(type.baked_params.size() > 16);
        for (const auto& parameter : type.baked_params) {
            CHECK(parameter.id != 0);
            CHECK(std::isfinite(parameter.min_value));
            CHECK(std::isfinite(parameter.max_value));
            CHECK(std::isfinite(parameter.default_value));
            CHECK(parameter.min_value <= parameter.default_value);
            CHECK(parameter.default_value <= parameter.max_value);
        }
    }
    CHECK(drum::make_drum_node(EngineId::dx7_msfa).type_id.empty());
}

TEST_CASE("Forge drum stable common parameter contract is honest",
          "[host][forge][drum][contract]") {
    const auto type = drum::make_drum_node(pulp::signal::drum::EngineId::kick_oscillator);
    REQUIRE(type.baked_params.size() >= 16);
    CHECK(type.baked_params[0].id == drum::kTrigger);
    CHECK(type.baked_params[0].min_value == 0.0f);
    CHECK(type.baked_params[0].max_value == 1.0f);
    CHECK(type.baked_params[0].default_value == 0.0f);
    CHECK(type.baked_params[1].id == drum::kVelocity);
    CHECK(type.baked_params[1].default_value == 1.0f);
    CHECK(type.baked_params[3].id == drum::kChokeMs);
    CHECK(type.baked_params[3].default_value == 4.0f);
    const auto tune = std::find_if(type.baked_params.begin(), type.baked_params.end(),
                                   [](const auto& p) { return p.id == drum::kTuneHz; });
    REQUIRE(tune != type.baked_params.end());
    CHECK(tune->min_value == 20.0f);
    CHECK(tune->max_value == 400.0f);
    CHECK(tune->default_value == 55.0f);

    const auto clap = drum::make_drum_node(pulp::signal::drum::EngineId::clap);
    CHECK(parameter(clap, drum::kClapStereoWidth).max_value == 1.0f);
    const auto membrane = drum::make_drum_node(pulp::signal::drum::EngineId::membrane_modal);
    CHECK(parameter(membrane, drum::kMembraneExciterCutoffHz).min_value == 100.0f);
    CHECK(parameter(membrane, drum::kMembraneClickDecayMs).min_value == 0.1f);
    CHECK(parameter(membrane, drum::kMembraneClickDecayMs).max_value == 50.0f);
    const auto fm2 = drum::make_drum_node(pulp::signal::drum::EngineId::fm2);
    CHECK(parameter(fm2, drum::kControl4).max_value == 25.0f);
    CHECK(parameter(fm2, drum::kControl11).max_value == 2.0f);
}

TEST_CASE("Forge drum preparation uses the graph zero-latency contract",
          "[host][forge][drum][latency]") {
    const auto type = drum::make_drum_node(pulp::signal::drum::EngineId::cymbal_comb);
    void* opaque = type.create();
    REQUIRE(opaque != nullptr);
    type.prepare(opaque, kSampleRate, kFrames);
    const auto& instance = *static_cast<drum::detail::DrumInstance*>(opaque);
    CHECK(instance.voice->output_oversampling() == pulp::signal::drum::OutputOversampling::bypass);
    CHECK(instance.voice->latency_samples() == 0);
    type.destroy(opaque);
}

TEST_CASE("Forge drum lowering injects a hit and remains deterministic",
          "[host][forge][drum][lowering][injection]") {
    const auto type = drum::make_drum_node(pulp::signal::drum::EngineId::kick_oscillator);
    Fixture first(type);
    Fixture second(type);
    inject(first, drum::kTrigger, 1.0f);
    inject(second, drum::kTrigger, 1.0f);
    const auto a = render(first);
    const auto b = render(second);
    CHECK(energy(a) > 0.0);
    CHECK(a == b);

    inject(first, drum::kTrigger, 0.0f);
    render(first);
    inject(first, drum::kVelocity, 0.1f);
    render(first);
    inject(first, drum::kTrigger, 1.0f);
    const auto quiet = render(first);
    CHECK(energy(quiet) < energy(a));
}

TEST_CASE("Forge drum sanitizes non-finite injection and allocates nothing in process",
          "[host][forge][drum][rt-safety]") {
    const auto type = drum::make_drum_node(pulp::signal::drum::EngineId::cymbal_comb);
    Fixture fixture(type);
    {
        auto injector = fixture.baked().claim_param_injection(fixture.node);
        REQUIRE(injector.valid());
        CHECK(injector.inject({drum::kTuneHz, 0, std::numeric_limits<float>::quiet_NaN(), 0}) ==
              pulp::host::InjectStatus::Ok);
    }
    inject(fixture, drum::kNoiseColor, 1.0f);
    render(fixture);
    inject(fixture, drum::kTrigger, 1.0f);
    for (int block = 0; block < 4; ++block)
        render(fixture);

    std::vector<float> input(kFrames, 0.0f);
    const float* input_pointer = input.data();
    std::array<std::vector<float>, 2> output{std::vector<float>(kFrames, 0.0f),
                                             std::vector<float>(kFrames, 0.0f)};
    float* output_pointers[] = {output[0].data(), output[1].data()};
    pulp::audio::BufferView<const float> input_view(&input_pointer, 1, kFrames);
    pulp::audio::BufferView<float> output_view(output_pointers, 2, kFrames);
    pulp::midi::MidiBuffer midi_input;
    pulp::midi::MidiBuffer midi_output;
    pulp::format::ProcessContext context;
    context.sample_rate = kSampleRate;
    context.num_samples = kFrames;
    pulp::test::RtAllocationProbe probe;
    fixture.lowered.processor->process(output_view, input_view, midi_input, midi_output, context);
    CHECK(probe.allocation_count() == 0);
    for (const auto& channel : output)
        for (const float sample : channel)
            CHECK(std::isfinite(sample));

    const auto circuit_type = drum::make_drum_node(pulp::signal::drum::EngineId::kick_circuit);
    Fixture circuit(circuit_type);
    inject(circuit, drum::kCircuitC41, std::numeric_limits<float>::quiet_NaN());
    inject(circuit, drum::kCircuitR161, -std::numeric_limits<float>::infinity());
    inject(circuit, drum::kTrigger, 1.0f);
    const auto circuit_output = render(circuit);
    CHECK(energy(circuit_output) > 0.0);
    for (const auto& channel : circuit_output)
        for (const float sample : channel)
            CHECK(std::isfinite(sample));
}
