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

/// Build the semantic catalog projection used by both the CLI and the installed
/// SDK snapshot.
std::vector<ForgeCatalogExportNode> forge_catalog_export_nodes();

/// Audit descriptor/range joins and the independent expected-membership list.
std::vector<ForgeAuditFinding>
audit_forge_catalog_export(const std::vector<ForgeCatalogExportNode>& nodes);

} // namespace pulp::host
