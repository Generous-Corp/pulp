#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::host {

enum class PreparedDescriptionRefusal : std::uint8_t {
    None,
    Unprepared,
    UnsupportedPreparedProfile,
    LogicalIdentityMissing,
    AuthoredBindingMismatch,
    AuthoredBindingStale,
    ExecutionPathUnavailable,
    SnapshotChanged,
    GenerationExhausted,
    AllocationFailed,
};

struct AuthoredNodeBinding {
    std::uint64_t node_id = 0;
    std::string authored_key;
};

struct AuthoredChannelBinding {
    std::uint64_t node_id = 0;
    std::string authored_port;
    bool output = false;
    std::string authored_channel;
    std::uint32_t authored_ordinal = 0;
    std::uint32_t runtime_port_index = 0;
};

struct AuthoredInterfaceChannelBinding {
    std::string interface_key;
    bool output = false;
    std::uint64_t node_id = 0;
    std::string authored_port;
    std::string authored_channel;
    std::uint32_t authored_ordinal = 0;
    std::uint32_t runtime_bus_index = 0;
    std::uint32_t runtime_channel_index = 0;
};

struct AuthoredRuntimeBinding {
    std::vector<AuthoredNodeBinding> nodes;
    std::vector<AuthoredChannelBinding> channels;
    std::vector<AuthoredInterfaceChannelBinding> interfaces;
};

struct PreparedDescriptionNode {
    std::string authored_key;
    std::string kind;
    std::uint32_t input_ports = 0;
    std::uint32_t output_ports = 0;
    std::uint32_t gain_bits = 0;
};

struct PreparedDescriptionEdge {
    std::string source;
    std::string destination;
    std::uint32_t source_port = 0;
    std::uint32_t destination_port = 0;
    std::string input_mode;
};

// Detached, value-owned capture of one admitted prepared serial path. No
// pointer, NodeId, allocator state, or mutable runtime storage is retained.
struct PreparedDescriptionData {
    std::string schema = "pulp.graph.prepared-io-gain.v1";
    std::string preparation_contract = "pulp.graph.prepare.io-gain.serial.v1";
    std::string semantic_profile;
    std::string semantic_payload;
    std::string semantic_digest;
    double sample_rate = 0.0;
    std::uint64_t max_block_frames = 0;
    std::vector<std::string> channels;
    std::vector<PreparedDescriptionNode> nodes;
    std::vector<PreparedDescriptionEdge> edges;
    std::vector<std::string> schedule;
    std::uint64_t snapshot_id = 0;
    std::uint64_t pool_snapshot_id = 0;
    std::uint64_t owner_generation = 0;
    std::uint64_t profile_max_nodes = 512;
    std::uint64_t profile_max_connections = 2048;
    std::uint64_t profile_max_ports = 4096;
    std::uint64_t profile_max_block_frames = 4096;
    std::uint64_t host_max_nodes = 0;
    std::uint64_t host_max_connections = 0;
    std::uint64_t host_max_ports = 0;
    std::uint64_t host_max_block_frames = 0;
    std::uint64_t host_max_estimated_work_units = 0;
    std::uint64_t effective_max_nodes = 0;
    std::uint64_t effective_max_connections = 0;
    std::uint64_t effective_max_ports = 0;
    std::uint64_t effective_max_block_frames = 0;

    std::string canonical_bytes() const;
    std::string content_hash() const;
};

struct PreparedDescriptionResult {
    PreparedDescriptionRefusal refusal = PreparedDescriptionRefusal::None;
    std::shared_ptr<const PreparedDescriptionData> value;

    explicit operator bool() const noexcept {
        return value != nullptr && refusal == PreparedDescriptionRefusal::None;
    }
    static PreparedDescriptionResult refused(PreparedDescriptionRefusal code) noexcept {
        return {code, {}};
    }
};

const char* prepared_description_refusal_name(PreparedDescriptionRefusal) noexcept;

} // namespace pulp::host
