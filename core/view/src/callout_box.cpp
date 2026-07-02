#include <pulp/view/callout_box.hpp>

#include <pulp/view/input_events.hpp>

#include <algorithm>

namespace pulp::view {

CalloutSide opposite_side(CalloutSide s) {
    switch (s) {
        case CalloutSide::above:    return CalloutSide::below;
        case CalloutSide::below:    return CalloutSide::above;
        case CalloutSide::left_of:  return CalloutSide::right_of;
        case CalloutSide::right_of: return CalloutSide::left_of;
    }
    return CalloutSide::below;
}

namespace {
bool is_vertical(CalloutSide s) {
    return s == CalloutSide::above || s == CalloutSide::below;
}
}  // namespace

CalloutPlacement place_callout(const Rect& anchor, float body_w, float body_h,
                               const Rect& window, CalloutSide preferred,
                               const CalloutStyle& style) {
    const float reach = style.arrow_length + style.gap;

    // Does the body fit on side `s` without spilling past the window margin?
    auto fits = [&](CalloutSide s) -> bool {
        switch (s) {
            case CalloutSide::below:
                return anchor.bottom() + reach + body_h + style.margin <= window.bottom();
            case CalloutSide::above:
                return anchor.y - reach - body_h - style.margin >= window.y;
            case CalloutSide::right_of:
                return anchor.right() + reach + body_w + style.margin <= window.right();
            case CalloutSide::left_of:
                return anchor.x - reach - body_w - style.margin >= window.x;
        }
        return false;
    };

    // Auto-flip: only when the preferred side doesn't fit but the opposite does.
    CalloutSide side = preferred;
    if (!fits(preferred) && fits(opposite_side(preferred)))
        side = opposite_side(preferred);

    const Point ac = anchor.center();
    CalloutPlacement out;
    out.side = side;
    out.body.width = body_w;
    out.body.height = body_h;

    // Position the body: fixed on the main axis, centered on the anchor on the
    // cross axis.
    switch (side) {
        case CalloutSide::below:
            out.body.y = anchor.bottom() + reach;
            out.body.x = ac.x - body_w * 0.5f;
            break;
        case CalloutSide::above:
            out.body.y = anchor.y - reach - body_h;
            out.body.x = ac.x - body_w * 0.5f;
            break;
        case CalloutSide::right_of:
            out.body.x = anchor.right() + reach;
            out.body.y = ac.y - body_h * 0.5f;
            break;
        case CalloutSide::left_of:
            out.body.x = anchor.x - reach - body_w;
            out.body.y = ac.y - body_h * 0.5f;
            break;
    }

    // Clamp the CROSS axis inside the window margins (the main axis stays put so
    // the arrow keeps reaching the anchor). Guard against a window narrower than
    // the body: clamp() requires lo <= hi.
    if (is_vertical(side)) {
        const float lo = window.x + style.margin;
        const float hi = window.right() - style.margin - body_w;
        out.body.x = (hi >= lo) ? std::clamp(out.body.x, lo, hi) : lo;
    } else {
        const float lo = window.y + style.margin;
        const float hi = window.bottom() - style.margin - body_h;
        out.body.y = (hi >= lo) ? std::clamp(out.body.y, lo, hi) : lo;
    }

    // Arrow: base on the body edge facing the anchor, tip `arrow_length` beyond it
    // pointing at the anchor. Track the anchor center along the edge, clamped so
    // the triangle base stays on the flat part of the edge (clear of the corners).
    const float half_aw = style.arrow_width * 0.5f;
    if (is_vertical(side)) {
        const float lo = out.body.x + style.corner_radius + half_aw;
        const float hi = out.body.right() - style.corner_radius - half_aw;
        const float bx = (hi >= lo) ? std::clamp(ac.x, lo, hi)
                                    : (out.body.x + out.body.width * 0.5f);
        out.arrow_base_x = out.arrow_tip_x = bx;
        if (side == CalloutSide::below) {
            out.arrow_base_y = out.body.y;                 // top edge
            out.arrow_tip_y = out.body.y - style.arrow_length;
        } else {  // above
            out.arrow_base_y = out.body.bottom();          // bottom edge
            out.arrow_tip_y = out.body.bottom() + style.arrow_length;
        }
    } else {
        const float lo = out.body.y + style.corner_radius + half_aw;
        const float hi = out.body.bottom() - style.corner_radius - half_aw;
        const float by = (hi >= lo) ? std::clamp(ac.y, lo, hi)
                                    : (out.body.y + out.body.height * 0.5f);
        out.arrow_base_y = out.arrow_tip_y = by;
        if (side == CalloutSide::right_of) {
            out.arrow_base_x = out.body.x;                 // left edge
            out.arrow_tip_x = out.body.x - style.arrow_length;
        } else {  // left_of
            out.arrow_base_x = out.body.right();           // right edge
            out.arrow_tip_x = out.body.right() + style.arrow_length;
        }
    }
    return out;
}

// ── AnchoredCallout view ──────────────────────────────────────────────────────────

void AnchoredCallout::set_content(std::unique_ptr<View> content) {
    if (content_) remove_child(content_);
    content_ = content.get();
    if (content_) {
        if (content_w_ <= 0.0f) content_w_ = content_->flex().preferred_width;
        if (content_h_ <= 0.0f) content_h_ = content_->flex().preferred_height;
        add_child(std::move(content));
    }
}

CalloutPlacement AnchoredCallout::compute_placement(const Rect& window) const {
    const float body_w = content_w_ + kBodyPadding * 2.0f;
    const float body_h = content_h_ + kBodyPadding * 2.0f;
    return place_callout(anchor_, body_w, body_h, window, preferred_, style_);
}

void AnchoredCallout::layout_children() {
    if (!content_) return;
    const CalloutPlacement p = compute_placement(local_bounds());
    content_->set_bounds({p.body.x + kBodyPadding, p.body.y + kBodyPadding,
                          content_w_, content_h_});
}

void AnchoredCallout::paint(canvas::Canvas& canvas) {
    const CalloutPlacement p = compute_placement(local_bounds());
    const auto bg = resolve_color("modal.bg", canvas::Color::rgba8(30, 34, 40));
    const auto border = resolve_color("modal.border", canvas::Color::rgba8(60, 66, 74));

    // Body.
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(p.body.x, p.body.y, p.body.width, p.body.height,
                             style_.corner_radius);

    // Pointer triangle: base spans arrow_width along the body edge, apex at the
    // tip. Drawn as a filled path in the same body fill so it reads as one shape.
    const float half_aw = style_.arrow_width * 0.5f;
    canvas::Canvas::Point2D tri[3];
    if (p.side == CalloutSide::below || p.side == CalloutSide::above) {
        tri[0] = {p.arrow_base_x - half_aw, p.arrow_base_y};
        tri[1] = {p.arrow_base_x + half_aw, p.arrow_base_y};
        tri[2] = {p.arrow_tip_x, p.arrow_tip_y};
    } else {
        tri[0] = {p.arrow_base_x, p.arrow_base_y - half_aw};
        tri[1] = {p.arrow_base_x, p.arrow_base_y + half_aw};
        tri[2] = {p.arrow_tip_x, p.arrow_tip_y};
    }
    canvas.set_fill_color(bg);
    canvas.fill_path(tri, 3);

    // Thin border on the body (the arrow sits flush; keeping the border on the
    // body only is a deliberate simplification — matches the ContextMenu style).
    canvas.set_stroke_color(border);
    canvas.set_line_width(1.0f);
    canvas.stroke_rounded_rect(p.body.x, p.body.y, p.body.width, p.body.height,
                               style_.corner_radius);
}

AnchoredCallout* AnchoredCallout::show(View* root, Rect anchor, std::unique_ptr<View> content,
                             CalloutSide preferred, CalloutStyle style) {
    if (!root) return nullptr;
    auto box = std::make_unique<AnchoredCallout>();
    AnchoredCallout* raw = box.get();
    raw->set_bounds(root->local_bounds());
    raw->set_anchor(anchor);
    raw->set_preferred_side(preferred);
    raw->set_style(style);
    raw->set_content(std::move(content));
    root->add_child(std::move(box));
    raw->layout_children();
    return raw;
}

}  // namespace pulp::view
