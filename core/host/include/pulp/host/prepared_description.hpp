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
    std::string kind_contract;
    std::string implementation;
    std::uint32_t input_ports = 0;
    std::uint32_t output_ports = 0;
    std::uint32_t gain_bits = 0;
    std::uint64_t latency_samples = 0;
    std::uint64_t tail_samples = 0;
    bool transport_sensitive = false;
    bool persistent_output = false;
    bool reads_inputs_only = false;
};

struct PreparedDescriptionInterface {
    std::string key;
    std::string node;
    std::string port;
    bool output = false;
    std::vector<std::string> channels;
};

struct PreparedDescriptionEdge {
    std::string source;
    std::string destination;
    std::uint32_t source_port = 0;
    std::uint32_t destination_port = 0;
    std::string input_mode;
    std::uint32_t gather_ordinal = 0;
    std::uint32_t pdc_delay_samples = 0;
    bool feedback = false;
    bool assignment_allowed = false;
    std::string final_input_mode;
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
    std::string sample_precision = "f32";
    std::vector<std::string> channels;
    std::vector<PreparedDescriptionInterface> interfaces;
    std::vector<PreparedDescriptionNode> nodes;
    std::vector<PreparedDescriptionEdge> edges;
    std::vector<std::string> schedule;
    std::uint64_t snapshot_id = 0;
    std::uint64_t pool_snapshot_id = 0;
    // Process-local owner provenance. Deliberately excluded from canonical
    // portable content; reminted only when a candidate is adopted.
    std::uint64_t owner_graph_identity = 0;
    std::uint64_t owner_generation = 0;
    std::string execution_backend = "pulp.graph.canonical-executor.f32.v1";
    std::string pdc_execution_domain = "dynamic";
    bool canonical_executor_routing_enabled = true;
    bool parallel_routing_enabled = false;
    bool anticipation_enabled = false;
    std::uint64_t prepared_plan_node_count = 0;
    std::uint64_t prepared_plan_connection_count = 0;
    bool serial_pool_bound = false;
    std::uint64_t buffer_slot_count = 0;
    std::uint64_t pool_slot_count = 0;
    std::uint64_t pool_max_frames = 0;
    std::uint64_t total_latency_samples = 0;
    std::string executor_contract = "pulp.graph.canonical-executor.f32.v1";
    std::string execution_mode = "serial";
    std::string tie_break_contract = "ready-node-id-ascending.v1";
    std::string execution_fallback_policy = "internal_copy";
    std::vector<std::string> input_modes;
    std::vector<std::uint32_t> connection_delay_samples;
    std::vector<std::uint8_t> reads_in_place;
    // Normalized value-owned storage/lifetime records. Each entry is a stable
    // textual record so the detached receipt never retains executor pointers.
    std::vector<std::string> buffer_classes;
    std::vector<std::string> buffer_values;
    bool host_input_alias_eligible = false;
    std::string host_input_alias_mode = "internal_copy";
    std::string host_input_alias_fallback = "internal_copy";
    std::string host_input_alias_source;
    std::string host_input_alias_view;
    std::vector<std::string> host_input_alias_mappings;
    bool host_input_alias_requires_full_width = true;
    bool host_input_alias_requires_disjoint = true;
    std::uint64_t execution_selection_generation = 0;
    std::uint64_t profile_max_nodes = 512;
    std::uint64_t profile_max_connections = 2048;
    std::uint64_t profile_max_ports_per_node = 64;
    std::uint64_t profile_max_ports = 4096;
    std::uint64_t profile_max_block_frames = 4096;
    std::uint64_t host_max_nodes = 0;
    std::uint64_t host_max_connections = 0;
    std::uint64_t host_max_ports = 0;
    std::uint64_t host_max_block_frames = 0;
    std::uint64_t host_max_estimated_work_units = 0;
    std::uint64_t runtime_max_nodes = 0;
    std::uint64_t runtime_max_connections = 0;
    std::uint64_t runtime_max_ports_per_node = 0;
    std::uint64_t runtime_max_ports = 0;
    std::uint64_t runtime_max_block_frames = 0;
    std::uint64_t effective_max_nodes = 0;
    std::uint64_t effective_max_connections = 0;
    std::uint64_t effective_max_ports_per_node = 0;
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
