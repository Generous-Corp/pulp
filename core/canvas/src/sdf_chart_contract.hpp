#pragma once

#include <pulp/canvas/canvas.hpp>

#include <string>

// The SDF chart admission contract, header-only on purpose.
//
// Whether a shape emits a stroke chart is decided by a switch on an enum and
// needs no renderer, so both the Skia path and the non-Skia stub must give the
// same answer. Keeping the definitions inline here rather than in a separate
// translation unit matters because the canvas sources are not always linked as
// the `pulp-canvas` library: the web targets compile
// `core/canvas/src/*.cpp` straight into their own module, so a companion .cpp
// registered only in `core/canvas/CMakeLists.txt` leaves those builds with an
// undefined symbol at link time.
namespace pulp::canvas::sdf_chart {

/// Whether a shape emits a usable stroke chart (t/d/side). A property of the
/// geometry, not of the renderer — the rule `sksl_declares_uniform()` already
/// follows for uniforms, so the two backends agree.
inline bool shape_has_chart(Canvas::SDFShape shape) {
    return shape == Canvas::SDFShape::flat_arc;
}

/// The shape's name, so a refusal is something a reader can act on.
inline const char* shape_name(Canvas::SDFShape shape) {
    switch (shape) {
        case Canvas::SDFShape::rect: return "rect";
        case Canvas::SDFShape::circle: return "circle";
        case Canvas::SDFShape::rounded_rect: return "rounded_rect";
        case Canvas::SDFShape::arc: return "arc";
        case Canvas::SDFShape::diamond: return "diamond";
        case Canvas::SDFShape::squircle: return "squircle";
        case Canvas::SDFShape::triangle: return "triangle";
        case Canvas::SDFShape::ring: return "ring";
        case Canvas::SDFShape::stadium: return "stadium";
        case Canvas::SDFShape::cross: return "cross";
        case Canvas::SDFShape::flat_segment: return "flat_segment";
        case Canvas::SDFShape::rounded_segment: return "rounded_segment";
        case Canvas::SDFShape::flat_arc: return "flat_arc";
        case Canvas::SDFShape::quadratic_bezier: return "quadratic_bezier";
    }
    return "unknown";
}

/// Whether author source asks for the chart at all. A shape without a chart is
/// only a problem for a shader that wants one.
inline bool source_uses_chart(const std::string& sksl) {
    return sksl.find("PulpChart") != std::string::npos ||
           sksl.find("pulp_chart") != std::string::npos;
}

/// The refusal an unsupported shape earns, naming the shape.
inline std::string refusal_for(Canvas::SDFShape shape) {
    return std::string("Shape '") + shape_name(shape) +
           "' has no stroke chart (t/d/side); chart shaders require a band "
           "shape whose chart is implemented (flat_arc)";
}

} // namespace pulp::canvas::sdf_chart
