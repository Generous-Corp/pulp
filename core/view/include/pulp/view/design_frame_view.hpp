#pragma once

#include <pulp/view/view.hpp>

#include <string>
#include <vector>

namespace pulp::view {

// One interactive element overlaid on a faithfully-rendered design frame. The
// element list is TYPED and supplied by the importer (source-side semantics) —
// DesignFrameView does NOT guess from the SVG. Bounds/coords are in the SVG's own
// coordinate space. (Per the Plan-B review: real behavior comes from source
// metadata, not SVG structure.)
struct DesignFrameElement {
    // Extensible: search / dropdown / button / fader land in later slices, each
    // with its own binding contract. B1 wires `knob`.
    enum class Kind { knob };

    Kind kind = Kind::knob;
    float cx = 0.0f;            ///< pivot / hit center, SVG coords
    float cy = 0.0f;
    float hit_radius = 0.0f;    ///< click-target radius, SVG coords
    // Knob: the `d` of its needle path in the source SVG. Dragging rotates that
    // path around (cx, cy) by the value angle and re-renders — only the needle
    // moves; the rest of the chrome stays pixel-exact.
    std::string needle_d;
    float value = 0.5f;        ///< 0..1
};

// Renders a design's own SVG document pixel-faithfully via Canvas::draw_svg
// (Skia SkSVGDOM), cropped to its panel, and overlays native interaction from a
// typed element list. This is the faithful-vector design-import lane's view: the
// importer materializes one of these per imported frame.
//
// B1 scope: render + crop + interactive knobs (drag to turn). Each drag patches
// only the dragged knob's needle in the SVG and repaints. The SVG is currently
// re-rendered per repaint (SkSVGDOM rebuilds the DOM each draw_svg call) — fine
// at interactive rates here, but a parsed-DOM cache is a planned optimization.
class DesignFrameView : public View {
public:
    // `svg` is the full SVG document. The panel (the design body the window
    // should show edge-to-edge) is auto-detected as the largest <rect>; pass a
    // positive panel_* to override. `elements` are the interactive overlays.
    DesignFrameView(std::string svg, std::vector<DesignFrameElement> elements,
                    float panel_x = -1, float panel_y = -1,
                    float panel_w = -1, float panel_h = -1);

    int element_count() const { return static_cast<int>(elements_.size()); }
    float panel_width() const { return panel_w_; }
    float panel_height() const { return panel_h_; }
    // Value of element `i` (0..1), or -1 if out of range. For tests/bindings.
    float element_value(int i) const;
    void set_element_value(int i, float v);

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;

private:
    int hit_element(Point pos) const;

    std::string svg_;
    std::vector<DesignFrameElement> elements_;
    float svg_w_ = 0.0f, svg_h_ = 0.0f;            // SVG intrinsic size
    float panel_x_ = 0, panel_y_ = 0, panel_w_ = 0, panel_h_ = 0;  // crop, SVG coords
    int drag_ = -1;
    float drag_start_y_ = 0.0f, drag_start_value_ = 0.0f;
};

}  // namespace pulp::view
