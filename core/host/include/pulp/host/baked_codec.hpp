// pulp/host/baked_codec.hpp — the .pulpbake plan payload codec (internal).
//
// A BakedPlan is the serializable form of a lowerable frozen graph: the plan
// INPUTS (bus arity + node records + connections) that prepare() recompiles into
// the snapshot deterministically. This header owns only the PLAN PAYLOAD codec —
// binary serialize + a bounded, capped, NON-public parse. It does NOT verify a
// signature and MUST NOT be handed untrusted bytes on its own: the public,
// signature-verifying load path (baked_graph_processor's load_baked) parses the
// plan through parse_plan_bounded ONLY after the Ed25519 envelope has been
// verified. Custom node process code is never in the plan — a Custom record carries
// only (type_id, version, state); the code is re-resolved from the host registry at
// load, exactly like a node pack.

#pragma once

#include <pulp/host/sample_region_authoring.hpp>
#include <pulp/host/signal_graph.hpp>  // NodeId, NodeType

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pulp::host {

// v1 plan caps. These match the routed executor's plan-builder limits (the tightest
// real constraint), NOT SignalGraph::GraphLimits — a plan the executor cannot build
// is worthless, and the tighter caps shrink the untrusted-parse surface.
inline constexpr std::size_t kBakedMaxNodes         = 512;
inline constexpr std::size_t kBakedMaxConnections   = 2048;
inline constexpr int         kBakedMaxPortsPerNode  = 64;
inline constexpr std::size_t kBakedMaxTotalPorts    = 4096;
inline constexpr int         kBakedMaxBlock         = 16384;
inline constexpr std::size_t kBakedMaxCustomTypeId  = 256;    // bytes
inline constexpr std::size_t kBakedMaxCustomState   = 65536;  // bytes per node
inline constexpr std::size_t kBakedMaxPlanBytes     = 4u * 1024u * 1024u;  // whole plan
// Version one is the legacy payload. Keep its value explicit so the default
// constructed plan and every v1 caller retain the original wire bytes while
// the public format constant now describes the largest format this codec can
// admit.
inline constexpr int kBakedPlanV1FormatVersion = 1;
inline constexpr int kBakedMaxSupportedFormatVersion = 2;
inline constexpr int kBakedPlanFormatVersion = kBakedMaxSupportedFormatVersion;

inline constexpr std::size_t kBakedMaxRegions = 16;
inline constexpr std::size_t kBakedMaxRegionMembers = 64;
inline constexpr std::size_t kBakedMaxRegionBoundaries = 8;
inline constexpr std::size_t kBakedMaxRegionParameters = 16;
inline constexpr std::size_t kBakedMaxRegionString = 1024;

// The serializable frozen-graph plan. No Plugin nodes (bake refuses them); no
// std::function anywhere (Custom code is re-resolved from the registry).
struct BakedPlan {
    int format_version = kBakedPlanV1FormatVersion;
    int input_channels = 0;
    int output_channels = 0;

    struct Node {
        NodeId id = 0;
        NodeType type = NodeType::Gain;
        int num_input_ports = 0;
        int num_output_ports = 0;
        float gain = 1.0f;                       // Gain nodes
        std::string custom_type_id;              // Custom nodes
        int custom_version = 0;                  // Custom nodes
        std::vector<std::uint8_t> custom_state;  // Custom nodes
        bool operator==(const Node&) const = default;
    };
    struct Conn {
        NodeId src_node = 0;
        int src_port = 0;
        NodeId dst_node = 0;
        int dst_port = 0;
        bool feedback = false;
        bool operator==(const Conn&) const = default;
    };
    std::vector<Node> nodes;
    std::vector<Conn> connections;
    // v2 only. These are authored, callback-free records; executable sample
    // kernels are resolved from the paired registration supplied to the v2
    // loader. The writer/parser require canonical region/member/parameter
    // ordering and refuse a non-canonical signed payload.
    std::vector<SampleRegionDefinition> sample_regions;

    bool operator==(const BakedPlan& other) const {
        if (format_version != other.format_version || input_channels != other.input_channels ||
            output_channels != other.output_channels || nodes != other.nodes ||
            connections != other.connections ||
            sample_regions.size() != other.sample_regions.size())
            return false;

        const auto equal_config = [](const SampleKernelConfig& lhs, const SampleKernelConfig& rhs) {
            return lhs.kind == rhs.kind &&
                   lhs.boundary_index_or_parameter_id == rhs.boundary_index_or_parameter_id &&
                   lhs.constant == rhs.constant;
        };
        const auto equal_member = [&](const SampleRegionKernelNode& lhs,
                                      const SampleRegionKernelNode& rhs) {
            return lhs.node == rhs.node && lhs.type_id == rhs.type_id &&
                   lhs.version == rhs.version && equal_config(lhs.config, rhs.config);
        };
        const auto equal_limits = [](const SampleRegionLimits& lhs, const SampleRegionLimits& rhs) {
            return lhs.max_member_nodes == rhs.max_member_nodes &&
                   lhs.max_internal_connections == rhs.max_internal_connections &&
                   lhs.max_input_boundaries == rhs.max_input_boundaries &&
                   lhs.max_output_boundaries == rhs.max_output_boundaries &&
                   lhs.max_delay_nodes == rhs.max_delay_nodes &&
                   lhs.max_promoted_parameters == rhs.max_promoted_parameters &&
                   lhs.max_state_bytes == rhs.max_state_bytes &&
                   lhs.max_logical_boundary_bytes == rhs.max_logical_boundary_bytes &&
                   lhs.max_work_per_frame == rhs.max_work_per_frame &&
                   lhs.max_work_per_block == rhs.max_work_per_block;
        };
        const auto equal_range = [](const state::ParamRange& lhs, const state::ParamRange& rhs) {
            return lhs.min == rhs.min && lhs.max == rhs.max &&
                   lhs.default_value == rhs.default_value && lhs.step == rhs.step &&
                   lhs.skew == rhs.skew && lhs.symmetric_skew == rhs.symmetric_skew;
        };
        const auto equal_parameter = [&](const SampleRegionPromotedParameter& lhs,
                                         const SampleRegionPromotedParameter& rhs) {
            return lhs.param_id == rhs.param_id && lhs.key == rhs.key && lhs.name == rhs.name &&
                   lhs.unit == rhs.unit && equal_range(lhs.range, rhs.range) &&
                   lhs.rate == rhs.rate &&
                   lhs.smoothing_ramp_seconds == rhs.smoothing_ramp_seconds &&
                   lhs.bound_node_id == rhs.bound_node_id && lhs.bound_port == rhs.bound_port;
        };
        const auto equal_region = [&](const SampleRegionDefinition& lhs,
                                      const SampleRegionDefinition& rhs) {
            if (lhs.region_id != rhs.region_id || !equal_limits(lhs.limits, rhs.limits) ||
                lhs.members.size() != rhs.members.size() ||
                lhs.input_boundaries != rhs.input_boundaries ||
                lhs.output_boundaries != rhs.output_boundaries ||
                lhs.promoted_parameters.size() != rhs.promoted_parameters.size())
                return false;
            for (std::size_t i = 0; i < lhs.members.size(); ++i)
                if (!equal_member(lhs.members[i], rhs.members[i]))
                    return false;
            for (std::size_t i = 0; i < lhs.promoted_parameters.size(); ++i)
                if (!equal_parameter(lhs.promoted_parameters[i], rhs.promoted_parameters[i]))
                    return false;
            return true;
        };

        for (std::size_t i = 0; i < sample_regions.size(); ++i)
            if (!equal_region(sample_regions[i], other.sample_regions[i]))
                return false;
        return true;
    }
};

// The raw plan payload codec. These are in `detail` — NOT the public surface —
// precisely because parse_plan_bounded is unsafe on unverified bytes: its contract
// is the INVERSE of the public verify_and_extract_plan (verification must precede
// it). Only verify_and_extract_plan (after the Ed25519 envelope check) and the codec
// tests may call them.
namespace detail {

// Serialize a plan to its canonical little-endian binary form (the bytes the
// Ed25519 signature covers, minus the domain-separated header the signer prepends).
// Deterministic: the same plan always yields the same bytes.
std::vector<std::uint8_t> serialize_plan(const BakedPlan& plan);

// Parse plan bytes into a BakedPlan, enforcing every cap DURING the streaming read
// (before any per-element allocation grows unbounded) with checked arithmetic and
// port-range validation. Returns std::nullopt on ANY violation: truncation,
// unknown/future format_version, a cap exceeded, a port out of range, or trailing
// garbage. Pure — resolves no custom type and calls no load_state (that happens on
// the verified plan in the public loader). Safe to call on attacker bytes ONLY
// after the signature envelope has been verified.
std::optional<BakedPlan> parse_plan_bounded(std::span<const std::uint8_t> bytes);

}  // namespace detail

// ── signed .pulpbake envelope ────────────────────────────────────────────
// File layout: magic "PULPBAKE"(8) | manifest_len u32 | plan_len u32 | manifest |
// plan. The manifest is fixed-size v1 { format_version, schema_version, ext_len,
// plan_len, plan_sha256(32), signer_public_key(32), signature(64) }. The signature
// covers a DOMAIN-SEPARATED canonical message binding the tag + versions + ext_len
// + plan_len + plan_sha256 (which in turn covers the plan bytes) — so a tampered
// plan, hash, version, or length all fail verification. There is NO unsigned load.
inline constexpr int kBakedManifestSchemaVersion = 1;
inline constexpr std::size_t kBakedManifestMaxBytes = 4096;

// The set of publisher public keys a loader accepts (mirror NodePackTrust). Revoke
// a key by dropping it. A plan signed by a key not in this set is rejected.
struct BakedTrust {
    std::vector<std::vector<std::uint8_t>> trusted_public_keys;  // each 32 bytes
};

// Serialize + sign a plan into a distributable .pulpbake artifact. `private_key_64`
// is the NaCl-form Ed25519 secret (seed || public_key); the public key is derived
// from it and embedded. Returns empty on a signing/size failure.
std::vector<std::uint8_t> write_baked_signed(const BakedPlan& plan,
                                             std::span<const std::uint8_t> private_key_64);

// Verify a .pulpbake artifact and return its plan — or std::nullopt on ANY failure:
// bad magic, over-cap length, malformed manifest, unknown/future version, untrusted
// signer, bad signature, plan-hash mismatch, or a plan that fails parse_plan_bounded.
// Nothing below the signature check touches the plan bytes. This is the ONLY entry
// point for untrusted bytes.
std::optional<BakedPlan> verify_and_extract_plan(std::span<const std::uint8_t> bytes,
                                                 const BakedTrust& trust);

}  // namespace pulp::host
