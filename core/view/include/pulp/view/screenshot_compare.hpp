#pragma once

/// @file screenshot_compare.hpp
/// Compare two PNG screenshots for visual similarity.
/// Used by the design import pipeline to validate that generated Pulp code
/// renders close to the original design.

#include <string>
#include <vector>
#include <cstdint>

namespace pulp::view {

/// The default similarity a render must reach to count as matching its
/// reference, as a fraction (0.0-1.0). This is the single source of truth for
/// the C++ side — `passes()`, `pulp-screenshot --compare`, and
/// `pulp import-design --validate` all resolve their default here rather than
/// hard-coding a literal. The out-of-process harnesses under
/// `tools/import-validation/` mirror this value in their own language and
/// should be updated alongside it.
inline constexpr float kDefaultSimilarityThreshold = 0.85f;

/// Result of comparing two screenshots.
struct CompareResult {
    bool valid = false;           ///< True if comparison completed successfully
    float similarity = 0.0f;     ///< 0.0 = completely different, 1.0 = identical
    uint32_t total_pixels = 0;   ///< Total pixels compared
    uint32_t diff_pixels = 0;    ///< Pixels that differ beyond threshold
    float mean_error = 0.0f;     ///< Mean per-channel error (0-255 scale)
    std::string error;           ///< Error message if comparison failed

    /// Returns true if similarity is at or above the given threshold, which
    /// defaults to kDefaultSimilarityThreshold.
    bool passes(float threshold = kDefaultSimilarityThreshold) const {
        return valid && similarity >= threshold;
    }
};

/// Bounding box of changed pixels between two screenshots.
struct DiffBounds {
    bool valid = false;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t diff_pixels = 0;
};

/// Basic content statistics for a decoded PNG. This is intentionally separate
/// from visual similarity: two screenshots can be byte-identical and still be
/// useless if they are stable empty frames.
struct ScreenshotContentStats {
    bool valid = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t total_pixels = 0;
    uint32_t unique_colors = 0;  ///< Exact count unless unique_colors_capped is true.
    bool unique_colors_capped = false;
    double luminance_mean = 0.0;
    double luminance_stddev = 0.0;
    double alpha_mean = 0.0;
    double opaque_coverage = 0.0;
    /// Fraction of pixels that differ from the dominant background color. This
    /// is exact unless unique_colors_capped is true, where it is still useful
    /// as an empty-frame floor but not a precise coverage metric.
    double non_background_coverage = 0.0;
    std::string error;

    bool passes_content_floor(uint32_t min_unique_colors = 16,
                              double min_luminance_stddev = 1.0,
                              double min_non_background_coverage = 0.05,
                              double min_opaque_coverage = 0.95) const {
        return valid &&
               unique_colors >= min_unique_colors &&
               luminance_stddev >= min_luminance_stddev &&
               non_background_coverage >= min_non_background_coverage &&
               opaque_coverage >= min_opaque_coverage;
    }
};

/// Compare two PNG images for visual similarity.
/// @param reference_png  Raw PNG bytes of the reference (source design) image
/// @param rendered_png   Raw PNG bytes of the rendered Pulp output
/// @param tolerance      Per-channel color tolerance (0-255). Pixels within
///                       this tolerance are considered matching. Default 32
///                       allows for minor rendering differences (antialiasing,
///                       theme color variations, font rendering).
CompareResult compare_screenshots(
    const std::vector<uint8_t>& reference_png,
    const std::vector<uint8_t>& rendered_png,
    uint8_t tolerance = 32
);

/// Compare two PNG files for visual similarity.
CompareResult compare_screenshot_files(
    const std::string& reference_path,
    const std::string& rendered_path,
    uint8_t tolerance = 32
);

/// Generate a visual diff image highlighting differences between two PNGs.
/// Returns empty vector on failure.
/// Matching pixels are dimmed, differing pixels are highlighted in red.
std::vector<uint8_t> generate_diff_image(
    const std::vector<uint8_t>& reference_png,
    const std::vector<uint8_t>& rendered_png,
    uint8_t tolerance = 32
);

/// Crop a rectangular region from a PNG image and return it as a new PNG.
/// Returns an empty vector if decoding fails or the requested region does not
/// intersect the source image.
std::vector<uint8_t> crop_png(
    const std::vector<uint8_t>& png,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
);

/// Compute the bounding box of pixels that differ beyond the given tolerance.
/// Returns an invalid result when decoding fails or no pixels differ.
DiffBounds diff_bounds(
    const std::vector<uint8_t>& reference_png,
    const std::vector<uint8_t>& rendered_png,
    uint8_t tolerance = 32
);

/// Decode a PNG and compute content statistics suitable for catching empty or
/// stable single-color captures before any parity claim is accepted.
ScreenshotContentStats analyze_screenshot_content(const std::vector<uint8_t>& png);

} // namespace pulp::view
