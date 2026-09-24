#pragma once

#include <pulp/canvas/canvas.hpp>

#include <string>

namespace pulp::canvas::sdf_chart {

/// Whether a shape emits a usable stroke chart (t/d/side). This is a property
/// of the geometry, not of the renderer, so every backend answers it the same
/// way — the rule `sksl_declares_uniform()` already follows for uniforms.
bool shape_has_chart(Canvas::SDFShape shape);

/// The shape's name, for an error a reader can act on.
const char* shape_name(Canvas::SDFShape shape);

/// Whether author source asks for the chart at all. A shape without a chart is
/// only a problem for a shader that wants one.
bool source_uses_chart(const std::string& sksl);

/// The refusal an unsupported shape earns, naming the shape.
std::string refusal_for(Canvas::SDFShape shape);

} // namespace pulp::canvas::sdf_chart
