// Every control the drum catalog declares must be able to move the audio.
//
// Registration tests prove a parameter exists, has a range, and round-trips.
// They cannot catch the failure that actually reaches a player: a knob that is
// declared, saved into presets, and given a place in a UI while the engine
// behind it never reads the value. This suite renders the node twice -- once
// at the declared default, once at another value in the declared range -- and
// requires the two renders to differ.
//
// Many controls are conditional by design: a decay time for a layer whose
// level is zero, or an LFO rate with no depth. Those are not dead knobs, so
// each one names the companion settings that make it observable, and both
// renders are taken under those settings. A control that still cannot move the
// output under any declared configuration is a contract defect.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/forge_drum_catalog.hpp>
#include <pulp/midi/buffer.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace drum = pulp::host::forge_drum;
using pulp::signal::drum::EngineId;
using namespace pulp::host;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kFrames = 128;
// Long enough for a decay tail to separate, and spanning two hits so
// per-hit lifecycle controls (restart, hit life) are reachable.
constexpr int kBlocks = 24;
constexpr int kSecondHitBlock = 10;

struct Setting {
    pulp::state::ParamID id = 0;
    float value = 0.0f;
};

std::vector<float> render_hits(const CustomNodeType& type, const std::vector<Setting>& settings) {
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(type));
    const auto node = graph.add_custom_node(type.type_id, type.version, "Drum");
    const auto out_node = graph.add_output_node(2, "Output");
    REQUIRE(graph.connect(node, 0, out_node, 0));
    REQUIRE(graph.connect(node, 1, out_node, 1));
    graph.set_canonical_executor_routing_enabled(true);
    REQUIRE(graph.prepare(kSampleRate, kFrames));
    auto lowered = bake(graph);
    REQUIRE(lowered.accepted);

    pulp::format::PrepareContext prepare_context;
    prepare_context.sample_rate = kSampleRate;
    prepare_context.max_buffer_size = kFrames;
    prepare_context.input_channels = 1;
    prepare_context.output_channels = 2;
    lowered.processor->prepare(prepare_context);
    auto& baked_processor = *static_cast<BakedGraphProcessor*>(lowered.processor.get());

    const auto set = [&](pulp::state::ParamID id, float value) {
        auto injector = baked_processor.claim_param_injection(node);
        REQUIRE(injector.valid());
        injector.inject({id, 0, value, 0});
    };

    for (const auto& setting : settings)
        set(setting.id, setting.value);

    std::vector<float> pcm;
    const std::vector<float> input(kFrames, 0.0f);
    for (int block = 0; block < kBlocks; ++block) {
        if (block == 1 || block == kSecondHitBlock + 1)
            set(drum::kTrigger, 0.0f);
        if (block == kSecondHitBlock)
            set(drum::kTrigger, 1.0f);

        std::vector<float> left(kFrames, 0.0f);
        std::vector<float> right(kFrames, 0.0f);
        const float* input_pointer = input.data();
        float* output_pointers[] = {left.data(), right.data()};
        pulp::audio::BufferView<const float> input_view(&input_pointer, 1, kFrames);
        pulp::audio::BufferView<float> output_view(output_pointers, 2, kFrames);
        pulp::midi::MidiBuffer midi_input;
        pulp::midi::MidiBuffer midi_output;
        pulp::format::ProcessContext process_context;
        process_context.sample_rate = kSampleRate;
        process_context.num_samples = kFrames;
        lowered.processor->process(output_view, input_view, midi_input, midi_output,
                                   process_context);
        pcm.insert(pcm.end(), left.begin(), left.end());
        pcm.insert(pcm.end(), right.begin(), right.end());
    }
    return pcm;
}

bool differs(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size())
        return true;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::fabs(a[i] - b[i]) > 1.0e-9f)
            return true;
    return false;
}

/// Values inside the declared range that are not the declared default. The
/// extremes come first because they are the most likely to separate, and the
/// midpoint covers controls whose ends happen to coincide in effect.
std::vector<float> probe_values(const CustomNodeBakedParam& parameter) {
    std::vector<float> values;
    const auto push = [&](float value) {
        if (!std::isfinite(value) || value == parameter.default_value)
            return;
        if (value < parameter.min_value || value > parameter.max_value)
            return;
        if (std::find(values.begin(), values.end(), value) == values.end())
            values.push_back(value);
    };
    push(parameter.min_value);
    push(parameter.max_value);
    push(0.5f * (parameter.min_value + parameter.max_value));
    push(std::min(parameter.max_value, parameter.min_value + 1.0f));
    return values;
}

/// Companion settings that make `id` observable on `engine`, applied to both
/// the baseline and the probe render. Each entry answers "what else has to be
/// true before this control can be heard at all".
std::vector<Setting> enabling_context(EngineId engine, pulp::state::ParamID id) {
    // A hit has to happen for anything but the trigger itself to be audible.
    std::vector<Setting> context;
    if (id != drum::kTrigger)
        context.push_back({drum::kTrigger, 1.0f});
    // Velocity response only spreads between hits below full scale.
    if (id != drum::kVelocity)
        context.push_back({drum::kVelocity, 0.5f});

    const auto need = [&](pulp::state::ParamID companion, float value) {
        context.push_back({companion, value});
    };

    // Shared output stage.
    if (id == drum::kOutputAttackMs || id == drum::kOutputHoldMs || id == drum::kOutputDecayMs)
        need(drum::kOutputAhdEnabled, 1.0f);
    if (id == drum::kChokeMs)
        need(drum::kChoke, 1.0f);

    switch (engine) {
    case EngineId::kick_oscillator:
    case EngineId::kick_resonant:
    case EngineId::kick_circuit:
        if (id == drum::kNoiseColor || id == drum::kKickNoiseDecayMs)
            need(drum::kKickNoiseLevel, 1.0f);
        if (id == drum::kKickFmRatio)
            need(drum::kKickFmAmount, 4.0f);
        break;
    case EngineId::snare:
        if (id == drum::kSnareRattleHz)
            need(drum::kSnareRattle, 1.0f);
        if (id == drum::kSnareShellResonance)
            need(drum::kSnareShellLevel, 1.0f);
        break;
    case EngineId::hat:
        if (id == drum::kHatGritRatio)
            need(drum::kHatGrit, 1.0f);
        break;
    case EngineId::clap:
        if (id == drum::kClapBodyHz)
            need(drum::kClapBodyLevel, 1.0f);
        break;
    case EngineId::membrane_modal:
        if (id == drum::kMembraneAirDecayMs)
            need(drum::kMembraneAirLevel, 1.0f);
        if (id == drum::kMembraneClickDecayMs)
            need(drum::kMembraneClickLevel, 1.0f);
        break;
    case EngineId::string_karplus_strong:
        if (id == drum::kStringModulation || id == drum::kStringModulationRatio ||
            id == drum::kStringFmDepthOctaves)
            need(drum::kStringModulationMix, 1.0f);
        if (id == drum::kStringModulationMix || id == drum::kStringModulationRatio ||
            id == drum::kStringFmDepthOctaves)
            need(drum::kStringModulation, 1.0f);
        if (id >= drum::kGateRiseMs && id <= drum::kGateGainExponent)
            need(drum::kStringLpgAmount, 1.0f);
        break;
    case EngineId::zap_cz:
        if (id == drum::kZapRingRatio)
            need(drum::kZapRing, 1.0f);
        break;
    case EngineId::fm2:
        if (id == drum::kNoiseColor || id == drum::kFm2NoiseDecayMs)
            need(drum::kFm2NoiseLevel, 1.0f);
        if (id == drum::kFm2CarrierWarpMs)
            need(drum::kFm2CarrierWarp, 1.0f);
        if (id == drum::kFm2ModulatorWarpMs)
            need(drum::kFm2ModulatorWarp, 1.0f);
        if (id == drum::kFm2LfoRateHz || id == drum::kFm2LfoDelayMs || id == drum::kFm2LfoFadeMs)
            need(drum::kFm2LfoDepthOctaves, 1.0f);
        break;
    case EngineId::fm6:
        if (id == drum::kPitchSweepMs)
            need(drum::kPitchSweepOctaves, 2.0f);
        break;
    case EngineId::fm8:
        if (id == drum::kNoiseColor || id == drum::kFm8NoiseDecayMs)
            need(drum::kFm8NoiseLevel, 1.0f);
        break;
    default:
        break;
    }
    return context;
}

} // namespace

TEST_CASE("Every declared drum control can move the rendered audio",
          "[forge-drum][contract][parity]") {
    for (const auto& metadata : pulp::signal::drum::engine_registry) {
        if (!metadata.available)
            continue;
        const auto type = drum::make_drum_node(metadata.id);
        if (type.type_id.empty())
            continue;

        for (const auto& parameter : type.baked_params) {
            const auto context = enabling_context(metadata.id, parameter.id);
            auto baseline_settings = context;
            baseline_settings.push_back({parameter.id, parameter.default_value});
            const auto baseline = render_hits(type, baseline_settings);

            bool moved = false;
            for (const float candidate : probe_values(parameter)) {
                auto probe_settings = context;
                probe_settings.push_back({parameter.id, candidate});
                if (differs(baseline, render_hits(type, probe_settings))) {
                    moved = true;
                    break;
                }
            }

            INFO("engine=" << type.type_id << " param=" << parameter.id
                           << " min=" << parameter.min_value << " max=" << parameter.max_value
                           << " default=" << parameter.default_value);
            CHECK(moved);
        }
    }
}
