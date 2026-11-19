// GraphSerializer save path: picks the writer for a graph (plain, spectral or
// the saved-identity v7 envelope), builds the plain and saved-identity
// documents, writes .pulpgraph files through a durable replacement and keeps
// the legacy v6 writer that GraphSerializerTestAccess exposes to tests.

#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/isolated_plugin_slot.hpp>
#include <pulp/runtime/detail/durable_file_replacement.hpp>

#include "graph_serializer_internal.hpp"
#include "graph_serializer_regions.hpp"
#include "graph_serializer_test_access.hpp"
#include "spectral_graph_persistence.hpp"

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulp::host {
namespace {

using detail::format_str;
using detail::kMaxSerializedConnections;
using detail::kMaxSerializedNodes;
using detail::kMaxSerializedSampleRegionConnections;
using detail::kSavedIdentityV6;
using detail::kSavedIdentityV7;
using detail::kSavedIdentityV7FormatVersion;
using detail::node_type_str;
using detail::structural_node_identities;
using detail::valid_authored_id;

std::atomic<bool> g_fail_next_write{false};
std::atomic<bool> g_fail_next_replace{false};

// Minimal base64 encoder (no padding stripping), good enough for state
// blobs in JSON; choc::json escapes inline strings already.
std::string b64_encode(const std::vector<uint8_t>& bytes) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        uint32_t v =
            (uint32_t)bytes[i] << 16 | (uint32_t)bytes[i + 1] << 8 | (uint32_t)bytes[i + 2];
        out.push_back(kAlphabet[(v >> 18) & 0x3f]);
        out.push_back(kAlphabet[(v >> 12) & 0x3f]);
        out.push_back(kAlphabet[(v >> 6) & 0x3f]);
        out.push_back(kAlphabet[v & 0x3f]);
    }
    if (i < bytes.size()) {
        uint32_t v = (uint32_t)bytes[i] << 16;
        if (i + 1 < bytes.size())
            v |= (uint32_t)bytes[i + 1] << 8;
        out.push_back(kAlphabet[(v >> 18) & 0x3f]);
        out.push_back(kAlphabet[(v >> 12) & 0x3f]);
        out.push_back(i + 1 < bytes.size() ? kAlphabet[(v >> 6) & 0x3f] : '=');
        out.push_back('=');
    }
    return out;
}

} // namespace

void GraphSerializerTestAccess::fail_next_write() noexcept {
    g_fail_next_write.store(true, std::memory_order_release);
}

void GraphSerializerTestAccess::fail_next_replace() noexcept {
    g_fail_next_replace.store(true, std::memory_order_release);
}

namespace {
std::string serialize(const SignalGraph& graph,
                      const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout,
                      bool saved_identity, std::string* reason);

GraphSerializer::SaveResult
write_graph_file(const SignalGraph& graph, const std::filesystem::path& path,
                 const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout,
                 bool saved_identity) {
    using SaveResult = GraphSerializer::SaveResult;
    constexpr auto max_file_bytes = GraphSerializer::max_file_bytes;
    SaveResult result;
    for (const auto& node : graph.nodes()) {
        if (node.type == NodeType::Custom &&
            graph.custom_node_type(node.custom_type_id, node.custom_type_version) == nullptr) {
            result.code = SaveResult::Code::serialization_refused;
            result.error = "graph contains an unresolved custom node type: " + node.custom_type_id +
                           "@" + std::to_string(node.custom_type_version);
            return result;
        }
        if (node.type == NodeType::Plugin && node.plugin && is_isolated_plugin_slot(*node.plugin)) {
            result.code = SaveResult::Code::serialization_refused;
            result.error =
                "graph contains an isolated plugin node, which cannot be saved: " + node.name;
            return result;
        }
    }
    std::string reason;
    const auto json = serialize(graph, editor_layout, saved_identity, &reason);
    if (json.empty()) {
        result.code = SaveResult::Code::serialization_refused;
        result.error = saved_identity ? "graph saved-identity serialization failed"
                                      : "graph serialization failed";
        if (!reason.empty())
            result.error += ": " + reason;
        return result;
    }
    const auto payload_size = json.size() + 1; // canonical files end with a newline
    if (payload_size > max_file_bytes) {
        result.code = SaveResult::Code::file_too_large;
        result.error = "graph file exceeds " + std::to_string(max_file_bytes) + " byte limit";
        return result;
    }

    std::error_code ec;
    const auto parent =
        path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    if (!std::filesystem::is_directory(parent, ec)) {
        result.code = SaveResult::Code::parent_unavailable;
        result.error = "graph file destination directory is unavailable: " + parent.string();
        return result;
    }

    using pulp::runtime::detail::DurableFileCommitOutcome;
    using pulp::runtime::detail::DurableFileReplacement;
    auto replacement = DurableFileReplacement::create(path);
    if (!replacement) {
        result.code = SaveResult::Code::temporary_unavailable;
        result.error = "could not create a temporary graph file for " + path.string();
        return result;
    }
    const auto bytes = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(json.data()), json.size()};
    if (g_fail_next_write.exchange(false, std::memory_order_acq_rel) ||
        !replacement->write_all(bytes) ||
        !replacement->write_all(
            std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>("\n"), 1})) {
        replacement->cancel();
        result.code = SaveResult::Code::write_failed;
        result.error = "graph file write failed: " + path.string();
        return result;
    }
    if (g_fail_next_replace.exchange(false, std::memory_order_acq_rel)) {
        replacement->cancel();
        result.code = SaveResult::Code::replace_failed;
        result.error = "graph file replace failed: " + path.string();
        return result;
    }
    switch (replacement->commit()) {
    case DurableFileCommitOutcome::ReplacedDurably:
        result.ok = true;
        result.code = SaveResult::Code::ok;
        return result;
    case DurableFileCommitOutcome::ReplacedButDirectorySyncFailed:
        result.code = SaveResult::Code::directory_sync_failed;
        result.error = "graph file replaced but parent directory sync failed: " + path.string();
        return result;
    case DurableFileCommitOutcome::NotReplaced:
        result.code = SaveResult::Code::replace_failed;
        result.error = "graph file replace failed: " + path.string();
        return result;
    }
    result.code = SaveResult::Code::replace_failed;
    result.error = "graph file replace failed: " + path.string();
    return result;
}
} // namespace

GraphSerializer::SaveResult
GraphSerializer::to_file(const SignalGraph& graph, const std::filesystem::path& path,
                         const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout) {
    return write_graph_file(graph, path, editor_layout, false);
}

GraphSerializer::SaveResult GraphSerializer::to_saved_file(
    const SignalGraph& graph, const std::filesystem::path& path,
    const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout) {
    return write_graph_file(graph, path, editor_layout, true);
}

const char* GraphSerializer::save_code_name(SaveResult::Code code) noexcept {
    switch (code) {
    case SaveResult::Code::ok:
        return "ok";
    case SaveResult::Code::serialization_refused:
        return "serialization_refused";
    case SaveResult::Code::file_too_large:
        return "file_too_large";
    case SaveResult::Code::parent_unavailable:
        return "parent_unavailable";
    case SaveResult::Code::temporary_unavailable:
        return "temporary_unavailable";
    case SaveResult::Code::write_failed:
        return "write_failed";
    case SaveResult::Code::replace_failed:
        return "replace_failed";
    case SaveResult::Code::directory_sync_failed:
        return "directory_sync_failed";
    }
    return "serialization_refused";
}

namespace {
// The plain (non-spectral, no envelope) writer. serialize() owns the choice
// between it, the spectral writer and the saved-identity envelope.
std::string
plain_document(const SignalGraph& graph,
               const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout) {
    bool has_processors = false;
    bool has_typed_ports = false;
    for (const auto& node : graph.nodes()) {
        if (node.spectral_identity)
            return {};
        // An isolated proxy has no state to save (asking for it makes the node
        // terminal), and its identity alone would reload the plugin in-process.
        if (node.type == NodeType::Plugin && node.plugin && is_isolated_plugin_slot(*node.plugin))
            return {};
        has_processors |= graph.is_processor_node(node.id);
        if (node.type == NodeType::Custom) {
            // A node may carry fewer handles than ports (none at all for a
            // node built without named ports); a missing handle is a legacy
            // audio port and needs no typed record. More handles than ports
            // is a corrupt node.
            if (node.input_ports.size() >
                    static_cast<std::size_t>(std::max(0, node.num_input_ports)) ||
                node.output_ports.size() >
                    static_cast<std::size_t>(std::max(0, node.num_output_ports)))
                return {};
            const auto non_default = [](const auto& ports, bool output) {
                for (std::size_t i = 0; i < ports.size(); ++i) {
                    const auto name = i == 0
                                          ? std::string("audio")
                                          : std::string(output ? "out" : "in") + std::to_string(i);
                    if (ports[i].name != name || ports[i].kind != PortKind::Audio)
                        return true;
                }
                return false;
            };
            has_typed_ports |=
                non_default(node.input_ports, false) || non_default(node.output_ports, true);
        }
    }
    for (const auto& connection : graph.connections())
        has_typed_ports |= connection.control_signal;
    auto root = choc::value::createObject("PulpGraph");
    const auto regions = graph.sample_regions();
    if (graph.nodes().size() > kMaxSerializedNodes ||
        graph.connections().size() > kMaxSerializedConnections)
        return {};
    // Canonicalize collection order before emitting JSON. SignalGraph preserves
    // insertion order for editing, while the serialized contract is content
    // addressed: equivalent graphs must produce identical bytes regardless of
    // construction order.
    std::vector<const GraphNode*> canonical_nodes;
    canonical_nodes.reserve(graph.nodes().size());
    for (const auto& node : graph.nodes())
        canonical_nodes.push_back(&node);
    if (regions.empty())
        std::sort(canonical_nodes.begin(), canonical_nodes.end(),
                  [](const auto* lhs, const auto* rhs) { return lhs->id < rhs->id; });
    std::vector<const Connection*> canonical_connections;
    canonical_connections.reserve(graph.connections().size());
    for (const auto& connection : graph.connections())
        canonical_connections.push_back(&connection);
    if (regions.empty())
        std::sort(canonical_connections.begin(), canonical_connections.end(),
                  [](const auto* lhs, const auto* rhs) {
                      return std::tie(lhs->source_node, lhs->source_port, lhs->dest_node,
                                      lhs->dest_port, lhs->feedback, lhs->midi, lhs->automation,
                                      lhs->audio_rate_modulation, lhs->sidechain,
                                      lhs->control_signal, lhs->automation_param_id,
                                      lhs->automation_range_lo, lhs->automation_range_hi,
                                      lhs->automation_smoothing_ms, lhs->automation_mix) <
                             std::tie(rhs->source_node, rhs->source_port, rhs->dest_node,
                                      rhs->dest_port, rhs->feedback, rhs->midi, rhs->automation,
                                      rhs->audio_rate_modulation, rhs->sidechain,
                                      rhs->control_signal, rhs->automation_param_id,
                                      rhs->automation_range_lo, rhs->automation_range_hi,
                                      rhs->automation_smoothing_ms, rhs->automation_mix);
                  });
    std::vector<SampleRegionDefinition> canonical_regions(regions.begin(), regions.end());
    // Region records and their nested members remain authored sequences: their
    // order carries topology/control meaning across round trips.
    if (!regions.empty() && graph.connections().size() > kMaxSerializedSampleRegionConnections) {
        return {};
    }
    if (!regions.empty()) {
        SampleRegionParserShape shape{};
        shape.regions = regions.size();
        shape.connections_total = graph.connections().size();
        for (const auto& region : canonical_regions) {
            detail::accumulate_parser_region_shape(graph, region, shape);
            std::uint64_t region_connections = 0;
            for (const auto& connection : graph.connections()) {
                const auto touches = [&](NodeId id) {
                    return std::any_of(region.members.begin(), region.members.end(),
                                       [&](const auto& member) { return member.node == id; });
                };
                if (touches(connection.source_node) || touches(connection.dest_node))
                    ++region_connections;
            }
            shape.connections_per_region =
                std::max(shape.connections_per_region, region_connections);
        }
        if (!prove_sample_region_parser_shape(shape).accepted)
            return {};
    }
    // Preserve canonical bytes for existing ordinary and sample-region files;
    // v4 is selected by the future Processor writer, while the reader accepts
    // region-bearing v4 files through the migration above.
    const int version = has_typed_ports ? 5 : (has_processors ? 4 : (regions.empty() ? 2 : 3));
    root.addMember("format_version", (int64_t)version);

    // Nodes
    auto nodes_arr = choc::value::createEmptyArray();
    for (const auto* n_ptr : canonical_nodes) {
        const auto& n = *n_ptr;
        auto node_obj = choc::value::createObject("Node");
        node_obj.addMember("id", (int64_t)n.id);
        node_obj.addMember("type",
                           graph.is_processor_node(n.id) ? "processor" : node_type_str(n.type));
        node_obj.addMember("name", n.name);
        node_obj.addMember("num_input_ports", (int64_t)n.num_input_ports);
        node_obj.addMember("num_output_ports", (int64_t)n.num_output_ports);
        node_obj.addMember("gain", (double)n.gain);
        if (n.type == NodeType::Plugin) {
            // Always serialize the plugin identity (from GraphNode::plugin_info),
            // even when the slot itself failed to load on this machine. State
            // blob is omitted for unresolved plugins.
            const auto& info = n.plugin_info;
            auto plug_obj = choc::value::createObject("Plugin");
            plug_obj.addMember("format", std::string(format_str(info.format)));
            plug_obj.addMember("unique_id", info.unique_id);
            plug_obj.addMember("name", info.name);
            plug_obj.addMember("manufacturer", info.manufacturer);
            plug_obj.addMember("version", info.version);
            plug_obj.addMember("last_path", info.path);
            if (n.plugin) {
                plug_obj.addMember("state_b64", b64_encode(n.plugin->save_state()));
            }
            node_obj.addMember("plugin", plug_obj);
        }
        if (n.type == NodeType::Custom) {
            auto custom_obj = choc::value::createObject("CustomNode");
            custom_obj.addMember("type_id", n.custom_type_id);
            custom_obj.addMember("version",
                                 (int64_t)(n.custom_type_version > 0 ? n.custom_type_version : 1));
            if (has_typed_ports) {
                const auto port_array = [](const auto& ports, int count, bool output) {
                    auto array = choc::value::createEmptyArray();
                    for (int i = 0; i < count; ++i) {
                        auto item = choc::value::createObject("Port");
                        if (static_cast<std::size_t>(i) < ports.size()) {
                            item.addMember("name", ports[static_cast<std::size_t>(i)].name);
                            item.addMember("kind",
                                           (int64_t)ports[static_cast<std::size_t>(i)].kind);
                        } else {
                            item.addMember("name", i == 0 ? std::string("audio")
                                                          : std::string(output ? "out" : "in") +
                                                                std::to_string(i));
                            item.addMember("kind", (int64_t)PortKind::Audio);
                        }
                        array.addArrayElement(item);
                    }
                    return array;
                };
                custom_obj.addMember("input_ports",
                                     port_array(n.input_ports, n.num_input_ports, false));
                custom_obj.addMember("output_ports",
                                     port_array(n.output_ports, n.num_output_ports, true));
            }
            // Opaque custom-node state: the live instance's save_state(), or
            // the preserved blob for unresolved nodes. Omitted when empty so
            // stateless process-only nodes serialize byte-for-byte as before.
            if (auto state = graph.custom_node_state(n.id); !state.empty()) {
                custom_obj.addMember("state_b64", b64_encode(state));
            }
            node_obj.addMember("custom", custom_obj);
        }
        auto layout_it = editor_layout.find(n.id);
        if (layout_it != editor_layout.end()) {
            auto pos = choc::value::createObject("Pos");
            pos.addMember("x", (double)layout_it->second.first);
            pos.addMember("y", (double)layout_it->second.second);
            node_obj.addMember("layout", pos);
        }
        nodes_arr.addArrayElement(node_obj);
    }
    root.addMember("nodes", nodes_arr);

    if (has_processors) {
        auto processors = choc::value::createEmptyArray();
        for (const auto& n : graph.nodes()) {
            if (!graph.is_processor_node(n.id))
                continue;
            const auto instance = graph.processor_node_instance(n.id);
            if (!instance)
                return {};
            const auto& d = instance->descriptor();
            auto p = choc::value::createObject("Processor");
            p.addMember("node_id", (int64_t)n.id);
            p.addMember("type_id", instance->graph_type_id().empty()
                                       ? d.bundle_id
                                       : std::string(instance->graph_type_id()));
            p.addMember("type_version", (int64_t)instance->graph_type_version());
            p.addMember("state_schema_version", (int64_t)instance->graph_state_schema_version());
            p.addMember("input_channels", (int64_t)d.default_input_channels());
            p.addMember("output_channels", (int64_t)d.default_output_channels());
            p.addMember("parameters_b64", b64_encode(instance->state_store().serialize()));
            p.addMember("custom_b64", b64_encode(instance->processor().serialize_plugin_state()));
            processors.addArrayElement(p);
        }
        root.addMember("processors", processors);
    }

    // Connections
    auto conns_arr = choc::value::createEmptyArray();
    for (const auto* c_ptr : canonical_connections) {
        const auto& c = *c_ptr;
        auto co = choc::value::createObject("Conn");
        co.addMember("source_node", (int64_t)c.source_node);
        co.addMember("source_port", (int64_t)c.source_port);
        co.addMember("dest_node", (int64_t)c.dest_node);
        co.addMember("dest_port", (int64_t)c.dest_port);
        co.addMember("feedback", c.feedback);
        co.addMember("midi", c.midi);
        co.addMember("automation", c.automation);
        co.addMember("audio_rate_modulation", c.audio_rate_modulation);
        // Sidechain flag is additive; absent in older blobs.
        co.addMember("sidechain", c.sidechain);
        if (has_typed_ports)
            co.addMember("control_signal", c.control_signal);
        if (c.automation || c.audio_rate_modulation) {
            co.addMember("auto_param_id", (int64_t)c.automation_param_id);
            co.addMember("auto_range_lo", (double)c.automation_range_lo);
            co.addMember("auto_range_hi", (double)c.automation_range_hi);
            co.addMember("auto_smoothing", (double)c.automation_smoothing_ms);
            co.addMember("auto_mix", (int64_t)c.automation_mix);
        }
        conns_arr.addArrayElement(co);
    }
    root.addMember("connections", conns_arr);

    if (!regions.empty()) {
        auto regions_arr = choc::value::createEmptyArray();
        for (const auto& region : canonical_regions) {
            auto ro = choc::value::createObject("SampleRegion");
            ro.addMember("id", (int64_t)region.region_id);
            auto members = choc::value::createEmptyArray();
            for (const auto& member : region.members) {
                auto mo = choc::value::createObject("Member");
                mo.addMember("node", (int64_t)member.node);
                mo.addMember("type_id", member.type_id);
                mo.addMember("version", (int64_t)member.version);
                mo.addMember("config_kind", (int64_t)member.config.kind);
                mo.addMember("config_value", (int64_t)member.config.boundary_index_or_parameter_id);
                mo.addMember("constant", (double)member.config.constant);
                members.addArrayElement(mo);
            }
            ro.addMember("members", members);
            auto inputs = choc::value::createEmptyArray();
            for (auto id : region.input_boundaries)
                inputs.addArrayElement((int64_t)id);
            ro.addMember("input_boundaries", inputs);
            auto outputs = choc::value::createEmptyArray();
            for (auto id : region.output_boundaries)
                outputs.addArrayElement((int64_t)id);
            ro.addMember("output_boundaries", outputs);
            auto params = choc::value::createEmptyArray();
            for (const auto& p : region.promoted_parameters) {
                auto po = choc::value::createObject("Parameter");
                po.addMember("param_id", (int64_t)p.param_id);
                po.addMember("key", p.key);
                po.addMember("name", p.name);
                po.addMember("unit", p.unit);
                po.addMember("min", (double)p.range.min);
                po.addMember("max", (double)p.range.max);
                po.addMember("default", (double)p.range.default_value);
                po.addMember("step", (double)p.range.step);
                po.addMember("skew", (double)p.range.skew);
                po.addMember("symmetric_skew", p.range.symmetric_skew);
                po.addMember("rate", (int64_t)p.rate);
                po.addMember("smoothing", (double)p.smoothing_ramp_seconds);
                po.addMember("bound_node", (int64_t)p.bound_node_id);
                po.addMember("bound_port", (int64_t)p.bound_port);
                params.addArrayElement(po);
            }
            ro.addMember("promoted_parameters", params);
            auto lim = choc::value::createObject("Limits");
            lim.addMember("max_member_nodes", (int64_t)region.limits.max_member_nodes);
            lim.addMember("max_internal_connections",
                          (int64_t)region.limits.max_internal_connections);
            lim.addMember("max_input_boundaries", (int64_t)region.limits.max_input_boundaries);
            lim.addMember("max_output_boundaries", (int64_t)region.limits.max_output_boundaries);
            lim.addMember("max_delay_nodes", (int64_t)region.limits.max_delay_nodes);
            lim.addMember("max_promoted_parameters",
                          (int64_t)region.limits.max_promoted_parameters);
            lim.addMember("max_state_bytes", (int64_t)region.limits.max_state_bytes);
            lim.addMember("max_logical_boundary_bytes",
                          (int64_t)region.limits.max_logical_boundary_bytes);
            lim.addMember("max_work_per_frame", (int64_t)region.limits.max_work_per_frame);
            lim.addMember("max_work_per_block", (int64_t)region.limits.max_work_per_block);
            ro.addMember("limits", lim);
            regions_arr.addArrayElement(ro);
        }
        root.addMember("sample_regions", regions_arr);
    }

    return choc::json::toString(root, true);
}

// The plain document wrapped in the saved-identity v7 envelope: format 7,
// each node record carrying its authored_id, and identities and bindings
// spelled with those IDs.
std::string
saved_identity_document(const SignalGraph& graph,
                        const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout) {
    const auto raw = plain_document(graph, editor_layout);
    if (raw.empty())
        return {};
    try {
        auto root = choc::json::parse(raw);
        // Inspection requires node records in ascending id order, the order
        // the identity ordinals are counted in.
        if (root.hasObjectMember("nodes") && root["nodes"].isArray()) {
            const auto records = root["nodes"];
            std::vector<std::uint32_t> order(records.size());
            for (std::uint32_t i = 0; i < records.size(); ++i)
                order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
                return records[a]["id"].getInt64() < records[b]["id"].getInt64();
            });
            std::unordered_map<NodeId, const GraphNode*> by_id;
            for (const auto& node : graph.nodes())
                by_id.emplace(node.id, &node);
            auto sorted = choc::value::createEmptyArray();
            for (const auto i : order) {
                choc::value::Value record(records[i]);
                const auto found = by_id.find(static_cast<NodeId>(record["id"].getInt64()));
                if (found == by_id.end() || !valid_authored_id(found->second->authored_id.str()))
                    return {};
                record.addMember("authored_id", found->second->authored_id.str());
                sorted.addArrayElement(record);
            }
            root.setMember("nodes", sorted);
        }
        root.setMember("format_version", static_cast<int64_t>(kSavedIdentityV7FormatVersion));
        auto identities = choc::value::createEmptyArray();
        auto bindings = choc::value::createObject("Bindings");
        std::vector<const GraphNode*> nodes;
        std::unordered_set<std::string_view> seen;
        for (const auto& node : graph.nodes())
            nodes.push_back(&node);
        std::sort(nodes.begin(), nodes.end(),
                  [](const auto* a, const auto* b) { return a->id < b->id; });
        for (const auto* node : nodes) {
            // A graph never holds two equal IDs; refuse rather than write a
            // document inspection would reject.
            if (!seen.insert(node->authored_id.str()).second)
                return {};
            auto item = choc::value::createObject("Identity");
            item.addMember("id", node->authored_id.str());
            item.addMember("node_id", static_cast<int64_t>(node->id));
            identities.addArrayElement(item);
            bindings.addMember(node->authored_id.str(), static_cast<int64_t>(node->id));
        }
        root.addMember("identity_schema", std::string(kSavedIdentityV7));
        auto logical = choc::value::createObject("LogicalIdentities");
        logical.addMember("profile", std::string(kSavedIdentityV7));
        logical.addMember("identities", identities);
        root.addMember("logical_identities", logical);
        root.addMember("bindings", bindings);
        // The authored event binding only; published events, traces and
        // queue contents belong to a prepared generation and are never saved.
        // A graph without one writes no member, so its document is unchanged.
        if (const auto edge = graph.graph_event_edge()) {
            const auto* producer = graph.node(edge->producer);
            const auto* consumer = graph.node(edge->consumer);
            if (producer == nullptr || consumer == nullptr)
                return {};
            auto record = choc::value::createObject("EventConnection");
            record.addMember("producer", producer->authored_id.str());
            record.addMember("consumer", consumer->authored_id.str());
            record.addMember("source_port", static_cast<int64_t>(edge->source_port));
            record.addMember("destination_port", static_cast<int64_t>(edge->destination_port));
            record.addMember("kind", static_cast<int64_t>(edge->kind));
            auto events = choc::value::createEmptyArray();
            events.addArrayElement(record);
            root.addMember("event_connections", events);
        }
        return choc::json::toString(root, true);
    } catch (...) {
        return {};
    }
}

// The pre-v7 saved-identity writer, kept only for GraphSerializerTestAccess so
// tests can build the legacy documents a v6 binary wrote.
std::string
legacy_v6_document(const SignalGraph& graph,
                   const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout) {
    if (spectral_persistence::contains_spectral_nodes(graph) || graph.graph_event_edge())
        return {};
    const auto raw = plain_document(graph, editor_layout);
    if (raw.empty())
        return {};
    try {
        auto root = choc::json::parse(raw);
        if (root.hasObjectMember("nodes") && root["nodes"].isArray()) {
            const auto records = root["nodes"];
            std::vector<std::uint32_t> order(records.size());
            for (std::uint32_t i = 0; i < records.size(); ++i)
                order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
                return records[a]["id"].getInt64() < records[b]["id"].getInt64();
            });
            auto sorted = choc::value::createEmptyArray();
            for (const auto i : order)
                sorted.addArrayElement(choc::value::Value(records[i]));
            root.setMember("nodes", sorted);
        }
        const auto structural = structural_node_identities(graph);
        std::vector<NodeId> ids;
        for (const auto& [id, identity] : structural)
            ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        auto identities = choc::value::createEmptyArray();
        auto bindings = choc::value::createObject("Bindings");
        for (const auto id : ids) {
            auto item = choc::value::createObject("Identity");
            item.addMember("id", structural.at(id));
            item.addMember("node_id", static_cast<int64_t>(id));
            identities.addArrayElement(item);
            bindings.addMember(structural.at(id), static_cast<int64_t>(id));
        }
        root.addMember("identity_schema", std::string(kSavedIdentityV6));
        auto logical = choc::value::createObject("LogicalIdentities");
        logical.addMember("profile", std::string(kSavedIdentityV6));
        logical.addMember("identities", identities);
        root.addMember("logical_identities", logical);
        root.addMember("bindings", bindings);
        return choc::json::toString(root, true);
    } catch (...) {
        return {};
    }
}

// The one place that picks a writer: a graph holding spectral nodes is written
// by the spectral writer (format 6, or the saved-identity v7 document with its
// closed spectral member set), every other graph by the plain writer,
// optionally wrapped in the saved-identity envelope. Returns an empty string
// on refusal and, when `reason` is given and the writer names one, the rule
// the graph breaks.
std::string serialize(const SignalGraph& graph,
                      const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout,
                      bool saved_identity, std::string* reason) {
    if (reason)
        reason->clear();
    // Only the saved-identity document names event connection endpoints by
    // authored ID; any other format would drop the connection.
    if (!saved_identity && graph.graph_event_edge()) {
        if (reason)
            *reason = "a graph event connection persists only in the saved-identity document";
        return {};
    }
    if (spectral_persistence::contains_spectral_nodes(graph)) {
        std::string why;
        auto json = spectral_persistence::write(graph, editor_layout, why, saved_identity);
        if (reason)
            *reason = std::move(why);
        return json;
    }
    return saved_identity ? saved_identity_document(graph, editor_layout)
                          : plain_document(graph, editor_layout);
}
} // namespace

std::string GraphSerializerTestAccess::legacy_v6_saved_document(
    const SignalGraph& graph,
    const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout) {
    return legacy_v6_document(graph, editor_layout);
}

std::unordered_map<NodeId, std::string>
GraphSerializerTestAccess::legacy_v6_identities(const SignalGraph& graph) {
    return structural_node_identities(graph);
}

std::string
GraphSerializer::to_json(const SignalGraph& graph,
                         const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout) {
    return serialize(graph, editor_layout, false, nullptr);
}

std::unordered_map<NodeId, std::string>
GraphSerializer::saved_node_identities(const SignalGraph& graph) {
    std::unordered_map<NodeId, std::string> identities;
    for (const auto& node : graph.nodes())
        identities.emplace(node.id, node.authored_id.str());
    return identities;
}

std::string GraphSerializer::to_saved_document(
    const SignalGraph& graph,
    const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout) {
    return serialize(graph, editor_layout, true, nullptr);
}

} // namespace pulp::host
