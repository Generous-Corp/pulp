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
    /// Region of the REFERENCE to compare, in reference pixels. An explicit
    /// override: leave it zero and the region is resolved from the IR, which is
    /// what carries the capture's own registration rect. Zero width or height
    /// means "resolve it", never "compare the whole image".
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
    /// False when the comparison REFUSED to score. `similarity`, `diff_pixels`,
    /// `total_pixels` and `mean_error` are then meaningless and must not be
    /// printed as a measurement: a number nobody can trust is worse than no
    /// number, because it gets quoted.
    bool scored = false;
    float similarity = 0.0f;
    std::uint64_t diff_pixels = 0;
    std::uint64_t total_pixels = 0;
    double mean_error = 0.0;
    std::string error;
    /// Why the comparison refused, empty when it scored. Distinct from `error`:
    /// `error` means the validation could not run and the import is broken,
    /// while an unregistered capture still produced a perfectly good panel and
    /// only its oracle is unusable.
    std::string registration_reason;
};

/// Where the authored panel sits inside the reference image, in reference
/// (device) pixels.
///
/// `registered == false` is not a zero offset. It means the two images cannot
/// be put in correspondence at all, and a comparison run anyway reads the same
/// pixel box out of two pictures that hold different content there -- which
/// returns a plausible number for nothing.
struct ReferenceRegistration {
    bool registered = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    /// Why registration failed; empty when registered.
    std::string reason;
};

/// Resolve the region of the reference that corresponds to the render, from the
/// IR the render was built from. Pure: no file I/O and no image decoding, so
/// the registration rule is testable without a renderer.
///
/// In order: an explicit crop on `options`; the root's
/// `browser_authored_frame_*` attributes scaled by
/// `browser_device_scale_factor`; the identity rect when the reference already
/// is the size of the render. Otherwise it refuses.
ReferenceRegistration resolve_reference_registration(
    const pulp::view::DesignIR& ir,
    const BrowserCaptureValidationOptions& options,
    int reference_width,
    int reference_height,
    int rendered_width,
    int rendered_height);

BrowserCaptureValidationResult validate_browser_capture_design_ir(
    const pulp::view::DesignIR& ir,
    const BrowserCaptureValidationOptions& options);

}  // namespace pulp::import_design
