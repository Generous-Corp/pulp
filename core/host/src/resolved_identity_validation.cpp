// SPDX-License-Identifier: MIT
#include "resolved_identity_validation.hpp"
#include "resolved_identity_json.hpp"

#include <choc/text/choc_JSON.h>
#include <pulp/host/identity_text.hpp>
#include <pulp/host/prepared_description.hpp>
#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace pulp::host {
// Reuse the frozen D1/D2 parser and schema authority.
bool validate_resolved_semantic_payload(std::string_view);

namespace detail {
namespace {
using V = choc::value::ValueView;
using J = choc::value::Value;
using Encoder = std::string (*)(V);
constexpr std::string_view kSemanticDomain = "pulp.graph.semantic.v1";
constexpr std::string_view kSemanticProfile = "pulp.graph.identity.v1";
constexpr std::string_view kResolvedProfile = "pulp.graph.resolved.io-gain.v1";
constexpr std::string_view kExecutor = "pulp.graph.canonical-executor.f32.v1";
constexpr auto kU32 = std::numeric_limits<std::uint32_t>::max();

bool text(V value, std::string_view expected) {
    return value.isString() && value.getString() == expected;
}
bool shape(V value, std::initializer_list<std::string_view> fields) {
    if (!value.isObject() || value.size() != fields.size())
        return false;
    for (auto field : fields)
        if (!value.hasObjectMember(field))
            return false;
    return true;
}
bool unsigned_value(V value, std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max()) {
    const auto integer = resolved_json::number(value);
    return integer && *integer <= maximum;
}
std::uint64_t u(V value) {
    return resolved_json::number(value).value();
}
std::string s(V value) {
    return std::string(value.getString());
}
bool boolean(V value, bool expected) {
    return value.isBool() && value.getBool() == expected;
}
bool key_text(std::string_view value) {
    std::string normalized;
    return !value.empty() && value.size() <= 4096 &&
           normalize_authored_identity_text(value, normalized) && normalized == value;
}
bool key_path(V value) {
    return value.isArray() && value.size() == 1 && value[0].isString() &&
           key_text(value[0].getString());
}
bool less_bytes(std::string_view a, std::string_view b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](unsigned char x, unsigned char y) { return x < y; });
}
std::optional<std::uint64_t> floating_bits(V value, unsigned digits) {
    if (!value.isString())
        return {};
    const auto value_text = value.getString();
    const auto prefix = digits == 8 ? "f32:" : "f64:";
    if (value_text.size() != digits + 4 || !value_text.starts_with(prefix))
        return {};
    std::uint64_t bits = 0;
    for (char c : value_text.substr(4)) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return {};
        bits = (bits << 4) | static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    return bits;
}
struct Node {
    V record;
    std::string name;
    unsigned position = 0;
    bool input = false;
    bool output = false;
    bool persistent = false;
    unsigned inputs = 0;
    unsigned outputs = 0;
};
struct Access {
    std::string node, channel, role, phase, mode;
    bool output = false;
    unsigned ordinal = 0;
    auto operator<=>(const Access&) const = default;
};
struct ValueKey {
    std::string node, channel, phase;
    bool output = false;
    unsigned ordinal = 0;
    auto operator<=>(const ValueKey&) const = default;
};
struct ExpectedValue {
    std::set<Access> accesses;
    unsigned begin = 0, end = 0;
    std::optional<std::pair<unsigned, unsigned>> last_read;
    std::uint64_t class_id = 0;
    bool external_output = false;
};
unsigned phase_index(std::string_view phase) {
    return phase == "gather" ? 0u : phase == "execute" ? 1u : 2u;
}
bool known_phase(V phase) {
    return text(phase, "gather") || text(phase, "execute") || text(phase, "output_copy");
}

class Validator {
  public:
    Validator(V projection, V authored, Encoder encoder)
        : p_(projection), authored_(authored), encode_(encoder) {}

    bool run() {
        return preparation() && nodes_and_schedule() && interfaces() && edges() && admission() &&
               alias_policy() && storage();
    }

  private:
    bool preparation() {
        const auto prep = p_["preparation"];
        if (!shape(prep, {"contract", "sample_rate", "max_block_frames", "sample_precision"}) ||
            !text(prep["contract"], "pulp.graph.prepare.io-gain.serial.v1") ||
            !text(prep["sample_precision"], "f32") ||
            !unsigned_value(prep["max_block_frames"], 4096) || u(prep["max_block_frames"]) == 0)
            return false;
        const auto bits = floating_bits(prep["sample_rate"], 16);
        if (!bits)
            return false;
        const auto rate = std::bit_cast<double>(*bits);
        if (!std::isfinite(rate) || rate <= 0)
            return false;
        frames_ = u(prep["max_block_frames"]);
        const auto e = p_["execution"];
        return shape(e,
                     {"backend", "executor_contract", "mode", "tie_break_contract",
                      "fallback_policy", "pdc_domain", "canonical", "parallel", "anticipation"}) &&
               text(e["backend"], kExecutor) && text(e["executor_contract"], kExecutor) &&
               text(e["mode"], "serial") &&
               text(e["tie_break_contract"], "ready-node-id-ascending.v1") &&
               text(e["fallback_policy"], "internal_copy") && text(e["pdc_domain"], "dynamic") &&
               boolean(e["canonical"], true) && boolean(e["parallel"], false) &&
               boolean(e["anticipation"], false) && unsigned_value(p_["total_latency_samples"]) &&
               u(p_["total_latency_samples"]) == 0;
    }

    bool nodes_and_schedule() {
        const auto records = p_["nodes"], channels = p_["channels"], schedule = p_["schedule"];
        if (!records.isObject() || records.size() > 512 ||
            records.size() != authored_["nodes"].size() || !channels.isArray() ||
            channels.size() != (records.size() ? 2u : 0u) || !schedule.isArray() ||
            schedule.size() != records.size())
            return false;
        for (unsigned i = 0; i < channels.size(); ++i) {
            if (!channels[i].isString() || !key_text(channels[i].getString()))
                return false;
            channels_.push_back(s(channels[i]));
        }
        if (channels_.size() == 2 && channels_[0] == channels_[1])
            return false;
        std::map<std::string, unsigned> positions;
        for (unsigned i = 0; i < schedule.size(); ++i) {
            if (!key_path(schedule[i]) || !positions.emplace(s(schedule[i][0]), i).second)
                return false;
        }
        unsigned inputs = 0, outputs = 0;
        for (unsigned i = 0; i < records.size(); ++i) {
            const auto member = records.getObjectMemberAt(i);
            const auto key = choc::json::parse(std::string(member.name));
            if (!key_path(key) || encode_(key) != member.name)
                return false;
            const auto name = s(key[0]);
            if (!positions.contains(name) || !authored_["nodes"].hasObjectMember(name))
                return false;
            const auto a = authored_["nodes"][name], n = member.value;
            if (!shape(n, {"kind", "kind_contract", "implementation", "input_ports", "output_ports",
                           "gain", "latency_samples", "tail_samples", "transport_sensitive",
                           "persistent_output", "required", "reads_inputs_only",
                           "preserve_parameter_events", "audio_rate_delivery"}))
                return false;
            const bool input = text(a["kind"], "audio_input"),
                       output = text(a["kind"], "audio_output");
            const bool gain = text(a["kind"], "gain");
            if ((!input && !output && !gain) ||
                !text(n["kind"], input    ? "AudioInput"
                                 : output ? "AudioOutput"
                                          : "Gain") ||
                encode_(n["kind_contract"]) != encode_(a["kind_contract"]) ||
                encode_(n["implementation"]) != encode_(a["implementation"]) ||
                !unsigned_value(n["input_ports"], 2) || !unsigned_value(n["output_ports"], 2) ||
                u(n["input_ports"]) != (input ? 0u : 2u) ||
                u(n["output_ports"]) != (output ? 0u : 2u) ||
                !unsigned_value(n["latency_samples"]) || u(n["latency_samples"]) != 0 ||
                !unsigned_value(n["tail_samples"]) || u(n["tail_samples"]) != 0 ||
                !boolean(n["transport_sensitive"], false) ||
                !boolean(n["persistent_output"], false) || !boolean(n["required"], gain) ||
                !boolean(n["reads_inputs_only"], gain) ||
                !boolean(n["preserve_parameter_events"], false) ||
                !text(n["audio_rate_delivery"], "none"))
                return false;
            const auto bits = floating_bits(n["gain"], 8);
            if (!bits || !std::isfinite(std::bit_cast<float>(static_cast<std::uint32_t>(*bits))) ||
                (gain ? encode_(n["gain"]) != encode_(a["config"]["gain"]) : *bits != 0x3f800000u))
                return false;
            for (const auto direction : {"in", "out"}) {
                const bool active = std::string_view(direction) == "in" ? !input : !output;
                if (active &&
                    encode_(a["ports"][direction]["audio"]["channels"]) != encode_(channels))
                    return false;
            }
            Node node{n,
                      name,
                      positions.at(name),
                      input,
                      output,
                      n["persistent_output"].getBool(),
                      static_cast<unsigned>(u(n["input_ports"])),
                      static_cast<unsigned>(u(n["output_ports"]))};
            if (!nodes_.emplace(name, std::move(node)).second)
                return false;
            inputs += input;
            outputs += output;
            ports_ += u(n["input_ports"]) + u(n["output_ports"]);
            if (input)
                input_node_ = name;
        }
        return records.size() == 0 || (inputs == 1 && outputs == 1);
    }

    bool endpoint(V e, bool output, bool mono, std::string* name = nullptr,
                  std::string* channel = nullptr) const {
        if (!shape(e, {"node", "direction", "kind", "port", "channel", "parameter"}) ||
            !key_path(e["node"]) || !text(e["direction"], output ? "out" : "in") ||
            !text(e["kind"], "audio") || !text(e["port"], "audio") || !e["parameter"].isVoid())
            return false;
        const auto node = s(e["node"][0]);
        const auto found = nodes_.find(node);
        if (found == nodes_.end() || (output ? found->second.outputs : found->second.inputs) != 2)
            return false;
        if (mono) {
            if (!e["channel"].isString() ||
                std::find(channels_.begin(), channels_.end(), s(e["channel"])) == channels_.end())
                return false;
            if (channel)
                *channel = s(e["channel"]);
        } else if (!e["channel"].isVoid())
            return false;
        if (name)
            *name = node;
        return true;
    }

    bool interfaces() {
        const auto list = p_["interfaces"];
        if (!list.isArray() || list.size() != authored_["interfaces"].size() ||
            list.size() != (nodes_.empty() ? 0u : 2u))
            return false;
        for (unsigned i = 0; i < list.size(); ++i) {
            const auto value = list[i], a = authored_["interfaces"][i];
            if (!shape(value, {"key", "role", "kind", "endpoint", "channels", "bus_index",
                               "channel_indices"}) ||
                encode_(value["key"]) != encode_(a["key"]) ||
                encode_(value["role"]) != encode_(a["role"]) || !text(value["kind"], "audio") ||
                encode_(value["channels"]) != encode_(p_["channels"]) ||
                !unsigned_value(value["bus_index"], 0) ||
                encode_(value["channel_indices"]) != "[0,1]")
                return false;
            std::string name;
            if (!endpoint(value["endpoint"], text(value["role"], "input"), false, &name) ||
                name != s(a["endpoint"]["node"]))
                return false;
        }
        return true;
    }

    bool edges() {
        const auto list = p_["edges"];
        if (!list.isArray() || list.size() > 2048 || list.size() != authored_["edges"].size() * 2)
            return false;
        std::set<std::tuple<std::string, std::string, std::string>> expected, actual;
        for (unsigned i = 0; i < authored_["edges"].size(); ++i) {
            const auto edge = authored_["edges"].getObjectMemberAt(i).value;
            for (const auto& channel : channels_)
                expected.emplace(s(edge["source"]["node"]), s(edge["destination"]["node"]),
                                 channel);
        }
        std::string previous;
        std::map<std::string, std::set<unsigned>> ordinals;
        for (unsigned i = 0; i < list.size(); ++i) {
            const auto e = list[i];
            if (!shape(e, {"source", "destination", "channel_source", "channel_destination",
                           "input_mode", "gather_ordinal", "pdc_delay_samples", "feedback",
                           "assignment_allowed", "final_input_mode"}))
                return false;
            std::string source, destination;
            if (!endpoint(e["source"], true, false, &source) ||
                !endpoint(e["destination"], false, false, &destination) ||
                !e["channel_source"].isString() || !e["channel_destination"].isString())
                return false;
            const auto channel = s(e["channel_source"]);
            if (channel != s(e["channel_destination"]) ||
                std::find(channels_.begin(), channels_.end(), channel) == channels_.end() ||
                nodes_.at(source).position >= nodes_.at(destination).position ||
                !unsigned_value(e["pdc_delay_samples"], 0) || !text(e["feedback"], "none") ||
                !(text(e["input_mode"], "copy") || text(e["input_mode"], "in_place")) ||
                encode_(e["input_mode"]) != encode_(e["final_input_mode"]) ||
                !e["assignment_allowed"].isBool() || !unsigned_value(e["gather_ordinal"], kU32))
                return false;
            auto tuple = choc::value::createEmptyArray();
            tuple.addArrayElement(e["source"]);
            tuple.addArrayElement(e["destination"]);
            tuple.addArrayElement(e["channel_source"]);
            tuple.addArrayElement(e["channel_destination"]);
            const auto order_key = encode_(tuple);
            if (i && !less_bytes(previous, order_key))
                return false;
            previous = order_key;
            if (!actual.emplace(source, destination, channel).second ||
                !inbound_.emplace(std::pair{destination, channel}, e).second ||
                !ordinals[destination].insert(static_cast<unsigned>(u(e["gather_ordinal"]))).second)
                return false;
            ++readers_[{source, channel}];
        }
        if (actual != expected)
            return false;
        for (const auto& [name, node] : nodes_) {
            for (const auto& channel : channels_)
                if (node.inputs && !inbound_.contains({name, channel}))
                    return false;
            if (!node.output && !readers_.contains({name, channels_.front()}))
                return false;
        }
        for (const auto& [name, values] : ordinals) {
            unsigned ordinal = 0;
            for (auto value : values)
                if (value != ordinal++)
                    return false;
        }
        // Assignment permission and final read-only relaxation are separate facts.
        // Every connected destination here is Gain or AudioOutput: the executor
        // uses read-only views even for fan-out. Zero would erase a real edge,
        // and Copy would misdescribe this selected backend's source table.
        for (unsigned i = 0; i < list.size(); ++i) {
            const auto e = list[i];
            const auto source = s(e["source"]["node"][0]), channel = s(e["channel_source"]);
            const bool permission =
                readers_.at({source, channel}) == 1 && !nodes_.at(source).persistent;
            if (!boolean(e["assignment_allowed"], permission) ||
                !text(e["final_input_mode"], "in_place"))
                return false;
        }
        return true;
    }

    bool admission() {
        const auto limits = p_["admission_limits"];
        if (!shape(limits, {"profile", "host", "runtime", "effective"}))
            return false;
        for (auto layer : {"profile", "runtime", "effective"})
            if (!shape(limits[layer],
                       {"nodes", "connections", "ports_per_node", "ports", "block_frames"}))
                return false;
        if (!shape(limits["host"],
                   {"nodes", "connections", "ports", "block_frames", "max_estimated_work_units"}) ||
            !unsigned_value(limits["host"]["max_estimated_work_units"]))
            return false;
        for (const auto& [field, fixed] :
             std::initializer_list<std::pair<std::string_view, std::uint64_t>>{
                 {"nodes", 512},
                 {"connections", 2048},
                 {"ports_per_node", 64},
                 {"ports", 4096},
                 {"block_frames", 4096}}) {
            if (!unsigned_value(limits["profile"][field]) || u(limits["profile"][field]) != fixed)
                return false;
            auto minimum = fixed;
            for (auto layer : {"host", "runtime"}) {
                if (std::string_view(layer) == "host" && field == "ports_per_node")
                    continue;
                const auto bound = limits[layer][field];
                if (!bound.isVoid()) {
                    if (!unsigned_value(bound) || u(bound) == 0)
                        return false;
                    minimum = std::min(minimum, u(bound));
                }
            }
            if (!unsigned_value(limits["effective"][field]) ||
                u(limits["effective"][field]) != minimum)
                return false;
        }
        if (nodes_.size() > u(limits["effective"]["nodes"]) ||
            p_["edges"].size() > u(limits["effective"]["connections"]) ||
            ports_ > u(limits["effective"]["ports"]) ||
            frames_ > u(limits["effective"]["block_frames"]))
            return false;
        for (const auto& [_, node] : nodes_)
            if (node.inputs > u(limits["effective"]["ports_per_node"]) ||
                node.outputs > u(limits["effective"]["ports_per_node"]))
                return false;
        // SignalGraph::estimate_generated_graph_work_units has no modulation
        // terms in this closed audio-only profile. These bounded operands cannot overflow.
        const auto work = nodes_.size() * std::uint64_t{16} +
                          p_["edges"].size() * std::uint64_t{8} + ports_ * frames_;
        const auto work_limit = u(limits["host"]["max_estimated_work_units"]);
        if (work_limit != 0 && work > work_limit)
            return false;
        return true;
    }

    bool alias_policy() {
        const auto a = p_["host_input_alias_policy"];
        if (!shape(a, {"eligible", "mode", "fallback", "source", "view", "mappings",
                       "requires_full_width", "requires_disjoint"}) ||
            !a["eligible"].isBool() || !a["mappings"].isArray() ||
            !text(a["fallback"], "internal_copy") || !boolean(a["requires_full_width"], true) ||
            !boolean(a["requires_disjoint"], true))
            return false;
        const bool eligible = !nodes_.empty();
        if (!boolean(a["eligible"], eligible) ||
            !text(a["mode"], eligible ? "conditional_host_input" : "internal_copy"))
            return false;
        if (!eligible)
            return a["source"].isVoid() && a["view"].isVoid() && a["mappings"].size() == 0;
        std::string name;
        if (!endpoint(a["source"], true, false, &name) || name != input_node_ ||
            encode_(a["source"]) != encode_(a["view"]) || a["mappings"].size() != 2)
            return false;
        for (unsigned i = 0; i < 2; ++i) {
            const auto mapping = a["mappings"][i];
            std::string channel;
            if (!shape(mapping, {"source", "view"}) ||
                !endpoint(mapping["source"], true, true, &name, &channel) || name != input_node_ ||
                channel != channels_[i] || encode_(mapping["source"]) != encode_(mapping["view"]))
                return false;
        }
        return true;
    }

    bool access(V value, Access& out) const {
        if (!shape(value,
                   {"node", "port", "direction", "channel", "role", "phase", "ordinal", "mode"}) ||
            !key_path(value["node"]) || !text(value["port"], "audio") ||
            !(text(value["direction"], "in") || text(value["direction"], "out")) ||
            !value["channel"].isString() || !value["role"].isString() ||
            !known_phase(value["phase"]) || !value["mode"].isString() ||
            !unsigned_value(value["ordinal"], kU32))
            return false;
        out = {s(value["node"][0]),
               s(value["channel"]),
               s(value["role"]),
               s(value["phase"]),
               s(value["mode"]),
               text(value["direction"], "out"),
               static_cast<unsigned>(u(value["ordinal"]))};
        const auto node = nodes_.find(out.node);
        return node != nodes_.end() &&
               (out.output ? node->second.outputs : node->second.inputs) == 2 &&
               out.ordinal < channels_.size() && out.channel == channels_[out.ordinal];
    }
    void read(ExpectedValue& value, Access access_value) {
        const auto position = nodes_.at(access_value.node).position;
        value.end = std::max(value.end, position + 1);
        const auto time = std::pair{position, phase_index(access_value.phase)};
        if (!value.last_read || *value.last_read < time)
            value.last_read = time;
        value.accesses.insert(std::move(access_value));
    }
    void expected_storage() {
        std::vector<const Node*> order(nodes_.size());
        for (const auto& [_, node] : nodes_)
            order[node.position] = &node;
        for (const auto* node : order) {
            const auto t = node->position;
            for (unsigned c = 0; c < node->inputs; ++c) {
                const auto& channel = channels_[c];
                const auto edge = inbound_.at({node->name, channel});
                ValueKey key{s(edge["source"]["node"][0]), channel, "execute", true, c};
                auto& incoming = expected_values_.at(key);
                if (text(edge["final_input_mode"], "in_place")) {
                    read(incoming,
                         {node->name, channel, "input_view", "execute", "in_place_read", false, c});
                } else {
                    read(incoming, {node->name, channel, "input_view", "gather", "read", false, c});
                    key = {node->name, channel, "gather", false, c};
                    ExpectedValue scratch;
                    scratch.begin = t;
                    scratch.end = t + 1;
                    scratch.accesses.insert(
                        {node->name, channel, "input_scratch", "gather", "copy", false, c});
                    read(scratch,
                         {node->name, channel, "input_scratch", "execute", "read", false, c});
                    expected_values_.emplace(key, std::move(scratch));
                }
                if (node->output) {
                    read(expected_values_.at(key),
                         {node->name, channel, "external_output", "output_copy", "read", false, c});
                    ExpectedValue external;
                    external.begin = t;
                    external.end = t + 1;
                    external.external_output = true;
                    external.accesses.insert(
                        {node->name, channel, "external_output", "output_copy", "write", false, c});
                    expected_values_.emplace(ValueKey{node->name, channel, "output_copy", false, c},
                                             std::move(external));
                }
            }
            for (unsigned c = 0; c < node->outputs; ++c) {
                ExpectedValue value;
                value.begin = t;
                value.end = t + 1;
                value.accesses.insert({node->name, channels_[c],
                                       node->input ? "external_input" : "node_output", "execute",
                                       node->input ? "copy" : "write", true, c});
                expected_values_.emplace(ValueKey{node->name, channels_[c], "execute", true, c},
                                         std::move(value));
            }
        }
    }

    bool storage() {
        const auto buffers = p_["buffers"];
        if (!shape(buffers, {"classes", "values"}) || !buffers["classes"].isArray() ||
            !buffers["values"].isArray())
            return false;
        expected_storage();
        const auto classes = buffers["classes"], values = buffers["values"];
        if (values.size() != expected_values_.size() || classes.size() > values.size())
            return false;
        std::map<std::uint64_t, V> class_records;
        std::map<std::string, std::uint64_t> canonical_access_classes;
        std::map<std::uint64_t, std::set<Access>> members, used_members;
        for (unsigned i = 0; i < classes.size(); ++i) {
            const auto c = classes[i];
            if (!shape(c, {"id", "storage_kind", "capacity_frames", "sample_format",
                           "initialization", "members"}) ||
                !unsigned_value(c["id"], kU32) || u(c["id"]) != i ||
                !unsigned_value(c["capacity_frames"]) || u(c["capacity_frames"]) < frames_ ||
                !text(c["sample_format"], "f32") ||
                !(text(c["storage_kind"], "scratch") ||
                  text(c["storage_kind"], "external_output")) ||
                !c["initialization"].isString() || !c["members"].isArray() ||
                c["members"].size() == 0 || c["members"].size() > 16384)
                return false;
            class_records.emplace(i, c);
            std::string previous;
            for (unsigned j = 0; j < c["members"].size(); ++j) {
                Access a;
                const auto encoded = encode_(c["members"][j]);
                if (!access(c["members"][j], a) || (j && !less_bytes(previous, encoded)) ||
                    !members[i].insert(a).second ||
                    !canonical_access_classes.emplace(encoded, i).second)
                    return false;
                previous = encoded;
            }
        }
        // Normalize by first logical access, not physical slot value or class order.
        std::vector<std::pair<std::string, std::uint64_t>> sorted(canonical_access_classes.begin(),
                                                                  canonical_access_classes.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return less_bytes(a.first, b.first); });
        std::set<std::uint64_t> encountered;
        for (const auto& [_, id] : sorted)
            if (encountered.insert(id).second && id != encountered.size() - 1)
                return false;
        std::set<ValueKey> seen;
        std::map<std::uint64_t, std::vector<std::pair<unsigned, unsigned>>> intervals;
        std::string previous_key;
        for (unsigned i = 0; i < values.size(); ++i) {
            const auto value = values[i];
            if (!shape(value, {"key", "class_id", "accesses", "lifetime_begin", "lifetime_end",
                               "last_read_phase"}) ||
                !shape(value["key"], {"producer", "production_phase", "production_ordinal"}) ||
                !unsigned_value(value["class_id"], kU32) ||
                !class_records.contains(u(value["class_id"])) ||
                !unsigned_value(value["lifetime_begin"], kU32) ||
                !unsigned_value(value["lifetime_end"], kU32) ||
                !known_phase(value["key"]["production_phase"]) ||
                !unsigned_value(value["key"]["production_ordinal"], 1) ||
                !value["accesses"].isArray())
                return false;
            const auto encoded_key = encode_(value["key"]);
            if (i && !less_bytes(previous_key, encoded_key))
                return false;
            previous_key = encoded_key;
            const auto producer = value["key"]["producer"];
            const bool output = text(producer["direction"], "out");
            std::string name, channel;
            if (!endpoint(producer, output, true, &name, &channel))
                return false;
            ValueKey key{name, channel, s(value["key"]["production_phase"]), output,
                         static_cast<unsigned>(u(value["key"]["production_ordinal"]))};
            const auto found = expected_values_.find(key);
            if (found == expected_values_.end() || !seen.insert(key).second)
                return false;
            auto& expected = found->second;
            if (u(value["lifetime_begin"]) != expected.begin ||
                u(value["lifetime_end"]) != expected.end ||
                value["accesses"].size() != expected.accesses.size())
                return false;
            if (expected.last_read) {
                const auto phase = expected.last_read->second;
                if (!text(value["last_read_phase"], phase == 0   ? "gather"
                                                    : phase == 1 ? "execute"
                                                                 : "output_copy"))
                    return false;
            } else if (!value["last_read_phase"].isVoid())
                return false;
            const auto id = u(value["class_id"]);
            const auto klass = class_records.at(id);
            if (!text(klass["storage_kind"],
                      expected.external_output ? "external_output" : "scratch"))
                return false;
            const bool initialization_ok =
                expected.external_output ? text(klass["initialization"], "external_contract")
                                         : (text(klass["initialization"], "prepare_zero") ||
                                            text(klass["initialization"], "overwrite_before_read"));
            if (!initialization_ok)
                return false;
            std::set<Access> actual;
            std::string previous;
            for (unsigned j = 0; j < value["accesses"].size(); ++j) {
                Access a;
                const auto encoded = encode_(value["accesses"][j]);
                if (!access(value["accesses"][j], a) || (j && !less_bytes(previous, encoded)) ||
                    !actual.insert(a).second || !used_members[id].insert(a).second)
                    return false;
                previous = encoded;
            }
            if (actual != expected.accesses)
                return false;
            intervals[id].emplace_back(expected.begin, expected.end);
            expected.class_id = id;
        }
        if (members != used_members)
            return false;
        for (auto& [_, ranges] : intervals) {
            std::sort(ranges.begin(), ranges.end());
            for (std::size_t i = 1; i < ranges.size(); ++i)
                if (ranges[i].first < ranges[i - 1].second)
                    return false;
        }
        return true;
    }

    V p_, authored_;
    Encoder encode_;
    std::uint64_t frames_ = 0, ports_ = 0;
    std::vector<std::string> channels_;
    std::map<std::string, Node> nodes_;
    std::string input_node_;
    std::map<std::pair<std::string, std::string>, V> inbound_;
    std::map<std::pair<std::string, std::string>, unsigned> readers_;
    std::map<ValueKey, ExpectedValue> expected_values_;
};
} // namespace

bool validate_resolved_identity_projection(V projection, std::string_view authored_payload,
                                           std::string_view authored_digest, Encoder encoder) {
    if (!encoder || !validate_resolved_semantic_payload(authored_payload) ||
        !shape(projection,
               {"schema", "semantic", "preparation", "execution", "channels", "interfaces", "nodes",
                "edges", "schedule", "buffers", "host_input_alias_policy", "total_latency_samples",
                "admission_limits"}) ||
        !text(projection["schema"], kResolvedProfile) ||
        !shape(projection["semantic"], {"domain", "profile", "digest"}) ||
        !text(projection["semantic"]["domain"], kSemanticDomain) ||
        !text(projection["semantic"]["profile"], kSemanticProfile) ||
        !text(projection["semantic"]["digest"], authored_digest))
        return false;
    std::string hash_input(kSemanticDomain);
    hash_input += '\0';
    hash_input += authored_payload;
    if (authored_digest != "sha256:" + runtime::sha256_hex(hash_input))
        return false;
    const auto authored = choc::json::parse(std::string(authored_payload));
    return Validator(projection, authored, encoder).run();
}

bool validate_resolved_identity_capture(const PreparedDescriptionData& d) {
    // These host fields are hard caps, unlike the zero-disabled work budget.
    // The new profile has positive effective bounds and cannot encode a real
    // zero cap as an unbounded null. Keep that live-graph case explicitly outside it.
    if (d.host_max_nodes == 0 || d.host_max_connections == 0 || d.host_max_ports == 0)
        return false;
    if (d.schema != "pulp.graph.prepared-io-gain.v1" || !d.typed_resolved_capture ||
        !d.serial_pool_bound || !std::isfinite(d.sample_rate) || d.sample_rate <= 0 ||
        d.max_block_frames == 0 || d.max_block_frames > 4096 ||
        d.semantic_profile != kSemanticProfile || d.pool_max_frames < d.max_block_frames ||
        d.pool_slot_count < d.buffer_slot_count ||
        d.runtime_max_block_frames != d.pool_max_frames || d.nodes.size() > 512 ||
        d.edges.size() > 2048 || d.prepared_plan_node_count != d.nodes.size() ||
        d.prepared_plan_connection_count != d.edges.size() ||
        d.resolved_node_bindings.size() != d.nodes.size() ||
        d.resolved_edge_bindings.size() != d.edges.size() || d.schedule.size() != d.nodes.size() ||
        d.resolved_buffer_classes.size() > 4098 || d.resolved_buffer_values.size() > 4098)
        return false;
    std::set<std::string> nodes, bindings;
    for (const auto& node : d.nodes)
        if (!nodes.insert(node.authored_key).second)
            return false;
    for (const auto& binding : d.resolved_node_bindings)
        if (!nodes.contains(binding.node) || !bindings.insert(binding.node).second)
            return false;
    for (std::size_t i = 0; i < d.edges.size(); ++i) {
        const auto& edge = d.edges[i];
        const auto& binding = d.resolved_edge_bindings[i];
        if (!nodes.contains(binding.source.node) || !nodes.contains(binding.destination.node) ||
            !binding.source.output || binding.destination.output ||
            edge.source_port >= d.channels.size() || edge.destination_port >= d.channels.size() ||
            binding.channel_source != d.channels[edge.source_port] ||
            binding.channel_destination != d.channels[edge.destination_port])
            return false;
    }
    std::set<std::uint32_t> classes;
    for (const auto& klass : d.resolved_buffer_classes)
        if (!classes.insert(klass.id).second || klass.members.empty() ||
            klass.members.size() > 16384 || klass.capacity_frames < d.max_block_frames)
            return false;
    if (d.host_input_alias_eligible != d.resolved_alias_endpoint.has_value())
        return false;
    return true;
}
} // namespace detail
} // namespace pulp::host
