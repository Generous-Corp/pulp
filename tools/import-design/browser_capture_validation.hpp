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
    /// Region of the REFERENCE to compare, in reference pixels. Set when the
    /// design was cropped out of a larger capture: our render is then the panel
    /// alone while the reference is still the whole document, and comparing
    /// them whole reports a meaningless similarity that never passes. A gate
    /// that permanently says NEEDS is worse than no gate, because people learn
    /// to ignore it. Zero width or height compares the whole image.
    int reference_crop_x = 0;
    int reference_crop_y = 0;
    int reference_crop_width = 0;
    int reference_crop_height = 0;
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
