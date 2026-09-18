#include <pulp/host/baked_codec.hpp>
#include <pulp/host/sample_region_proof.hpp>

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

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
void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
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

    bool have(std::size_t k) const {
        return ok && pos <= n && k <= n - pos;
    }
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
    std::uint64_t u64() {
        if (!have(8)) {
            ok = false;
            return 0;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<std::uint64_t>(p[pos + i]) << (8 * i);
        pos += 8;
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
    std::string string(std::size_t cap) {
        const std::uint32_t len = u32();
        if (!ok || len > cap || !have(len)) {
            ok = false;
            return {};
        }
        if (len == 0)
            return {};
        std::string value(reinterpret_cast<const char*>(p + pos), len);
        pos += len;
        return value;
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

bool finite_f32(float value) noexcept {
    return std::isfinite(value);
}

bool positive_zero(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value) == 0u;
}

bool valid_utf8(std::string_view value) noexcept {
    std::size_t i = 0;
    while (i < value.size()) {
        const auto first = static_cast<std::uint8_t>(value[i]);
        if (first <= 0x7F) {
            ++i;
            continue;
        }
        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xC2 && first <= 0xDF) {
            continuation_count = 1;
            code_point = first & 0x1F;
            minimum = 0x80;
        } else if (first >= 0xE0 && first <= 0xEF) {
            continuation_count = 2;
            code_point = first & 0x0F;
            minimum = 0x800;
        } else if (first >= 0xF0 && first <= 0xF4) {
            continuation_count = 3;
            code_point = first & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (i + continuation_count >= value.size())
            return false;
        for (std::size_t j = 1; j <= continuation_count; ++j) {
            const auto byte = static_cast<std::uint8_t>(value[i + j]);
            if ((byte & 0xC0) != 0x80)
                return false;
            code_point = (code_point << 6) | (byte & 0x3F);
        }
        if (code_point < minimum || code_point > 0x10FFFF ||
            (code_point >= 0xD800 && code_point <= 0xDFFF))
            return false;
        i += continuation_count + 1;
    }
    return true;
}

bool canonical_region_order(const SampleRegionDefinition& region) {
    if (!std::is_sorted(region.members.begin(), region.members.end(),
                        [](const auto& a, const auto& b) { return a.node < b.node; }))
        return false;
    for (std::size_t i = 1; i < region.members.size(); ++i)
        if (region.members[i - 1].node == region.members[i].node)
            return false;

    const auto boundary_index = [&](NodeId id) -> std::optional<std::uint32_t> {
        const auto found = std::find_if(region.members.begin(), region.members.end(),
                                        [id](const auto& member) { return member.node == id; });
        return found == region.members.end()
                   ? std::nullopt
                   : std::optional<std::uint32_t>{found->config.boundary_index_or_parameter_id};
    };
    const auto boundary_less = [&](NodeId a, NodeId b) {
        const auto ai = boundary_index(a);
        const auto bi = boundary_index(b);
        if (!ai || !bi)
            return a < b;
        return *ai == *bi ? a < b : *ai < *bi;
    };
    if (!std::is_sorted(region.input_boundaries.begin(), region.input_boundaries.end(),
                        boundary_less) ||
        !std::is_sorted(region.output_boundaries.begin(), region.output_boundaries.end(),
                        boundary_less))
        return false;
    if (std::adjacent_find(region.input_boundaries.begin(), region.input_boundaries.end()) !=
            region.input_boundaries.end() ||
        std::adjacent_find(region.output_boundaries.begin(), region.output_boundaries.end()) !=
            region.output_boundaries.end())
        return false;
    if (!std::is_sorted(region.promoted_parameters.begin(), region.promoted_parameters.end(),
                        [](const auto& a, const auto& b) { return a.param_id < b.param_id; }))
        return false;
    for (std::size_t i = 1; i < region.promoted_parameters.size(); ++i)
        if (region.promoted_parameters[i - 1].param_id == region.promoted_parameters[i].param_id)
            return false;
    return true;
}

bool canonical_config(const SampleKernelConfig& config) noexcept {
    switch (config.kind) {
    case SampleKernelConfigKind::None:
        return config.boundary_index_or_parameter_id == 0 && positive_zero(config.constant);
    case SampleKernelConfigKind::BoundaryIndex:
        return positive_zero(config.constant);
    case SampleKernelConfigKind::FiniteConstant:
        return config.boundary_index_or_parameter_id == 0 && finite_f32(config.constant);
    case SampleKernelConfigKind::PromotedParameterId:
        return config.boundary_index_or_parameter_id != 0 && positive_zero(config.constant);
    case SampleKernelConfigKind::Invalid:
        return false;
    }
    return false;
}

bool canonical_v2_node(const BakedPlan::Node& node) {
    if (node.id == 0 || !finite_f32(node.gain))
        return false;
    if (node.type == NodeType::Custom) {
        return node.gain == 1.0f && !node.custom_type_id.empty() &&
               valid_utf8(node.custom_type_id) && node.custom_version > 0;
    }
    return node.custom_type_id.empty() && node.custom_version == 0 && node.custom_state.empty() &&
           (node.type == NodeType::Gain || node.gain == 1.0f);
}

bool limits_within_v1(const SampleRegionLimits& limits) noexcept {
    const auto cap = SampleRegionLimits::v1();
    return limits.max_member_nodes <= cap.max_member_nodes &&
           limits.max_internal_connections <= cap.max_internal_connections &&
           limits.max_input_boundaries <= cap.max_input_boundaries &&
           limits.max_output_boundaries <= cap.max_output_boundaries &&
           limits.max_delay_nodes <= cap.max_delay_nodes &&
           limits.max_promoted_parameters <= cap.max_promoted_parameters &&
           limits.max_state_bytes <= cap.max_state_bytes &&
           limits.max_logical_boundary_bytes <= cap.max_logical_boundary_bytes &&
           limits.max_work_per_frame <= cap.max_work_per_frame &&
           limits.max_work_per_block <= cap.max_work_per_block;
}

bool valid_wire_region(const SampleRegionDefinition& region) {
    if (region.region_id == 0 || region.members.empty() ||
        region.members.size() > kBakedMaxRegionMembers || region.input_boundaries.empty() ||
        region.input_boundaries.size() > kBakedMaxRegionBoundaries ||
        region.output_boundaries.empty() ||
        region.output_boundaries.size() > kBakedMaxRegionBoundaries ||
        region.promoted_parameters.size() > kBakedMaxRegionParameters ||
        !limits_within_v1(region.limits) || !canonical_region_order(region))
        return false;
    if (region.members.size() > region.limits.max_member_nodes ||
        region.input_boundaries.size() > region.limits.max_input_boundaries ||
        region.output_boundaries.size() > region.limits.max_output_boundaries ||
        region.promoted_parameters.size() > region.limits.max_promoted_parameters)
        return false;

    std::unordered_set<NodeId> members;
    for (const auto& member : region.members) {
        if (member.node == 0 || member.type_id.empty() ||
            member.type_id.size() > kBakedMaxRegionString || !valid_utf8(member.type_id) ||
            member.version <= 0 || !canonical_config(member.config) ||
            !members.insert(member.node).second)
            return false;
    }
    std::unordered_set<NodeId> boundaries;
    for (const auto id : region.input_boundaries)
        if (id == 0 || !members.contains(id) || !boundaries.insert(id).second)
            return false;
    for (const auto id : region.output_boundaries)
        if (id == 0 || !members.contains(id) || !boundaries.insert(id).second)
            return false;

    std::unordered_set<state::ParamID> parameter_ids;
    std::unordered_set<std::string> keys;
    for (const auto& parameter : region.promoted_parameters) {
        if (parameter.param_id == 0 || parameter.key.empty() || parameter.name.empty() ||
            parameter.key.size() > kBakedMaxRegionString ||
            parameter.name.size() > kBakedMaxRegionString ||
            parameter.unit.size() > kBakedMaxRegionString || !valid_utf8(parameter.key) ||
            !valid_utf8(parameter.name) || !valid_utf8(parameter.unit) ||
            !parameter_ids.insert(parameter.param_id).second ||
            !keys.insert(parameter.key).second || parameter.bound_node_id == 0 ||
            !members.contains(parameter.bound_node_id) || parameter.bound_port != 0 ||
            parameter.rate != state::ParamRate::ControlRate ||
            !positive_zero(parameter.smoothing_ramp_seconds) ||
            !finite_f32(parameter.smoothing_ramp_seconds) || !finite_f32(parameter.range.min) ||
            !finite_f32(parameter.range.max) || !finite_f32(parameter.range.default_value) ||
            !finite_f32(parameter.range.step) || !finite_f32(parameter.range.skew) ||
            parameter.range.min > parameter.range.max ||
            parameter.range.default_value < parameter.range.min ||
            parameter.range.default_value > parameter.range.max || parameter.range.step < 0.0f ||
            parameter.range.skew <= 0.0f)
            return false;
    }
    return true;
}

// Validate plan-level invariants that cannot be checked by an individual
// region record. This is deliberately callback-free: the signed codec admits
// only identities and metadata; the loader performs the exact descriptor and
// causal proof after resolving trusted registrations.
bool canonical_region_manifest(const BakedPlan& plan) {
    if (plan.format_version == kBakedPlanV1FormatVersion)
        return plan.sample_regions.empty();
    if (plan.format_version != kBakedMaxSupportedFormatVersion || plan.sample_regions.empty() ||
        plan.sample_regions.size() > kBakedMaxRegions)
        return false;

    std::unordered_map<NodeId, const BakedPlan::Node*> nodes;
    nodes.reserve(plan.nodes.size());
    for (const auto& node : plan.nodes)
        if (!nodes.emplace(node.id, &node).second)
            return false;

    if (!std::is_sorted(plan.sample_regions.begin(), plan.sample_regions.end(),
                        [](const auto& a, const auto& b) { return a.region_id < b.region_id; }))
        return false;
    std::unordered_set<SampleRegionId> region_ids;
    std::unordered_set<NodeId> all_members;
    std::unordered_set<state::ParamID> all_parameters;
    std::size_t member_total = 0;
    std::size_t input_total = 0;
    std::size_t output_total = 0;
    std::size_t parameter_total = 0;
    for (const auto& region : plan.sample_regions) {
        if (!region_ids.insert(region.region_id).second || !valid_wire_region(region))
            return false;
        member_total += region.members.size();
        input_total += region.input_boundaries.size();
        output_total += region.output_boundaries.size();
        parameter_total += region.promoted_parameters.size();
        if (member_total > kBakedMaxNodes || input_total > 128 || output_total > 128 ||
            parameter_total > 256)
            return false;
        for (const auto& member : region.members) {
            const auto node = nodes.find(member.node);
            if (node == nodes.end() || node->second->type != NodeType::Custom ||
                node->second->custom_type_id != member.type_id ||
                node->second->custom_version != member.version ||
                !all_members.insert(member.node).second)
                return false;
        }
        for (const auto id : region.input_boundaries)
            if (std::find_if(region.members.begin(), region.members.end(),
                             [id](const auto& member) { return member.node == id; }) ==
                region.members.end())
                return false;
        for (const auto id : region.output_boundaries)
            if (std::find_if(region.members.begin(), region.members.end(),
                             [id](const auto& member) { return member.node == id; }) ==
                region.members.end())
                return false;
        for (const auto& parameter : region.promoted_parameters)
            if (!all_parameters.insert(parameter.param_id).second)
                return false;
    }
    return true;
}

bool parser_shape_within_limits(const BakedPlan& plan, std::size_t available_bytes) {
    SampleRegionParserShape shape{};
    shape.regions = plan.sample_regions.size();
    shape.connections_total = plan.connections.size();
    shape.available_bytes = available_bytes;
    const auto add = [&](std::uint64_t value, std::uint64_t& total) {
        if (value > std::numeric_limits<std::uint64_t>::max() - total) {
            shape.arithmetic_overflow = true;
            return;
        }
        total += value;
    };
    for (const auto& region : plan.sample_regions) {
        shape.members_per_region =
            std::max(shape.members_per_region, static_cast<std::uint64_t>(region.members.size()));
        add(region.members.size(), shape.members_total);
        shape.input_boundaries_per_region =
            std::max(shape.input_boundaries_per_region,
                     static_cast<std::uint64_t>(region.input_boundaries.size()));
        shape.output_boundaries_per_region =
            std::max(shape.output_boundaries_per_region,
                     static_cast<std::uint64_t>(region.output_boundaries.size()));
        add(region.input_boundaries.size(), shape.input_boundaries_total);
        add(region.output_boundaries.size(), shape.output_boundaries_total);
        shape.parameters_per_region =
            std::max(shape.parameters_per_region,
                     static_cast<std::uint64_t>(region.promoted_parameters.size()));
        add(region.promoted_parameters.size(), shape.parameters_total);

        std::uint64_t delays = 0;
        std::unordered_set<NodeId> members;
        members.reserve(region.members.size());
        for (const auto& member : region.members) {
            members.insert(member.node);
            if (member.type_id == "pulp.core.unit-delay" && member.version == 1)
                ++delays;
            if (member.type_id == "pulp.core.unit-delay" && member.version == 1) {
                shape.kernel_state_bytes =
                    std::max<std::uint64_t>(shape.kernel_state_bytes, sizeof(float));
                shape.kernel_state_alignment =
                    std::max<std::uint64_t>(shape.kernel_state_alignment, alignof(float));
            }
        }
        shape.delays_per_region = std::max(shape.delays_per_region, delays);
        add(delays, shape.delays_total);
        shape.state_bytes_per_region =
            std::max(shape.state_bytes_per_region,
                     static_cast<std::uint64_t>(region.limits.max_state_bytes));
        shape.logical_boundary_bytes_per_region = std::max(
            shape.logical_boundary_bytes_per_region, region.limits.max_logical_boundary_bytes);
        shape.work_per_frame_per_region =
            std::max(shape.work_per_frame_per_region,
                     static_cast<std::uint64_t>(region.limits.max_work_per_frame));
        shape.work_per_block_per_region =
            std::max(shape.work_per_block_per_region, region.limits.max_work_per_block);
        add(region.limits.max_state_bytes, shape.state_bytes_total);
        add(region.limits.max_logical_boundary_bytes, shape.logical_boundary_bytes_total);
        add(region.limits.max_work_per_frame, shape.work_per_frame_total);
        add(region.limits.max_work_per_block, shape.work_per_block_total);

        std::uint64_t touching = 0;
        for (const auto& connection : plan.connections) {
            if (members.contains(connection.src_node) || members.contains(connection.dst_node))
                ++touching;
        }
        shape.connections_per_region = std::max(shape.connections_per_region, touching);
    }
    shape.declared_bytes = available_bytes;
    return prove_sample_region_parser_shape(shape).accepted;
}

void put_region(std::vector<std::uint8_t>& out, const SampleRegionDefinition& region) {
    put_u32(out, region.region_id);
    put_u32(out, region.limits.max_member_nodes);
    put_u32(out, region.limits.max_internal_connections);
    put_u32(out, region.limits.max_input_boundaries);
    put_u32(out, region.limits.max_output_boundaries);
    put_u32(out, region.limits.max_delay_nodes);
    put_u32(out, region.limits.max_promoted_parameters);
    put_u32(out, region.limits.max_state_bytes);
    put_u64(out, region.limits.max_logical_boundary_bytes);
    put_u32(out, region.limits.max_work_per_frame);
    put_u64(out, region.limits.max_work_per_block);
    put_u32(out, static_cast<std::uint32_t>(region.members.size()));
    for (const auto& member : region.members) {
        put_u32(out, member.node);
        put_u8(out, static_cast<std::uint8_t>(member.config.kind));
        put_u32(out, member.config.boundary_index_or_parameter_id);
        put_f32(out, member.config.constant);
    }
    put_u32(out, static_cast<std::uint32_t>(region.input_boundaries.size()));
    for (const auto id : region.input_boundaries)
        put_u32(out, id);
    put_u32(out, static_cast<std::uint32_t>(region.output_boundaries.size()));
    for (const auto id : region.output_boundaries)
        put_u32(out, id);
    put_u32(out, static_cast<std::uint32_t>(region.promoted_parameters.size()));
    for (const auto& parameter : region.promoted_parameters) {
        put_u32(out, parameter.param_id);
        put_u32(out, static_cast<std::uint32_t>(parameter.key.size()));
        put_bytes(out, reinterpret_cast<const std::uint8_t*>(parameter.key.data()),
                  parameter.key.size());
        put_u32(out, static_cast<std::uint32_t>(parameter.name.size()));
        put_bytes(out, reinterpret_cast<const std::uint8_t*>(parameter.name.data()),
                  parameter.name.size());
        put_u32(out, static_cast<std::uint32_t>(parameter.unit.size()));
        put_bytes(out, reinterpret_cast<const std::uint8_t*>(parameter.unit.data()),
                  parameter.unit.size());
        put_f32(out, parameter.range.min);
        put_f32(out, parameter.range.max);
        put_f32(out, parameter.range.default_value);
        put_f32(out, parameter.range.step);
        put_f32(out, parameter.range.skew);
        put_u8(out, parameter.range.symmetric_skew ? 1 : 0);
        put_u8(out, static_cast<std::uint8_t>(parameter.rate));
        put_f32(out, parameter.smoothing_ramp_seconds);
        put_u32(out, parameter.bound_node_id);
        put_u16(out, parameter.bound_port);
    }
}

}  // namespace

namespace detail {

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
    if (plan.format_version == kBakedMaxSupportedFormatVersion) {
        put_u32(out, static_cast<std::uint32_t>(plan.sample_regions.size()));
        for (const auto& region : plan.sample_regions)
            put_region(out, region);
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
        static_cast<std::size_t>(plan.input_channels) > kBakedMaxTotalPorts ||
        static_cast<std::size_t>(plan.output_channels) > kBakedMaxTotalPorts) {
        return std::nullopt;
    }

    const std::uint32_t node_count = r.u32();
    if (!r.ok || node_count > kBakedMaxNodes) return std::nullopt;
    plan.nodes.reserve(node_count);
    // Store arity by value (not a Node* into the growing vector) so the port-OOB
    // check below can't be broken by a future edit to the reserve/growth policy.
    struct Arity { int in; int out; };
    std::unordered_map<NodeId, Arity> by_id;
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
        if (plan.format_version == kBakedMaxSupportedFormatVersion && !canonical_v2_node(n))
            return std::nullopt;
        if (n.num_input_ports > kBakedMaxPortsPerNode ||
            n.num_output_ports > kBakedMaxPortsPerNode) {
            return std::nullopt;
        }
        total_ports += static_cast<std::size_t>(n.num_input_ports) +
                       static_cast<std::size_t>(n.num_output_ports);
        if (total_ports > kBakedMaxTotalPorts) return std::nullopt;
        if (!by_id.emplace(n.id,
                           Arity{n.num_input_ports, n.num_output_ports}).second) {
            return std::nullopt;
        }
        plan.nodes.push_back(std::move(n));
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
        const auto feedback = r.u8();
        if (!r.ok || (plan.format_version == kBakedMaxSupportedFormatVersion && feedback > 1))
            return std::nullopt;
        c.feedback = feedback != 0;
        // Port-OOB check against the referenced nodes' declared arity: a connection
        // that indexes a port a node doesn't have would corrupt buffer routing.
        const auto sit = by_id.find(c.src_node);
        const auto dit = by_id.find(c.dst_node);
        if (sit == by_id.end() || dit == by_id.end()) return std::nullopt;
        if (c.src_port < 0 || c.src_port >= sit->second.out) return std::nullopt;
        if (c.dst_port < 0 || c.dst_port >= dit->second.in) return std::nullopt;
        plan.connections.push_back(c);
    }

    if (plan.format_version == kBakedMaxSupportedFormatVersion) {
        const std::uint32_t region_count = r.u32();
        if (!r.ok || region_count == 0 || region_count > kBakedMaxRegions)
            return std::nullopt;
        plan.sample_regions.reserve(region_count);
        for (std::uint32_t region_index = 0; region_index < region_count; ++region_index) {
            SampleRegionDefinition region;
            region.region_id = r.u32();
            region.limits.max_member_nodes = r.u32();
            region.limits.max_internal_connections = r.u32();
            region.limits.max_input_boundaries = r.u32();
            region.limits.max_output_boundaries = r.u32();
            region.limits.max_delay_nodes = r.u32();
            region.limits.max_promoted_parameters = r.u32();
            region.limits.max_state_bytes = r.u32();
            region.limits.max_logical_boundary_bytes = r.u64();
            region.limits.max_work_per_frame = r.u32();
            region.limits.max_work_per_block = r.u64();
            const std::uint32_t member_count = r.u32();
            if (!r.ok || member_count == 0 || member_count > kBakedMaxRegionMembers ||
                member_count > region.limits.max_member_nodes)
                return std::nullopt;
            region.members.reserve(member_count);
            for (std::uint32_t i = 0; i < member_count; ++i) {
                const NodeId id = static_cast<NodeId>(r.u32());
                const auto kind = static_cast<SampleKernelConfigKind>(r.u8());
                if (kind == SampleKernelConfigKind::Invalid ||
                    static_cast<std::uint8_t>(kind) >
                        static_cast<std::uint8_t>(SampleKernelConfigKind::PromotedParameterId))
                    return std::nullopt;
                SampleRegionKernelNode member;
                member.node = id;
                member.config.kind = kind;
                member.config.boundary_index_or_parameter_id = r.u32();
                member.config.constant = r.f32();
                const auto node = std::find_if(plan.nodes.begin(), plan.nodes.end(),
                                               [id](const auto& value) { return value.id == id; });
                if (!r.ok || node == plan.nodes.end() || node->type != NodeType::Custom)
                    return std::nullopt;
                member.type_id = node->custom_type_id;
                member.version = node->custom_version;
                region.members.push_back(std::move(member));
            }

            auto read_boundaries = [&](std::vector<NodeId>& boundaries, std::size_t cap,
                                       std::uint32_t limit) -> bool {
                const std::uint32_t count = r.u32();
                if (!r.ok || count == 0 || count > cap || count > limit)
                    return false;
                boundaries.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i)
                    boundaries.push_back(static_cast<NodeId>(r.u32()));
                return r.ok;
            };
            if (!read_boundaries(region.input_boundaries, kBakedMaxRegionBoundaries,
                                 region.limits.max_input_boundaries) ||
                !read_boundaries(region.output_boundaries, kBakedMaxRegionBoundaries,
                                 region.limits.max_output_boundaries))
                return std::nullopt;

            const std::uint32_t parameter_count = r.u32();
            if (!r.ok || parameter_count > kBakedMaxRegionParameters ||
                parameter_count > region.limits.max_promoted_parameters)
                return std::nullopt;
            region.promoted_parameters.reserve(parameter_count);
            for (std::uint32_t i = 0; i < parameter_count; ++i) {
                SampleRegionPromotedParameter parameter;
                parameter.param_id = r.u32();
                parameter.key = r.string(kBakedMaxRegionString);
                parameter.name = r.string(kBakedMaxRegionString);
                parameter.unit = r.string(kBakedMaxRegionString);
                parameter.range.min = r.f32();
                parameter.range.max = r.f32();
                parameter.range.default_value = r.f32();
                parameter.range.step = r.f32();
                parameter.range.skew = r.f32();
                const auto symmetric = r.u8();
                const auto rate = r.u8();
                parameter.range.symmetric_skew = symmetric != 0;
                parameter.rate = static_cast<state::ParamRate>(rate);
                parameter.smoothing_ramp_seconds = r.f32();
                parameter.bound_node_id = static_cast<NodeId>(r.u32());
                parameter.bound_port = static_cast<PortIndex>(r.u16());
                if (!r.ok || symmetric > 1 ||
                    rate != static_cast<std::uint8_t>(state::ParamRate::ControlRate))
                    return std::nullopt;
                region.promoted_parameters.push_back(std::move(parameter));
            }
            plan.sample_regions.push_back(std::move(region));
        }
    }

    if (!r.at_end()) return std::nullopt;  // trailing garbage
    if (!canonical_region_manifest(plan))
        return std::nullopt;
    if (plan.format_version == kBakedMaxSupportedFormatVersion &&
        !parser_shape_within_limits(plan, bytes.size()))
        return std::nullopt;
    return plan;
}

}  // namespace detail

namespace {

constexpr std::string_view kBakedMagic = "PULPBAKE";           // 8 bytes
constexpr std::string_view kBakedSigDomainV1 = "PULPBAKE-sig-v1";
constexpr std::string_view kBakedSigDomainV2 = "PULPBAKE-sig-v2";

// The domain-separated message the Ed25519 signature covers. Binding the tag +
// versions + ext_len + plan_len + plan_sha256 means a tampered plan (hash), an
// altered version, or a changed length all break verification — not just a swapped
// plan body. The hash stands in for the plan bytes (checked separately at load).
// Membership test for a signer key against the trust set. Mirrors node_pack's
// key_is_trusted, including its refusal to ever trust an empty key (a misconfigured
// empty entry must not match an empty/degenerate signer key).
bool key_is_trusted(const BakedTrust& trust, const std::vector<std::uint8_t>& key) {
    if (key.empty()) return false;
    return std::any_of(trust.trusted_public_keys.begin(), trust.trusted_public_keys.end(),
                       [&](const std::vector<std::uint8_t>& k) {
                           return !k.empty() && k.size() == key.size() &&
                                  std::equal(k.begin(), k.end(), key.begin());
                       });
}

std::vector<std::uint8_t> build_canonical_message(std::uint32_t format_version,
                                                  std::uint32_t schema_version,
                                                  std::uint32_t ext_len,
                                                  std::uint32_t plan_len,
                                                  const std::vector<std::uint8_t>& plan_sha256) {
    std::vector<std::uint8_t> m;
    const auto domain =
        format_version == kBakedPlanV1FormatVersion ? kBakedSigDomainV1 : kBakedSigDomainV2;
    m.insert(m.end(), domain.begin(), domain.end());
    put_u32(m, format_version);
    put_u32(m, schema_version);
    put_u32(m, ext_len);
    put_u32(m, plan_len);
    m.insert(m.end(), plan_sha256.begin(), plan_sha256.end());
    return m;
}

}  // namespace

std::vector<std::uint8_t> write_baked_signed(const BakedPlan& plan,
                                             std::span<const std::uint8_t> private_key_64) {
    if (private_key_64.size() != runtime::ed25519_private_key_size) return {};
    if ((plan.format_version != kBakedPlanV1FormatVersion &&
         plan.format_version != kBakedMaxSupportedFormatVersion) ||
        !canonical_region_manifest(plan))
        return {};
    const auto plan_bytes = detail::serialize_plan(plan);
    if (plan_bytes.size() > kBakedMaxPlanBytes) return {};

    const auto plan_sha = runtime::sha256(plan_bytes.data(), plan_bytes.size());
    if (plan_sha.size() != 32) return {};
    const std::uint32_t ext_len = 0;  // reserved for v1
    const auto canonical =
        build_canonical_message(static_cast<std::uint32_t>(plan.format_version),
                                static_cast<std::uint32_t>(kBakedManifestSchemaVersion), ext_len,
                                static_cast<std::uint32_t>(plan_bytes.size()), plan_sha);
    const auto sig = runtime::ed25519_sign(private_key_64.data(), private_key_64.size(),
                                           canonical.data(), canonical.size());
    if (!sig || sig->size() != runtime::ed25519_signature_size) return {};
    // NaCl secret key is seed(32) || public_key(32); the public half is embedded.
    const std::uint8_t* public_key = private_key_64.data() + runtime::ed25519_seed_size;

    std::vector<std::uint8_t> manifest;
    put_u32(manifest, static_cast<std::uint32_t>(plan.format_version));
    put_u32(manifest, static_cast<std::uint32_t>(kBakedManifestSchemaVersion));
    put_u32(manifest, ext_len);
    put_u32(manifest, static_cast<std::uint32_t>(plan_bytes.size()));
    put_bytes(manifest, plan_sha.data(), plan_sha.size());
    put_bytes(manifest, public_key, runtime::ed25519_public_key_size);
    put_bytes(manifest, sig->data(), sig->size());

    std::vector<std::uint8_t> out;
    put_bytes(out, reinterpret_cast<const std::uint8_t*>(kBakedMagic.data()), kBakedMagic.size());
    put_u32(out, static_cast<std::uint32_t>(manifest.size()));
    put_u32(out, static_cast<std::uint32_t>(plan_bytes.size()));
    put_bytes(out, manifest.data(), manifest.size());
    put_bytes(out, plan_bytes.data(), plan_bytes.size());
    return out;
}

std::optional<BakedPlan> verify_and_extract_plan(std::span<const std::uint8_t> bytes,
                                                 const BakedTrust& trust) {
    Reader r{bytes.data(), bytes.size()};
    for (char c : kBakedMagic) {
        if (r.u8() != static_cast<std::uint8_t>(c)) return std::nullopt;
    }
    const std::uint32_t manifest_len = r.u32();
    const std::uint32_t plan_len = r.u32();
    if (!r.ok || manifest_len > kBakedManifestMaxBytes || plan_len > kBakedMaxPlanBytes) {
        return std::nullopt;
    }
    if (!r.have(manifest_len)) return std::nullopt;

    // Parse the fixed v1 manifest through a sub-reader bounded to manifest_len, so a
    // short/oversized manifest can never read into the plan region.
    Reader m{bytes.data() + r.pos, manifest_len};
    r.pos += manifest_len;
    const std::uint32_t fmt = m.u32();
    const std::uint32_t schema = m.u32();
    const std::uint32_t ext_len = m.u32();
    const std::uint32_t m_plan_len = m.u32();
    if (!m.ok ||
        (fmt != static_cast<std::uint32_t>(kBakedPlanV1FormatVersion) &&
         fmt != static_cast<std::uint32_t>(kBakedMaxSupportedFormatVersion)) ||
        schema != static_cast<std::uint32_t>(kBakedManifestSchemaVersion) || ext_len != 0 ||
        m_plan_len != plan_len) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> plan_sha(32), public_key(32), signature(64);
    for (auto& b : plan_sha) b = m.u8();
    for (auto& b : public_key) b = m.u8();
    for (auto& b : signature) b = m.u8();
    if (!m.ok || !m.at_end()) return std::nullopt;  // manifest must be exactly consumed

    // Signer must be trusted BEFORE we spend a verify on attacker-chosen bytes.
    if (!key_is_trusted(trust, public_key)) return std::nullopt;

    const auto canonical =
        build_canonical_message(fmt, schema, ext_len, plan_len, plan_sha);
    if (!runtime::ed25519_verify(public_key.data(), public_key.size(), signature.data(),
                                 signature.size(), canonical.data(), canonical.size())) {
        return std::nullopt;
    }

    if (!r.have(plan_len)) return std::nullopt;
    std::span<const std::uint8_t> plan_span(bytes.data() + r.pos, plan_len);
    r.pos += plan_len;
    if (!r.at_end()) return std::nullopt;  // trailing garbage past the plan

    // Signature is authentic and binds this hash; confirm the bytes match it, THEN
    // parse. No plan byte has been interpreted before this point.
    const auto actual_sha = runtime::sha256(plan_span.data(), plan_span.size());
    if (actual_sha.size() != plan_sha.size() ||
        !std::equal(actual_sha.begin(), actual_sha.end(), plan_sha.begin())) {
        return std::nullopt;
    }
    auto plan = detail::parse_plan_bounded(plan_span);
    // Cross-check the plan's own format_version against the (signed) manifest fmt so a
    // future v2 body cannot ride inside a v1-declared envelope, or vice versa.
    if (plan && static_cast<std::uint32_t>(plan->format_version) != fmt) return std::nullopt;
    return plan;
}

}  // namespace pulp::host
