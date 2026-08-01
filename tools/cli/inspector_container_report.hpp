#pragma once

#include "inspector_shipping_report.hpp"

namespace pulp::cli::inspector_shipping {

// Mount a DMG or expand a PKG into a private temporary directory, then verify
// the contained standalone artifacts against their adjacent evidence sidecars.
bool load_container_artifact_report(const fs::path& input, Report& report,
                                    std::string& error);

} // namespace pulp::cli::inspector_shipping
