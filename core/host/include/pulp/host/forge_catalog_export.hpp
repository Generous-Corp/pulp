#pragma once

#include <pulp/host/forge_descriptor_audit.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace pulp::host {

/// One actually constructed realization. Keeping every realization's baked
/// contract avoids silently projecting one preset's defaults onto another.
struct ForgeCatalogExportRealization {
    std::string_view mode;
    std::string type_id;
    std::vector<CustomNodeBakedParam> baked_params;
};

/// One machine-readable catalog family before JSON serialization. The
/// descriptor owns semantic vocabulary; each constructed realization remains
/// the sole numeric range/default and concrete type-id authority.
struct ForgeCatalogExportNode {
    ForgeNodeDescriptor descriptor;
    std::vector<ForgeCatalogExportRealization> realizations;
};

/// Exact v1 authoring routes. Boundaries can only be created by a region builder.
/// This separate record preserves the layout of existing Forge export records.
struct ForgeSampleRegionV1 {
    std::string_view role;
    std::string_view type_id;
    std::string_view label;
    int type_version;
    int sample_kernel_version;
    unsigned inputs;
    unsigned outputs;
    unsigned state_bytes;
    unsigned state_alignment;
    std::string_view config_kind;
    std::string_view placement;
};

inline constexpr ForgeSampleRegionV1 kForgeSampleRegionV1[] = {
    {"input_boundary", "pulp.core.sample-region.input", "Sample Region Input", 1, 1, 1, 1, 0, 1,
     "BoundaryIndex", "region_builder_only"},
    {"output_boundary", "pulp.core.sample-region.output", "Sample Region Output", 1, 1, 1, 1, 0, 1,
     "BoundaryIndex", "region_builder_only"},
    {"constant", "pulp.core.sample-region.constant", "Constant", 1, 1, 0, 1, 0, 1, "FiniteConstant",
     "normal_node"},
    {"parameter", "pulp.core.sample-region.parameter", "Parameter", 1, 1, 0, 1, 0, 1,
     "PromotedParameterId", "normal_node"},
    {"add", "pulp.core.sample-region.add", "Add", 1, 1, 2, 1, 0, 1, "None", "normal_node"},
    {"multiply", "pulp.core.sample-region.multiply", "Multiply", 1, 1, 2, 1, 0, 1, "None",
     "normal_node"},
    {"unit_delay", "pulp.core.unit-delay", "Unit Delay", 1, 1, 1, 1, 4, 4, "None", "normal_node"},
};

/// A legacy realization without an exact v1 route remains block-only.
inline const ForgeSampleRegionV1* forge_sample_region_v1(std::string_view type_id) noexcept {
    for (const auto& row : kForgeSampleRegionV1)
        if (row.type_id == type_id)
            return &row;
    return nullptr;
}

/// Build the semantic catalog projection used by both the CLI and the installed
/// SDK snapshot.
std::vector<ForgeCatalogExportNode> forge_catalog_export_nodes();

/// Audit descriptor/range joins and the independent expected-membership list.
std::vector<ForgeAuditFinding>
audit_forge_catalog_export(const std::vector<ForgeCatalogExportNode>& nodes);

} // namespace pulp::host
