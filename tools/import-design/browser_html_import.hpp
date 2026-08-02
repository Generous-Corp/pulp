#pragma once

#include <pulp/view/design_ir.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace pulp::import_design {

class BrowserCaptureWorkspace;

struct BrowserHtmlImportRequest {
    std::filesystem::path input_file;
    std::filesystem::path output_file;
    std::filesystem::path importer_executable;
    std::optional<std::filesystem::path> browser_executable;
    std::optional<std::filesystem::path> browser_interactions;
    pulp::view::DesignSource source = pulp::view::DesignSource::claude;
    int initial_width = 1280;
    int initial_height = 800;
    bool offline = false;
    bool skia_validation = true;
    bool allow_browser_network = false;
    bool dry_run = false;
    bool supports_faithful_capture = true;
    /// Draw the panel from its lowered nodes instead of the captured bitmap.
    /// See BrowserCaptureIrOptions::native_panel_lowering — off by default
    /// because the capture is the A-side of the A/B, not a legacy path.
    bool native_panel_lowering = false;
};

struct BrowserHtmlNotApplicable {};

struct BrowserHtmlLegacyFallback {
    std::string shape;
};

struct BrowserHtmlFailure {
    int exit_code = 2;
    std::string error;
    std::string shape;
    std::vector<std::shared_ptr<BrowserCaptureWorkspace>> workspaces;
};

struct BrowserHtmlCaptured {
    std::string shape;
    pulp::view::DesignIR design_ir;
    std::filesystem::path capture_directory;
    std::filesystem::path durable_capture_directory;
    std::filesystem::path reference_png;
    std::filesystem::path semantic_report;
    std::vector<std::shared_ptr<BrowserCaptureWorkspace>> workspaces;
};

using BrowserHtmlImportResult = std::variant<
    BrowserHtmlNotApplicable,
    BrowserHtmlLegacyFallback,
    BrowserHtmlFailure,
    BrowserHtmlCaptured>;

struct BrowserImportReadiness {
    bool available = false;
    std::filesystem::path executable;
    std::string product;
    std::string version;
    std::string error;
};

BrowserImportReadiness probe_browser_import_readiness(
    const std::optional<std::filesystem::path>& browser_executable = {});

/// Classify and, when applicable, evaluate one runnable HTML document through
/// isolated Chromium. This is the single policy/orchestration boundary between
/// CLI argument handling and the browser backend/DesignIR lowering layers.
BrowserHtmlImportResult import_browser_html(
    const BrowserHtmlImportRequest& request,
    std::string_view content);

}  // namespace pulp::import_design
