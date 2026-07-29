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
    missing_catalog_node,    ///< an indexed semantic node is absent from the export
    unexpected_catalog_node, ///< an export node is absent from the canonical registry
    missing_realization,     ///< a declared realization was not constructed for export
    unexpected_realization,  ///< a constructed realization is absent from the descriptor
    mismatched_type_id,      ///< a constructed node disagrees with its declared type id
    duplicate_type_id,       ///< two concrete realizations register the same type id
    duplicate_axis,          ///< two axes or two values on one axis share a stable key
    duplicate_realization,   ///< two realizations share a stable mode
    missing_axis_setting,    ///< a realization does not select every declared axis
    unexpected_axis_setting, ///< a realization selects an axis the node does not declare
    unknown_axis_value,      ///< a realization selects an undeclared value for an axis
    undescribed_param,       ///< the node bakes a parameter no descriptor names
    stale_descriptor,        ///< a descriptor names a parameter the node no longer bakes
    duplicate_key,           ///< two descriptors share a stable key
    duplicate_id,            ///< two descriptors name the same parameter
    missing_choices,         ///< stepped, but no named states
    duplicate_choice,        ///< named states share a stable token or display label
    choices_on_continuous,   ///< named states on a continuous sweep
    choice_out_of_range,     ///< a named state's value is outside the baked range
    unknown_realization,     ///< a realization_mode naming no declared realization
    empty_field,             ///< key, label or description left blank
    no_realizations          ///< a node with no concrete realization to build
};

struct ForgeAuditFinding {
    ForgeAuditFault fault = ForgeAuditFault::undescribed_param;
    std::string node;    ///< node descriptor key
    std::string subject; ///< param key, id, or mode — whatever identifies it
    std::string detail;
};

const char* forge_audit_fault_name(ForgeAuditFault fault);

/// Audit one descriptor against the node it describes.
///
/// `baked` is the parameter list from one built CustomNodeType — the single
/// source of numeric truth for the named realization. Catalog-level callers
/// must audit every realization because ranges, defaults, and parameter
/// presence may differ between them.
std::vector<ForgeAuditFinding>
audit_forge_descriptor(const ForgeNodeDescriptor& descriptor,
                       const std::vector<CustomNodeBakedParam>& baked,
                       std::string_view realization_mode = {});

/// Prove that a projected export contains exactly the expected semantic nodes,
/// each once. The expected list is maintained independently of the projection
/// so a dropped or orphan registration cannot silently produce a different,
/// still-valid JSON document.
std::vector<ForgeAuditFinding>
audit_forge_catalog_membership(const std::vector<ForgeNodeDescriptor>& descriptors,
                               const std::vector<std::string_view>& expected_node_keys);

/// Human-readable one-line rendering, for a test failure or a CLI report.
std::string describe_forge_audit_finding(const ForgeAuditFinding& finding);

} // namespace pulp::host
