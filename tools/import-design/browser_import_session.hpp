// SPDX-License-Identifier: MIT
#pragma once

#include "browser_import_cli.hpp"
#include "import_preparation_policy.hpp"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace pulp::import_design {

/// Browser-captured state adopted by the source-independent import pipeline.
struct BrowserCaptureAdoption {
    int render_width = 1280;
    int render_height = 800;
    std::string reference_image;
    bool similarity_failed = false;
    pulp::view::DesignIR design_ir;
};

/// Owns the CLI boundary between browser intake and generated-output
/// publication. Source-parsed imports pass through staging/publication as
/// no-ops; captured imports retain their transaction until publish().
class BrowserImportSession {
public:
    BrowserImportSession() = default;
    explicit BrowserImportSession(BrowserCapturedImport capture);
    BrowserImportSession(BrowserImportSession&&) noexcept = default;
    BrowserImportSession& operator=(BrowserImportSession&&) noexcept = default;
    BrowserImportSession(const BrowserImportSession&) = delete;
    BrowserImportSession& operator=(const BrowserImportSession&) = delete;

    [[nodiscard]] bool has_capture() const noexcept;
    [[nodiscard]] ImportPreparationPolicy preparation_policy() const noexcept;

    /// Move captured state into the downstream pipeline. This is a one-shot
    /// adoption; subsequent calls return no value.
    [[nodiscard]] std::optional<BrowserCaptureAdoption>
    take_capture_adoption();

    /// Return the destination unchanged for source-parsed imports, or reserve
    /// a private transaction path for a captured import.
    [[nodiscard]] std::optional<std::filesystem::path> stage_primary_output(
        const std::filesystem::path& destination,
        std::ostream& diagnostics);

    /// Publish the capture evidence and all staged primary outputs. This is a
    /// no-op for source-parsed imports and remains retryable after failure.
    [[nodiscard]] bool publish(std::ostream& diagnostics);

private:
    std::optional<BrowserCapturedImport> capture_;
    bool capture_adopted_ = false;
};

using BrowserImportSessionResult =
    std::variant<BrowserImportFailure, BrowserImportSession>;

/// Normalize browser intake into either an exit-bearing failure or a live
/// source-parsed/captured publication session.
BrowserImportSessionResult run_browser_import_session(
    const BrowserImportCliRequest& request,
    std::string_view content);

namespace internal {

/// Result-normalization seam for the injected browser-import tests.
BrowserImportSessionResult make_browser_import_session(
    BrowserImportCliResult result);

}  // namespace internal

}  // namespace pulp::import_design
