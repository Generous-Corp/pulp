#pragma once

#include <pulp/host/sample_region_proof.hpp>

namespace pulp::host {

// Persistable parameter metadata. Host callbacks and borrowed store addresses
// belong to the parameter owner, never to an authored region snapshot.
struct SampleRegionPromotedParameter {
    state::ParamID param_id = 0;
    std::string key;
    std::string name;
    std::string unit;
    state::ParamRange range;
    state::ParamRate rate = state::ParamRate::ControlRate;
    float smoothing_ramp_seconds = 0.0f;
    NodeId bound_node_id = 0;
    PortIndex bound_port = 0;
};

// declare_sample_region consumes this value, validates identities, and sorts
// members by NodeId, boundaries by authored index, and parameters by ParamID.
// Nodes and boundary nodes must already exist in the private topology edit.
struct SampleRegionDefinition {
    SampleRegionId region_id = 0;
    std::vector<SampleRegionKernelNode> members;
    std::vector<NodeId> input_boundaries;
    std::vector<NodeId> output_boundaries;
    std::vector<SampleRegionPromotedParameter> promoted_parameters;
    SampleRegionLimits limits = SampleRegionLimits::v1();
};

struct SampleRegionDescriptor : SampleRegionDefinition {
    SampleRegionResourceStats resources;
};

struct SampleRegionResult {
    bool accepted = false;
    SampleRegionRefusalReason reason = SampleRegionRefusalReason::UnknownRegion;
    SampleRegionId region_id = 0;
    NodeId offending_node = 0;
    SampleRegionConnection offending_connection;
    bool has_offending_connection = false;
    std::string message;
};

} // namespace pulp::host
