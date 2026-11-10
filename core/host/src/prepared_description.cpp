#include <pulp/host/prepared_description.hpp>
#include "resolved_identity_validation.hpp"

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <tuple>
#include <vector>

namespace pulp::host {
namespace {

void quote(std::string& out, std::string_view value) {
    out.push_back('"');
    for (const unsigned char c : value) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
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
            if (c < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0xf]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

void number(std::string& out, std::uint64_t value) {
    char buffer[32];
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    out.append(buffer, result.ptr);
}

void float_bits(std::string& out, float value) {
    out += "\"f32:";
    constexpr char hex[] = "0123456789abcdef";
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (int shift = 28; shift >= 0; shift -= 4)
        out.push_back(hex[(bits >> shift) & 0xf]);
    out.push_back('"');
}

} // namespace

std::string PreparedDescriptionData::canonical_bytes() const {
    std::string out;
    out.reserve(1024);
    out += "{\"schema\":";
    quote(out, schema);
    out += ",\"preparation_contract\":";
    quote(out, preparation_contract);
    out += ",\"semantic_profile\":";
    quote(out, semantic_profile);
    out += ",\"semantic_payload\":";
    quote(out, semantic_payload);
    out += ",\"semantic_digest\":";
    quote(out, semantic_digest);
    out += ",\"sample_rate_bits\":";
    out += "\"f64:";
    const auto sr = std::bit_cast<std::uint64_t>(sample_rate);
    constexpr char hex[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4)
        out.push_back(hex[(sr >> shift) & 0xf]);
    out.push_back('"');
    out += ",\"max_block_frames\":";
    number(out, max_block_frames);
    out += ",\"sample_precision\":";
    quote(out, sample_precision);
    out += ",\"execution\":{\"backend\":";
    quote(out, execution_backend);
    out += ",\"pdc_domain\":";
    quote(out, pdc_execution_domain);
    out += ",\"executor_contract\":";
    quote(out, executor_contract);
    out += ",\"execution_mode\":";
    quote(out, execution_mode);
    out += ",\"tie_break_contract\":";
    quote(out, tie_break_contract);
    out += ",\"fallback_policy\":";
    quote(out, execution_fallback_policy);
    out += ",\"canonical_executor_routing_enabled\":";
    out += canonical_executor_routing_enabled ? "true" : "false";
    out += ",\"parallel_routing_enabled\":";
    out += parallel_routing_enabled ? "true" : "false";
    out += ",\"anticipation_enabled\":";
    out += anticipation_enabled ? "true" : "false";
    out += ",\"plan_node_count\":";
    number(out, prepared_plan_node_count);
    out += ",\"plan_connection_count\":";
    number(out, prepared_plan_connection_count);
    out += ",\"serial_pool_bound\":";
    out += serial_pool_bound ? "true" : "false";
    out += ",\"buffer_slot_count\":";
    number(out, buffer_slot_count);
    out += ",\"pool_slot_count\":";
    number(out, pool_slot_count);
    out += ",\"pool_max_frames\":";
    number(out, pool_max_frames);
    out += ",\"total_latency_samples\":";
    number(out, total_latency_samples);
    out += ",\"input_modes\":[";
    for (std::size_t i = 0; i < input_modes.size(); ++i) {
        if (i)
            out.push_back(',');
        quote(out, input_modes[i]);
    }
    out += "],\"connection_delay_samples\":[";
    for (std::size_t i = 0; i < connection_delay_samples.size(); ++i) {
        if (i)
            out.push_back(',');
        number(out, connection_delay_samples[i]);
    }
    out += "],\"reads_in_place\":[";
    for (std::size_t i = 0; i < reads_in_place.size(); ++i) {
        if (i)
            out.push_back(',');
        number(out, reads_in_place[i]);
    }
    out += "]";
    out += ",\"host_input_alias_policy\":{\"eligible\":";
    out += host_input_alias_eligible ? "true" : "false";
    out += ",\"mode\":";
    quote(out, host_input_alias_mode);
    out += ",\"fallback\":";
    quote(out, host_input_alias_fallback);
    out += ",\"source\":";
    quote(out, host_input_alias_source);
    out += ",\"view\":";
    quote(out, host_input_alias_view);
    out += ",\"mappings\":[";
    for (std::size_t i = 0; i < host_input_alias_mappings.size(); ++i) {
        if (i)
            out.push_back(',');
        quote(out, host_input_alias_mappings[i]);
    }
    out += "]";
    out += ",\"requires_full_width\":";
    out += host_input_alias_requires_full_width ? "true" : "false";
    out += ",\"requires_disjoint\":";
    out += host_input_alias_requires_disjoint ? "true" : "false";
    out += "},\"execution_selection_generation\":";
    number(out, execution_selection_generation);
    out += "}";
    out += ",\"limits\":{\"profile\":{\"nodes\":";
    number(out, profile_max_nodes);
    out += ",\"connections\":";
    number(out, profile_max_connections);
    out += ",\"ports_per_node\":";
    number(out, profile_max_ports_per_node);
    out += ",\"ports\":";
    number(out, profile_max_ports);
    out += ",\"block_frames\":";
    number(out, profile_max_block_frames);
    out += "},\"host\":{\"nodes\":";
    number(out, host_max_nodes);
    out += ",\"connections\":";
    number(out, host_max_connections);
    out += ",\"ports\":";
    number(out, host_max_ports);
    out += ",\"block_frames\":";
    number(out, host_max_block_frames);
    out += ",\"max_estimated_work_units\":";
    number(out, host_max_estimated_work_units);
    out += "},\"runtime\":{\"nodes\":";
    number(out, runtime_max_nodes);
    out += ",\"connections\":";
    number(out, runtime_max_connections);
    out += ",\"ports_per_node\":";
    number(out, runtime_max_ports_per_node);
    out += ",\"ports\":";
    number(out, runtime_max_ports);
    out += ",\"block_frames\":";
    number(out, runtime_max_block_frames);
    out += "},\"effective\":{\"nodes\":";
    number(out, effective_max_nodes);
    out += ",\"connections\":";
    number(out, effective_max_connections);
    out += ",\"ports_per_node\":";
    number(out, effective_max_ports_per_node);
    out += ",\"ports\":";
    number(out, effective_max_ports);
    out += ",\"block_frames\":";
    number(out, effective_max_block_frames);
    out += "}},\"channels\":[";
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (i)
            out.push_back(',');
        quote(out, channels[i]);
    }
    out += "],\"interfaces\":[";
    for (std::size_t i = 0; i < interfaces.size(); ++i) {
        if (i)
            out.push_back(',');
        const auto& iface = interfaces[i];
        out += "{\"key\":";
        quote(out, iface.key);
        out += ",\"role\":";
        quote(out, iface.output ? "output" : "input");
        out += ",\"kind\":\"audio\",\"endpoint\":{\"node\":";
        quote(out, iface.node);
        out += ",\"direction\":";
        quote(out, iface.output ? "in" : "out");
        out += ",\"kind\":\"audio\",\"port\":";
        quote(out, iface.port);
        out += "},\"channels\":[";
        for (std::size_t c = 0; c < iface.channels.size(); ++c) {
            if (c)
                out.push_back(',');
            quote(out, iface.channels[c]);
        }
        out += "],\"runtime_bus_index\":";
        number(out, iface.runtime_bus_index);
        out += ",\"runtime_channel_indices\":[";
        for (std::size_t c = 0; c < iface.runtime_channel_indices.size(); ++c) {
            if (c)
                out.push_back(',');
            number(out, iface.runtime_channel_indices[c]);
        }
        out += "]}";
    }
    out += "],\"nodes\":[";
    // Nodes are a logical-key map in the D4 record. Keep the in-memory vector
    // in authoring/plan order for diagnostics, but canonical bytes must not
    // depend on that incidental order.
    std::vector<std::size_t> node_order(nodes.size());
    for (std::size_t i = 0; i < node_order.size(); ++i)
        node_order[i] = i;
    std::sort(node_order.begin(), node_order.end(), [&](std::size_t lhs, std::size_t rhs) {
        return nodes[lhs].authored_key < nodes[rhs].authored_key;
    });
    for (std::size_t ordered = 0; ordered < node_order.size(); ++ordered) {
        if (ordered)
            out.push_back(',');
        const auto& node = nodes[node_order[ordered]];
        out += "{\"key\":";
        quote(out, node.authored_key);
        out += ",\"kind\":";
        quote(out, node.kind);
        out += ",\"kind_contract\":";
        quote(out, node.kind_contract);
        out += ",\"implementation\":";
        quote(out, node.implementation);
        out += ",\"input_ports\":";
        number(out, node.input_ports);
        out += ",\"output_ports\":";
        number(out, node.output_ports);
        out += ",\"gain\":";
        float_bits(out, std::bit_cast<float>(node.gain_bits));
        out += ",\"latency_samples\":";
        number(out, node.latency_samples);
        out += ",\"tail_samples\":";
        number(out, node.tail_samples);
        out += ",\"transport_sensitive\":";
        out += node.transport_sensitive ? "true" : "false";
        out += ",\"persistent_output\":";
        out += node.persistent_output ? "true" : "false";
        out += ",\"reads_inputs_only\":";
        out += node.reads_inputs_only ? "true" : "false";
        out.push_back('}');
    }
    out += "],\"edges\":[";
    std::vector<std::size_t> edge_order(edges.size());
    for (std::size_t i = 0; i < edge_order.size(); ++i)
        edge_order[i] = i;
    std::sort(edge_order.begin(), edge_order.end(), [&](std::size_t lhs, std::size_t rhs) {
        const auto& left = edges[lhs];
        const auto& right = edges[rhs];
        return std::tie(left.source, left.destination, left.source_port, left.destination_port) <
               std::tie(right.source, right.destination, right.source_port, right.destination_port);
    });
    for (std::size_t ordered = 0; ordered < edge_order.size(); ++ordered) {
        if (ordered)
            out.push_back(',');
        const auto& edge = edges[edge_order[ordered]];
        out += "{\"source\":";
        quote(out, edge.source);
        out += ",\"destination\":";
        quote(out, edge.destination);
        out += ",\"source_port\":";
        number(out, edge.source_port);
        out += ",\"destination_port\":";
        number(out, edge.destination_port);
        out += ",\"input_mode\":";
        quote(out, edge.input_mode);
        out += ",\"gather_ordinal\":";
        number(out, edge.gather_ordinal);
        out += ",\"pdc_delay_samples\":";
        number(out, edge.pdc_delay_samples);
        out += ",\"feedback\":";
        out += edge.feedback ? "true" : "false";
        out += ",\"assignment_allowed\":";
        out += edge.assignment_allowed ? "true" : "false";
        out += ",\"final_input_mode\":";
        quote(out, edge.final_input_mode);
        out.push_back('}');
    }
    out += "],\"schedule\":[";
    for (std::size_t i = 0; i < schedule.size(); ++i) {
        if (i)
            out.push_back(',');
        quote(out, schedule[i]);
    }
    out += "],\"buffers\":{\"classes\":[";
    for (std::size_t i = 0; i < buffer_classes.size(); ++i) {
        if (i)
            out.push_back(',');
        quote(out, buffer_classes[i]);
    }
    out += "],\"values\":[";
    for (std::size_t i = 0; i < buffer_values.size(); ++i) {
        if (i)
            out.push_back(',');
        quote(out, buffer_values[i]);
    }
    out += "]}}";
    return out;
}

std::string PreparedDescriptionData::content_hash() const {
    const auto bytes = canonical_bytes();
    return runtime::sha256_hex(std::string_view(bytes));
}

const char* prepared_description_refusal_name(PreparedDescriptionRefusal code) noexcept {
    switch (code) {
    case PreparedDescriptionRefusal::None:
        return "none";
    case PreparedDescriptionRefusal::Unprepared:
        return "unprepared";
    case PreparedDescriptionRefusal::UnsupportedPreparedProfile:
        return "unsupported_prepared_profile";
    case PreparedDescriptionRefusal::LogicalIdentityMissing:
        return "logical_identity_missing";
    case PreparedDescriptionRefusal::AuthoredBindingMismatch:
        return "authored_binding_mismatch";
    case PreparedDescriptionRefusal::AuthoredBindingStale:
        return "authored_binding_stale";
    case PreparedDescriptionRefusal::ExecutionPathUnavailable:
        return "execution_path_unavailable";
    case PreparedDescriptionRefusal::SnapshotChanged:
        return "snapshot_changed";
    case PreparedDescriptionRefusal::GenerationExhausted:
        return "generation_exhausted";
    case PreparedDescriptionRefusal::AllocationFailed:
        return "allocation_failed";
    }
    return "unknown";
}

} // namespace pulp::host

// The resolved profile is additive: the D4 encoder above is deliberately frozen.
#include <pulp/host/graph_identity.hpp>
#include <pulp/host/identity_text.hpp>
#include <choc/text/choc_JSON.h>
#include <cmath>
#include <map>
#include <set>
#include <limits>

namespace pulp::host {
bool validate_resolved_semantic_payload(std::string_view);

struct ResolvedIdentityAccess {
    static GraphIdentityValue make(std::string domain, std::string profile, std::string payload,
                                   std::string digest) {
        GraphIdentityValue value;
        value.domain_ = std::move(domain);
        value.profile_ = std::move(profile);
        value.payload_ = std::move(payload);
        value.digest_ = std::move(digest);
        return value;
    }
};

namespace {
using J = choc::value::Value;
using V = choc::value::ValueView;
constexpr std::string_view resolved_domain = "pulp.graph.resolved-plan.v1";
constexpr std::string_view resolved_profile = "pulp.graph.resolved.io-gain.v1";
constexpr std::string_view semantic_domain = "pulp.graph.semantic.v1";
constexpr std::string_view semantic_profile = "pulp.graph.identity.v1";

J str(std::string_view s) {
    return J(std::string(s));
}
J num(std::uint64_t n) {
    if (n > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("integer outside bounded profile");
    return J(static_cast<std::int64_t>(n));
}
J arr() {
    return choc::value::createEmptyArray();
}
J obj(std::initializer_list<std::pair<std::string_view, J>> fields) {
    auto result = choc::value::createObject("");
    for (const auto& field : fields)
        result.addMember(std::string(field.first), field.second);
    return result;
}
J boolean(bool value) {
    return J(value);
}
J key(std::string_view node) {
    auto a = arr();
    a.addArrayElement(str(node));
    return a;
}
J endpoint(const ResolvedEndpoint& e) {
    return obj({{"node", key(e.node)},
                {"port", str("audio")},
                {"direction", str(e.output ? "out" : "in")},
                {"kind", str("audio")},
                {"channel", J{}},
                {"parameter", J{}}});
}
J endpoint_channel(const ResolvedEndpoint& e, std::string_view channel) {
    return obj({{"node", key(e.node)},
                {"port", str("audio")},
                {"direction", str(e.output ? "out" : "in")},
                {"kind", str("audio")},
                {"channel", str(channel)},
                {"parameter", J{}}});
}
J access(const ResolvedBufferAccess& a) {
    return obj({{"node", key(a.endpoint.node)},
                {"port", str("audio")},
                {"direction", str(a.endpoint.output ? "out" : "in")},
                {"channel", str(a.channel)},
                {"role", str(a.role)},
                {"phase", str(a.phase)},
                {"ordinal", num(a.ordinal)},
                {"mode", str(a.mode)}});
}
bool unsigned_less(std::string_view a, std::string_view b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](unsigned char x, unsigned char y) { return x < y; });
}
void resolved_quote(std::string& out, std::string_view text) {
    std::string normalized;
    // Canonical JSON permits controls, but this profile's keys/literals do not.
    if (!text.empty() && !normalize_authored_identity_text(text, normalized))
        throw std::invalid_argument("invalid UTF-8 string");
    out += '"';
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += static_cast<char>(c);
    }
    out += '"';
}
void encode(V v, std::string& out) {
    if (v.isVoid())
        out += "null";
    else if (v.isString())
        resolved_quote(out, v.getString());
    else if (v.isBool())
        out += v.getBool() ? "true" : "false";
    else if (v.isInt()) {
        if (v.getInt64() < 0)
            throw std::invalid_argument("negative integer");
        number(out, static_cast<std::uint64_t>(v.getInt64()));
    } else if (v.isArray()) {
        out += '[';
        for (std::uint32_t i = 0; i < v.size(); ++i) {
            if (i)
                out += ',';
            encode(v[i], out);
        }
        out += ']';
    } else if (v.isObject()) {
        std::vector<std::string> names;
        for (std::uint32_t i = 0; i < v.size(); ++i)
            names.emplace_back(v.getObjectMemberAt(i).name);
        std::sort(names.begin(), names.end(), unsigned_less);
        if (std::adjacent_find(names.begin(), names.end()) != names.end())
            throw std::invalid_argument("duplicate JSON key");
        out += '{';
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i)
                out += ',';
            resolved_quote(out, names[i]);
            out += ':';
            encode(v[names[i]], out);
        }
        out += '}';
    } else
        throw std::invalid_argument("noncanonical number");
}
std::string encoded(V v) {
    std::string s;
    encode(v, s);
    return s;
}
std::string hash(std::string_view domain, std::string_view bytes) {
    std::string input(domain);
    input += '\0';
    input += bytes;
    return "sha256:" + runtime::sha256_hex(input);
}
std::string bits(std::uint64_t value, unsigned digits) {
    std::string s = digits == 8 ? "f32:" : "f64:";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned i = digits; i; --i)
        s += hex[(value >> ((i - 1) * 4)) & 15];
    return s;
}
J limit(std::uint64_t nodes, std::uint64_t connections, std::uint64_t per_node, std::uint64_t ports,
        std::uint64_t frames, bool nullable) {
    const auto bound = [nullable](std::uint64_t n) { return nullable && n == 0 ? J{} : num(n); };
    return obj({{"nodes", bound(nodes)},
                {"connections", bound(connections)},
                {"ports_per_node", bound(per_node)},
                {"ports", bound(ports)},
                {"block_frames", bound(frames)}});
}
J projection(const PreparedDescriptionData& d) {
    auto nodes = choc::value::createObject("");
    for (const auto& n : d.nodes) {
        const auto binding =
            std::find_if(d.resolved_node_bindings.begin(), d.resolved_node_bindings.end(),
                         [&](const auto& b) { return b.node == n.authored_key; });
        if (binding == d.resolved_node_bindings.end())
            throw std::invalid_argument("missing binding");
        nodes.addMember(
            encoded(key(n.authored_key)),
            obj({{"kind", str(n.kind)},
                 {"kind_contract", str(n.kind_contract)},
                 {"implementation", str(n.implementation)},
                 {"input_ports", num(n.input_ports)},
                 {"output_ports", num(n.output_ports)},
                 // The legacy detached record keeps zero for I/O gain bits. The
                 // admitted resolved projection carries the normative unity gain
                 // for I/O nodes without changing D4 canonical bytes.
                 {"gain", str(bits(n.kind == "Gain" ? n.gain_bits : 0x3f800000u, 8))},
                 {"latency_samples", num(n.latency_samples)},
                 {"tail_samples", num(n.tail_samples)},
                 {"transport_sensitive", boolean(n.transport_sensitive)},
                 {"persistent_output", boolean(n.persistent_output)},
                 {"required", boolean(binding->required)},
                 {"reads_inputs_only", boolean(n.reads_inputs_only)},
                 {"preserve_parameter_events", boolean(binding->preserve_parameter_events)},
                 {"audio_rate_delivery", str(binding->audio_rate_delivery)}}));
    }
    auto interfaces = arr();
    for (const auto& i : d.interfaces) {
        auto channels = arr();
        for (const auto& c : i.channels)
            channels.addArrayElement(str(c));
        auto indices = arr();
        for (const auto c : i.runtime_channel_indices)
            indices.addArrayElement(num(c));
        interfaces.addArrayElement(obj({{"key", str(i.key)},
                                        {"role", str(i.output ? "output" : "input")},
                                        {"kind", str("audio")},
                                        {"endpoint", endpoint({i.node, !i.output})},
                                        {"channels", channels},
                                        {"bus_index", num(i.runtime_bus_index)},
                                        {"channel_indices", indices}}));
    }
    if (d.edges.size() != d.resolved_edge_bindings.size())
        throw std::invalid_argument("edge count");
    std::vector<J> edge_records;
    for (std::size_t i = 0; i < d.edges.size(); ++i) {
        const auto& e = d.edges[i];
        const auto& b = d.resolved_edge_bindings[i];
        edge_records.push_back(obj({{"source", endpoint(b.source)},
                                    {"destination", endpoint(b.destination)},
                                    {"channel_source", str(b.channel_source)},
                                    {"channel_destination", str(b.channel_destination)},
                                    {"input_mode", str(e.input_mode)},
                                    {"gather_ordinal", num(e.gather_ordinal)},
                                    {"pdc_delay_samples", num(e.pdc_delay_samples)},
                                    {"feedback", str(e.feedback ? "block" : "none")},
                                    {"assignment_allowed", boolean(e.assignment_allowed)},
                                    {"final_input_mode", str(e.final_input_mode)}}));
    }
    std::sort(edge_records.begin(), edge_records.end(), [](const J& a, const J& b) {
        const auto tuple = [](V e) {
            auto v = arr();
            v.addArrayElement(e["source"]);
            v.addArrayElement(e["destination"]);
            v.addArrayElement(e["channel_source"]);
            v.addArrayElement(e["channel_destination"]);
            return encoded(v);
        };
        return unsigned_less(tuple(a), tuple(b));
    });
    auto edges = arr();
    for (const auto& e : edge_records)
        edges.addArrayElement(e);
    auto schedule = arr();
    for (const auto& n : d.schedule)
        schedule.addArrayElement(key(n));
    auto channels = arr();
    for (const auto& c : d.channels)
        channels.addArrayElement(str(c));
    // Normalize only used classes, by first canonical logical access, never raw slot ID.
    std::vector<std::pair<std::string, std::uint32_t>> all_accesses;
    for (const auto& c : d.resolved_buffer_classes)
        for (const auto& a : c.members)
            all_accesses.emplace_back(encoded(access(a)), c.id);
    std::sort(all_accesses.begin(), all_accesses.end(),
              [](const auto& a, const auto& b) { return unsigned_less(a.first, b.first); });
    std::map<std::uint32_t, std::uint32_t> ids;
    for (const auto& a : all_accesses)
        if (!ids.contains(a.second))
            ids.emplace(a.second, ids.size());
    std::vector<J> class_records(ids.size());
    for (const auto& c : d.resolved_buffer_classes) {
        if (!ids.contains(c.id))
            throw std::invalid_argument("unused class");
        std::vector<std::string> sorted;
        for (const auto& a : c.members)
            sorted.push_back(encoded(access(a)));
        std::sort(sorted.begin(), sorted.end(), unsigned_less);
        auto members = arr();
        for (const auto& a : sorted)
            members.addArrayElement(choc::json::parse(a));
        class_records[ids.at(c.id)] = obj({{"id", num(ids.at(c.id))},
                                           {"storage_kind", str(c.storage_kind)},
                                           {"capacity_frames", num(c.capacity_frames)},
                                           {"sample_format", str(c.sample_format)},
                                           {"initialization", str(c.initialization)},
                                           {"members", members}});
    }
    auto classes = arr();
    for (const auto& c : class_records)
        classes.addArrayElement(c);
    std::vector<J> value_records;
    for (const auto& v : d.resolved_buffer_values) {
        std::vector<std::string> sorted;
        for (const auto& a : v.accesses)
            sorted.push_back(encoded(access(a)));
        std::sort(sorted.begin(), sorted.end(), unsigned_less);
        auto accesses = arr();
        for (const auto& a : sorted)
            accesses.addArrayElement(choc::json::parse(a));
        if (!ids.contains(v.class_id))
            throw std::invalid_argument("unbound value");
        // The channel is part of the endpoint's produced mono value identity.
        auto producer = endpoint_channel(v.producer, v.channel);
        value_records.push_back(
            obj({{"key", obj({{"producer", producer},
                              {"production_phase", str(v.production_phase)},
                              {"production_ordinal", num(v.production_ordinal)}})},
                 {"class_id", num(ids.at(v.class_id))},
                 {"accesses", accesses},
                 {"lifetime_begin", num(v.lifetime_begin)},
                 {"lifetime_end", num(v.lifetime_end)},
                 {"last_read_phase", v.last_read_phase ? str(*v.last_read_phase) : J{}}}));
    }
    std::sort(value_records.begin(), value_records.end(), [](const J& a, const J& b) {
        return unsigned_less(encoded(a["key"]), encoded(b["key"]));
    });
    auto values = arr();
    for (const auto& v : value_records)
        values.addArrayElement(v);
    auto mappings = arr();
    if (d.resolved_alias_endpoint)
        for (const auto& c : d.channels) {
            auto e = endpoint_channel(*d.resolved_alias_endpoint, c);
            mappings.addArrayElement(obj({{"source", e}, {"view", e}}));
        }
    auto host = obj({{"nodes", d.host_max_nodes ? num(d.host_max_nodes) : J{}},
                     {"connections", d.host_max_connections ? num(d.host_max_connections) : J{}},
                     {"ports", d.host_max_ports ? num(d.host_max_ports) : J{}},
                     {"block_frames", d.host_max_block_frames ? num(d.host_max_block_frames) : J{}},
                     {"max_estimated_work_units", num(d.host_max_estimated_work_units)}});
    return obj(
        {{"schema", str(resolved_profile)},
         {"semantic", obj({{"domain", str(semantic_domain)},
                           {"profile", str(d.semantic_profile)},
                           {"digest", str(d.semantic_digest)}})},
         {"preparation",
          obj({{"contract", str(d.preparation_contract)},
               {"sample_rate", str(bits(std::bit_cast<std::uint64_t>(d.sample_rate), 16))},
               {"max_block_frames", num(d.max_block_frames)},
               {"sample_precision", str(d.sample_precision)}})},
         {"execution", obj({{"backend", str(d.execution_backend)},
                            {"executor_contract", str(d.executor_contract)},
                            {"mode", str(d.execution_mode)},
                            {"tie_break_contract", str(d.tie_break_contract)},
                            {"fallback_policy", str(d.execution_fallback_policy)},
                            {"pdc_domain", str(d.pdc_execution_domain)},
                            {"canonical", boolean(d.canonical_executor_routing_enabled)},
                            {"parallel", boolean(d.parallel_routing_enabled)},
                            {"anticipation", boolean(d.anticipation_enabled)}})},
         {"channels", channels},
         {"interfaces", interfaces},
         {"nodes", nodes},
         {"edges", edges},
         {"schedule", schedule},
         {"buffers", obj({{"classes", classes}, {"values", values}})},
         {"host_input_alias_policy",
          obj({{"eligible", boolean(d.host_input_alias_eligible)},
               {"mode",
                str(d.host_input_alias_eligible ? "conditional_host_input" : "internal_copy")},
               {"fallback", str(d.host_input_alias_fallback)},
               {"source", d.resolved_alias_endpoint ? endpoint(*d.resolved_alias_endpoint) : J{}},
               {"view", d.resolved_alias_endpoint ? endpoint(*d.resolved_alias_endpoint) : J{}},
               {"mappings", mappings},
               {"requires_full_width", boolean(d.host_input_alias_requires_full_width)},
               {"requires_disjoint", boolean(d.host_input_alias_requires_disjoint)}})},
         {"total_latency_samples", num(d.total_latency_samples)},
         {"admission_limits",
          obj({{"profile",
                limit(d.profile_max_nodes, d.profile_max_connections, d.profile_max_ports_per_node,
                      d.profile_max_ports, d.profile_max_block_frames, false)},
               {"host", host},
               {"runtime",
                limit(d.runtime_max_nodes, d.runtime_max_connections, d.runtime_max_ports_per_node,
                      d.runtime_max_ports, d.runtime_max_block_frames, true)},
               {"effective", limit(d.effective_max_nodes, d.effective_max_connections,
                                   d.effective_max_ports_per_node, d.effective_max_ports,
                                   d.effective_max_block_frames, false)}})}});
}

void require(bool condition) {
    if (!condition)
        throw std::invalid_argument("invalid resolved receipt");
}
bool text(V value, std::string_view expected) {
    return value.isString() && value.getString() == expected;
}
std::string string(V v) {
    require(v.isString());
    return std::string(v.getString());
}
void shape(V v, std::initializer_list<std::string_view> names) {
    require(v.isObject() && v.size() == names.size());
    for (const auto name : names)
        require(v.hasObjectMember(name));
}

} // namespace

GraphIdentityResult resolved_fingerprint(const PreparedDescriptionData& data) {
    try {
        require(detail::validate_resolved_identity_capture(data));
        const auto payload = encoded(projection(data));
        require(detail::validate_resolved_identity_projection(
            choc::json::parse(payload), data.semantic_payload, data.semantic_digest, encoded));
        const auto digest = hash(resolved_domain, payload);
        return GraphIdentityResult(runtime::Ok(ResolvedIdentityAccess::make(
            std::string(resolved_domain), std::string(resolved_profile), payload, digest)));
    } catch (const std::bad_alloc&) {
        return GraphIdentityResult(runtime::Err(GraphIdentityError::AllocationFailed));
    } catch (const std::exception& error) {
        return GraphIdentityResult(runtime::Err(GraphIdentityError::UnsupportedPreparedProfile));
    } catch (...) {
        return GraphIdentityResult(runtime::Err(GraphIdentityError::UnsupportedPreparedProfile));
    }
}

GraphReceiptResult capture_graph_identity_receipt(const PreparedDescriptionData& data) {
    try {
        const auto resolved = resolved_fingerprint(data);
        if (!resolved)
            return GraphReceiptResult(runtime::Err(resolved.error()));
        auto authored =
            ResolvedIdentityAccess::make(std::string(semantic_domain), data.semantic_profile,
                                         data.semantic_payload, data.semantic_digest);
        require(validate_resolved_semantic_payload(data.semantic_payload));
        GraphIdentityReceipt receipt{std::move(authored), resolved.value(),
                                     "capture:snapshot=" + std::to_string(data.snapshot_id),
                                     "pulp.graph.identity.receipt.v1"};
        return GraphReceiptResult(runtime::Ok(std::move(receipt)));
    } catch (const std::bad_alloc&) {
        return GraphReceiptResult(runtime::Err(GraphIdentityError::AllocationFailed));
    } catch (...) {
        return GraphReceiptResult(runtime::Err(GraphIdentityError::ReceiptBindingInvalid));
    }
}

std::string GraphIdentityReceipt::canonical_bytes() const {
    auto root = obj({{"schema", str("pulp.graph.identity.receipt.v1")},
                     {"authored", obj({{"domain", str(authored.domain_)},
                                       {"profile", str(authored.profile_)},
                                       {"payload", choc::json::parse(authored.payload_)},
                                       {"digest", str(authored.digest_)},
                                       {"state_blobs", obj({})}})},
                     {"resolved", obj({{"domain", str(resolved.domain_)},
                                       {"profile", str(resolved.profile_)},
                                       {"payload", choc::json::parse(resolved.payload_)},
                                       {"digest", str(resolved.digest_)}})},
                     {"capture_provenance", str(capture_provenance)},
                     {"artifact_provenance", str(artifact_provenance)}});
    return encoded(root);
}

GraphReceiptResult decode_graph_identity_receipt(std::span<const std::byte> bytes) {
    try {
        require(bytes.size() <= 64 * 1024 * 1024);
        std::string source(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        auto root = choc::json::parse(source);
        shape(root,
              {"schema", "authored", "resolved", "capture_provenance", "artifact_provenance"});
        require(text(root["schema"], "pulp.graph.identity.receipt.v1"));
        const auto read = [](V v, bool authored_record) {
            if (authored_record) {
                shape(v, {"domain", "profile", "payload", "digest", "state_blobs"});
                require(v["state_blobs"].isObject() && v["state_blobs"].size() == 0);
            } else {
                shape(v, {"domain", "profile", "payload", "digest"});
            }
            require(v["payload"].isObject());
            auto payload = encoded(v["payload"]);
            auto domain = string(v["domain"]);
            auto profile = string(v["profile"]);
            auto digest = string(v["digest"]);
            require(digest == hash(domain, payload));
            return ResolvedIdentityAccess::make(std::move(domain), std::move(profile),
                                                std::move(payload), std::move(digest));
        };
        auto authored = read(root["authored"], true);
        auto resolved = read(root["resolved"], false);
        require(authored.domain() == semantic_domain && resolved.domain() == resolved_domain &&
                resolved.profile() == resolved_profile && authored.profile() == semantic_profile);
        require(detail::validate_resolved_identity_projection(
            choc::json::parse(std::string(resolved.canonical_bytes())),
            std::string(authored.canonical_bytes()), std::string(authored.digest()), encoded));
        GraphIdentityReceipt result{std::move(authored), std::move(resolved),
                                    string(root["capture_provenance"]),
                                    string(root["artifact_provenance"])};
        require(result.canonical_bytes() == source);
        return GraphReceiptResult(runtime::Ok(std::move(result)));
    } catch (const std::bad_alloc&) {
        return GraphReceiptResult(runtime::Err(GraphIdentityError::AllocationFailed));
    } catch (...) {
        return GraphReceiptResult(runtime::Err(GraphIdentityError::ReceiptBindingInvalid));
    }
}

runtime::Result<bool, GraphIdentityError>
compare_graph_identities(const GraphIdentityValue& before, const GraphIdentityValue& after) {
    if (before.domain_ != after.domain_ || before.profile_ != after.profile_)
        return runtime::Result<bool, GraphIdentityError>(
            runtime::Err(GraphIdentityError::IncompatibleProfile));
    if (before.digest_ != hash(before.domain_, before.payload_) ||
        after.digest_ != hash(after.domain_, after.payload_))
        return runtime::Result<bool, GraphIdentityError>(
            runtime::Err(GraphIdentityError::DigestPayloadMismatch));
    return runtime::Result<bool, GraphIdentityError>(
        runtime::Ok(before.payload_ == after.payload_));
}

const char* graph_identity_error_name(GraphIdentityError error) noexcept {
    switch (error) {
    case GraphIdentityError::UnsupportedPreparedProfile:
        return "unsupported_prepared_profile";
    case GraphIdentityError::ReceiptBindingInvalid:
        return "receipt_binding_invalid";
    case GraphIdentityError::DigestPayloadMismatch:
        return "digest_payload_mismatch";
    case GraphIdentityError::IncompatibleProfile:
        return "incompatible_profile";
    case GraphIdentityError::AllocationFailed:
        return "allocation_failed";
    }
    return "unknown";
}

} // namespace pulp::host
