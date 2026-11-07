// SignalGraph prepared-topology transactions.
//
// Unlike begin_swap_edit(), this path never mutates the owner before success.
// A private SignalGraph carries the complete candidate authoring state and is
// compiled off-side. Commit moves that state and publishes its snapshot while
// holding the owner's control lock, so audio observes exactly the old or new
// compiled graph and control readers cannot observe a mixed authoring graph.

#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

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
    void skip_space() {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\n' || text_[position_] == '\r' ||
                text_[position_] == '\t'))
            ++position_;
    }

    bool string_token() {
        if (position_ >= text_.size() || text_[position_++] != '"')
            return false;
        while (position_ < text_.size()) {
            const auto c = static_cast<unsigned char>(text_[position_++]);
            if (c == '"')
                return true;
            if (c < 0x20)
                return false;
            if (c == '\\') {
                if (position_ >= text_.size())
                    return false;
                const auto escaped = text_[position_++];
                if (escaped == 'u') {
                    if (position_ + 4 > text_.size())
                        return false;
                    for (int i = 0; i < 4; ++i) {
                        const auto h = static_cast<unsigned char>(text_[position_++]);
                        if (!std::isxdigit(h))
                            return false;
                    }
                } else if (escaped != '"' && escaped != '\\' && escaped != '/' && escaped != 'b' &&
                           escaped != 'f' && escaped != 'n' && escaped != 'r' && escaped != 't') {
                    return false;
                }
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
            const auto start = position_;
            if (!string_token())
                return false;
            if (!keys.emplace(std::string(text_.substr(start, position_ - start))).second)
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
            while (position_ < text_.size() && text_[position_] != ',' && text_[position_] != ']' &&
                   text_[position_] != '}' && !std::isspace(static_cast<unsigned char>(text_[position_])))
                ++position_;
            return true;
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
    const auto bits = static_cast<std::uint32_t>(std::stoul(std::string(value.substr(4)), nullptr, 16));
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
        !canonical_key(value["node"]) || !canonical_key(value["port"])
        || !value["direction"].isString() || !value["kind"].isString() ||
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
        !canonical_member_shape(root, {"assets", "edges", "interfaces", "nodes", "policies", "profile"},
                                {"assets", "edges", "interfaces", "nodes", "policies", "profile"}) ||
        !root["profile"].isString() || root["profile"].getString() != "pulp.graph.identity.v1" ||
        !root["assets"].isObject() || root["assets"].size() != 0) {
        return false;
    }
    const auto policies = root["policies"];
    if (!canonical_member_shape(policies,
                                {"event_order", "fault", "feedback", "rate_conversion", "seed",
                                 "state_retention"},
                                {"event_order", "fault", "feedback", "rate_conversion", "seed",
                                 "state_retention"}) ||
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
                                    {"kind", "kind_contract", "implementation", "config", "initial_state",
                                     "ports", "parameter_bindings", "policies"},
                                    {"kind", "kind_contract", "implementation", "config", "initial_state",
                                     "ports", "parameter_bindings", "policies"}))
        {
            return false;
        }
        const auto node = member.value;
        if (!node["kind"].isString())
        {
            return false;
        }
        const std::string kind(node["kind"].getString());
        if (kind != "audio_input" && kind != "audio_output" && kind != "gain")
        {
            return false;
        }
        const std::string revision = kind == "gain" ? "gain.v1" : "io.v1";
        const std::string implementation =
            kind == "audio_input" ? "pulp.builtin.audio-input.v1"
                                   : (kind == "audio_output" ? "pulp.builtin.audio-output.v1"
                                                               : "pulp.builtin.gain.v1");
        if (!node["kind_contract"].isString() || node["kind_contract"].getString() != revision ||
            !node["implementation"].isString() || node["implementation"].getString() != implementation ||
            !node["parameter_bindings"].isObject() || node["parameter_bindings"].size() != 0 ||
            !node["policies"].isObject() || node["policies"].size() != 0 ||
            !canonical_member_shape(node["initial_state"], {"mode", "revision"}, {"mode", "revision"}) ||
            node["initial_state"]["mode"].getString() != "default" ||
            node["initial_state"]["revision"].getString() != revision)
            return false;
        if (kind == "gain") {
            if (!canonical_member_shape(node["config"], {"gain"}, {"gain"}) ||
                !node["config"]["gain"].isString() || !canonical_gain(node["config"]["gain"].getString()))
                {
                return false;
                }
        } else if (!node["config"].isObject() || node["config"].size() != 0) {
            return false;
        }
        if (!canonical_member_shape(node["ports"], {"in", "out"}, {"in", "out"}))
        {
            return false;
        }
        std::vector<std::string> active_channels;
        for (const auto direction : {std::string_view{"in"}, std::string_view{"out"}}) {
            const bool active = kind == "gain" || (kind == "audio_input" && direction == "out") ||
                                (kind == "audio_output" && direction == "in");
            const auto lookup = direction == "in" ? node["ports"]["in"] : node["ports"]["out"];
            const auto ports = lookup;
            const bool valid_ports = ports.isObject() &&
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
                    port["kind"].getString() != "audio" || port["contract"].getString() != "audio-port.v1" ||
                    !canonical_channels(port["channels"], active_channels))
                {
                    return false;
                }
                if (port_channels.contains(member.name) && port_channels[member.name] != active_channels)
                {
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
        if (kind == "gain" && node["ports"]["in"]["audio"]["channels"] !=
                                   node["ports"]["out"]["audio"]["channels"]) {
            return false;
        }
        kinds.emplace(std::string(member.name), kind);
    }
    channel_names = std::move(graph_channels);
    if (channel_names.size() != 2)
        return false;
    std::unordered_set<std::string> interface_keys;
    std::size_t input_interfaces = 0, output_interfaces = 0;
    for (std::uint32_t i = 0; i < interfaces.size(); ++i) {
        const auto item = interfaces[i];
        if (!canonical_member_shape(item, {"key", "role", "kind", "endpoint", "channels"},
                                    {"key", "role", "kind", "endpoint", "channels"}) ||
            !canonical_key(item["key"]) ||
            !interface_keys.insert(std::string(item["key"].getString())).second ||
            !item["role"].isString() || (item["role"].getString() != "input" && item["role"].getString() != "output") ||
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
        if (!canonical_member_shape(edge, {"lane", "source", "destination", "semantic_order", "feedback", "channel_map"},
                                    {"lane", "source", "destination", "semantic_order", "feedback", "channel_map"}) ||
            !edge["lane"].isString() || edge["lane"].getString() != "audio" || !edge["feedback"].isBool() ||
            edge["feedback"].getBool() || !edge["semantic_order"].isVoid())
            return false;
        std::string source_node, source_direction, source_port, destination_node, destination_direction,
            destination_port;
        if (!canonical_endpoint(edge["source"], source_node, source_direction, source_port) ||
            !canonical_endpoint(edge["destination"], destination_node, destination_direction, destination_port) ||
            source_direction != "out" || destination_direction != "in" || !kinds.contains(source_node) ||
            !kinds.contains(destination_node) || source_port != "audio" || destination_port != "audio" ||
            member.name != canonical_edge_key(source_node, source_direction, source_port, destination_node,
                                              destination_direction, destination_port))
            return false;
        const auto map = edge["channel_map"];
        if (!map.isArray() || map.size() != channel_names.size())
            return false;
        for (std::uint32_t c = 0; c < map.size(); ++c) {
            if (!map[c].isArray() || map[c].size() != 2 || !map[c][0].isString() || !map[c][1].isString() ||
                map[c][0].getString() != channel_names[c] || map[c][1].getString() != channel_names[c])
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

const char* prepared_kind(NodeType type) noexcept {
    switch (type) {
    case NodeType::AudioInput:
        return "AudioInput";
    case NodeType::AudioOutput:
        return "AudioOutput";
    case NodeType::Gain:
        return "Gain";
    default:
        return nullptr;
    }
}

std::string prepared_custom_key(std::string_view type_id, int version) {
    std::string key(type_id);
    key.push_back('\x1f');
    key += std::to_string(version);
    return key;
}

bool has_feedback(const std::vector<Connection>& connections) {
    return std::any_of(connections.begin(), connections.end(),
                       [](const Connection& c) { return c.feedback; });
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

  private:
    std::unique_ptr<PluginSlot> inner_;
    bool prepare_entered_ = false;
    bool released_ = false;
};

} // namespace

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

    candidate_->nodes_ = owner.nodes_;
    candidate_->processor_nodes_ = owner.processor_nodes_;
    candidate_->connections_ = owner.connections_;
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

PreparedDescriptionResult SignalGraph::PreparedTopologyEdit::bind_authored_identity(
    std::string_view canonical_semantic_payload, const AuthoredRuntimeBinding& bindings) {
    if (prepare_attempted_ || committed_ || mutation_failed_)
        return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::AuthoredBindingStale);
    if (canonical_semantic_payload.empty())
        return PreparedDescriptionResult::refused(
            PreparedDescriptionRefusal::LogicalIdentityMissing);

    const auto result = std::make_shared<PreparedDescriptionData>();
    result->semantic_profile = "pulp.graph.authored-io-gain.v1";
    result->semantic_payload.assign(canonical_semantic_payload);

    choc::value::Value authored_root;
    std::vector<std::string> channel_names;
    try {
        if (!strict_semantic_payload(canonical_semantic_payload, authored_root, channel_names)) {
            return PreparedDescriptionResult::refused(
                PreparedDescriptionRefusal::AuthoredBindingMismatch);
        }
    } catch (const std::exception& e) {
        return PreparedDescriptionResult::refused(
            PreparedDescriptionRefusal::AuthoredBindingMismatch);
    }
    const auto& nodes = candidate_->nodes_;
    const auto authored_interfaces = authored_root["interfaces"];
    if (!nodes.empty() && channel_names.size() != 2)
        return PreparedDescriptionResult::refused(
            PreparedDescriptionRefusal::UnsupportedPreparedProfile);
    std::string digest_preimage{"pulp.graph.semantic.v1", 22};
    digest_preimage.push_back('\0');
    digest_preimage.append(canonical_semantic_payload.data(), canonical_semantic_payload.size());
    result->semantic_digest = "sha256:" + runtime::sha256_hex(digest_preimage);
    if (nodes.empty()) {
        if (!bindings.nodes.empty() || !bindings.channels.empty() || !bindings.interfaces.empty())
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
            connection.source_port > 1 || connection.dest_port > 1)
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
            const auto expected_output = port < static_cast<std::uint32_t>(node.num_output_ports);
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
            normalized_binding_key != binding.authored_key || !keys.emplace(binding.node_id, binding.authored_key).second ||
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
        const auto destination = std::find_if(nodes.begin(), nodes.end(), [&](const GraphNode& n) {
            return n.id == connection.dest_node;
        });
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
        const std::string edge_key = canonical_edge_key(
            source_key, "out", "audio", destination_key, "in", "audio");
        const auto edge = authored_edges[edge_key];
        if (!edge.isObject() || !edge["channel_map"].isArray() ||
            edge["channel_map"].size() != channel_names.size())
            return PreparedDescriptionResult::refused(
                PreparedDescriptionRefusal::AuthoredBindingMismatch);
        for (std::size_t i = 0; i < channel_names.size(); ++i) {
            const auto pair = edge["channel_map"][static_cast<std::uint32_t>(i)];
            if (!pair.isArray() || pair.size() != 2 || !pair[0].isString() || !pair[1].isString() ||
                pair[0].getString() != channel_names[i] || pair[1].getString() != channel_names[i])
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
                 .insert(std::to_string(binding.node_id) + ":" + (binding.output ? "out:" : "in:") +
                         binding.authored_channel)
                 .second)
            return PreparedDescriptionResult::refused(
                PreparedDescriptionRefusal::AuthoredBindingMismatch);
        const auto node = std::find_if(nodes.begin(), nodes.end(),
                                       [&](const GraphNode& n) { return n.id == binding.node_id; });
        if (node == nodes.end() || binding.authored_ordinal >= 2 ||
            (binding.output ? node->num_output_ports : node->num_input_ports) != 2)
            return PreparedDescriptionResult::refused(
                PreparedDescriptionRefusal::AuthoredBindingMismatch);
    }
    std::size_t required_channels = 0;
    for (const auto& node : nodes)
        required_channels += static_cast<std::size_t>(node.num_input_ports + node.num_output_ports);
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
            !normalize_authored_identity_text(binding.interface_key, normalized_interface_key) ||
            normalized_interface_key != binding.interface_key || binding.authored_port != "audio" ||
            binding.authored_ordinal > 1 || binding.authored_channel.empty() ||
            binding.authored_channel != channel_names[binding.authored_ordinal] ||
            binding.runtime_bus_index != 0 || binding.runtime_channel_index > 1 ||
            binding.authored_ordinal != binding.runtime_channel_index ||
            !interfaces
                 .insert(binding.interface_key + ":" + (binding.output ? "out:" : "in:") +
                         std::to_string(binding.authored_ordinal))
                 .second)
            return PreparedDescriptionResult::refused(
                PreparedDescriptionRefusal::AuthoredBindingMismatch);
        const auto node = std::find_if(nodes.begin(), nodes.end(),
                                       [&](const GraphNode& n) { return n.id == binding.node_id; });
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
        const auto interface = std::find_if(
            bindings.interfaces.begin(), bindings.interfaces.end(),
            [&](const AuthoredInterfaceChannelBinding& binding) {
                return binding.interface_key == entry["key"].getString() &&
                       binding.output == output && binding.authored_ordinal == 0;
            });
        if (interface == bindings.interfaces.end())
            return PreparedDescriptionResult::refused(
                PreparedDescriptionRefusal::AuthoredBindingMismatch);
        result->interfaces.push_back({interface->interface_key,
                                      keys.at(interface->node_id),
                                      interface->authored_port,
                                      interface->output,
                                      channel_names});
    }
    result->nodes.reserve(nodes.size());
    for (const auto& node : nodes) {
        PreparedDescriptionNode description{keys.at(node.id),
                                            prepared_kind(node.type),
                                            node.type == NodeType::Gain ? "gain.v1" : "io.v1",
                                            node.type == NodeType::AudioInput
                                                ? "pulp.builtin.audio-input.v1"
                                                : (node.type == NodeType::AudioOutput
                                                       ? "pulp.builtin.audio-output.v1"
                                                       : "pulp.builtin.gain.v1"),
                                            static_cast<std::uint32_t>(node.num_input_ports),
                                            static_cast<std::uint32_t>(node.num_output_ports),
                                            std::bit_cast<std::uint32_t>(node.gain)};
        result->nodes.push_back(std::move(description));
        result->schedule.push_back(keys.at(node.id));
    }
    for (const auto& connection : candidate_->connections_) {
        const auto source = std::find_if(nodes.begin(), nodes.end(), [&](const GraphNode& n) {
            return n.id == connection.source_node;
        });
        const auto destination = std::find_if(nodes.begin(), nodes.end(), [&](const GraphNode& n) {
            return n.id == connection.dest_node;
        });
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
}

PreparedDescriptionResult
SignalGraph::PreparedTopologyEdit::build_prepared_description_(double sample_rate,
                                                               int max_block_size) const {
    if (!prepared_description_ || !std::isfinite(sample_rate) || sample_rate <= 0.0 ||
        max_block_size < 1 || max_block_size > 4096)
        return PreparedDescriptionResult::refused(
            prepared_description_ ? PreparedDescriptionRefusal::UnsupportedPreparedProfile
                                  : PreparedDescriptionRefusal::LogicalIdentityMissing);
    auto copy = std::make_shared<PreparedDescriptionData>(*prepared_description_);
    copy->sample_rate = sample_rate;
    copy->max_block_frames = static_cast<std::uint64_t>(max_block_size);
    return {PreparedDescriptionRefusal::None, std::move(copy)};
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
        if (baseline.type == NodeType::Plugin &&
            dynamic_cast<const PreparedOwnedBuiltInSlot*>(baseline.plugin.get()) == nullptr)
            return Result::BaselinePluginRemovalRequiresRelease;
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
    const auto selection_generation = owner_->execution_selection_generation_.load(std::memory_order_relaxed);
    if (snapshot_->prepared_description->execution_selection_generation != selection_generation)
        return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::SnapshotChanged);
    auto copy = std::make_shared<PreparedDescriptionData>(*snapshot_->prepared_description);
    if (owner_->execution_selection_generation_.load(std::memory_order_relaxed) != selection_generation)
        return PreparedDescriptionResult::refused(PreparedDescriptionRefusal::SnapshotChanged);
    return {PreparedDescriptionRefusal::None, std::move(copy)};
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

    GraphMutationLock owner_lock(*owner_);
    if (!base_is_current_locked_() || owner_->in_swap_edit_) {
        return last_result_ = Result::StaleBase;
    }

    if (owner_->spectral_mode_.load(std::memory_order_acquire)) {
        const auto unsupported = [](const auto& nodes) {
            return std::any_of(nodes.begin(), nodes.end(), [](const auto& n) {
                return n.type == NodeType::Plugin || n.type == NodeType::Custom;
            });
        };
        if (unsupported(owner_->nodes_) || unsupported(candidate_->nodes_)) {
            owner_->spectral_error_.store(SpectralError::UnsupportedTransactionalReplacement,
                                          std::memory_order_relaxed);
            return last_result_ = Result::PreflightFailed;
        }
    }
    GraphMutationLock candidate_lock(*candidate_);
    if (const auto rejection = sample_region_preparation_result_(sample_rate, max_block_size))
        return last_result_ = *rejection;
    const auto old_keepalive = owner_->spectral_mode_.load(std::memory_order_acquire)
                                   ? std::shared_ptr<CompiledGraph>{}
                                   : owner_->live_slot_.live();
    const auto* old = old_keepalive.get();
    const bool candidate_has_midi_output =
        std::any_of(candidate_->nodes_.begin(), candidate_->nodes_.end(),
                    [](const GraphNode& node) { return node.type == NodeType::MidiOutput; });
    const bool live_has_midi_output =
        old != nullptr &&
        std::any_of(old->shapes.begin(), old->shapes.end(),
                    [](const auto& entry) { return entry.second.type == NodeType::MidiOutput; });
    if (!owner_->spectral_mode_.load(std::memory_order_acquire) &&
        (candidate_has_midi_output || live_has_midi_output)) {
        return last_result_ = Result::MidiOutputSnapshotLocalRequired;
    }

    if (const auto rejection = baseline_removal_rejection_locked_())
        return last_result_ = *rejection;
    if (!candidate_->preflight_locked_(max_block_size) ||
        (candidate_->sample_region_definitions_.empty() &&
         candidate_->processing_order().size() != candidate_->nodes_.size())) {
        return last_result_ = Result::PreflightFailed;
    }
    const bool dimensions_changed = old != nullptr && (old->sample_rate != sample_rate ||
                                                       old->max_block_size != max_block_size);
    if (old != nullptr) {
        if (old->anticipation.valid ||
            owner_->anticipation_enabled_.load(std::memory_order_relaxed) ||
            candidate_->anticipation_enabled_.load(std::memory_order_relaxed)) {
            return last_result_ = Result::ExternalPluginReprepareRequired;
        }
    }

    for (const auto& [id, processor] : candidate_->processor_nodes_) {
        if (!processor)
            return last_result_ = Result::PreflightFailed;
        if (!is_new_node_(id)) {
            if (old == nullptr || dimensions_changed) {
                return last_result_ = Result::ExternalPluginReprepareRequired;
            }
            const auto live = old->processors.find(id);
            if (live == old->processors.end() || live->second.get() != processor.get()) {
                return last_result_ = Result::ExternalPluginReprepareRequired;
            }
            continue;
        }
        const auto* node = candidate_->node(id);
        if (node == nullptr)
            return last_result_ = Result::PreflightFailed;
        format::PrepareContext context;
        context.sample_rate = sample_rate;
        context.max_buffer_size = max_block_size;
        context.input_channels = node->num_input_ports;
        context.output_channels = node->num_output_ports;
        if (!processor->instance->prepare(context))
            return last_result_ = Result::ExternalPluginReprepareRequired;
    }

    // Existing external instances may be shared with an off-side compile, but
    // they must never be prepared or replaced by this transaction.
    for (const auto& n : candidate_->nodes_) {
        if (n.type != NodeType::Plugin || !n.plugin)
            continue;
        if (is_new_node_(n.id) &&
            dynamic_cast<const PreparedOwnedBuiltInSlot*>(n.plugin.get()) != nullptr) {
            try {
                if (!n.plugin->prepare(sample_rate, max_block_size))
                    return last_result_ = Result::ExternalPluginReprepareRequired;
            } catch (...) {
                return last_result_ = Result::ExternalPluginReprepareRequired;
            }
            candidate_->prepared_plugin_meta_[n.id] = SignalGraph::PreparedPluginMetadata{
                n.plugin->parameters(), std::max(0, n.plugin->latency_samples()),
                n.plugin->wants_transport()};
            continue;
        }
        if (old == nullptr) {
            return last_result_ = Result::ExternalPluginReprepareRequired;
        }
        if (dimensions_changed) {
            return last_result_ = Result::ExternalPluginReprepareRequired;
        }
        const auto live_plugin = old->plugins.find(n.id);
        if (live_plugin == old->plugins.end() || live_plugin->second.get() != n.plugin.get() ||
            candidate_->prepared_plugin_meta_.count(n.id) == 0) {
            return last_result_ = Result::ExternalPluginReprepareRequired;
        }
    }

    if (!replaced_registry_keys_.empty()) {
        return last_result_ = Result::CustomRegistryConflict;
    }

    // Existing custom instances are immutable identities for this operation.
    // Only nodes minted inside this edit may run create/load/prepare callbacks.
    for (auto& n : candidate_->nodes_) {
        if (n.type != NodeType::Custom || is_new_node_(n.id))
            continue;
        if (n.custom_state_pending) {
            return last_result_ = Result::ExistingCustomReprepareRequired;
        }
        const auto* type = candidate_->custom_node_type(n.custom_type_id, n.custom_type_version);
        if (dimensions_changed && type != nullptr && (type->prepare || type->release)) {
            return last_result_ = Result::ExistingCustomReprepareRequired;
        }
        if (old == nullptr) {
            if (type != nullptr && type->create) {
                return last_result_ = Result::ExistingCustomReprepareRequired;
            }
            continue;
        }
        const auto existing = old->custom_instances.find(n.id);
        if (existing == old->custom_instances.end() ||
            existing->second != n.custom_instance.get()) {
            return last_result_ = Result::ExistingCustomReprepareRequired;
        }
    }

    for (auto& n : candidate_->nodes_) {
        if (n.type != NodeType::Custom || !is_new_node_(n.id))
            continue;
        const auto* type = candidate_->custom_node_type(n.custom_type_id, n.custom_type_version);
        if (type == nullptr || type->num_input_ports != n.num_input_ports ||
            type->num_output_ports != n.num_output_ports || !type->create) {
            continue;
        }
        void* raw = type->create();
        if (raw == nullptr) {
            return last_result_ = Result::CustomInstanceCreateFailed;
        }
        auto destroy = type->destroy;
        n.custom_instance = std::shared_ptr<void>(raw, [destroy](void* p) {
            if (destroy && p)
                destroy(p);
        });
        prepared_new_custom_ids_.push_back(n.id);
        if (n.custom_state_pending && type->load_state) {
            if (!type->load_state(n.custom_instance.get(), n.custom_state_blob)) {
                return last_result_ = Result::CustomStateRestoreFailed;
            }
            n.custom_state_pending = false;
        }
        if (type->prepare) {
            type->prepare(n.custom_instance.get(), sample_rate, max_block_size);
        }
    }

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
        auto capture = std::make_shared<PreparedDescriptionData>(*prepared_description_);
        capture->sample_rate = sample_rate;
        capture->max_block_frames = static_cast<std::uint64_t>(max_block_size);
        capture->canonical_executor_routing_enabled =
            candidate_->canonical_executor_routing_enabled_.load(std::memory_order_relaxed);
        capture->parallel_routing_enabled =
            candidate_->parallel_routing_enabled_.load(std::memory_order_relaxed);
        capture->anticipation_enabled =
            candidate_->anticipation_enabled_.load(std::memory_order_relaxed);
        capture->execution_selection_generation =
            candidate_->execution_selection_generation_.load(std::memory_order_relaxed);
        capture->pdc_execution_domain = "dynamic";
        capture->executor_contract = "pulp.graph.canonical-executor.f32.v1";
        capture->execution_mode = "serial";
        capture->tie_break_contract = "ready-node-id-ascending.v1";
        capture->execution_fallback_policy = "internal_copy";
        const auto& limits = candidate_->limits_;
        capture->host_max_nodes = static_cast<std::uint64_t>(limits.max_nodes);
        capture->host_max_connections = static_cast<std::uint64_t>(limits.max_connections);
        capture->host_max_ports = static_cast<std::uint64_t>(limits.max_ports);
        capture->host_max_block_frames = limits.max_block_size > 0
                                            ? static_cast<std::uint64_t>(limits.max_block_size)
                                            : 0;
        capture->host_max_estimated_work_units =
            static_cast<std::uint64_t>(limits.max_estimated_work_units);
        // The routed planner is compiled with this closed, explicit limit set.
        // Capture those actual limits instead of copying GraphLimits, whose
        // defaults are intentionally broader than the executor contract.
        const graph::GraphRuntimeLimits runtime_limits{};
        capture->runtime_max_nodes = runtime_limits.max_nodes;
        capture->runtime_max_connections = runtime_limits.max_connections;
        capture->runtime_max_ports_per_node = runtime_limits.max_ports_per_node;
        capture->runtime_max_ports = runtime_limits.max_total_ports;
        // The routed pool is the runtime's actual frame capacity for this
        // prepared serial path. It is value-owned here rather than inferred
        // from the host request or a default limit struct.
        capture->runtime_max_block_frames = next->routed.serial.pool.max_frames();
        const auto effective_bound = [](std::uint64_t profile, std::uint64_t host,
                                        std::uint64_t runtime) {
            std::uint64_t result = profile;
            if (host != 0)
                result = std::min(result, host);
            if (runtime != 0)
                result = std::min(result, runtime);
            return result;
        };
        capture->effective_max_nodes = effective_bound(
            capture->profile_max_nodes, capture->host_max_nodes, capture->runtime_max_nodes);
        capture->effective_max_connections = effective_bound(
            capture->profile_max_connections, capture->host_max_connections,
            capture->runtime_max_connections);
        capture->effective_max_ports_per_node = effective_bound(
            capture->profile_max_ports_per_node, 0, capture->runtime_max_ports_per_node);
        capture->effective_max_ports = effective_bound(
            capture->profile_max_ports, capture->host_max_ports, capture->runtime_max_ports);
        capture->effective_max_block_frames = effective_bound(
            capture->profile_max_block_frames, capture->host_max_block_frames,
            capture->runtime_max_block_frames);
        if (capture->nodes.size() > capture->effective_max_nodes ||
            capture->edges.size() > capture->effective_max_connections ||
            std::any_of(capture->nodes.begin(), capture->nodes.end(), [&](const auto& node) {
                return std::max(node.input_ports, node.output_ports) >
                       capture->effective_max_ports_per_node;
            }) ||
            static_cast<std::uint64_t>(max_block_size) > capture->effective_max_block_frames)
            return last_result_ = Result::PreflightFailed;
        capture->snapshot_id = next->routed.serial.snapshot.id();
        // bound_to() compares the pool's private binding identity to this
        // snapshot. The public pool intentionally does not expose that token;
        // retain the paired snapshot identity as the detached provenance.
        capture->pool_snapshot_id = capture->snapshot_id;
        capture->owner_generation = base_authoring_generation_;
        const auto& plan = next->routed.serial.snapshot.plan();
        if (!next->routed.serial.snapshot.valid() || plan.nodes.size() != capture->nodes.size() ||
            plan.connections.size() != capture->edges.size() ||
            plan.nodes.size() > capture->effective_max_nodes ||
            plan.connections.size() > capture->effective_max_connections ||
            plan.nodes.size() > std::numeric_limits<std::uint32_t>::max() ||
            plan.connections.size() > std::numeric_limits<std::uint32_t>::max())
            return last_result_ = Result::PreflightFailed;
        capture->prepared_plan_node_count = plan.nodes.size();
        capture->prepared_plan_connection_count = plan.connections.size();
        capture->serial_pool_bound =
            next->routed.serial.pool.bound_to(next->routed.serial.snapshot);
        const auto& assignment = next->routed.serial.snapshot.buffer_assignment();
        if (assignment.nodes.size() != plan.nodes.size() ||
            assignment.connection_delay_samples.size() != plan.connections.size() ||
            assignment.reads_in_place.size() != plan.connections.size())
            return last_result_ = Result::PreflightFailed;
        capture->buffer_slot_count = assignment.slot_count;
        capture->pool_slot_count = next->routed.serial.pool.slot_count();
        capture->pool_max_frames = next->routed.serial.pool.max_frames();
        capture->total_latency_samples = assignment.total_latency_samples;
        capture->connection_delay_samples = assignment.connection_delay_samples;
        capture->reads_in_place = assignment.reads_in_place;
        if (!capture->serial_pool_bound || capture->pool_slot_count != capture->buffer_slot_count ||
            capture->pool_max_frames < capture->max_block_frames)
            return last_result_ = Result::PreflightFailed;
        capture->host_input_alias_eligible = false;
        capture->host_input_alias_mode = "conditional_direct_host_input";
        capture->host_input_alias_fallback = "internal_copy";
        capture->host_input_alias_requires_full_width = true;
        capture->host_input_alias_requires_disjoint = true;
        capture->host_input_alias_mappings.clear();
        const auto mode_name = [](format::GraphRuntimeSnapshot::InputPortSource::Mode mode) {
            switch (mode) {
            case format::GraphRuntimeSnapshot::InputPortSource::Mode::Zero:
                return "zero";
            case format::GraphRuntimeSnapshot::InputPortSource::Mode::InPlace:
                return "in_place";
            case format::GraphRuntimeSnapshot::InputPortSource::Mode::Copy:
                return "copy";
            case format::GraphRuntimeSnapshot::InputPortSource::Mode::Sum:
                return "sum";
            }
            return "unknown";
        };
        const auto bindings = next->routed.serial.snapshot.bindings();
        if (bindings.size() != plan.nodes.size())
            return last_result_ = Result::PreflightFailed;
        std::vector<std::size_t> authored_positions(plan.nodes.size());
        std::vector<std::size_t> processing_positions(plan.nodes.size(),
                                                       std::numeric_limits<std::size_t>::max());
        for (std::size_t position = 0; position < plan.processing_order_indices.size(); ++position) {
            const auto dense = plan.processing_order_indices[position];
            if (dense >= plan.nodes.size() ||
                processing_positions[dense] != std::numeric_limits<std::size_t>::max())
                return last_result_ = Result::PreflightFailed;
            processing_positions[dense] = position;
        }
        if (plan.processing_order_indices.size() != plan.nodes.size())
            return last_result_ = Result::PreflightFailed;
        capture->nodes.resize(plan.nodes.size());
        for (std::uint32_t dense = 0; dense < plan.nodes.size(); ++dense) {
            const auto authored = std::find_if(
                candidate_->nodes_.begin(), candidate_->nodes_.end(),
                [&](const GraphNode& node) { return node.id == plan.nodes[dense].id; });
            if (authored == candidate_->nodes_.end() || dense >= assignment.nodes.size())
                return last_result_ = Result::PreflightFailed;
            const auto position = static_cast<std::size_t>(authored - candidate_->nodes_.begin());
            if (position >= capture->nodes.size())
                return last_result_ = Result::PreflightFailed;
            authored_positions[dense] = position;
            auto& node = capture->nodes[position];
            node.input_ports = plan.nodes[dense].input_ports;
            node.output_ports = plan.nodes[dense].output_ports;
            node.latency_samples = plan.nodes[dense].latency_samples;
            node.persistent_output = plan.nodes[dense].persistent_output;
            node.reads_inputs_only = bindings[dense].reads_inputs_only;
            node.transport_sensitive = authored->transport_sensitive;
            node.tail_samples = 0;
            if (node.input_ports > runtime_limits.max_ports_per_node ||
                node.output_ports > runtime_limits.max_ports_per_node)
                return last_result_ = Result::PreflightFailed;
        }
        std::uint64_t total_ports = 0;
        for (const auto& node : plan.nodes) {
            if (node.input_ports > std::numeric_limits<std::uint64_t>::max() - total_ports)
                return last_result_ = Result::PreflightFailed;
            total_ports += node.input_ports;
            if (node.output_ports > std::numeric_limits<std::uint64_t>::max() - total_ports)
                return last_result_ = Result::PreflightFailed;
            total_ports += node.output_ports;
        }
        if (total_ports > capture->effective_max_ports)
            return last_result_ = Result::PreflightFailed;

        capture->input_modes.clear();
        capture->schedule.clear();
        for (std::uint32_t dense = 0; dense < plan.nodes.size(); ++dense) {
            const auto& key = capture->nodes[authored_positions[dense]].authored_key;
            const auto sources = next->routed.serial.snapshot.input_port_sources(dense);
            if (sources.size() != plan.nodes[dense].input_ports)
                return last_result_ = Result::PreflightFailed;
            for (std::uint32_t port = 0; port < sources.size(); ++port) {
                if (sources[port].mode == format::GraphRuntimeSnapshot::InputPortSource::Mode::Sum)
                    return last_result_ = Result::PreflightFailed;
                capture->input_modes.push_back(key + ":port=audio:channel=" +
                                               std::to_string(port) + ",mode=" +
                                               mode_name(sources[port].mode));
            }
        }
        for (const auto dense_index : plan.processing_order_indices)
            capture->schedule.push_back(capture->nodes[authored_positions[dense_index]].authored_key);

        // Copy the actual node binding and edge lowering facts. The initial
        // authored record is deliberately not trusted as a plan substitute.
        for (std::uint32_t dense = 0; dense < plan.nodes.size(); ++dense) {
            const auto& pnode = plan.nodes[dense];
            auto& node = capture->nodes[authored_positions[dense]];
            node.input_ports = pnode.input_ports;
            node.output_ports = pnode.output_ports;
            node.latency_samples = pnode.latency_samples;
            node.persistent_output = pnode.persistent_output;
            node.reads_inputs_only = bindings[dense].reads_inputs_only;
        }
        const auto final_mode = [&](std::uint32_t dense, std::uint32_t port) {
            const auto sources = next->routed.serial.snapshot.input_port_sources(dense);
            return sources.size() > port ? mode_name(sources[port].mode) : "unknown";
        };
        std::vector<PreparedDescriptionEdge> actual_edges;
        actual_edges.reserve(plan.connections.size());
        for (std::uint32_t index = 0; index < plan.connections.size(); ++index) {
            const auto& connection = plan.connections[index];
            if (connection.source_index >= plan.nodes.size() || connection.dest_index >= plan.nodes.size() ||
                index >= assignment.connection_delay_samples.size() ||
                index >= assignment.reads_in_place.size())
                return last_result_ = Result::PreflightFailed;
            const auto& source_key = capture->nodes[authored_positions[connection.source_index]].authored_key;
            const auto& destination_key =
                capture->nodes[authored_positions[connection.dest_index]].authored_key;
            std::size_t gather = 0;
            const auto& destination = plan.nodes[connection.dest_index];
            bool found = false;
            for (std::uint32_t offset = 0; offset < destination.inbound_connection_count; ++offset) {
                const auto at = destination.first_inbound_connection + offset;
                if (at < plan.inbound_connection_indices.size() &&
                    plan.inbound_connection_indices[at] == index) {
                    gather = offset;
                    found = true;
                    break;
                }
            }
            if (!found)
                return last_result_ = Result::PreflightFailed;
            const std::string_view mode = final_mode(connection.dest_index, connection.dest_port);
            if (mode == "sum" || mode == "unknown" || !graph::carries_audio(connection) ||
                connection.feedback ||
                assignment.connection_delay_samples[index] != 0)
                return last_result_ = Result::PreflightFailed;
            actual_edges.push_back({source_key + ":" + std::to_string(connection.source_port),
                                    destination_key + ":" + std::to_string(connection.dest_port),
                                    connection.source_port, connection.dest_port, std::string(mode),
                                    static_cast<std::uint32_t>(gather),
                                    assignment.connection_delay_samples[index], connection.feedback,
                                    assignment.reads_in_place[index] != 0, std::string(mode)});
        }
        capture->edges = std::move(actual_edges);

        // Recompute the host-input policy using the same facts consumed by
        // GraphRuntimeBufferPool::bind(), including read-only bindings and
        // resolved in-place input modes. The policy is conditional; it never
        // exports a callback-time pointer.
        std::uint32_t input_dense = 0;
        std::size_t input_count = 0;
        for (std::uint32_t dense = 0; dense < plan.nodes.size(); ++dense) {
            if (plan.nodes[dense].kind == graph::GraphRuntimeNodeKind::AudioInput) {
                input_dense = dense;
                ++input_count;
            }
        }
        bool aliasable = input_count == 1;
        for (const auto& connection : plan.connections) {
            if (!aliasable || connection.source_index != input_dense)
                continue;
            const auto ports = next->routed.serial.snapshot.input_port_sources(connection.dest_index);
            const auto& destination = plan.nodes[connection.dest_index];
            const bool read_only = destination.kind == graph::GraphRuntimeNodeKind::AudioOutput ||
                                   bindings[connection.dest_index].reads_inputs_only;
            if (!graph::carries_audio(connection) || connection.feedback || !read_only ||
                connection.dest_port >= ports.size() ||
                ports[connection.dest_port].mode !=
                    format::GraphRuntimeSnapshot::InputPortSource::Mode::InPlace)
                aliasable = false;
        }
        capture->host_input_alias_eligible = aliasable && input_count == 1;
        if (capture->host_input_alias_eligible &&
            plan.nodes[input_dense].output_ports != capture->channels.size())
            capture->host_input_alias_eligible = false;
        if (capture->host_input_alias_eligible) {
            capture->host_input_alias_source = capture->nodes[authored_positions[input_dense]].authored_key +
                                               ":audio:out";
            capture->host_input_alias_view = capture->nodes[authored_positions[input_dense]].authored_key +
                                             ":audio:out";
            for (const auto& channel : capture->channels)
                capture->host_input_alias_mappings.push_back(
                    capture->host_input_alias_source + ":channel=" + channel +
                    "->" + capture->host_input_alias_view + ":channel=" + channel);
        }

        // Normalize the actual slot classes and per-value processing lifetimes.
        capture->buffer_classes.clear();
        capture->buffer_values.clear();
        std::vector<bool> used_slots(assignment.slot_count, false);
        bool invalid_slot = false;
        const auto add_class = [&](std::uint32_t slot) {
            if (slot < used_slots.size())
                used_slots[slot] = true;
            else
                invalid_slot = true;
        };
        const auto add_value = [&](std::string value) { capture->buffer_values.push_back(std::move(value)); };
        for (std::uint32_t dense = 0; dense < plan.nodes.size(); ++dense) {
            const auto& pnode = plan.nodes[dense];
            const auto& slots = assignment.nodes[dense];
            const auto& key = capture->nodes[authored_positions[dense]].authored_key;
            const auto position = processing_positions[dense];
            const auto sources = next->routed.serial.snapshot.input_port_sources(dense);
            for (std::uint32_t port = 0; port < pnode.input_ports; ++port) {
                const auto mode = sources[port].mode;
                const auto slot = mode == format::GraphRuntimeSnapshot::InputPortSource::Mode::InPlace
                                      ? sources[port].slot
                                      : slots.input_base + port;
                add_class(slot);
                add_value(key + ":port=audio:" + (mode == format::GraphRuntimeSnapshot::InputPortSource::Mode::InPlace
                                                         ? "direction=in,role=input_view"
                                                         : "direction=in,role=input_scratch") +
                          ",channel=" + std::to_string(port) + ",phase=gather,ordinal=" +
                          std::to_string(port) + ",mode=" + mode_name(mode) + ",class=" +
                          std::to_string(slot) + ",lifetime=[" + std::to_string(position) + "," +
                          std::to_string(position + 1) + ")");
            }
            for (std::uint32_t port = 0; port < pnode.output_ports; ++port) {
                const auto slot = slots.output_base + port;
                add_class(slot);
                std::size_t last_reader = position;
                for (const auto& connection : plan.connections)
                    if (connection.source_index == dense && connection.source_port == port)
                        last_reader = std::max(last_reader, processing_positions[connection.dest_index]);
                add_value(key + ":port=audio:direction=out,role=node_output,channel=" +
                          std::to_string(port) + ",phase=execute,ordinal=" + std::to_string(port) +
                          ",mode=write,class=" + std::to_string(slot) + ",lifetime=[" +
                          std::to_string(position) + "," + std::to_string(last_reader + 1) + ")");
            }
        }
        for (std::uint32_t slot = 0; slot < used_slots.size(); ++slot)
            if (used_slots[slot])
                capture->buffer_classes.push_back("storage_kind=scratch,capacity_frames=" +
                                                  std::to_string(capture->max_block_frames) +
                                                  ",sample_format=f32,initialization=zero,class=" +
                                                  std::to_string(slot));
        std::sort(capture->buffer_values.begin(), capture->buffer_values.end());
        if (invalid_slot)
            return last_result_ = Result::PreflightFailed;

        // Slot numbers are executor-local and may change after a harmless
        // allocator renumbering. Assign portable class ordinals in the
        // canonical access order, then rewrite both the class records and
        // every value record to that normalized ordinal.
        std::unordered_map<std::uint32_t, std::uint32_t> normalized_classes;
        const auto normalize_class = [&](std::string& record) {
            const auto marker = record.find("class=");
            if (marker == std::string::npos)
                return false;
            const auto begin = marker + 6;
            const auto end = record.find(',', begin);
            const auto token = record.substr(begin, end == std::string::npos ? std::string::npos
                                                                            : end - begin);
            std::uint32_t raw = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), raw);
            if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size())
                return false;
            const auto [it, inserted] = normalized_classes.emplace(
                raw, static_cast<std::uint32_t>(normalized_classes.size()));
            const auto replacement = "class=" + std::to_string(it->second);
            record.replace(marker, (end == std::string::npos ? record.size() : end) - marker,
                           replacement);
            return true;
        };
        for (auto& value : capture->buffer_values)
            if (!normalize_class(value))
                return last_result_ = Result::PreflightFailed;
        for (auto& value : capture->buffer_classes)
            if (!normalize_class(value))
                return last_result_ = Result::PreflightFailed;
        std::sort(capture->buffer_values.begin(), capture->buffer_values.end());
        std::sort(capture->buffer_classes.begin(), capture->buffer_classes.end(),
                  [](const std::string& lhs, const std::string& rhs) {
                      const auto l = lhs.find("class=");
                      const auto r = rhs.find("class=");
                      return lhs.substr(l + 6) < rhs.substr(r + 6);
                  });
        next->prepared_description = std::move(capture);
    }

    if (old != nullptr) {
        // Adopt stable runtime ingress/egress identities before publication. A
        // control-thread MIDI/parameter batch published during compilation is
        // therefore visible to exactly one audio block across the boundary.
        for (auto& [id, rt] : next->runtime) {
            const auto live_rt = old->runtime.find(id);
            const auto live_shape = old->shapes.find(id);
            const auto next_shape = next->shapes.find(id);
            if (live_rt == old->runtime.end() || live_shape == old->shapes.end() ||
                next_shape == next->shapes.end() ||
                live_shape->second.type != next_shape->second.type) {
                continue;
            }
            if (rt.parameter_input_mailbox
                && live_rt->second.parameter_input_mailbox
                && rt.exact_parameter_event_owner.lock()
                    == live_rt->second.exact_parameter_event_owner.lock()) {
                rt.parameter_input_mailbox = live_rt->second.parameter_input_mailbox;
            }
            if (rt.midi_input_mailbox && live_rt->second.midi_input_mailbox) {
                rt.midi_input_mailbox = live_rt->second.midi_input_mailbox;
            }
        }
        const auto rebind_parameter_sequence = [&](CompiledGraph::RoutedPath& path) {
            for (auto& ctx : path.plugin_ctx) {
                const auto rt = next->runtime.find(ctx.node_id);
                ctx.parameter_events_live_sequence_seen =
                    rt != next->runtime.end() && rt->second.parameter_input_mailbox
                        ? &rt->second.parameter_input_mailbox->sequence_seen
                        : nullptr;
                ctx.parameter_events_exact_sequence_seen =
                    rt != next->runtime.end()
                            && rt->second.exact_parameter_input_mailbox
                        ? &rt->second.exact_parameter_input_mailbox->sequence_seen
                        : nullptr;
            }
        };
        rebind_parameter_sequence(next->routed.serial);
        rebind_parameter_sequence(next->routed.parallel);

        // A sample-rate/block-size transition intentionally starts fresh DSP
        // history. Stable mailbox identities above still preserve any pending
        // MIDI/parameter publication, but differently-sized PDC/feedback rings
        // are never copied or shared across the boundary.
        if (dimensions_changed) {
            prepared_snapshot_ = std::move(next);
            return last_result_ = Result::Prepared;
        }

        // Feedback owns previous-block samples per snapshot and is not safely
        // adoptable. Feed-forward PDC carries only the unchanged identity+shape
        // intersection: removed rings retire with the old snapshot, newly
        // delayed or reshaped rings start fresh, and no stale history can attach
        // to a disconnect+reconnect edge that minted a new identity.
        if (has_feedback(old->connections) || has_feedback(next->connections) ||
            old->connection_identities.size() != old->connection_delays.size() ||
            next->connection_identities.size() != next->connection_delays.size()) {
            return last_result_ = Result::RuntimeAdoptionFailed;
        }

        std::unordered_map<std::uint64_t, std::size_t> old_delayed;
        for (std::size_t i = 0; i < old->connection_delays.size(); ++i) {
            if (old->connection_delays[i].delay_samples > 0) {
                old_delayed.emplace(old->connection_identities[i], i);
            }
        }
        std::vector<std::pair<std::size_t, std::size_t>> matched;
        for (std::size_t i = 0; i < next->connection_delays.size(); ++i) {
            const auto& delay = next->connection_delays[i];
            if (delay.delay_samples <= 0)
                continue;
            const auto found = old_delayed.find(next->connection_identities[i]);
            if (found == old_delayed.end())
                continue;
            const auto& live_delay = old->connection_delays[found->second];
            if (delay.delay_samples != live_delay.delay_samples || !delay.state ||
                !live_delay.state || delay.state->ring.size() != live_delay.state->ring.size()) {
                continue;
            }
            matched.emplace_back(i, found->second);
        }
        for (const auto [candidate_index, live_index] : matched) {
            bool routed_state_compatible = next->pdc_execution_domain == old->pdc_execution_domain;
            if (routed_state_compatible &&
                next->pdc_execution_domain == PdcExecutionDomain::RoutedSerial) {
                routed_state_compatible =
                    next->routed.serial.valid && old->routed.serial.valid &&
                    next->routed.serial.pool.can_adopt_delay_ring_state(
                        static_cast<std::uint32_t>(candidate_index), old->routed.serial.pool,
                        static_cast<std::uint32_t>(live_index));
            } else if (routed_state_compatible &&
                       next->pdc_execution_domain == PdcExecutionDomain::RoutedParallel) {
                routed_state_compatible =
                    next->routed.parallel.valid && old->routed.parallel.valid &&
                    next->routed.parallel.pool.can_adopt_delay_ring_state(
                        static_cast<std::uint32_t>(candidate_index), old->routed.parallel.pool,
                        static_cast<std::uint32_t>(live_index));
            }
            if (!routed_state_compatible)
                continue;
            next->connection_delays[candidate_index].state =
                old->connection_delays[live_index].state;
            if (next->pdc_execution_domain == PdcExecutionDomain::RoutedSerial) {
                (void)next->routed.serial.pool.adopt_delay_ring_state(
                    static_cast<std::uint32_t>(candidate_index), old->routed.serial.pool,
                    static_cast<std::uint32_t>(live_index));
            }
            if (next->pdc_execution_domain == PdcExecutionDomain::RoutedParallel) {
                (void)next->routed.parallel.pool.adopt_delay_ring_state(
                    static_cast<std::uint32_t>(candidate_index), old->routed.parallel.pool,
                    static_cast<std::uint32_t>(live_index));
            }
        }
    }

    prepared_snapshot_ = std::move(next);
    return last_result_ = Result::Prepared;
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
    if (owner_->refuse_spectral_mutation_())
        return last_result_ = Result::PreflightFailed;
    if (!base_is_current_locked_() || owner_->in_swap_edit_)
        return last_result_ = Result::StaleBase;
    if (const auto rejection = baseline_removal_rejection_locked_())
        return last_result_ = *rejection;

    {
        GraphMutationLock candidate_lock(*candidate_);
        if (const auto rejection = sample_region_preparation_result_(sample_rate, max_block_size))
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
            owner_->nodes_.begin(), owner_->nodes_.end(),
            [&](const GraphNode& current) {
                return current.id == node.id && current.custom_instance == node.custom_instance;
            });
        if (owner_node == owner_->nodes_.end())
            continue;
        if (replaced_registry_keys_.count(
                prepared_custom_key(node.custom_type_id, node.custom_type_version)) != 0) {
            return last_result_ = Result::ExistingCustomReprepareRequired;
        }
        const auto* candidate_type = candidate_->custom_node_type(
            node.custom_type_id, node.custom_type_version);
        const auto* owner_type = owner_->custom_node_type(
            owner_node->custom_type_id, owner_node->custom_type_version);
        if (candidate_type != nullptr && candidate_type->prepare && owner_type != nullptr) {
            quiesced_customs_.push_back(
                {node.custom_instance, owner_type->prepare, owner_type->release,
                 false});
        }
    }
    quiesced_lifecycles_dirty_ =
        !quiesced_processors_.empty() || !quiesced_plugins_.empty() || !quiesced_customs_.empty();

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
        candidate_prepared = candidate_->prepare_impl_(
            sample_rate, max_block_size, &lifecycle_observer);
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
        const auto owner_node = std::find_if(
            owner_->nodes_.begin(), owner_->nodes_.end(),
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

    std::shared_ptr<const PreparedDescriptionData> reminted_description;
    if (prepared_snapshot_->prepared_description) {
        if (owner_->authoring_generation_ == std::numeric_limits<std::uint64_t>::max())
            return fail_before_commit(Result::RuntimeAdoptionFailed);
        try {
            auto value = std::make_shared<PreparedDescriptionData>(
                *prepared_snapshot_->prepared_description);
            value->owner_graph_identity = owner_->diagnostic_graph_identity_;
            value->owner_generation = owner_->authoring_generation_ + 1;
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
        throw;
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
    this->nodes_ = std::move(candidate.nodes_);
    this->processor_nodes_ = std::move(candidate.processor_nodes_);
    this->connections_ = std::move(candidate.connections_);
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
