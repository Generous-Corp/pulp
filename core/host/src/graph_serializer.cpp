// GraphSerializer implementation: SignalGraph <-> .pulpgraph JSON.

#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/sample_region_parameters.hpp>
#include <pulp/runtime/log.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulp::host {
namespace {

// Tolerant numeric coercion — choc distinguishes int/float strictly and
// getFloat64() throws on integers, so wrap accesses to coerce the common
// "1.0 round-trips as int64" case. Templated so it accepts both Value
// and ValueView returned by operator[].
template <typename V>
inline double get_double(const V& v) {
    if (v.isFloat32() || v.isFloat64()) return v.getFloat64();
    if (v.isInt32() || v.isInt64()) return (double)v.getInt64();
    return 0.0;
}

constexpr int kFormatVersion = 3;
constexpr std::size_t kMaxSerializedSampleRegionConnections = 2'048;

struct GraphMigrationEntry {
    int from_version = 0;
    int to_version = 0;
    GraphSerializer::MigrationFn migration;
};

bool migrate_graph_v1_to_v2(const std::string& source_json,
                            std::string& migrated_json);
bool migrate_graph_v2_to_v3(const std::string& source_json, std::string& migrated_json);

std::vector<GraphMigrationEntry>& graph_migrations() {
    static std::vector<GraphMigrationEntry> migrations = {
        {1, 2, migrate_graph_v1_to_v2},
        {2, 3, migrate_graph_v2_to_v3},
    };
    return migrations;
}

std::optional<int> graph_format_version(const choc::value::Value& root) {
    const auto& value = root["format_version"];
    if (value.isInt32() || value.isInt64()) {
        const auto raw = value.getInt64();
        if (raw < std::numeric_limits<int>::min()
            || raw > std::numeric_limits<int>::max()) {
            return std::nullopt;
        }
        return static_cast<int>(raw);
    }

    return std::nullopt;
}

bool replace_format_version(std::string& json,
                            int expected_version,
                            int replacement_version) {
    const auto key_pos = json.find("\"format_version\"");
    if (key_pos == std::string::npos) return false;
    const auto colon_pos = json.find(':', key_pos);
    if (colon_pos == std::string::npos) return false;

    auto value_begin = colon_pos + 1;
    while (value_begin < json.size()
           && (json[value_begin] == ' '
               || json[value_begin] == '\t'
               || json[value_begin] == '\r'
               || json[value_begin] == '\n')) {
        ++value_begin;
    }

    auto value_end = value_begin;
    if (value_end < json.size() && json[value_end] == '-') ++value_end;
    while (value_end < json.size()
           && json[value_end] >= '0'
           && json[value_end] <= '9') {
        ++value_end;
    }
    if (value_end == value_begin) return false;

    int parsed = 0;
    try {
        parsed = std::stoi(json.substr(value_begin, value_end - value_begin));
    } catch (...) {
        return false;
    }
    if (parsed != expected_version) return false;

    json.replace(value_begin, value_end - value_begin,
                 std::to_string(replacement_version));
    return true;
}

bool migrate_graph_v1_to_v2(const std::string& source_json,
                            std::string& migrated_json) {
    choc::value::Value root;
    try {
        root = choc::json::parse(source_json);
    } catch (...) {
        return false;
    }
    if (!root.isObject()) return false;
    auto version = graph_format_version(root);
    if (!version.has_value() || *version != 1) return false;

    migrated_json = source_json;
    return replace_format_version(migrated_json, 1, 2);
}

bool migrate_graph_v2_to_v3(const std::string& source_json, std::string& migrated_json) {
    migrated_json = source_json;
    try {
        const auto root = choc::json::parse(source_json);
        if (!root.isObject())
            return false;
        if (root.hasObjectMember("sample_regions")) {
            const auto& regions = root["sample_regions"];
            if (!regions.isArray() || regions.size() != 0)
                return false;
            return replace_format_version(migrated_json, 2, 3);
        }
    } catch (...) {
        return false;
    }
    const auto end = migrated_json.rfind('}');
    if (end == std::string::npos)
        return false;
    const auto body_end = migrated_json.find_last_not_of(" \t\r\n", end - 1);
    const bool comma = body_end != std::string::npos && migrated_json[body_end] != '{';
    migrated_json.insert(end, std::string(comma ? "," : "") + "\"sample_regions\":[]");
    return replace_format_version(migrated_json, 2, 3);
}

const GraphMigrationEntry* find_graph_migration(int from_version) {
    for (const auto& entry : graph_migrations()) {
        if (entry.from_version == from_version) {
            return &entry;
        }
    }
    return nullptr;
}

bool migrate_graph_json(std::string& json, std::string& error) {
    for (;;) {
        choc::value::Value root;
        try {
            root = choc::json::parse(json);
        } catch (const std::exception& e) {
            error = std::string("JSON parse failed: ") + e.what();
            return false;
        }

        if (!root.isObject()) {
            error = "root is not an object";
            return false;
        }
        if (!root.hasObjectMember("format_version")) {
            error = "missing graph format_version";
            return false;
        }

        auto version = graph_format_version(root);
        if (!version.has_value()) {
            error = "format_version is not an integer";
            return false;
        }

        if (*version == kFormatVersion) {
            return true;
        }
        if (*version > kFormatVersion) {
            error = "unsupported graph format_version "
                    + std::to_string(*version);
            return false;
        }

        const auto* migration = find_graph_migration(*version);
        if (migration == nullptr) {
            error = "unsupported graph format_version "
                    + std::to_string(*version);
            return false;
        }

        if (migration->to_version <= *version
            || migration->to_version > kFormatVersion) {
            error = "graph migration does not move toward current format_version";
            return false;
        }

        std::string migrated;
        if (!migration->migration(json, migrated) || migrated.empty()) {
            error = "graph migration failed";
            return false;
        }

        choc::value::Value migrated_root;
        try {
            migrated_root = choc::json::parse(migrated);
        } catch (const std::exception& e) {
            error = std::string("graph migration produced invalid JSON: ") + e.what();
            return false;
        }

        if (!migrated_root.isObject()
            || !migrated_root.hasObjectMember("format_version")) {
            error = "graph migration did not produce expected format_version";
            return false;
        }

        auto migrated_version = graph_format_version(migrated_root);
        if (!migrated_version.has_value()
            || *migrated_version != migration->to_version) {
            error = "graph migration did not produce expected format_version";
            return false;
        }

        json = std::move(migrated);
    }
}

const char* node_type_str(NodeType t) {
    switch (t) {
        case NodeType::AudioInput:  return "audio_in";
        case NodeType::AudioOutput: return "audio_out";
        case NodeType::Plugin:      return "plugin";
        case NodeType::Gain:        return "gain";
        case NodeType::MidiInput:   return "midi_in";
        case NodeType::MidiOutput:  return "midi_out";
        case NodeType::Custom:      return "custom";
    }
    return "unknown";
}

const char* format_str(PluginFormat f) {
    switch (f) {
        case PluginFormat::VST3:        return "vst3";
        case PluginFormat::AudioUnit:   return "au";
        case PluginFormat::AudioUnitV3: return "auv3";
        case PluginFormat::CLAP:        return "clap";
        case PluginFormat::LV2:         return "lv2";
        case PluginFormat::BuiltIn:     return "builtin";
    }
    return "unknown";
}

PluginFormat parse_format(std::string_view s) {
    if (s == "vst3") return PluginFormat::VST3;
    if (s == "au")   return PluginFormat::AudioUnit;
    if (s == "auv3") return PluginFormat::AudioUnitV3;
    if (s == "clap") return PluginFormat::CLAP;
    if (s == "builtin") return PluginFormat::BuiltIn;
    return PluginFormat::LV2;
}

bool parse_type(std::string_view s, NodeType& out) {
    if (s == "audio_in")  { out = NodeType::AudioInput; return true; }
    if (s == "audio_out") { out = NodeType::AudioOutput; return true; }
    if (s == "plugin")    { out = NodeType::Plugin; return true; }
    if (s == "gain")      { out = NodeType::Gain; return true; }
    if (s == "midi_in")   { out = NodeType::MidiInput; return true; }
    if (s == "midi_out")  { out = NodeType::MidiOutput; return true; }
    if (s == "custom")    { out = NodeType::Custom; return true; }
    return false;
}

std::string custom_type_label(std::string_view type_id, int version) {
    std::ostringstream oss;
    oss << type_id << "@" << version;
    return oss.str();
}

bool validate_generated_node_shape(NodeType type,
                                   std::string_view type_name,
                                   int inputs,
                                   int outputs,
                                   std::string& error) {
    if (inputs < 0 || outputs < 0) {
        error = "invalid generated node port count for "
              + std::string(type_name);
        return false;
    }

    auto fail_shape = [&] {
        error = "invalid generated node shape for " + std::string(type_name);
        return false;
    };

    switch (type) {
        case NodeType::AudioInput:
            return inputs == 0 && outputs > 0 ? true : fail_shape();
        case NodeType::AudioOutput:
            return inputs > 0 && outputs == 0 ? true : fail_shape();
        case NodeType::Gain:
            return inputs == 2 && outputs == 2 ? true : fail_shape();
        case NodeType::MidiInput:
            return inputs == 0 && outputs == 1 ? true : fail_shape();
        case NodeType::MidiOutput:
            return inputs == 1 && outputs == 0 ? true : fail_shape();
        case NodeType::Plugin:
        case NodeType::Custom:
            return true;
    }
    return fail_shape();
}

template <typename V> bool json_u64(const V& value, std::uint64_t max, std::uint64_t& out) {
    if (!(value.isInt32() || value.isInt64()))
        return false;
    const auto n = value.getInt64();
    if (n < 0 || static_cast<std::uint64_t>(n) > max)
        return false;
    out = static_cast<std::uint64_t>(n);
    return true;
}

template <typename V> bool json_float(const V& value, float& out) {
    if (!(value.isFloat32() || value.isFloat64() || value.isInt32() || value.isInt64()))
        return false;
    const auto d = get_double(value);
    if (!std::isfinite(d) || d < -std::numeric_limits<float>::max() ||
        d > std::numeric_limits<float>::max())
        return false;
    out = static_cast<float>(d);
    return true;
}

bool persisted_region_limits_within_v1(const SampleRegionLimits& value) noexcept {
    const auto cap = SampleRegionLimits::v1();
    return value.max_member_nodes <= cap.max_member_nodes &&
           value.max_internal_connections <= cap.max_internal_connections &&
           value.max_input_boundaries <= cap.max_input_boundaries &&
           value.max_output_boundaries <= cap.max_output_boundaries &&
           value.max_delay_nodes <= cap.max_delay_nodes &&
           value.max_promoted_parameters <= cap.max_promoted_parameters &&
           value.max_state_bytes <= cap.max_state_bytes &&
           value.max_logical_boundary_bytes <= cap.max_logical_boundary_bytes &&
           value.max_work_per_frame <= cap.max_work_per_frame &&
           value.max_work_per_block <= cap.max_work_per_block;
}

bool valid_persisted_kernel_config(const SampleKernelConfig& config) noexcept {
    switch (config.kind) {
    case SampleKernelConfigKind::None:
        return config.boundary_index_or_parameter_id == 0 && config.constant == 0.0f;
    case SampleKernelConfigKind::BoundaryIndex:
        return config.constant == 0.0f;
    case SampleKernelConfigKind::FiniteConstant:
        return config.boundary_index_or_parameter_id == 0 && std::isfinite(config.constant);
    case SampleKernelConfigKind::PromotedParameterId:
        return config.boundary_index_or_parameter_id != 0 && config.constant == 0.0f;
    case SampleKernelConfigKind::Invalid:
        return false;
    }
    return false;
}

bool validate_persisted_region_metadata(const SignalGraph& graph,
                                        std::span<const SampleRegionDefinition> definitions,
                                        std::string& error) {
    const auto parameters = SampleRegionParameterContract::from_regions(definitions);
    if (!parameters.valid()) {
        error = "invalid promoted parameter metadata";
        return false;
    }

    for (const auto& definition : definitions) {
        if (!persisted_region_limits_within_v1(definition.limits) ||
            definition.members.size() > definition.limits.max_member_nodes ||
            definition.input_boundaries.size() > definition.limits.max_input_boundaries ||
            definition.output_boundaries.size() > definition.limits.max_output_boundaries ||
            definition.promoted_parameters.size() > definition.limits.max_promoted_parameters) {
            error = "sample region metadata exceeds authored limits";
            return false;
        }

        std::unordered_map<NodeId, const SampleRegionKernelNode*> members;
        std::size_t delay_nodes = 0;
        for (const auto& member : definition.members) {
            const auto* current = graph.node(member.node);
            if (member.node == 0 || !members.emplace(member.node, &member).second ||
                current == nullptr || current->type != NodeType::Custom ||
                current->custom_type_id != member.type_id ||
                current->custom_type_version != member.version ||
                !valid_persisted_kernel_config(member.config)) {
                error = "invalid or duplicate sample region member";
                return false;
            }
            if (member.type_id == "pulp.core.unit-delay")
                ++delay_nodes;
        }
        if (delay_nodes > definition.limits.max_delay_nodes) {
            error = "sample region metadata exceeds authored limits";
            return false;
        }

        const auto validate_boundaries = [&](std::span<const NodeId> boundaries,
                                             std::string_view type) {
            if (boundaries.empty())
                return false;
            std::unordered_set<NodeId> unique;
            std::unordered_set<std::uint32_t> indexes;
            for (const auto id : boundaries) {
                const auto found = members.find(id);
                if (!unique.insert(id).second || found == members.end() ||
                    found->second->type_id != type ||
                    found->second->config.kind != SampleKernelConfigKind::BoundaryIndex ||
                    found->second->config.constant != 0.0f ||
                    !indexes.insert(found->second->config.boundary_index_or_parameter_id).second) {
                    return false;
                }
            }
            for (std::uint32_t expected = 0; expected < indexes.size(); ++expected)
                if (!indexes.contains(expected))
                    return false;
            for (const auto& member : definition.members) {
                if (member.type_id == type && !unique.contains(member.node))
                    return false;
            }
            return true;
        };
        if (!validate_boundaries(definition.input_boundaries, "pulp.core.sample-region.input") ||
            !validate_boundaries(definition.output_boundaries, "pulp.core.sample-region.output")) {
            error = "invalid sample region boundary metadata";
            return false;
        }

        std::unordered_set<state::ParamID> bound_parameters;
        for (const auto& parameter : definition.promoted_parameters) {
            const auto found = members.find(parameter.bound_node_id);
            if (found == members.end() ||
                found->second->type_id != "pulp.core.sample-region.parameter" ||
                found->second->config.kind != SampleKernelConfigKind::PromotedParameterId ||
                found->second->config.boundary_index_or_parameter_id != parameter.param_id ||
                found->second->config.constant != 0.0f ||
                !bound_parameters.insert(parameter.param_id).second) {
                error = "invalid promoted parameter binding";
                return false;
            }
        }
        for (const auto& member : definition.members) {
            if (member.type_id == "pulp.core.sample-region.parameter" &&
                !bound_parameters.contains(member.config.boundary_index_or_parameter_id)) {
                error = "invalid promoted parameter binding";
                return false;
            }
        }
    }
    return true;
}

bool validate_persisted_region_topology(const SignalGraph& graph,
                                        std::span<const SampleRegionDefinition> definitions,
                                        std::span<const Connection> connections,
                                        std::string& error) {
    const auto reject = [&] {
        error = "invalid persisted sample region topology";
        return false;
    };
    for (const auto& definition : definitions) {
        std::unordered_map<NodeId, std::size_t> member_index;
        for (std::size_t i = 0; i < definition.members.size(); ++i)
            member_index.emplace(definition.members[i].node, i);
        const std::unordered_set<NodeId> input_boundaries(definition.input_boundaries.begin(),
                                                          definition.input_boundaries.end());
        const std::unordered_set<NodeId> output_boundaries(definition.output_boundaries.begin(),
                                                           definition.output_boundaries.end());
        std::vector<std::vector<std::size_t>> internal_producers;
        std::vector<std::size_t> internal_consumers(definition.members.size());
        std::vector<std::size_t> external_producers(definition.members.size());
        internal_producers.reserve(definition.members.size());
        for (const auto& member : definition.members) {
            const auto* node = graph.node(member.node);
            if (node == nullptr || node->num_input_ports < 0)
                return reject();
            internal_producers.emplace_back(static_cast<std::size_t>(node->num_input_ports));
        }

        std::size_t internal_connections = 0;
        for (const auto& connection : connections) {
            const auto source = member_index.find(connection.source_node);
            const auto destination = member_index.find(connection.dest_node);
            const bool source_inside = source != member_index.end();
            const bool destination_inside = destination != member_index.end();
            if (!source_inside && !destination_inside)
                continue;
            if (connection.midi || connection.automation || connection.audio_rate_modulation ||
                connection.sidechain || connection.feedback)
                return reject();
            if (source_inside) {
                const auto* node = graph.node(connection.source_node);
                if (node == nullptr || node->num_output_ports <= 0 ||
                    connection.source_port >= static_cast<PortIndex>(node->num_output_ports))
                    return reject();
            }
            if (destination_inside) {
                const auto* node = graph.node(connection.dest_node);
                if (node == nullptr || node->num_input_ports <= 0 ||
                    connection.dest_port >= static_cast<PortIndex>(node->num_input_ports))
                    return reject();
            }
            if (source_inside && destination_inside) {
                ++internal_connections;
                ++internal_consumers[source->second];
                ++internal_producers[destination->second][connection.dest_port];
            } else if (destination_inside) {
                if (!input_boundaries.contains(connection.dest_node))
                    return reject();
                ++external_producers[destination->second];
            } else if (!output_boundaries.contains(connection.source_node)) {
                return reject();
            }
        }
        if (internal_connections > definition.limits.max_internal_connections)
            return reject();

        for (std::size_t i = 0; i < definition.members.size(); ++i) {
            const auto& member = definition.members[i];
            const auto producer_count = std::accumulate(internal_producers[i].begin(),
                                                        internal_producers[i].end(), std::size_t{});
            if (input_boundaries.contains(member.node)) {
                if (external_producers[i] != 1 || producer_count != 0)
                    return reject();
            } else if (output_boundaries.contains(member.node)) {
                if (producer_count != 1 || internal_consumers[i] != 0)
                    return reject();
            } else if (std::any_of(internal_producers[i].begin(), internal_producers[i].end(),
                                   [](const auto count) { return count != 1; })) {
                return reject();
            }
        }
    }
    return true;
}

// Minimal base64 encoder (no padding stripping), good enough for state
// blobs in JSON — choc::json escapes inline strings already.
std::string b64_encode(const std::vector<uint8_t>& bytes) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        uint32_t v = (uint32_t)bytes[i] << 16 | (uint32_t)bytes[i+1] << 8 | (uint32_t)bytes[i+2];
        out.push_back(kAlphabet[(v >> 18) & 0x3f]);
        out.push_back(kAlphabet[(v >> 12) & 0x3f]);
        out.push_back(kAlphabet[(v >>  6) & 0x3f]);
        out.push_back(kAlphabet[v & 0x3f]);
    }
    if (i < bytes.size()) {
        uint32_t v = (uint32_t)bytes[i] << 16;
        if (i + 1 < bytes.size()) v |= (uint32_t)bytes[i+1] << 8;
        out.push_back(kAlphabet[(v >> 18) & 0x3f]);
        out.push_back(kAlphabet[(v >> 12) & 0x3f]);
        out.push_back(i + 1 < bytes.size() ? kAlphabet[(v >> 6) & 0x3f] : '=');
        out.push_back('=');
    }
    return out;
}

std::optional<std::vector<uint8_t>> b64_decode(std::string_view s) {
    static int8_t kT[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) kT[i] = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) kT[(unsigned char)a[i]] = (int8_t)i;
        init = true;
    }

    std::string clean;
    clean.reserve(s.size());
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        clean.push_back(c);
    }
    if (clean.empty()) return std::vector<uint8_t>{};
    if (clean.size() % 4 != 0) return std::nullopt;

    std::vector<uint8_t> out;
    out.reserve(clean.size() * 3 / 4);
    for (size_t i = 0; i < clean.size(); i += 4) {
        const bool final_quad = (i + 4 == clean.size());
        int vals[4] = {0, 0, 0, 0};
        int padding = 0;
        for (int j = 0; j < 4; ++j) {
            const char c = clean[i + static_cast<size_t>(j)];
            if (c == '=') {
                if (j < 2) return std::nullopt;
                ++padding;
                continue;
            }
            if (padding > 0) return std::nullopt;
            const int8_t d = kT[static_cast<unsigned char>(c)];
            if (d < 0) return std::nullopt;
            vals[j] = d;
        }
        if (padding > 2) return std::nullopt;
        if (padding > 0 && !final_quad) return std::nullopt;

        const uint32_t v = (static_cast<uint32_t>(vals[0]) << 18)
                         | (static_cast<uint32_t>(vals[1]) << 12)
                         | (static_cast<uint32_t>(vals[2]) << 6)
                         | static_cast<uint32_t>(vals[3]);
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
        if (padding < 2) out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
        if (padding < 1) out.push_back(static_cast<uint8_t>(v & 0xff));
    }
    return out;
}

} // namespace

int GraphSerializer::current_format_version() {
    return kFormatVersion;
}

bool GraphSerializer::register_migration(int from_version,
                                         int to_version,
                                         MigrationFn migration) {
    if (from_version >= to_version
        || to_version > kFormatVersion
        || !migration) {
        return false;
    }

    if (find_graph_migration(from_version) != nullptr) {
        return false;
    }

    graph_migrations().push_back({from_version, to_version, std::move(migration)});
    return true;
}

std::string GraphSerializer::to_json(
    const SignalGraph& graph,
    const std::unordered_map<NodeId, std::pair<float,float>>& editor_layout) {
    for (const auto& node : graph.nodes()) {
        if (graph.is_processor_node(node.id)) {
            return {};
        }
    }
    auto root = choc::value::createObject("PulpGraph");
    const auto regions = graph.sample_regions();
    if (!regions.empty() && graph.connections().size() > kMaxSerializedSampleRegionConnections) {
        return {};
    }
    for (const auto& region : regions) {
        SampleRegionParserShape shape{};
        for (const auto& connection : graph.connections()) {
            const auto touches = [&](NodeId id) {
                return std::any_of(region.members.begin(), region.members.end(),
                                   [&](const auto& member) { return member.node == id; });
            };
            if (touches(connection.source_node) || touches(connection.dest_node))
                ++shape.connections_per_region;
        }
        if (!prove_sample_region_parser_shape(shape).accepted)
            return {};
    }
    const int version = regions.empty() ? 2 : kFormatVersion;
    root.addMember("format_version", (int64_t)version);

    // Nodes
    auto nodes_arr = choc::value::createEmptyArray();
    for (const auto& n : graph.nodes()) {
        auto node_obj = choc::value::createObject("Node");
        node_obj.addMember("id",   (int64_t)n.id);
        node_obj.addMember("type", node_type_str(n.type));
        node_obj.addMember("name", n.name);
        node_obj.addMember("num_input_ports",  (int64_t)n.num_input_ports);
        node_obj.addMember("num_output_ports", (int64_t)n.num_output_ports);
        node_obj.addMember("gain", (double)n.gain);
        if (n.type == NodeType::Plugin) {
            // Always serialize the plugin identity (from GraphNode::plugin_info),
            // even when the slot itself failed to load on this machine. State
            // blob is omitted for unresolved plugins.
            const auto& info = n.plugin_info;
            auto plug_obj = choc::value::createObject("Plugin");
            plug_obj.addMember("format",       std::string(format_str(info.format)));
            plug_obj.addMember("unique_id",    info.unique_id);
            plug_obj.addMember("name",         info.name);
            plug_obj.addMember("manufacturer", info.manufacturer);
            plug_obj.addMember("version",      info.version);
            plug_obj.addMember("last_path",    info.path);
            if (n.plugin) {
                plug_obj.addMember("state_b64", b64_encode(n.plugin->save_state()));
            }
            node_obj.addMember("plugin", plug_obj);
        }
        if (n.type == NodeType::Custom) {
            auto custom_obj = choc::value::createObject("CustomNode");
            custom_obj.addMember("type_id", n.custom_type_id);
            custom_obj.addMember(
                "version",
                (int64_t)(n.custom_type_version > 0 ? n.custom_type_version : 1));
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

    // Connections
    auto conns_arr = choc::value::createEmptyArray();
    for (const auto& c : graph.connections()) {
        auto co = choc::value::createObject("Conn");
        co.addMember("source_node", (int64_t)c.source_node);
        co.addMember("source_port", (int64_t)c.source_port);
        co.addMember("dest_node",   (int64_t)c.dest_node);
        co.addMember("dest_port",   (int64_t)c.dest_port);
        co.addMember("feedback",    c.feedback);
        co.addMember("midi",        c.midi);
        co.addMember("automation",  c.automation);
        co.addMember("audio_rate_modulation", c.audio_rate_modulation);
        // Sidechain flag is additive; absent in older blobs.
        co.addMember("sidechain",   c.sidechain);
        if (c.automation || c.audio_rate_modulation) {
            co.addMember("auto_param_id",  (int64_t)c.automation_param_id);
            co.addMember("auto_range_lo",  (double)c.automation_range_lo);
            co.addMember("auto_range_hi",  (double)c.automation_range_hi);
            co.addMember("auto_smoothing", (double)c.automation_smoothing_ms);
            co.addMember("auto_mix",       (int64_t)c.automation_mix);
        }
        conns_arr.addArrayElement(co);
    }
    root.addMember("connections", conns_arr);

    if (!regions.empty()) {
        auto regions_arr = choc::value::createEmptyArray();
        for (const auto& region : regions) {
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

GraphSerializer::LoadResult GraphSerializer::from_json(SignalGraph& graph, const std::string& json) {
    LoadResult result;
    graph.clear();

    std::string readable_json = json;
    if (!migrate_graph_json(readable_json, result.error)) {
        return result;
    }

    choc::value::Value root;
    try {
        root = choc::json::parse(readable_json);
    } catch (const std::exception& e) {
        result.error = std::string("JSON parse failed: ") + e.what();
        return result;
    }
    if (!root.isObject()) {
        result.error = "root is not an object";
        return result;
    }

    struct ParsedRegion {
        SampleRegionDefinition definition;
    };
    std::vector<ParsedRegion> parsed_regions;
    std::vector<Connection> parsed_connections;
    const bool has_region_records = root.hasObjectMember("sample_regions") &&
                                    root["sample_regions"].isArray() &&
                                    root["sample_regions"].size() != 0;
    try {
        if (root.hasObjectMember("connections")) {
            const auto& conns = root["connections"];
            if (has_region_records && conns.size() > kMaxSerializedSampleRegionConnections) {
                result.error = "too many connections";
                return result;
            }
            parsed_connections.reserve(conns.size());
            for (uint32_t i = 0; i < conns.size(); ++i) {
                const auto& cv = conns[i];
                std::uint64_t n = 0;
                Connection c{};
                if (!json_u64(cv["source_node"], std::numeric_limits<NodeId>::max(), n)) {
                    result.error = "invalid source_node";
                    return result;
                }
                c.source_node = (NodeId)n;
                if (!json_u64(cv["source_port"], std::numeric_limits<PortIndex>::max(), n)) {
                    result.error = "invalid source_port";
                    return result;
                }
                c.source_port = (PortIndex)n;
                if (!json_u64(cv["dest_node"], std::numeric_limits<NodeId>::max(), n)) {
                    result.error = "invalid dest_node";
                    return result;
                }
                c.dest_node = (NodeId)n;
                if (!json_u64(cv["dest_port"], std::numeric_limits<PortIndex>::max(), n)) {
                    result.error = "invalid dest_port";
                    return result;
                }
                c.dest_port = (PortIndex)n;
                if (!cv["feedback"].isBool() || !cv["midi"].isBool() ||
                    !cv["automation"].isBool()) {
                    result.error = "invalid connection flags";
                    return result;
                }
                c.feedback = cv["feedback"].getBool();
                c.midi = cv["midi"].getBool();
                c.automation = cv["automation"].getBool();
                if ((cv.hasObjectMember("audio_rate_modulation") &&
                     !cv["audio_rate_modulation"].isBool()) ||
                    (cv.hasObjectMember("sidechain") && !cv["sidechain"].isBool())) {
                    result.error = "invalid connection flags";
                    return result;
                }
                c.audio_rate_modulation = cv.hasObjectMember("audio_rate_modulation")
                                              ? cv["audio_rate_modulation"].getBool()
                                              : false;
                c.sidechain = cv.hasObjectMember("sidechain") ? cv["sidechain"].getBool() : false;
                if (c.automation || c.audio_rate_modulation) {
                    if (!json_u64(cv["auto_param_id"], std::numeric_limits<std::uint32_t>::max(),
                                  n)) {
                        result.error = "invalid automation parameter";
                        return result;
                    }
                    c.automation_param_id = (std::uint32_t)n;
                    if (!json_float(cv["auto_range_lo"], c.automation_range_lo) ||
                        !json_float(cv["auto_range_hi"], c.automation_range_hi) ||
                        !json_float(cv["auto_smoothing"], c.automation_smoothing_ms) ||
                        !json_u64(cv["auto_mix"], 255, n)) {
                        result.error = "invalid automation values";
                        return result;
                    }
                    c.automation_mix = (AutomationMix)(std::uint8_t)n;
                }
                parsed_connections.push_back(c);
            }
        }
        if (root.hasObjectMember("sample_regions")) {
            const auto& regions = root["sample_regions"];
            if (!regions.isArray() || regions.size() > 16) {
                result.error = "invalid sample_regions";
                return result;
            }
            SampleRegionParserShape shape{};
            shape.regions = regions.size();
            std::unordered_set<SampleRegionId> region_ids;
            for (uint32_t i = 0; i < regions.size(); ++i) {
                const auto& rv = regions[i];
                if (!rv.isObject()) {
                    result.error = "invalid sample region";
                    return result;
                }
                ParsedRegion parsed;
                auto& d = parsed.definition;
                std::uint64_t n = 0;
                if (!json_u64(rv["id"], std::numeric_limits<SampleRegionId>::max(), n) || n == 0 ||
                    !region_ids.insert((SampleRegionId)n).second) {
                    result.error = "invalid or duplicate sample region id";
                    return result;
                }
                d.region_id = (SampleRegionId)n;
                if (!rv["members"].isArray() || rv["members"].size() > 64) {
                    result.error = "invalid sample region members";
                    return result;
                }
                shape.members_total += rv["members"].size();
                shape.members_per_region =
                    std::max(shape.members_per_region, (std::uint64_t)rv["members"].size());
                for (uint32_t j = 0; j < rv["members"].size(); ++j) {
                    const auto& mv = rv["members"][j];
                    SampleRegionKernelNode m;
                    if (!json_u64(mv["node"], std::numeric_limits<NodeId>::max(), n)) {
                        result.error = "invalid sample region member node";
                        return result;
                    }
                    m.node = (NodeId)n;
                    m.type_id = mv["type_id"].getString();
                    if (!json_u64(mv["version"], std::numeric_limits<int>::max(), n) || n == 0) {
                        result.error = "invalid sample region member version";
                        return result;
                    }
                    m.version = (int)n;
                    if (!json_u64(mv["config_kind"], 255, n) || n == 0 ||
                        (n > (std::uint64_t)SampleKernelConfigKind::PromotedParameterId)) {
                        result.error = "invalid sample kernel config kind";
                        return result;
                    }
                    m.config.kind = (SampleKernelConfigKind)(std::uint8_t)n;
                    if (!json_u64(mv["config_value"], std::numeric_limits<std::uint32_t>::max(),
                                  n)) {
                        result.error = "invalid sample kernel config value";
                        return result;
                    }
                    m.config.boundary_index_or_parameter_id = (std::uint32_t)n;
                    if (!json_float(mv["constant"], m.config.constant)) {
                        result.error = "invalid sample kernel constant";
                        return result;
                    }
                    d.members.push_back(std::move(m));
                }
                auto parse_ids = [&](const char* key, std::vector<NodeId>& out) -> bool {
                    if (!rv[key].isArray() || rv[key].size() > 8)
                        return false;
                    for (uint32_t j = 0; j < rv[key].size(); ++j) {
                        if (!json_u64(rv[key][j], std::numeric_limits<NodeId>::max(), n))
                            return false;
                        out.push_back((NodeId)n);
                    }
                    return true;
                };
                if (!parse_ids("input_boundaries", d.input_boundaries) ||
                    !parse_ids("output_boundaries", d.output_boundaries)) {
                    result.error = "invalid sample region boundaries";
                    return result;
                }
                shape.input_boundaries_total += d.input_boundaries.size();
                shape.output_boundaries_total += d.output_boundaries.size();
                shape.input_boundaries_per_region = std::max(
                    shape.input_boundaries_per_region, (std::uint64_t)d.input_boundaries.size());
                shape.output_boundaries_per_region = std::max(
                    shape.output_boundaries_per_region, (std::uint64_t)d.output_boundaries.size());
                const auto& ps = rv["promoted_parameters"];
                if (!ps.isArray() || ps.size() > 16) {
                    result.error = "invalid promoted parameters";
                    return result;
                }
                shape.parameters_total += ps.size();
                for (uint32_t j = 0; j < ps.size(); ++j) {
                    const auto& pv = ps[j];
                    SampleRegionPromotedParameter p;
                    if (!json_u64(pv["param_id"], std::numeric_limits<state::ParamID>::max(), n)) {
                        result.error = "invalid promoted parameter id";
                        return result;
                    }
                    p.param_id = (state::ParamID)n;
                    p.key = pv["key"].getString();
                    p.name = pv["name"].getString();
                    p.unit = pv["unit"].getString();
                    if (!json_float(pv["min"], p.range.min) ||
                        !json_float(pv["max"], p.range.max) ||
                        !json_float(pv["default"], p.range.default_value) ||
                        !json_float(pv["step"], p.range.step) ||
                        !json_float(pv["skew"], p.range.skew) || !pv["symmetric_skew"].isBool() ||
                        !json_u64(pv["rate"], 255, n) ||
                        n != (std::uint64_t)state::ParamRate::ControlRate ||
                        !json_float(pv["smoothing"], p.smoothing_ramp_seconds) ||
                        !json_u64(pv["bound_node"], std::numeric_limits<NodeId>::max(), n)) {
                        result.error = "invalid promoted parameter metadata";
                        return result;
                    }
                    p.range.symmetric_skew = pv["symmetric_skew"].getBool();
                    p.rate = state::ParamRate::ControlRate;
                    p.bound_node_id = (NodeId)n;
                    if (!json_u64(pv["bound_port"], std::numeric_limits<PortIndex>::max(), n)) {
                        result.error = "invalid promoted parameter binding";
                        return result;
                    }
                    p.bound_port = (PortIndex)n;
                    d.promoted_parameters.push_back(std::move(p));
                }
                shape.parameters_per_region =
                    std::max(shape.parameters_per_region, (std::uint64_t)ps.size());
                const auto& lv = rv["limits"];
                if (!lv.isObject()) {
                    result.error = "invalid sample region limits";
                    return result;
                }
                auto u32 = [&](const char* k, std::uint32_t& v) {
                    if (!json_u64(lv[k], std::numeric_limits<std::uint32_t>::max(), n))
                        return false;
                    v = (std::uint32_t)n;
                    return true;
                };
                auto u64 = [&](const char* k, std::uint64_t& v) {
                    return json_u64(lv[k], std::numeric_limits<std::uint64_t>::max(), v);
                };
                if (!u32("max_member_nodes", d.limits.max_member_nodes) ||
                    !u32("max_internal_connections", d.limits.max_internal_connections) ||
                    !u32("max_input_boundaries", d.limits.max_input_boundaries) ||
                    !u32("max_output_boundaries", d.limits.max_output_boundaries) ||
                    !u32("max_delay_nodes", d.limits.max_delay_nodes) ||
                    !u32("max_promoted_parameters", d.limits.max_promoted_parameters) ||
                    !u32("max_state_bytes", d.limits.max_state_bytes) ||
                    !u64("max_logical_boundary_bytes", d.limits.max_logical_boundary_bytes) ||
                    !u32("max_work_per_frame", d.limits.max_work_per_frame) ||
                    !u64("max_work_per_block", d.limits.max_work_per_block)) {
                    result.error = "invalid sample region limits";
                    return result;
                }
                parsed_regions.push_back(std::move(parsed));
            }
            shape.connections_total = parsed_connections.size();
            shape.available_bytes = json.size();
            for (const auto& r : parsed_regions) {
                std::uint64_t region_connections = 0;
                for (const auto& c : parsed_connections) {
                    const auto touches = [&](NodeId id) {
                        return std::any_of(r.definition.members.begin(), r.definition.members.end(),
                                           [&](const auto& m) { return m.node == id; });
                    };
                    if (touches(c.source_node) || touches(c.dest_node))
                        ++region_connections;
                }
                shape.connections_per_region =
                    std::max(shape.connections_per_region, region_connections);
            }
            if (!prove_sample_region_parser_shape(shape).accepted) {
                result.error = "sample region parser shape exceeds limits";
                return result;
            }
        }
    } catch (const std::exception& e) {
        result.error = std::string("preflight failed: ") + e.what();
        return result;
    }

    // From here on, choc field accessors (getInt64 / getString / etc.) throw
    // on type mismatch. Wrap the deserialization body so a malformed
    // .pulpgraph leaves the graph in its cleared state with a clear error.
    try {

    // Pass 1: instantiate every node, build old-id → new-id map.
    std::unordered_map<NodeId, NodeId> id_map;
    std::unordered_map<NodeId, std::pair<NodeId, std::vector<uint8_t>>> deferred_state;
    std::unordered_set<NodeId> unresolved_plugin_nodes;

    if (root.hasObjectMember("nodes")) {
        const auto& nodes = root["nodes"];
        for (uint32_t i = 0; i < nodes.size(); ++i) {
            const auto& nv = nodes[i];
            const NodeId old_id = (NodeId)nv["id"].getInt64();
            const std::string name(nv["name"].getString());
            const std::string type_s(nv["type"].getString());
            if (type_s == "processor") {
                graph.clear();
                result.error = "Processor nodes are runtime-owned and unsupported by .pulpgraph v1";
                return result;
            }
            NodeType t = NodeType::Custom;
            const bool known_type = parse_type(type_s, t);
            const int in_ch  = (int)nv["num_input_ports"].getInt64();
            const int out_ch = (int)nv["num_output_ports"].getInt64();
            const float gain = nv.hasObjectMember("gain") ? (float)get_double(nv["gain"]) : 1.0f;
            std::string node_shape_error;
            if (!validate_generated_node_shape(
                    t, type_s, in_ch, out_ch, node_shape_error)) {
                graph.clear();
                result.ok = false;
                result.error = node_shape_error;
                return result;
            }

            NodeId new_id = 0;
            switch (t) {
                case NodeType::AudioInput:  new_id = graph.add_input_node(out_ch, name); break;
                case NodeType::AudioOutput: new_id = graph.add_output_node(in_ch, name); break;
                case NodeType::Gain:        new_id = graph.add_gain_node(name); break;
                case NodeType::MidiInput:   new_id = graph.add_midi_input_node(name); break;
                case NodeType::MidiOutput:  new_id = graph.add_midi_output_node(name); break;
                case NodeType::Custom: {
                    std::string type_id = type_s;
                    int version = 1;
                    if (known_type && nv.hasObjectMember("custom")) {
                        const auto& cv = nv["custom"];
                        if (cv.hasObjectMember("type_id")) {
                            type_id = cv["type_id"].getString();
                        }
                        if (cv.hasObjectMember("version")) {
                            version = (int)cv["version"].getInt64();
                        }
                    }
                    const auto* registered = graph.custom_node_type(type_id, version);
                    if (registered
                        && registered->num_input_ports == in_ch
                        && registered->num_output_ports == out_ch) {
                        new_id = graph.add_custom_node(type_id, version, name);
                    } else {
                        result.missing_custom_node_types.push_back(
                            custom_type_label(type_id, version));
                        new_id = graph.add_unresolved_custom_node(
                            type_id, version, in_ch, out_ch, name);
                    }
                    // Restore opaque custom-node state. Applied to a resolved
                    // node's instance on its next prepare(); for an unresolved
                    // node the blob is preserved so a re-save keeps it.
                    if (new_id != 0 && nv.hasObjectMember("custom")) {
                        const auto& cv2 = nv["custom"];
                        if (cv2.hasObjectMember("state_b64")) {
                            auto blob = b64_decode(cv2["state_b64"].getString());
                            if (!blob) {
                                graph.clear();
                                result.ok = false;
                                result.error = "invalid custom state_b64";
                                return result;
                            }
                            graph.set_custom_node_state(new_id, *blob);
                        }
                    }
                    break;
                }
                case NodeType::Plugin: {
                    if (!nv.hasObjectMember("plugin")) {
                        result.missing_plugins.push_back(name + " (no plugin info)");
                        break;
                    }
                    const auto& pv = nv["plugin"];
                    PluginInfo info;
                    info.name         = pv["name"].getString();
                    info.manufacturer = pv["manufacturer"].getString();
                    info.version      = pv["version"].getString();
                    info.unique_id    = pv["unique_id"].getString();
                    info.path         = pv["last_path"].getString();
                    info.format       = parse_format(pv["format"].getString());
                    info.num_inputs   = in_ch;
                    info.num_outputs  = out_ch;
                    auto slot = PluginSlot::load(info);
                    if (!slot) {
                        result.missing_plugins.push_back(
                            std::string(format_str(info.format)) + ":" + info.unique_id);
                        // Still create a placeholder Plugin node with no slot so
                        // connection IDs remain stable.
                        new_id = graph.add_unresolved_plugin_node(info, in_ch, out_ch, name);
                        if (new_id != 0)
                            unresolved_plugin_nodes.insert(new_id);
                    } else {
                        if (pv.hasObjectMember("state_b64")) {
                            auto blob = b64_decode(pv["state_b64"].getString());
                            if (!blob) {
                                graph.clear();
                                result.ok = false;
                                result.error = "invalid plugin state_b64";
                                return result;
                            }
                            if (!blob->empty()) slot->restore_state(*blob);
                        }
                        new_id = graph.add_plugin_node(std::move(slot), in_ch, out_ch, name);
                    }
                    break;
                }
            }
            if (new_id != 0) {
                id_map[old_id] = new_id;
                graph.set_node_gain(new_id, gain);
                if (nv.hasObjectMember("layout")) {
                    const auto& pos = nv["layout"];
                    result.editor_layout[new_id] = {
                        (float)get_double(pos["x"]),
                        (float)get_double(pos["y"])
                    };
                }
            }
        }
    }

    // v3 region-bearing graphs are installed as one locked authoring mutation.
    if (!parsed_regions.empty()) {
        std::vector<SampleRegionDefinition> definitions;
        definitions.reserve(parsed_regions.size());
        const auto remap_node = [&](const char* context, NodeId persisted, NodeId& remapped) {
            const auto it = id_map.find(persisted);
            if (it == id_map.end()) {
                result.error = std::string("sample region ") + context +
                               " references missing node " + std::to_string(persisted);
                return false;
            }
            remapped = it->second;
            return true;
        };
        for (auto& parsed : parsed_regions) {
            auto definition = std::move(parsed.definition);
            for (auto& member : definition.members) {
                if (!remap_node("member", member.node, member.node)) {
                    graph.clear();
                    return result;
                }
            }
            for (auto& id : definition.input_boundaries) {
                if (!remap_node("input boundary", id, id)) {
                    graph.clear();
                    return result;
                }
            }
            for (auto& id : definition.output_boundaries) {
                if (!remap_node("output boundary", id, id)) {
                    graph.clear();
                    return result;
                }
            }
            for (auto& parameter : definition.promoted_parameters) {
                if (!remap_node("promoted parameter", parameter.bound_node_id,
                                parameter.bound_node_id)) {
                    graph.clear();
                    return result;
                }
            }
            definitions.push_back(std::move(definition));
        }
        if (!validate_persisted_region_metadata(graph, definitions, result.error)) {
            graph.clear();
            return result;
        }
        std::sort(definitions.begin(), definitions.end(),
                  [](const auto& a, const auto& b) { return a.region_id < b.region_id; });
        for (auto& definition : definitions) {
            std::sort(definition.members.begin(), definition.members.end(),
                      [](const auto& a, const auto& b) { return a.node < b.node; });
            std::sort(definition.promoted_parameters.begin(), definition.promoted_parameters.end(),
                      [](const auto& a, const auto& b) { return a.param_id < b.param_id; });
            const auto boundary_index = [&](NodeId id) {
                const auto it = std::find_if(definition.members.begin(), definition.members.end(),
                                             [&](const auto& m) { return m.node == id; });
                return it == definition.members.end() ? std::numeric_limits<std::uint32_t>::max()
                                                      : it->config.boundary_index_or_parameter_id;
            };
            std::sort(definition.input_boundaries.begin(), definition.input_boundaries.end(),
                      [&](NodeId a, NodeId b) { return boundary_index(a) < boundary_index(b); });
            std::sort(definition.output_boundaries.begin(), definition.output_boundaries.end(),
                      [&](NodeId a, NodeId b) { return boundary_index(a) < boundary_index(b); });
        }
        for (auto& connection : parsed_connections) {
            if (!remap_node("connection source", connection.source_node, connection.source_node) ||
                !remap_node("connection destination", connection.dest_node, connection.dest_node)) {
                graph.clear();
                return result;
            }
        }
        std::string connection_error;
        std::unordered_map<NodeId, SampleRegionId> region_members;
        for (const auto& definition : definitions) {
            for (const auto& member : definition.members) {
                if (!region_members.emplace(member.node, definition.region_id).second) {
                    connection_error = "sample region member belongs to multiple regions";
                    break;
                }
            }
            if (!connection_error.empty())
                break;
        }
        if (connection_error.empty() &&
            !validate_persisted_region_topology(graph, definitions, parsed_connections,
                                                connection_error)) {
            graph.clear();
            result.error = std::move(connection_error);
            return result;
        }
        const auto node_region = [&](NodeId id) {
            const auto found = region_members.find(id);
            return found == region_members.end() ? SampleRegionId{} : found->second;
        };
        const auto region_internal = [&](const Connection& connection) {
            const auto source_region = node_region(connection.source_node);
            return source_region != 0 && source_region == node_region(connection.dest_node);
        };

        // Region-internal cycles are legal when a UnitDelay makes them sample-causal,
        // so validate their structural subset manually and stage them before replaying
        // every exterior edge through the normal public connection API.
        {
            SignalGraph::GraphMutationLock lock(graph);
            graph.connections_.clear();
            graph.connection_identities_.clear();
            for (const auto& connection : parsed_connections) {
                const unsigned lane_count =
                    static_cast<unsigned>(connection.midi) +
                    static_cast<unsigned>(connection.automation) +
                    static_cast<unsigned>(connection.audio_rate_modulation) +
                    static_cast<unsigned>(connection.sidechain);
                if (lane_count > 1 || (connection.feedback && lane_count != 0)) {
                    connection_error = "connection has conflicting lane flags";
                    break;
                }
                if ((connection.automation || connection.audio_rate_modulation) &&
                    (connection.automation_mix != AutomationMix::Replace &&
                     connection.automation_mix != AutomationMix::Add)) {
                    connection_error = "connection has an invalid automation mix";
                    break;
                }
                if (!region_internal(connection)) {
                    if ((node_region(connection.source_node) != 0 ||
                         node_region(connection.dest_node) != 0) &&
                        (lane_count != 0 || connection.feedback)) {
                        connection_error = "sample region boundary connection is not plain audio";
                        break;
                    }
                    continue;
                }
                const auto* source = graph.node(connection.source_node);
                const auto* destination = graph.node(connection.dest_node);
                if (source == nullptr || destination == nullptr || lane_count != 0 ||
                    connection.feedback || source->num_output_ports <= 0 ||
                    destination->num_input_ports <= 0 ||
                    connection.source_port >= static_cast<PortIndex>(source->num_output_ports) ||
                    connection.dest_port >= static_cast<PortIndex>(destination->num_input_ports)) {
                    connection_error = "sample region connection has an invalid shape";
                    break;
                }
                if (std::find(graph.connections_.begin(), graph.connections_.end(), connection) !=
                    graph.connections_.end()) {
                    connection_error = "sample region connection is duplicated";
                    break;
                }
                graph.append_connection_locked_(connection);
            }
        }

        std::vector<bool> drop_connection(parsed_connections.size(), false);
        for (std::size_t i = 0; i < parsed_connections.size(); ++i) {
            const auto& connection = parsed_connections[i];
            if (!connection_error.empty() || region_internal(connection))
                continue;
            bool accepted = false;
            if (connection.audio_rate_modulation) {
                accepted = graph.connect_audio_rate_modulation(
                    connection.source_node, connection.source_port, connection.dest_node,
                    connection.automation_param_id, connection.automation_range_lo,
                    connection.automation_range_hi, connection.automation_smoothing_ms,
                    connection.automation_mix);
            } else if (connection.automation) {
                accepted = graph.connect_automation(
                    connection.source_node, connection.source_port, connection.dest_node,
                    connection.automation_param_id, connection.automation_range_lo,
                    connection.automation_range_hi, connection.automation_smoothing_ms,
                    connection.automation_mix);
            } else if (connection.sidechain) {
                accepted = graph.connect_sidechain(connection.source_node, connection.source_port,
                                                   connection.dest_node, connection.dest_port);
            } else if (connection.midi) {
                accepted = graph.connect_midi(connection.source_node, connection.dest_node);
            } else if (connection.feedback) {
                accepted = graph.connect_feedback(connection.source_node, connection.source_port,
                                                  connection.dest_node, connection.dest_port);
            } else {
                accepted = graph.connect(connection.source_node, connection.source_port,
                                         connection.dest_node, connection.dest_port);
            }
            if (accepted) {
                SignalGraph::GraphMutationLock lock(graph);
                parsed_connections[i] = graph.connections_.back();
            }
            const auto* unresolved_source = graph.node(connection.source_node);
            const bool valid_unresolved_source =
                unresolved_source != nullptr && unresolved_source->num_output_ports > 0 &&
                connection.source_port <
                    static_cast<PortIndex>(unresolved_source->num_output_ports);
            if (!accepted && valid_unresolved_source &&
                (connection.automation || connection.audio_rate_modulation) &&
                unresolved_plugin_nodes.contains(connection.dest_node)) {
                // Match the legacy partial-load contract: an unavailable plugin
                // cannot accept parameter routing, so omit only that edge while
                // preserving its placeholder and the rest of the graph.
                drop_connection[i] = true;
                continue;
            }
            if (!accepted)
                connection_error = "connection failed validation";
        }

        if (connection_error.empty()) {
            SignalGraph::GraphMutationLock lock(graph);
            std::vector<SampleRegionCandidate> resolved_candidates;
            for (const auto& definition : definitions) {
                const bool fully_resolved = std::all_of(
                    definition.members.begin(), definition.members.end(), [&](const auto& member) {
                        return graph.sample_kernel_type(member.type_id, member.version) != nullptr;
                    });
                if (!fully_resolved)
                    continue;
                const auto metadata = graph.sample_region_metadata_proof_locked_(definition, true);
                if (!metadata.accepted) {
                    connection_error = "sample region metadata proof failed: " + metadata.message;
                    break;
                }
                auto candidate = graph.sample_region_candidate_locked_(definition);
                // Persistence has no runtime block size. One frame is the smallest
                // preparable block and preserves every structural and aggregate
                // admission check without inventing a later host block size.
                candidate.max_block_size = 1;
                resolved_candidates.push_back(std::move(candidate));
            }
            if (connection_error.empty() && !resolved_candidates.empty()) {
                const auto proof = pulp::host::prove_sample_regions(resolved_candidates);
                if (!proof.accepted) {
                    connection_error =
                        "sample region proof failed: complete graph admission rejected";
                    if (!proof.region_proof.message.empty())
                        connection_error += ": " + proof.region_proof.message;
                }
            }
            if (connection_error.empty()) {
                for (const auto& node : graph.nodes_) {
                    if (node.type == NodeType::Custom &&
                        graph.sample_kernel_type(node.custom_type_id, node.custom_type_version) !=
                            nullptr &&
                        !region_members.contains(node.id)) {
                        connection_error = "sample region proof failed: scalar kernel is outside "
                                           "every declared region";
                        break;
                    }
                }
            }
        }

        if (connection_error.empty()) {
            SignalGraph::GraphMutationLock lock(graph);
            graph.connections_.clear();
            graph.connection_identities_.clear();
            for (std::size_t i = 0; i < parsed_connections.size(); ++i) {
                if (!drop_connection[i])
                    graph.append_connection_locked_(parsed_connections[i]);
            }
            graph.sample_region_definitions_ = std::move(definitions);
            graph.invalidate_live_locked_();
        }
        if (!connection_error.empty()) {
            graph.clear();
            result.error = std::move(connection_error);
            return result;
        }
    } else if (root.hasObjectMember("connections")) {
        // Pass 2: replay connections using the id map (legacy v1/v2 path).
        const auto& conns = root["connections"];
        for (uint32_t i = 0; i < conns.size(); ++i) {
            const auto& cv = conns[i];
            NodeId src_old = (NodeId)cv["source_node"].getInt64();
            NodeId dst_old = (NodeId)cv["dest_node"].getInt64();
            auto sit = id_map.find(src_old);
            auto dit = id_map.find(dst_old);
            if (sit == id_map.end() || dit == id_map.end()) continue;
            NodeId src = sit->second;
            NodeId dst = dit->second;
            PortIndex sp = (PortIndex)cv["source_port"].getInt64();
            PortIndex dp = (PortIndex)cv["dest_port"].getInt64();
            const bool fb   = cv["feedback"].getBool();
            const bool md   = cv["midi"].getBool();
            const bool au   = cv["automation"].getBool();
            const bool ar   = cv.hasObjectMember("audio_rate_modulation")
                ? cv["audio_rate_modulation"].getBool()
                : false;
            const bool sc   = cv.hasObjectMember("sidechain")
                ? cv["sidechain"].getBool()
                : false;
            if (ar) {
                graph.connect_audio_rate_modulation(
                    src, sp, dst,
                    (uint32_t)cv["auto_param_id"].getInt64(),
                    (float)get_double(cv["auto_range_lo"]),
                    (float)get_double(cv["auto_range_hi"]),
                    (float)get_double(cv["auto_smoothing"]),
                    (AutomationMix)(uint8_t)cv["auto_mix"].getInt64());
            } else if (au) {
                graph.connect_automation(
                    src, sp, dst,
                    (uint32_t)cv["auto_param_id"].getInt64(),
                    (float)get_double(cv["auto_range_lo"]),
                    (float)get_double(cv["auto_range_hi"]),
                    (float)get_double(cv["auto_smoothing"]),
                    (AutomationMix)(uint8_t)cv["auto_mix"].getInt64());
            } else if (fb) {
                graph.connect_feedback(src, sp, dst, dp);
            } else if (md) {
                graph.connect_midi(src, dst);
            } else if (sc) {
                graph.connect_sidechain(src, sp, dst, dp);
            } else {
                graph.connect(src, sp, dst, dp);
            }
        }
    }

    } catch (const std::exception& e) {
        graph.clear();
        result.ok = false;
        result.error = std::string("field deserialization failed: ") + e.what();
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace pulp::host
