#pragma once

#include <pulp/host/forge_catalog_export.hpp>

#include <string>
#include <vector>

namespace pulp::host {

std::string serialize_forge_catalog_json(const std::vector<ForgeCatalogExportNode>& nodes);

} // namespace pulp::host
