#include "atomic_text_file.hpp"
#include "cli_common.hpp"

#include <algorithm>
#include <charconv>
#include <choc/text/choc_JSON.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <pulp/audio/audio_file.hpp>
#include <pulp/host/graph_rt_report.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <pulp/host/signal_graph.hpp>

namespace {
void usage() {
    std::cout << "Usage: pulp graph "
                 "<inspect|validate|explain|render|probe|report|fmt|canonicalize|save> "
                 "<file.pulpgraph> [--json] [--output path]\n";
    std::cout << "report: [--sample-rate 48000] [--max-block-size 512] [--block-size N] "
                 "[--measure-blocks 16] (always JSON)\n";
    std::cout << "probe: --input file.wav --source-node NAME --source-port NAME "
                 "--destination-node NAME --destination-port NAME [--block-frames 64] "
                 "[--plan] [--latency] [--node-loads]\n";
}
void json_error(const std::string& code, const std::string& message) {
    auto value = choc::value::createObject("GraphError");
    value.addMember("valid", false);
    value.addMember("code", code);
    value.addMember("message", message);
    std::cout << choc::json::toString(value) << '\n';
}

int probe_graph(const std::vector<std::string>& args) {
    using Graph = pulp::host::SignalGraph;
    const auto fail = [](const std::string& code, const std::string& message) {
        json_error(code, message);
        return 2;
    };
    std::map<std::string, std::string> options;
    bool include_plan = false;
    bool include_latency = false;
    bool include_node_loads = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--json")
            continue;
        if (args[i] == "--plan") {
            include_plan = true;
            continue;
        }
        if (args[i] == "--latency") {
            include_latency = true;
            continue;
        }
        if (args[i] == "--node-loads") {
            include_node_loads = true;
            continue;
        }
        const auto key = args[i];
        if (key != "--input" && key != "--source-node" && key != "--source-port" &&
            key != "--destination-node" && key != "--destination-port" && key != "--block-frames")
            return fail("invalid_option", "unknown probe option: " + key);
        if (++i == args.size() || !options.emplace(key, args[i]).second)
            return fail("invalid_option", "each probe option requires one value: " + key);
    }
    for (const auto* key :
         {"--input", "--source-node", "--source-port", "--destination-node", "--destination-port"})
        if (options[key].empty())
            return fail("missing_option", std::string("probe requires ") + key);
    int block = 64;
    if (options.contains("--block-frames")) {
        const auto& value = options["--block-frames"];
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), block);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || block < 1 ||
            block > 4096)
            return fail("invalid_block_frames", "block-frames must be an integer from 1 to 4096");
    }
    std::ifstream graph_file(args[1], std::ios::binary);
    if (!graph_file)
        return fail("graph_file_limit",
                    "probe requires a readable graph file no larger than 1 MiB");
    std::string text(1024 * 1024 + 1, '\0');
    graph_file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(graph_file.gcount()));
    if (text.size() > 1024 * 1024 || graph_file.bad())
        return fail("graph_file_limit", "graph file exceeds probe limits or could not be read");
    try {
        const auto document = choc::json::parse(text);
        const auto nodes = document["nodes"];
        const auto connections = document["connections"];
        if (!document.isObject() || !nodes.isArray() || !connections.isArray())
            return fail("invalid_graph", "graph requires nodes and connections arrays");
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
        return fail("invalid_graph", error.what());
    }
    Graph graph;
    pulp::host::GraphLoadOptions load_options;
    load_options.load_plugins = false;
    const auto loaded = pulp::host::GraphSerializer::from_json(graph, text, load_options);
    if (!loaded.ok)
        return fail("invalid_graph", loaded.error);
    if (!loaded.missing_plugins.empty() || !loaded.missing_custom_node_types.empty())
        return fail("unresolved_node", "probe refuses unresolved node types");
    for (const auto& node : graph.nodes())
        if (node.type == pulp::host::NodeType::Plugin || node.type == pulp::host::NodeType::Custom)
            return fail("unsupported_node",
                        "offline probe supports only built-in input, output, gain and MIDI nodes");
    const pulp::host::GraphNode* source = nullptr;
    const pulp::host::GraphNode* destination = nullptr;
    for (const auto& node : graph.nodes()) {
        if (node.name == options["--source-node"]) {
            if (source)
                return fail("ambiguous_node", "source node name must be unique");
            source = &node;
        }
        if (node.name == options["--destination-node"]) {
            if (destination)
                return fail("ambiguous_node", "destination node name must be unique");
            destination = &node;
        }
    }
    if (!source || !destination)
        return fail("unknown_node", "use existing unique source and destination node names");
    const auto source_port = source->out(options["--source-port"]);
    const auto destination_port = destination->in(options["--destination-port"]);
    auto input = pulp::audio::read_audio_file(options["--input"], [block](const auto& info) {
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
    const auto status = graph.attach_audio_edge_probe(source_port, destination_port);
    const char* status_name = "Busy";
    switch (status) {
    case Graph::EdgeProbeStatus::Attached:
        status_name = "Attached";
        break;
    case Graph::EdgeProbeStatus::Detached:
        status_name = "Detached";
        break;
    case Graph::EdgeProbeStatus::Unprepared:
        status_name = "Unprepared";
        break;
    case Graph::EdgeProbeStatus::UnknownPort:
        status_name = "UnknownPort";
        break;
    case Graph::EdgeProbeStatus::MissingEdge:
        status_name = "MissingEdge";
        break;
    case Graph::EdgeProbeStatus::UnsupportedKind:
        status_name = "UnsupportedKind";
        break;
    case Graph::EdgeProbeStatus::UnsupportedExecution:
        status_name = "UnsupportedExecution";
        break;
    case Graph::EdgeProbeStatus::Busy:
        break;
    }
    if (status != Graph::EdgeProbeStatus::Attached)
        return fail(status_name, "cannot attach the requested serial ordinary audio edge");
    const auto identity = graph.audio_edge_probe_identity();
    // Timing is opt-in: an unattached/ordinary graph remains on the zero-cost
    // path, and the per-node clock reads begin only when requested by tooling.
    if (include_node_loads)
        graph.set_node_load_monitoring(true);
    auto records = choc::value::createEmptyArray();
    std::vector<float*> pointers(input->num_channels());
    for (std::uint64_t offset = 0; offset < input->num_frames(); offset += block) {
        const auto count = std::min<std::uint64_t>(block, input->num_frames() - offset);
        for (std::size_t c = 0; c < pointers.size(); ++c)
            pointers[c] = input->channels[c].data() + offset;
        const auto rendered = host.render(
            {.frame_count = count,
             .input = {pointers.data(), pointers.size(), static_cast<std::size_t>(count)}});
        if (!rendered.ok)
            return fail("render_failed", "offline probe render failed");
        pulp::graph::GraphRuntimeProbeSample sample;
        while (graph.read_audio_edge_probe(sample)) {
            if (!std::isfinite(sample.peak) || !std::isfinite(sample.rms))
                return fail("non_finite_sample",
                            "probe measurement is non-finite; no JSON sample report was published");
            auto record = choc::value::createObject("ProbeSample");
            record.addMember("sequence", static_cast<int64_t>(sample.sequence));
            record.addMember("timestamp_samples", static_cast<int64_t>(sample.timestamp_samples));
            record.addMember("edge_index", static_cast<int64_t>(sample.edge_index));
            record.addMember("frames", static_cast<int64_t>(sample.frames));
            record.addMember("peak", sample.peak);
            record.addMember("rms", sample.rms);
            records.addArrayElement(record);
        }
    }
    auto report = choc::value::createObject("GraphProbe");
    report.addMember("valid", true);
    report.addMember("code", "Attached");
    report.addMember("observation", "source_before_mixing_and_pdc");
    report.addMember("sample_rate", static_cast<int64_t>(input->sample_rate));
    report.addMember("frames", static_cast<int64_t>(input->num_frames()));
    report.addMember("dropped", static_cast<int64_t>(graph.audio_edge_probe_dropped()));
    auto id = choc::value::createObject("ProbeIdentity");
    id.addMember("graph_identity", std::to_string(identity.authoring.graph_identity));
    id.addMember("generation", std::to_string(identity.authoring.generation));
    id.addMember("connection", std::to_string(identity.connection));
    id.addMember("source", static_cast<int64_t>(identity.source));
    id.addMember("destination", static_cast<int64_t>(identity.destination));
    id.addMember("source_port", static_cast<int64_t>(identity.source_port));
    id.addMember("destination_port", static_cast<int64_t>(identity.destination_port));
    report.addMember("identity", id);
    report.addMember("records", records);
    if (include_plan) {
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
        report.addMember("plan", plan);
    }
    if (include_latency) {
        auto latency = choc::value::createObject("LatencyPDC");
        latency.addMember("total_latency_samples", graph.latency_samples());
        auto snapshot = graph.prepared_latency_snapshot();
        if (!snapshot)
            return fail("unprepared_latency",
                        "latency/PDC introspection requires a prepared graph");
        auto nodes = choc::value::createEmptyArray();
        auto connections = choc::value::createEmptyArray();
        {
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
        }
        latency.addMember("nodes", nodes);
        latency.addMember("connections", connections);
        report.addMember("latency_pdc", latency);
    }
    if (include_node_loads) {
        auto loads = choc::value::createEmptyArray();
        for (const auto& entry : graph.node_loads()) {
            auto item = choc::value::createObject("NodeLoad");
            item.addMember("node_id", static_cast<int64_t>(entry.node_id));
            item.addMember("load", entry.load.load);
            item.addMember("callback_count", static_cast<int64_t>(entry.load.callback_count));
            loads.addArrayElement(item);
        }
        report.addMember("node_loads", loads);
    }
    std::cout << choc::json::toString(report) << '\n';
    return 0;
}
bool parse_report_int(const std::string& text, int min, int max, int& out) {
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < min ||
        value > max)
        return false;
    out = value;
    return true;
}

// `pulp graph report`: prepare the graph offline and print the static realtime
// report (declared contracts, latency semantics, and a measured pass) as JSON.
int report_graph(const std::vector<std::string>& args) {
    int sample_rate = 48000;
    int max_block = 512;
    int block = 0;
    int measure_blocks = 16;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const auto& key = args[i];
        if (key == "--json")
            continue;
        int* target = key == "--sample-rate"      ? &sample_rate
                      : key == "--max-block-size" ? &max_block
                      : key == "--block-size"     ? &block
                      : key == "--measure-blocks" ? &measure_blocks
                                                  : nullptr;
        if (target == nullptr) {
            json_error("invalid_option", "unknown report option: " + key);
            return 2;
        }
        const int min = target == &measure_blocks ? 0 : 1;
        const int max = target == &sample_rate ? 768000 : target == &measure_blocks ? 4096 : 16384;
        if (++i == args.size() || !parse_report_int(args[i], min, max, *target)) {
            json_error("invalid_option", key + " requires an integer from " + std::to_string(min) +
                                             " to " + std::to_string(max));
            return 2;
        }
    }
    if (block > max_block) {
        json_error("invalid_option", "--block-size must not exceed --max-block-size");
        return 2;
    }
    pulp::host::SignalGraph graph;
    pulp::host::GraphLoadOptions load_options;
    load_options.load_plugins = false;
    const auto loaded = pulp::host::GraphSerializer::from_file(graph, args[1], load_options);
    if (!loaded.ok) {
        json_error(loaded.error.starts_with("graph file ") ? "file_unreadable" : "invalid_graph",
                   loaded.error);
        return 1;
    }
    if (!graph.prepare(static_cast<double>(sample_rate), max_block)) {
        json_error("prepare_failed", "graph preparation failed");
        return 1;
    }
    pulp::host::GraphRtReportOptions report_options;
    report_options.block_size = block;
    auto report = pulp::host::build_graph_rt_report(graph, report_options);
    if (measure_blocks > 0) {
        pulp::host::GraphRtMeasureOptions measure;
        measure.blocks = measure_blocks;
        measure.block_size = report.block_size;
        if (const auto measured = pulp::host::measure_graph_rt_work(graph, measure))
            pulp::host::apply_graph_rt_measurement(report, *measured, report_options);
    }
    std::cout << pulp::host::graph_rt_report_to_json(report) << '\n';
    return 0;
}
} // namespace
int cmd_graph(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        usage();
        return 0;
    }
    const std::string op = args[0];
    if (args.size() < 2) {
        usage();
        return 2;
    }
    const std::string path = args[1];
    if (op == "probe")
        return probe_graph(args);
    if (op == "report")
        return report_graph(args);
    const bool json = std::find(args.begin() + 2, args.end(), "--json") != args.end();
    for (const auto* flag : {"--plan", "--latency", "--node-loads"}) {
        if (std::find(args.begin() + 2, args.end(), flag) != args.end()) {
            if (json)
                json_error("invalid_option", std::string(flag) + " is only valid for graph probe");
            else
                std::cerr << "pulp graph: " << flag << " is only valid for graph probe\n";
            return 2;
        }
    }
    if (op != "inspect" && op != "validate" && op != "explain" && op != "render" && op != "fmt" &&
        op != "canonicalize" && op != "save") {
        if (json)
            json_error("unknown_operation", "unknown graph subcommand: " + op);
        else
            std::cerr << "pulp graph: unknown subcommand '" << op << "'\n";
        return 2;
    }
    pulp::host::SignalGraph graph;
    pulp::host::GraphLoadOptions opts;
    opts.load_plugins = false;
    auto loaded = pulp::host::GraphSerializer::from_file(graph, path, opts);
    if (!loaded.ok) {
        const auto file_error = loaded.error.starts_with("graph file ");
        if (json)
            json_error(file_error ? "file_unreadable" : "invalid_graph", loaded.error);
        else
            std::cerr << "pulp graph: " << (file_error ? "cannot read: " : "invalid graph: ")
                      << loaded.error << "\n";
        return 1;
    }
    if (op == "validate") {
        if (json) {
            std::cout << "{\"valid\":true,\"code\":\"ok\",\"format_version\":"
                      << pulp::host::GraphSerializer::current_format_version()
                      << ",\"nodes\":" << graph.nodes().size()
                      << ",\"connections\":" << graph.connections().size() << "}\n";
        } else
            std::cout << "valid: true\nformat_version: "
                      << pulp::host::GraphSerializer::current_format_version()
                      << "\nnodes: " << graph.nodes().size()
                      << "\nconnections: " << graph.connections().size() << "\n";
        return 0;
    }
    if (op == "inspect" || op == "explain") {
        if (json)
            std::cout << "{\"valid\":true,\"format_version\":"
                      << pulp::host::GraphSerializer::current_format_version()
                      << ",\"nodes\":" << graph.nodes().size()
                      << ",\"connections\":" << graph.connections().size()
                      << ",\"missing_plugins\":" << loaded.missing_plugins.size() << "}\n";
        else
            std::cout << "format_version: " << pulp::host::GraphSerializer::current_format_version()
                      << "\nnodes: " << graph.nodes().size()
                      << "\nconnections: " << graph.connections().size()
                      << "\nmissing_plugins: " << loaded.missing_plugins.size() << "\n";
        return 0;
    }
    if (op == "render") {
        if (!loaded.missing_plugins.empty() || !loaded.missing_custom_node_types.empty()) {
            if (json)
                json_error("unresolved_node", "cannot render with unresolved node types");
            else
                std::cerr << "pulp graph: cannot render with unresolved node types\n";
            return 1;
        }
        pulp::host::OfflineSignalGraphHost host(graph);
        pulp::host::OfflineSignalGraphConfig config;
        if (!host.prepare(config)) {
            if (json)
                json_error("prepare_failed", "graph preparation failed");
            else
                std::cerr << "pulp graph: graph preparation failed\n";
            return 1;
        }
        pulp::host::OfflineSignalGraphOptions options;
        options.frame_count = 48000;
        const auto rendered = host.render(options);
        if (!rendered.ok) {
            if (json)
                json_error("render_failed", "offline render failed");
            else
                std::cerr << "pulp graph: offline render failed\n";
            return 1;
        }
        const auto output_arg = std::find(args.begin() + 2, args.end(), "--output");
        if (output_arg != args.end()) {
            if (output_arg + 1 == args.end()) {
                if (json)
                    json_error("missing_output", "--output requires a WAV path");
                else
                    std::cerr << "pulp graph: --output requires a WAV path\n";
                return 2;
            }
            pulp::audio::AudioFileData file;
            file.sample_rate = 48000;
            for (std::size_t channel = 0; channel < rendered.audio.num_channels(); ++channel) {
                const auto samples = rendered.audio.channel(channel);
                file.channels.emplace_back(samples.begin(), samples.end());
            }
            if (!pulp::audio::write_wav_file(*(output_arg + 1), file,
                                             pulp::audio::WavBitDepth::Float32)) {
                if (json)
                    json_error("output_failed", "could not write WAV output");
                else
                    std::cerr << "pulp graph: could not write WAV output\n";
                return 1;
            }
        }
        if (json)
            std::cout << "{\"valid\":true,\"code\":\"ok\",\"frames\":48000,\"sample_rate\":48000,"
                         "\"blocks\":"
                      << rendered.blocks_rendered << "}\n";
        else
            std::cout << "rendered 48000 frames in " << rendered.blocks_rendered << " blocks\n";
        return 0;
    }
    if (op == "save") {
        const auto output_arg = std::find(args.begin() + 2, args.end(), "--output");
        if (output_arg == args.end() || output_arg + 1 == args.end() ||
            (*(output_arg + 1)).empty()) {
            if (json)
                json_error("missing_output", "save requires --output PATH");
            else
                std::cerr << "pulp graph: save requires --output PATH\n";
            return 2;
        }
        if (!loaded.missing_plugins.empty() || !loaded.state_restore_failures.empty() ||
            !loaded.missing_custom_node_types.empty()) {
            if (json)
                json_error("serialization_refused",
                           "cannot save with unresolved plugin or custom node state");
            else
                std::cerr
                    << "pulp graph: cannot save with unresolved plugin or custom node state\n";
            return 1;
        }
        const auto saved =
            pulp::host::GraphSerializer::to_file(graph, *(output_arg + 1), loaded.editor_layout);
        if (!saved.ok) {
            if (json)
                json_error(pulp::host::GraphSerializer::save_code_name(saved.code), saved.error);
            else
                std::cerr << "pulp graph: " << saved.error << '\n';
            return 1;
        }
        if (json)
            std::cout << "{\"valid\":true,\"code\":\"saved\"}\n";
        return 0;
    }
    if (op == "canonicalize" || op == "fmt") {
        if (!loaded.missing_plugins.empty()) {
            std::cerr << "pulp graph: cannot canonicalize while plugin nodes are unresolved; "
                         "resolve all plugins first\n";
            return 1;
        }
        if (!loaded.state_restore_failures.empty()) {
            std::cerr << "pulp graph: cannot canonicalize because "
                      << loaded.state_restore_failures.size()
                      << " plugin(s) rejected their persisted state; canonicalizing would "
                         "overwrite it\n";
            return 1;
        }
        const auto saved = pulp::host::GraphSerializer::to_file(graph, std::filesystem::path(path),
                                                                loaded.editor_layout);
        if (!saved.ok) {
            std::cerr << "pulp graph: cannot canonicalize graph ("
                      << pulp::host::GraphSerializer::save_code_name(saved.code)
                      << "): " << saved.error << '\n';
            return 1;
        }
        return 0;
    }
    return 2;
}
