#pragma once

#include <pulp/view/design_ir.hpp>
#include <pulp/view/screenshot.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace pulp::import_design {

struct BrowserCaptureValidationOptions {
    std::filesystem::path reference;
    std::filesystem::path rendered;
    std::filesystem::path diff;
    int width = 0;
    int height = 0;
    float fail_below_percent = -1.0f;
    pulp::view::ScreenshotBackend backend =
        pulp::view::ScreenshotBackend::skia;
};

struct BrowserCaptureValidationResult {
    bool valid = false;
    bool passes = false;
    float similarity = 0.0f;
    std::uint64_t diff_pixels = 0;
    std::uint64_t total_pixels = 0;
    double mean_error = 0.0;
    std::string error;
};

BrowserCaptureValidationResult validate_browser_capture_design_ir(
    const pulp::view::DesignIR& ir,
    const BrowserCaptureValidationOptions& options);

}  // namespace pulp::import_design
