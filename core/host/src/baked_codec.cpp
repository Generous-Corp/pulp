#include <pulp/host/baked_codec.hpp>

#include <cstring>
#include <unordered_map>

namespace pulp::host {
namespace {

// ── little-endian writers ────────────────────────────────────────────────
void put_u8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }
void put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void put_f32(std::vector<std::uint8_t>& out, float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32(out, bits);
}
void put_bytes(std::vector<std::uint8_t>& out, const std::uint8_t* p, std::size_t n) {
    out.insert(out.end(), p, p + n);
}

// ── bounds-checked reader ────────────────────────────────────────────────
// Every read validates `remaining` first; any short read flips `ok` false and all
// subsequent reads no-op, so the parser can check `ok` once at the end.
struct Reader {
    const std::uint8_t* p;
    std::size_t n;
    std::size_t pos = 0;
    bool ok = true;

    bool have(std::size_t k) const { return ok && pos + k <= n; }
    std::uint8_t u8() {
        if (!have(1)) { ok = false; return 0; }
        return p[pos++];
    }
    std::uint16_t u16() {
        if (!have(2)) { ok = false; return 0; }
        std::uint16_t v = static_cast<std::uint16_t>(p[pos]) |
                          static_cast<std::uint16_t>(p[pos + 1] << 8);
        pos += 2;
        return v;
    }
    std::uint32_t u32() {
        if (!have(4)) { ok = false; return 0; }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[pos + i]) << (8 * i);
        pos += 4;
        return v;
    }
    float f32() {
        std::uint32_t bits = u32();
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    // Length-prefixed (u32) byte blob, capped: a length beyond `cap` or beyond the
    // remaining buffer fails the read rather than allocating.
    std::vector<std::uint8_t> blob(std::size_t cap) {
        const std::uint32_t len = u32();
        if (!ok || len > cap || !have(len)) { ok = false; return {}; }
        std::vector<std::uint8_t> v(p + pos, p + pos + len);
        pos += len;
        return v;
    }
    bool at_end() const { return ok && pos == n; }
};

std::uint8_t node_type_code(NodeType t) { return static_cast<std::uint8_t>(t); }

bool node_type_from_code(std::uint8_t code, NodeType& out) {
    // Every enumerator through Custom is valid on the wire; the LOWERABILITY proof
    // (run later on the verified plan) is what rejects Plugin/MIDI — here we only
    // reject a code outside the enum so an out-of-range value can't be cast to UB.
    if (code > static_cast<std::uint8_t>(NodeType::Custom)) return false;
    out = static_cast<NodeType>(code);
    return true;
}

}  // namespace

std::vector<std::uint8_t> serialize_plan(const BakedPlan& plan) {
    std::vector<std::uint8_t> out;
    put_u32(out, static_cast<std::uint32_t>(plan.format_version));
    put_u32(out, static_cast<std::uint32_t>(plan.input_channels));
    put_u32(out, static_cast<std::uint32_t>(plan.output_channels));
    put_u32(out, static_cast<std::uint32_t>(plan.nodes.size()));
    for (const auto& n : plan.nodes) {
        put_u32(out, static_cast<std::uint32_t>(n.id));
        put_u8(out, node_type_code(n.type));
        put_u16(out, static_cast<std::uint16_t>(n.num_input_ports));
        put_u16(out, static_cast<std::uint16_t>(n.num_output_ports));
        put_f32(out, n.gain);
        put_u32(out, static_cast<std::uint32_t>(n.custom_type_id.size()));
        put_bytes(out, reinterpret_cast<const std::uint8_t*>(n.custom_type_id.data()),
                  n.custom_type_id.size());
        put_u32(out, static_cast<std::uint32_t>(n.custom_version));
        put_u32(out, static_cast<std::uint32_t>(n.custom_state.size()));
        put_bytes(out, n.custom_state.data(), n.custom_state.size());
    }
    put_u32(out, static_cast<std::uint32_t>(plan.connections.size()));
    for (const auto& c : plan.connections) {
        put_u32(out, static_cast<std::uint32_t>(c.src_node));
        put_u16(out, static_cast<std::uint16_t>(c.src_port));
        put_u32(out, static_cast<std::uint32_t>(c.dst_node));
        put_u16(out, static_cast<std::uint16_t>(c.dst_port));
        put_u8(out, c.feedback ? 1 : 0);
    }
    return out;
}

std::optional<BakedPlan> parse_plan_bounded(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > kBakedMaxPlanBytes) return std::nullopt;
    Reader r{bytes.data(), bytes.size()};
    BakedPlan plan;

    plan.format_version = static_cast<int>(r.u32());
    if (!r.ok || plan.format_version < 1 || plan.format_version > kBakedPlanFormatVersion) {
        return std::nullopt;  // truncated or unknown/future version
    }
    plan.input_channels = static_cast<int>(r.u32());
    plan.output_channels = static_cast<int>(r.u32());
    if (!r.ok || plan.input_channels < 0 || plan.output_channels < 0 ||
        plan.input_channels > kBakedMaxTotalPorts ||
        plan.output_channels > kBakedMaxTotalPorts) {
        return std::nullopt;
    }

    const std::uint32_t node_count = r.u32();
    if (!r.ok || node_count > kBakedMaxNodes) return std::nullopt;
    plan.nodes.reserve(node_count);
    std::unordered_map<NodeId, BakedPlan::Node*> by_id;
    std::size_t total_ports = 0;
    for (std::uint32_t i = 0; i < node_count; ++i) {
        BakedPlan::Node n;
        n.id = static_cast<NodeId>(r.u32());
        NodeType t{};
        if (!node_type_from_code(r.u8(), t)) return std::nullopt;
        n.type = t;
        n.num_input_ports = static_cast<int>(r.u16());
        n.num_output_ports = static_cast<int>(r.u16());
        n.gain = r.f32();
        auto type_id = r.blob(kBakedMaxCustomTypeId);
        n.custom_type_id.assign(type_id.begin(), type_id.end());
        n.custom_version = static_cast<int>(r.u32());
        n.custom_state = r.blob(kBakedMaxCustomState);
        if (!r.ok) return std::nullopt;
        if (n.num_input_ports > kBakedMaxPortsPerNode ||
            n.num_output_ports > kBakedMaxPortsPerNode) {
            return std::nullopt;
        }
        total_ports += static_cast<std::size_t>(n.num_input_ports) +
                       static_cast<std::size_t>(n.num_output_ports);
        if (total_ports > kBakedMaxTotalPorts) return std::nullopt;
        plan.nodes.push_back(std::move(n));
        by_id[plan.nodes.back().id] = &plan.nodes.back();
    }

    const std::uint32_t conn_count = r.u32();
    if (!r.ok || conn_count > kBakedMaxConnections) return std::nullopt;
    plan.connections.reserve(conn_count);
    for (std::uint32_t i = 0; i < conn_count; ++i) {
        BakedPlan::Conn c;
        c.src_node = static_cast<NodeId>(r.u32());
        c.src_port = static_cast<int>(r.u16());
        c.dst_node = static_cast<NodeId>(r.u32());
        c.dst_port = static_cast<int>(r.u16());
        c.feedback = r.u8() != 0;
        if (!r.ok) return std::nullopt;
        // Port-OOB check against the referenced nodes' declared arity: a connection
        // that indexes a port a node doesn't have would corrupt buffer routing.
        const auto sit = by_id.find(c.src_node);
        const auto dit = by_id.find(c.dst_node);
        if (sit == by_id.end() || dit == by_id.end()) return std::nullopt;
        if (c.src_port < 0 || c.src_port >= sit->second->num_output_ports) return std::nullopt;
        if (c.dst_port < 0 || c.dst_port >= dit->second->num_input_ports) return std::nullopt;
        plan.connections.push_back(c);
    }

    if (!r.at_end()) return std::nullopt;  // trailing garbage
    return plan;
}

}  // namespace pulp::host
