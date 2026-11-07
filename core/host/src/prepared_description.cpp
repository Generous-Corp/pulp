#include <pulp/host/prepared_description.hpp>

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
