#include <pulp/vellum_components/gpu_health_content.hpp>

#include <vellum/view/screenshot_compare.hpp>

namespace pulp::vellum_components {

PngContentStats analyze_png_content(const std::vector<std::uint8_t>& png) {
    const auto stats = ::vellum::view::analyze_screenshot_content(png);
    PngContentStats out;
    out.valid = stats.valid;
    out.passes_content_floor = stats.passes_content_floor();
    out.total_pixels = stats.total_pixels;
    out.opaque_coverage = stats.opaque_coverage;
    out.unique_colors = stats.unique_colors;
    return out;
}

} // namespace pulp::vellum_components
