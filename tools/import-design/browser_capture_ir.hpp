#pragma once

#include <pulp/view/design_ir.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace pulp::import_design {

/// Result of lowering a `pulp-browser-capture-v1` envelope into the portable
/// DesignIR boundary. The browser reference remains available separately for
/// same-session A/B validation; DesignIR owns the runtime-facing visual asset.
struct BrowserCaptureIrResult {
    std::optional<pulp::view::DesignIR> design_ir;
    std::filesystem::path reference_png;
    std::filesystem::path semantic_report;
    std::optional<std::filesystem::path> interaction_report;
    std::string error;

    explicit operator bool() const noexcept { return design_ir.has_value(); }
};

struct BrowserCaptureIrOptions {
    pulp::view::DesignSource source = pulp::view::DesignSource::claude;
    std::string source_file;
    bool require_interaction_report = false;
};

/// Parse and validate a capture envelope, reject sidecar paths that escape its
/// directory, and produce a faithful_capture DesignIR root backed by the
/// browser screenshot. No browser execution or fallback happens here.
BrowserCaptureIrResult lower_browser_capture_to_ir(
    const std::filesystem::path& envelope_path,
    const BrowserCaptureIrOptions& options = {});

}  // namespace pulp::import_design
