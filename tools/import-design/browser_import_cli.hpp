#pragma once

#include <pulp/view/design_ir.hpp>
#include <pulp/view/screenshot.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace pulp::import_design {

class BrowserCaptureWorkspace;
namespace internal {
struct BrowserImportCliResultBuilder;
}

struct BrowserImportCliRequest {
    std::filesystem::path input_file;
    std::filesystem::path output_file;
    std::filesystem::path importer_executable;
    std::optional<std::filesystem::path> browser_executable;
    pulp::view::DesignSource source = pulp::view::DesignSource::claude;
    int initial_width = 1280;
    int initial_height = 800;
    std::string reference_image;
    std::string diff_output;
    std::vector<std::filesystem::path> reserved_output_paths;
    float fail_below_percent = -1.0f;
    pulp::view::ScreenshotBackend screenshot_backend =
        pulp::view::ScreenshotBackend::skia;
    bool offline = false;
    bool allow_browser_network = false;
    bool dry_run = false;
    bool supports_faithful_capture = true;
    /// Browser capture always runs its required A/B proof. This records an
    /// explicit --validate request to additionally publish convenience render
    /// and diff files beside the primary output.
    bool validate = false;
};

struct BrowserImportNotApplicable {};

struct BrowserImportFailure {
    int exit_code = 1;
};

class BrowserCapturedImport {
public:
    BrowserCapturedImport(BrowserCapturedImport&&) noexcept;
    BrowserCapturedImport& operator=(BrowserCapturedImport&&) noexcept;
    ~BrowserCapturedImport();
    BrowserCapturedImport(const BrowserCapturedImport&) = delete;
    BrowserCapturedImport& operator=(const BrowserCapturedImport&) = delete;

    [[nodiscard]] int render_width() const noexcept { return render_width_; }
    [[nodiscard]] int render_height() const noexcept { return render_height_; }
    [[nodiscard]] bool similarity_failed() const noexcept {
        return similarity_failed_;
    }
    [[nodiscard]] const std::string& reference_image() const noexcept {
        return reference_image_;
    }
    [[nodiscard]] pulp::view::DesignIR take_design_ir() {
        return std::move(design_ir_);
    }

    /// Publish localized assets and evidence first, then atomically publish all
    /// staged primary outputs. Repeated calls are harmless; a failed commit
    /// remains retryable.
    [[nodiscard]] bool commit_evidence(std::string& error);

    /// Reserve a private path for a required generated output. The caller
    /// writes only that path; commit_evidence publishes dependencies first and
    /// primary outputs last in one rollback-capable transaction.
    [[nodiscard]] std::filesystem::path stage_primary_output(
        const std::filesystem::path& destination, std::string& error);

private:
    class EvidenceTransaction;

    BrowserCapturedImport() = default;

    pulp::view::DesignIR design_ir_;
    int render_width_ = 1280;
    int render_height_ = 800;
    bool similarity_failed_ = false;
    std::string reference_image_;
    std::unique_ptr<EvidenceTransaction> evidence_;

    friend struct internal::BrowserImportCliResultBuilder;
};

using BrowserImportCliResult = std::variant<
    BrowserImportNotApplicable,
    BrowserImportFailure,
    BrowserCapturedImport>;

/// Own the browser-specific CLI transaction: capture messaging and policy,
/// portable-asset localization, native validation, and deferred evidence
/// publication. A captured result is move-only because it owns a one-shot
/// evidence transaction. The caller adopts its DesignIR, writes generated
/// outputs to stage_primary_output() paths, then commits required generated
/// outputs, localized assets, and evidence together. Diagnostic/report
/// ledgers remain intentional failure-side effects for troubleshooting.
BrowserImportCliResult run_browser_import_cli(
    const BrowserImportCliRequest& request,
    std::string_view content);

/// Handle detect-only reporting for a runnable browser-backed HTML file.
/// Returns no value when the input should continue through compat detection.
std::optional<int> run_browser_detect_cli(
    const std::filesystem::path& input_file,
    const std::optional<std::filesystem::path>& browser_executable = {});

/// Apply the direct-file HTML source default and print its CLI notice.
/// Explicit source selections remain untouched.
bool infer_browser_html_source_cli(
    const std::filesystem::path& input_file,
    std::string& source);

}  // namespace pulp::import_design
