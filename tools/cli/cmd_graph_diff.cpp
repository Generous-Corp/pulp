// `pulp graph diff`: compare two saved-identity documents by logical node
// identity. Read-only; neither input is modified.

#include "cmd_graph_internal.hpp"

#include <algorithm>
#include <choc/text/choc_JSON.h>
#include <iostream>
#include <pulp/host/graph_history.hpp>
#include <string>
#include <utility>
#include <vector>

namespace pulp_cli::graph_detail {
namespace {

choc::value::Value string_array(const std::vector<std::string>& values) {
    auto array = choc::value::createEmptyArray();
    for (const auto& value : values)
        array.addArrayElement(value);
    return array;
}

// Collects the two positional paths; any other `--` option is refused.
int parse_diff_paths(const std::vector<std::string>& args, bool json,
                     std::vector<std::string>& paths) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--json")
            continue;
        if (args[i].starts_with("--")) {
            if (json)
                json_error("invalid_option", "unknown diff option: " + args[i]);
            else
                std::cerr << "pulp graph: unknown diff option: " << args[i] << '\n';
            return 2;
        }
        paths.push_back(args[i]);
    }
    if (paths.size() != 2) {
        if (json)
            json_error("missing_input", "diff requires exactly two graph files: BEFORE AFTER");
        else
            std::cerr << "pulp graph: diff requires exactly two graph files: BEFORE AFTER\n";
        return 2;
    }
    return 0;
}

int report_diff_refusal(const pulp::host::GraphDocumentDiff& diff,
                        const std::vector<std::string>& paths, bool json) {
    const bool after = diff.code == pulp::host::GraphDocumentDiffCode::AfterRefused ||
                       diff.detail.starts_with("after:");
    // A plain graph (any format) has no envelope: the inspector refuses
    // a v4+ one as missing it and an older one by format version.
    const bool missing_identity =
        diff.refusal == pulp::host::GraphDocumentRefusal::LogicalIdentityMissing ||
        diff.refusal == pulp::host::GraphDocumentRefusal::InvalidFormatVersion;
    const std::string resave =
        "write the identity envelope with: pulp graph save FILE --output FILE "
        "--saved-identity";
    if (json) {
        // The document refusal is the C++ InvalidDocument class; the
        // inspection refusal and the side name the exact cause.
        auto value = choc::value::createObject("GraphError");
        value.addMember("valid", false);
        value.addMember("code", patch_code(Code::InvalidDocument));
        value.addMember("refusal", pulp::host::graph_document_refusal_name(diff.refusal));
        value.addMember("side", after ? "after" : "before");
        value.addMember("message", diff.detail);
        if (missing_identity)
            value.addMember("hint", resave);
        std::cout << choc::json::toString(value) << '\n';
    } else {
        std::cerr << "pulp graph: cannot diff (" << (after ? paths[1] : paths[0])
                  << "): " << diff.detail << '\n';
        if (missing_identity)
            std::cerr << "pulp graph: " << resave << '\n';
    }
    return 1;
}

void print_diff_json(const pulp::host::GraphDocumentDiff& diff) {
    auto result = choc::value::createObject("GraphDiff");
    result.addMember("valid", true);
    result.addMember("code", patch_code(Code::Ok));
    result.addMember("equal", diff.equal());
    result.addMember("before_digest", diff.before_digest);
    result.addMember("after_digest", diff.after_digest);
    auto nodes = choc::value::createObject("Nodes");
    nodes.addMember("added", string_array(diff.nodes_added));
    nodes.addMember("removed", string_array(diff.nodes_removed));
    auto changed = choc::value::createEmptyArray();
    for (const auto& change : diff.nodes_changed) {
        auto item = choc::value::createObject("NodeChange");
        item.addMember("identity", change.identity);
        item.addMember("fields", string_array(change.fields));
        changed.addArrayElement(item);
    }
    nodes.addMember("changed", changed);
    result.addMember("nodes", nodes);
    auto connections = choc::value::createObject("Connections");
    connections.addMember("added", string_array(diff.connections_added));
    connections.addMember("removed", string_array(diff.connections_removed));
    connections.addMember("changed", string_array(diff.connections_changed));
    result.addMember("connections", connections);
    auto events = choc::value::createObject("EventConnections");
    events.addMember("added", string_array(diff.event_connections_added));
    events.addMember("removed", string_array(diff.event_connections_removed));
    events.addMember("changed", string_array(diff.event_connections_changed));
    result.addMember("event_connections", events);
    auto assets = choc::value::createObject("SpectralAssets");
    assets.addMember("added", string_array(diff.spectral_assets_added));
    assets.addMember("removed", string_array(diff.spectral_assets_removed));
    assets.addMember("changed", string_array(diff.spectral_assets_changed));
    result.addMember("spectral_assets", assets);
    result.addMember("layout_changed", string_array(diff.layout_changed));
    result.addMember("sections_changed", string_array(diff.sections_changed));
    std::cout << choc::json::toString(result) << '\n';
}

void print_diff_text(const pulp::host::GraphDocumentDiff& diff) {
    for (const auto& id : diff.nodes_added)
        std::cout << "+ node " << id << '\n';
    for (const auto& id : diff.nodes_removed)
        std::cout << "- node " << id << '\n';
    for (const auto& change : diff.nodes_changed) {
        std::cout << "~ node " << change.identity << " (";
        for (std::size_t i = 0; i < change.fields.size(); ++i)
            std::cout << (i ? ", " : "") << change.fields[i];
        std::cout << ")\n";
    }
    for (const auto& key : diff.connections_added)
        std::cout << "+ connection " << key << '\n';
    for (const auto& key : diff.connections_removed)
        std::cout << "- connection " << key << '\n';
    for (const auto& key : diff.connections_changed)
        std::cout << "~ connection " << key << '\n';
    for (const auto& key : diff.event_connections_added)
        std::cout << "+ event connection " << key << '\n';
    for (const auto& key : diff.event_connections_removed)
        std::cout << "- event connection " << key << '\n';
    for (const auto& key : diff.event_connections_changed)
        std::cout << "~ event connection " << key << '\n';
    for (const auto& id : diff.spectral_assets_added)
        std::cout << "+ spectral asset " << id << '\n';
    for (const auto& id : diff.spectral_assets_removed)
        std::cout << "- spectral asset " << id << '\n';
    for (const auto& id : diff.spectral_assets_changed)
        std::cout << "~ spectral asset " << id << '\n';
    for (const auto& name : diff.sections_changed)
        std::cout << "~ section " << name << '\n';
    for (const auto& id : diff.layout_changed)
        std::cout << "  layout " << id << '\n';
    if (diff.equal())
        std::cout << "no semantic differences\n";
}

} // namespace

int diff_graphs(const std::vector<std::string>& args) {
    const bool json = std::find(args.begin() + 1, args.end(), "--json") != args.end();
    std::vector<std::string> paths;
    if (const auto status = parse_diff_paths(args, json, paths))
        return status;
    std::string documents[2];
    for (std::size_t i = 0; i < 2; ++i) {
        Code code = Code::Ok;
        std::string message;
        auto text = read_graph_document(paths[i], code, message);
        if (!text) {
            if (json)
                json_error(patch_code(code), message);
            else
                std::cerr << "pulp graph: " << message << '\n';
            return 1;
        }
        documents[i] = std::move(*text);
    }
    const auto diff = pulp::host::diff_graph_documents(documents[0], documents[1]);
    if (!diff.ok())
        return report_diff_refusal(diff, paths, json);
    if (json)
        print_diff_json(diff);
    else
        print_diff_text(diff);
    return 0;
}

} // namespace pulp_cli::graph_detail
