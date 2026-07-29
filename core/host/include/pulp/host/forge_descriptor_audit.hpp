#pragma once

// The descriptor lifecycle check.
//
// A descriptor that drifts from its DSP is worse than no descriptor: it reads
// as authoritative while naming a parameter that moved or vanished. This audit
// joins each descriptor against the node it annotates and fails closed in both
// directions — a newly baked parameter with no descriptor is as much a failure
// as a descriptor for a parameter that was removed.
//
// Everything here is structural. It needs no per-family knowledge, so a new
// catalog pack is covered the moment it is indexed.

#include <pulp/host/forge_param_descriptor.hpp>
#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace pulp::host {

enum class ForgeAuditFault {
    undescribed_param,     ///< the node bakes a parameter no descriptor names
    stale_descriptor,      ///< a descriptor names a parameter the node no longer bakes
    duplicate_key,         ///< two descriptors share a stable key
    duplicate_id,          ///< two descriptors name the same parameter
    missing_choices,       ///< stepped, but no named states
    choices_on_continuous, ///< named states on a continuous sweep
    choice_out_of_range,   ///< a named state's value is outside the baked range
    invalid_curve,         ///< the declared mapping is undefined for the baked range
    unknown_realization,   ///< a realization_mode naming no declared realization
    empty_field,           ///< key, label or description left blank
    no_realizations        ///< a node with no concrete realization to build
};

struct ForgeAuditFinding {
    ForgeAuditFault fault = ForgeAuditFault::undescribed_param;
    std::string node;     ///< node descriptor key
    std::string subject;  ///< param key, id, or mode — whatever identifies it
    std::string detail;
};

const char* forge_audit_fault_name(ForgeAuditFault fault);

/// Audit one descriptor against the node it describes.
///
/// `baked` is the parameter list from the built CustomNodeType — the single
/// source of numeric truth. Pass the node built at any realization: the baked
/// parameter set is the family's, and per-realization absence is expressed by
/// `realization_modes` rather than by a different bake.
std::vector<ForgeAuditFinding> audit_forge_descriptor(
    const ForgeNodeDescriptor& descriptor,
    const std::vector<CustomNodeBakedParam>& baked);

/// Audit a whole catalog. Also proves node keys are unique across packs, which
/// a per-node audit cannot see.
std::vector<ForgeAuditFinding> audit_forge_catalog(
    const std::vector<ForgeNodeDescriptor>& descriptors,
    const std::vector<std::vector<CustomNodeBakedParam>>& baked_per_node);

/// Human-readable one-line rendering, for a test failure or a CLI report.
std::string describe_forge_audit_finding(const ForgeAuditFinding& finding);

}  // namespace pulp::host
