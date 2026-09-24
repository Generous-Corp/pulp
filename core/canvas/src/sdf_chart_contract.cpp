#include "sdf_chart_contract.hpp"

namespace pulp::canvas::sdf_chart {

bool shape_has_chart(Canvas::SDFShape shape) {
    return shape == Canvas::SDFShape::flat_arc;
}

const char* shape_name(Canvas::SDFShape shape) {
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

bool source_uses_chart(const std::string& sksl) {
    return sksl.find("PulpChart") != std::string::npos ||
           sksl.find("pulp_chart") != std::string::npos;
}

std::string refusal_for(Canvas::SDFShape shape) {
    return std::string("Shape '") + shape_name(shape) +
           "' has no stroke chart (t/d/side); chart shaders require a band "
           "shape whose chart is implemented (flat_arc)";
}

} // namespace pulp::canvas::sdf_chart
