// SPDX-License-Identifier: MIT
#pragma once

#include "browser_capture_backend.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

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

struct VerifiedDomSnapshot {
    std::filesystem::path path;
    std::string bytes;
};

/// Resolve and hash a producer-declared DOM snapshot from the capture
/// directory. The returned bytes are the exact bytes whose lowercase SHA-256
/// matched the declaration, so consumers can parse this buffer without a
/// second filesystem read.
std::optional<VerifiedDomSnapshot> read_verified_dom_snapshot(
    const std::filesystem::path& envelope,
    std::string_view snapshot_asset,
    std::string_view snapshot_sha256,
    std::string& error);

/// Validate the source, browser, viewport, and optional materialized-runtime
/// sidecar provenance emitted by browser_capture/capture.mjs.
CaptureProvenanceResult validate_capture_provenance(const CaptureProvenanceRequest& request);

} // namespace pulp::import_design::browser_capture
