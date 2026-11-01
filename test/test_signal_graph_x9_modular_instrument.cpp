#include "../examples/PulpSynth/pulp_synth.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
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
using pulp::host::PluginFormat;
using pulp::host::PluginInfo;
using pulp::host::SignalGraph;

constexpr double kSampleRate = 48'000.0;
constexpr int kBlockSize = 64;

struct HostedInstrument {
    SignalGraph graph;
    NodeId input = 0;
    NodeId plugin = 0;
    NodeId output = 0;
};

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

Buffer<float> hosted_input(std::size_t frames) {
    Buffer<float> result(2, frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float value = 0.2f + 0.001f * static_cast<float>(frame);
        result.channel(0)[frame] = value;
        result.channel(1)[frame] = -value;
    }
    return result;
}

std::unique_ptr<HostedInstrument> make_hosted_instrument(const PluginInfo& info) {
    auto fixture = std::make_unique<HostedInstrument>();
    auto slot = pulp::host::PluginSlot::load(info);
    REQUIRE(slot);
    fixture->input = fixture->graph.add_input_node(info.num_inputs, "Input");
    fixture->plugin = fixture->graph.add_plugin_node(std::move(slot), info.num_inputs,
                                                     info.num_outputs, "Effect");
    fixture->output = fixture->graph.add_output_node(info.num_outputs, "Output");
    REQUIRE(fixture->input != 0);
    REQUIRE(fixture->plugin != 0);
    REQUIRE(fixture->output != 0);
    for (int channel = 0; channel < info.num_inputs; ++channel)
        REQUIRE(fixture->graph.connect(fixture->input, channel, fixture->plugin, channel));
    for (int channel = 0; channel < info.num_outputs; ++channel)
        REQUIRE(fixture->graph.connect(fixture->plugin, channel, fixture->output, channel));
    REQUIRE(fixture->graph.prepare(kSampleRate, kBlockSize));
    return fixture;
}

Buffer<float> render_hosted(HostedInstrument& fixture, const Buffer<float>& input) {
    Buffer<float> output(2, input.num_samples());
    for (std::size_t offset = 0; offset < input.num_samples(); offset += kBlockSize) {
        const auto frames = std::min<std::size_t>(kBlockSize, input.num_samples() - offset);
        std::array<const float*, 2> input_channels{input.channel(0).data() + offset,
                                                   input.channel(1).data() + offset};
        std::array<float*, 2> output_channels{output.channel(0).data() + offset,
                                              output.channel(1).data() + offset};
        pulp::audio::BufferView<const float> input_view(input_channels.data(), 2,
                                                        static_cast<int>(frames));
        pulp::audio::BufferView<float> output_view(output_channels.data(), 2,
                                                   static_cast<int>(frames));
        fixture.graph.process(output_view, input_view, static_cast<int>(frames));
    }
    return output;
}

void require_finite_and_different(const Buffer<float>& baseline, const Buffer<float>& changed) {
    REQUIRE(baseline.num_channels() == changed.num_channels());
    REQUIRE(baseline.num_samples() == changed.num_samples());
    bool differs = false;
    for (std::size_t channel = 0; channel < changed.num_channels(); ++channel)
        for (std::size_t frame = 0; frame < changed.num_samples(); ++frame) {
            REQUIRE(std::isfinite(changed.channel(channel)[frame]));
            differs |= changed.channel(channel)[frame] != baseline.channel(channel)[frame];
        }
    REQUIRE(differs);
}

void require_equal_buffer(const Buffer<float>& expected, const Buffer<float>& actual) {
    REQUIRE(expected.num_channels() == actual.num_channels());
    REQUIRE(expected.num_samples() == actual.num_samples());
    for (std::size_t channel = 0; channel < expected.num_channels(); ++channel)
        for (std::size_t frame = 0; frame < expected.num_samples(); ++frame)
            REQUIRE(expected.channel(channel)[frame] == actual.channel(channel)[frame]);
}

// X3's smallest useful reusable component is an authoring helper that expands
// into ordinary SignalGraph nodes. Each invocation owns a separate node and
// gain state; the runtime remains the existing graph.
NodeId add_gain_component(SignalGraph& graph, NodeId source, NodeId destination, const char* name,
                          float gain) {
    const auto stage = graph.add_gain_node(name);
    REQUIRE(stage != 0);
    REQUIRE(graph.connect(source, 0, stage, 0));
    REQUIRE(graph.connect(stage, 0, destination, 0));
    REQUIRE(graph.set_node_gain(stage, gain));
    return stage;
}

void run_hosted_format(const char* path, PluginFormat format) {
    REQUIRE(path != nullptr);
    REQUIRE(std::filesystem::exists(path));
    const auto plugins = pulp::host::PluginScanner::scan_bundle(path, format);
    REQUIRE(plugins.size() == 1);
    const auto& info = plugins.front();
    REQUIRE(info.num_inputs == 2);
    REQUIRE(info.num_outputs == 2);

    const auto input = hosted_input(kBlockSize * 2);
    auto fixture = make_hosted_instrument(info);
    const auto handle = fixture->graph.live_snapshot_handle();
    REQUIRE(handle);
    auto* live_slot = fixture->graph.live_plugin_slot(fixture->plugin);
    REQUIRE(live_slot);
    const auto saved_state = live_slot->save_state();
    REQUIRE_FALSE(saved_state.empty());

    const auto baseline = render_hosted(*fixture, input);
    REQUIRE(rms(baseline) > 1.0e-5f);

    const auto parameters = live_slot->parameters();
    REQUIRE_FALSE(parameters.empty());
    const auto parameter = parameters.front();
    const float before = live_slot->get_parameter(parameter.id);
    const float target = before < 0.5f ? 0.75f : 0.25f;
    live_slot->set_parameter(parameter.id, target);
    REQUIRE(live_slot->get_parameter(parameter.id) == target);
    const auto changed = render_hosted(*fixture, input);
    require_finite_and_different(baseline, changed);

    REQUIRE(live_slot->restore_state(saved_state));
    const auto restored = render_hosted(*fixture, input);
    require_equal_buffer(baseline, restored);

    auto offline_fixture = make_hosted_instrument(info);
    pulp::host::OfflineSignalGraphHost offline(offline_fixture->graph);
    pulp::host::OfflineSignalGraphConfig config;
    config.sample_rate = kSampleRate;
    config.block_frames = kBlockSize;
    config.input_channels = 2;
    config.output_channels = 2;
    REQUIRE(offline.prepare(config));
    pulp::host::OfflineSignalGraphOptions options;
    options.frame_count = input.num_samples();
    options.input = std::as_const(input).view();
    const auto offline_result = offline.render(options);
    REQUIRE(offline_result.ok);
    require_equal_buffer(baseline, offline_result.audio);
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

Buffer<float> render_live_note(ModularInstrument& fixture, float modulation) {
    REQUIRE(fixture.graph.prepare(kSampleRate, kBlockSize));
    auto input = constant_input(kBlockSize, modulation);
    Buffer<float> output(2, kBlockSize);
    REQUIRE(fixture.graph.inject_midi(fixture.midi, note_on()));
    auto output_view = output.view();
    fixture.graph.process(output_view, std::as_const(input).view(), kBlockSize);
    return output;
}

Buffer<float> render_offline_note(ModularInstrument& fixture, float modulation) {
    REQUIRE(fixture.graph.prepare(kSampleRate, kBlockSize));
    pulp::host::OfflineSignalGraphHost offline(fixture.graph);
    pulp::host::OfflineSignalGraphConfig config;
    config.sample_rate = kSampleRate;
    config.block_frames = kBlockSize;
    config.input_channels = 1;
    config.output_channels = 2;
    REQUIRE(offline.prepare(config));
    auto input = constant_input(kBlockSize, modulation);
    REQUIRE(fixture.graph.inject_midi(fixture.midi, note_on()));
    pulp::host::OfflineSignalGraphOptions options;
    options.frame_count = kBlockSize;
    options.input = std::as_const(input).view();
    const auto result = offline.render(options);
    REQUIRE(result.ok);
    REQUIRE(result.blocks_rendered == 1);
    return result.audio;
}

} // namespace

TEST_CASE("X9 live and offline rendering agree for the same event and input",
          "[host][graph][x9][offline][deterministic]") {
    ModularInstrument live;
    ModularInstrument offline;
    configure_modular_instrument(live);
    configure_modular_instrument(offline);

    const auto live_output = render_live_note(live, 0.5f);
    const auto offline_output = render_offline_note(offline, 0.5f);
    REQUIRE(rms(live_output) > 1.0e-5f);
    REQUIRE(rms(offline_output) > 1.0e-5f);
    require_equal_buffer(live_output, offline_output);
}

TEST_CASE("X9 chains named serial stages through a live graph",
          "[host][graph][x9][serial][mutation]") {
    SignalGraph graph;
    const auto input = graph.add_input_node(2, "Voice Bus");
    const auto aux = graph.add_gain_node("Aux Bus");
    const auto master = graph.add_gain_node("Master Bus");
    const auto output = graph.add_output_node(2, "Master Output");
    REQUIRE(input != 0);
    REQUIRE(aux != 0);
    REQUIRE(master != 0);
    REQUIRE(output != 0);
    for (int channel = 0; channel < 2; ++channel) {
        REQUIRE(graph.connect(input, channel, aux, channel));
        REQUIRE(graph.connect(aux, channel, master, channel));
        REQUIRE(graph.connect(master, channel, output, channel));
    }
    REQUIRE(graph.set_node_gain(aux, 0.5f));
    REQUIRE(graph.set_node_gain(master, 0.5f));
    REQUIRE(graph.prepare(kSampleRate, kBlockSize));

    Buffer<float> input_buffer(2, kBlockSize);
    std::fill(input_buffer.channel(0).begin(), input_buffer.channel(0).end(), 1.0f);
    std::fill(input_buffer.channel(1).begin(), input_buffer.channel(1).end(), -1.0f);
    Buffer<float> baseline(2, kBlockSize);
    auto baseline_view = baseline.view();
    const auto input_view = std::as_const(input_buffer).view();
    graph.process(baseline_view, input_view, kBlockSize);
    for (std::size_t frame = 0; frame < kBlockSize; ++frame) {
        REQUIRE(baseline.channel(0)[frame] == 0.25f);
        REQUIRE(baseline.channel(1)[frame] == -0.25f);
    }

    const auto encoded = pulp::host::GraphSerializer::to_json(graph);
    REQUIRE(encoded.find("Aux Bus") != std::string::npos);
    REQUIRE(encoded.find("Master Bus") != std::string::npos);

    // Gain edits are live control changes on the named serial stages and do not
    // require replacing the graph or introducing another routing model.
    REQUIRE(graph.set_node_gain(aux, 0.75f));
    Buffer<float> changed(2, kBlockSize);
    auto changed_view = changed.view();
    graph.process(changed_view, input_view, kBlockSize);
    for (std::size_t frame = 0; frame < kBlockSize; ++frame) {
        REQUIRE(changed.channel(0)[frame] == 0.375f);
        REQUIRE(changed.channel(1)[frame] == -0.375f);
    }
}

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

    // A malformed candidate must be rejected without disturbing the committed
    // graph. This is the failure control for runtime graph mutation.
    const auto active_snapshot_before_rejection = fixture.graph.live_snapshot_handle();
    auto rejected = fixture.graph.begin_prepared_topology_edit();
    REQUIRE(rejected);
    REQUIRE_FALSE(rejected->connect(fixture.synth_node, 0, 999'999, 0));
    REQUIRE(rejected->prepare(kSampleRate, kBlockSize) ==
            SignalGraph::PreparedTopologyEdit::Result::InvalidMutation);
    REQUIRE(rejected->commit() == SignalGraph::PreparedTopologyEdit::Result::NotPrepared);
    REQUIRE(fixture.graph.live_snapshot_handle() == active_snapshot_before_rejection);

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

TEST_CASE("X3 reusable graph components expand with independent state",
          "[host][graph][x3][serializer][composition]") {
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "Input");
    const auto output = graph.add_output_node(1, "Output");
    REQUIRE(input != 0);
    REQUIRE(output != 0);

    // The same parameterized component is instantiated twice. Both stages
    // consume the same source and fan into the output, proving independent
    // authored identities without introducing nested runtime execution.
    const auto dry = add_gain_component(graph, input, output, "Dry", 0.25f);
    const auto wet = add_gain_component(graph, input, output, "Wet", 0.5f);
    REQUIRE(dry != wet);
    REQUIRE(graph.prepare(kSampleRate, kBlockSize));

    Buffer<float> input_buffer(1, kBlockSize);
    std::fill(input_buffer.channel(0).begin(), input_buffer.channel(0).end(), 1.0f);
    Buffer<float> output_buffer(1, kBlockSize);
    auto output_view = output_buffer.view();
    const auto input_view = std::as_const(input_buffer).view();
    graph.process(output_view, input_view, kBlockSize);
    for (const auto sample : output_buffer.channel(0))
        REQUIRE(sample == 0.75f);

    const auto encoded = pulp::host::GraphSerializer::to_json(graph);
    REQUIRE(encoded.find("Dry") != std::string::npos);
    REQUIRE(encoded.find("Wet") != std::string::npos);
    REQUIRE(encoded.find("0.25") != std::string::npos);
    REQUIRE(encoded.find("0.5") != std::string::npos);

    SignalGraph restored;
    const auto loaded = pulp::host::GraphSerializer::from_json(restored, encoded);
    REQUIRE(loaded.ok);
    REQUIRE(restored.nodes().size() == graph.nodes().size());
    REQUIRE(restored.connections().size() == graph.connections().size());
    const auto restored_dry_it = std::find_if(restored.nodes().begin(), restored.nodes().end(),
                                              [](const auto& node) { return node.name == "Dry"; });
    REQUIRE(restored_dry_it != restored.nodes().end());
    REQUIRE(restored_dry_it->gain == 0.25f);
    const auto restored_dry = restored_dry_it->id;
    REQUIRE(restored.prepare(kSampleRate, kBlockSize));
    Buffer<float> restored_output(1, kBlockSize);
    auto restored_output_view = restored_output.view();
    restored.process(restored_output_view, input_view, kBlockSize);
    require_equal_buffer(output_buffer, restored_output);

    REQUIRE(restored.set_node_gain(restored_dry, 0.0f));
    Buffer<float> independently_changed(1, kBlockSize);
    auto independently_changed_view = independently_changed.view();
    restored.process(independently_changed_view, input_view, kBlockSize);
    for (const auto sample : independently_changed.channel(0))
        REQUIRE(sample == 0.5f);
}

#ifdef PULP_X9_CLAP_PATH
TEST_CASE("X9 hosted graph lifecycle covers the CLAP format",
          "[host][graph][x9][plugin][clap][offline]") {
    run_hosted_format(PULP_X9_CLAP_PATH, PluginFormat::CLAP);
}
#endif

#ifdef PULP_X9_VST3_PATH
TEST_CASE("X9 hosted graph lifecycle covers the VST3 format",
          "[host][graph][x9][plugin][vst3][offline]") {
    run_hosted_format(PULP_X9_VST3_PATH, PluginFormat::VST3);
}
#endif

#ifdef PULP_X9_AU_PATH
TEST_CASE("X9 hosted graph lifecycle covers the Audio Unit format",
          "[host][graph][x9][plugin][au][offline]") {
    REQUIRE(std::filesystem::exists(PULP_X9_AU_PATH));
    const auto plugins =
        pulp::host::PluginScanner::scan_bundle(PULP_X9_AU_PATH, PluginFormat::AudioUnit);
    if (plugins.empty())
        SKIP("Audio Unit bundle discovery is unavailable through scan_bundle on this host");
    REQUIRE(plugins.size() == 1);
    run_hosted_format(PULP_X9_AU_PATH, PluginFormat::AudioUnit);
}
#endif
