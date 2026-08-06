#pragma once

#include <pulp/view/view.hpp>

#include <memory>

namespace pulp::view {

/// Uniformly scales one fixed-size subtree to fit this view's bounds — the
/// SUB-VIEW analogue of `WindowHost::set_design_viewport`.
///
/// `set_design_viewport` fits a design into the WINDOW: it pins the root to the
/// design size and letterboxes at paint. That is the right tool when the design
/// IS the window (a plug-in editor). It cannot help when the design is one PANE
/// inside a larger app — a preview pane in a builder, a thumbnail, a split
/// view — because there is only one window viewport and the surrounding chrome
/// already owns it.
///
/// Without this, a host embedding a fixed-size imported design (an IR root with
/// `widthMode/heightMode: fixed`) into a shorter pane has only bad options: let
/// it overflow (clipped at the bottom), scroll it (the design is a single
/// composition, not a document — scrolling hides half a control panel), or
/// stretch it to the pane (distorts every circle and corner radius). None of
/// those is what "show me the design" means.
///
/// So: give the design its authored bounds, and scale the whole subtree
/// uniformly to fit — letterbox, never distort. Layout inside the design still
/// happens at authored size, so nothing reflows, no text re-wraps, and a canvas
/// child still records its draw commands at the size it was authored for.
///
/// The container takes the size its parent gives it (flex_grow / a percent
/// dimension / an explicit preferred size). It deliberately reports no
/// intrinsic size: a fit container that measured itself at the design size
/// would size its parent to the design and there would be nothing to fit into.
///
/// Input stays aligned with paint because `hit_test` inverts the SAME stored
/// transform paint applies, and `point_to_local` (pointer_dispatch) already
/// divides out an ancestor's `scale()` when localizing the event — so a control
/// is hit exactly where it is drawn, at any pane size.
///
/// Holds exactly ONE child, installed with `set_content` — the container owns
/// that subtree's bounds and render transform (it overwrites any `set_scale` /
/// transform-origin the content carried). A child added any other way is
/// neither laid out nor hit-tested here; wrap sibling overlays around the
/// DesignFitView instead, where they stay at pane scale.
class DesignFitView : public View {
public:
    DesignFitView();
    ~DesignFitView() override;

    /// Install the subtree to fit. Replaces (and returns) any previous content.
    std::unique_ptr<View> set_content(std::unique_ptr<View> content);
    View* content() const { return content_; }
    /// Detach the content without destroying it.
    std::unique_ptr<View> take_content();

    /// Authored size of the content, in design points. Pass (0, 0) to go back
    /// to deriving it from the content itself (its `preferred_width/height`,
    /// else its intrinsic size) — which is what an imported design's own root
    /// already carries.
    void set_design_size(float w, float h);
    float design_width() const { return design_w_; }
    float design_height() const { return design_h_; }

    /// Whether a design SMALLER than the container may be scaled UP to fill it.
    /// Default false: a design is authored at a size, and blowing it past that
    /// size fattens hairlines and softens raster assets for no gain. A design
    /// that already fits is left at 1:1 and centered.
    void set_allow_upscale(bool allow);
    bool allow_upscale() const { return allow_upscale_; }

    /// Anchor the fitted design to the TOP of the container instead of
    /// centering it vertically. Horizontal centering is unchanged. Matches the
    /// `top_align` option on the window-level design viewport.
    void set_top_align(bool top_align);
    bool top_align() const { return top_align_; }

    /// The applied fit, valid after layout. `fit_scale` is uniform on both axes
    /// by construction; the offsets are the design's top-left in this view's
    /// coordinate space (the letterbox bars are the slack around it).
    float fit_scale() const { return fit_scale_; }
    float content_offset_x() const { return offset_x_; }
    float content_offset_y() const { return offset_y_; }

    bool owns_child_layout() const override { return true; }
    void layout_children() override;
    View* hit_test(Point local_point) override;

private:
    /// Resolve the design size actually used this pass: the explicit size when
    /// set, else what the content declares. Returns false when neither yields a
    /// positive size (nothing to fit — the content is then given the container's
    /// own bounds at 1:1, which is the pre-fit behavior).
    bool resolve_design_size(float& w, float& h) const;

    View* content_ = nullptr;
    float design_w_ = 0.0f;
    float design_h_ = 0.0f;
    bool allow_upscale_ = false;
    bool top_align_ = false;

    float fit_scale_ = 1.0f;
    float offset_x_ = 0.0f;
    float offset_y_ = 0.0f;
};

}  // namespace pulp::view
