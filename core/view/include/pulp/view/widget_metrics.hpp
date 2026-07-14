#pragma once

/// @file widget_metrics.hpp
/// A pluggable *sizing* delegate for Pulp's stock widgets — the sibling of
/// `WidgetPainter` (widget_painter.hpp).
///
/// A `WidgetPainter` answers "what pixels?". A `WidgetMetrics` answers
/// "how big?". They are deliberately separate objects with the same
/// installation and cascade rules, because the two questions have different
/// lifetimes: metrics are consulted during LAYOUT (possibly many times, with
/// no canvas and no pixels), painting happens once per frame afterwards.
/// Fusing them into one class would force every skin that only restyles to also
/// be dragged into the layout pass.
///
/// ## Installation and inheritance
///
/// `View::set_metrics(std::shared_ptr<WidgetMetrics>)`. Applies to that view
/// and its whole subtree; a widget uses the nearest delegate found by walking
/// up the parent chain (`View::effective_metrics()`). Identical cascade to
/// `effective_painter()`.
///
/// ## Partial override
///
/// Every hook is non-pure and returns `false` by default, meaning "I have no
/// opinion — use the widget's stock metric". A delegate therefore only sizes
/// the things it actually wants to size.
///
/// ## How this reaches the layout engine
///
/// Pulp lays out with a flex/grid engine that measures leaf views through
/// `View::intrinsic_width()` / `View::intrinsic_height()`. A widget that wants
/// its size delegated simply consults `effective_metrics()` from inside those
/// two overrides. There is no second seam and no parallel measure protocol: the
/// delegate feeds the SAME function the layout engine already calls. Widgets
/// that are not laid out by the engine at all — an overlay panel such as
/// `ContextMenu`, which positions itself from an anchor point — consult the
/// delegate directly when they compute their own box.
///
/// ## Text measurement
///
/// Metric hooks run during layout, where there is no canvas. Measure with
/// `pulp::canvas::global_text_shaper()`, which is the same shaper the painter
/// draws with, so a measured width and a drawn width cannot disagree.

#include <string>

#include <pulp/view/geometry.hpp>

namespace pulp::view {

class View;

/// A font request, as a delegate expresses it. `letter_spacing` is in ems, the
/// same unit `Label::set_letter_spacing` takes.
struct FontSpec {
    std::string family = "Inter";
    float size = 13.0f;
    int weight = 400;          ///< 100–900, 400 = regular
    float letter_spacing = 0.0f;
};

/// Per-side insets in pixels.
struct EdgeInsets {
    float top = 0.0f, right = 0.0f, bottom = 0.0f, left = 0.0f;
    float horizontal() const { return left + right; }
    float vertical() const { return top + bottom; }
};

/// What a menu row is, when the delegate is asked how big it should be.
struct MenuItemMetricsQuery {
    std::string text;
    bool separator = false;
    bool header = false;
    bool ticked = false;
    bool has_submenu = false;
    /// The height the widget would use if the delegate declines — passed so a
    /// delegate can honor a caller-requested row height instead of forcing
    /// its own.
    float standard_height = 0.0f;
};

/// A control's natural size. Either field may be left at 0 to mean "no opinion
/// on this axis"; the widget keeps its stock metric for that axis alone.
struct BoxSize {
    float width = 0.0f;
    float height = 0.0f;
};

/// Install on a View to supply the SIZES of the controls in its subtree.
/// Every hook declines by default.
class WidgetMetrics {
public:
    virtual ~WidgetMetrics() = default;

    // ── Menus ────────────────────────────────────────────────────────────
    //
    // A menu panel sizes itself from its rows: it asks for each row's natural
    // size, takes the widest, stacks the heights, and adds the border. All
    // three of those inputs are delegated, so a skin can make a menu wider,
    // denser, or thicker-bordered without the menu widget knowing why.

    virtual bool menu_item_size(const MenuItemMetricsQuery& query, BoxSize& out, View& source) {
        (void)query; (void)out; (void)source;
        return false;
    }

    /// Padding between the panel edge and the rows, all four sides.
    virtual bool menu_border(float& out, View& source) {
        (void)out; (void)source;
        return false;
    }

    virtual bool menu_font(FontSpec& out, View& source) {
        (void)out; (void)source;
        return false;
    }

    // ── Text-bearing controls ────────────────────────────────────────────

    virtual bool button_font(FontSpec& out, View& source) {
        (void)out; (void)source;
        return false;
    }

    virtual bool combo_box_font(FontSpec& out, View& source) {
        (void)out; (void)source;
        return false;
    }

    virtual bool text_field_font(FontSpec& out, View& source) {
        (void)out; (void)source;
        return false;
    }

    /// The gap between a text field's frame and its text — the sum of whatever
    /// the skin thinks of as its frame inset and its text indent. One hook,
    /// because the widget only ever needs the total.
    virtual bool text_field_insets(EdgeInsets& out, View& source) {
        (void)out; (void)source;
        return false;
    }

    /// Caret stroke width in pixels. Sub-pixel values are legal and render as
    /// an antialiased hairline.
    virtual bool caret_width(float& out, View& source) {
        (void)out; (void)source;
        return false;
    }
};

} // namespace pulp::view
