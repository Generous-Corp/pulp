#pragma once

#include <pulp/canvas/canvas.hpp>
#include <choc/text/choc_JSON.h>
#include <optional>
#include <string>

namespace pulp::view {

/// Validated geometry and paint bounds for a retained canvas SDF command.
struct RetainedShaderGeometry {
    canvas::Canvas::ShaderGeometry geometry;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

/// Parse the same bounded SDF tree accepted by widget shader installation.
/// This keeps retained canvas replay on the exact validated emitter path,
/// including topology limits and per-leaf uniforms.
std::optional<RetainedShaderGeometry> parse_retained_shader_geometry(
    const choc::value::ValueView& root, std::string& error);

} // namespace pulp::view
