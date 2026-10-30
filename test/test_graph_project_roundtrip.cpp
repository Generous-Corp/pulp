#include <catch2/catch_test_macros.hpp>

#include "preset_test_sandbox.hpp"
#include <pulp/audio/buffer.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/preset_manager.hpp>
#include <pulp/state/store.hpp>

#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using pulp::host::CustomNodeType;
using pulp::host::GraphSerializer;
using pulp::host::OfflineSignalGraphHost;
using pulp::host::SignalGraph;

constexpr char kNodeType[] = "pulp.test.project-state-gain";
constexpr pulp::state::ParamID kGainParam = 1;

struct GainState {
    float gain = 0.5f;
};

CustomNodeType make_gain_type(float initial_gain) {
    CustomNodeType type;
    type.type_id = kNodeType;
    type.version = 1;
    type.num_input_ports = 1;
    type.num_output_ports = 1;
    type.default_name = "Project Gain";
    type.create = [initial_gain] {
        return static_cast<void*>(new GainState{initial_gain});
    };
    type.destroy = [](void* p) { delete static_cast<GainState*>(p); };
    type.prepare = [](void*, double, int) {};
    type.release = [](void*) {};
    type.reset = [](void* p) { static_cast<GainState*>(p)->gain = 0.5f; };
    type.process_instance = [](void* p, pulp::audio::BufferView<float>& out,
                               const pulp::audio::BufferView<const float>& in,
                               int samples) {
        const float gain = static_cast<GainState*>(p)->gain;
        auto output = out.channel(0);
        const auto input = in.channel(0);
        for (int i = 0; i < samples; ++i) output[static_cast<std::size_t>(i)] =
            input[static_cast<std::size_t>(i)] * gain;
    };
    type.save_state = [](void* p) {
        const auto bits = std::bit_cast<std::uint32_t>(static_cast<GainState*>(p)->gain);
        return std::vector<std::uint8_t>{static_cast<std::uint8_t>(bits),
                                         static_cast<std::uint8_t>(bits >> 8),
                                         static_cast<std::uint8_t>(bits >> 16),
                                         static_cast<std::uint8_t>(bits >> 24)};
    };
    type.load_state = [](void* p, const std::vector<std::uint8_t>& bytes) {
        if (bytes.size() != 4) return false;
        const auto bits = static_cast<std::uint32_t>(bytes[0]) |
                          (static_cast<std::uint32_t>(bytes[1]) << 8) |
                          (static_cast<std::uint32_t>(bytes[2]) << 16) |
                          (static_cast<std::uint32_t>(bytes[3]) << 24);
        static_cast<GainState*>(p)->gain = std::bit_cast<float>(bits);
        return true;
    };
    return type;
}

struct Project {
    SignalGraph graph;
    pulp::state::StateStore state;
    pulp::host::NodeId input = 0;
    pulp::host::NodeId gain = 0;
    pulp::host::NodeId output = 0;
    pulp::host::NodeId midi_in = 0;
    pulp::host::NodeId midi_out = 0;
};

std::unique_ptr<Project> make_project(float parameter_value) {
    auto project = std::make_unique<Project>();
    project->state.add_parameter({.id = kGainParam, .name = "Gain", .range = {0.0, 2.0, 0.5}});
    project->state.set_value(kGainParam, parameter_value);
    REQUIRE(project->graph.register_custom_node_type(make_gain_type(parameter_value)));
    project->input = project->graph.add_input_node(1, "Input");
    project->gain = project->graph.add_custom_node(kNodeType, "Gain");
    project->output = project->graph.add_output_node(1, "Output");
    project->midi_in = project->graph.add_midi_input_node("MIDI In");
    project->midi_out = project->graph.add_midi_output_node("MIDI Out");
    REQUIRE(project->graph.connect(project->input, 0, project->gain, 0));
    REQUIRE(project->graph.connect(project->gain, 0, project->output, 0));
    REQUIRE(project->graph.connect_midi(project->midi_in, project->midi_out));
    return project;
}

std::uint64_t hash_bytes(std::uint64_t h, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) h = (h ^ bytes[i]) * 1099511628211ull;
    return h;
}

std::uint64_t audio_hash(const pulp::audio::Buffer<float>& audio) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t c = 0; c < audio.num_channels(); ++c)
        h = hash_bytes(h, audio.channel(c).data(), audio.channel(c).size() * sizeof(float));
    return h;
}

std::uint64_t midi_hash(const pulp::midi::MidiBuffer& midi) {
    std::uint64_t h = 1469598103934665603ull;
    for (const auto& event : midi) {
        h = hash_bytes(h, &event.sample_offset, sizeof(event.sample_offset));
        h = hash_bytes(h, event.data(), event.size());
    }
    return h;
}

TEST_CASE("public graph, state, preset, MIDI, and offline render round-trip together",
          "[host][state][project][cc8]") {
    pulp::test::PresetTestSandbox sandbox("pulp-project-roundtrip");
    auto baseline = make_project(0.5f);
    auto source = make_project(1.25f);
    pulp::audio::Buffer<float> baseline_input(1, 32);
    for (int i = 0; i < 32; ++i) baseline_input.channel(0)[i] = static_cast<float>(i + 1);
    const auto baseline_input_view =
        static_cast<const pulp::audio::Buffer<float>&>(baseline_input).view();
    OfflineSignalGraphHost baseline_host(baseline->graph);
    REQUIRE(baseline_host.prepare({.sample_rate = 48000.0, .block_frames = 32,
                                   .input_channels = 1, .output_channels = 1}));
    const auto baseline_render =
        baseline_host.render({.frame_count = 32, .input = baseline_input_view});
    REQUIRE(baseline_render.ok);
    REQUIRE(source->graph.prepare(48000.0, 32));

    pulp::midi::MidiBuffer events;
    auto note = pulp::midi::MidiEvent::note_on(0, 60, 100);
    note.sample_offset = 7;
    events.add(note);
    REQUIRE(source->graph.inject_midi(source->midi_in, events));

    pulp::audio::Buffer<float> input(1, 32);
    for (int i = 0; i < 32; ++i) input.channel(0)[i] = static_cast<float>(i + 1);
    const auto input_view = static_cast<const pulp::audio::Buffer<float>&>(input).view();
    OfflineSignalGraphHost source_host(source->graph);
    REQUIRE(source_host.prepare({.sample_rate = 48000.0, .block_frames = 32,
                                 .input_channels = 1, .output_channels = 1}));
    const auto source_render = source_host.render({.frame_count = 32, .input = input_view});
    REQUIRE(source_render.ok);
    OfflineSignalGraphHost source_fine_host(source->graph);
    REQUIRE(source_fine_host.prepare({.sample_rate = 48000.0, .block_frames = 16,
                                      .input_channels = 1, .output_channels = 1}));
    const auto source_fine_render = source_fine_host.render({.frame_count = 32, .input = input_view});
    REQUIRE(source_fine_render.ok);
    REQUIRE(audio_hash(source_render.audio) != audio_hash(baseline_render.audio));
    pulp::midi::MidiBuffer source_events;
    source_events.reserve_events(4);
    REQUIRE(source->graph.extract_midi(source->midi_out, source_events));

    const auto graph_json = GraphSerializer::to_json(source->graph);
    REQUIRE_FALSE(graph_json.empty());
    const auto state_blob = source->state.serialize();
    pulp::state::PresetManager presets(source->state, "PulpTest", "ProjectRoundTrip");
    REQUIRE(presets.save("RoundTrip"));
    const auto preset = presets.user_presets();
    REQUIRE(preset.size() == 1);

    auto restored = make_project(0.0f);
    REQUIRE(restored->state.deserialize(state_blob));
    REQUIRE(restored->state.get_value(kGainParam) == 1.25f);
    pulp::state::PresetManager restored_presets(restored->state, "PulpTest", "ProjectRoundTrip");
    REQUIRE(restored_presets.load(preset.front().path));
    REQUIRE(restored->state.get_value(kGainParam) == 1.25f);
    const auto loaded = GraphSerializer::from_json(restored->graph, graph_json);
    REQUIRE(loaded.ok);
    REQUIRE(loaded.missing_custom_node_types.empty());
    REQUIRE(restored->graph.prepare(48000.0, 32));
    REQUIRE(restored->graph.inject_midi(restored->midi_in, events));
    OfflineSignalGraphHost restored_host(restored->graph);
    REQUIRE(restored_host.prepare({.sample_rate = 48000.0, .block_frames = 32,
                                   .input_channels = 1, .output_channels = 1}));
    const auto restored_render = restored_host.render({.frame_count = 32, .input = input_view});
    REQUIRE(restored_render.ok);
    OfflineSignalGraphHost restored_fine_host(restored->graph);
    REQUIRE(restored_fine_host.prepare({.sample_rate = 48000.0, .block_frames = 16,
                                        .input_channels = 1, .output_channels = 1}));
    const auto restored_fine_render =
        restored_fine_host.render({.frame_count = 32, .input = input_view});
    REQUIRE(restored_fine_render.ok);
    pulp::midi::MidiBuffer restored_events;
    restored_events.reserve_events(4);
    REQUIRE(restored->graph.extract_midi(restored->midi_out, restored_events));

    REQUIRE(audio_hash(source_render.audio) == audio_hash(restored_render.audio));
    REQUIRE(audio_hash(source_fine_render.audio) == audio_hash(restored_fine_render.audio));
    REQUIRE(midi_hash(source_events) == midi_hash(restored_events));
}

TEST_CASE("project graph restore reports deterministic missing and corrupt state",
          "[host][state][project][cc8][negative]") {
    auto source = make_project(0.75f);
    const auto json = GraphSerializer::to_json(source->graph);

    SignalGraph missing;
    const auto missing_result = GraphSerializer::from_json(missing, json);
    REQUIRE(missing_result.ok);
    REQUIRE(missing_result.missing_custom_node_types.size() == 1);
    REQUIRE(missing_result.missing_custom_node_types.front().find(kNodeType) != std::string::npos);

    SignalGraph corrupt;
    auto truncated = json;
    REQUIRE_FALSE(truncated.empty());
    truncated.pop_back();
    const auto corrupt_result = GraphSerializer::from_json(corrupt, truncated);
    REQUIRE_FALSE(corrupt_result.ok);
    REQUIRE_FALSE(corrupt_result.error.empty());
}

} // namespace
