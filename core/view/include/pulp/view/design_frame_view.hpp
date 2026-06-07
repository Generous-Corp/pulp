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
    // `knob` is SVG-patch (rotates its needle path in the SVG). `text_field` /
    // `dropdown` / `tab_group` / `stepper` are NATIVE-OVERLAY: an opaque child
    // widget is positioned over the element's rect and replaces that baked SVG
    // region. `stepper` is a header value cycled in place by `< >` chevrons.
    enum class Kind { knob, text_field, dropdown, tab_group, stepper };

    Kind kind = Kind::knob;

    // ── knob (SVG-patch) ─────────────────────────────────────────────────
    float cx = 0.0f;            ///< pivot / hit center, SVG coords
    float cy = 0.0f;
    float hit_radius = 0.0f;    ///< click-target radius, SVG coords
    // Knob: the `d` of its needle path in the source SVG. Dragging rotates that
    // path around (cx, cy) by the value angle and re-renders — only the needle
    // moves; the rest of the chrome stays pixel-exact.
    std::string needle_d;
    float value = 0.5f;        ///< 0..1

    // ── overlay controls (text_field / dropdown / tab_group) ─────────────
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;  ///< element rect, SVG coords
    std::string placeholder;                ///< text_field
    std::vector<std::string> options;       ///< dropdown options / tab labels
    int selected_index = 0;                 ///< dropdown / tab selection
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
    // The native-overlay child widget for element `i`, or nullptr (e.g. for a
    // knob, or out of range). For tests/bindings.
    View* overlay_widget(int i) const;

    // The panel is the view's natural size — a host should size its window to
    // this aspect so the design fills it with no letterbox (see paint()).
    float intrinsic_width() const override { return panel_w_; }
    float intrinsic_height() const override { return panel_h_; }

    void paint(canvas::Canvas& canvas) override;
    void layout_children() override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;

private:
    // Build the native-overlay child widgets (TextEditor / ComboBox / tabs) for
    // the non-knob elements; called once from the constructor.
    void build_overlays();
    // The ONE transform shared by paint() and hit_element(): a uniform fit of the
    // panel into `bounds`, centered (letterbox when bounds aspect != panel
    // aspect). `scale` is panel→view; (ox,oy) is the view-space position of the
    // panel's top-left. paint draws through it; hit_element inverts it — so a
    // knob is hit exactly where it is drawn, at ANY host window aspect.
    struct PanelTransform { float scale = 0.0f, ox = 0.0f, oy = 0.0f; };
    PanelTransform panel_transform(const Rect& bounds) const;

    int hit_element(Point pos) const;

    // A native-overlay child widget bound to one element (by index). The widget
    // is owned by the View child list; this just maps element -> widget so
    // layout_children() can position it via the panel transform.
    struct Overlay { int element_index = -1; View* widget = nullptr; };

    std::string svg_;
    std::vector<DesignFrameElement> elements_;
    std::vector<Overlay> overlays_;
    float svg_w_ = 0.0f, svg_h_ = 0.0f;            // SVG intrinsic size
    float panel_x_ = 0, panel_y_ = 0, panel_w_ = 0, panel_h_ = 0;  // crop, SVG coords
    int drag_ = -1;
    float drag_start_y_ = 0.0f, drag_start_value_ = 0.0f;
};

// The native-overlay widget for a `tab_group` element: a compact segmented
// control drawn opaque over the design's tab strip (so it replaces the baked
// tabs + highlight). Clicking a slot selects it and moves the highlight — the
// "regular selection state" a static SVG can't provide. Styling approximates the
// design's dark strip; pixel-exact theming is a follow-up.
class DesignTabGroup : public View {
public:
    DesignTabGroup(std::vector<std::string> labels, int selected);
    int selected() const { return selected_; }
    int tab_count() const { return static_cast<int>(labels_.size()); }

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;

private:
    std::vector<std::string> labels_;
    int selected_ = 0;
};

// The native-overlay widget for a `stepper` element: a header value cycled in
// place by `< >` chevrons (the design's section-header preset selectors). It
// paints the current option centered with a `<` on the left and `>` on the
// right; clicking the left third steps to the previous option, the right third
// to the next (clamped). Nothing is drawn behind the text, so the design's
// header chrome shows through — only the value text and chevrons are ours.
class DesignStepper : public View {
public:
    DesignStepper(std::vector<std::string> options, int selected);
    int selected() const { return selected_; }
    int option_count() const { return static_cast<int>(options_.size()); }
    const std::string& current() const;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;

private:
    std::vector<std::string> options_;
    int selected_ = 0;
};

}  // namespace pulp::view
