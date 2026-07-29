// SPDX-License-Identifier: MIT
#pragma once

#include "browser_capture_limits.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <pulp/platform/child_process.hpp>

namespace pulp::import_design::browser_capture {

namespace fs = std::filesystem;

inline constexpr int kMinimumChromiumMajor = 109;
inline constexpr std::string_view kBrowserCaptureRuntimeDirectory =
    "browser_capture-v1";
inline constexpr std::string_view kLegacyBrowserCaptureRuntimeDirectory =
    "browser_capture";

enum class BrowserOrigin {
    explicit_override,
    environment_override,
    managed,
    system,
};

std::string browser_origin_name(BrowserOrigin origin);

enum class BrowserMode {
    auto_select,
    managed,
    system,
};

std::string browser_mode_name(BrowserMode mode);

struct BrowserCandidate {
    fs::path executable;
    BrowserOrigin origin = BrowserOrigin::system;
};

enum class BrowserProbeFailure {
    none,
    browser_unavailable,
    browser_incompatible,
    node_unavailable,
    node_incompatible,
    capture_runtime_unavailable,
    capture_capability_unavailable,
};

struct BrowserProbeResult {
    BrowserCandidate candidate;
    bool compatible = false;
    std::string product;
    std::string version;
    int major_version = 0;
    std::string failure;
    BrowserProbeFailure failure_kind = BrowserProbeFailure::none;
};

struct BrowserInstallation {
    fs::path executable;
    BrowserOrigin origin = BrowserOrigin::system;
    std::string product;
    std::string version;
    int major_version = 0;
};

struct Diagnostic {
    std::string code;
    std::string message;
    std::string phase;
};

struct BrowserDiscoveryOptions {
    std::optional<fs::path> explicit_path;

    // nullopt reads PULP_DESIGN_BROWSER. An engaged empty string disables the
    // environment lookup, which keeps tests and hermetic callers deterministic.
    std::optional<std::string> environment_override;

    // nullopt reads PULP_DESIGN_BROWSER_MODE, then
    // [import_design].browser. An engaged empty string selects auto without
    // reading process configuration, which keeps tests hermetic.
    std::optional<std::string> mode_override;

    // Defaults to PULP_HOME/tools/chrome-for-testing or the
    // platform-equivalent Pulp tool root.
    std::optional<fs::path> managed_root;

    // These candidates are considered before the platform defaults. Set
    // include_default_system_candidates=false for a hermetic discovery table.
    std::vector<fs::path> system_candidates;
    bool include_default_system_candidates = true;

    std::optional<fs::path> node_executable;
    std::optional<fs::path> capture_script;
    int minimum_major = kMinimumChromiumMajor;
    int probe_timeout_ms = 15000;
};

struct BrowserModeSelection {
    std::optional<BrowserMode> mode;
    std::string source;
    std::string error;
};

BrowserModeSelection resolve_browser_mode(
    const BrowserDiscoveryOptions& options = {});

using BrowserProbeFunction =
    std::function<BrowserProbeResult(const BrowserCandidate&)>;

struct BrowserDiscoveryResult {
    std::optional<BrowserInstallation> selected;
    std::vector<BrowserProbeResult> probes;
    Diagnostic diagnostic;

    [[nodiscard]] bool ok() const noexcept { return selected.has_value(); }
};

std::vector<BrowserCandidate>
collect_browser_candidates(const BrowserDiscoveryOptions& options = {});

BrowserProbeResult probe_browser(
    const BrowserCandidate& candidate,
    const BrowserDiscoveryOptions& options = {});

BrowserDiscoveryResult discover_browser(
    const BrowserDiscoveryOptions& options = {},
    BrowserProbeFunction probe = {});

std::string browser_unavailable_human(
    const BrowserDiscoveryResult& discovery);

std::string browser_unavailable_json(
    const BrowserDiscoveryResult& discovery);

struct CaptureRequest {
    fs::path input_file;
    fs::path staged_root;
    fs::path output_directory;

    std::optional<fs::path> node_executable;
    std::optional<fs::path> capture_script;

    int initial_width = 1280;
    int initial_height = 800;
    int device_scale_factor = kDefaultDeviceScaleFactor;
    int timeout_ms = 60000;
    bool allow_network = false;
};

struct CaptureArtifacts {
    fs::path envelope;
    fs::path reference_png;
    fs::path semantic_report;
    fs::path token_report;
    fs::path dom_snapshot;
};

struct CaptureResult {
    std::optional<CaptureArtifacts> artifacts;
    Diagnostic diagnostic;
    platform::ProcessResult process;

    [[nodiscard]] bool ok() const noexcept { return artifacts.has_value(); }
};

CaptureResult capture_document(
    const BrowserInstallation& browser,
    const CaptureRequest& request);

struct DiscoverAndCaptureResult {
    BrowserDiscoveryResult discovery;
    CaptureResult capture;

    [[nodiscard]] bool ok() const noexcept {
        return discovery.ok() && capture.ok();
    }
};

DiscoverAndCaptureResult discover_and_capture(
    const BrowserDiscoveryOptions& discovery_options,
    const CaptureRequest& request,
    BrowserProbeFunction probe = {});

}  // namespace pulp::import_design::browser_capture
