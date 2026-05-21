// GraphSerializer implementation: SignalGraph <-> .pulpgraph JSON.

#include <pulp/host/graph_serializer.hpp>
#include <pulp/runtime/log.hpp>

#include <choc/text/choc_JSON.h>

#include <sstream>
#include <unordered_map>
#include <cstdint>
#include <limits>
#include <optional>
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

constexpr int kFormatVersion = 2;

struct GraphMigrationEntry {
    int from_version = 0;
    int to_version = 0;
    GraphSerializer::MigrationFn migration;
};

bool migrate_graph_v1_to_v2(const std::string& source_json,
                            std::string& migrated_json);

std::vector<GraphMigrationEntry>& graph_migrations() {
    static std::vector<GraphMigrationEntry> migrations = {
        {1, 2, migrate_graph_v1_to_v2},
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
    }
    return "unknown";
}

PluginFormat parse_format(std::string_view s) {
    if (s == "vst3") return PluginFormat::VST3;
    if (s == "au")   return PluginFormat::AudioUnit;
    if (s == "auv3") return PluginFormat::AudioUnitV3;
    if (s == "clap") return PluginFormat::CLAP;
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

std::vector<uint8_t> b64_decode(std::string_view s) {
    static int8_t kT[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) kT[i] = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) kT[(unsigned char)a[i]] = (int8_t)i;
        init = true;
    }
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4);
    int v = 0, bits = 0;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int8_t d = kT[(unsigned char)c];
        if (d < 0) continue;
        v = (v << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((v >> bits) & 0xff));
        }
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
    auto root = choc::value::createObject("PulpGraph");
    root.addMember("format_version", (int64_t)kFormatVersion);

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

    // From here on, choc field accessors (getInt64 / getString / etc.) throw
    // on type mismatch. Wrap the deserialization body so a malformed
    // .pulpgraph leaves the graph in its cleared state with a clear error.
    try {

    // Pass 1: instantiate every node, build old-id → new-id map.
    std::unordered_map<NodeId, NodeId> id_map;
    std::unordered_map<NodeId, std::pair<NodeId, std::vector<uint8_t>>> deferred_state;

    if (root.hasObjectMember("nodes")) {
        const auto& nodes = root["nodes"];
        for (uint32_t i = 0; i < nodes.size(); ++i) {
            const auto& nv = nodes[i];
            const NodeId old_id = (NodeId)nv["id"].getInt64();
            const std::string name(nv["name"].getString());
            const std::string type_s(nv["type"].getString());
            NodeType t = NodeType::Custom;
            const bool known_type = parse_type(type_s, t);
            const int in_ch  = (int)nv["num_input_ports"].getInt64();
            const int out_ch = (int)nv["num_output_ports"].getInt64();
            const float gain = nv.hasObjectMember("gain") ? (float)get_double(nv["gain"]) : 1.0f;

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
                    } else {
                        if (pv.hasObjectMember("state_b64")) {
                            auto blob = b64_decode(pv["state_b64"].getString());
                            if (!blob.empty()) slot->restore_state(blob);
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

    // Pass 2: replay connections using the id map.
    if (root.hasObjectMember("connections")) {
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
