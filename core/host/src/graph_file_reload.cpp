#include <pulp/host/graph_file_reload.hpp>
#include <pulp/format/processor_node_adapter.hpp>

#include <unordered_map>
#include <unordered_set>
#include <fstream>

namespace pulp::host {
GraphFileChange GraphFileWatch::poll() {
    using Code = GraphFileChange::Code;
    auto error = [&](Code code) {
        pending_.reset();
        if (previous_error_ == code) return GraphFileChange{};
        previous_error_ = code;
        return GraphFileChange{.code = code};
    };
    const auto extension = path_.extension();
    if (extension != ".pat" && extension != ".pulpgraph")
        return error(Code::UnsupportedFormat);
    std::ifstream file(path_, std::ios::binary);
    if (!file) return error(Code::Unreadable);
    constexpr std::size_t limit = 1024 * 1024;
    std::string text(limit + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto bytes = file.gcount();
    if (file.bad()) return error(Code::Unreadable);
    if (bytes > static_cast<std::streamsize>(limit)) return error(Code::TooLarge);
    text.resize(static_cast<std::size_t>(bytes));
    previous_error_ = Code::Unchanged;
    if (observed_ && text == *observed_) {
        pending_.reset();
        return {};
    }
    if (!pending_ || text != *pending_) {
        pending_ = std::move(text);
        return {.code = Code::Pending};
    }
    observed_ = text;
    pending_.reset();
    return {.code = Code::Changed,
            .format = extension == ".pat" ? GraphFileFormat::Notation : GraphFileFormat::Pulpgraph,
            .text = std::move(text)};
}

const char* graph_file_reload_code_name(GraphFileReloadCode code) noexcept {
    switch (code) {
    case GraphFileReloadCode::Prepared: return "prepared";
    case GraphFileReloadCode::Committed: return "committed";
    case GraphFileReloadCode::InvalidDocument: return "invalid-document";
    case GraphFileReloadCode::UnsupportedContract: return "unsupported-contract";
    case GraphFileReloadCode::InvalidMutation: return "invalid-mutation";
    case GraphFileReloadCode::PrepareFailed: return "prepare-failed";
    case GraphFileReloadCode::StaleBase: return "stale-base";
    case GraphFileReloadCode::CommitFailed: return "commit-failed";
    }
    return "invalid-document";
}

GraphFileReloadResult GraphFileReloadTransaction::prepare(
    std::string_view text, GraphFileFormat format, const GraphPatchCatalog& catalog,
    double sample_rate, int maximum_block_size) {
    using Code = GraphFileReloadCode;
    using Topology = SignalGraph::PreparedTopologyEdit::Result;
    if (attempted_)
        return {.code = Code::InvalidMutation};
    attempted_ = true;
    if (text.size() > 1024u * 1024u)
        return {.code = Code::InvalidDocument,
                .diagnostic = {.code = GraphPatchDiagnostic::Code::ResourceLimit,
                               .rule = "graph file exceeds 1 MiB",
                               .suggested_fix = "reduce the graph file to 1 MiB or less"}};
    edit_ = graph_.begin_prepared_topology_edit();
    if (!edit_)
        return {.code = Code::PrepareFailed};
    auto parsed = format == GraphFileFormat::Notation ? parse_graph_patch(text, catalog)
                                                      : load_graph_patch_json(text, catalog);
    if (!parsed)
        return {.code = Code::InvalidDocument, .diagnostic = std::move(parsed.diagnostic)};
    // Strict saved loading can retain richer state than notation represents.
    // Refuse those contracts before constructing a replacement, never erase them.
    auto representable = format_graph_patch(*parsed.graph, catalog);
    if (!representable)
        return {.code = Code::UnsupportedContract,
                .diagnostic = std::move(representable.diagnostic)};
    try {
        const auto edges = parsed.graph->connections();
        const auto nodes = parsed.graph->nodes();
        const auto baseline = edit_->nodes();
        for (const auto& node : baseline)
            if (!edit_->remove_node(node.id))
                return {.code = Code::InvalidMutation};
        std::unordered_map<NodeId, NodeId> ids;
        std::unordered_set<std::string> registered;
        for (const auto& node : nodes) {
            NodeId id = 0;
            switch (node.type) {
            case NodeType::AudioInput: id = edit_->add_input_node(node.num_output_ports, node.name); break;
            case NodeType::AudioOutput: id = edit_->add_output_node(node.num_input_ports, node.name); break;
            case NodeType::Gain:
                id = edit_->add_gain_node(node.name);
                if (id && !edit_->set_node_gain(id, node.gain))
                    return {.code = Code::InvalidMutation};
                break;
            case NodeType::MidiInput: id = edit_->add_midi_input_node(node.name); break;
            case NodeType::MidiOutput: id = edit_->add_midi_output_node(node.name); break;
            case NodeType::Plugin: {
                auto instance = parsed.graph->processor_node_instance(node.id);
                if (!instance)
                    return {.code = Code::UnsupportedContract};
                // Drop the staging graph's lifetime before preparing this fresh
                // instance, so it cannot later release the published processor.
                if (!parsed.graph->remove_node(node.id))
                    return {.code = Code::InvalidMutation};
                id = edit_->add_processor_node(std::move(instance), node.name);
                break;
            }
            case NodeType::Custom: {
                const auto* type = parsed.graph->custom_node_type(node.custom_type_id,
                                                                  node.custom_type_version);
                if (!type)
                    return {.code = Code::UnsupportedContract};
                const auto key = type->type_id + "@" + std::to_string(type->version);
                if (registered.insert(key).second &&
                    !edit_->reuse_or_register_custom_node_type_(*type))
                    return {.code = Code::UnsupportedContract,
                            .diagnostic = {.code = GraphPatchDiagnostic::Code::UnsupportedContract,
                                .node = node.name,
                                .rule = "Custom registration differs or callback identity is opaque",
                                .suggested_fix = "use the same raw callbacks or a new type/version"}};
                id = edit_->add_custom_node(node.custom_type_id, node.custom_type_version, node.name);
                break;
            }
            default: return {.code = Code::UnsupportedContract};
            }
            if (!id)
                return {.code = Code::InvalidMutation};
            ids.emplace(node.id, id);
        }
        parsed.graph.reset();
        for (const auto& edge : edges) {
            const auto source = ids.at(edge.source_node), destination = ids.at(edge.dest_node);
            const bool ok = edge.midi ? edit_->connect_midi(source, destination)
                : edge.automation ? edit_->connect_automation(source, edge.source_port, destination,
                    edge.automation_param_id, edge.automation_range_lo, edge.automation_range_hi, edge.automation_smoothing_ms, edge.automation_mix)
                : edit_->connect(source, edge.source_port, destination, edge.dest_port);
            if (!ok)
                return {.code = Code::InvalidMutation};
        }
        const auto result = edit_->prepare(sample_rate, maximum_block_size);
        if (result != Topology::Prepared)
            return {.code = result == Topology::StaleBase ? Code::StaleBase : Code::PrepareFailed,
                    .topology_result = result};
        if (!edit_->routed_execution_ready(maximum_block_size))
            return {.code = Code::PrepareFailed, .topology_result = result};
        ready_ = true;
        return {.code = Code::Prepared, .topology_result = result};
    } catch (...) {
        edit_.reset();
        return {.code = Code::PrepareFailed};
    }
}

GraphFileReloadResult GraphFileReloadTransaction::commit() {
    using Topology = SignalGraph::PreparedTopologyEdit::Result;
    if (!edit_ || !ready_)
        return {.code = GraphFileReloadCode::CommitFailed};
    ready_ = false;
    const auto result = edit_->commit();
    return {.code = result == Topology::Committed ? GraphFileReloadCode::Committed
                    : result == Topology::StaleBase ? GraphFileReloadCode::StaleBase
                                                    : GraphFileReloadCode::CommitFailed,
            .topology_result = result};
}
} // namespace pulp::host
