#pragma once

// Format constants, writer bounds and node identity spelling shared by the
// GraphSerializer save path (graph_serializer_save.cpp) and its inspection and
// load paths (graph_serializer.cpp), so the writer and the readers cannot
// disagree on them.

#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulp::host::detail {

inline constexpr int kSavedIdentityV7FormatVersion = 7;
inline constexpr std::string_view kSavedIdentityV6 = "pulp.graph.saved-identity.v6";
inline constexpr std::string_view kSavedIdentityV7 = "pulp.graph.saved-identity.v7";
inline constexpr std::size_t kMaxSerializedSampleRegionConnections = 2'048;
inline constexpr std::size_t kMaxSerializedNodes = 512;
static_assert(GraphSerializer::max_object_members >= kMaxSerializedNodes,
              "the member bound must admit the writer's own bindings object");
inline constexpr std::size_t kMaxSerializedConnections = 16'384;

inline const char* node_type_str(NodeType t) {
    switch (t) {
    case NodeType::AudioInput:
        return "audio_in";
    case NodeType::AudioOutput:
        return "audio_out";
    case NodeType::Plugin:
        return "plugin";
    case NodeType::Gain:
        return "gain";
    case NodeType::MidiInput:
        return "midi_in";
    case NodeType::MidiOutput:
        return "midi_out";
    case NodeType::Custom:
        return "custom";
    }
    return "unknown";
}

// The one authored identity key format. The writer (from a live node) and
// inspection (from a saved record) both build keys here, so they cannot
// disagree. A Plugin key carries its unique id (empty for a Processor node), a
// Custom key carries its type id and version. The writer records a custom type
// version below 1 as 1, so the key uses the same normalized version and a
// reload reproduces it.
inline std::string identity_key(NodeType type, std::string_view name, std::string_view unique_id,
                                std::string_view custom_type_id, std::int64_t custom_version) {
    std::string key = std::string(node_type_str(type)) + ":" + std::string(name);
    if (type == NodeType::Plugin)
        key += ":" + std::string(unique_id);
    if (type == NodeType::Custom)
        key += ":" + std::string(custom_type_id) + "@" +
               std::to_string(custom_version > 0 ? custom_version : 1);
    return key;
}

// A logical identity: the key plus its ordinal among nodes sharing that key,
// counted in persisted node id order.
inline std::string format_identity(std::string_view key, std::size_t ordinal) {
    return "node:" + std::string(key) + ":" + std::to_string(ordinal);
}

inline std::string
structural_node_identity(const GraphNode& node,
                         std::unordered_map<std::string, std::size_t>& ordinals) {
    const auto key = identity_key(node.type, node.name, node.plugin_info.unique_id,
                                  node.custom_type_id, node.custom_type_version);
    return format_identity(key, ordinals[key]++);
}

// The saved-identity v6 identity of every node: the structural key plus its
// ordinal in NodeId order. Only v6 validation uses it.
inline std::unordered_map<NodeId, std::string>
structural_node_identities(const SignalGraph& graph) {
    std::unordered_map<std::string, std::size_t> ordinals;
    std::vector<const GraphNode*> nodes;
    for (const auto& node : graph.nodes())
        nodes.push_back(&node);
    std::sort(nodes.begin(), nodes.end(),
              [](const auto* a, const auto* b) { return a->id < b->id; });
    std::unordered_map<NodeId, std::string> identities;
    for (const auto* node : nodes)
        identities.emplace(node->id, structural_node_identity(*node, ordinals));
    return identities;
}

// An authored node ID is opaque. Readers accept any 1..128 character string
// over [A-Za-z0-9._~-]; this implementation writes "nid-" plus 32 hex digits.
inline bool valid_authored_id(std::string_view id) noexcept {
    if (id.empty() || id.size() > 128)
        return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '~' || c == '-';
    });
}

inline const char* format_str(PluginFormat f) {
    switch (f) {
    case PluginFormat::VST3:
        return "vst3";
    case PluginFormat::AudioUnit:
        return "au";
    case PluginFormat::AudioUnitV3:
        return "auv3";
    case PluginFormat::CLAP:
        return "clap";
    case PluginFormat::LV2:
        return "lv2";
    case PluginFormat::BuiltIn:
        return "builtin";
    }
    return "unknown";
}

} // namespace pulp::host::detail
