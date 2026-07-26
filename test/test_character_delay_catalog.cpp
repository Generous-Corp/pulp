// Multi-character delay — host catalog integration.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/forge_character_delay_catalog.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/signal/character_delay.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace cd = pulp::signal::chardelay;
using Engine = pulp::signal::CharacterDelay;
using Character = Engine::Character;
using TapeTier = Engine::TapeTier;

namespace {

constexpr double kSr = 48000.0;

int peak_index(const std::vector<float>& samples, int from, int to) {
    int best = from;
    double best_value = -1.0;
    const int end = std::min(to, static_cast<int>(samples.size()));
    for (int i = std::max(0, from); i < end; ++i) {
        const double magnitude =
            std::abs(static_cast<double>(samples[static_cast<std::size_t>(i)]));
        if (magnitude > best_value) {
            best_value = magnitude;
            best = i;
        }
    }
    return best;
}
}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Catalog node
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("every character registers as a distinct catalog node",
          "[character-delay][catalog]") {
    namespace catalog = pulp::host::character_delay;
    pulp::host::SignalGraph graph;

    const auto nodes = {
        catalog::make_character_delay_node(Character::clean),
        catalog::make_character_delay_node(Character::vintage_digital),
        catalog::make_character_delay_node(Character::tape),
        catalog::make_character_delay_node(Character::tape, TapeTier::physical),
        catalog::make_character_delay_node(Character::bbd),
        catalog::make_character_delay_node(Character::diffusion),
    };

    for (const auto& type : nodes) {
        INFO("type " << type.type_id);
        CHECK(type.num_input_ports == 2);
        CHECK(type.num_output_ports == 2);
        CHECK(type.lowerable);
        CHECK(type.baked_params.size() == 10);
        CHECK(static_cast<bool>(type.process_instance_baked_param));
        CHECK(graph.register_custom_node_type(type));
    }
    // Each character must claim a DISTINCT id — that identity is what a baked
    // artifact resolves against.
    std::vector<std::string> ids;
    for (const auto& type : nodes) ids.push_back(type.type_id);
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

TEST_CASE("the catalog node bakes and delays", "[character-delay][catalog]") {
    namespace catalog = pulp::host::character_delay;
    using namespace pulp::host;

    const auto type = catalog::make_character_delay_node(Character::clean);
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(type));
    const auto input = graph.add_input_node(2, "In");
    const auto node = graph.add_custom_node(type.type_id, 1, "Engine");
    const auto output = graph.add_output_node(2, "Out");
    for (PortIndex port = 0; port < 2; ++port) {
        REQUIRE(graph.connect(input, port, node, port));
        REQUIRE(graph.connect(node, port, output, port));
    }
    graph.set_canonical_executor_routing_enabled(true);
    REQUIRE(graph.prepare(kSr, 512));

    auto result = bake(graph);
    REQUIRE(result.accepted);
    REQUIRE(result.processor);

    pulp::format::PrepareContext prepare_context;
    prepare_context.sample_rate = kSr;
    prepare_context.max_buffer_size = 512;
    prepare_context.input_channels = 2;
    prepare_context.output_channels = 2;
    result.processor->prepare(prepare_context);

    // The node defaults to 350 ms; render long enough to see the repeat.
    const int frames = 512;
    const int blocks = static_cast<int>(kSr * 0.5) / frames;
    std::vector<float> left(static_cast<std::size_t>(frames), 0.0f);
    std::vector<float> right(static_cast<std::size_t>(frames), 0.0f);
    std::vector<float> captured;
    captured.reserve(static_cast<std::size_t>(blocks * frames));

    for (int block = 0; block < blocks; ++block) {
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        if (block == 0) left[0] = 1.0f;

        const float* in_pointers[] = {left.data(), right.data()};
        float* out_pointers[] = {left.data(), right.data()};
        pulp::audio::BufferView<const float> in_view(in_pointers, 2,
                                                     static_cast<std::uint32_t>(frames));
        pulp::audio::BufferView<float> out_view(out_pointers, 2,
                                                static_cast<std::uint32_t>(frames));
        pulp::midi::MidiBuffer midi_in;
        pulp::midi::MidiBuffer midi_out;
        pulp::format::ProcessContext context;
        context.sample_rate = kSr;
        context.num_samples = frames;
        result.processor->process(out_view, in_view, midi_in, midi_out, context);
        captured.insert(captured.end(), left.begin(), left.end());
    }

    const int index = peak_index(captured, 1, static_cast<int>(captured.size()));
    INFO("baked repeat at " << index << ", expected " << 0.35 * kSr);
    CHECK(std::abs(index - 0.35 * kSr) <= 0.002 * kSr);
}
