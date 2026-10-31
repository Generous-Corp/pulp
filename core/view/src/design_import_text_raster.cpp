// How an imported design's text is rasterized: the CSS
// `-webkit-font-smoothing` a browser capture recorded on each text node, and
// the surface policy that follows from it (pulp/canvas/text_raster_policy.hpp).
#include "design_import_native_common.hpp"
#include "design_ir_helpers.hpp"

#include <pulp/canvas/text_raster_policy.hpp>

#include <vector>

namespace pulp::view {

canvas::FontSmoothing captured_font_smoothing(const IRNode& node) {
    const auto value = attr(node, "font_smoothing");
    return value && *value == "antialiased" ? canvas::FontSmoothing::antialiased
                                            : canvas::FontSmoothing::auto_smoothing;
}

canvas::TextRasterPolicy design_ir_text_raster_policy(const DesignIR& ir) {
    std::vector<canvas::FontSmoothing> text;
    const auto walk = [&](auto&& self, const IRNode& node) -> void {
        if (node.type == "text")
            text.push_back(captured_font_smoothing(node));
        for (const auto& child : node.children)
            self(self, child);
        for (const auto& alternate : node.alternate_frames)
            self(self, alternate);
    };
    walk(walk, ir.root);
    return canvas::select_text_raster_policy(text);
}

} // namespace pulp::view
