#pragma once

#include <pulp/host/custom_node_type.hpp>
#include <pulp/host/graph_types.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::host {

using SampleRegionId = std::uint32_t;

enum class SampleRegionConnectionLane : std::uint8_t {
    PlainAudio,
    Midi,
    Event,
    Automation,
    AudioRateModulation,
    Sidechain,
};

struct SampleRegionConnection {
    NodeId source = 0;
    PortIndex source_port = 0;
    NodeId destination = 0;
    PortIndex destination_port = 0;
    SampleRegionConnectionLane lane = SampleRegionConnectionLane::PlainAudio;
    bool legacy_feedback = false;

    friend bool operator==(const SampleRegionConnection&, const SampleRegionConnection&) = default;
};

struct SampleRegionKernelNode {
    NodeId node = 0;
    std::string type_id;
    int version = 1;
    SampleKernelConfig config;
};

using SampleRegionKernelResolveFn = const SampleKernelDescriptor* (*)(const void* context,
                                                                      std::string_view type_id,
                                                                      int version) noexcept;

struct SampleRegionKernelRegistryView {
    const void* context = nullptr;
    SampleRegionKernelResolveFn resolve = nullptr;

    // Returned descriptors must remain stable for the duration of a proof or
    // plan build. A SignalGraph registry satisfies that contract while its
    // authoring state is held stable by the caller.
    const SampleKernelDescriptor* find(std::string_view type_id, int version) const noexcept {
        return resolve == nullptr ? nullptr : resolve(context, type_id, version);
    }
};

struct SampleRegionLimits {
    std::uint32_t max_member_nodes = 64;
    std::uint32_t max_internal_connections = 125;
    std::uint32_t max_input_boundaries = 8;
    std::uint32_t max_output_boundaries = 8;
    std::uint32_t max_delay_nodes = 32;
    std::uint32_t max_promoted_parameters = 16;
    std::uint32_t max_state_bytes = 128;
    std::uint64_t max_logical_boundary_bytes = 1'048'576;
    std::uint32_t max_work_per_frame = 191;
    std::uint64_t max_work_per_block = 3'129'344;

    static constexpr SampleRegionLimits v1() noexcept {
        return {};
    }
};

struct SampleRegionCandidate {
    SampleRegionId region_id = 0;
    SampleRegionKernelRegistryView registry;
    std::vector<SampleRegionKernelNode> members;
    // Includes internal edges and the legal outside-to-input/output-to-outside
    // crossings for this region. Connections with neither endpoint in members
    // are invalid input to the proof.
    std::vector<SampleRegionConnection> connections;
    std::vector<state::ParamID> promoted_parameters;
    SampleRegionLimits limits = SampleRegionLimits::v1();
    std::uint32_t max_block_size = 0;
};

struct SampleRegionResourceStats {
    std::uint32_t member_nodes = 0;
    std::uint32_t internal_connections = 0;
    std::uint32_t input_boundaries = 0;
    std::uint32_t output_boundaries = 0;
    std::uint32_t delay_nodes = 0;
    std::uint32_t promoted_parameters = 0;
    std::uint64_t state_bytes = 0;
    std::uint32_t state_alignment = 1;
    std::uint64_t logical_boundary_bytes = 0;
    std::uint64_t work_per_frame = 0;
    std::uint64_t work_per_block = 0;
};

enum class SampleRegionRefusalReason : std::uint8_t {
    None,
    UnknownRegion,
    UnknownMember,
    MemberInMultipleRegions,
    InvalidBoundary,
    InvalidBoundaryCrossing,
    InvalidProducerCardinality,
    DisconnectedMember,
    InvalidKernelConfig,
    UnsupportedNodeKind,
    SampleKernelOutsideRegion,
    UnresolvedSampleKernel,
    UnsupportedConnectionLane,
    LegacyFeedbackInRegion,
    CycleCrossesRegionBoundary,
    InstantaneousCycle,
    NonZeroCompensatableLatency,
    DuplicatePromotedParameter,
    ParameterContractMismatch,
    RegionLimitExceeded,
    NodeLimitExceeded,
    ConnectionLimitExceeded,
    DelayStateLimitExceeded,
    ParameterLimitExceeded,
    StateBudgetExceeded,
    BoundaryBufferBudgetExceeded,
    WorkBudgetExceeded,
    ArithmeticOverflow,
    PrepareFailed,
};

struct SampleRegionProof {
    bool accepted = false;
    SampleRegionRefusalReason reason = SampleRegionRefusalReason::UnknownRegion;
    SampleRegionId region_id = 0;
    NodeId offending_node = 0;
    SampleRegionConnection offending_connection;
    bool has_offending_connection = false;
    std::uint64_t actual = 0;
    std::uint64_t limit = 0;
    SampleRegionResourceStats resources;
    std::string message;
};

struct SampleRegionGraphProof {
    bool accepted = false;
    SampleRegionRefusalReason reason = SampleRegionRefusalReason::UnknownRegion;
    SampleRegionId offending_region = 0;
    SampleRegionProof region_proof;
    SampleRegionResourceStats totals;
    std::uint64_t actual = 0;
    std::uint64_t limit = 0;
};

// Hostile-input ceilings are checked before a format parser allocates or walks
// record arrays. `available_bytes` is the bounded payload remaining after the
// fixed header; `declared_bytes` is the checked size implied by its counts.
struct SampleRegionParserShape {
    std::uint64_t regions = 0;
    std::uint64_t members_per_region = 0;
    std::uint64_t members_total = 0;
    std::uint64_t connections_per_region = 0;
    std::uint64_t connections_total = 0;
    std::uint64_t input_boundaries_per_region = 0;
    std::uint64_t output_boundaries_per_region = 0;
    std::uint64_t input_boundaries_total = 0;
    std::uint64_t output_boundaries_total = 0;
    std::uint64_t delays_per_region = 0;
    std::uint64_t delays_total = 0;
    std::uint64_t parameters_per_region = 0;
    std::uint64_t parameters_total = 0;
    std::uint64_t kernel_state_bytes = 0;
    std::uint64_t kernel_state_alignment = 0;
    std::uint64_t state_bytes_per_region = 0;
    std::uint64_t state_bytes_total = 0;
    std::uint64_t logical_boundary_bytes_per_region = 0;
    std::uint64_t logical_boundary_bytes_total = 0;
    std::uint64_t work_per_frame_per_region = 0;
    std::uint64_t work_per_frame_total = 0;
    std::uint64_t work_per_block_per_region = 0;
    std::uint64_t work_per_block_total = 0;
    std::uint64_t declared_bytes = 0;
    std::uint64_t available_bytes = 0;
    bool arithmetic_overflow = false;
};

struct SampleRegionParserProof {
    bool accepted = false;
    SampleRegionRefusalReason reason = SampleRegionRefusalReason::PrepareFailed;
    std::uint64_t actual = 0;
    std::uint64_t limit = 0;
};

SampleRegionProof prove_sample_region(const SampleRegionCandidate& candidate);
SampleRegionGraphProof prove_sample_regions(const std::vector<SampleRegionCandidate>& candidates);
SampleRegionParserProof prove_sample_region_parser_shape(const SampleRegionParserShape& shape);

} // namespace pulp::host
