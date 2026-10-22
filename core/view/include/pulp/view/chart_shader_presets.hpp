#pragma once

// Named chart-shader presets: ready-made `half4 shade(PulpChart g)` sources for
// the bevelled caps, inset tracks, tubes and LED light pipes audio UIs are
// built from. Each preset is ordinary SkSL fed to the existing chart API —
// `Canvas::draw_sdf_shape_with_shader` in C++, `setWidgetShaderChart` or
// `setWidgetShaderChartPreset` from JS — so it works on every charted shape
// (flat arc, ring, flat and rounded segment, stadium) and can be copied and
// edited like hand-written source.
//
// Shading reads the chart's cross-section: `u = cross / halfWidth` runs from
// -1 to +1 across the band, a height profile over `u` gives a surface normal
// along the chart normal (tan.y, -tan.x), and a directional light is applied
// with Lambert diffuse and Blinn-Phong specular. Colours are decoded from sRGB
// to linear before any lighting arithmetic and encoded back afterwards, and a
// flat face facing the viewer reproduces the authored colour exactly.

#include <pulp/canvas/canvas.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {

/// Cross-section of a tube or light pipe across its band.
enum class ChartProfile {
    round,   ///< a cylinder: the normal turns smoothly from edge to edge
    flat,    ///< a flat face with a narrow rounded rim at each edge
    chamfer, ///< a flat centre with 45 degree chamfers on the outer 40%
};

/// Largest accepted `glow_sigma`. The draw rect grows by several sigma on
/// every side, so an unbounded sigma would grow it without limit.
inline constexpr float kChartGlowSigmaMax = 256.0f;

struct ChartShaderPresetOptions {
    /// Material colour (sRGB) for `bevel`, `inset-channel`, `tube` and the
    /// base of `centre-highlight`.
    canvas::Color color = canvas::Color::rgba8(150, 155, 165);
    /// Emissive colour (sRGB) for `light-pipe`, `glow` and the highlight of
    /// `centre-highlight`.
    canvas::Color accent = canvas::Color::rgba8(100, 170, 255);
    /// Direction the light comes from, in screen degrees counter-clockwise
    /// from the right: 90 is from above, 270 from below. The light lives in
    /// the canvas's local space, so a rotated draw rotates it too.
    float light_angle_deg = 120.0f;
    /// Height of the light above the surface, 0 (grazing) to 90 (overhead).
    float light_elevation_deg = 45.0f;
    /// `bevel` only: a pressed bevel sinks instead of rising, which reverses
    /// which edge catches the light.
    bool pressed = false;
    /// Cross-section for `tube` and `light-pipe`.
    ChartProfile profile = ChartProfile::round;
    /// Fraction of the half width the bevel or channel wall occupies, (0, 1].
    float bevel_width = 0.35f;
    /// Strength of the highlight, specular or glow, >= 0. Zero removes it.
    float intensity = 1.0f;
    /// `glow`: Gaussian falloff outside the band, in local units, from 0 to
    /// kChartGlowSigmaMax.
    float glow_sigma = 6.0f;
};

struct ChartShaderPreset {
    /// SkSL defining `half4 shade(PulpChart g)`; empty when refused.
    std::string sksl;
    /// Draw-rect reach the preset needs on every side: for `glow`, whose
    /// light lives outside the band, `chart_glow_reach(sigma, intensity)`;
    /// otherwise zero.
    float reach = 0.0f;
    /// Why the request was refused; empty on success.
    std::string error;
};

/// The preset names: `bevel`, `inset-channel`, `tube`, `light-pipe`,
/// `centre-highlight` and `glow`. `center-highlight` is accepted as an alias.
const std::vector<std::string>& chart_shader_preset_names();

/// Build the named preset's chart shader with `options` baked in as
/// constants. Pure: no canvas or GPU is needed, and the same inputs give the
/// same source. An unknown name or an out-of-range option is refused with a
/// message naming it.
ChartShaderPreset make_chart_shader_preset(std::string_view name,
                                           const ChartShaderPresetOptions& options = {});

/// Distance past which a glow of `sigma` and `intensity` is below one 8-bit
/// step (intensity * exp(-r^2 / (2 sigma^2)) < 1/255), and never less than
/// three sigma. Zero when either is zero.
float chart_glow_reach(float sigma, float intensity);

/// Parse a profile name (`round`, `flat`, `chamfer`).
bool parse_chart_profile(std::string_view name, ChartProfile& out);

} // namespace pulp::view
