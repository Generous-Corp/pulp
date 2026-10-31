#include "../examples/PulpSynth/pulp_synth.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace {

using pulp::audio::Buffer;
using pulp::examples::create_pulp_synth;
using pulp::examples::kFilterCutoff;
using pulp::examples::PulpSynthProcessor;
using pulp::format::ProcessorNodeInstance;
using pulp::host::NodeId;
using pulp::host::SignalGraph;

constexpr double kSampleRate = 48'000.0;
constexpr int kBlockSize = 64;

struct ModularInstrument {
    SignalGraph graph;
    std::shared_ptr<ProcessorNodeInstance> synth;
    NodeId input = 0;
    NodeId midi = 0;
    NodeId synth_node = 0;
    NodeId output = 0;
};

// One authoring helper is intentionally reused for the live and offline paths.
// It is the fixture's smallest reusable graph component: no parallel runtime or
// second graph model is introduced.
void configure_modular_instrument(ModularInstrument& fixture) {
    fixture.synth = ProcessorNodeInstance::create(create_pulp_synth());
    REQUIRE(fixture.synth);
    fixture.input = fixture.graph.add_input_node(1, "Modulation");
    fixture.midi = fixture.graph.add_midi_input_node("MIDI");
    fixture.synth_node = fixture.graph.add_processor_node(fixture.synth, "Voice");
    fixture.output = fixture.graph.add_output_node(2, "Output");
    REQUIRE(fixture.input != 0);
    REQUIRE(fixture.midi != 0);
    REQUIRE(fixture.synth_node != 0);
    REQUIRE(fixture.output != 0);
    REQUIRE(fixture.graph.connect(fixture.synth_node, 0, fixture.output, 0));
    REQUIRE(fixture.graph.connect(fixture.synth_node, 1, fixture.output, 1));
    REQUIRE(fixture.graph.connect_midi(fixture.midi, fixture.synth_node));
    // A control-rate graph modulation lane drives a real instrument parameter.
    REQUIRE(fixture.graph.connect_automation(fixture.input, 0, fixture.synth_node, kFilterCutoff,
                                             800.0f, 12'000.0f));
}

Buffer<float> constant_input(std::size_t frames, float value) {
    Buffer<float> result(1, frames);
    std::fill(result.channel(0).begin(), result.channel(0).end(), value);
    return result;
}

float rms(const Buffer<float>& buffer) {
    double sum = 0.0;
    for (std::size_t channel = 0; channel < buffer.num_channels(); ++channel)
        for (std::size_t frame = 0; frame < buffer.num_samples(); ++frame) {
            const auto sample = buffer.channel(channel)[frame];
            REQUIRE(std::isfinite(sample));
            sum += static_cast<double>(sample) * sample;
        }
    return static_cast<float>(
        std::sqrt(sum / static_cast<double>(buffer.num_channels() * buffer.num_samples())));
}

pulp::midi::MidiBuffer chord() {
    pulp::midi::MidiBuffer events;
    for (const auto note : {uint8_t{60}, uint8_t{64}, uint8_t{67}}) {
        auto event = pulp::midi::MidiEvent::note_on(0, note, 110);
        event.sample_offset = 0;
        events.add(event);
    }
    return events;
}

pulp::midi::MidiBuffer note_on() {
    pulp::midi::MidiBuffer events;
    auto event = pulp::midi::MidiEvent::note_on(0, 60, 110);
    event.sample_offset = 0;
    events.add(event);
    return events;
}

} // namespace

TEST_CASE("X9 public APIs compose a modular instrument through a live graph",
          "[host][graph][x9][midi][modulation][state][offline]") {
    ModularInstrument fixture;
    configure_modular_instrument(fixture);
    REQUIRE(fixture.graph.prepare(kSampleRate, kBlockSize));

    auto input = constant_input(kBlockSize, 0.5f);
    Buffer<float> first_output(2, kBlockSize);
    auto events = chord();
    REQUIRE(events.size() == 3);
    REQUIRE(fixture.graph.inject_midi(fixture.midi, events));
    auto first_output_view = first_output.view();
    auto input_view = std::as_const(input).view();
    fixture.graph.process(first_output_view, input_view, kBlockSize);
    REQUIRE(rms(first_output) > 1.0e-4f);

    // Preset/state round-trip uses the shared Processor state envelope while
    // the Processor remains owned by the graph's ProcessorNode.
    auto& store = fixture.synth->state_store();
    store.set_value(kFilterCutoff, 2'000.0f);
    const auto preset = pulp::format::plugin_state_io::serialize(store, fixture.synth->processor());
    REQUIRE_FALSE(preset.empty());
    store.set_value(kFilterCutoff, 10'000.0f);
    REQUIRE(pulp::format::plugin_state_io::deserialize(preset, store, fixture.synth->processor()));
    REQUIRE(store.get_value(kFilterCutoff) == 2'000.0f);

    // Reconfigure the prepared graph atomically by inserting two reusable
    // built-in effect stages, one per output channel.
    auto edit = fixture.graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    const auto left_gain = edit->add_gain_node("Effect L");
    const auto right_gain = edit->add_gain_node("Effect R");
    REQUIRE(left_gain != 0);
    REQUIRE(right_gain != 0);
    REQUIRE(edit->disconnect(fixture.synth_node, 0, fixture.output, 0));
    REQUIRE(edit->disconnect(fixture.synth_node, 1, fixture.output, 1));
    REQUIRE(edit->connect(fixture.synth_node, 0, left_gain, 0));
    REQUIRE(edit->connect(left_gain, 0, fixture.output, 0));
    REQUIRE(edit->connect(fixture.synth_node, 1, right_gain, 0));
    REQUIRE(edit->connect(right_gain, 0, fixture.output, 1));
    REQUIRE(edit->set_node_gain(left_gain, 0.5f));
    REQUIRE(edit->set_node_gain(right_gain, 0.5f));
    REQUIRE(edit->prepare(kSampleRate, kBlockSize) ==
            SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);

    Buffer<float> edited_output(2, kBlockSize);
    auto edited_output_view = edited_output.view();
    fixture.graph.process(edited_output_view, input_view, kBlockSize);
    REQUIRE(rms(edited_output) > 1.0e-5f);

    // OfflineSignalGraphHost exercises the same authored graph and receives a
    // MIDI publication before its first deterministic render block.
    pulp::host::OfflineSignalGraphHost offline(fixture.graph);
    pulp::host::OfflineSignalGraphConfig config;
    config.sample_rate = kSampleRate;
    config.block_frames = kBlockSize;
    config.input_channels = 1;
    config.output_channels = 2;
    REQUIRE(offline.prepare(config));
    auto offline_events = note_on();
    REQUIRE(fixture.graph.inject_midi(fixture.midi, offline_events));
    auto offline_input = constant_input(kBlockSize * 2, 0.5f);
    pulp::host::OfflineSignalGraphOptions options;
    options.frame_count = kBlockSize * 2;
    options.input = std::as_const(offline_input).view();
    const auto rendered = offline.render(options);
    REQUIRE(rendered.ok);
    REQUIRE(rendered.blocks_rendered == 2);
    REQUIRE(rendered.audio.num_channels() == 2);
    REQUIRE(rendered.audio.num_samples() == kBlockSize * 2);
    REQUIRE(rms(rendered.audio) > 1.0e-5f);
}

TEST_CASE("X9 records the runtime ProcessorNode persistence boundary",
          "[host][graph][x9][serializer][capability]") {
    ModularInstrument fixture;
    configure_modular_instrument(fixture);
    const auto encoded = pulp::host::GraphSerializer::to_json(fixture.graph);
    REQUIRE(encoded.empty());

    constexpr auto serialized_processor_node = R"json({
      "format_version": 2,
      "nodes": [{
        "id": 1,
        "type": "processor",
        "name": "runtime-only",
        "num_input_ports": 0,
        "num_output_ports": 2
      }],
      "connections": []
    })json";
    SignalGraph loaded;
    const auto result = pulp::host::GraphSerializer::from_json(loaded, serialized_processor_node);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("unsupported by .pulpgraph") != std::string::npos);
    REQUIRE(loaded.nodes().empty());
}
