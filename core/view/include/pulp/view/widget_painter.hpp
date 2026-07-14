#pragma once

/// @file widget_painter.hpp
/// A pluggable paint delegate for Pulp's stock widgets.
///
/// A `WidgetPainter` replaces the *rendering* of a widget without replacing the
/// widget: the control keeps its own hit-testing, value engine, accessibility
/// node and event handling, and only the pixels are supplied by the delegate.
/// That is the difference between this and subclassing — a subclass owns
/// behavior; a painter owns look.
///
/// ## Installation and inheritance
///
/// Install with `View::set_painter(std::shared_ptr<WidgetPainter>)`. The
/// delegate applies to that view **and to its whole subtree**: a widget looks
/// for a painter on itself first, then walks up the parent chain and uses the
/// nearest one it finds (`View::effective_painter()`). A descendant that
/// installs its own painter overrides the ancestor's for its own subtree. This
/// is the same cascade shape as the typography inheritance in `View`
/// (`inheritable_text_color()` and friends) — set it once on a panel and every
/// control inside is skinned.
///
/// ## Partial override
///
/// Every hook is a non-pure virtual whose default returns `false`, meaning
/// "I did not paint this — use the widget's stock rendering". A delegate
/// therefore only overrides the controls it actually restyles; everything else
/// keeps Pulp's default look. Return `true` from a hook once you have drawn.
///
/// ## Coordinates and units
///
/// Every state struct is expressed in the widget's own LOCAL coordinates, with
/// the origin at the widget's top-left. Note the deliberate asymmetry between
/// the rotary and linear states: a rotary control's position is a **normalized
/// proportion** (the angle is derived from it), whereas a linear control's
/// position is in **pixels along the travel axis** (the track geometry, not the
/// value, is what a linear skin needs to place a thumb). Both forms are given
/// because a skin that had to reconstruct one from the other would have to
/// duplicate the widget's padding and thumb-size rules.
///
/// ## Colors
///
/// A delegate reads named color tokens off the source view with
/// `View::resolve_color(name, fallback)`, which walks the parent chain, and a
/// host installs per-view overrides with `View::set_color(name, color)`.

#include <string>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/geometry.hpp>

namespace pulp::view {

class View;

// ── Shared control state ─────────────────────────────────────────────────────

/// State every control hands to its painter.
struct ControlPaintState {
    Rect bounds{};             ///< paint area, LOCAL to the widget (origin 0,0)
    bool enabled = true;
    bool hovered = false;      ///< pointer is over the control
    bool pressed = false;      ///< pointer is down on the control
    bool focused = false;      ///< control holds keyboard focus
};

/// A rotary control. `position` is the normalized travel proportion in [0,1]
/// AFTER the response curve has been applied — i.e. it is what the pointer angle
/// should be derived from, not the raw value. `start_angle` / `end_angle` are in
/// radians in Pulp's screen convention (y-down, 0 = the +x axis, clockwise
/// positive — the same convention `painters::KnobStyle` documents); the pointer
/// angle is `start_angle + position * (end_angle - start_angle)`.
struct RotaryPaintState : ControlPaintState {
    float position = 0.0f;
    float start_angle = 0.0f;
    float end_angle = 0.0f;
    /// Real-world value and range, for skins that print the value.
    double value = 0.0;
    double value_min = 0.0;
    double value_max = 1.0;
};

/// A linear control (fader / slider / scroll thumb). All three positions are in
/// PIXELS along the travel axis: `track_min` is the coordinate of proportion 0,
/// `track_max` of proportion 1, and `thumb_pos` of the current proportion.
/// For a vertical control the travel axis is y and `track_min` is the BOTTOM
/// (proportion 0), so `track_min > track_max` there — a skin must not assume
/// the pair is sorted.
struct LinearPaintState : ControlPaintState {
    bool horizontal = true;
    float thumb_pos = 0.0f;
    float track_min = 0.0f;
    float track_max = 0.0f;
    float thumb_size = 0.0f;   ///< thumb extent along the travel axis, pixels
    double value = 0.0;
    double value_min = 0.0;
    double value_max = 1.0;
};

/// A push / toggle button. `background` is the color the widget itself would
/// have used, offered so a skin can tint rather than replace it.
struct ButtonPaintState : ControlPaintState {
    bool toggled = false;
    std::string text;
    canvas::Color background{};
};

/// A dropdown field (the closed control, not the open list).
struct ComboBoxPaintState : ControlPaintState {
    bool popup_open = false;
    std::string text;
    Rect arrow_zone{};   ///< the region reserved for the disclosure arrow
};

/// A menu panel's backing surface. `bounds` is the whole panel.
struct MenuBackgroundPaintState : ControlPaintState {};

/// One menu row. `bounds` is the row rect within the menu panel.
struct MenuItemPaintState : ControlPaintState {
    std::string text;
    bool separator = false;
    bool highlighted = false;   ///< pointer/keyboard cursor is on this row
    bool ticked = false;
    bool has_submenu = false;
};

/// A menu section title row.
struct MenuHeaderPaintState : ControlPaintState {
    std::string text;
};

/// A text field's frame. `bounds` is the whole field. Split into a background
/// hook and an outline hook because the two are independently restyled far more
/// often than they are restyled together: a field that wants a flat fill and no
/// frame overrides one and lets the other decline.
struct TextFieldPaintState : ControlPaintState {
    bool read_only = false;
    bool empty = true;             ///< no text (the placeholder is showing)
};

/// The text-insertion caret. `bounds` is the caret's cell — the full rect the
/// caret is allowed to occupy, at the field's line height. A skin that wants a
/// hairline draws a narrower rect inside it; `width` is the stroke the field's
/// metrics resolved to (see `WidgetMetrics::caret_width`), offered so a painter
/// and a metrics delegate that both restyle the caret cannot disagree.
struct CaretPaintState : ControlPaintState {
    float width = 1.0f;
};

// ── The delegate ─────────────────────────────────────────────────────────────

/// Install on a View to supply the pixels for the controls in its subtree.
/// Every hook defaults to declining (`false`), which leaves the widget's stock
/// rendering in place, so a delegate only implements what it restyles.
///
/// `source` is the widget being painted; a delegate can read its named colors
/// (`source.resolve_color(...)`) or downcast it for richer state.
class WidgetPainter {
public:
    virtual ~WidgetPainter() = default;

    virtual bool paint_rotary(canvas::Canvas& canvas, const RotaryPaintState& state,
                              View& source) {
        (void)canvas; (void)state; (void)source;
        return false;
    }

    virtual bool paint_linear(canvas::Canvas& canvas, const LinearPaintState& state,
                              View& source) {
        (void)canvas; (void)state; (void)source;
        return false;
    }

    virtual bool paint_button_background(canvas::Canvas& canvas, const ButtonPaintState& state,
                                         View& source) {
        (void)canvas; (void)state; (void)source;
        return false;
    }

    virtual bool paint_button_text(canvas::Canvas& canvas, const ButtonPaintState& state,
                                   View& source) {
        (void)canvas; (void)state; (void)source;
        return false;
    }

    virtual bool paint_combo_box(canvas::Canvas& canvas, const ComboBoxPaintState& state,
                                 View& source) {
        (void)canvas; (void)state; (void)source;
        return false;
    }

    virtual bool paint_scroll_bar(canvas::Canvas& canvas, const LinearPaintState& state,
                                  View& source) {
        (void)canvas; (void)state; (void)source;
        return false;
    }

    virtual bool paint_menu_background(canvas::Canvas& canvas, const MenuBackgroundPaintState& state,
                                       View& source) {
        (void)canvas; (void)state; (void)source;
        return false;
    }

    virtual bool paint_menu_item(canvas::Canvas& canvas, const MenuItemPaintState& state,
                                 View& source) {
        (void)canvas; (void)state; (void)source;
        return false;
    }

    virtual bool paint_menu_section_header(canvas::Canvas& canvas, const MenuHeaderPaintState& state,
                                           View& source) {
        (void)canvas; (void)state; (void)source;
        return false;
    }

    virtual bool paint_text_field_background(canvas::Canvas& canvas,
                                             const TextFieldPaintState& state, View& source) {
        (void)canvas; (void)state; (void)source;
        return false;
    }

    virtual bool paint_text_field_outline(canvas::Canvas& canvas,
                                          const TextFieldPaintState& state, View& source) {
        (void)canvas; (void)state; (void)source;
        return false;
    }

    virtual bool paint_caret(canvas::Canvas& canvas, const CaretPaintState& state, View& source) {
        (void)canvas; (void)state; (void)source;
        return false;
    }
};

} // namespace pulp::view
