#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/runtime/crypto.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

struct RenderResult {
    std::vector<float> left;
    std::vector<float> right;
};

RenderResult render(const pulp::host::PluginInfo& info) {
    auto slot = pulp::host::PluginSlot::load(info);
    REQUIRE(slot);
    REQUIRE(slot->is_loaded());

    pulp::host::SignalGraph graph;
    const auto midi = graph.add_midi_input_node();
    const auto input = graph.add_input_node(info.num_inputs);
    const auto plugin = graph.add_plugin_node(std::move(slot), info.num_inputs, info.num_outputs,
                                              "external fixture");
    const auto output = graph.add_output_node(info.num_outputs);
    REQUIRE(graph.connect_midi(midi, plugin));
    for (int ch = 0; ch < info.num_inputs; ++ch)
        REQUIRE(graph.connect(input, ch, plugin, ch));
    for (int ch = 0; ch < info.num_outputs; ++ch)
        REQUIRE(graph.connect(plugin, ch, output, ch));
    REQUIRE(graph.prepare(48000.0, 128));

    pulp::midi::MidiBuffer events;
    events.add(pulp::midi::MidiEvent::note_on(0, 60, 100));
    REQUIRE(graph.inject_midi(midi, events));

    std::array<float, 128> left{};
    std::array<float, 128> right{};
    std::array<float*, 2> out_channels{left.data(), right.data()};
    pulp::audio::BufferView<float> out(out_channels.data(), info.num_outputs, 128);
    std::array<float, 128> silence{};
    std::array<const float*, 2> in_channels{silence.data(), silence.data()};
    pulp::audio::BufferView<const float> in(in_channels.data(), info.num_inputs, 128);
    RenderResult result;
    for (int block = 0; block < 64; ++block) {
        graph.process(out, in, 128);
        result.left.insert(result.left.end(), left.begin(), left.end());
        result.right.insert(result.right.end(), right.begin(), right.end());
    }
    return result;
}

std::uint64_t fingerprint(const RenderResult& result) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto* channel : {&result.left, &result.right}) {
        for (const float value : *channel) {
            hash ^= std::bit_cast<std::uint32_t>(value);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

std::filesystem::path main_executable(const std::filesystem::path& bundle) {
    std::vector<std::filesystem::path> candidates;
    const auto root = bundle / "Contents" / "MacOS";
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() != ".json")
            candidates.push_back(entry.path());
    }
    REQUIRE(candidates.size() == 1);
    return candidates.front();
}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
    return pulp::runtime::sha256_hex(bytes.data(), bytes.size());
}

struct GraphParts {
    pulp::host::SignalGraph graph;
    pulp::host::NodeId input{};
    pulp::host::NodeId plugin{};
    pulp::host::NodeId output{};
};

std::unique_ptr<GraphParts> make_graph(const pulp::host::PluginInfo& info,
                                       std::unique_ptr<pulp::host::PluginSlot> slot) {
    auto parts = std::make_unique<GraphParts>();
    parts->input = parts->graph.add_input_node(info.num_inputs);
    parts->plugin = parts->graph.add_plugin_node(std::move(slot), info.num_inputs, info.num_outputs,
                                                 "external lifecycle");
    parts->output = parts->graph.add_output_node(info.num_outputs);
    for (int ch = 0; ch < info.num_inputs; ++ch)
        REQUIRE(parts->graph.connect(parts->input, ch, parts->plugin, ch));
    for (int ch = 0; ch < info.num_outputs; ++ch)
        REQUIRE(parts->graph.connect(parts->plugin, ch, parts->output, ch));
    REQUIRE(parts->graph.prepare(48000.0, 128));
    return parts;
}

RenderResult render_one(GraphParts& parts) {
    std::array<float, 128> left{};
    std::array<float, 128> right{};
    std::array<float*, 2> outputs{left.data(), right.data()};
    pulp::audio::BufferView<float> out(outputs.data(), 2, 128);
    std::array<float, 128> silence{};
    std::array<const float*, 2> inputs{silence.data(), silence.data()};
    pulp::audio::BufferView<const float> in(inputs.data(), 2, 128);
    parts.graph.process(out, in, 128);
    return {{left.begin(), left.end()}, {right.begin(), right.end()}};
}

} // namespace

TEST_CASE("external CLAP loads and renders through a signal graph", "[host][graph][external]") {
    const char* path = std::getenv("PULP_EXTERNAL_CLAP_PATH");
    REQUIRE(path);
    REQUIRE(std::filesystem::exists(path));
    const auto plugins =
        pulp::host::PluginScanner::scan_bundle(path, pulp::host::PluginFormat::CLAP);
    REQUIRE(plugins.size() == 1);
    const auto& info = plugins.front();
    std::cout << "descriptor id=" << info.unique_id << " inputs=" << info.num_inputs
              << " outputs=" << info.num_outputs << " midi_in=" << info.supports_midi_in << '\n';
    REQUIRE(info.path == path);
    REQUIRE(info.num_inputs == 2);
    REQUIRE(info.num_outputs == 2);

    const auto first = render(info);
    const auto second = render(info);
    REQUIRE(first.left.size() == 8192);
    REQUIRE(first.right.size() == 8192);
    double energy = 0.0;
    for (const auto* channel : {&first.left, &first.right}) {
        for (const float value : *channel) {
            REQUIRE(std::isfinite(value));
            energy += static_cast<double>(value) * value;
        }
    }
    std::cout << "energy=" << energy << " hash_first=" << fingerprint(first)
              << " hash_second=" << fingerprint(second) << '\n';
    REQUIRE(energy > 1.0e-8);
    REQUIRE(first.left == second.left);
    REQUIRE(first.right == second.right);
}

TEST_CASE("external CLAP lifecycle has explicit identity and removal controls",
          "[host][graph][external][lifecycle]") {
    const char* path = std::getenv("PULP_EXTERNAL_CLAP_PATH");
    const char* expected_sha = std::getenv("PULP_EXTERNAL_CLAP_SHA256");
    REQUIRE(path);
    REQUIRE(expected_sha);
    REQUIRE(std::filesystem::exists(path));
    const auto executable = main_executable(path);
    REQUIRE(sha256_file(executable) == expected_sha);

    const auto plugins =
        pulp::host::PluginScanner::scan_bundle(path, pulp::host::PluginFormat::CLAP);
    REQUIRE(plugins.size() == 1);
    const auto info = plugins.front();
    REQUIRE(info.num_inputs == 2);
    REQUIRE(info.num_outputs == 2);
    CHECK_FALSE(info.supports_midi_in);

    auto first_slot = pulp::host::PluginSlot::load(info);
    REQUIRE(first_slot);
    auto parts = make_graph(info, std::move(first_slot));
    const auto baseline = render_one(*parts);
    const auto baseline_hash = fingerprint(baseline);
    REQUIRE(baseline_hash != 0);
    auto expected_replay_hash = baseline_hash;
    REQUIRE(parts->graph.live_plugin_slot(parts->plugin));
    auto* live_slot = parts->graph.live_plugin_slot(parts->plugin);
    const auto original_state = live_slot->save_state();
    REQUIRE_FALSE(original_state.empty());

    const auto parameters = live_slot->parameters();
    std::vector<std::uint8_t> state_to_restore = original_state;
    std::optional<std::pair<std::uint32_t, float>> mutated_parameter;
    const auto writable = std::find_if(
        parameters.begin(), parameters.end(), [](const pulp::host::HostParamInfo& parameter) {
            return parameter.flags.automatable && !parameter.flags.read_only;
        });
    if (writable == parameters.end()) {
        std::cout << "parameter_state=refused reason=no_writable_automatable_parameter\n";
    } else {
        const float before = live_slot->get_parameter(writable->id);
        const float target = std::abs(before - 0.73f) > 0.05f ? 0.73f : 0.17f;
        live_slot->set_parameter(writable->id, target);
        const float after = live_slot->get_parameter(writable->id);
        REQUIRE(std::abs(after - before) > 1.0e-4f);
        const auto mutated = render_one(*parts);
        const auto mutated_hash = fingerprint(mutated);
        std::cout << "parameter_output_changed=" << (mutated_hash != baseline_hash) << '\n';
        const auto mutated_state = live_slot->save_state();
        if (mutated_state == original_state) {
            CHECK(mutated_state == original_state);
            std::cout << "parameter_state=refused reason=state_blob_unchanged id=" << writable->id
                      << " before=" << before << " after=" << after << '\n';
        } else {
            expected_replay_hash = mutated_hash;
            state_to_restore = mutated_state;
            mutated_parameter = std::make_pair(writable->id, after);
            std::cout << "parameter_state=id=" << writable->id << " before=" << before
                      << " after=" << after << " bytes_before=" << original_state.size()
                      << " bytes_after=" << mutated_state.size() << '\n';
        }
    }

    REQUIRE(parts->graph.remove_node(parts->plugin));
    REQUIRE(parts->graph.prepare(48000.0, 128));
    const auto removed = render_one(*parts);
    for (const auto* channel : {&removed.left, &removed.right})
        for (const auto value : *channel)
            REQUIRE(std::isfinite(value));

    auto replacement = pulp::host::PluginSlot::load(info);
    REQUIRE(replacement);
    const bool restored = replacement->restore_state(state_to_restore);
    REQUIRE(restored);
    if (mutated_parameter) {
        REQUIRE(std::abs(replacement->get_parameter(mutated_parameter->first) -
                         mutated_parameter->second) < 1.0e-3f);
    }
    std::cout << "state_restore=" << restored << " bytes=" << state_to_restore.size() << '\n';
    const auto replacement_node =
        parts->graph.add_plugin_node(std::move(replacement), info.num_inputs, info.num_outputs,
                                     "external lifecycle replacement");
    REQUIRE(replacement_node);
    for (int ch = 0; ch < info.num_inputs; ++ch)
        REQUIRE(parts->graph.connect(parts->input, ch, replacement_node, ch));
    for (int ch = 0; ch < info.num_outputs; ++ch)
        REQUIRE(parts->graph.connect(replacement_node, ch, parts->output, ch));
    REQUIRE(parts->graph.prepare(48000.0, 128));
    const auto replay = render_one(*parts);
    for (const auto* channel : {&replay.left, &replay.right})
        for (const auto value : *channel)
            REQUIRE(std::isfinite(value));
    REQUIRE(fingerprint(replay) == expected_replay_hash);
}
