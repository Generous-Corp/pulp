#pragma once

/// @file gpu_health_content.hpp
/// Content statistics for a captured frame, decoded by the Vellum view layer.
/// The GPU health adapter asks for them to tell a rendered editor from a blank
/// or single-colour capture. The fields and the acceptance floor are the ones
/// the adapter's `ContentStats` carries, so a host can forward the result
/// field for field.

#include <cstdint>
#include <vector>

namespace pulp::vellum_components {

struct PngContentStats {
    bool valid = false;
    /// True when the capture clears the floor for real content: enough
    /// distinct colours, some luminance spread, enough pixels off the dominant
    /// colour, and essentially opaque.
    bool passes_content_floor = false;
    std::uint32_t total_pixels = 0;
    double opaque_coverage = 0.0;
    std::uint32_t unique_colors = 0;
};

/// Decodes @p png and reports its content statistics. An undecodable input
/// reports `valid == false` and never passes the floor.
PngContentStats analyze_png_content(const std::vector<std::uint8_t>& png);

} // namespace pulp::vellum_components
