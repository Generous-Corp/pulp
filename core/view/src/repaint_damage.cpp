#include <pulp/view/repaint_damage.hpp>

#include <pulp/canvas/view_effect.hpp>
#include <pulp/view/view.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

// FU-2 / WI-17 — pure partial-repaint damage model. See repaint_damage.hpp for
// the pixel-identity rule this implements. No GPU / canvas / platform state.

namespace pulp::view {
namespace {

// The maximum distance (px) a view's own drawing reaches BEYOND its box because
// it samples neighbours. Zero for point-wise views. Used to inflate a hazard's
// box before testing it against the damage: a hazard within `radius` of the
// damage boundary can pull the changed pixels into its output near that edge.
float sample_radius(const View& v) {
    float r = std::max(v.filter_blur(), v.backdrop_blur());
    for (const auto& op : v.filter_chain()) {
        switch (op.kind) {
            case View::FilterOp::Kind::blur:
                r = std::max(r, op.amount);
                break;
            case View::FilterOp::Kind::drop_shadow:
                // A drop-shadow reaches its offset PLUS its blur past the box.
                r = std::max(r, std::abs(op.ds_offset_x) + op.ds_blur);
                r = std::max(r, std::abs(op.ds_offset_y) + op.ds_blur);
                break;
            default:
                break;  // point-wise colour op — no spatial reach
        }
    }
    return r;
}

// True when `v` samples pixels at a distance — its output near a clip edge
// depends on content a bounded clip may not repaint. See the header for the
// full hazard catalogue. Render-transform / scroll-offset are handled in the
// walk (they make the box UNKNOWN rather than sampling per se).
bool is_sampling_hazard(const View& v) {
    if (v.backdrop_blur() > 0.0f) return true;
    if (v.filter_blur() > 0.0f) return true;
    if (!v.mask_image().empty() && v.mask_image() != "none") return true;
    if (v.effect() && v.effect()->needs_layer()) return true;
    // A filter chain is a hazard only when it carries a spatial (sampling) op; a
    // colour-only chain (brightness/contrast/grayscale/…) is point-wise + safe.
    for (const auto& op : v.filter_chain()) {
        if (op.kind == View::FilterOp::Kind::blur ||
            op.kind == View::FilterOp::Kind::drop_shadow)
            return true;
    }
    return false;
}

// Recursive walk. Returns true if any hazard forces a full repaint.
// `origin_x/y` is `v`'s accumulated root-space origin (the same summed-origin
// mapping paint applies via canvas.translate down the tree). `box_known` is
// false once any ancestor carried a render transform or a child-paint offset
// (scroll), which makes `v`'s painted box unknowable from the plain walk — a
// hazard with an unknown box is assumed to reach the damage.
bool subtree_forces_full(const View& v, float origin_x, float origin_y,
                         bool box_known, const Rect& requested) {
    // `v`'s OWN render transform makes its painted box unknowable from the plain
    // origin walk (a rotation paints outside the axis-aligned bounds), so a
    // transformed view's box is unknown for the hazard test on itself too.
    const bool self_box_known = box_known && !v.has_render_transform();

    if (is_sampling_hazard(v)) {
        if (!self_box_known) return true;  // unknown extent → assume it reaches
        const Rect box{origin_x, origin_y, v.bounds().width, v.bounds().height};
        if (box.expanded(sample_radius(v)).intersects(requested))
            return true;
    }

    // A scroll offset additionally makes the CHILDREN's boxes unknown (paint
    // translates them by -scroll); combined with the transform case above, any
    // hazard inside such a subtree escalates unconditionally.
    const bool child_box_known =
        self_box_known && !v.applies_child_paint_offset();
    for (std::size_t i = 0; i < v.child_count(); ++i) {
        const View* child = v.child_at(i);
        if (!child) continue;
        if (subtree_forces_full(*child,
                                origin_x + child->bounds().x,
                                origin_y + child->bounds().y,
                                child_box_known, requested))
            return true;
    }
    return false;
}

}  // namespace

DamageDecision compute_effective_damage(const View& root,
                                        const Rect& requested_root_rect,
                                        float device_scale) {
    // A degenerate request has nothing to bound — repaint in full (safe).
    if (requested_root_rect.is_empty())
        return {true, {}};

    // Walk the tree once from the root at origin (0,0). Any hazard whose
    // (inflated) box reaches the damage — or any hazard with an unknown box —
    // escalates to a full repaint.
    if (subtree_forces_full(root, 0.0f, 0.0f, /*box_known=*/true,
                            requested_root_rect))
        return {true, {}};

    // Snap the bounded clip OUT to the device-pixel grid so its edge lands on a
    // pixel boundary — no AA seam between the clipped repaint and the preserved
    // content. floor the top-left, ceil the bottom-right in device pixels.
    const float scale = device_scale > 0.0f ? device_scale : 1.0f;
    const float left = std::floor(requested_root_rect.x * scale) / scale;
    const float top = std::floor(requested_root_rect.y * scale) / scale;
    const float right = std::ceil(requested_root_rect.right() * scale) / scale;
    const float bottom = std::ceil(requested_root_rect.bottom() * scale) / scale;
    return {false, Rect{left, top, right - left, bottom - top}};
}

}  // namespace pulp::view
