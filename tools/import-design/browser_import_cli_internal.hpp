#pragma once

#include "browser_capture_validation.hpp"
#include "browser_html_import.hpp"
#include "browser_import_cli.hpp"

#include <functional>

namespace pulp::import_design::internal {

/// Private operations seam for transaction tests. Production callers use the
/// public two-argument entrypoint and cannot inject capture behavior.
struct BrowserImportCliOperations {
    std::function<BrowserHtmlImportResult(
        const BrowserHtmlImportRequest&, std::string_view)> import_html;
    std::function<BrowserCaptureValidationResult(
        const pulp::view::DesignIR&,
        const BrowserCaptureValidationOptions&)> validate_capture;
    std::function<bool(
        pulp::view::DesignIR&, const std::string&, std::string*)>
        localize_assets;
};

BrowserImportCliResult run_browser_import_cli_with_operations(
    const BrowserImportCliRequest& request,
    std::string_view content,
    const BrowserImportCliOperations& operations);

}  // namespace pulp::import_design::internal
