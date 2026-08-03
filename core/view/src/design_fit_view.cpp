#include <pulp/view/design_fit_view.hpp>

#include <pulp/view/window_host.hpp>  // compute_design_viewport_transform

namespace pulp::view {

DesignFitView::DesignFitView() = default;
DesignFitView::~DesignFitView() = default;

std::unique_ptr<View> DesignFitView::set_content(std::unique_ptr<View> content) {
    std::unique_ptr<View> prior;
    if (content_) prior = remove_child(content_);
    content_ = content.get();
    if (content) add_child(std::move(content));
    invalidate_layout();
    request_repaint();
    return prior;
}

std::unique_ptr<View> DesignFitView::take_content() {
    if (!content_) return {};
    auto prior = remove_child(content_);
    content_ = nullptr;
    invalidate_layout();
    request_repaint();
    return prior;
}

void DesignFitView::set_design_size(float w, float h) {
    design_w_ = w;
    design_h_ = h;
    invalidate_layout();
    request_repaint();
}

void DesignFitView::set_allow_upscale(bool allow) {
    if (allow_upscale_ == allow) return;
    allow_upscale_ = allow;
    invalidate_layout();
    request_repaint();
}

void DesignFitView::set_top_align(bool top_align) {
    if (top_align_ == top_align) return;
    top_align_ = top_align;
    invalidate_layout();
    request_repaint();
}

bool DesignFitView::resolve_design_size(float& w, float& h) const {
    if (design_w_ > 0.0f && design_h_ > 0.0f) {
        w = design_w_;
        h = design_h_;
        return true;
    }
    if (!content_) return false;
    // An imported design's root declares its authored size as a preferred
    // dimension; a measured widget declares it as an intrinsic one.
    w = content_->flex().preferred_width > 0.0f ? content_->flex().preferred_width
                                                : content_->intrinsic_width();
    h = content_->flex().preferred_height > 0.0f ? content_->flex().preferred_height
                                                 : content_->intrinsic_height();
    return w > 0.0f && h > 0.0f;
}

void DesignFitView::layout_children() {
    if (!content_) return;

    const Rect area = local_bounds();
    float design_w = 0.0f, design_h = 0.0f;
    float sx = 1.0f, sy = 1.0f, tx = 0.0f, ty = 0.0f;
    if (!resolve_design_size(design_w, design_h) ||
        !WindowHost::compute_design_viewport_transform(
            area.width, area.height, design_w, design_h, sx, sy, tx, ty,
            top_align_)) {
        // No authored size to fit (or no container yet): hand the content the
        // container verbatim, which is exactly what a plain flex parent does.
        fit_scale_ = 1.0f;
        offset_x_ = 0.0f;
        offset_y_ = 0.0f;
        content_->set_scale(1.0f);
        content_->set_bounds(area);
        content_->layout_children();
        return;
    }

    // sx == sy by construction — the fit is uniform, so the design letterboxes
    // rather than stretching.
    float scale = sx;
    if (!allow_upscale_ && scale > 1.0f) {
        // The design already fits. Leave it at authored size and re-center the
        // slack, rather than blowing it up to fill the container.
        scale = 1.0f;
        tx = (area.width - design_w) * 0.5f;
        ty = top_align_ ? 0.0f : (area.height - design_h) * 0.5f;
    }

    fit_scale_ = scale;
    offset_x_ = tx;
    offset_y_ = ty;

    // The content keeps its AUTHORED bounds and is scaled about its own
    // top-left: layout inside it still solves at design size (nothing reflows,
    // no text re-wraps) and only paint is scaled. Anchoring the scale at the
    // top-left — not the default center origin — is what makes the child's
    // bounds origin double as the letterbox offset, which is the mapping
    // `hit_test` below and `point_to_local` both invert.
    content_->set_transform_origin(0.0f, 0.0f);
    content_->set_scale(scale);
    content_->set_bounds({offset_x_, offset_y_, design_w, design_h});
    content_->layout_children();
}

View* DesignFitView::hit_test(Point local_point) {
    if (!visible() || !enabled() || !hit_testable()) return nullptr;
    if (pointer_events() == PointerEvents::none) return nullptr;

    if (content_ && content_->visible() &&
        pointer_events() != PointerEvents::box_only && fit_scale_ > 0.0f) {
        // Inverse of the transform layout_children applied. The base
        // View::hit_test walk subtracts a child's bounds origin but knows
        // nothing about `scale()`, so a fitted subtree would be hit-tested at
        // authored coordinates while it paints at fitted ones — clicks landing
        // on the wrong control, or on nothing.
        const Point design_pt{(local_point.x - offset_x_) / fit_scale_,
                              (local_point.y - offset_y_) / fit_scale_};
        if (content_->local_bounds().contains(design_pt)) {
            if (auto* hit = content_->hit_test(design_pt)) return hit;
        }
    }

    if (pointer_events() == PointerEvents::box_none) return nullptr;
    if (local_bounds().contains(local_point)) return this;
    return nullptr;
}

}  // namespace pulp::view
