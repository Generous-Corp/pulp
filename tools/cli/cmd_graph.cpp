#include "atomic_text_file.hpp"
#include "cli_common.hpp"

#include <choc/text/choc_JSON.h>
#include <algorithm>
#include <pulp/audio/audio_file.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <pulp/host/signal_graph.hpp>
#include <sstream>

namespace {
std::optional<std::string> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}
void usage() {
    std::cout << "Usage: pulp graph <inspect|validate|explain|render|fmt|canonicalize> <file.pulpgraph> [--json] [--output file.wav (render)]\n";
}
void json_error(const std::string& code, const std::string& message) {
    auto value = choc::value::createObject("GraphError");
    value.addMember("valid", false);
    value.addMember("code", code);
    value.addMember("message", message);
    std::cout << choc::json::toString(value) << '\n';
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
    const bool json = std::find(args.begin() + 2, args.end(), "--json") != args.end();
    if (op != "inspect" && op != "validate" && op != "explain" && op != "render"
        && op != "fmt" && op != "canonicalize") {
        if (json) json_error("unknown_operation", "unknown graph subcommand: " + op);
        else std::cerr << "pulp graph: unknown subcommand '" << op << "'\n";
        return 2;
    }
    auto text = read_file(path);
    if (!text) {
        if (json) json_error("file_unreadable", "cannot read '" + path + "'");
        else std::cerr << "pulp graph: cannot read '" << path << "'\n";
        return 1;
    }
    pulp::host::SignalGraph graph;
    pulp::host::GraphLoadOptions opts;
    opts.load_plugins = false;
    auto loaded = pulp::host::GraphSerializer::from_json(graph, *text, opts);
    if (!loaded.ok) {
        if (json) json_error("invalid_graph", loaded.error);
        else std::cerr << "pulp graph: invalid graph: " << loaded.error << "\n";
        return 1;
    }
    if (op == "validate") {
        if (json) {
            std::cout << "{\"valid\":true,\"code\":\"ok\",\"format_version\":"
                      << pulp::host::GraphSerializer::current_format_version()
                      << ",\"nodes\":" << graph.nodes().size()
                      << ",\"connections\":" << graph.connections().size() << "}\n";
        } else std::cout << "valid: true\nformat_version: "
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
            if (json) json_error("unresolved_node", "cannot render with unresolved node types");
            else std::cerr << "pulp graph: cannot render with unresolved node types\n";
            return 1;
        }
        pulp::host::OfflineSignalGraphHost host(graph);
        pulp::host::OfflineSignalGraphConfig config;
        if (!host.prepare(config)) {
            if (json) json_error("prepare_failed", "graph preparation failed");
            else std::cerr << "pulp graph: graph preparation failed\n";
            return 1;
        }
        pulp::host::OfflineSignalGraphOptions options;
        options.frame_count = 48000;
        const auto rendered = host.render(options);
        if (!rendered.ok) {
            if (json) json_error("render_failed", "offline render failed");
            else std::cerr << "pulp graph: offline render failed\n";
            return 1;
        }
        const auto output_arg = std::find(args.begin() + 2, args.end(), "--output");
        if (output_arg != args.end()) {
            if (output_arg + 1 == args.end()) {
                if (json) json_error("missing_output", "--output requires a WAV path");
                else std::cerr << "pulp graph: --output requires a WAV path\n";
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
                if (json) json_error("output_failed", "could not write WAV output");
                else std::cerr << "pulp graph: could not write WAV output\n";
                return 1;
            }
        }
        if (json) std::cout << "{\"valid\":true,\"code\":\"ok\",\"frames\":48000,\"sample_rate\":48000,\"blocks\":" << rendered.blocks_rendered << "}\n";
        else std::cout << "rendered 48000 frames in " << rendered.blocks_rendered << " blocks\n";
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
        const auto serialized = pulp::host::GraphSerializer::to_json(graph, loaded.editor_layout);
        if (serialized.empty()) {
            std::cerr << "pulp graph: cannot canonicalize graph because serialization failed\n";
            return 1;
        }
        try {
            const auto parsed = choc::json::parse(serialized);
            if (!parsed.isObject()) {
                std::cerr << "pulp graph: cannot canonicalize graph because serialization was not "
                             "an object\n";
                return 1;
            }
        } catch (const std::exception& error) {
            std::cerr << "pulp graph: cannot canonicalize graph because serialization was invalid: "
                      << error.what() << '\n';
            return 1;
        }
        std::string error;
        if (!pulp::cli::write_text_file_atomically(std::filesystem::path(path), serialized + '\n',
                                                   error)) {
            std::cerr << "pulp graph: cannot replace '" << path << "': " << error << '\n';
            return 1;
        }
        return 0;
    }
    return 2;
}
