// `pulp graph probe`: render a bounded built-in graph offline with one edge
// probe attached and print every probe sample as JSON.

#include "cmd_graph_internal.hpp"

#include <algorithm>
#include <charconv>
#include <choc/text/choc_JSON.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <pulp/audio/audio_file.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <string>
#include <utility>
#include <vector>

namespace pulp_cli::graph_detail {
namespace {

using Graph = pulp::host::SignalGraph;

// Every probe refusal prints the JSON error envelope and exits 2.
int fail(const std::string& code, const std::string& message) {
    json_error(code, message);
    return 2;
}

// Each attach refusal names its cause and the change that clears it, so a
// caller can act on the message without reading the probe implementation.
std::pair<std::string, std::string> probe_refusal(Graph::EdgeProbeStatus status,
                                                  const std::string& edge_kind) {
    using Status = Graph::EdgeProbeStatus;
    switch (status) {
    case Status::Unprepared:
        return {"Unprepared", "the graph is not prepared, so no edge can be probed; check that "
                              "`pulp graph validate` accepts the file"};
    case Status::UnknownPort:
        if (edge_kind == "control")
            return {"UnknownPort",
                    "no such port: --source-port must name an output port of --source-node; a "
                    "control edge is selected by --destination-node and --destination-param, "
                    "not by a destination port"};
        return {"UnknownPort",
                "no such port: --source-port must name an output port of --source-node and "
                "--destination-port an input port of --destination-node"};
    case Status::MissingEdge:
        if (edge_kind == "control")
            return {"MissingEdge",
                    "no automation edge reaches --destination-param on --destination-node; graphs "
                    "loaded by the CLI cannot contain automation edges, so control probes are "
                    "available only through the C++ API (SignalGraph::attach_control_edge_probe)"};
        return {"MissingEdge", "no connection joins the named source and destination ports; pick "
                               "a pair from the graph file's connections array"};
    case Status::UnsupportedKind:
        return {"UnsupportedKind",
                "the selected ports do not carry a " + edge_kind +
                    " edge; use --edge-kind audio for audio connections, event for MIDI "
                    "connections and control for automation connections"};
    case Status::UnsupportedExecution:
        return {"UnsupportedExecution",
                "edge probes attach only to the routed serial or parallel executor; enable "
                "canonical routing, disable anticipation, then prepare the graph again"};
    case Status::Busy:
        return {"Busy", "a live swap edit is open, or eight displaced probes are still parked "
                        "because a snapshot holder or running process() pins them; commit or "
                        "cancel the edit, or release the snapshot holders, then retry"};
    case Status::Attached:
    case Status::Detached:
        break;
    }
    return {"Detached", "the probe did not attach"};
}

struct ProbeOptions {
    std::map<std::string, std::string> values;
    bool include_plan = false;
    bool include_latency = false;
    bool include_node_loads = false;
    std::string edge_kind = "audio";
    int block = 64;
};

int parse_probe_options(const std::vector<std::string>& args, ProbeOptions& out) {
    auto& options = out.values;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--json")
            continue;
        if (args[i] == "--plan") {
            out.include_plan = true;
            continue;
        }
        if (args[i] == "--latency") {
            out.include_latency = true;
            continue;
        }
        if (args[i] == "--node-loads") {
            out.include_node_loads = true;
            continue;
        }
        const auto key = args[i];
        if (key != "--input" && key != "--source-node" && key != "--source-port" &&
            key != "--destination-node" && key != "--destination-port" && key != "--block-frames" &&
            key != "--edge-kind" && key != "--destination-param")
            return fail("invalid_option", "unknown probe option: " + key);
        if (++i == args.size() || !options.emplace(key, args[i]).second)
            return fail("invalid_option", "each probe option requires one value: " + key);
    }
    if (options.contains("--edge-kind")) {
        out.edge_kind = options["--edge-kind"];
        if (out.edge_kind != "audio" && out.edge_kind != "control" && out.edge_kind != "event")
            return fail("invalid_edge_kind", "edge-kind must be audio, control or event");
    }
    for (const auto* key : {"--input", "--source-node", "--source-port", "--destination-node"})
        if (options[key].empty())
            return fail("missing_option", std::string("probe requires ") + key);
    if (out.edge_kind != "control" && options["--destination-port"].empty())
        return fail("missing_option", "probe requires --destination-port for audio/event edges");
    if (out.edge_kind == "control" && options["--destination-param"].empty())
        return fail("missing_option", "control probes require --destination-param");
    if (options.contains("--block-frames")) {
        const auto& value = options["--block-frames"];
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), out.block);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
            out.block < 1 || out.block > 4096)
            return fail("invalid_block_frames", "block-frames must be an integer from 1 to 4096");
    }
    return 0;
}

int read_probe_file(const std::string& path, std::string& text) {
    std::ifstream graph_file(path, std::ios::binary);
    if (!graph_file)
        return fail(patch_code(Code::FileUnreadable), "graph file is unreadable: " + path);
    // The probe shares the reload and notation bound. The buffer is sized from
    // the file, and the extra byte detects a file that grew while it was read.
    constexpr auto limit = pulp::host::max_graph_notation_bytes;
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (size_error)
        return fail(patch_code(Code::FileUnreadable), "graph file could not be read: " + path);
    if (size > limit)
        return fail("graph_file_limit",
                    "probe admits graph files no larger than " + std::to_string(limit) + " bytes");
    text.assign(static_cast<std::size_t>(size) + 1, '\0');
    graph_file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (graph_file.bad() || static_cast<std::uintmax_t>(graph_file.gcount()) != size)
        return fail(patch_code(Code::FileUnreadable), "graph file could not be read: " + path);
    text.resize(static_cast<std::size_t>(size));
    return 0;
}

// Shape and size admission over the raw document, before the serializer runs.
int admit_probe_document(const std::string& text) {
    try {
        const auto document = choc::json::parse(text);
        const auto nodes = document["nodes"];
        const auto connections = document["connections"];
        if (!document.isObject() || !nodes.isArray() || !connections.isArray())
            return fail(patch_code(Code::InvalidDocument),
                        "graph requires nodes and connections arrays");
        if (nodes.size() > 128 || connections.size() > 512)
            return fail("graph_limit", "probe admits at most 128 nodes and 512 connections");
        if (document.hasObjectMember("sample_regions") && document["sample_regions"].size() != 0)
            return fail("unsupported_node", "offline probe does not admit sample regions");
        std::int64_t ports = 0;
        for (const auto node : nodes) {
            for (const auto* key : {"num_input_ports", "num_output_ports"}) {
                const auto value = node[key];
                if (!value.isInt())
                    return fail("graph_limit", "port counts must be bounded integers");
                const auto count = value.getInt64();
                if (count < 0 || count > 1024 || (ports += count) > 1024)
                    return fail("graph_limit", "probe admits at most 1024 declared ports");
            }
            const auto type = node["type"].getString();
            if (type != "audio_in" && type != "audio_out" && type != "gain" && type != "midi_in" &&
                type != "midi_out")
                return fail("unsupported_node",
                            "offline probe requires built-in input, output, gain or MIDI nodes");
        }
    } catch (const std::exception& error) {
        return fail(patch_code(Code::InvalidDocument), error.what());
    }
    return 0;
}

int load_probe_graph(Graph& graph, const std::string& text) {
    pulp::host::GraphLoadOptions load_options;
    load_options.load_plugins = false;
    const auto loaded = pulp::host::GraphSerializer::from_json(graph, text, load_options);
    if (!loaded.ok)
        return fail(patch_code(Code::InvalidDocument), loaded.error);
    if (!loaded.missing_plugins.empty() || !loaded.missing_custom_node_types.empty())
        return fail("unresolved_node", "probe refuses unresolved node types");
    for (const auto& node : graph.nodes())
        if (node.type == pulp::host::NodeType::Plugin || node.type == pulp::host::NodeType::Custom)
            return fail("unsupported_node",
                        "offline probe supports only built-in input, output, gain and MIDI nodes");
    return 0;
}

int find_probe_endpoints(const Graph& graph, ProbeOptions& options,
                         const pulp::host::GraphNode*& source,
                         const pulp::host::GraphNode*& destination) {
    for (const auto& node : graph.nodes()) {
        if (node.name == options.values["--source-node"]) {
            if (source)
                return fail("ambiguous_node", "source node name must be unique");
            source = &node;
        }
        if (node.name == options.values["--destination-node"]) {
            if (destination)
                return fail("ambiguous_node", "destination node name must be unique");
            destination = &node;
        }
    }
    if (!source || !destination)
        return fail("unknown_node", "use existing unique source and destination node names");
    return 0;
}

int read_probe_input(const std::string& path, int block,
                     std::optional<pulp::audio::AudioFileData>& input) {
    input = pulp::audio::read_audio_file(path, [block](const auto& info) {
        return info.sample_rate >= 8000 && info.sample_rate <= 192000 && info.num_channels >= 1 &&
               info.num_channels <= 8 && info.num_frames >= 1 && info.num_frames <= 480000 &&
               (info.num_frames + block - 1) / block <= 4096;
    });
    if (!input || input->empty())
        return fail("input_limit", "input must decode to 1..8 channels, 8000..192000 Hz, 1..480000 "
                                   "frames and at most 4096 blocks");
    for (const auto& channel : input->channels)
        for (const auto sample : channel)
            if (!std::isfinite(sample))
                return fail("non_finite_input", "input samples must be finite");
    return 0;
}

int attach_probe(Graph& graph, ProbeOptions& options, pulp::host::PortHandle source_port,
                 pulp::host::PortHandle destination_port,
                 const pulp::host::GraphNode& destination) {
    const auto& edge_kind = options.edge_kind;
    Graph::EdgeProbeStatus status = Graph::EdgeProbeStatus::Busy;
    if (edge_kind == "audio") {
        status = graph.attach_audio_edge_probe(source_port, destination_port);
    } else if (edge_kind == "event") {
        status = graph.attach_event_edge_probe(source_port, destination_port);
    } else {
        std::uint32_t parameter = 0;
        const auto text = options.values["--destination-param"];
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), parameter);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            return fail("invalid_destination_param", "destination-param must be an integer");
        status = graph.attach_control_edge_probe(source_port, destination.id, parameter);
    }
    if (status != Graph::EdgeProbeStatus::Attached) {
        const auto [code, message] = probe_refusal(status, edge_kind);
        return fail(code, message);
    }
    return 0;
}

choc::value::Value probe_sample_record(const pulp::graph::GraphRuntimeProbeSample& sample) {
    auto record = choc::value::createObject("ProbeSample");
    record.addMember("sequence", static_cast<int64_t>(sample.sequence));
    record.addMember("timestamp_samples", static_cast<int64_t>(sample.timestamp_samples));
    record.addMember("edge_index", static_cast<int64_t>(sample.edge_index));
    record.addMember("frames", static_cast<int64_t>(sample.frames));
    record.addMember("kind", static_cast<int64_t>(sample.kind));
    record.addMember("peak", sample.peak);
    record.addMember("rms", sample.rms);
    record.addMember("value", sample.value);
    record.addMember("value_end", sample.value_end);
    record.addMember("payload_id", static_cast<int64_t>(sample.payload_id));
    record.addMember("payload_count", static_cast<int64_t>(sample.payload_count));
    record.addMember("payload_status", static_cast<int64_t>(sample.payload_status));
    record.addMember("payload_offset", static_cast<int64_t>(sample.payload_offset));
    record.addMember("payload_dropped", static_cast<int64_t>(sample.payload_dropped));
    return record;
}

// Renders the whole input block by block and drains the probe after each
// block into `records`.
int render_probe_records(Graph& graph, pulp::host::OfflineSignalGraphHost& host,
                         pulp::audio::AudioFileData& input, int block, const std::string& edge_kind,
                         choc::value::Value& records) {
    std::vector<float*> pointers(input.num_channels());
    for (std::uint64_t offset = 0; offset < input.num_frames(); offset += block) {
        const auto count = std::min<std::uint64_t>(block, input.num_frames() - offset);
        for (std::size_t c = 0; c < pointers.size(); ++c)
            pointers[c] = input.channels[c].data() + offset;
        const auto rendered = host.render(
            {.frame_count = count,
             .input = {pointers.data(), pointers.size(), static_cast<std::size_t>(count)}});
        if (!rendered.ok)
            return fail("render_failed", "offline probe render failed");
        pulp::graph::GraphRuntimeProbeSample sample;
        auto read_sample = [&] {
            if (edge_kind == "audio")
                return graph.read_audio_edge_probe(sample);
            if (edge_kind == "control")
                return graph.read_control_edge_probe(sample);
            return graph.read_event_edge_probe(sample);
        };
        while (read_sample()) {
            if (edge_kind == "audio" && (!std::isfinite(sample.peak) || !std::isfinite(sample.rms)))
                return fail("non_finite_sample",
                            "probe measurement is non-finite; no JSON sample report was published");
            records.addArrayElement(probe_sample_record(sample));
        }
    }
    return 0;
}

template <typename Identity> choc::value::Value probe_identity_json(const Identity& identity) {
    auto id = choc::value::createObject("ProbeIdentity");
    id.addMember("graph_identity", std::to_string(identity.authoring.graph_identity));
    id.addMember("generation", std::to_string(identity.authoring.generation));
    id.addMember("connection", std::to_string(identity.connection));
    id.addMember("source", static_cast<int64_t>(identity.source));
    id.addMember("destination", static_cast<int64_t>(identity.destination));
    id.addMember("source_port", static_cast<int64_t>(identity.source_port));
    id.addMember("destination_port", static_cast<int64_t>(identity.destination_port));
    id.addMember("parameter_id", static_cast<int64_t>(identity.parameter_id));
    return id;
}

choc::value::Value probe_plan_json(const Graph& graph) {
    auto plan = choc::value::createObject("GraphPlan");
    auto nodes = choc::value::createEmptyArray();
    for (const auto& node : graph.nodes()) {
        auto item = choc::value::createObject("GraphPlanNode");
        item.addMember("id", static_cast<int64_t>(node.id));
        item.addMember("name", node.name);
        item.addMember("input_ports", node.num_input_ports);
        item.addMember("output_ports", node.num_output_ports);
        nodes.addArrayElement(item);
    }
    auto connections = choc::value::createEmptyArray();
    for (const auto& edge : graph.connections()) {
        auto item = choc::value::createObject("GraphPlanConnection");
        item.addMember("source_node", static_cast<int64_t>(edge.source_node));
        item.addMember("source_port", static_cast<int64_t>(edge.source_port));
        item.addMember("destination_node", static_cast<int64_t>(edge.dest_node));
        item.addMember("destination_port", static_cast<int64_t>(edge.dest_port));
        item.addMember("feedback", edge.feedback);
        item.addMember("midi", edge.midi);
        item.addMember("automation", edge.automation);
        item.addMember("audio_rate_modulation", edge.audio_rate_modulation);
        item.addMember("sidechain", edge.sidechain);
        connections.addArrayElement(item);
    }
    plan.addMember("nodes", nodes);
    plan.addMember("connections", connections);
    return plan;
}

int probe_latency_json(const Graph& graph, choc::value::Value& latency) {
    latency = choc::value::createObject("LatencyPDC");
    latency.addMember("total_latency_samples", graph.latency_samples());
    auto snapshot = graph.prepared_latency_snapshot();
    if (!snapshot)
        return fail("unprepared_latency", "latency/PDC introspection requires a prepared graph");
    auto nodes = choc::value::createEmptyArray();
    auto connections = choc::value::createEmptyArray();
    for (const auto& node : snapshot->nodes) {
        auto item = choc::value::createObject("LatencyNode");
        item.addMember("node_id", static_cast<int64_t>(node.node_id));
        item.addMember("input_latency_samples", node.input_latency_samples);
        item.addMember("output_latency_samples", node.output_latency_samples);
        nodes.addArrayElement(item);
    }
    for (const auto& edge : snapshot->connections) {
        auto item = choc::value::createObject("PDCConnection");
        item.addMember("source_node", static_cast<int64_t>(edge.connection.source_node));
        item.addMember("source_port", static_cast<int64_t>(edge.connection.source_port));
        item.addMember("destination_node", static_cast<int64_t>(edge.connection.dest_node));
        item.addMember("destination_port", static_cast<int64_t>(edge.connection.dest_port));
        item.addMember("delay_samples", edge.delay_samples);
        connections.addArrayElement(item);
    }
    latency.addMember("nodes", nodes);
    latency.addMember("connections", connections);
    return 0;
}

choc::value::Value probe_node_loads_json(const Graph& graph) {
    auto loads = choc::value::createEmptyArray();
    for (const auto& entry : graph.node_loads()) {
        auto item = choc::value::createObject("NodeLoad");
        item.addMember("node_id", static_cast<int64_t>(entry.node_id));
        item.addMember("load", entry.load.load);
        item.addMember("callback_count", static_cast<int64_t>(entry.load.callback_count));
        loads.addArrayElement(item);
    }
    return loads;
}

} // namespace

int probe_graph(const std::vector<std::string>& args) {
    ProbeOptions options;
    if (const auto status = parse_probe_options(args, options))
        return status;
    const auto& edge_kind = options.edge_kind;
    const int block = options.block;
    std::string text;
    if (const auto status = read_probe_file(args[1], text))
        return status;
    if (const auto status = admit_probe_document(text))
        return status;
    Graph graph;
    if (const auto status = load_probe_graph(graph, text))
        return status;
    const pulp::host::GraphNode* source = nullptr;
    const pulp::host::GraphNode* destination = nullptr;
    if (const auto status = find_probe_endpoints(graph, options, source, destination))
        return status;
    const auto source_port = source->out(options.values["--source-port"]);
    const auto destination_port = edge_kind == "control"
                                      ? pulp::host::PortHandle{}
                                      : destination->in(options.values["--destination-port"]);
    std::optional<pulp::audio::AudioFileData> input;
    if (const auto status = read_probe_input(options.values["--input"], block, input))
        return status;
    graph.set_limits({.max_nodes = 128,
                      .max_connections = 512,
                      .max_ports = 1024,
                      .max_block_size = 4096,
                      .max_estimated_work_units = 4194304});
    if (!graph.validate_generated_graph(block).accepted)
        return fail("graph_limit", "graph exceeds offline probe resource limits");
    pulp::host::OfflineSignalGraphHost host(graph);
    if (!host.prepare({.sample_rate = static_cast<double>(input->sample_rate),
                       .block_frames = block,
                       .input_channels = static_cast<int>(input->num_channels()),
                       .output_channels = 8}))
        return fail("prepare_failed", "graph preparation failed");
    if (const auto status =
            attach_probe(graph, options, source_port, destination_port, *destination))
        return status;
    const auto identity = edge_kind == "audio"     ? graph.audio_edge_probe_identity()
                          : edge_kind == "control" ? graph.control_edge_probe_identity()
                                                   : graph.event_edge_probe_identity();
    if (edge_kind == "event") {
        pulp::midi::MidiBuffer events;
        events.reserve(4);
        events.add(pulp::midi::MidiEvent::note_on(0, 60, 100));
        events.sort();
        if (!graph.inject_midi(source->id, events))
            return fail("event_injection_failed", "event probe could not seed the MIDI input");
    }
    // Timing is opt-in: an unattached/ordinary graph remains on the zero-cost
    // path, and the per-node clock reads begin only when requested by tooling.
    if (options.include_node_loads)
        graph.set_node_load_monitoring(true);
    auto records = choc::value::createEmptyArray();
    if (const auto status = render_probe_records(graph, host, *input, block, edge_kind, records))
        return status;
    auto report = choc::value::createObject("GraphProbe");
    report.addMember("valid", true);
    report.addMember("code", "Attached");
    report.addMember("edge_kind", edge_kind);
    report.addMember("observation", edge_kind == "audio" ? "source_before_mixing_and_pdc"
                                    : edge_kind == "control"
                                        ? "per_edge_mapped_and_slewed_before_mixing_and_bounds"
                                        : "events_before_destination_consumer");
    report.addMember("sample_rate", static_cast<int64_t>(input->sample_rate));
    report.addMember("frames", static_cast<int64_t>(input->num_frames()));
    report.addMember("dropped", static_cast<int64_t>(
                                    edge_kind == "audio"     ? graph.audio_edge_probe_dropped()
                                    : edge_kind == "control" ? graph.control_edge_probe_dropped()
                                                             : graph.event_edge_probe_dropped()));
    report.addMember("identity", probe_identity_json(identity));
    report.addMember("records", records);
    if (options.include_plan)
        report.addMember("plan", probe_plan_json(graph));
    if (options.include_latency) {
        choc::value::Value latency;
        if (const auto status = probe_latency_json(graph, latency))
            return status;
        report.addMember("latency_pdc", latency);
    }
    if (options.include_node_loads)
        report.addMember("node_loads", probe_node_loads_json(graph));
    std::cout << choc::json::toString(report) << '\n';
    return 0;
}

} // namespace pulp_cli::graph_detail
