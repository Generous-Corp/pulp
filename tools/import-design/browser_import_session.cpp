// SPDX-License-Identifier: MIT

#include "browser_import_session.hpp"

#include <ostream>
#include <utility>
#include <variant>

namespace pulp::import_design {

BrowserImportSession::BrowserImportSession(BrowserCapturedImport capture)
    : capture_(std::move(capture)) {}

bool BrowserImportSession::has_capture() const noexcept {
    return capture_.has_value();
}

ImportPreparationPolicy
BrowserImportSession::preparation_policy() const noexcept {
    return has_capture() ? ImportPreparationPolicy::captured_frame()
                         : ImportPreparationPolicy::source_parsed();
}

std::optional<BrowserCaptureAdoption>
BrowserImportSession::take_capture_adoption() {
    if (capture_adopted_ || !capture_) return std::nullopt;

    BrowserCaptureAdoption adoption;
    adoption.render_width = capture_->render_width();
    adoption.render_height = capture_->render_height();
    adoption.reference_image = capture_->reference_image();
    adoption.similarity_failed = capture_->similarity_failed();
    adoption.design_ir = capture_->take_design_ir();
    capture_adopted_ = true;
    return adoption;
}

std::optional<std::filesystem::path>
BrowserImportSession::stage_primary_output(
    const std::filesystem::path& destination,
    std::ostream& diagnostics) {
    if (!capture_) return destination;

    std::string error;
    auto staged = capture_->stage_primary_output(destination, error);
    if (!staged.empty()) return staged;
    diagnostics << "Error: " << error << "\n";
    return std::nullopt;
}

bool BrowserImportSession::publish(std::ostream& diagnostics) {
    if (!capture_) return true;

    std::string error;
    if (capture_->commit_evidence(error)) return true;
    diagnostics << "Error: " << error << "\n";
    return false;
}

BrowserImportSessionResult internal::make_browser_import_session(
    BrowserImportCliResult result) {
    if (auto* failure = std::get_if<BrowserImportFailure>(&result)) {
        return *failure;
    }
    if (auto* capture = std::get_if<BrowserCapturedImport>(&result)) {
        return BrowserImportSession(std::move(*capture));
    }
    return BrowserImportSession();
}

BrowserImportSessionResult run_browser_import_session(
    const BrowserImportCliRequest& request,
    std::string_view content) {
    return internal::make_browser_import_session(
        run_browser_import_cli(request, content));
}

}  // namespace pulp::import_design
