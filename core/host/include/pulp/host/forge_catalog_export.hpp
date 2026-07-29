#pragma once

#include <pulp/host/forge_param_descriptor.hpp>
#include <pulp/host/signal_graph.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace pulp::host {

inline constexpr std::size_t kForgeCatalogNodeCount = 78;

struct ForgeCatalogExportResult {
    bool ok = false;
    std::string json;
    std::string error;
    std::size_t node_count = 0;
};

struct ForgeRealizationBakedParams {
    std::string mode;
    std::vector<CustomNodeBakedParam> params;
};

using ForgeNodeRealizationBakedParams = std::vector<ForgeRealizationBakedParams>;

/// Complete semantic inventory, in stable pack order.
std::vector<ForgeNodeDescriptor> forge_catalog_descriptors();

/// Numeric contracts for every concrete realization, in descriptor order.
std::vector<ForgeNodeRealizationBakedParams> forge_catalog_realization_baked_params();

/// Union contracts used by the structural descriptor audit. Export callers use
/// the realization-specific contracts above so differing defaults stay exact.
std::vector<std::vector<CustomNodeBakedParam>> forge_catalog_baked_params();

/// Join semantic descriptors to the factories' baked numeric contracts and
/// serialize one fail-closed machine-readable document.
ForgeCatalogExportResult export_forge_catalog_json();

/// Test seam for the fail-closed join. Production callers use the no-argument
/// overload; a missing or extra node, audit fault, or count mismatch fails.
ForgeCatalogExportResult
export_forge_catalog_json(const std::vector<ForgeNodeDescriptor>& descriptors,
                          const std::vector<ForgeNodeRealizationBakedParams>& baked_per_node,
                          std::size_t expected_node_count);

} // namespace pulp::host
