#include <pulp/host/prepared_description.hpp>

#include <pulp/runtime/crypto.hpp>

#include <bit>
#include <charconv>

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
    out += ",\"execution\":{\"backend\":";
    quote(out, execution_backend);
    out += ",\"pdc_domain\":";
    quote(out, pdc_execution_domain);
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
    out += "}";
    out += ",\"limits\":{\"profile\":{\"nodes\":";
    number(out, profile_max_nodes);
    out += ",\"connections\":";
    number(out, profile_max_connections);
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
    out += "},\"effective\":{\"nodes\":";
    number(out, effective_max_nodes);
    out += ",\"connections\":";
    number(out, effective_max_connections);
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
    out += "],\"nodes\":[";
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (i)
            out.push_back(',');
        const auto& node = nodes[i];
        out += "{\"key\":";
        quote(out, node.authored_key);
        out += ",\"kind\":";
        quote(out, node.kind);
        out += ",\"input_ports\":";
        number(out, node.input_ports);
        out += ",\"output_ports\":";
        number(out, node.output_ports);
        out += ",\"gain\":";
        float_bits(out, std::bit_cast<float>(node.gain_bits));
        out.push_back('}');
    }
    out += "],\"edges\":[";
    for (std::size_t i = 0; i < edges.size(); ++i) {
        if (i)
            out.push_back(',');
        const auto& edge = edges[i];
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
        out.push_back('}');
    }
    out += "],\"schedule\":[";
    for (std::size_t i = 0; i < schedule.size(); ++i) {
        if (i)
            out.push_back(',');
        quote(out, schedule[i]);
    }
    out += "]}";
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
