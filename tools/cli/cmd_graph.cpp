// `pulp graph`: the dispatcher plus the handlers that load one graph document
// and print or rewrite it (inspect, explain, validate, fmt, canonicalize and
// save). probe, render, report and diff live in their own translation units;
// cmd_graph_internal.hpp lists them.

#include "cli_common.hpp"
#include "cmd_graph_internal.hpp"

#include <algorithm>
#include <choc/text/choc_JSON.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <pulp/host/graph_patch.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph.hpp>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pulp_cli::graph_detail;

void usage() {
    std::cout << "Usage: pulp graph "
                 "<catalog|inspect|validate|explain|render|probe|report|fmt|canonicalize|save> "
                 "<file.pulpgraph|file.pat> [--json] [--notation] [--output path]\n";
    std::cout << "save: --output path [--saved-identity]; a saved-identity input (and "
                 "--saved-identity) writes the identity envelope that graph diff reads, and fmt "
                 "keeps it\n";
    std::cout
        << "diff: pulp graph diff <before.pulpgraph> <after.pulpgraph> [--json]; compares two "
           "saved-identity documents by logical node identity (read-only)\n";
    std::cout << "render: --input file.wav (required when the graph has audio_in) "
                 "[--output out.wav] [--frames N]; reports frames, sample_rate and peak\n";
    std::cout << "report: [--sample-rate 48000] [--max-block-size 512] [--block-size N] "
                 "[--measure-blocks 16] (always JSON)\n";
    std::cout << "probe: --input file.wav --source-node NAME --source-port NAME "
                 "--destination-node NAME --destination-port NAME [--block-frames 64] "
                 "[--edge-kind audio|control|event] [--destination-param ID] "
                 "[--plan] [--latency] [--node-loads]\n";
}

// True when `flag` appears after the subcommand and its path.
bool has_flag(const std::vector<std::string>& args, const char* flag) {
    return std::find(args.begin() + 2, args.end(), flag) != args.end();
}

struct RewriteDeclaration {
    bool saved_identity = false;
    bool subgraph_records = false;
};

// What a rewrite (save/fmt/canonicalize) must not drop: the saved-identity
// envelope or subgraph records, which the plain writer cannot express. The
// inspector's size and shape preflight runs once, first; a document it refuses
// for size, nesting, members or syntax is never parsed here, so the loader
// refuses it under the same bounds. A legacy document refused only for a
// missing envelope declares no identity but is still checked for subgraph
// records. A parse failure is left to the loader.
RewriteDeclaration declares_rewrite_content(const std::string& text) {
    using Refusal = pulp::host::GraphDocumentRefusal;
    RewriteDeclaration out;
    const auto inspection = pulp::host::GraphSerializer::inspect_document(text);
    bool parse_envelope = false;
    if (inspection.ok()) {
        out.saved_identity = true;
        parse_envelope = true;
    } else {
        switch (inspection.refusal) {
        case Refusal::EmptyDocument:
        case Refusal::DocumentTooLarge:
        case Refusal::NestingLimitExceeded:
        case Refusal::MemberLimitExceeded:
        case Refusal::InvalidJson:
            return out;
        default:
            parse_envelope = inspection.refusal != Refusal::LogicalIdentityMissing;
            break;
        }
    }
    try {
        const auto root = choc::json::parse(text);
        if (!root.isObject())
            return out;
        out.subgraph_records = root.hasObjectMember("subgraphs");
        if (parse_envelope && !out.saved_identity) {
            out.saved_identity = root.hasObjectMember("identity_schema") ||
                                 root.hasObjectMember("logical_identities") ||
                                 root.hasObjectMember("bindings") || out.subgraph_records;
        }
    } catch (...) {
    }
    return out;
}

// One loaded document and what the rewrite handlers need to know about it.
struct LoadedGraph {
    std::unique_ptr<pulp::host::SignalGraph> graph = std::make_unique<pulp::host::SignalGraph>();
    pulp::host::GraphSerializer::LoadResult loaded;
    bool saved_identity = false;
};

// Strict load through the notation/patch loader (validate, `.pat` inputs and
// `fmt --notation`).
int load_strict(const std::string& path, bool json, LoadedGraph& out) {
    auto strict = pulp::host::load_graph_patch_file(path);
    if (!strict) {
        if (json) {
            // The loader's typed diagnostic is the contract for notation
            // and saved graphs alike, so the code matches the C++ API.
            std::cout << pulp::host::graph_patch_diagnostic_json(strict.diagnostic) << '\n';
        } else
            std::cerr << "pulp graph: " << strict.diagnostic.message() << '\n';
        return 1;
    }
    out.graph = std::move(strict.graph);
    out.loaded.ok = true;
    return 0;
}

// Load for save, fmt and canonicalize.
int load_for_rewrite(const std::string& op, const std::vector<std::string>& args,
                     const std::string& path, bool json, LoadedGraph& out) {
    auto& graph_ptr = out.graph;
    auto& loaded = out.loaded;
    pulp::host::GraphLoadOptions opts;
    opts.load_plugins = false;
    // A rewrite must not drop what the plain writer cannot express: the
    // saved-identity envelope is kept (and verified on load), and subgraph
    // records, which need the caller's templates, are refused.
    Code read_code = Code::Ok;
    std::string message;
    auto text = read_graph_document(path, read_code, message);
    if (!text) {
        if (json)
            json_error(patch_code(read_code), message);
        else
            std::cerr << "pulp graph: " << message << '\n';
        return 1;
    }
    const auto declared = declares_rewrite_content(*text);
    out.saved_identity = declared.saved_identity;
    if (declared.subgraph_records) {
        const std::string refusal =
            "the document holds subgraph records, which need the authoring templates to "
            "rewrite; rewriting it here would drop every subgraph instance";
        if (json)
            json_error("subgraph_records_unsupported", refusal);
        else
            std::cerr << "pulp graph: " << refusal << '\n';
        return 1;
    }
    loaded = out.saved_identity
                 ? pulp::host::GraphSerializer::from_saved_document(*graph_ptr, *text, opts)
                 : pulp::host::GraphSerializer::from_json(*graph_ptr, *text, opts);
    // A rewrite writes back only the nodes the load created, so a node
    // record the tolerant load could not add would vanish from the file.
    if (loaded.ok && !loaded.unrestored_node_ids.empty()) {
        loaded.ok = false;
        loaded.error = "node_not_restored: node record " +
                       std::to_string(loaded.unrestored_node_ids.front()) +
                       " could not be added to the graph; rewriting the document would "
                       "drop it";
    }
    // Stamping an envelope on a plain document asserts that the written
    // graph is the document, so the tolerant load must not have dropped an
    // edge. A spectral document is read strictly and restores every edge,
    // spectral lanes included, so it passes the same check.
    const bool adds_identity =
        loaded.ok && !out.saved_identity && op == "save" && has_flag(args, "--saved-identity");
    if (adds_identity) {
        if (auto missing = pulp::host::GraphSerializer::unrestored_connection(
                *graph_ptr, *text, loaded.persisted_node_ids)) {
            loaded.ok = false;
            loaded.error = "connection_not_restored: " + *missing +
                           "; the identity envelope would describe a different graph";
        }
    }
    if (!loaded.ok) {
        if (json)
            json_error(patch_code(Code::InvalidDocument), loaded.error);
        else
            std::cerr << "pulp graph: " << loaded.error << '\n';
        return 1;
    }
    return 0;
}

// Tolerant file load for inspect, explain and render.
int load_tolerant(const std::string& path, bool json, LoadedGraph& out) {
    pulp::host::GraphLoadOptions opts;
    opts.load_plugins = false;
    out.loaded = pulp::host::GraphSerializer::from_file(*out.graph, path, opts);
    if (!out.loaded.ok) {
        if (json)
            json_error(patch_code(file_load_code(out.loaded)), out.loaded.error);
        else
            std::cerr << "pulp graph: " << out.loaded.error << '\n';
        return 1;
    }
    return 0;
}

int format_notation(const pulp::host::SignalGraph& graph, bool json) {
    const auto formatted = pulp::host::format_graph_patch(graph);
    if (!formatted) {
        if (json)
            std::cout << pulp::host::graph_patch_diagnostic_json(formatted.diagnostic) << '\n';
        else
            std::cerr << formatted.diagnostic.message() << '\n';
        return 1;
    }
    if (json) {
        auto result = choc::value::createObject("GraphNotation");
        result.addMember("valid", true);
        result.addMember("code", patch_code(Code::Ok));
        result.addMember("notation", formatted.text);
        std::cout << choc::json::toString(result) << '\n';
    } else
        std::cout << formatted.text;
    return 0;
}

int print_validate(const pulp::host::SignalGraph& graph, bool json) {
    if (json) {
        std::cout << "{\"valid\":true,\"code\":\"" << patch_code(Code::Ok)
                  << "\",\"format_version\":"
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

int print_inspect(const pulp::host::SignalGraph& graph,
                  const pulp::host::GraphSerializer::LoadResult& loaded, bool json) {
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

int save_graph(const std::vector<std::string>& args, const pulp::host::SignalGraph& graph,
               const pulp::host::GraphSerializer::LoadResult& loaded, bool saved_identity,
               bool json) {
    const auto output_arg = std::find(args.begin() + 2, args.end(), "--output");
    if (output_arg == args.end() || output_arg + 1 == args.end() || (*(output_arg + 1)).empty()) {
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
            std::cerr << "pulp graph: cannot save with unresolved plugin or custom node state\n";
        return 1;
    }
    const bool write_identity = saved_identity || has_flag(args, "--saved-identity");
    const auto saved =
        write_identity
            ? pulp::host::GraphSerializer::to_saved_file(graph, *(output_arg + 1),
                                                         loaded.editor_layout)
            : pulp::host::GraphSerializer::to_file(graph, *(output_arg + 1), loaded.editor_layout);
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

int canonicalize_graph(const std::string& path, const pulp::host::SignalGraph& graph,
                       const pulp::host::GraphSerializer::LoadResult& loaded, bool saved_identity,
                       bool json) {
    if (!loaded.missing_plugins.empty()) {
        if (json) {
            json_error("unresolved_node", "cannot canonicalize while plugin nodes are "
                                          "unresolved; resolve all plugins first");
            return 1;
        }
        std::cerr << "pulp graph: cannot canonicalize while plugin nodes are unresolved; "
                     "resolve all plugins first\n";
        return 1;
    }
    if (!loaded.state_restore_failures.empty()) {
        if (json) {
            json_error("RestoreFailed", std::to_string(loaded.state_restore_failures.size()) +
                                            " plugin(s) rejected their persisted state; "
                                            "canonicalizing would overwrite it");
            return 1;
        }
        std::cerr << "pulp graph: cannot canonicalize because "
                  << loaded.state_restore_failures.size()
                  << " plugin(s) rejected their persisted state; canonicalizing would "
                     "overwrite it\n";
        return 1;
    }
    const auto saved =
        saved_identity ? pulp::host::GraphSerializer::to_saved_file(
                             graph, std::filesystem::path(path), loaded.editor_layout)
                       : pulp::host::GraphSerializer::to_file(graph, std::filesystem::path(path),
                                                              loaded.editor_layout);
    if (!saved.ok) {
        if (json) {
            json_error(pulp::host::GraphSerializer::save_code_name(saved.code), saved.error);
            return 1;
        }
        std::cerr << "pulp graph: cannot canonicalize graph ("
                  << pulp::host::GraphSerializer::save_code_name(saved.code) << "): " << saved.error
                  << '\n';
        return 1;
    }
    if (json)
        std::cout << "{\"valid\":true,\"code\":\"Ok\"}\n";
    return 0;
}

// Refuses probe-only flags and unknown subcommands for the single-document
// operations. Returns 0 when `op` may proceed.
int check_document_operation(const std::string& op, const std::vector<std::string>& args,
                             bool json) {
    for (const auto* flag : {"--plan", "--latency", "--node-loads"}) {
        if (has_flag(args, flag)) {
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
    return 0;
}
} // namespace

int cmd_graph(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        usage();
        return 0;
    }
    const std::string op = args[0];
    if (op == "catalog") {
        std::cout << pulp::host::GraphPatchCatalog{}.to_json() << '\n';
        return 0;
    }
    if (op == "diff")
        return diff_graphs(args);
    if (args.size() < 2) {
        usage();
        return 2;
    }
    const std::string path = args[1];
    if (op == "probe")
        return probe_graph(args);
    if (op == "report")
        return report_graph(args);
    const bool json = has_flag(args, "--json");
    if (const auto status = check_document_operation(op, args, json))
        return status;
    std::optional<RenderOptions> render_options;
    if (op == "render") {
        std::string code;
        std::string message;
        render_options = parse_render_options(args, code, message);
        if (!render_options) {
            if (json)
                json_error(code, message);
            else
                std::cerr << "pulp graph: " << message << '\n';
            return 2;
        }
    }
    LoadedGraph document;
    const bool notation = std::filesystem::path(path).extension() == ".pat";
    const bool as_notation = has_flag(args, "--notation");
    int load_status = 0;
    if (op == "validate" || notation || (op == "fmt" && as_notation))
        load_status = load_strict(path, json, document);
    else if (op == "save" || op == "fmt" || op == "canonicalize")
        load_status = load_for_rewrite(op, args, path, json, document);
    else
        load_status = load_tolerant(path, json, document);
    if (load_status != 0)
        return load_status;
    auto& graph = *document.graph;
    const auto& loaded = document.loaded;
    if (op == "fmt" && (notation || as_notation))
        return format_notation(graph, json);
    if (op == "validate")
        return print_validate(graph, json);
    if (op == "inspect" || op == "explain")
        return print_inspect(graph, loaded, json);
    if (op == "render")
        return render_graph(graph, loaded, *render_options, json);
    if (op == "save")
        return save_graph(args, graph, loaded, document.saved_identity, json);
    if (op == "canonicalize" || op == "fmt")
        return canonicalize_graph(path, graph, loaded, document.saved_identity, json);
    return 2;
}
