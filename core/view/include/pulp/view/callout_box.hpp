#pragma once

/// @file callout_box.hpp
/// Anchored popover with a pointer triangle.
///
/// AnchoredCallout is a floating content box tethered to a target rect (a knob, a
/// button) by a little arrow. It auto-flips to the other side when it would clip
/// the window, clamps itself inside the window edges, and keeps the arrow pointing
/// at the anchor even after the body is clamped.
///
/// Named AnchoredCallout to avoid colliding with Pulp's existing `CallOutBox` in
/// ui_components.hpp, which is a modal message/confirm box with no anchor or arrow
/// — a different primitive, despite the similar name. Pulp also has ContextMenu (a
/// view-tree overlay) and TooltipWindow, but neither gives arbitrary child content
/// an arrowed, edge-aware anchor — that is what AnchoredCallout adds.
///
/// The placement math (`place_callout`) is a pure function of rects + a style, so
/// side selection / edge clamping / arrow tracking are fully unit-testable
/// headlessly; AnchoredCallout is the thin View that hosts content, computes the
/// placement against a window rect, and paints the rounded body + triangle.

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/geometry.hpp>
#include <pulp/view/view.hpp>

#include <functional>
#include <memory>

namespace pulp::view {

/// Which side of the anchor the callout BODY sits on. The arrow points from the
/// body toward the anchor, so `above` = body above the anchor with the arrow on
/// the body's bottom edge pointing down at it.
enum class CalloutSide { above, below, left_of, right_of };

/// Opposite side, used by the auto-flip.
CalloutSide opposite_side(CalloutSide s);

/// Tunables for the callout geometry. All lengths in the window coordinate space.
struct CalloutStyle {
    float arrow_length = 10.0f;   ///< how far the tip extends past the body edge toward the anchor
    float arrow_width = 18.0f;    ///< base width of the triangle along the body edge
    float margin = 8.0f;          ///< minimum gap kept between the body and the window edges
    float gap = 0.0f;             ///< extra gap between the anchor and the arrow tip
    float corner_radius = 8.0f;   ///< body corner radius (arrow base stays clear of the corners)
};

/// Resolved placement in the window coordinate space.
struct CalloutPlacement {
    Rect body{};                  ///< the content box
    CalloutSide side = CalloutSide::below;  ///< side chosen AFTER any auto-flip
    float arrow_tip_x = 0.0f;     ///< triangle tip (touches/points at the anchor)
    float arrow_tip_y = 0.0f;
    float arrow_base_x = 0.0f;    ///< center of the triangle base, on the body edge
    float arrow_base_y = 0.0f;
};

/// Place a `body_w`×`body_h` callout tethered to `anchor`, kept inside `window`.
/// Starts from `preferred`; if the body wouldn't fit on that side but WOULD on the
/// opposite side, flips. Centers the body on the anchor along the cross axis, then
/// clamps that cross axis inside the window margins. The arrow tip points at the
/// center of the anchor's facing edge, clamped so its base stays on the body edge
/// clear of the corner radii — so a body pushed sideways by clamping still points
/// back at the anchor. Robust for a tiny window (returns a best-effort box).
CalloutPlacement place_callout(const Rect& anchor, float body_w, float body_h,
                               const Rect& window, CalloutSide preferred,
                               const CalloutStyle& style = {});

/// A view-tree callout: hosts one content child, anchored to a target rect with a
/// pointer triangle, auto-flipping + edge-clamping against a window rect. Mirrors
/// the ContextMenu overlay idiom (full-window transparent overlay, outside-click /
/// Escape dismiss) but with arbitrary content and an arrow.
class AnchoredCallout : public View {
public:
    AnchoredCallout() { set_focusable(true); }

    /// Anchor rect in the window/overlay LOCAL coordinate space.
    void set_anchor(Rect anchor) { anchor_ = anchor; }
    Rect anchor() const { return anchor_; }

    void set_preferred_side(CalloutSide s) { preferred_ = s; }
    CalloutSide preferred_side() const { return preferred_; }

    void set_style(CalloutStyle style) { style_ = style; }
    const CalloutStyle& style() const { return style_; }

    /// The content box size (before the arrow / margins). Defaults to the content
    /// child's preferred size when one is attached; set explicitly to override.
    void set_content_size(float w, float h) { content_w_ = w; content_h_ = h; }

    /// Attach the content view (ownership moves to the callout). Positioned inside
    /// the body on layout.
    void set_content(std::unique_ptr<View> content);
    View* content() const { return content_; }

    /// Resolve the placement against `window` (typically this overlay's bounds).
    CalloutPlacement compute_placement(const Rect& window) const;

    /// Fired on Escape / outside-click dismiss.
    std::function<void()> on_dismiss;

    /// Mount as a full-window overlay child of `root`, anchored to `anchor`,
    /// hosting `content`. Returns a raw pointer (root owns it).
    static AnchoredCallout* show(View* root, Rect anchor, std::unique_ptr<View> content,
                            CalloutSide preferred = CalloutSide::below,
                            CalloutStyle style = {});

    void paint(canvas::Canvas& canvas) override;
    void layout_children() override;

private:
    Rect anchor_{};
    CalloutSide preferred_ = CalloutSide::below;
    CalloutStyle style_{};
    float content_w_ = 0.0f, content_h_ = 0.0f;
    View* content_ = nullptr;   ///< non-owning; owned by the View child list
    static constexpr float kBodyPadding = 12.0f;
};

}  // namespace pulp::view
