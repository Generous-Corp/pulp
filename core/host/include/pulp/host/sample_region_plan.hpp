#pragma once

#include <pulp/host/sample_region_proof.hpp>

#include <optional>
#include <vector>

namespace pulp::host {

struct PreparedSampleKernelBinding {
    NodeId node = 0;
    SampleKernelDescriptor descriptor;
    PreparedSampleKernelConfig config;
    std::uint32_t input_slot_offset = 0;
    std::uint32_t output_slot_offset = 0;
};

struct PreparedSampleRegionTransfer {
    NodeId source = 0;
    PortIndex source_port = 0;
    NodeId destination = 0;
    PortIndex destination_port = 0;
    std::uint32_t source_slot = 0;
    std::uint32_t destination_slot = 0;

    friend bool operator==(const PreparedSampleRegionTransfer&,
                           const PreparedSampleRegionTransfer&) = default;
};

enum class PreparedSampleRegionOperationKind : std::uint8_t {
    DelayPublish,
    Process,
    Transfer,
    DelayCommit,
};

struct PreparedSampleRegionOperation {
    PreparedSampleRegionOperationKind kind = PreparedSampleRegionOperationKind::Process;
    std::size_t index = 0;

    friend bool operator==(const PreparedSampleRegionOperation&,
                           const PreparedSampleRegionOperation&) = default;
};

struct PreparedSampleRegionPlan {
    SampleRegionId region_id = 0;
    std::vector<PreparedSampleKernelBinding> kernels;
    std::vector<std::size_t> delay_publish_order;
    std::vector<std::size_t> combinational_order;
    std::vector<std::size_t> delay_commit_order;
    std::vector<PreparedSampleRegionTransfer> transfers;
    std::vector<PreparedSampleRegionOperation> operations;
    std::vector<state::ParamID> promoted_parameters;
    std::uint32_t scalar_slot_count = 0;
    SampleRegionResourceStats resources;
};

struct SampleRegionPlanResult {
    SampleRegionProof proof;
    std::optional<PreparedSampleRegionPlan> plan;
};

SampleRegionPlanResult build_sample_region_plan(const SampleRegionCandidate& candidate);

} // namespace pulp::host
