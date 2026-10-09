// SPDX-License-Identifier: MIT
#pragma once

#include "browser_capture_backend.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace pulp::import_design::browser_capture {

struct CaptureProvenanceRequest {
    std::filesystem::path envelope;
    std::filesystem::path source;
    std::optional<std::filesystem::path> materialized_document;
    BrowserInstallation browser;
    int initial_width = 0;
    int initial_height = 0;
    int device_scale_factor = kDefaultDeviceScaleFactor;
};

struct CaptureProvenanceResult {
    bool valid = false;
    std::string code;
    std::string message;

    explicit operator bool() const noexcept {
        return valid;
    }
};

/// Validate the source, browser, viewport, and optional materialized-runtime
/// sidecar provenance emitted by browser_capture/capture.mjs.
CaptureProvenanceResult validate_capture_provenance(const CaptureProvenanceRequest& request);

} // namespace pulp::import_design::browser_capture
