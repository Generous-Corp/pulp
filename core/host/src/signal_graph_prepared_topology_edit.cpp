// SignalGraph prepared-topology transactions.
//
// Unlike begin_swap_edit(), this path never mutates the owner before success.
// A private SignalGraph carries the complete candidate authoring state and is
// compiled off-side. Commit moves that state and publishes its snapshot while
// holding the owner's control lock, so audio observes exactly the old or new
// compiled graph and control readers cannot observe a mixed authoring graph.

#include <pulp/host/signal_graph_prepared_topology_edit.hpp>
#include <pulp/host/control_index_nodes.hpp>
#include "signal_graph_prepared_description_capture.hpp"
#include "signal_graph_prepared_edit_admission.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <pulp/host/detail/prepared_lifecycle_ownership.hpp>
#include <pulp/host/graph_identity.hpp>
#include <pulp/host/identity_text.hpp>
#include <pulp/host/sample_region_plan.hpp>
#include <pulp/runtime/crypto.hpp>
#include <choc/text/choc_JSON.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <set>

namespace pulp::host {
namespace {

class JsonDuplicateScanner {
  public:
    explicit JsonDuplicateScanner(std::string_view text) : text_(text) {}

    bool valid() {
        skip_space();
        if (!value())
            return false;
        skip_space();
        return position_ == text_.size();
    }

  private:
    static bool append_code_point(std::string& output, std::uint32_t code_point) {
        if (code_point <= 0x7fu) {
            output.push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7ffu) {
            output.push_back(static_cast<char>(0xc0u | (code_point >> 6)));
            output.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
        } else if (code_point <= 0xffffu) {
            output.push_back(static_cast<char>(0xe0u | (code_point >> 12)));
            output.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
        } else if (code_point <= 0x10ffffu) {
            output.push_back(static_cast<char>(0xf0u | (code_point >> 18)));
            output.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
        } else {
            return false;
        }
        return true;
    }

    bool escaped_code_unit(std::uint16_t& value) {
        if (position_ + 4 > text_.size())
            return false;
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const auto c = static_cast<unsigned char>(text_[position_++]);
            if (!std::isxdigit(c))
                return false;
            value = static_cast<std::uint16_t>(value << 4);
            if (c >= '0' && c <= '9')
                value = static_cast<std::uint16_t>(value + c - '0');
            else if (c >= 'a' && c <= 'f')
                value = static_cast<std::uint16_t>(value + c - 'a' + 10);
            else
                value = static_cast<std::uint16_t>(value + c - 'A' + 10);
        }
        return true;
    }

    bool raw_utf8(std::string& output, unsigned char first) {
        std::uint32_t code_point = 0;
        std::size_t continuation_count = 0;
        if (first <= 0x7fu) {
            output.push_back(static_cast<char>(first));
            return true;
        }
        if (first >= 0xc2u && first <= 0xdfu) {
            code_point = first & 0x1fu;
            continuation_count = 1;
        } else if (first >= 0xe0u && first <= 0xefu) {
            code_point = first & 0x0fu;
            continuation_count = 2;
        } else if (first >= 0xf0u && first <= 0xf4u) {
            code_point = first & 0x07u;
            continuation_count = 3;
        } else {
            return false;
        }
        if (position_ + continuation_count > text_.size())
            return false;
        for (std::size_t i = 0; i < continuation_count; ++i) {
            const auto c = static_cast<unsigned char>(text_[position_++]);
            if ((c & 0xc0u) != 0x80u)
                return false;
            code_point = (code_point << 6) | (c & 0x3fu);
        }
        if ((continuation_count == 1 && code_point < 0x80u) ||
            (continuation_count == 2 && code_point < 0x800u) ||
            (continuation_count == 3 && code_point < 0x10000u) ||
            (code_point >= 0xd800u && code_point <= 0xdfffu) || code_point > 0x10ffffu)
            return false;
        return append_code_point(output, code_point);
    }

    void skip_space() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\n' ||
                                            text_[position_] == '\r' || text_[position_] == '\t'))
            ++position_;
    }

    bool string_token(std::string* decoded = nullptr) {
        if (position_ >= text_.size() || text_[position_++] != '"')
            return false;
        std::string value;
        while (position_ < text_.size()) {
            const auto c = static_cast<unsigned char>(text_[position_++]);
            if (c == '"') {
                if (decoded)
                    *decoded = std::move(value);
                return true;
            }
            if (c < 0x20)
                return false;
            if (c == '\\') {
                if (position_ >= text_.size())
                    return false;
                const auto escaped = text_[position_++];
                switch (escaped) {
                case '"':
                    value.push_back('"');
                    break;
                case '\\':
                    value.push_back('\\');
                    break;
                case '/':
                    value.push_back('/');
                    break;
                case 'b':
                    value.push_back('\b');
                    break;
                case 'f':
                    value.push_back('\f');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                case 'u': {
                    std::uint16_t high = 0;
                    if (!escaped_code_unit(high))
                        return false;
                    std::uint32_t code_point = high;
                    if (high >= 0xd800u && high <= 0xdbffu) {
                        if (position_ + 6 > text_.size() || text_[position_] != '\\' ||
                            text_[position_ + 1] != 'u')
                            return false;
                        position_ += 2;
                        std::uint16_t low = 0;
                        if (!escaped_code_unit(low) || low < 0xdc00u || low > 0xdfffu)
                            return false;
                        code_point = 0x10000u +
                                     ((static_cast<std::uint32_t>(high) - 0xd800u) << 10) +
                                     (static_cast<std::uint32_t>(low) - 0xdc00u);
                    } else if (high >= 0xdc00u && high <= 0xdfffu) {
                        return false;
                    }
                    if (!append_code_point(value, code_point))
                        return false;
                    break;
                }
                default:
                    return false;
                }
            } else if (!raw_utf8(value, c)) {
                return false;
            }
        }
        return false;
    }

    bool object() {
        ++position_; // '{'
        skip_space();
        std::set<std::string> keys;
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            return true;
        }
        while (position_ < text_.size()) {
            std::string decoded_key;
            if (!string_token(&decoded_key))
                return false;
            if (!keys.emplace(std::move(decoded_key)).second)
                return false;
            skip_space();
            if (position_ >= text_.size() || text_[position_++] != ':')
                return false;
            skip_space();
            if (!value())
                return false;
            skip_space();
            if (position_ < text_.size() && text_[position_] == '}') {
                ++position_;
                return true;
            }
            if (position_ >= text_.size() || text_[position_++] != ',')
                return false;
            skip_space();
        }
        return false;
    }

    bool array() {
        ++position_; // '['
        skip_space();
        if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            return true;
        }
        while (position_ < text_.size()) {
            if (!value())
                return false;
            skip_space();
            if (position_ < text_.size() && text_[position_] == ']') {
                ++position_;
                return true;
            }
            if (position_ >= text_.size() || text_[position_++] != ',')
                return false;
            skip_space();
        }
        return false;
    }

    bool value() {
        skip_space();
        if (position_ >= text_.size())
            return false;
        switch (text_[position_]) {
        case '{':
            return object();
        case '[':
            return array();
        case '"':
            return string_token();
        default:
            // A comma or closing delimiter cannot stand in for a JSON value.
            // Without this guard the token loop below consumes zero bytes,
            // which accidentally accepts trailing commas and missing object
            // values before the real parser sees them.
            if (text_[position_] == ',' || text_[position_] == ']' || text_[position_] == '}')
                return false;
            const auto start = position_;
            while (position_ < text_.size() && text_[position_] != ',' && text_[position_] != ']' &&
                   text_[position_] != '}' &&
                   !std::isspace(static_cast<unsigned char>(text_[position_])))
                ++position_;
            return position_ > start;
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

void append_canonical_json_string(std::string& out, std::string_view value) {
    out.push_back('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        switch (byte) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20) {
                out += "\\u00";
                out.push_back(hex[byte >> 4]);
                out.push_back(hex[byte & 0xf]);
            } else {
                out.push_back(static_cast<char>(byte));
            }
        }
    }
    out.push_back('"');
}

void append_canonical_json(const choc::value::ValueView& value, std::string& out) {
    if (value.isVoid()) {
        out += "null";
    } else if (value.isString()) {
        append_canonical_json_string(out, value.getString());
    } else if (value.isBool()) {
        out += value.getBool() ? "true" : "false";
    } else if (value.isInt()) {
        out += std::to_string(value.get<int64_t>());
    } else if (value.isFloat()) {
        out += choc::json::doubleToString(value.get<double>());
    } else if (value.isArray() || value.isVector()) {
        out.push_back('[');
        for (std::uint32_t i = 0; i < value.size(); ++i) {
            if (i)
                out.push_back(',');
            append_canonical_json(value[i], out);
        }
        out.push_back(']');
    } else if (value.isObject()) {
        std::vector<std::pair<std::string, std::uint32_t>> members;
        members.reserve(value.size());
        for (std::uint32_t i = 0; i < value.size(); ++i) {
            const auto member = value.getObjectMemberAt(i);
            members.emplace_back(std::string(member.name), i);
        }
        std::sort(members.begin(), members.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
        out.push_back('{');
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (i)
                out.push_back(',');
            append_canonical_json_string(out, members[i].first);
            out.push_back(':');
            append_canonical_json(value[members[i].first], out);
        }
        out.push_back('}');
    }
}

bool canonical_member_shape(const choc::value::ValueView& value,
                            std::initializer_list<std::string_view> allowed,
                            std::initializer_list<std::string_view> required) {
    if (!value.isObject())
        return false;
    const auto has = [&](std::string_view key) { return value.hasObjectMember(key); };
    for (std::uint32_t i = 0; i < value.size(); ++i) {
        const auto member = value.getObjectMemberAt(i);
        if (std::find(allowed.begin(), allowed.end(), member.name) == allowed.end())
            return false;
    }
    return std::all_of(required.begin(), required.end(), has);
}

bool canonical_key(const choc::value::ValueView& value) {
    if (!value.isString())
        return false;
    std::string normalized;
    return normalize_authored_identity_text(value.getString(), normalized) &&
           normalized == value.getString();
}

bool canonical_key_text(std::string_view value) {
    std::string normalized;
    return normalize_authored_identity_text(value, normalized) && normalized == value;
}

bool canonical_gain(std::string_view value) {
    if (value.size() != 12 || value.substr(0, 4) != "f32:")
        return false;
    for (const auto c : value.substr(4))
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f'))
            return false;
    const auto bits =
        static_cast<std::uint32_t>(std::stoul(std::string(value.substr(4)), nullptr, 16));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

bool canonical_channels(const choc::value::ValueView& value, std::vector<std::string>& output) {
    if (!value.isArray() || value.size() == 0)
        return false;
    output.clear();
    std::unordered_set<std::string> seen;
    for (std::uint32_t i = 0; i < value.size(); ++i) {
        if (!canonical_key(value[i]))
            return false;
        const std::string channel(value[i].getString());
        if (!seen.insert(channel).second)
            return false;
        output.push_back(channel);
    }
    return true;
}

bool canonical_endpoint(const choc::value::ValueView& value, std::string& node,
                        std::string& direction, std::string& port) {
    if (!canonical_member_shape(value, {"node", "direction", "kind", "port"},
                                {"node", "direction", "kind", "port"}) ||
        !canonical_key(value["node"]) || !canonical_key(value["port"]) ||
        !value["direction"].isString() || !value["kind"].isString() ||
        (value["direction"].getString() != "in" && value["direction"].getString() != "out") ||
        value["kind"].getString() != "audio")
        return false;
    node = value["node"].getString();
    direction = value["direction"].getString();
    port = value["port"].getString();
    return true;
}

std::string canonical_edge_key(std::string_view source_node, std::string_view source_direction,
                               std::string_view source_port, std::string_view dest_node,
                               std::string_view dest_direction, std::string_view dest_port) {
    std::string result{"[\"audio\",{\"direction\":"};
    append_canonical_json_string(result, source_direction);
    result += ",\"kind\":\"audio\",\"node\":";
    append_canonical_json_string(result, source_node);
    result += ",\"port\":";
    append_canonical_json_string(result, source_port);
    result += "},{\"direction\":";
    append_canonical_json_string(result, dest_direction);
    result += ",\"kind\":\"audio\",\"node\":";
    append_canonical_json_string(result, dest_node);
    result += ",\"port\":";
    append_canonical_json_string(result, dest_port);
    result += "},null]";
    return result;
}

bool strict_semantic_payload(std::string_view payload, choc::value::Value& root,
                             std::vector<std::string>& channel_names) {
    if (payload.find('\0') != std::string_view::npos || !JsonDuplicateScanner(payload).valid()) {
        return false;
    }
    try {
        root = choc::json::parse(std::string(payload));
    } catch (...) {
        return false;
    }
    std::string canonical;
    append_canonical_json(root, canonical);
    if (canonical != payload ||
        !canonical_member_shape(
            root, {"assets", "edges", "interfaces", "nodes", "policies", "profile"},
            {"assets", "edges", "interfaces", "nodes", "policies", "profile"}) ||
        !root["profile"].isString() || root["profile"].getString() != "pulp.graph.identity.v1" ||
        !root["assets"].isObject() || root["assets"].size() != 0) {
        return false;
    }
    const auto policies = root["policies"];
    if (!canonical_member_shape(
            policies,
            {"event_order", "fault", "feedback", "rate_conversion", "seed", "state_retention"},
            {"event_order", "fault", "feedback", "rate_conversion", "seed", "state_retention"}) ||
        policies["event_order"].getString() != "none" || policies["fault"].getString() != "fail" ||
        policies["feedback"].getString() != "forbid" ||
        policies["rate_conversion"].getString() != "none" || !policies["seed"].isVoid() ||
        policies["state_retention"].getString() != "none") {
        return false;
    }
    const auto nodes = root["nodes"];
    const auto edges = root["edges"];
    const auto interfaces = root["interfaces"];
    if (!nodes.isObject() || !edges.isObject() || !interfaces.isArray()) {
        return false;
    }
    if (nodes.size() == 0)
        return edges.size() == 0 && interfaces.size() == 0;
    if (interfaces.size() != 2) {
        return false;
    }

    std::unordered_map<std::string, std::string> kinds;
    std::unordered_map<std::string, std::vector<std::string>> port_channels;
    std::vector<std::string> graph_channels;
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        const auto member = nodes.getObjectMemberAt(i);
        if (!canonical_key_text(member.name) ||
            !canonical_member_shape(member.value,
                                    {"kind", "kind_contract", "implementation", "config",
                                     "initial_state", "ports", "parameter_bindings", "policies"},
                                    {"kind", "kind_contract", "implementation", "config",
                                     "initial_state", "ports", "parameter_bindings", "policies"})) {
            return false;
        }
        const auto node = member.value;
        if (!node["kind"].isString()) {
            return false;
        }
        const std::string kind(node["kind"].getString());
        if (kind != "audio_input" && kind != "audio_output" && kind != "gain") {
            return false;
        }
        const std::string revision = kind == "gain" ? "gain.v1" : "io.v1";
        const std::string implementation =
            kind == "audio_input" ? "pulp.builtin.audio-input.v1"
                                  : (kind == "audio_output" ? "pulp.builtin.audio-output.v1"
                                                            : "pulp.builtin.gain.v1");
        if (!node["kind_contract"].isString() || node["kind_contract"].getString() != revision ||
            !node["implementation"].isString() ||
            node["implementation"].getString() != implementation ||
            !node["parameter_bindings"].isObject() || node["parameter_bindings"].size() != 0 ||
            !node["policies"].isObject() || node["policies"].size() != 0 ||
            !canonical_member_shape(node["initial_state"], {"mode", "revision"},
                                    {"mode", "revision"}) ||
            node["initial_state"]["mode"].getString() != "default" ||
            node["initial_state"]["revision"].getString() != revision)
            return false;
        if (kind == "gain") {
            if (!canonical_member_shape(node["config"], {"gain"}, {"gain"}) ||
                !node["config"]["gain"].isString() ||
                !canonical_gain(node["config"]["gain"].getString())) {
                return false;
            }
        } else if (!node["config"].isObject() || node["config"].size() != 0) {
            return false;
        }
        if (!canonical_member_shape(node["ports"], {"in", "out"}, {"in", "out"})) {
            return false;
        }
        std::vector<std::string> active_channels;
        for (const auto direction : {std::string_view{"in"}, std::string_view{"out"}}) {
            const bool active = kind == "gain" || (kind == "audio_input" && direction == "out") ||
                                (kind == "audio_output" && direction == "in");
            const auto lookup = direction == "in" ? node["ports"]["in"] : node["ports"]["out"];
            const auto ports = lookup;
            const bool valid_ports =
                ports.isObject() &&
                (active ? (ports.size() == 1 &&
                           std::string(ports.getObjectMemberAt(0).name) == "audio")
                        : ports.size() == 0);
            if (!valid_ports) {
                return false;
            }
            if (active) {
                const auto port = ports["audio"];
                if (!canonical_member_shape(port, {"kind", "channels", "contract"},
                                            {"kind", "channels", "contract"}) ||
                    port["kind"].getString() != "audio" ||
                    port["contract"].getString() != "audio-port.v1" ||
                    !canonical_channels(port["channels"], active_channels)) {
                    return false;
                }
                if (port_channels.contains(member.name) &&
                    port_channels[member.name] != active_channels) {
                    return false;
                }
                port_channels[std::string(member.name)] = active_channels;
                if (graph_channels.empty())
                    graph_channels = active_channels;
                else if (graph_channels != active_channels) {
                    return false;
                }
            }
        }
        if (kind == "gain" &&
            node["ports"]["in"]["audio"]["channels"] != node["ports"]["out"]["audio"]["channels"]) {
            return false;
        }
        kinds.emplace(std::string(member.name), kind);
    }
    // D1/D2 remains the semantic authority.  Mono and wider channel layouts
    // are valid authored documents even though this first prepared-export
    // profile admits stereo only.  Keep parsing/validation separate from the
    // D4 profile gate so callers receive `unsupported_prepared_profile` rather
    // than a generic semantic-binding mismatch for a valid non-stereo graph.
    channel_names = std::move(graph_channels);
    std::unordered_set<std::string> interface_keys;
    std::size_t input_interfaces = 0, output_interfaces = 0;
    for (std::uint32_t i = 0; i < interfaces.size(); ++i) {
        const auto item = interfaces[i];
        if (!canonical_member_shape(item, {"key", "role", "kind", "endpoint", "channels"},
                                    {"key", "role", "kind", "endpoint", "channels"}) ||
            !canonical_key(item["key"]) ||
            !interface_keys.insert(std::string(item["key"].getString())).second ||
            !item["role"].isString() ||
            (item["role"].getString() != "input" && item["role"].getString() != "output") ||
            !item["kind"].isString() || item["kind"].getString() != "audio")
            return false;
        std::string node, direction, port;
        if (!canonical_endpoint(item["endpoint"], node, direction, port) || !kinds.contains(node) ||
            port != "audio" || direction != (item["role"].getString() == "input" ? "out" : "in") ||
            kinds[node] != (item["role"].getString() == "input" ? "audio_input" : "audio_output"))
            return false;
        std::vector<std::string> names;
        if (!canonical_channels(item["channels"], names) || names != channel_names)
            return false;
        if (item["role"].getString() == "input")
            ++input_interfaces;
        else
            ++output_interfaces;
    }
    if (input_interfaces != 1 || output_interfaces != 1)
        return false;
    std::unordered_map<std::string, std::size_t> indegree, outdegree;
    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    for (const auto& [name, _] : kinds)
        indegree[name] = outdegree[name] = 0;
    for (std::uint32_t i = 0; i < edges.size(); ++i) {
        const auto member = edges.getObjectMemberAt(i);
        const auto edge = member.value;
        if (!canonical_member_shape(
                edge,
                {"lane", "source", "destination", "semantic_order", "feedback", "channel_map"},
                {"lane", "source", "destination", "semantic_order", "feedback", "channel_map"}) ||
            !edge["lane"].isString() || edge["lane"].getString() != "audio" ||
            !edge["feedback"].isBool() || edge["feedback"].getBool() ||
            !edge["semantic_order"].isVoid())
            return false;
        std::string source_node, source_direction, source_port, destination_node,
            destination_direction, destination_port;
        if (!canonical_endpoint(edge["source"], source_node, source_direction, source_port) ||
            !canonical_endpoint(edge["destination"], destination_node, destination_direction,
                                destination_port) ||
            source_direction != "out" || destination_direction != "in" ||
            !kinds.contains(source_node) || !kinds.contains(destination_node) ||
            source_port != "audio" || destination_port != "audio" ||
            member.name != canonical_edge_key(source_node, source_direction, source_port,
                                              destination_node, destination_direction,
                                              destination_port))
            return false;
        const auto map = edge["channel_map"];
        if (!map.isArray() || map.size() != channel_names.size())
            return false;
        for (std::uint32_t c = 0; c < map.size(); ++c) {
            if (!map[c].isArray() || map[c].size() != 2 || !map[c][0].isString() ||
                !map[c][1].isString() || map[c][0].getString() != channel_names[c] ||
                map[c][1].getString() != channel_names[c])
                return false;
        }
        ++outdegree[source_node];
        ++indegree[destination_node];
        adjacency[source_node].push_back(destination_node);
    }
    for (const auto& [name, kind] : kinds) {
        const auto expected_in = kind == "audio_input" ? 0u : 1u;
        const auto expected_out = kind == "audio_output" ? 0u : 1u;
        if (indegree[name] != expected_in || outdegree[name] != expected_out)
            return false;
    }
    std::unordered_map<std::string, std::uint8_t> marks;
    const auto visit = [&](const auto& self, const std::string& name) -> bool {
        auto& mark = marks[name];
        if (mark == 1)
            return false;
        if (mark == 2)
            return true;
        mark = 1;
        for (const auto& next : adjacency[name])
            if (!self(self, next))
                return false;
        mark = 2;
        return true;
    };
    for (const auto& [name, _] : kinds)
        if (!visit(visit, name))
            return false;
    return true;
}

using detail::prepared_kind;

std::string prepared_custom_key(std::string_view type_id, int version) {
    std::string key(type_id);
    key.push_back('\x1f');
    key += std::to_string(version);
    return key;
}

// Marks a PluginSlot admitted by a prepared edit as graph-owned and balances
// its lifecycle only after every authoring/snapshot shared owner has drained.
class PreparedOwnedBuiltInSlot final : public PluginSlot {
  public:
    explicit PreparedOwnedBuiltInSlot(std::unique_ptr<PluginSlot> inner)
        : inner_(std::move(inner)) {}
    ~PreparedOwnedBuiltInSlot() override {
        try {
            release();
        } catch (...) {
        }
    }

    const PluginInfo& info() const override { return inner_->info(); }
    bool is_loaded() const override { return inner_->is_loaded(); }
    bool prepare(double sample_rate, int maximum_block_size) override {
        prepare_entered_ = true;
        return inner_->prepare(sample_rate, maximum_block_size);
    }
    void release() override {
        if (prepare_entered_ && !released_) {
            released_ = true;
            inner_->release();
        }
    }
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 const midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                 const ParameterEventQueue& events, int frames) override {
        inner_->process(output, input, midi_in, midi_out, events, frames);
    }
    std::vector<HostParamInfo> parameters() const override { return inner_->parameters(); }
    float get_parameter(std::uint32_t id) const override { return inner_->get_parameter(id); }
    void set_parameter(std::uint32_t id, float value) override {
        inner_->set_parameter(id, value);
    }
    void set_bypass(bool bypassed) override { inner_->set_bypass(bypassed); }
    bool is_bypassed() const override { return inner_->is_bypassed(); }
    BypassSurface bypass_surface() const override { return inner_->bypass_surface(); }
    std::vector<std::uint8_t> save_state() const override { return inner_->save_state(); }
    bool restore_state(const std::vector<std::uint8_t>& data) override {
        return inner_->restore_state(data);
    }
    bool has_editor() const override { return inner_->has_editor(); }
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    void* create_editor_view() override { return inner_->create_editor_view(); }
    void destroy_editor_view() override { inner_->destroy_editor_view(); }
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif
    std::unique_ptr<HostedEditor> create_hosted_editor(void* parent) override {
        return inner_->create_hosted_editor(parent);
    }
    void destroy_hosted_editor(std::unique_ptr<HostedEditor> editor) override {
        inner_->destroy_hosted_editor(std::move(editor));
    }
    int latency_samples() const override { return inner_->latency_samples(); }
    int tail_samples() const override { return inner_->tail_samples(); }
    // Forwarded alongside the number it qualifies. Without this the wrapper
    // inherits the base default of `Available`, so a backend that cannot read a
    // latency at all is reported as confidently answering zero, and a caller
    // that checks before reading gets a fact where none exists.
    LatencyQuery latency_query() const override { return inner_->latency_query(); }
    bool set_host_settings(const PluginHostSettings& settings) override {
        return inner_->set_host_settings(settings);
    }
    PluginHostSettings host_settings() const override {
        return inner_->host_settings();
    }
    std::size_t drain_parameter_edits(std::span<PluginParameterEdit> out) override {
        return inner_->drain_parameter_edits(out);
    }
    std::uint64_t dropped_parameter_edit_count() const noexcept override {
        return inner_->dropped_parameter_edit_count();
    }
    std::uint64_t restart_request_count() const noexcept override {
        return inner_->restart_request_count();
    }
    std::uint64_t state_change_count() const noexcept override {
        return inner_->state_change_count();
    }

  private:
    std::unique_ptr<PluginSlot> inner_;
    bool prepare_entered_ = false;
    bool released_ = false;
};

} // namespace

namespace detail {

bool is_prepared_owned_built_in_slot(const PluginSlot* slot) noexcept {
    return dynamic_cast<const PreparedOwnedBuiltInSlot*>(slot) != nullptr;
}

} // namespace detail

std::unique_ptr<SignalGraph::PreparedTopologyEdit> SignalGraph::begin_prepared_topology_edit() {
    if (spectral_access_closed_.load(std::memory_order_seq_cst)) {
        spectral_error_.store(SpectralError::Busy, std::memory_order_relaxed);
        return {};
    }
    return std::unique_ptr<PreparedTopologyEdit>(new PreparedTopologyEdit(*this));
}

SignalGraph::PreparedTopologyEdit::PreparedTopologyEdit(SignalGraph& owner)
    : owner_(&owner), candidate_(std::make_unique<SignalGraph>()) {
    GraphMutationLock owner_lock(owner);

    if (owner.refuse_isolated_mode_locked_("prepared topology edit")) {
        mutation_failed_ = true;
        return;
    }
    candidate_->nodes_ = owner.nodes_;
    candidate_->processor_nodes_ = owner.processor_nodes_;
    candidate_->connections_ = owner.connections_;
    candidate_->graph_event_edge_ = owner.graph_event_edge_;
    candidate_->connection_identities_ = owner.connection_identities_;
    candidate_->next_connection_identity_ = owner.next_connection_identity_;
    candidate_->custom_node_types_ = owner.custom_node_types_;
    candidate_->sample_kernel_types_ = owner.sample_kernel_types_;
    candidate_->custom_node_diagnostics_ = owner.custom_node_diagnostics_;
    candidate_->sample_region_definitions_ = owner.sample_region_definitions_;
    candidate_->sample_region_parameter_binding_ = owner.sample_region_parameter_binding_;
    sample_region_parameter_binding_ = owner.sample_region_parameter_binding_;
    candidate_->sample_region_proof_block_size_ = owner.sample_region_proof_block_size_;
    candidate_->custom_registry_generation_ = owner.custom_registry_generation_;
    candidate_->next_id_ = owner.next_id_;
    candidate_->limits_ = owner.limits_;
    candidate_->hosted_plugin_process_mode_ = owner.hosted_plugin_process_mode_;
    candidate_->last_plugin_prepare_rate_ = owner.last_plugin_prepare_rate_;
    candidate_->last_plugin_prepare_block_ = owner.last_plugin_prepare_block_;
    candidate_->prepared_plugin_meta_ = owner.prepared_plugin_meta_;
    candidate_->exact_parameter_event_claims_ = owner.exact_parameter_event_claims_;
    candidate_->canonical_executor_routing_enabled_.store(
        owner.canonical_executor_routing_enabled_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    candidate_->parallel_routing_enabled_.store(
        owner.parallel_routing_enabled_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    candidate_->anticipation_enabled_.store(
        owner.anticipation_enabled_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    candidate_->execution_selection_generation_.store(
        owner.execution_selection_generation_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    candidate_->desired_live_dsp_telemetry_enabled_.store(
        owner.desired_live_dsp_telemetry_enabled_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    candidate_->prepared_edit_origin_ = &owner;

    baseline_node_ids_.reserve(owner.nodes_.size());
    for (const auto& n : owner.nodes_)
        baseline_node_ids_.insert(n.id);
    baseline_registry_keys_.reserve(owner.custom_node_types_.size());
    for (const auto& [key, _] : owner.custom_node_types_) {
        baseline_registry_keys_.insert(key);
    }

    base_authoring_generation_ = owner.authoring_generation_;
    base_live_ =
        owner.spectral_mode_.load(std::memory_order_acquire) ? nullptr : owner.live_slot_.live();
    base_canonical_routing_ =
        owner.canonical_executor_routing_enabled_.load(std::memory_order_relaxed);
    base_parallel_routing_ = owner.parallel_routing_enabled_.load(std::memory_order_relaxed);
    base_anticipation_ = owner.anticipation_enabled_.load(std::memory_order_relaxed);
    base_execution_selection_generation_ =
        owner.execution_selection_generation_.load(std::memory_order_relaxed);
}

// Shared by the new receipt decoder; the original D1/D2 validator remains
// the single authority and its canonical bytes are unchanged.
bool validate_resolved_semantic_payload(std::string_view payload) {
    choc::value::Value root;
    std::vector<std::string> channels;
    return payload.size() <= 4 * 1024 * 1024 && strict_semantic_payload(payload, root, channels) &&
           (root["nodes"].size() == 0 || channels.size() == 2);
}

PreparedDescriptionResult SignalGraph::PreparedTopologyEdit::bind_authored_identity(
    std::string_view canonical_semantic_payload, const AuthoredRuntimeBinding& bindings) {
    if (prepare_attempted_ || committed_ || mutation_failed_)
        return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::AuthoredBindingStale);
    if (canonical_semantic_payload.empty())
        return PreparedDescriptionResult::refused(
            PreparedDescriptionRefusal::LogicalIdentityMissing);

    try {
        const auto result = std::make_shared<PreparedDescriptionData>();
        // The prepared record carries the exact D1/D2 semantic profile.  The
        // closed prepared shape is a separate D4 admission decision below and must
        // not masquerade as a replacement semantic profile.
        result->semantic_profile = "pulp.graph.identity.v1";
        result->semantic_payload.assign(canonical_semantic_payload);

        choc::value::Value authored_root;
        std::vector<std::string> channel_names;
        try {
            if (!strict_semantic_payload(canonical_semantic_payload, authored_root,
                                         channel_names)) {
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);
            }
        } catch (const std::bad_alloc&) {
            return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::AllocationFailed);
        } catch (const std::exception&) {
            return PreparedDescriptionResult::refused(
                PreparedDescriptionRefusal::AuthoredBindingMismatch);
        }
        const auto& nodes = candidate_->nodes_;
        const auto authored_interfaces = authored_root["interfaces"];
        if (!nodes.empty() && channel_names.size() != 2)
            return PreparedDescriptionResult::refused(
                PreparedDescriptionRefusal::UnsupportedPreparedProfile);
        constexpr std::string_view digest_prefix = "pulp.graph.semantic.v1";
        std::string digest_preimage;
        digest_preimage.reserve(digest_prefix.size() + 1 + canonical_semantic_payload.size());
        digest_preimage.append(digest_prefix.data(), digest_prefix.size());
        digest_preimage.push_back('\0');
        digest_preimage.append(canonical_semantic_payload.data(),
                               canonical_semantic_payload.size());
        result->semantic_digest = "sha256:" + runtime::sha256_hex(digest_preimage);
        if (nodes.empty()) {
            if (!bindings.nodes.empty() || !bindings.channels.empty() ||
                !bindings.interfaces.empty())
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);
            prepared_description_ = result;
            authored_binding_stale_ = false;
            return {PreparedDescriptionRefusal::None, result};
        }

        std::size_t inputs = 0, outputs = 0;
        for (const auto& node : nodes) {
            if (!prepared_kind(node.type) ||
                ((node.type == NodeType::AudioInput || node.type == NodeType::AudioOutput ||
                  node.type == NodeType::Gain) &&
                 (node.num_input_ports > 2 || node.num_output_ports > 2)))
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::UnsupportedPreparedProfile);
            if (node.type == NodeType::AudioInput)
                ++inputs;
            if (node.type == NodeType::AudioOutput)
                ++outputs;
            if ((node.type == NodeType::AudioInput &&
                 (node.num_input_ports != 0 || node.num_output_ports != 2)) ||
                (node.type == NodeType::AudioOutput &&
                 (node.num_input_ports != 2 || node.num_output_ports != 0)) ||
                (node.type == NodeType::Gain &&
                 (node.num_input_ports != 2 || node.num_output_ports != 2)))
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::UnsupportedPreparedProfile);
        }
        if (inputs != 1 || outputs != 1)
            return PreparedDescriptionResult::refused(
                PreparedDescriptionRefusal::UnsupportedPreparedProfile);

        // D4 admits one closed stereo path only.  A node-level bijection is not
        // enough here: every active mono port must have exactly one audio edge,
        // and no event/automation/sidechain/feedback lane may be smuggled into the
        // prepared export.  Count the candidate's real port edges before comparing
        // it with the authored document so disconnected, fan-in, and split graphs
        // cannot be relabelled as this profile.
        std::unordered_map<NodeId, std::array<std::uint8_t, 2>> input_edge_counts;
        std::unordered_map<NodeId, std::array<std::uint8_t, 2>> output_edge_counts;
        for (const auto& node : nodes) {
            input_edge_counts.emplace(node.id, std::array<std::uint8_t, 2>{});
            output_edge_counts.emplace(node.id, std::array<std::uint8_t, 2>{});
        }
        const auto find_node = [&](NodeId id) {
            return std::find_if(nodes.begin(), nodes.end(),
                                [&](const GraphNode& node) { return node.id == id; });
        };
        for (const auto& connection : candidate_->connections_) {
            if (connection.feedback || connection.midi || connection.automation ||
                connection.audio_rate_modulation || connection.sidechain || connection.spectral ||
                connection.control_signal || connection.source_port > 1 || connection.dest_port > 1)
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::UnsupportedPreparedProfile);
            const auto source = find_node(connection.source_node);
            const auto destination = find_node(connection.dest_node);
            if (source == nodes.end() || destination == nodes.end() ||
                connection.source_port >= static_cast<PortIndex>(source->num_output_ports) ||
                connection.dest_port >= static_cast<PortIndex>(destination->num_input_ports) ||
                connection.source_port != connection.dest_port)
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);
            auto& source_count = output_edge_counts.at(source->id)[connection.source_port];
            auto& destination_count = input_edge_counts.at(destination->id)[connection.dest_port];
            if (source_count++ != 0 || destination_count++ != 0)
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::UnsupportedPreparedProfile);
        }
        for (const auto& node : nodes) {
            const auto& inputs_for_node = input_edge_counts.at(node.id);
            const auto& outputs_for_node = output_edge_counts.at(node.id);
            for (std::uint32_t port = 0; port < 2; ++port) {
                const auto expected_input = port < static_cast<std::uint32_t>(node.num_input_ports);
                const auto expected_output =
                    port < static_cast<std::uint32_t>(node.num_output_ports);
                if ((expected_input ? inputs_for_node[port] != 1 : inputs_for_node[port] != 0) ||
                    (expected_output ? outputs_for_node[port] != 1 : outputs_for_node[port] != 0))
                    return PreparedDescriptionResult::refused(
                        PreparedDescriptionRefusal::UnsupportedPreparedProfile);
            }
        }

        std::unordered_map<NodeId, std::string> keys;
        std::unordered_set<std::string> authored_keys;
        for (const auto& binding : bindings.nodes) {
            std::string normalized_binding_key;
            if (binding.node_id == 0 || binding.authored_key.empty() ||
                !normalize_authored_identity_text(binding.authored_key, normalized_binding_key) ||
                normalized_binding_key != binding.authored_key ||
                !keys.emplace(binding.node_id, binding.authored_key).second ||
                !authored_keys.insert(binding.authored_key).second)
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);
        }
        if (keys.size() != nodes.size())
            return PreparedDescriptionResult::refused(
                PreparedDescriptionRefusal::AuthoredBindingMismatch);
        for (const auto& node : nodes)
            if (keys.find(node.id) == keys.end())
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);
        const auto authored_nodes = authored_root["nodes"];
        if (!authored_nodes.isObject() || authored_nodes.size() != nodes.size())
            return PreparedDescriptionResult::refused(
                PreparedDescriptionRefusal::AuthoredBindingMismatch);
        for (const auto& [id, key] : keys) {
            const auto authored_node = authored_nodes[key];
            if (!authored_node.isObject() || !authored_node.hasObjectMember("kind"))
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);
            const auto node = std::find_if(nodes.begin(), nodes.end(),
                                           [&](const GraphNode& n) { return n.id == id; });
            std::string kind;
            switch (node->type) {
            case NodeType::AudioInput:
                kind = "audio_input";
                break;
            case NodeType::AudioOutput:
                kind = "audio_output";
                break;
            case NodeType::Gain:
                kind = "gain";
                break;
            default:
                break;
            }
            if (!authored_node["kind"].isString() || authored_node["kind"].getString() != kind)
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);
            if (node->type == NodeType::Gain) {
                const auto config = authored_node["config"];
                if (!config.isObject())
                    return PreparedDescriptionResult::refused(
                        PreparedDescriptionRefusal::AuthoredBindingMismatch);
                const auto gain = config["gain"];
                if (!gain.isString())
                    return PreparedDescriptionResult::refused(
                        PreparedDescriptionRefusal::AuthoredBindingMismatch);
                constexpr char hex[] = "0123456789abcdef";
                std::string expected = "f32:";
                const auto bits = std::bit_cast<std::uint32_t>(node->gain);
                for (int shift = 28; shift >= 0; shift -= 4)
                    expected.push_back(hex[(bits >> shift) & 0xf]);
                if (gain.getString() != expected)
                    return PreparedDescriptionResult::refused(
                        PreparedDescriptionRefusal::AuthoredBindingMismatch);
            }
        }

        const auto authored_edges = authored_root["edges"];
        std::unordered_set<std::string> expected_edge_pairs;
        std::unordered_map<std::string, std::uint8_t> runtime_edge_masks;
        std::unordered_set<std::string> runtime_edge_keys;
        for (const auto& connection : candidate_->connections_)
            expected_edge_pairs.insert(std::to_string(connection.source_node) + ":" +
                                       std::to_string(connection.dest_node));
        if (!authored_edges.isObject() || authored_edges.size() != expected_edge_pairs.size())
            return PreparedDescriptionResult::refused(
                PreparedDescriptionRefusal::AuthoredBindingMismatch);
        for (const auto& connection : candidate_->connections_) {
            const auto source = std::find_if(nodes.begin(), nodes.end(), [&](const GraphNode& n) {
                return n.id == connection.source_node;
            });
            const auto destination =
                std::find_if(nodes.begin(), nodes.end(),
                             [&](const GraphNode& n) { return n.id == connection.dest_node; });
            if (source == nodes.end() || destination == nodes.end() || connection.feedback ||
                connection.source_port > 1 || connection.dest_port > 1)
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::UnsupportedPreparedProfile);
            const auto source_key = keys.at(source->id);
            const auto destination_key = keys.at(destination->id);
            if (connection.source_port != connection.dest_port ||
                !runtime_edge_keys
                     .insert(source_key + ":" + destination_key + ":" +
                             std::to_string(connection.source_port))
                     .second)
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);
            const auto pair_key = source_key + ":" + destination_key;
            runtime_edge_masks[pair_key] = static_cast<std::uint8_t>(
                runtime_edge_masks[pair_key] | (1u << connection.source_port));
            const std::string edge_key =
                canonical_edge_key(source_key, "out", "audio", destination_key, "in", "audio");
            const auto edge = authored_edges[edge_key];
            if (!edge.isObject() || !edge["channel_map"].isArray() ||
                edge["channel_map"].size() != channel_names.size())
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);
            for (std::size_t i = 0; i < channel_names.size(); ++i) {
                const auto pair = edge["channel_map"][static_cast<std::uint32_t>(i)];
                if (!pair.isArray() || pair.size() != 2 || !pair[0].isString() ||
                    !pair[1].isString() || pair[0].getString() != channel_names[i] ||
                    pair[1].getString() != channel_names[i])
                    return PreparedDescriptionResult::refused(
                        PreparedDescriptionRefusal::AuthoredBindingMismatch);
            }
        }
        for (const auto& [_, mask] : runtime_edge_masks)
            if (mask != 0x3u)
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);

        std::unordered_set<std::string> channel_keys;
        for (const auto& binding : bindings.channels) {
            if (binding.authored_port != "audio" || binding.authored_channel.empty() ||
                binding.authored_ordinal > 1 ||
                binding.authored_channel != channel_names[binding.authored_ordinal] ||
                binding.runtime_port_index != binding.authored_ordinal ||
                !channel_keys
                     .insert(std::to_string(binding.node_id) + ":" +
                             (binding.output ? "out:" : "in:") + binding.authored_channel)
                     .second)
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);
            const auto node = std::find_if(nodes.begin(), nodes.end(), [&](const GraphNode& n) {
                return n.id == binding.node_id;
            });
            if (node == nodes.end() || binding.authored_ordinal >= 2 ||
                (binding.output ? node->num_output_ports : node->num_input_ports) != 2)
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);
        }
        std::size_t required_channels = 0;
        for (const auto& node : nodes)
            required_channels +=
                static_cast<std::size_t>(node.num_input_ports + node.num_output_ports);
        if (bindings.channels.size() != required_channels)
            return PreparedDescriptionResult::refused(
                PreparedDescriptionRefusal::AuthoredBindingMismatch);

        std::unordered_set<std::string> interfaces;
        if (authored_interfaces.size() != 2)
            return PreparedDescriptionResult::refused(
                PreparedDescriptionRefusal::AuthoredBindingMismatch);
        std::size_t authored_inputs = 0;
        std::size_t authored_outputs = 0;
        for (std::size_t i = 0; i < authored_interfaces.size(); ++i) {
            const auto entry = authored_interfaces[static_cast<std::uint32_t>(i)];
            if (!entry.isObject() || !entry["channels"].isArray() ||
                entry["channels"].size() != channel_names.size() || !entry["role"].isString() ||
                !entry["endpoint"].isObject())
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);
            for (std::size_t channel = 0; channel < channel_names.size(); ++channel) {
                if (!entry["channels"][static_cast<std::uint32_t>(channel)].isString() ||
                    entry["channels"][static_cast<std::uint32_t>(channel)].getString() !=
                        channel_names[channel])
                    return PreparedDescriptionResult::refused(
                        PreparedDescriptionRefusal::AuthoredBindingMismatch);
            }
            if (entry["role"].getString() == "input")
                ++authored_inputs;
            else if (entry["role"].getString() == "output")
                ++authored_outputs;
            else
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);
        }
        if (authored_inputs != 1 || authored_outputs != 1)
            return PreparedDescriptionResult::refused(
                PreparedDescriptionRefusal::AuthoredBindingMismatch);
        for (const auto& binding : bindings.interfaces) {
            std::string normalized_interface_key;
            if (binding.interface_key.empty() ||
                !normalize_authored_identity_text(binding.interface_key,
                                                  normalized_interface_key) ||
                normalized_interface_key != binding.interface_key ||
                binding.authored_port != "audio" || binding.authored_ordinal > 1 ||
                binding.authored_channel.empty() ||
                binding.authored_channel != channel_names[binding.authored_ordinal] ||
                binding.runtime_bus_index != 0 || binding.runtime_channel_index > 1 ||
                binding.authored_ordinal != binding.runtime_channel_index ||
                !interfaces
                     .insert(binding.interface_key + ":" + (binding.output ? "out:" : "in:") +
                             std::to_string(binding.authored_ordinal))
                     .second)
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);
            const auto node = std::find_if(nodes.begin(), nodes.end(), [&](const GraphNode& n) {
                return n.id == binding.node_id;
            });
            if (node == nodes.end() || (binding.output ? node->type != NodeType::AudioOutput
                                                       : node->type != NodeType::AudioInput))
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);
            const auto semantic_entry = [&]() -> choc::value::ValueView {
                for (std::uint32_t i = 0; i < authored_interfaces.size(); ++i) {
                    const auto candidate = authored_interfaces[i];
                    if (candidate.isObject() && candidate["key"].isString() &&
                        candidate["key"].getString() == binding.interface_key)
                        return candidate;
                }
                return {};
            }();
            if (semantic_entry.isVoid() ||
                semantic_entry["role"].getString() != (binding.output ? "output" : "input") ||
                !semantic_entry["endpoint"].isObject() ||
                semantic_entry["endpoint"]["node"].getString() != keys.at(binding.node_id) ||
                semantic_entry["endpoint"]["port"].getString() != binding.authored_port ||
                semantic_entry["endpoint"]["direction"].getString() !=
                    (binding.output ? "in" : "out"))
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);
        }
        if (bindings.interfaces.size() != 4)
            return PreparedDescriptionResult::refused(
                PreparedDescriptionRefusal::AuthoredBindingMismatch);

        result->channels = channel_names;
        // Preserve the authored interface order in the detached record.  Binding
        // vectors are lookup inputs and may arrive in any order; using that order
        // would make equivalent bindings produce different portable exports.
        for (std::uint32_t index = 0; index < authored_interfaces.size(); ++index) {
            const auto entry = authored_interfaces[index];
            const bool output = entry["role"].getString() == "output";
            const auto interface =
                std::find_if(bindings.interfaces.begin(), bindings.interfaces.end(),
                             [&](const AuthoredInterfaceChannelBinding& binding) {
                                 return binding.interface_key == entry["key"].getString() &&
                                        binding.output == output && binding.authored_ordinal == 0;
                             });
            if (interface == bindings.interfaces.end())
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::AuthoredBindingMismatch);
            std::vector<std::uint32_t> runtime_channel_indices;
            runtime_channel_indices.reserve(channel_names.size());
            for (std::uint32_t channel = 0; channel < channel_names.size(); ++channel) {
                const auto channel_binding =
                    std::find_if(bindings.interfaces.begin(), bindings.interfaces.end(),
                                 [&](const AuthoredInterfaceChannelBinding& binding) {
                                     return binding.interface_key == entry["key"].getString() &&
                                            binding.output == output &&
                                            binding.authored_ordinal == channel;
                                 });
                if (channel_binding == bindings.interfaces.end())
                    return PreparedDescriptionResult::refused(
                        PreparedDescriptionRefusal::AuthoredBindingMismatch);
                runtime_channel_indices.push_back(channel_binding->runtime_channel_index);
            }
            result->interfaces.push_back({interface->interface_key, keys.at(interface->node_id),
                                          interface->authored_port, interface->output,
                                          channel_names, interface->runtime_bus_index,
                                          std::move(runtime_channel_indices)});
        }
        result->nodes.reserve(nodes.size());
        for (const auto& node : nodes) {
            PreparedDescriptionNode description{
                keys.at(node.id),
                prepared_kind(node.type),
                node.type == NodeType::Gain ? "gain.v1" : "io.v1",
                node.type == NodeType::AudioInput
                    ? "pulp.builtin.audio-input.v1"
                    : (node.type == NodeType::AudioOutput ? "pulp.builtin.audio-output.v1"
                                                          : "pulp.builtin.gain.v1"),
                static_cast<std::uint32_t>(node.num_input_ports),
                static_cast<std::uint32_t>(node.num_output_ports),
                node.type == NodeType::Gain ? std::bit_cast<std::uint32_t>(node.gain) : 0u,
                0u,
                0u,
                false,
                false,
                false};
            result->nodes.push_back(std::move(description));
            result->schedule.push_back(keys.at(node.id));
        }
        for (const auto& connection : candidate_->connections_) {
            const auto source = std::find_if(nodes.begin(), nodes.end(), [&](const GraphNode& n) {
                return n.id == connection.source_node;
            });
            const auto destination =
                std::find_if(nodes.begin(), nodes.end(),
                             [&](const GraphNode& n) { return n.id == connection.dest_node; });
            if (source == nodes.end() || destination == nodes.end() || connection.feedback ||
                connection.source_port > 1 || connection.dest_port > 1)
                return PreparedDescriptionResult::refused(
                    PreparedDescriptionRefusal::UnsupportedPreparedProfile);
            result->edges.push_back(
                {keys.at(source->id) + ":" + std::to_string(connection.source_port),
                 keys.at(destination->id) + ":" + std::to_string(connection.dest_port),
                 connection.source_port, connection.dest_port, "copy", 0, 0, connection.feedback,
                 true, "copy"});
        }
        prepared_description_ = result;
        authored_binding_stale_ = false;
        return {PreparedDescriptionRefusal::None, result};
    } catch (const std::bad_alloc&) {
        return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::AllocationFailed);
    } catch (...) {
        return PreparedDescriptionResult::refused(
            PreparedDescriptionRefusal::AuthoredBindingMismatch);
    }
}

PreparedDescriptionResult
SignalGraph::PreparedTopologyEdit::build_prepared_description_(double sample_rate,
                                                               int max_block_size) const {
    if (!prepared_description_ || !std::isfinite(sample_rate) || sample_rate <= 0.0 ||
        max_block_size < 1 || max_block_size > 4096)
        return PreparedDescriptionResult::refused(
            prepared_description_ ? PreparedDescriptionRefusal::UnsupportedPreparedProfile
                                  : PreparedDescriptionRefusal::LogicalIdentityMissing);
    try {
        auto copy = std::make_shared<PreparedDescriptionData>(*prepared_description_);
        copy->sample_rate = sample_rate;
        copy->max_block_frames = static_cast<std::uint64_t>(max_block_size);
        return {PreparedDescriptionRefusal::None, std::move(copy)};
    } catch (const std::bad_alloc&) {
        return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::AllocationFailed);
    } catch (...) {
        return PreparedDescriptionResult::refused(
            PreparedDescriptionRefusal::AuthoredBindingMismatch);
    }
}

SignalGraph::PreparedTopologyEdit::~PreparedTopologyEdit() {
    prepared_snapshot_.reset();
    if (!committed_) {
        GraphMutationLock owner_lock(*owner_);
        (void)rollback_quiesced_lifecycles_locked_();
        owner_lock.unlock();
        release_new_custom_instances_();
    }
}

bool SignalGraph::PreparedTopologyEdit::base_is_current_locked_() const {
    owner_->assert_graph_mutation_locked_();
    return owner_->authoring_generation_ == base_authoring_generation_ &&
           (owner_->spectral_mode_.load(std::memory_order_acquire)
                ? !owner_->spectral_access_closed_.load(std::memory_order_seq_cst)
                : owner_->live_slot_.live() == base_live_) &&
           owner_->canonical_executor_routing_enabled_.load(std::memory_order_relaxed) ==
               base_canonical_routing_ &&
           owner_->parallel_routing_enabled_.load(std::memory_order_relaxed) ==
               base_parallel_routing_ &&
           owner_->anticipation_enabled_.load(std::memory_order_relaxed) == base_anticipation_ &&
           owner_->execution_selection_generation_.load(std::memory_order_relaxed) ==
               base_execution_selection_generation_;
}

bool SignalGraph::PreparedTopologyEdit::is_new_node_(NodeId id) const {
    return baseline_node_ids_.find(id) == baseline_node_ids_.end();
}

std::optional<SignalGraph::PreparedTopologyEdit::Result>
SignalGraph::PreparedTopologyEdit::baseline_removal_rejection_locked_() const {
    owner_->assert_graph_mutation_locked_();
    for (const auto& baseline : owner_->nodes_) {
        const bool retained = std::any_of(
            candidate_->nodes_.begin(), candidate_->nodes_.end(),
            [&](const GraphNode& node) { return node.id == baseline.id; });
        if (retained)
            continue;
        if (baseline.type == NodeType::Plugin) {
            const auto processor = owner_->processor_nodes_.find(baseline.id);
            // Processor lifetimes ride the compiled snapshots. An old pinned
            // generation keeps its instance until control-thread retirement;
            // external or unowned slots still require the stopped lifecycle.
            const bool graph_owned_processor = !baseline.plugin &&
                                               processor != owner_->processor_nodes_.end() &&
                                               processor->second && processor->second->instance;
            if (!graph_owned_processor &&
                dynamic_cast<const PreparedOwnedBuiltInSlot*>(baseline.plugin.get()) == nullptr)
                return Result::BaselinePluginRemovalRequiresRelease;
        }
        if (baseline.type == NodeType::Custom) {
            const auto type = owner_->custom_node_types_.find(
                prepared_custom_key(baseline.custom_type_id, baseline.custom_type_version));
            if (type != owner_->custom_node_types_.end() && type->second.release)
                return Result::BaselineCustomRemovalRequiresRelease;
        }
    }
    return std::nullopt;
}

bool SignalGraph::PreparedTopologyEdit::rollback_quiesced_lifecycles_locked_() noexcept {
    owner_->assert_graph_mutation_locked_();
    if (!quiesced_lifecycles_dirty_)
        return true;

    bool restored = true;
    if (base_live_ != nullptr) {
        for (const auto& retained : quiesced_processors_) {
            format::PrepareContext context;
            context.sample_rate = base_live_->sample_rate;
            context.max_buffer_size = base_live_->max_block_size;
            context.input_channels = retained.input_channels;
            context.output_channels = retained.output_channels;
            if (!retained.processor->prepare(context))
                restored = false;
        }
        for (const auto& retained : quiesced_plugins_) {
            if (!retained.touched)
                continue;
            try {
                if (!retained.plugin->prepare(base_live_->sample_rate,
                                              base_live_->max_block_size)) {
                    restored = false;
                }
            } catch (...) {
                restored = false;
            }
        }
        for (const auto& retained : quiesced_customs_) {
            if (!retained.touched)
                continue;
            if (!retained.prepare)
                continue;
            try {
                retained.prepare(retained.instance.get(), base_live_->sample_rate,
                                 base_live_->max_block_size);
            } catch (...) {
                restored = false;
            }
        }
    } else {
        // The base graph had no prepared lifecycle. Undo only candidate prepare
        // callbacks that were entered; untouched retained objects were never
        // acquired and must not receive a release callback.
        for (const auto& retained : quiesced_processors_) {
            if (!retained.processor->release())
                restored = false;
        }
        for (const auto& retained : quiesced_plugins_) {
            if (!retained.touched)
                continue;
            try {
                retained.plugin->release();
            } catch (...) {
                restored = false;
            }
        }
        for (const auto& retained : quiesced_customs_) {
            if (!retained.touched)
                continue;
            if (!retained.release) {
                restored = false;
                continue;
            }
            try {
                retained.release(retained.instance.get());
            } catch (...) {
                restored = false;
            }
        }
    }

    quiesced_lifecycles_dirty_ = false;
    prepared_snapshot_.reset();
    quiesced_processors_.clear();

    // A different live generation cannot safely keep running after its shared
    // objects were restored to this edit's older base dimensions.
    if (owner_->live_slot_.live() != base_live_)
        restored = false;
    if (!restored) {
        owner_->live_slot_.unpublish();
        owner_->total_latency_samples_.store(0, std::memory_order_relaxed);
        owner_->clear_prepared_stats_locked_();
    }
    return restored;
}

void SignalGraph::PreparedTopologyEdit::release_new_custom_instances_() noexcept {
    if (!candidate_)
        return;
    GraphMutationLock candidate_lock(*candidate_);
    for (NodeId id : prepared_new_custom_ids_) {
        auto* n = candidate_->node_mut_locked_(id);
        if (n == nullptr || n->type != NodeType::Custom || !n->custom_instance) {
            continue;
        }
        const auto* type = candidate_->custom_node_type(n->custom_type_id, n->custom_type_version);
        if (type != nullptr && type->release) {
            type->release(n->custom_instance.get());
        }
    }
    prepared_new_custom_ids_.clear();
}

bool SignalGraph::PreparedTopologyEdit::register_custom_node_type(CustomNodeType type) {
    if (mutation_failed_ || committed_ || prepare_attempted_ ||
        !type.is_valid_registration()) {
        mutation_failed_ = true;
        return false;
    }
    const auto key = prepared_custom_key(type.type_id, type.version);
    const auto generation_before = candidate_->custom_registry_generation_;
    const bool ok = candidate_->register_custom_node_type(std::move(type));
    if (!ok) {
        mutation_failed_ = true;
    } else if (candidate_->custom_registry_generation_ != generation_before &&
               baseline_registry_keys_.count(key) != 0) {
        replaced_registry_keys_.insert(key);
    }
    return ok;
}

bool SignalGraph::PreparedTopologyEdit::register_custom_node_type(
    CustomNodeType type, SampleKernelDescriptor sample_kernel) {
    if (mutation_failed_ || committed_ || prepare_attempted_ || !type.is_valid_registration() ||
        !sample_kernel.is_valid_registration()) {
        mutation_failed_ = true;
        return false;
    }
    const bool ok =
        candidate_->register_custom_node_type(std::move(type), std::move(sample_kernel));
    if (!ok) {
        mutation_failed_ = true;
    }
    return ok;
}

bool SignalGraph::PreparedTopologyEdit::unregister_custom_node_type(std::string_view type_id,
                                                                    int version) {
    return mutate_([&] {
        GraphMutationLock candidate_lock(*candidate_);
        const auto key = prepared_custom_key(type_id, version);
        const auto found = candidate_->custom_node_types_.find(key);
        if (found == candidate_->custom_node_types_.end())
            return false;
        const bool in_use = std::any_of(
            candidate_->nodes_.begin(), candidate_->nodes_.end(), [&](const GraphNode& node) {
                return node.type == NodeType::Custom && node.custom_type_id == type_id &&
                       node.custom_type_version == version;
            });
        if (in_use)
            return false;
        candidate_->custom_node_types_.erase(found);
        candidate_->sample_kernel_types_.erase(key);
        candidate_->custom_node_diagnostics_.erase(key);
        ++candidate_->custom_registry_generation_;
        candidate_->invalidate_live_locked_();
        return true;
    });
}

std::size_t SignalGraph::PreparedTopologyEdit::prune_unused_custom_node_types() {
    if (mutation_failed_ || committed_ || prepare_attempted_) {
        mutation_failed_ = true;
        return 0;
    }
    GraphMutationLock candidate_lock(*candidate_);
    std::unordered_set<std::string> used;
    used.reserve(candidate_->nodes_.size());
    for (const auto& n : candidate_->nodes_) {
        if (n.type == NodeType::Custom) {
            used.insert(prepared_custom_key(n.custom_type_id, n.custom_type_version));
        }
    }
    std::size_t removed = 0;
    for (auto it = candidate_->custom_node_types_.begin();
         it != candidate_->custom_node_types_.end();) {
        if (used.count(it->first) == 0) {
            candidate_->sample_kernel_types_.erase(it->first);
            candidate_->custom_node_diagnostics_.erase(it->first);
            it = candidate_->custom_node_types_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed != 0) {
        ++candidate_->custom_registry_generation_;
        candidate_->invalidate_live_locked_();
    }
    return removed;
}

std::size_t SignalGraph::PreparedTopologyEdit::custom_node_type_count() const {
    if (!candidate_)
        return 0;
    GraphMutationLock candidate_lock(*candidate_);
    return candidate_->custom_node_types_.size();
}

bool register_builtin_sample_region_types(SignalGraph::PreparedTopologyEdit& edit) {
    if (edit.mutation_failed_ || edit.committed_ || edit.prepare_attempted_) {
        edit.mutation_failed_ = true;
        return false;
    }
    if (!register_builtin_sample_region_types(*edit.candidate_)) {
        edit.mutation_failed_ = true;
        return false;
    }
    return true;
}

NodeId SignalGraph::PreparedTopologyEdit::add_input_node(int channels, const std::string& name) {
    return add_node_([&] { return candidate_->add_input_node(channels, name); });
}

NodeId SignalGraph::PreparedTopologyEdit::add_output_node(int channels, const std::string& name) {
    return add_node_([&] { return candidate_->add_output_node(channels, name); });
}

NodeId SignalGraph::PreparedTopologyEdit::add_gain_node(const std::string& name) {
    return add_node_([&] { return candidate_->add_gain_node(name); });
}

NodeId SignalGraph::PreparedTopologyEdit::add_midi_input_node(const std::string& name) {
    return add_node_([&] { return candidate_->add_midi_input_node(name); });
}

NodeId SignalGraph::PreparedTopologyEdit::add_midi_output_node(const std::string& name) {
    return add_node_([&] { return candidate_->add_midi_output_node(name); });
}

NodeId SignalGraph::PreparedTopologyEdit::add_owned_builtin_plugin_node(
    std::unique_ptr<PluginSlot> slot, int num_inputs, int num_outputs, const std::string& name) {
    if (!slot || slot->info().format != PluginFormat::BuiltIn) {
        mutation_failed_ = true;
        return 0;
    }
    return add_node_([&] {
        return candidate_->add_plugin_node(
            std::make_unique<PreparedOwnedBuiltInSlot>(std::move(slot)), num_inputs, num_outputs,
            name);
    });
}

NodeId SignalGraph::PreparedTopologyEdit::add_processor_node(
    std::shared_ptr<format::ProcessorNodeInstance> processor, const std::string& name) {
    GraphMutationLock owner_lock(*owner_);
    const auto aliases_baseline = [&](const auto& baseline) {
        return std::any_of(baseline.begin(), baseline.end(), [&](const auto& entry) {
            return entry.second && entry.second->instance == processor;
        });
    };
    // A removed baseline ID is still live in the published generation. Reject
    // before minting a second lifetime token, even if the caller removes it again.
    if (processor && (aliases_baseline(owner_->processor_nodes_) ||
                      (base_live_ && aliases_baseline(base_live_->processors)))) {
        mutation_failed_ = true;
        return 0;
    }
    return add_node_([&] { return candidate_->add_processor_node(std::move(processor), name); });
}

NodeId
SignalGraph::PreparedTopologyEdit::add_processor_node(std::unique_ptr<format::Processor> processor,
                                                      const std::string& name) {
    return add_processor_node(format::ProcessorNodeInstance::create(std::move(processor)), name);
}

NodeId SignalGraph::PreparedTopologyEdit::add_custom_node(std::string_view type_id,
                                                          const std::string& name) {
    return add_node_([&] { return candidate_->add_custom_node(type_id, name); });
}

NodeId SignalGraph::PreparedTopologyEdit::add_custom_node(std::string_view type_id, int version,
                                                          const std::string& name) {
    return add_node_([&] { return candidate_->add_custom_node(type_id, version, name); });
}

NodeId SignalGraph::PreparedTopologyEdit::add_unresolved_custom_node(std::string_view type_id,
                                                                     int version, int num_inputs,
                                                                     int num_outputs,
                                                                     const std::string& name) {
    return add_node_([&] {
        return candidate_->add_unresolved_custom_node(type_id, version, num_inputs, num_outputs,
                                                      name);
    });
}

bool SignalGraph::PreparedTopologyEdit::remove_node(NodeId id) {
    return mutate_([&] { return candidate_->remove_node(id); });
}

bool SignalGraph::PreparedTopologyEdit::connect(NodeId source, PortIndex source_port, NodeId dest,
                                                PortIndex dest_port) {
    return mutate_([&] { return candidate_->connect(source, source_port, dest, dest_port); });
}

bool SignalGraph::PreparedTopologyEdit::connect_automation(NodeId source, PortIndex source_port,
                                                           NodeId dest, uint32_t dest_param_id,
                                                           float range_lo, float range_hi,
                                                           float smoothing_ms, AutomationMix mix) {
    return mutate_([&] {
        return candidate_->connect_automation(source, source_port, dest, dest_param_id, range_lo,
                                              range_hi, smoothing_ms, mix);
    });
}

bool SignalGraph::PreparedTopologyEdit::connect_audio_rate_modulation(
    NodeId source, PortIndex source_port, NodeId dest, uint32_t dest_param_id, float range_lo,
    float range_hi, float smoothing_ms, AutomationMix mix) {
    return mutate_([&] {
        return candidate_->connect_audio_rate_modulation(source, source_port, dest, dest_param_id,
                                                         range_lo, range_hi, smoothing_ms, mix);
    });
}

bool SignalGraph::PreparedTopologyEdit::connect_control_modulation(
    const PortHandle& source, NodeId dest, uint32_t dest_param_id, float range_lo, float range_hi,
    float smoothing_ms, AutomationMix mix) {
    return mutate_([&] {
        return candidate_->connect_control_modulation(source, dest, dest_param_id, range_lo,
                                                      range_hi, smoothing_ms, mix);
    });
}

bool SignalGraph::PreparedTopologyEdit::set_control_node_state(
    NodeId id, const std::vector<std::uint8_t>& bytes) {
    if (baseline_node_ids_.contains(id))
        return false;
    const auto* node = candidate_->node(id);
    if (node == nullptr || node->type != NodeType::Custom || node->custom_instance ||
        candidate_->custom_node_type(node->custom_type_id, node->custom_type_version) == nullptr ||
        !validate_control_node_state(node->custom_type_id, bytes))
        return false;
    return mutate_([&] { return candidate_->set_custom_node_state(id, bytes); });
}

bool SignalGraph::PreparedTopologyEdit::connect_feedback(NodeId source, PortIndex source_port,
                                                         NodeId dest, PortIndex dest_port) {
    return mutate_(
        [&] { return candidate_->connect_feedback(source, source_port, dest, dest_port); });
}

bool SignalGraph::PreparedTopologyEdit::connect_midi(NodeId source, NodeId dest) {
    return mutate_([&] { return candidate_->connect_midi(source, dest); });
}

bool SignalGraph::PreparedTopologyEdit::disconnect(NodeId source, PortIndex source_port,
                                                   NodeId dest, PortIndex dest_port) {
    return mutate_([&] { return candidate_->disconnect(source, source_port, dest, dest_port); });
}

bool SignalGraph::PreparedTopologyEdit::disconnect_modulation(NodeId source, PortIndex source_port,
                                                              NodeId dest, uint32_t dest_param_id,
                                                              bool audio_rate) {
    return mutate_([&] {
        return candidate_->disconnect_modulation(source, source_port, dest, dest_param_id,
                                                 audio_rate);
    });
}

bool SignalGraph::PreparedTopologyEdit::set_node_gain(NodeId id, float linear_gain) {
    return mutate_([&] { return candidate_->set_node_gain(id, linear_gain); });
}

SampleRegionResult SignalGraph::PreparedTopologyEdit::reject_sample_region_(
    SampleRegionId id, SampleRegionRefusalReason reason, std::string message, NodeId node) {
    mutation_failed_ = true;
    SampleRegionResult result;
    result.region_id = id;
    result.reason = reason;
    result.offending_node = node;
    result.message = std::move(message);
    return result;
}

SampleRegionResult
SignalGraph::PreparedTopologyEdit::replace_sample_region_(SampleRegionDefinition definition,
                                                          bool declaration) {
    if (mutation_failed_ || committed_ || prepare_attempted_)
        return reject_sample_region_(definition.region_id, SampleRegionRefusalReason::PrepareFailed,
                                     "the topology edit no longer accepts mutations");
    GraphMutationLock candidate_lock(*candidate_);
    auto& definitions = candidate_->sample_region_definitions_;
    const auto found = std::find_if(definitions.begin(), definitions.end(), [&](const auto& value) {
        return value.region_id == definition.region_id;
    });
    if ((declaration && found != definitions.end()) || (!declaration && found == definitions.end()))
        return reject_sample_region_(definition.region_id, SampleRegionRefusalReason::UnknownRegion,
                                     declaration ? "region ID already exists"
                                                 : "region does not exist");
    const auto proof = candidate_->sample_region_metadata_proof_locked_(definition, false);
    if (!proof.accepted)
        return reject_sample_region_(proof.region_id, proof.reason, proof.message,
                                     proof.offending_node);
    std::sort(definition.members.begin(), definition.members.end(),
              [](const auto& left, const auto& right) { return left.node < right.node; });
    const auto boundary_less = [&](NodeId left, NodeId right) {
        const auto index = [&](NodeId id) {
            const auto member = std::find_if(definition.members.begin(), definition.members.end(),
                                             [&](const auto& value) { return value.node == id; });
            return member->config.boundary_index_or_parameter_id;
        };
        const auto li = index(left);
        const auto ri = index(right);
        return li == ri ? left < right : li < ri;
    };
    std::sort(definition.input_boundaries.begin(), definition.input_boundaries.end(),
              boundary_less);
    std::sort(definition.output_boundaries.begin(), definition.output_boundaries.end(),
              boundary_less);
    std::sort(definition.promoted_parameters.begin(), definition.promoted_parameters.end(),
              [](const auto& left, const auto& right) { return left.param_id < right.param_id; });
    const auto id = definition.region_id;
    if (declaration) {
        definitions.push_back(std::move(definition));
        std::sort(definitions.begin(), definitions.end(), [](const auto& left, const auto& right) {
            return left.region_id < right.region_id;
        });
    } else {
        *found = std::move(definition);
    }
    candidate_->invalidate_live_locked_();
    SampleRegionResult result;
    result.accepted = true;
    result.reason = SampleRegionRefusalReason::None;
    result.region_id = id;
    return result;
}

SampleRegionResult
SignalGraph::PreparedTopologyEdit::declare_sample_region(SampleRegionDefinition definition) {
    return replace_sample_region_(std::move(definition), true);
}

SampleRegionResult SignalGraph::PreparedTopologyEdit::remove_sample_region(SampleRegionId id) {
    if (mutation_failed_ || committed_ || prepare_attempted_)
        return reject_sample_region_(id, SampleRegionRefusalReason::PrepareFailed,
                                     "the topology edit no longer accepts mutations");
    GraphMutationLock candidate_lock(*candidate_);
    auto& definitions = candidate_->sample_region_definitions_;
    const auto found = std::find_if(definitions.begin(), definitions.end(),
                                    [&](const auto& value) { return value.region_id == id; });
    if (found == definitions.end())
        return reject_sample_region_(id, SampleRegionRefusalReason::UnknownRegion,
                                     "region does not exist");
    definitions.erase(found);
    candidate_->invalidate_live_locked_();
    SampleRegionResult result;
    result.accepted = true;
    result.reason = SampleRegionRefusalReason::None;
    result.region_id = id;
    return result;
}

SampleRegionResult
SignalGraph::PreparedTopologyEdit::add_sample_region_member(SampleRegionId id, NodeId member,
                                                            SampleKernelConfig config) {
    auto definition = sample_region(id);
    if (!definition)
        return reject_sample_region_(id, SampleRegionRefusalReason::UnknownRegion,
                                     "region does not exist");
    const auto* current = candidate_->node(member);
    if (current == nullptr)
        return reject_sample_region_(id, SampleRegionRefusalReason::UnknownMember,
                                     "member node does not exist", member);
    definition->members.push_back(
        {member, current->custom_type_id, current->custom_type_version, config});
    if (current->custom_type_id == "pulp.core.sample-region.input")
        definition->input_boundaries.push_back(member);
    if (current->custom_type_id == "pulp.core.sample-region.output")
        definition->output_boundaries.push_back(member);
    return replace_sample_region_(std::move(*definition), false);
}

SampleRegionResult SignalGraph::PreparedTopologyEdit::remove_sample_region_member(SampleRegionId id,
                                                                                  NodeId member) {
    auto definition = sample_region(id);
    if (!definition)
        return reject_sample_region_(id, SampleRegionRefusalReason::UnknownRegion,
                                     "region does not exist");
    const auto found = std::find_if(definition->members.begin(), definition->members.end(),
                                    [&](const auto& value) { return value.node == member; });
    if (found == definition->members.end())
        return reject_sample_region_(id, SampleRegionRefusalReason::UnknownMember,
                                     "node is not a member of this region", member);
    definition->members.erase(found);
    std::erase(definition->input_boundaries, member);
    std::erase(definition->output_boundaries, member);
    return replace_sample_region_(std::move(*definition), false);
}

SampleRegionResult
SignalGraph::PreparedTopologyEdit::set_sample_kernel_config(SampleRegionId id, NodeId member,
                                                            SampleKernelConfig config) {
    auto definition = sample_region(id);
    if (!definition)
        return reject_sample_region_(id, SampleRegionRefusalReason::UnknownRegion,
                                     "region does not exist");
    const auto found = std::find_if(definition->members.begin(), definition->members.end(),
                                    [&](const auto& value) { return value.node == member; });
    if (found == definition->members.end())
        return reject_sample_region_(id, SampleRegionRefusalReason::UnknownMember,
                                     "node is not a member of this region", member);
    found->config = config;
    return replace_sample_region_(std::move(*definition), false);
}

SampleRegionResult SignalGraph::PreparedTopologyEdit::connect_in_sample_region(
    SampleRegionId id, NodeId source, PortIndex source_port, NodeId destination,
    PortIndex destination_port) {
    if (mutation_failed_ || committed_ || prepare_attempted_)
        return reject_sample_region_(id, SampleRegionRefusalReason::PrepareFailed,
                                     "the topology edit no longer accepts mutations");
    GraphMutationLock candidate_lock(*candidate_);
    const auto region = std::find_if(candidate_->sample_region_definitions_.begin(),
                                     candidate_->sample_region_definitions_.end(),
                                     [&](const auto& value) { return value.region_id == id; });
    if (region == candidate_->sample_region_definitions_.end())
        return reject_sample_region_(id, SampleRegionRefusalReason::UnknownRegion,
                                     "region does not exist");
    const auto edge_failure = [&](SampleRegionRefusalReason reason, const char* message) {
        auto result = reject_sample_region_(id, reason, message, destination);
        result.has_offending_connection = true;
        result.offending_connection = {source, source_port, destination, destination_port};
        return result;
    };
    if (candidate_->sample_region_for_node_locked_(source) != id ||
        candidate_->sample_region_for_node_locked_(destination) != id)
        return edge_failure(SampleRegionRefusalReason::InvalidBoundaryCrossing,
                            "region-aware connections require two members of the declared region");
    const auto* src = candidate_->node(source);
    const auto* dst = candidate_->node(destination);
    if (src == nullptr || dst == nullptr ||
        source_port >= static_cast<PortIndex>(src->num_output_ports) ||
        destination_port >= static_cast<PortIndex>(dst->num_input_ports))
        return edge_failure(SampleRegionRefusalReason::InvalidProducerCardinality,
                            "connection ports must exist on both kernel members");
    Connection connection{source, source_port, destination, destination_port};
    if (std::find(candidate_->connections_.begin(), candidate_->connections_.end(), connection) !=
        candidate_->connections_.end())
        return edge_failure(SampleRegionRefusalReason::InvalidProducerCardinality,
                            "connection already exists");
    candidate_->append_connection_locked_(connection);
    candidate_->invalidate_live_locked_();
    SampleRegionResult result;
    result.accepted = true;
    result.reason = SampleRegionRefusalReason::None;
    result.region_id = id;
    return result;
}

SampleRegionProof SignalGraph::PreparedTopologyEdit::prove_sample_region(SampleRegionId id) const {
    return candidate_ ? candidate_->prove_sample_region(id) : SampleRegionProof{};
}

std::optional<SampleRegionDescriptor>
SignalGraph::PreparedTopologyEdit::sample_region(SampleRegionId id) const {
    return candidate_ ? candidate_->sample_region(id) : std::nullopt;
}

std::vector<SampleRegionDescriptor> SignalGraph::PreparedTopologyEdit::sample_regions() const {
    return candidate_ ? candidate_->sample_regions() : std::vector<SampleRegionDescriptor>{};
}

SampleRegionParameterContract
SignalGraph::PreparedTopologyEdit::sample_region_parameter_contract() const {
    return candidate_
               ? SampleRegionParameterContract::from_regions(candidate_->sample_region_definitions_)
               : SampleRegionParameterContract{};
}

SampleRegionResult SignalGraph::PreparedTopologyEdit::bind_sample_region_parameters(
    const SampleRegionParameterBinding& binding) {
    SampleRegionResult result;
    if (mutation_failed_ || committed_ || prepare_attempted_) {
        return reject_sample_region_(0, SampleRegionRefusalReason::PrepareFailed,
                                     "the topology edit no longer accepts parameter binding");
    }
    const auto candidate_contract = sample_region_parameter_contract();
    if (!candidate_contract.matches_promoted(binding.contract())) {
        return reject_sample_region_(0, SampleRegionRefusalReason::ParameterContractMismatch,
                                     "the bound parameter contract does not match the candidate");
    }
    sample_region_parameter_binding_ = &binding;
    candidate_->sample_region_parameter_binding_ = &binding;
    result.accepted = true;
    result.reason = SampleRegionRefusalReason::None;
    return result;
}

std::optional<SignalGraph::PreparedTopologyEdit::Result>
SignalGraph::PreparedTopologyEdit::sample_region_preparation_result_(double sample_rate,
                                                                     int max_block_size) {
    candidate_->assert_graph_mutation_locked_();
    if (candidate_->sample_region_definitions_.empty()) {
        if (candidate_->has_sample_kernel_nodes_locked_())
            return Result::PreflightFailed;
        const auto contract = sample_region_parameter_contract();
        if (sample_region_parameter_binding_ != nullptr &&
            !contract.matches_promoted(sample_region_parameter_binding_->contract()))
            return Result::ParameterContractMismatch;
        return std::nullopt;
    }
    candidate_->sample_region_proof_block_size_ =
        max_block_size > 0 ? static_cast<std::uint32_t>(max_block_size) : 0;
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0 ||
        !candidate_->validate_generated_graph(max_block_size).accepted)
        return Result::PreflightFailed;
    std::vector<SampleRegionCandidate> regions;
    std::vector<PreparedSampleRegionPlan> plans;
    for (const auto& definition : candidate_->sample_region_definitions_) {
        if (!candidate_->sample_region_metadata_proof_locked_(definition).accepted)
            return Result::PreflightFailed;
        regions.push_back(candidate_->sample_region_candidate_locked_(definition));
    }
    if (!pulp::host::prove_sample_regions(regions).accepted)
        return Result::PreflightFailed;
    for (const auto& node : candidate_->nodes_) {
        if (node.type == NodeType::Custom &&
            candidate_->sample_kernel_type(node.custom_type_id, node.custom_type_version) !=
                nullptr &&
            candidate_->sample_region_for_node_locked_(node.id) == 0)
            return Result::PreflightFailed;
    }
    if (!candidate_->sample_region_exterior_proof_locked_().accepted)
        return Result::PreflightFailed;
    plans.reserve(regions.size());
    for (const auto& region : regions) {
        auto planned = build_sample_region_plan(region);
        if (!planned.proof.accepted || !planned.plan)
            return Result::PreflightFailed;
        plans.push_back(std::move(*planned.plan));
    }
    const auto contract = sample_region_parameter_contract();
    if (!contract.valid() || sample_region_parameter_binding_ == nullptr ||
        !contract.matches_promoted(sample_region_parameter_binding_->contract()))
        return Result::ParameterContractMismatch;

    const auto live_bank = base_live_ != nullptr ? base_live_->sample_region_bank : nullptr;
    std::uint64_t binding_generation = 1;
    if (live_bank != nullptr) {
        if (live_bank->binding_generation() == std::numeric_limits<std::uint64_t>::max())
            return Result::RuntimeAdoptionFailed;
        binding_generation = live_bank->binding_generation() + 1;
    } else if (base_authoring_generation_ != std::numeric_limits<std::uint64_t>::max()) {
        binding_generation = std::max<std::uint64_t>(1, base_authoring_generation_ + 1);
    } else {
        return Result::RuntimeAdoptionFailed;
    }

    const bool can_adopt = live_bank != nullptr && base_live_->sample_rate == sample_rate &&
                           base_live_->max_block_size == max_block_size;
    auto bank = can_adopt ? SampleRegionStateBank::adopt(plans, *live_bank, sample_rate,
                                                         static_cast<std::uint32_t>(max_block_size),
                                                         binding_generation)
                          : SampleRegionStateBank::create_fresh(
                                plans, sample_rate, static_cast<std::uint32_t>(max_block_size),
                                binding_generation);
    if (!bank)
        return Result::RuntimeAdoptionFailed;

    std::vector<std::shared_ptr<PreparedSampleRegion>> prepared;
    try {
        prepared.reserve(plans.size());
        for (auto& plan : plans) {
            auto region = PreparedSampleRegion::create(std::move(plan), bank,
                                                       sample_region_parameter_binding_);
            if (!region)
                return Result::RuntimeAdoptionFailed;
            prepared.push_back(std::move(region));
        }
    } catch (...) {
        return Result::RuntimeAdoptionFailed;
    }
    candidate_->prepared_sample_region_bank_ = std::move(bank);
    candidate_->prepared_sample_regions_ = std::move(prepared);
    return std::nullopt;
}

void SignalGraph::PreparedTopologyEdit::set_canonical_executor_routing_enabled(
    bool enabled) noexcept {
    if (mutation_failed_ || committed_ || prepare_attempted_) {
        mutation_failed_ = true;
        return;
    }
    candidate_->set_canonical_executor_routing_enabled(enabled);
    if (prepared_description_)
        authored_binding_stale_ = true;
    prepared_description_.reset();
}

void SignalGraph::PreparedTopologyEdit::set_parallel_routing_enabled(bool enabled) noexcept {
    if (mutation_failed_ || committed_ || prepare_attempted_) {
        mutation_failed_ = true;
        return;
    }
    candidate_->set_parallel_routing_enabled(enabled);
    if (prepared_description_)
        authored_binding_stale_ = true;
    prepared_description_.reset();
}

void SignalGraph::PreparedTopologyEdit::set_anticipation_enabled(bool enabled) noexcept {
    if (mutation_failed_ || committed_ || prepare_attempted_) {
        mutation_failed_ = true;
        return;
    }
    candidate_->set_anticipation_enabled(enabled);
    if (prepared_description_)
        authored_binding_stale_ = true;
    prepared_description_.reset();
}

bool SignalGraph::PreparedTopologyEdit::set_exact_parameter_event_nodes(
    const std::shared_ptr<detail::ExactParameterIngressOwner>& owner,
    std::span<const NodeId> nodes, ExactParameterNodeClaimPasskey) {
    return mutate_([&] {
        if (!owner) return false;
        std::unordered_set<NodeId> claimed;
        claimed.reserve(nodes.size());
        for (const auto id : nodes) {
            const auto* candidate = candidate_->node(id);
            const bool processor_node = candidate != nullptr &&
                                        candidate->type == NodeType::Plugin &&
                                        candidate_->processor_nodes_.contains(id);
            if (candidate == nullptr || candidate->type != NodeType::Plugin ||
                (!candidate->plugin && !processor_node) || !claimed.insert(id).second) {
                return false;
            }
        }
        auto& claims = candidate_->exact_parameter_event_claims_;
        for (auto it = claims.begin(); it != claims.end();) {
            const auto existing = it->second.lock();
            if (!existing || existing == owner) {
                it = claims.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto id : claimed) {
            const auto found = claims.find(id);
            if (found != claims.end() && !found->second.expired()) return false;
        }
        for (const auto id : claimed) claims[id] = owner;
        return true;
    });
}

const GraphNode* SignalGraph::PreparedTopologyEdit::node(NodeId id) const {
    return candidate_ ? candidate_->node(id) : nullptr;
}

const std::vector<GraphNode>& SignalGraph::PreparedTopologyEdit::nodes() const {
    static const std::vector<GraphNode> empty;
    return candidate_ ? candidate_->nodes_ : empty;
}

const std::vector<Connection>& SignalGraph::PreparedTopologyEdit::connections() const {
    static const std::vector<Connection> empty;
    return candidate_ ? candidate_->connections_ : empty;
}

PreparedDescriptionResult SignalGraph::ExecutionSnapshot::export_prepared_description() const {
    if (!owner_ || !snapshot_)
        return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::Unprepared);
    GraphMutationLock owner_lock(*owner_);
    const auto current = owner_->live_slot_.live();
    if (!current || current.get() != snapshot_.get())
        return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::SnapshotChanged);
    if (!snapshot_->prepared_description)
        return PreparedDescriptionResult::refused(
            PreparedDescriptionRefusal::LogicalIdentityMissing);
    if (snapshot_->prepared_description->owner_graph_identity != owner_->diagnostic_graph_identity_)
        return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::SnapshotChanged);
    if (snapshot_->prepared_description->owner_generation != owner_->authoring_generation_)
        return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::SnapshotChanged);
    if (snapshot_->prepared_description->execution_generation != snapshot_->execution_generation)
        return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::SnapshotChanged);
    const auto selection_generation =
        owner_->execution_selection_generation_.load(std::memory_order_relaxed);
    if (selection_generation == std::numeric_limits<std::uint64_t>::max())
        return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::GenerationExhausted);
    if (snapshot_->prepared_description->execution_selection_generation != selection_generation)
        return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::SnapshotChanged);
    const auto& record = *snapshot_->prepared_description;
    const auto& serial = snapshot_->routed.serial;
    if (!serial.valid || !serial.snapshot.valid() || !serial.pool.bound_to(serial.snapshot) ||
        record.snapshot_id != serial.snapshot.id() ||
        record.pool_snapshot_id != serial.pool.bound_snapshot_id() ||
        record.pool_snapshot_id != record.snapshot_id ||
        record.pool_binding_identity != reinterpret_cast<std::uintptr_t>(&serial.pool) ||
        record.buffer_slot_count != serial.snapshot.buffer_slot_count() ||
        record.pool_slot_count != serial.pool.slot_count() ||
        record.pool_max_frames != serial.pool.max_frames() ||
        record.runtime_max_block_frames != serial.pool.max_frames() ||
        record.max_block_frames > serial.pool.max_frames())
        return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::SnapshotChanged);
    if (record.host_input_alias_eligible &&
        (record.host_input_alias_mode != "conditional_direct_host_input" ||
         record.host_input_alias_fallback != "internal_copy" ||
         !record.host_input_alias_requires_full_width ||
         !record.host_input_alias_requires_disjoint ||
         record.host_input_alias_mappings.size() != record.channels.size()))
        return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::SnapshotChanged);
    for (const auto& interface : record.interfaces) {
        if (interface.runtime_bus_index != 0 ||
            interface.runtime_channel_indices.size() != interface.channels.size() ||
            !std::all_of(interface.runtime_channel_indices.begin(),
                         interface.runtime_channel_indices.end(),
                         [ordinal = std::size_t{0}](std::uint32_t channel) mutable {
                             return channel == ordinal++;
                         }))
            return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::SnapshotChanged);
    }
    try {
        auto copy = std::make_shared<PreparedDescriptionData>(*snapshot_->prepared_description);
        if (owner_->execution_selection_generation_.load(std::memory_order_relaxed) !=
            selection_generation)
            return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::SnapshotChanged);
        return {PreparedDescriptionRefusal::None, std::move(copy)};
    } catch (const std::bad_alloc&) {
        return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::AllocationFailed);
    } catch (...) {
        return PreparedDescriptionResult::refused(
            PreparedDescriptionRefusal::LogicalIdentityMissing);
    }
}

bool SignalGraph::PreparedTopologyEdit::routed_execution_ready(int block_size) const noexcept {
    if (!prepared_snapshot_ || block_size <= 0)
        return false;
    const auto frames = static_cast<std::uint32_t>(block_size);
    const auto serial_ready = [&] {
        return prepared_snapshot_->routed.serial.valid &&
               prepared_snapshot_->routed.serial.pool.fits(
                   prepared_snapshot_->routed.serial.snapshot, frames);
    };
    const auto parallel_ready = [&] {
        return prepared_snapshot_->routed.parallel.valid &&
               prepared_snapshot_->routed.parallel.pool.fits(
                   prepared_snapshot_->routed.parallel.snapshot, frames);
    };
    switch (prepared_snapshot_->pdc_execution_domain) {
    case PdcExecutionDomain::Legacy:
        return false;
    case PdcExecutionDomain::RoutedSerial:
        return serial_ready();
    case PdcExecutionDomain::RoutedParallel:
        return parallel_ready();
    case PdcExecutionDomain::Dynamic:
        return (candidate_->parallel_routing_enabled_.load(std::memory_order_relaxed) &&
                parallel_ready()) ||
               (candidate_->canonical_executor_routing_enabled_.load(std::memory_order_relaxed) &&
                serial_ready());
    }
    return false;
}

SignalGraph::PreparedTopologyEdit::Result
SignalGraph::PreparedTopologyEdit::prepare(double sample_rate, int max_block_size) {
    if (committed_)
        return last_result_ = Result::AlreadyCommitted;
    if (mutation_failed_)
        return last_result_ = Result::InvalidMutation;
    if (authored_binding_stale_)
        return last_result_ = Result::PreflightFailed;
    if (prepared_snapshot_)
        return last_result_ = Result::Prepared;
    if (prepare_attempted_)
        return last_result_;
    prepare_attempted_ = true;

    try {
        GraphMutationLock owner_lock(*owner_);
        std::optional<GraphMutationLock> candidate_lock;
        std::shared_ptr<CompiledGraph> old_keepalive;
        bool dimensions_changed = false;
        if (const auto rejection = admission_result_(sample_rate, max_block_size, candidate_lock,
                                                     old_keepalive, dimensions_changed))
            return last_result_ = *rejection;
        const auto* old = old_keepalive.get();

        auto next = candidate_->compile_(sample_rate, max_block_size);
        if (!next) {
            owner_->spectral_error_.store(candidate_->spectral_error(), std::memory_order_relaxed);
            return last_result_ = Result::CompileFailed;
        }

        if (prepared_description_) {
            if (!candidate_->canonical_executor_routing_enabled_.load(std::memory_order_relaxed) ||
                candidate_->parallel_routing_enabled_.load(std::memory_order_relaxed) ||
                candidate_->anticipation_enabled_.load(std::memory_order_relaxed) ||
                next->pdc_execution_domain != PdcExecutionDomain::Dynamic ||
                !next->routed.serial.valid ||
                !next->routed.serial.pool.bound_to(next->routed.serial.snapshot) ||
                max_block_size < 1 || max_block_size > 4096) {
                return last_result_ = Result::PreflightFailed;
            }
            auto capture = detail::capture_prepared_description(
                *candidate_, *prepared_description_, next->routed.serial.snapshot,
                next->routed.serial.pool, sample_rate, max_block_size,
                candidate_->execution_selection_generation_.load(std::memory_order_relaxed),
                next->execution_generation, base_authoring_generation_);
            if (!capture)
                return last_result_ = Result::PreflightFailed;
            next->prepared_description = std::move(capture);
        }

        if (old != nullptr) {
            if (const auto rejection =
                    adopt_live_runtime_state_(old, next.get(), dimensions_changed))
                return last_result_ = *rejection;
        }

        prepared_snapshot_ = std::move(next);
        return last_result_ = Result::Prepared;
    } catch (const std::bad_alloc&) {
        // Preparation is an isolated transaction.  Allocation failure must not
        // publish a partial snapshot or escape as an exception to the owner.
        return last_result_ = Result::PreflightFailed;
    } catch (...) {
        return last_result_ = Result::PreflightFailed;
    }
}

SignalGraph::PreparedTopologyEdit::Result
SignalGraph::PreparedTopologyEdit::prepare_quiesced(double sample_rate,
                                                     int max_block_size) {
    if (committed_)
        return last_result_ = Result::AlreadyCommitted;
    if (mutation_failed_)
        return last_result_ = Result::InvalidMutation;
    if (prepared_snapshot_)
        return last_result_ = Result::Prepared;
    if (prepare_attempted_)
        return last_result_;
    prepare_attempted_ = true;

    // This path is deliberately separate from prepare(): the caller guarantees
    // that audio processing and anticipation are quiesced, so shared external
    // plugin/custom instances may be re-prepared for new dimensions on the
    // private candidate. The owner's authoring graph and live publication remain
    // unchanged unless the candidate succeeds and commit() is called.
    GraphMutationLock owner_lock(*owner_);
    try {
        if (owner_->refuse_spectral_mutation_())
            return last_result_ = Result::PreflightFailed;
        if (!base_is_current_locked_() || owner_->in_swap_edit_)
            return last_result_ = Result::StaleBase;
        if (const auto rejection = baseline_removal_rejection_locked_())
            return last_result_ = *rejection;

        {
            GraphMutationLock candidate_lock(*candidate_);
            if (const auto rejection =
                    sample_region_preparation_result_(sample_rate, max_block_size))
                return last_result_ = *rejection;
        }

        // Capture the exact retained objects and the owner's original custom
        // lifecycle callbacks before candidate preparation mutates shared state.
        quiesced_plugins_.clear();
        quiesced_customs_.clear();
        quiesced_processors_.clear();
        for (const auto& node : candidate_->nodes_) {
            if (is_new_node_(node.id))
                continue;
            if (const auto processor = candidate_->processor_nodes_.find(node.id);
                processor != candidate_->processor_nodes_.end() && processor->second) {
                quiesced_processors_.push_back(
                    {processor->second->instance, node.num_input_ports, node.num_output_ports});
                continue;
            }
            if (node.plugin) {
                quiesced_plugins_.push_back({node.plugin, false});
                continue;
            }
            if (node.type != NodeType::Custom || !node.custom_instance)
                continue;
            const auto owner_node = std::find_if(
                owner_->nodes_.begin(), owner_->nodes_.end(), [&](const GraphNode& current) {
                    return current.id == node.id && current.custom_instance == node.custom_instance;
                });
            if (owner_node == owner_->nodes_.end())
                continue;
            if (replaced_registry_keys_.count(
                    prepared_custom_key(node.custom_type_id, node.custom_type_version)) != 0) {
                return last_result_ = Result::ExistingCustomReprepareRequired;
            }
            const auto* candidate_type =
                candidate_->custom_node_type(node.custom_type_id, node.custom_type_version);
            const auto* owner_type = owner_->custom_node_type(owner_node->custom_type_id,
                                                              owner_node->custom_type_version);
            if (candidate_type != nullptr && candidate_type->prepare && owner_type != nullptr) {
                quiesced_customs_.push_back(
                    {node.custom_instance, owner_type->prepare, owner_type->release, false});
            }
        }
        quiesced_lifecycles_dirty_ = !quiesced_processors_.empty() || !quiesced_plugins_.empty() ||
                                     !quiesced_customs_.empty();

        bool candidate_prepared = false;
        const SignalGraph::PrepareLifecycleObserver lifecycle_observer{
            this,
            [](void* context, PluginSlot* plugin) noexcept {
                auto& edit = *static_cast<PreparedTopologyEdit*>(context);
                for (auto& retained : edit.quiesced_plugins_) {
                    if (retained.plugin.get() == plugin && !retained.touched) {
                        retained.touched = true;
                        return;
                    }
                }
            },
            [](void* context, void* instance) noexcept {
                auto& edit = *static_cast<PreparedTopologyEdit*>(context);
                for (auto& retained : edit.quiesced_customs_) {
                    if (retained.instance.get() == instance && !retained.touched) {
                        retained.touched = true;
                        return;
                    }
                }
            },
        };
        try {
            candidate_prepared =
                candidate_->prepare_impl_(sample_rate, max_block_size, &lifecycle_observer);
        } catch (...) {
            candidate_prepared = false;
        }

        // candidate_->prepare() may create some new custom instances before a
        // later plugin/custom prepare fails. Remember them even on failure so an
        // abandoned edit still balances every successful create/prepare with the
        // control-thread release callback.
        prepared_new_custom_ids_.clear();
        for (const auto& node : candidate_->nodes_) {
            if (node.type != NodeType::Custom || !node.custom_instance)
                continue;
            const auto owner_node =
                std::find_if(owner_->nodes_.begin(), owner_->nodes_.end(),
                             [&](const GraphNode& current) { return current.id == node.id; });
            if (owner_node == owner_->nodes_.end() ||
                owner_node->custom_instance != node.custom_instance) {
                prepared_new_custom_ids_.push_back(node.id);
            }
        }
        if (!candidate_prepared) {
            if (!rollback_quiesced_lifecycles_locked_())
                return last_result_ = Result::QuiescedRollbackFailed;
            return last_result_ = Result::ExternalPluginReprepareRequired;
        }

        prepared_snapshot_ = candidate_->live_slot_.live();
        if (!prepared_snapshot_) {
            if (!rollback_quiesced_lifecycles_locked_())
                return last_result_ = Result::QuiescedRollbackFailed;
            return last_result_ = Result::CompileFailed;
        }

        return last_result_ = Result::Prepared;
    } catch (const std::bad_alloc&) {
        if (!rollback_quiesced_lifecycles_locked_())
            return last_result_ = Result::QuiescedRollbackFailed;
        return last_result_ = Result::PreflightFailed;
    } catch (...) {
        if (!rollback_quiesced_lifecycles_locked_())
            return last_result_ = Result::QuiescedRollbackFailed;
        return last_result_ = Result::PreflightFailed;
    }
}

SignalGraph::PreparedTopologyEdit::Result SignalGraph::PreparedTopologyEdit::commit() {
    if (committed_)
        return last_result_ = Result::AlreadyCommitted;
    if (!prepared_snapshot_)
        return last_result_ = Result::NotPrepared;

    GraphMutationLock owner_lock(*owner_);
    if (prepared_snapshot_->spectral && !owner_->spectral_mode_.load(std::memory_order_acquire)) {
        owner_->spectral_error_.store(SpectralError::RequiresStoppedController,
                                      std::memory_order_relaxed);
        return last_result_ = Result::PreflightFailed;
    }
    const auto fail_before_commit = [&](Result result) {
        if (!rollback_quiesced_lifecycles_locked_())
            return last_result_ = Result::QuiescedRollbackFailed;
        return last_result_ = result;
    };
    if (mutation_failed_)
        return fail_before_commit(Result::InvalidMutation);
    if (!base_is_current_locked_() || owner_->in_swap_edit_) {
        return fail_before_commit(Result::StaleBase);
    }
    if (owner_->authoring_generation_ == std::numeric_limits<std::uint64_t>::max())
        return fail_before_commit(Result::RuntimeAdoptionFailed);
    const auto committed_generation = owner_->authoring_generation_ + 1;

    std::shared_ptr<const PreparedDescriptionData> reminted_description;
    if (prepared_snapshot_->prepared_description) {
        try {
            auto value = std::make_shared<PreparedDescriptionData>(
                *prepared_snapshot_->prepared_description);
            value->owner_graph_identity = owner_->diagnostic_graph_identity_;
            value->owner_generation = committed_generation;
            value->execution_generation = committed_generation;
            value->execution_selection_generation =
                owner_->execution_selection_generation_.load(std::memory_order_relaxed);
            reminted_description = std::move(value);
        } catch (...) {
            return fail_before_commit(Result::RuntimeAdoptionFailed);
        }
    }

    // Everything above the irreversible authoring move is an exception
    // boundary. Reserve both generic publication retirement and destination
    // load-map buckets now. After this point publication is noexcept and load
    // measurers move as allocation-free C++17 node handles.
    try {
        {
            std::scoped_lock load_locks(owner_->node_load_mu_, candidate_->node_load_mu_);
            std::size_t incoming = 0;
            for (const auto& [id, measurer] : candidate_->node_load_) {
                if (measurer && owner_->node_load_.find(id) == owner_->node_load_.end())
                    ++incoming;
            }
            owner_->node_load_.reserve(owner_->node_load_.size() + incoming);
        }
        if (!owner_->spectral_mode_.load(std::memory_order_acquire))
            owner_->live_slot_.prepare_publish();

        if (candidate_->parallel_routing_enabled_.load(std::memory_order_relaxed) &&
            prepared_snapshot_->routed.parallel.valid &&
            owner_->worker_pool_.worker_count() == 0) {
            const unsigned hw = std::thread::hardware_concurrency();
            const std::uint32_t workers =
                std::clamp<std::uint32_t>(hw == 0 ? 2 : hw, 2, 16);
            if (!owner_->worker_pool_.start(workers))
                return fail_before_commit(Result::ParallelWorkerStartFailed);
        }
        if (candidate_->parallel_routing_enabled_.load(std::memory_order_relaxed) &&
            prepared_snapshot_->routed.parallel.valid && !owner_->worker_pool_.running()) {
            return fail_before_commit(Result::ParallelWorkerStartFailed);
        }
    } catch (...) {
        if (!rollback_quiesced_lifecycles_locked_())
            return last_result_ = Result::QuiescedRollbackFailed;
        // Commit is a transactional control-thread API.  Once preflight has
        // crossed into publication preparation, an allocation or worker-pool
        // failure must be reported as a typed refusal after rollback rather
        // than escaping and leaving callers without a stable failure result.
        return last_result_ = Result::RuntimeAdoptionFailed;
    }

    if (owner_->spectral_mode_.load(std::memory_order_acquire)) {
        const auto result = owner_->offer_spectral_candidate_locked_(candidate_, prepared_snapshot_,
                                                                     base_authoring_generation_);
        owner_->spectral_error_.store(result, std::memory_order_relaxed);
        if (result != SpectralError::None)
            return last_result_ = Result::PreflightFailed;
        prepared_snapshot_.reset();
        committed_ = true;
        return last_result_ = Result::Staged;
    }
    owner_->adopt_candidate_authoring_locked_(*candidate_);
    // The candidate was compiled under its private authoring generation.  The
    // committed snapshot receives the owner's reminted generation at the same
    // publication boundary, so detached export cannot pair a candidate token
    // with the adopted owner state.
    prepared_snapshot_->execution_generation = committed_generation;
    prepared_snapshot_->legacy_processing_lifetime = owner_->processing_handle_lifetime_;

    // The telemetry toggle is an independent control setting, so it does not
    // stale a topology edit. Re-seed from the owner's authoritative
    // desired value at the publication boundary instead of restoring the value
    // captured when the candidate was created.
    prepared_snapshot_->live_dsp_telemetry.set_enabled(
        owner_->desired_live_dsp_telemetry_enabled_.load(std::memory_order_relaxed));
    if (reminted_description)
        prepared_snapshot_->prepared_description = std::move(reminted_description);
    owner_->live_slot_.publish_prepared(prepared_snapshot_);
    owner_->total_latency_samples_.store(prepared_snapshot_->total_latency_samples,
                                         std::memory_order_relaxed);
    owner_->publish_prepared_stats_locked_(*prepared_snapshot_);
    ++owner_->authoring_generation_;

    committed_snapshot_ = prepared_snapshot_;
    prepared_snapshot_.reset();
    prepared_new_custom_ids_.clear();
    quiesced_lifecycles_dirty_ = false;
    quiesced_plugins_.clear();
    quiesced_customs_.clear();
    quiesced_processors_.clear();
    committed_ = true;
    return last_result_ = Result::Committed;
}

void SignalGraph::adopt_candidate_authoring_locked_(SignalGraph& candidate) {
    // Transfer only load measurers allocated for genuinely new/reused IDs.
    // std::unordered_map<NodeId, unique_ptr<...>> uses the same default
    // allocator, integral hash, and equality on both sides; destination buckets
    // were reserved above. Extract/insert therefore transfers ownership and
    // preserves the addresses captured by the compiled snapshot without an
    // allocation after the exception boundary.
    {
        std::scoped_lock load_locks(this->node_load_mu_, candidate.node_load_mu_);
        for (auto it = candidate.node_load_.begin(); it != candidate.node_load_.end();) {
            if (!it->second || this->node_load_.find(it->first) != this->node_load_.end()) {
                ++it;
                continue;
            }
            auto current = it++;
            auto node = candidate.node_load_.extract(current);
            [[maybe_unused]] const auto inserted = this->node_load_.insert(std::move(node));
            assert(inserted.inserted);
        }
    }

    this->cancel_swap_edit_locked_();
    if (candidate.graph_event_edge_ &&
        !candidate.has_path_locked_(candidate.graph_event_edge_->producer,
                                    candidate.graph_event_edge_->consumer))
        candidate.graph_event_edge_.reset();
    this->nodes_ = std::move(candidate.nodes_);
    this->processor_nodes_ = std::move(candidate.processor_nodes_);
    this->connections_ = std::move(candidate.connections_);
    this->graph_event_edge_ = candidate.graph_event_edge_;
    this->connection_identities_ = std::move(candidate.connection_identities_);
    this->next_connection_identity_ = candidate.next_connection_identity_;
    this->custom_node_types_ = std::move(candidate.custom_node_types_);
    this->sample_kernel_types_ = std::move(candidate.sample_kernel_types_);
    this->custom_node_diagnostics_ = std::move(candidate.custom_node_diagnostics_);
    this->sample_region_definitions_ = std::move(candidate.sample_region_definitions_);
    this->sample_region_parameter_binding_ = candidate.sample_region_parameter_binding_;
    this->sample_region_proof_block_size_ = candidate.sample_region_proof_block_size_;
    this->prepared_sample_region_bank_ = candidate.prepared_sample_region_bank_;
    this->prepared_sample_regions_ = candidate.prepared_sample_regions_;
    this->custom_registry_generation_ = candidate.custom_registry_generation_;
    this->next_id_ = candidate.next_id_;
    this->limits_ = candidate.limits_;
    this->prepared_plugin_meta_ = std::move(candidate.prepared_plugin_meta_);
    this->exact_parameter_event_claims_ = std::move(candidate.exact_parameter_event_claims_);
    this->canonical_executor_routing_enabled_.store(
        candidate.canonical_executor_routing_enabled_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    this->parallel_routing_enabled_.store(
        candidate.parallel_routing_enabled_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    this->anticipation_enabled_.store(
        candidate.anticipation_enabled_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    this->execution_selection_generation_.store(
        candidate.execution_selection_generation_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    this->transport_suppressed_for_anticipation_.store(
        candidate.transport_suppressed_for_anticipation_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
}

} // namespace pulp::host
