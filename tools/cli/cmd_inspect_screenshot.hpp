#pragma once

#include <cstdint>
#include <filesystem>
#include <pulp/inspect/discovery.hpp>
#include <string>
#include <string_view>
#include <vector>

struct InspectScreenshotPayload {
    std::vector<std::uint8_t> png;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// Decode and validate a Capture.screenshot response. The client rechecks the
/// PNG signature and dimensions so a malformed or custom inspector cannot
/// leave an artifact that merely has a .png suffix.
bool decode_inspect_screenshot(std::string_view response_json,
                               InspectScreenshotPayload& payload,
                               std::string& error);

/// Atomically replace destination with a validated screenshot payload.
/// Parent directories are created when needed; bare filenames are supported.
bool write_inspect_screenshot(const std::filesystem::path& destination,
                              const InspectScreenshotPayload& payload,
                              std::string& error);

/// Request, validate, and persist one screenshot, including the stable human
/// or JSON result contract. Returns 3 only for an unsupported capability.
int run_inspect_screenshot(
    const pulp::inspect::InspectorDiscoveryRecord& publication,
    const pulp::inspect::InspectorDiscoveryReader& discovery,
    const std::filesystem::path& destination,
    bool json_output);

/// Report a discovery/selection failure before a publication is available.
int report_inspect_screenshot_selection_failure(std::string_view message,
                                                bool json_output);
