#include "cli_common.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <pulp/host/graph_serializer.hpp>
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
    std::cout << "Usage: pulp graph <inspect|validate|canonicalize> <file.pulpgraph> [--json]\n";
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
    auto text = read_file(path);
    if (!text) {
        std::cerr << "pulp graph: cannot read '" << path << "'\n";
        return 1;
    }
    pulp::host::SignalGraph graph;
    pulp::host::GraphLoadOptions opts;
    opts.load_plugins = false;
    auto loaded = pulp::host::GraphSerializer::from_json(graph, *text, opts);
    if (!loaded.ok) {
        std::cerr << "pulp graph: invalid graph: " << loaded.error << "\n";
        return 1;
    }
    if (op == "validate") {
        std::cout << "valid: true\nformat_version: "
                  << pulp::host::GraphSerializer::current_format_version()
                  << "\nnodes: " << graph.nodes().size()
                  << "\nconnections: " << graph.connections().size() << "\n";
        return 0;
    }
    if (op == "inspect") {
        const bool json = args.size() > 2 && args[2] == "--json";
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
    if (op == "canonicalize") {
        if (!loaded.missing_plugins.empty()) {
            std::cerr << "pulp graph: cannot canonicalize while plugin nodes are unresolved; "
                         "resolve all plugins first\n";
            return 1;
        }
        const std::filesystem::path destination(path);
        const auto temporary = destination.string() + ".tmp-" + std::to_string(std::rand());
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "pulp graph: cannot write temporary output beside '" << path << "'\n";
            return 1;
        }
        out << pulp::host::GraphSerializer::to_json(graph) << '\n';
        if (!out)
            return 1;
        out.close();
        std::error_code error;
        std::filesystem::rename(temporary, destination, error);
        if (error) {
            std::filesystem::remove(temporary);
            std::cerr << "pulp graph: cannot replace '" << path << "': " << error.message() << '\n';
            return 1;
        }
        return 0;
    }
    std::cerr << "pulp graph: unknown subcommand '" << op << "'\n";
    usage();
    return 2;
}
