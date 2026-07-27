// Forge modulation catalog boundary contracts.
//
// This suite owns the registry, lowering, injected-value sanitation,
// determinism, and RT-allocation contracts. Behavioral DSP measurements remain
// in test_forge_modulation_catalog.cpp.

#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/forge_modulation_catalog.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace pulp::host;
namespace catalog = pulp::host::forge_modulation;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kFrames = 128;

struct ExpectedParam {
    pulp::state::ParamID id;
    float min;
    float max;
    float default_value;
};

struct CatalogCase {
    CustomNodeType type;
    const char* type_id;
    int input_ports;
    std::vector<ExpectedParam> params;
};

std::vector<CatalogCase> catalog_cases() {
    using P = ExpectedParam;
    return {
        {
            catalog::make_mod_lfo_node(),
            "forge_mod_lfo",
            0,
            {
                P{catalog::kModLfoRateHz, 0.001f, 2000.0f, 2.0f},
                P{catalog::kModLfoDepth, 0.0f, 1.0f, 1.0f},
                P{catalog::kModLfoWave, 0.0f, 6.0f, 0.0f},
                P{catalog::kModLfoPulseWidth, 0.05f, 0.95f, 0.5f},
                P{catalog::kModLfoRandomBlend, 0.0f, 1.0f, 0.0f},
                P{catalog::kModLfoDelayMs, 0.0f, 5000.0f, 0.0f},
                P{catalog::kModLfoFadeInMs, 0.0f, 5000.0f, 0.0f},
                P{catalog::kModLfoShapeMorph, 0.0f, 3.0f, 0.0f},
                P{catalog::kModLfoMorphEnabled, 0.0f, 1.0f, 0.0f},
                P{catalog::kModLfoTriangleBias, -1.0f, 1.0f, 0.0f},
                P{catalog::kModLfoRandomSegments, 1.0f,
                  static_cast<float>(pulp::signal::Lfo::kMaxRandomSegments),
                  static_cast<float>(pulp::signal::Lfo::kDefaultRandomSegments)},
                P{catalog::kModLfoPhaseDegrees, 0.0f, 360.0f, 0.0f},
                P{catalog::kModLfoFadeOutMs, 0.0f, 5000.0f, 0.0f},
                P{catalog::kModLfoFadeQuadratic, 0.0f, 1.0f, 0.0f},
                P{catalog::kModLfoRepeatCount, 0.0f,
                  static_cast<float>(pulp::signal::Lfo::kMaxRepeatCount), 0.0f},
            },
        },
        {
            catalog::make_mod_lpg_node(),
            "forge_mod_lpg",
            2,
            {
                P{catalog::kModLpgDecayMs, 20.0f, 2000.0f, 150.0f},
                P{catalog::kModLpgColour, 0.0f, 1.0f, 0.5f},
                P{catalog::kModLpgDroop, 0.0f, 0.95f, 0.5f},
                P{catalog::kModLpgBrightnessHz, 500.0f, 12000.0f, 12000.0f},
                P{catalog::kModLpgStruck, 0.0f, 1.0f, 0.0f},
                P{catalog::kModLpgRiseMs, 0.05f, 100.0f,
                  static_cast<float>(pulp::signal::Lpg::kRiseMs)},
                P{catalog::kModLpgDarknessHz, pulp::signal::Lpg::kMinFcHz,
                  500.0f, pulp::signal::Lpg::kDefaultFcMinHz},
                P{catalog::kModLpgStrikeThreshold, 0.01f, 0.99f,
                  catalog::kDefaultModLpgStrikeThreshold},
                P{catalog::kModLpgRefractoryMs, 0.0f, 100.0f, 2.0f},
            },
        },
        {
            catalog::make_mod_slew_node(),
            "forge_mod_slew",
            1,
            {
                P{catalog::kModSlewRiseMs, 0.0f, 2000.0f, 20.0f},
                P{catalog::kModSlewFallMs, 0.0f, 2000.0f, 20.0f},
                P{catalog::kModSlewCurved, 0.0f, 1.0f, 0.0f},
            },
        },
        {
            catalog::make_mod_transient_node(),
            "forge_mod_transient",
            1,
            {
                P{catalog::kModTransientFastMs, 0.5f, 20.0f, 2.0f},
                P{catalog::kModTransientSlowMs, 10.0f, 500.0f, 40.0f},
                P{catalog::kModTransientSensitivity, 0.1f, 8.0f, 1.0f},
                P{catalog::kModTransientInvert, 0.0f, 1.0f, 0.0f},
            },
        },
        {
            catalog::make_mod_env_node(),
            "forge_mod_env",
            1,
            {
                P{catalog::kModEnvAttackMs, 0.1f, 500.0f, 2.0f},
                P{catalog::kModEnvHoldMs, 0.0f, 500.0f, 0.0f},
                P{catalog::kModEnvDecayMs, 1.0f, 2000.0f, 150.0f},
                P{catalog::kModEnvCurve, -1.0f, 1.0f, 0.0f},
                P{catalog::kModEnvThreshold, 0.05f, 0.95f, 0.3f},
                P{catalog::kModEnvDelayMs, 0.0f, 5000.0f, 0.0f},
                P{catalog::kModEnvDepth, 0.0f, 1.0f, 1.0f},
                P{catalog::kModEnvLoop, 0.0f, 1.0f, 0.0f},
                P{catalog::kModEnvLoopCount, 0.0f, 128.0f, 0.0f},
                P{catalog::kModEnvRefractoryMs, 0.0f, 100.0f, 2.0f},
                P{catalog::kModEnvVelocitySensitive, 0.0f, 1.0f, 0.0f},
                P{catalog::kModEnvIndependentCurves, 0.0f, 1.0f, 0.0f},
                P{catalog::kModEnvAttackCurve, -1.0f, 1.0f, 0.0f},
                P{catalog::kModEnvDecayCurve, -1.0f, 1.0f, 0.0f},
            },
        },
    };
}

pulp::state::ParameterEvent immediate(pulp::state::ParamID id, float value) {
    return {id, /*sample_offset=*/0, value, /*ramp_duration_sample_frames=*/0};
}

struct BakedFixture {
    SignalGraph graph;
    LowerResult result;
    NodeId node = 0;

    explicit BakedFixture(const CustomNodeType& type) {
        REQUIRE(graph.register_custom_node_type(type));
        const auto input = graph.add_input_node(std::max(1, type.num_input_ports), "In");
        node = graph.add_custom_node(type.type_id, 1, "Node");
        const auto output = graph.add_output_node(type.num_output_ports, "Out");
        for (int port = 0; port < type.num_input_ports; ++port) {
            REQUIRE(graph.connect(input, static_cast<PortIndex>(port), node,
                                  static_cast<PortIndex>(port)));
        }
        for (int port = 0; port < type.num_output_ports; ++port) {
            REQUIRE(graph.connect(node, static_cast<PortIndex>(port), output,
                                  static_cast<PortIndex>(port)));
        }
        graph.set_canonical_executor_routing_enabled(true);
        REQUIRE(graph.prepare(kSampleRate, kFrames));

        result = bake(graph);
        REQUIRE(result.accepted);
        REQUIRE(result.processor);
        REQUIRE(result.reason == LowerRejectReason::None);

        pulp::format::PrepareContext context;
        context.sample_rate = kSampleRate;
        context.max_buffer_size = kFrames;
        context.input_channels = std::max(1, type.num_input_ports);
        context.output_channels = type.num_output_ports;
        result.processor->prepare(context);
    }

    ParamInjector injector() {
        auto value = baked().claim_param_injection(node);
        REQUIRE(value.valid());
        return value;
    }

    BakedGraphProcessor& baked() {
        return *static_cast<BakedGraphProcessor*>(result.processor.get());
    }
};

struct ProcessBuffers {
    std::array<std::array<float, kFrames>, 2> input{};
    std::array<const float*, 2> input_ptrs{};
    std::array<float, kFrames> output{};
    float* output_ptr = output.data();
    pulp::audio::BufferView<const float> input_view;
    pulp::audio::BufferView<float> output_view;
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    pulp::format::ProcessContext context;

    explicit ProcessBuffers(int input_ports)
        : input_view(input_ptrs.data(), static_cast<std::uint32_t>(std::max(1, input_ports)),
                     static_cast<std::uint32_t>(kFrames)),
          output_view(&output_ptr, 1u, static_cast<std::uint32_t>(kFrames)) {
        for (std::size_t channel = 0; channel < input.size(); ++channel) {
            input_ptrs[channel] = input[channel].data();
        }
        for (int sample = 0; sample < kFrames; ++sample) {
            input[0][static_cast<std::size_t>(sample)] =
                sample == 0 ? 1.0f : ((sample & 1) == 0 ? 0.25f : -0.25f);
            input[1][static_cast<std::size_t>(sample)] = 0.8f;
        }
        context.sample_rate = kSampleRate;
        context.num_samples = kFrames;
    }

    const std::array<float, kFrames>& process(pulp::format::Processor& processor) {
        output.fill(0.0f);
        processor.process(output_view, input_view, midi_in, midi_out, context);
        return output;
    }
};

bool all_finite(const std::array<float, kFrames>& samples) {
    return std::all_of(samples.begin(), samples.end(),
                       [](float value) { return std::isfinite(value); });
}

} // namespace

TEST_CASE("Forge modulation catalog freezes every public node and parameter contract",
          "[host][forge][modulation][contract]") {
    std::vector<std::string> type_ids;
    for (const auto& item : catalog_cases()) {
        INFO("node " << item.type_id);
        REQUIRE(item.type.type_id == item.type_id);
        REQUIRE(item.type.version == 1);
        REQUIRE(item.type.num_input_ports == item.input_ports);
        REQUIRE(item.type.num_output_ports == 1);
        REQUIRE(item.type.lowerable);
        REQUIRE(static_cast<bool>(item.type.create));
        REQUIRE(static_cast<bool>(item.type.destroy));
        REQUIRE(static_cast<bool>(item.type.prepare));
        REQUIRE(static_cast<bool>(item.type.reset));
        REQUIRE(static_cast<bool>(item.type.process_instance_baked_param));
        REQUIRE(item.type.baked_params.size() == item.params.size());

        SignalGraph registration;
        REQUIRE(registration.register_custom_node_type(item.type));

        for (std::size_t index = 0; index < item.params.size(); ++index) {
            const auto& actual = item.type.baked_params[index];
            const auto& expected = item.params[index];
            INFO("parameter index " << index);
            REQUIRE(actual.id == expected.id);
            REQUIRE(actual.min_value == expected.min);
            REQUIRE(actual.max_value == expected.max);
            REQUIRE(actual.default_value == expected.default_value);
            REQUIRE(std::isfinite(actual.min_value));
            REQUIRE(std::isfinite(actual.max_value));
            REQUIRE(std::isfinite(actual.default_value));
            REQUIRE(actual.min_value <= actual.default_value);
            REQUIRE(actual.default_value <= actual.max_value);
        }
        type_ids.push_back(item.type.type_id);
    }

    std::sort(type_ids.begin(), type_ids.end());
    REQUIRE(std::adjacent_find(type_ids.begin(), type_ids.end()) == type_ids.end());
}

TEST_CASE("Every Forge modulation node registers and lowers through the baked executor",
          "[host][forge][modulation][lowering]") {
    for (const auto& item : catalog_cases()) {
        INFO("node " << item.type_id);
        BakedFixture fixture(item.type);
        ProcessBuffers buffers(item.input_ports);
        REQUIRE(all_finite(buffers.process(*fixture.result.processor)));
    }
}

TEST_CASE("Every Forge modulation parameter accepts its finite contract points",
          "[host][forge][modulation][injection]") {
    for (const auto& item : catalog_cases()) {
        BakedFixture fixture(item.type);
        auto injector = fixture.injector();
        ProcessBuffers buffers(item.input_ports);

        for (const auto& param : item.params) {
            INFO("node " << item.type_id << ", parameter " << param.id);
            for (float value : {param.min, param.default_value, param.max}) {
                REQUIRE(injector.inject(immediate(param.id, value)) == InjectStatus::Ok);
                REQUIRE(all_finite(buffers.process(*fixture.result.processor)));
            }
        }
    }
}

TEST_CASE("Non-finite Forge modulation injections use defaults and recover",
          "[host][forge][modulation][injection][nonfinite]") {
    const std::array invalid_values{
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };

    for (const auto& item : catalog_cases()) {
        BakedFixture reference(item.type);
        ProcessBuffers reference_buffers(item.input_ports);
        const auto expected = reference_buffers.process(*reference.result.processor);

        for (const auto& param : item.params) {
            for (float invalid : invalid_values) {
                INFO("node " << item.type_id << ", parameter " << param.id);
                CAPTURE(invalid);
                BakedFixture fixture(item.type);
                auto injector = fixture.injector();
                ProcessBuffers buffers(item.input_ports);

                REQUIRE(injector.inject(immediate(param.id, invalid)) == InjectStatus::Ok);
                const auto actual = buffers.process(*fixture.result.processor);
                REQUIRE(actual == expected);

                const float recovery = param.default_value == param.max ? param.min : param.max;
                REQUIRE(injector.inject(immediate(param.id, recovery)) == InjectStatus::Ok);
                REQUIRE(all_finite(buffers.process(*fixture.result.processor)));
            }
        }
    }
}

TEST_CASE("Forge modulation random LFO bakes deterministically and is nontrivial",
          "[host][forge][modulation][determinism]") {
    BakedFixture first(catalog::make_mod_lfo_node());
    BakedFixture second(catalog::make_mod_lfo_node());
    auto first_injector = first.injector();
    auto second_injector = second.injector();
    for (auto* injector : {&first_injector, &second_injector}) {
        REQUIRE(injector->inject(immediate(catalog::kModLfoRateHz, 12.0f)) == InjectStatus::Ok);
        REQUIRE(injector->inject(immediate(catalog::kModLfoDepth, 1.0f)) == InjectStatus::Ok);
        REQUIRE(injector->inject(immediate(catalog::kModLfoWave, 5.0f)) == InjectStatus::Ok);
    }

    ProcessBuffers first_buffers(/*input_ports=*/0);
    ProcessBuffers second_buffers(/*input_ports=*/0);
    float minimum = 1.0f;
    float maximum = 0.0f;
    for (int block = 0; block < 100; ++block) {
        const auto a = first_buffers.process(*first.result.processor);
        const auto b = second_buffers.process(*second.result.processor);
        REQUIRE(a == b);
        const auto bounds = std::minmax_element(a.begin(), a.end());
        minimum = std::min(minimum, *bounds.first);
        maximum = std::max(maximum, *bounds.second);
    }
    REQUIRE(maximum - minimum > 0.3f);
}

TEST_CASE("Forge modulation inject process and reset paths allocate nothing",
          "[host][forge][modulation][rt-safety]") {
    for (const auto& item : catalog_cases()) {
        INFO("node " << item.type_id);
        BakedFixture fixture(item.type);
        auto injector = fixture.injector();
        ProcessBuffers buffers(item.input_ports);
        (void)buffers.process(*fixture.result.processor);

        void* raw_instance = item.type.create();
        REQUIRE(raw_instance != nullptr);
        item.type.prepare(raw_instance, kSampleRate, kFrames);

        const auto& param = item.params.front();
        InjectStatus status = InjectStatus::InvalidHandle;
        std::size_t allocation_count = 0;
        std::size_t allocation_bytes = 0;
        {
            pulp::test::RtAllocationProbe probe;
            status = injector.inject(immediate(param.id, param.max));
            (void)buffers.process(*fixture.result.processor);
            item.type.reset(raw_instance);
            allocation_count = probe.allocation_count();
            allocation_bytes = probe.allocated_bytes();
        }

        item.type.destroy(raw_instance);
        REQUIRE(status == InjectStatus::Ok);
        REQUIRE(allocation_count == 0);
        REQUIRE(allocation_bytes == 0);
    }
}
