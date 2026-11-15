#include <pulp/host/graph_history.hpp>
#include <pulp/host/signal_graph_runtime.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <exception>
#include <map>
#include <new>
#include <optional>

namespace pulp::host {
namespace {

// Canonical text for one JSON value: object members sorted by name, array
// order kept, primitives spelled by choc's writer.
template <typename V> void append_canonical(const V& value, std::string& out) {
    if (value.isObject()) {
        std::vector<std::string> names;
        names.reserve(value.size());
        for (std::uint32_t i = 0; i < value.size(); ++i)
            names.emplace_back(value.getObjectMemberAt(i).name);
        std::sort(names.begin(), names.end());
        out.push_back('{');
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i)
                out.push_back(',');
            out += choc::json::toString(choc::value::createString(names[i]), false);
            out.push_back(':');
            append_canonical(value[names[i]], out);
        }
        out.push_back('}');
        return;
    }
    if (value.isArray()) {
        out.push_back('[');
        for (std::uint32_t i = 0; i < value.size(); ++i) {
            if (i)
                out.push_back(',');
            append_canonical(value[i], out);
        }
        out.push_back(']');
        return;
    }
    out += choc::json::toString(value, false);
}

template <typename V> std::string canonical(const V& value) {
    std::string out;
    append_canonical(value, out);
    return out;
}

using Fields = std::map<std::string, std::string>;

struct NodeRecord {
    Fields fields;
    std::string layout;
};

struct DocumentView {
    std::map<std::string, NodeRecord> nodes;
    std::map<std::string, std::string> connections;
    std::map<std::string, std::string> sections;
    std::string digest;
};

bool is_identity_section(std::string_view name) {
    return name == "nodes" || name == "connections" || name == "processors" ||
           name == "identity_schema" || name == "logical_identities" || name == "bindings";
}

template <typename V> std::int64_t node_id_of(const V& value) {
    return value.isInt32() || value.isInt64() ? value.getInt64()
                                              : static_cast<std::int64_t>(value.getFloat64());
}

// Returns the refusal code for a side that cannot be compared, or nullopt.
std::optional<GraphDocumentDiffCode> read_document(std::string_view json, DocumentView& view,
                                                   GraphDocumentRefusal& refusal,
                                                   std::string& detail) {
    const auto inspection = GraphSerializer::inspect_document(json);
    if (!inspection.ok()) {
        refusal = inspection.refusal;
        detail = inspection.detail;
        return inspection.refusal == GraphDocumentRefusal::LogicalIdentityMissing
                   ? GraphDocumentDiffCode::LogicalIdentityMissing
                   : GraphDocumentDiffCode::BeforeRefused;
    }
    const auto root = choc::json::parse(json);
    if (!root.hasObjectMember("identity_schema")) {
        refusal = GraphDocumentRefusal::LogicalIdentityMissing;
        detail = "document has no saved-identity v6 envelope; re-save it with "
                 "GraphSerializer::to_saved_document";
        return GraphDocumentDiffCode::LogicalIdentityMissing;
    }
    view.digest = std::string(inspection.document.semantic_digest());

    std::unordered_map<std::int64_t, std::string> identity_of;
    const auto& identities = root["logical_identities"]["identities"];
    for (std::uint32_t i = 0; i < identities.size(); ++i)
        identity_of.emplace(node_id_of(identities[i]["node_id"]),
                            std::string(identities[i]["id"].getString()));
    const auto identity = [&](const auto& value) {
        const auto id = node_id_of(value);
        const auto it = identity_of.find(id);
        return it != identity_of.end() ? it->second : "unbound:" + std::to_string(id);
    };

    if (root.hasObjectMember("nodes") && root["nodes"].isArray()) {
        const auto& nodes = root["nodes"];
        for (std::uint32_t i = 0; i < nodes.size(); ++i) {
            const auto& node = nodes[i];
            auto& record = view.nodes[identity(node["id"])];
            for (std::uint32_t m = 0; m < node.size(); ++m) {
                const auto member = node.getObjectMemberAt(m);
                const std::string name(member.name);
                if (name == "id")
                    continue;
                if (name == "layout")
                    record.layout = canonical(member.value);
                else
                    record.fields[name] = canonical(member.value);
            }
        }
    }
    if (root.hasObjectMember("processors") && root["processors"].isArray()) {
        const auto& processors = root["processors"];
        for (std::uint32_t i = 0; i < processors.size(); ++i) {
            const auto& processor = processors[i];
            std::string text = "{";
            std::vector<std::string> names;
            for (std::uint32_t m = 0; m < processor.size(); ++m)
                if (std::string_view(processor.getObjectMemberAt(m).name) != "node_id")
                    names.emplace_back(processor.getObjectMemberAt(m).name);
            std::sort(names.begin(), names.end());
            for (std::size_t n = 0; n < names.size(); ++n) {
                if (n)
                    text.push_back(',');
                text += choc::json::toString(choc::value::createString(names[n]), false);
                text.push_back(':');
                append_canonical(processor[names[n]], text);
            }
            text.push_back('}');
            view.nodes[identity(processor["node_id"])].fields["processor"] = std::move(text);
        }
    }
    if (root.hasObjectMember("connections") && root["connections"].isArray()) {
        std::map<std::string, std::vector<std::string>> grouped;
        const auto& connections = root["connections"];
        for (std::uint32_t i = 0; i < connections.size(); ++i) {
            const auto& c = connections[i];
            const auto key = identity(c["source_node"]) + "[" +
                             std::to_string(node_id_of(c["source_port"])) + "] -> " +
                             identity(c["dest_node"]) + "[" +
                             std::to_string(node_id_of(c["dest_port"])) + "]";
            std::string attributes = "{";
            bool first = true;
            std::vector<std::string> names;
            for (std::uint32_t m = 0; m < c.size(); ++m) {
                const std::string_view name(c.getObjectMemberAt(m).name);
                if (name != "source_node" && name != "source_port" && name != "dest_node" &&
                    name != "dest_port")
                    names.emplace_back(name);
            }
            std::sort(names.begin(), names.end());
            for (const auto& name : names) {
                if (!first)
                    attributes.push_back(',');
                first = false;
                attributes += choc::json::toString(choc::value::createString(name), false);
                attributes.push_back(':');
                append_canonical(c[name], attributes);
            }
            attributes.push_back('}');
            grouped[key].push_back(std::move(attributes));
        }
        // Parallel connections between the same ports are told apart by
        // their sorted attributes, so the pairing is independent of order.
        for (auto& [key, attributes] : grouped) {
            std::sort(attributes.begin(), attributes.end());
            for (std::size_t i = 0; i < attributes.size(); ++i)
                view.connections.emplace(i == 0 ? key : key + " #" + std::to_string(i + 1),
                                         std::move(attributes[i]));
        }
    }
    for (std::uint32_t m = 0; m < root.size(); ++m) {
        const auto member = root.getObjectMemberAt(m);
        if (!is_identity_section(member.name))
            view.sections.emplace(std::string(member.name), canonical(member.value));
    }
    return std::nullopt;
}

template <typename Map, typename Changed>
void compare_maps(const Map& before, const Map& after, std::vector<std::string>& added,
                  std::vector<std::string>& removed, Changed&& changed) {
    for (const auto& [key, value] : after)
        if (!before.contains(key))
            added.push_back(key);
    for (const auto& [key, value] : before) {
        const auto it = after.find(key);
        if (it == after.end())
            removed.push_back(key);
        else
            changed(key, value, it->second);
    }
}

std::uint64_t fingerprint(const std::vector<PortHandle>& ports) {
    return port_identity_fingerprint(ports);
}

} // namespace

const char* graph_document_diff_code_name(GraphDocumentDiffCode code) noexcept {
    switch (code) {
    case GraphDocumentDiffCode::Ok:
        return "Ok";
    case GraphDocumentDiffCode::BeforeRefused:
        return "BeforeRefused";
    case GraphDocumentDiffCode::AfterRefused:
        return "AfterRefused";
    case GraphDocumentDiffCode::LogicalIdentityMissing:
        return "LogicalIdentityMissing";
    case GraphDocumentDiffCode::AllocationFailed:
        return "AllocationFailed";
    }
    return "AllocationFailed";
}

GraphDocumentDiff diff_graph_documents(std::string_view before, std::string_view after) {
    GraphDocumentDiff diff;
    try {
        DocumentView lhs, rhs;
        if (auto code = read_document(before, lhs, diff.refusal, diff.detail)) {
            diff.code = *code;
            diff.detail = "before: " + diff.detail;
            return diff;
        }
        if (auto code = read_document(after, rhs, diff.refusal, diff.detail)) {
            diff.code = *code == GraphDocumentDiffCode::BeforeRefused
                            ? GraphDocumentDiffCode::AfterRefused
                            : *code;
            diff.detail = "after: " + diff.detail;
            return diff;
        }
        diff.before_digest = lhs.digest;
        diff.after_digest = rhs.digest;
        compare_maps(lhs.nodes, rhs.nodes, diff.nodes_added, diff.nodes_removed,
                     [&](const std::string& identity, const NodeRecord& a, const NodeRecord& b) {
                         GraphNodeChange change{identity, {}};
                         std::vector<std::string> added, removed;
                         compare_maps(a.fields, b.fields, added, removed,
                                      [&](const std::string& name, const std::string& x,
                                          const std::string& y) {
                                          if (x != y)
                                              change.fields.push_back(name);
                                      });
                         change.fields.insert(change.fields.end(), added.begin(), added.end());
                         change.fields.insert(change.fields.end(), removed.begin(), removed.end());
                         std::sort(change.fields.begin(), change.fields.end());
                         if (!change.fields.empty())
                             diff.nodes_changed.push_back(std::move(change));
                         if (a.layout != b.layout)
                             diff.layout_changed.push_back(identity);
                     });
        compare_maps(lhs.connections, rhs.connections, diff.connections_added,
                     diff.connections_removed,
                     [&](const std::string& key, const std::string& a, const std::string& b) {
                         if (a != b)
                             diff.connections_changed.push_back(key);
                     });
        std::vector<std::string> sections_added, sections_removed;
        compare_maps(lhs.sections, rhs.sections, sections_added, sections_removed,
                     [&](const std::string& name, const std::string& a, const std::string& b) {
                         if (a != b)
                             diff.sections_changed.push_back(name);
                     });
        diff.sections_changed.insert(diff.sections_changed.end(), sections_added.begin(),
                                     sections_added.end());
        diff.sections_changed.insert(diff.sections_changed.end(), sections_removed.begin(),
                                     sections_removed.end());
        std::sort(diff.sections_changed.begin(), diff.sections_changed.end());
        diff.code = GraphDocumentDiffCode::Ok;
        return diff;
    } catch (const std::bad_alloc&) {
        diff = {};
        diff.code = GraphDocumentDiffCode::AllocationFailed;
        diff.refusal = GraphDocumentRefusal::AllocationFailed;
        diff.detail = "allocation failed while comparing documents";
        return diff;
    } catch (const std::exception& error) {
        diff = {};
        diff.code = GraphDocumentDiffCode::BeforeRefused;
        diff.refusal = GraphDocumentRefusal::InvalidJson;
        diff.detail = error.what();
        return diff;
    }
}

std::vector<PortHandle> qualified_port_identities(const SignalGraph& graph) {
    const auto identities = GraphSerializer::saved_node_identities(graph);
    std::vector<PortHandle> ports;
    for (const auto& node : graph.nodes()) {
        const auto it = identities.find(node.id);
        if (it == identities.end())
            continue;
        for (const auto* list : {&node.input_ports, &node.output_ports}) {
            for (const auto& port : *list) {
                auto qualified = port;
                qualified.name = it->second + (port.output ? "/out/" : "/in/") +
                                 (port.name.empty() ? "#" + std::to_string(port.index) : port.name);
                ports.push_back(std::move(qualified));
            }
        }
    }
    return ports;
}

const char* graph_history_code_name(GraphHistoryCode code) noexcept {
    switch (code) {
    case GraphHistoryCode::Ok:
        return "Ok";
    case GraphHistoryCode::NoChange:
        return "NoChange";
    case GraphHistoryCode::NothingToUndo:
        return "NothingToUndo";
    case GraphHistoryCode::NothingToRedo:
        return "NothingToRedo";
    case GraphHistoryCode::CaptureFailed:
        return "CaptureFailed";
    case GraphHistoryCode::Diverged:
        return "Diverged";
    case GraphHistoryCode::RestoreFailed:
        return "RestoreFailed";
    case GraphHistoryCode::PortIdentityMismatch:
        return "PortIdentityMismatch";
    }
    return "CaptureFailed";
}

GraphEditHistory::GraphEditHistory(std::size_t capacity)
    : capacity_(std::max<std::size_t>(1, capacity)) {}

GraphEditCheckpoint GraphEditHistory::checkpoint(const SignalGraph& graph,
                                                 const GraphEditorLayout& editor_layout) {
    GraphEditCheckpoint result;
    result.document = GraphSerializer::to_saved_document(graph, editor_layout);
    if (result.document.empty())
        return result;
    const auto ports = qualified_port_identities(graph);
    result.ports = snapshot_port_identity(ports);
    return result;
}

GraphHistoryResult GraphEditHistory::commit(GraphEditCheckpoint before, const SignalGraph& graph,
                                            std::string label,
                                            const GraphEditorLayout& editor_layout) {
    GraphHistoryResult result;
    result.label = label;
    if (!before.valid()) {
        result.detail = "the pre-edit checkpoint is not a valid saved document";
        return result;
    }
    auto after = checkpoint(graph, editor_layout);
    if (!after.valid()) {
        result.detail = "the edited graph cannot be captured as a saved document with valid "
                        "authored port names";
        return result;
    }
    const auto diff = diff_graph_documents(before.document, after.document);
    if (!diff.ok()) {
        result.detail = diff.detail;
        return result;
    }
    if (diff.equal() && diff.layout_changed.empty()) {
        result.code = GraphHistoryCode::NoChange;
        return result;
    }
    result.port_changes = diff_port_identities(before.ports.before, after.ports.before);
    undo_.push_back({std::move(label), std::move(before), std::move(after)});
    redo_.clear();
    while (undo_.size() > capacity_)
        undo_.pop_front();
    result.code = GraphHistoryCode::Ok;
    return result;
}

GraphHistoryResult GraphEditHistory::undo(SignalGraph& graph, const GraphLoadOptions& options) {
    return step(graph, options, true);
}

GraphHistoryResult GraphEditHistory::redo(SignalGraph& graph, const GraphLoadOptions& options) {
    return step(graph, options, false);
}

GraphHistoryResult GraphEditHistory::step(SignalGraph& graph, const GraphLoadOptions& options,
                                          bool undo) {
    auto& from = undo ? undo_ : redo_;
    auto& to = undo ? redo_ : undo_;
    GraphHistoryResult result;
    if (from.empty()) {
        result.code = undo ? GraphHistoryCode::NothingToUndo : GraphHistoryCode::NothingToRedo;
        return result;
    }
    const auto& entry = from.back();
    result.label = entry.label;
    const auto& expected = undo ? entry.after : entry.before;
    const auto& target = undo ? entry.before : entry.after;

    const auto current = GraphSerializer::to_saved_document(graph);
    const auto current_ports = qualified_port_identities(graph);
    if (current.empty() || !validate_port_identities(current_ports).valid) {
        result.code = GraphHistoryCode::CaptureFailed;
        result.detail = "the live graph cannot be captured as a saved document";
        return result;
    }
    const auto divergence = diff_graph_documents(current, expected.document);
    if (!divergence.ok()) {
        result.code = GraphHistoryCode::CaptureFailed;
        result.detail = divergence.detail;
        return result;
    }
    if (!divergence.equal()) {
        result.code = GraphHistoryCode::Diverged;
        result.detail =
            "the graph was edited outside this history (" +
            std::to_string(divergence.nodes_added.size() + divergence.nodes_removed.size() +
                           divergence.nodes_changed.size()) +
            " node and " +
            std::to_string(divergence.connections_added.size() +
                           divergence.connections_removed.size() +
                           divergence.connections_changed.size()) +
            " connection differences); commit that edit first or clear the history";
        return result;
    }
    const auto inspection = GraphSerializer::inspect_document(target.document);
    if (!inspection.ok()) {
        result.code = GraphHistoryCode::RestoreFailed;
        result.detail =
            std::string(graph_document_refusal_name(inspection.refusal)) + ": " + inspection.detail;
        return result;
    }

    const auto rollback = [&](GraphHistoryCode code, std::string detail) {
        result.code = code;
        result.detail = std::move(detail);
        result.rolled_back = GraphSerializer::from_saved_document(graph, current, options).ok;
        return result;
    };
    const auto loaded = GraphSerializer::from_saved_document(graph, target.document, options);
    if (!loaded.ok)
        return rollback(GraphHistoryCode::RestoreFailed, loaded.error);
    if (!loaded.state_restore_failures.empty() || !loaded.missing_custom_node_types.empty() ||
        (options.load_plugins && !loaded.missing_plugins.empty()))
        return rollback(GraphHistoryCode::RestoreFailed,
                        "a node or its state could not be restored faithfully");
    const auto restored_ports = qualified_port_identities(graph);
    if (fingerprint(restored_ports) != fingerprint(target.ports.before))
        return rollback(GraphHistoryCode::PortIdentityMismatch,
                        "the restored graph's authored ports differ from the recorded snapshot");

    result.port_changes = diff_port_identities(current_ports, restored_ports);
    result.editor_layout = loaded.editor_layout;
    result.code = GraphHistoryCode::Ok;
    to.push_back(std::move(from.back()));
    from.pop_back();
    return result;
}

std::string_view GraphEditHistory::undo_label() const noexcept {
    return undo_.empty() ? std::string_view{} : std::string_view(undo_.back().label);
}

std::string_view GraphEditHistory::redo_label() const noexcept {
    return redo_.empty() ? std::string_view{} : std::string_view(redo_.back().label);
}

void GraphEditHistory::clear() noexcept {
    undo_.clear();
    redo_.clear();
}

} // namespace pulp::host
