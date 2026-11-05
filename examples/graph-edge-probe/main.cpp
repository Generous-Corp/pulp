#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <pulp/audio/audio_file.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <stdexcept>
#include <vector>

using namespace pulp::host;
using Status = SignalGraph::EdgeProbeStatus;
static int checks = 0;
static void require(bool ok, const char* rule) {
    ++checks;
    if (!ok)
        throw std::runtime_error(rule);
}

int main(int argc, char** argv) try {
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "input");
    const auto source = graph.add_gain_node("source");
    const auto downstream = graph.add_gain_node("downstream");
    const auto output = graph.add_output_node(1, "output");
    const auto midi_in = graph.add_midi_input_node("notes");
    const auto midi_out = graph.add_midi_output_node("notes-out");
    const auto from = graph.node(source)->out("audio");
    const auto to = graph.node(downstream)->in("audio");
    require(graph.connect(input, 0, source, 0) && graph.connect(source, 0, downstream, 0) &&
                graph.connect(downstream, 0, output, 0) && graph.connect_midi(midi_in, midi_out),
            "connections");
    require(graph.set_node_gain(source, 0.5f) && graph.set_node_gain(downstream, 0.25f), "gains");
    require(graph.attach_audio_edge_probe(from, to) == Status::Unprepared, "unprepared-refusal");
    pulp::audio::AudioFileData input_file;
    input_file.sample_rate = 48000;
    input_file.channels = {std::vector<float>(133)};
    for (std::size_t i = 0; i < 133; ++i)
        input_file.channels[0][i] = i % 2 ? -1.f : 1.f;
    float* pointers[] = {input_file.channels[0].data()};
    const pulp::audio::BufferView<const float> samples(pointers, 1, 133);
    if (argc == 2) {
        std::filesystem::create_directories(argv[1]);
        std::ofstream(std::filesystem::path(argv[1]) / "graph.pulpgraph")
            << GraphSerializer::to_json(graph);
        require(pulp::audio::write_wav_file((std::filesystem::path(argv[1]) / "input.wav").string(),
                                            input_file, pulp::audio::WavBitDepth::Float32),
                "write-input");
    }
    OfflineSignalGraphHost host(graph);
    require(
        host.prepare(
            {.sample_rate = 48000, .block_frames = 64, .input_channels = 1, .output_channels = 1}),
        "prepare");
    const auto baseline = host.render({.frame_count = 133, .input = samples});
    require(baseline.ok, "baseline-render");
    pulp::graph::GraphRuntimeProbeSample record;
    require(!graph.read_audio_edge_probe(record), "unattached-empty");
    require(graph.attach_audio_edge_probe(from, to) == Status::Attached, "attach");
    const auto identity = graph.audio_edge_probe_identity();
    require(identity.authoring.valid() && identity.connection && identity.source == source &&
                identity.destination == downstream && identity.source_port == 0 &&
                identity.destination_port == 0,
            "identity");
    require(graph.attach_audio_edge_probe(graph.node(source)->out("absent"), to) ==
                Status::UnknownPort,
            "unknown-port");
    require(graph.audio_edge_probe_identity().connection == identity.connection,
            "refusal-preserves-attachment");
    require(graph.attach_audio_edge_probe(graph.node(midi_in)->out("audio"),
                                          graph.node(midi_out)->in("audio")) ==
                Status::UnsupportedKind,
            "midi-refusal");
    require(graph.attach_audio_edge_probe(from, graph.node(output)->in("audio")) ==
                Status::MissingEdge,
            "missing-edge");
    const auto observed = host.render({.frame_count = 133, .input = samples});
    require(observed.ok, "observed-render");
    for (std::size_t i = 0; i < 133; ++i) {
        require(observed.audio.channel(0)[i] == baseline.audio.channel(0)[i], "unchanged-audio");
        require(observed.audio.channel(0)[i] == input_file.channels[0][i] * 0.125f,
                "downstream-oracle");
    }
    std::ofstream expected;
    if (argc == 2)
        expected.open(std::filesystem::path(argv[1]) / "expected.tsv");
    for (std::uint64_t i = 0; i < 3; ++i) {
        require(graph.read_audio_edge_probe(record), "publication");
        require(record.sequence == i && record.timestamp_samples == i * 64 &&
                    record.frames == (i == 2 ? 5 : 64),
                "sample-clock");
        require(record.edge_index == 1, "selected-edge");
        require(record.peak == 0.5f && record.rms == 0.5f, "source-before-downstream");
        if (expected)
            expected << record.sequence << ' ' << record.timestamp_samples << ' ' << record.frames
                     << ' ' << record.peak << ' ' << record.rms << '\n';
    }
    require(!graph.read_audio_edge_probe(record) && graph.audio_edge_probe_dropped() == 0,
            "drained");
    require(graph.attach_audio_edge_probe(from, to) == Status::Attached, "replace");
    require(host.render({.frame_count = 7, .input = samples}).ok, "short-block");
    require(host.render({.frame_count = 19, .input = samples}).ok, "variable-block");
    require(graph.read_audio_edge_probe(record) && record.timestamp_samples == 0 &&
                record.frames == 7,
            "replace-clock");
    require(graph.read_audio_edge_probe(record) && record.timestamp_samples == 7 &&
                record.frames == 19,
            "variable-clock");
    require(graph.attach_audio_edge_probe(from, to) == Status::Attached, "overflow-attach");
    require(host.render({.frame_count = 258 * 64, .input = samples}).ok, "overflow-render");
    require(graph.audio_edge_probe_dropped() == 2, "overflow-loss");
    for (std::uint64_t i = 0; i < 256; ++i)
        require(graph.read_audio_edge_probe(record) && record.sequence == i &&
                    record.timestamp_samples == i * 64,
                "overflow-order");
    require(!graph.read_audio_edge_probe(record), "overflow-capacity");
    require(host.render({.frame_count = 1, .input = samples}).ok, "post-loss-render");
    require(graph.read_audio_edge_probe(record) && record.sequence == 256 &&
                record.timestamp_samples == 258 * 64,
            "loss-clock");
    graph.detach_audio_edge_probe();
    require(graph.audio_edge_probe_status() == Status::Detached &&
                !graph.read_audio_edge_probe(record),
            "detach");
    require(graph.attach_audio_edge_probe(from, to) == Status::Attached, "topology-attach");
    require(graph.disconnect(source, 0, downstream, 0), "disconnect");
    require(graph.audio_edge_probe_status() == Status::Unprepared &&
                !graph.read_audio_edge_probe(record),
            "retired");
    require(graph.connect(source, 0, downstream, 0) && host.prepare({.sample_rate = 48000,
                                                                     .block_frames = 64,
                                                                     .input_channels = 1,
                                                                     .output_channels = 1}),
            "reconnect");
    require(graph.attach_audio_edge_probe(from, to) == Status::Attached &&
                graph.audio_edge_probe_identity().connection != identity.connection,
            "new-connection-identity");
    graph.set_parallel_routing_enabled(true);
    require(graph.attach_audio_edge_probe(from, to) == Status::UnsupportedExecution,
            "execution-refusal");
    std::cout << "graph-edge-probe PASS checks=" << checks << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << "graph-edge-probe FAIL rule=" << error.what() << '\n';
    return 1;
}
