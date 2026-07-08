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

#include <pulp/host/graph_types.hpp>

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
inline constexpr int         kBakedPlanFormatVersion = 1;

// The serializable frozen-graph plan. No Plugin nodes (bake refuses them); no
// std::function anywhere (Custom code is re-resolved from the registry).
struct BakedPlan {
    int format_version = kBakedPlanFormatVersion;
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
    };
    struct Conn {
        NodeId src_node = 0;
        int src_port = 0;
        NodeId dst_node = 0;
        int dst_port = 0;
        bool feedback = false;
    };
    std::vector<Node> nodes;
    std::vector<Conn> connections;

    bool operator==(const BakedPlan&) const = default;
};

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
