#pragma once

#include <algorithm>
#include <pulp/view/css_animation.hpp>
#include <pulp/view/geometry.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/view_effect.hpp>
#include <optional>
#include <vector>
#include <memory>
#include <string>

namespace pulp::view {

class WindowHost;  // Forward declaration for View→Host back-reference
class PluginViewHost;

class FrameClock;

// Base class for all UI elements
// Views form a tree: each view has zero or more children and one optional parent
class View {
public:
    View() = default;
    virtual ~View() {
        // pulp #1148 — clear the overlay slot if this dying View holds it.
        // Without this, an unmounted React popover leaves a dangling
        // pointer that the platform window host would dereference on
        // the next click.
        if (active_overlay_ == this) active_overlay_ = nullptr;
        // pulp #1708 — clear the input-focus slot if this dying View holds
        // it. Without this, a React unmount of the focused widget (e.g.,
        // closing a Settings modal) leaves the platform host's focused-
        // view pointer dangling; the next keypress crashes via
        // dynamic_cast on freed memory in -[PulpView focusedTextEditor].
        if (focused_input_ == this) focused_input_ = nullptr;
    }

    View(const View&) = delete;
    View& operator=(const View&) = delete;

    // ── Bounds and layout ────────────────────────────────────────────────

    // The view's bounds in its parent's coordinate space
    Rect bounds() const { return bounds_; }
    void set_bounds(Rect r);

    // The view's local bounds (origin at 0,0)
    Rect local_bounds() const { return {0, 0, bounds_.width, bounds_.height}; }

    // Flex layout style
    FlexStyle& flex() { return flex_; }
    const FlexStyle& flex() const { return flex_; }

    GridStyle& grid() { return grid_; }
    const GridStyle& grid() const { return grid_; }

    void set_layout_mode(LayoutMode m) { layout_mode_ = m; }
    LayoutMode layout_mode() const { return layout_mode_; }

    // ── Child management ─────────────────────────────────────────────────

    void add_child(std::unique_ptr<View> child);
    std::unique_ptr<View> remove_child(View* child);
    size_t child_count() const { return children_.size(); }
    View* child_at(size_t index) { return children_[index].get(); }
    const View* child_at(size_t index) const { return children_[index].get(); }
    View* parent() const { return parent_; }

    // ── Hit testing ──────────────────────────────────────────────────────

    // Find the deepest child that contains the given point (in local coords)
    virtual View* hit_test(Point local_point);

    // ── Theme ────────────────────────────────────────────────────────────

    void set_theme(const Theme& theme) { theme_ = theme; }
    const Theme& theme() const { return theme_; }

    // Resolve a color: check own theme first, then walk up to parent
    Color resolve_color(const std::string& name, Color fallback = {}) const;

    // ── CSS-style typography inheritance (issue-969) ─────────────────────
    //
    // Mirrors CSS: setting `color: white` on a parent View cascades down
    // to every text descendant unless overridden. These fields are stored
    // on the View but DO NOT affect the View's own paint — they're picked
    // up by Label::paint() (and other text widgets) when the widget has
    // no explicit value of its own.
    //
    // The cascade order at paint time is:
    //   1. Widget's own explicit value (e.g. Label::set_font_size on this Label)
    //   2. inheritable_*() — walks up the parent chain returning the first
    //      ancestor that set the matching field
    //   3. Theme token / widget default fallback (existing behavior)
    //
    // text_align uses int rather than LabelAlign to keep View free of a
    // back-include of widgets.hpp; the int matches LabelAlign's enum
    // order: 0 = left, 1 = center, 2 = right.

    void set_inheritable_text_color(Color c) { inh_text_color_ = c; }
    void clear_inheritable_text_color() { inh_text_color_.reset(); }
    /// Walks own value, then parent chain. nullopt if no ancestor set it.
    std::optional<Color> inheritable_text_color() const;

    void set_inheritable_font_size(float size) { inh_font_size_ = size; }
    void clear_inheritable_font_size() { inh_font_size_.reset(); }
    std::optional<float> inheritable_font_size() const;

    void set_inheritable_letter_spacing(float sp) { inh_letter_spacing_ = sp; }
    void clear_inheritable_letter_spacing() { inh_letter_spacing_.reset(); }
    std::optional<float> inheritable_letter_spacing() const;

    void set_inheritable_font_weight(int w) { inh_font_weight_ = w; }
    void clear_inheritable_font_weight() { inh_font_weight_.reset(); }
    std::optional<int> inheritable_font_weight() const;

    /// pulp #1434 Phase A2-5 — inheritable font-family cascade. Mirrors
    /// the font-weight pattern; Labels read this when set_font_family
    /// hasn't been called directly. Final SkFontMgr resolution is
    /// gated on pulp #932 — the cascade plumbing is independent.
    void set_inheritable_font_family(std::string f) { inh_font_family_ = std::move(f); }
    void clear_inheritable_font_family() { inh_font_family_.reset(); }
    std::optional<std::string> inheritable_font_family() const;

    /// 0 = left, 1 = center, 2 = right (matches LabelAlign).
    void set_inheritable_text_align(int a) { inh_text_align_ = a; }
    void clear_inheritable_text_align() { inh_text_align_.reset(); }
    std::optional<int> inheritable_text_align() const;

    // ── Visibility ───────────────────────────────────────────────────────

    bool visible() const { return visible_; }
    void set_visible(bool v) { visible_ = v; }

    // ── Layout ───────────────────────────────────────────────────────────

    // Perform flex layout on children (virtual so ScrollView can override)
    virtual void layout_children();

    /// Intrinsic content size (override in widgets that know their natural size).
    /// Returns 0 if no intrinsic size (use preferred_width/height instead).
    virtual float intrinsic_width() const { return 0; }
    virtual float intrinsic_height() const;  // default: sum of visible children for containers

    // ── Painting ──────────────────────────────────────────────────────────

    // Paint this view and all children into a canvas
    virtual void paint_all(canvas::Canvas& canvas);

    // ── Lifecycle (override in subclasses) ────────────────────────────────

    virtual void paint(canvas::Canvas&) {}
    virtual void on_resized() {}
    virtual void on_attached() {}
    virtual void on_detached() {}

    // ── Input events (rich, with modifiers and pointer ID) ──────────────

    /// Mouse down with full event context.
    virtual void on_mouse_event(const MouseEvent& event) {
        if (on_pointer_event) on_pointer_event(event);
    }
    /// Key event with modifiers and up/down state.
    /// Return true if handled (prevents propagation to parent).
    virtual bool on_key_event(const KeyEvent& event) { (void)event; return false; }
    /// Text input from keyboard/IME (separate from key events).
    virtual void on_text_input(const TextInputEvent& event) { (void)event; }
    /// Called when this view gains or loses focus.
    /// Default implementation updates has_focus_ state. Subclasses should call base.
    virtual void on_focus_changed(bool gained) { has_focus_ = gained; }

    /// Gesture event (pinch/rotate from multi-touch or trackpad).
    virtual void on_gesture_event(const GestureEvent& event) {
        if (on_gesture_cb) on_gesture_cb(event);
    }

    // ── Pointer capture (W3C setPointerCapture) ─────────────────────────

    /// Capture pointer events for this view — all events for pointer_id
    /// route here regardless of hit-test until released.
    void set_pointer_capture(int pointer_id);
    void release_pointer_capture(int pointer_id);
    bool has_pointer_capture(int pointer_id) const;

    // ── Legacy event handlers (kept for backward compatibility) ──────────

    virtual void on_mouse_down(Point pos) { (void)pos; }
    virtual void on_mouse_up(Point pos) { (void)pos; }
    virtual void on_mouse_drag(Point pos) { (void)pos; }

    /// Pointer / touch cancellation (iOS touchesCancelled, etc.).
    /// Distinct from on_mouse_up so widgets can roll back in-progress
    /// gestures. Default forwards to on_mouse_up for back-compat.
    virtual void on_mouse_cancel(Point pos) { on_mouse_up(pos); }
    virtual void on_key_press(int key_code) { (void)key_code; }

    // ── Hover events ──────────────────────────────────────────────────────

    virtual void on_mouse_enter() {}
    virtual void on_mouse_leave() {}
    bool is_hovered() const { return hovered_; }
    void set_hovered(bool h);

    // ── Frame clock ─────────────────────────────────────────────────────

    /// Set the frame clock on the root view. Children access via frame_clock().
    void set_frame_clock(FrameClock* clock) { frame_clock_ = clock; }

    /// Get the frame clock (walks up parent chain to find it).
    FrameClock* frame_clock() const;

    // ── Theme dimension resolution ──────────────────────────────────────

    /// Resolve a dimension token: check own theme, walk up to parent.
    float resolve_dimension(const std::string& name, float fallback) const;

    // Dispatch a synthetic click to the deepest view at the given point
    void simulate_click(Point root_pos);

    // Dispatch a synthetic drag from start to end
    void simulate_drag(Point start, Point end, int steps = 10);

    // Dispatch a synthetic hover to the view at the given point
    void simulate_hover(Point root_pos);

    // ── Keyboard focus ───────────────────────────────────────────────────

    bool focusable() const { return focusable_; }
    void set_focusable(bool f) { focusable_ = f; }

    /// CSS :disabled equivalent — blocks input, reduces opacity
    bool enabled() const { return enabled_; }
    void set_enabled(bool e) { enabled_ = e; }

    /// CSS pointer-events: none — view is invisible to hit testing
    bool hit_testable() const { return hit_testable_; }
    void set_hit_testable(bool h) { hit_testable_ = h; }

    /// React Native pointerEvents (issue-1026). Four-valued enum that
    /// mirrors RN's contract:
    ///   auto      — default; this view AND children are interactive.
    ///   none      — neither this view NOR descendants intercept events.
    ///   box_only  — this view receives events; children do NOT.
    ///   box_none  — this view does NOT receive events but children do.
    /// hit_test() honors all four cases. set_hit_testable(false) is the
    /// legacy two-valued knob and is preserved by also short-circuiting
    /// hit_test() — set_pointer_events(PointerEvents::none) is the
    /// idiomatic RN-shaped equivalent.
    enum class PointerEvents { auto_, none, box_only, box_none };
    void set_pointer_events(PointerEvents p) { pointer_events_ = p; }
    PointerEvents pointer_events() const { return pointer_events_; }

    /// React Native backfaceVisibility (issue-1026). Stored on the View
    /// for plumbing parity with `@pulp/react`. The flag is consumed by
    /// the paint path only when a 3D transform with negative Z is
    /// active; pulp's transform model is currently 2D-affine, so this
    /// is reserved for future 3D support and behaves as a no-op for
    /// painting today.
    bool backface_visible() const { return backface_visible_; }
    void set_backface_visible(bool v) { backface_visible_ = v; }

    /// Mark layout as needing recalculation (auto-invalidation)
    void invalidate_layout() { layout_dirty_ = true; }
    bool layout_dirty() const { return layout_dirty_; }
    void clear_layout_dirty() { layout_dirty_ = false; }

    /// Request that this view's host invalidate and schedule a repaint.
    /// Calls `repaint()` on the attached `WindowHost` or `PluginViewHost`.
    /// Hosts are propagated to children on `add_child`, so any attached
    /// descendant sees its own host pointer; if the view tree has not
    /// been attached to a host yet, this is a no-op.
    ///
    /// This is the idiomatic wiring point for sub-bridges (e.g. a `View`
    /// that owns its own `WidgetBridge` for an `@pulp/react` editor) — the
    /// bridge's repaint callback is auto-wired to call this so JS-driven
    /// `requestAnimationFrame` callbacks reach the host's invalidator.
    /// See `WidgetBridge::set_repaint_callback` for the override path.
    void request_repaint();
    bool has_focus() const { return has_focus_; }
    void set_focus(bool f) { has_focus_ = f; }

    // Move focus to next/previous focusable widget
    static View* focus_next(View& root, View* current);
    static View* focus_prev(View& root, View* current);

    // ── Accessibility ────────────────────────────────────────────────────

    enum class AccessRole { none, slider, toggle, label, group, meter, image };

    void set_access_role(AccessRole role) { access_role_ = role; }
    AccessRole access_role() const { return access_role_; }

    void set_access_label(std::string label) { access_label_ = std::move(label); }
    const std::string& access_label() const { return access_label_; }

    void set_access_value(std::string value) { access_value_ = std::move(value); }
    const std::string& access_value() const { return access_value_; }

    // pulp #1737 — ARIA state attributes. Tri-state semantics per the
    // ARIA 1.2 spec (`true` / `false` / `mixed` / unset). We store the
    // raw string the JS shim hands us so platform AT bridges can read
    // it back verbatim (NSAccessibility's `accessibilityValue` for
    // toggle buttons reads from this slot; Linux AT-SPI / Windows UIA
    // bridges follow the same pattern when they land — tracked under
    // pulp #217). Setting the empty string clears the state.
    void set_access_pressed(std::string s)  { access_pressed_  = std::move(s); }
    void set_access_checked(std::string s)  { access_checked_  = std::move(s); }
    void set_access_disabled(std::string s) { access_disabled_ = std::move(s); }
    void set_access_hidden(std::string s)   { access_hidden_   = std::move(s); }
    const std::string& access_pressed()  const { return access_pressed_; }
    const std::string& access_checked()  const { return access_checked_; }
    const std::string& access_disabled() const { return access_disabled_; }
    const std::string& access_hidden()   const { return access_hidden_; }

    /// Called by platform accessibility when VoiceOver increment/decrement is triggered.
    /// Delta is typically ±0.05 (5% step). Override in widgets to adjust the value.
    virtual void on_accessibility_adjust(float delta) { (void)delta; }

    // ── Identity ─────────────────────────────────────────────────────────

    void set_id(std::string id) { id_ = std::move(id); }
    const std::string& id() const { return id_; }

    // ── Visual properties (CSS Box Model) ────────────────────────────────

    /// Opacity (0.0 = transparent, 1.0 = opaque). Applied as layer alpha.
    void set_opacity(float o) { opacity_ = std::clamp(o, 0.0f, 1.0f); }
    float opacity() const { return opacity_; }

    /// Background color (optional — if set, painted before children)
    void set_background_color(Color c) { bg_color_ = c; has_bg_ = true; }
    void clear_background_color() { has_bg_ = false; }
    bool has_background_color() const { return has_bg_; }
    Color background_color() const { return bg_color_; }

    /// CSS `background-repeat` keyword (pulp #1552). Storage-only at the
    /// View level: paint() of solid-color backgrounds (the only currently
    /// rendered case) is a no-op for repeat semantics, but the value is
    /// preserved so future paint logic for `background-image: url(...)` /
    /// repeating gradients can honor it without an API break. Mirrors
    /// the strategy used for `text-decoration-style` (pulp #1434 batch 3)
    /// which also stores the keyword and currently degrades to solid in
    /// paint. Accepted CSS keywords: `repeat`, `repeat-x`, `repeat-y`,
    /// `no-repeat`, `space`, `round`. Unknown / empty = `repeat` (CSS
    /// initial value).
    void set_background_repeat(std::string kw) { background_repeat_ = std::move(kw); }
    const std::string& background_repeat() const { return background_repeat_; }

    /// Border (optional — painted on top of background)
    void set_border(Color c, float width, float radius = 0) {
        border_color_ = c; border_width_ = width; corner_radius_ = radius; has_border_ = true;
    }
    void clear_border() { has_border_ = false; }
    bool has_border() const { return has_border_; }
    Color border_color() const { return border_color_; }
    float border_width() const { return border_width_; }
    float corner_radius() const { return corner_radius_; }

    /// Standalone border setters (issue-1026, RN parity). Each setter
    /// flips the has_border_ flag on so paint_all() actually emits the
    /// stroke even when set_border() was never called.
    void set_border_color(Color c) { border_color_ = c; has_border_ = true; }
    void set_border_width(float w) { border_width_ = w; has_border_ = true; }
    void set_border_radius(float r) { corner_radius_ = r; corner_radius_pct_ = 0; }
    /// pulp #1663 — set corner radius as percent of min(width,height).
    /// Resolved at paint time. Pass 0 to clear the percent and revert
    /// to the plain px slot.
    void set_border_radius_pct(float pct) { corner_radius_pct_ = pct; }
    float corner_radius_pct() const { return corner_radius_pct_; }

    /// pulp #1737 RN-OOS-fixup (#1812) — RN's `borderCurve`: corner
    /// shape selection. `circular` (default) is the standard quarter-
    /// circle rounded corner; `continuous` is the iOS-style super-
    /// ellipse / squircle approximation. View::paint_all picks the
    /// appropriate path generator based on this slot. Slot has no
    /// effect when border-radius is 0.
    enum class BorderCurve { circular, continuous };
    void set_border_curve(BorderCurve c) { border_curve_ = c; }
    BorderCurve border_curve() const     { return border_curve_; }
    /// Compute the effective uniform corner radius in px against the
    /// given bounds (called by paint code).
    float effective_corner_radius(float width, float height) const {
        if (corner_radius_pct_ > 0.0f) {
            return corner_radius_pct_ * 0.01f * std::min(width, height);
        }
        return corner_radius_;
    }

    /// CSS / RN border-style. pulp #1434 Triage #10 — Skia path effect
    /// dispatches on style at paint time. CG falls through to solid
    /// (the cg_canvas.mm path inherits the canvas-base no-op
    /// set_line_dash). Values that pulp doesn't render natively
    /// (`double` / `groove` / `ridge` / `inset` / `outset`) degrade to
    /// solid — documented as a paint-time gap in the catalog. `none` /
    /// `hidden` skip the stroke entirely (paint() short-circuits).
    enum class BorderStyle {
        solid,    ///< Default — single continuous line.
        dashed,   ///< SkDashPathEffect with 3w/3w on/off pattern.
        dotted,   ///< SkDashPathEffect with w/2w on/off pattern (round caps).
        double_,  ///< Two parallel lines — degrades to solid for now.
        groove,   ///< Carved-in look — degrades to solid for now.
        ridge,    ///< Raised look — degrades to solid for now.
        inset,    ///< 3D-shaded inset — degrades to solid for now.
        outset,   ///< 3D-shaded outset — degrades to solid for now.
        none,     ///< No border drawn (paint short-circuits).
        hidden,   ///< Same as none for paint purposes.
    };
    void set_border_style(BorderStyle s) { border_style_ = s; }
    BorderStyle border_style() const { return border_style_; }

    /// CSS / RN list-style cluster (pulp #1514). Pulp doesn't model
    /// HTML <li>/<ul>/<ol> semantics — these slots store the values
    /// the consumer set so an external paint pass (or future <li>
    /// semantic surface) can honor them. The bridge round-trips the
    /// keyword/url; paint-time marker rendering is the follow-up.
    enum class ListStyleType {
        none,     ///< No marker.
        disc,     ///< Filled circle (default for <ul>).
        circle,   ///< Hollow circle.
        square,   ///< Filled square.
        decimal,  ///< Numeric (default for <ol>) — needs sibling-index, not painted yet.
        // CSS Counter Styles Level 3 keywords (pulp #1514). Storage-only
        // today; paint-side glyph rendering is the follow-up. The slots
        // exist so the bridge can round-trip the consumer's keyword
        // honestly and the catalog can flip from `missing` to
        // `supported-with-gaps`. Underscored C++ identifiers map to
        // hyphenated CSS keywords (e.g. lower_roman ↔ "lower-roman").
        decimal_leading_zero,  ///< 01, 02, ... 09, 10, 11.
        lower_roman,           ///< i, ii, iii, iv, v.
        upper_roman,           ///< I, II, III, IV, V.
        lower_alpha,           ///< a, b, c — CSS alias of lower-latin.
        upper_alpha,           ///< A, B, C — CSS alias of upper-latin.
        lower_latin,           ///< a, b, c.
        upper_latin,           ///< A, B, C.
        lower_greek,           ///< α, β, γ.
        armenian,              ///< Traditional Armenian numbering.
        georgian,              ///< Traditional Georgian numbering.
    };
    enum class ListStylePosition {
        outside,  ///< Marker hangs in the margin (CSS default).
        inside,   ///< Marker is part of the content box.
    };
    void set_list_style_type(ListStyleType t) { list_style_type_ = t; }
    ListStyleType list_style_type() const { return list_style_type_; }
    void set_list_style_image(std::string url) { list_style_image_ = std::move(url); }
    const std::string& list_style_image() const { return list_style_image_; }
    void set_list_style_position(ListStylePosition p) { list_style_position_ = p; }
    ListStylePosition list_style_position() const { return list_style_position_; }

    /// CSS / RN outline cluster (pulp #1519). Outline is a paint-time
    /// ring drawn OUTSIDE the border-box; it does NOT affect Yoga layout
    /// (no parent space reserved). Slotting mirrors border-* but lives
    /// in its own quartet of fields so a JSX prop diff that touches one
    /// outline-* prop preserves the others. Reuses View::BorderStyle for
    /// the line-style enum since CSS keyword sets are identical (solid /
    /// dashed / dotted / double / groove / ridge / inset / outset / none /
    /// hidden). The Skia paint inflates the box by `outline_offset_ +
    /// outline_width_ / 2` and strokes; CG falls through (set_line_dash
    /// canvas-base no-op) for dashed/dotted same as border-style.
    void set_outline_color(Color c) { outline_color_ = c; }
    void set_outline_offset(float px) { outline_offset_ = px; }
    void set_outline_style(BorderStyle s) { outline_style_ = s; }
    void set_outline_width(float px) { outline_width_ = px; }
    Color outline_color() const { return outline_color_; }
    float outline_offset() const { return outline_offset_; }
    BorderStyle outline_style() const { return outline_style_; }
    float outline_width() const { return outline_width_; }


    /// Per-side borders (CSS border-top, border-right, etc.)
    /// pulp #1566 (Codex P2 follow-up to #1543) — track an explicit
    /// per-edge "set" flag so a width of 0 on one edge can override
    /// the uniform shorthand. CSS / RN semantics: `borderWidth: 10`
    /// followed by `borderTopWidth: 0` must yield a 0-px top border,
    /// not the 10-px shorthand. Without a per-edge `set` bit, the
    /// stored 0 was indistinguishable from "unset" in
    /// `apply_border_widths` and the shorthand leaked through.
    void set_border_top(Color c, float w) { border_top_ = {c, w}; border_top_set_ = true; has_border_sides_ = true; }
    void set_border_right(Color c, float w) { border_right_ = {c, w}; border_right_set_ = true; has_border_sides_ = true; }
    void set_border_bottom(Color c, float w) { border_bottom_ = {c, w}; border_bottom_set_ = true; has_border_sides_ = true; }
    void set_border_left(Color c, float w) { border_left_ = {c, w}; border_left_set_ = true; has_border_sides_ = true; }
    /// Color-only setters (pulp #1566). Setting `borderTopColor` alone
    /// must NOT mark the top edge's WIDTH as explicitly set — that
    /// would let a stale 0 override the uniform `borderWidth` shorthand.
    /// Mirrors CSS, where `border-top-color` and `border-top-width` are
    /// independent longhands.
    void set_border_top_color(Color c)    { border_top_.color = c;    has_border_sides_ = true; }
    void set_border_right_color(Color c)  { border_right_.color = c;  has_border_sides_ = true; }
    void set_border_bottom_color(Color c) { border_bottom_.color = c; has_border_sides_ = true; }
    void set_border_left_color(Color c)   { border_left_.color = c;   has_border_sides_ = true; }
    /// Width-only setters (pulp #1566). Setting `borderTopWidth` alone
    /// preserves the existing per-edge color and explicitly marks the
    /// width as set — including a width of 0, which then overrides any
    /// uniform shorthand on that edge.
    void set_border_top_width(float w)    { border_top_.width = w;    border_top_set_ = true;    has_border_sides_ = true; }
    void set_border_right_width(float w)  { border_right_.width = w;  border_right_set_ = true;  has_border_sides_ = true; }
    void set_border_bottom_width(float w) { border_bottom_.width = w; border_bottom_set_ = true; has_border_sides_ = true; }
    void set_border_left_width(float w)   { border_left_.width = w;   border_left_set_ = true;   has_border_sides_ = true; }
    /// Per-side getters (issue-1026). The standalone setBorderTop/Right/...
    /// {Color,Width} bridge calls need to preserve the unrelated attribute
    /// when only one is being changed by a JSX prop diff.
    Color border_top_color() const { return border_top_.color; }
    float border_top_width() const { return border_top_.width; }
    Color border_right_color() const { return border_right_.color; }
    float border_right_width() const { return border_right_.width; }
    Color border_bottom_color() const { return border_bottom_.color; }
    float border_bottom_width() const { return border_bottom_.width; }
    Color border_left_color() const { return border_left_.color; }
    float border_left_width() const { return border_left_.width; }
    bool has_border_sides() const { return has_border_sides_; }
    /// pulp #1566 — per-edge "explicitly set" probes. Yoga wiring uses
    /// these to distinguish "not set on this edge" (fall back to uniform
    /// `border_width()`) from "explicitly set to 0" (zero wins, even if
    /// the shorthand says 10).
    bool has_border_top_set() const { return border_top_set_; }
    bool has_border_right_set() const { return border_right_set_; }
    bool has_border_bottom_set() const { return border_bottom_set_; }
    bool has_border_left_set() const { return border_left_set_; }

    /// Per-corner border-radius (CSS border-top-left-radius, etc.)
    /// pulp #1171 (Codex P2 on #1044) — when transitioning from uniform
    /// `corner_radius_` to per-corner mode, seed the un-overridden
    /// corners from the uniform value so a sequence like
    /// `set_border_radius(10); set_corner_radius_tl(2);` renders as
    /// {2, 10, 10, 10} instead of {2, 0, 0, 0} (which silently
    /// discarded the uniform radius).
    void set_corner_radius_tl(float r) { promote_uniform_to_per_corner(); corner_radii_[0] = r; corner_radii_pct_[0] = 0; has_corner_radii_ = true; }
    void set_corner_radius_tr(float r) { promote_uniform_to_per_corner(); corner_radii_[1] = r; corner_radii_pct_[1] = 0; has_corner_radii_ = true; }
    void set_corner_radius_bl(float r) { promote_uniform_to_per_corner(); corner_radii_[2] = r; corner_radii_pct_[2] = 0; has_corner_radii_ = true; }
    void set_corner_radius_br(float r) { promote_uniform_to_per_corner(); corner_radii_[3] = r; corner_radii_pct_[3] = 0; has_corner_radii_ = true; }
    /// pulp #1663 — per-corner percent setters. Same semantics as
    /// set_border_radius_pct: paint time resolves to
    /// `pct * 0.01 * min(width, height)`.
    void set_corner_radius_tl_pct(float pct) { promote_uniform_to_per_corner(); corner_radii_pct_[0] = pct; has_corner_radii_ = true; }
    void set_corner_radius_tr_pct(float pct) { promote_uniform_to_per_corner(); corner_radii_pct_[1] = pct; has_corner_radii_ = true; }
    void set_corner_radius_bl_pct(float pct) { promote_uniform_to_per_corner(); corner_radii_pct_[2] = pct; has_corner_radii_ = true; }
    void set_corner_radius_br_pct(float pct) { promote_uniform_to_per_corner(); corner_radii_pct_[3] = pct; has_corner_radii_ = true; }
    float corner_radius_tl_pct() const { return corner_radii_pct_[0]; }
    float corner_radius_tr_pct() const { return corner_radii_pct_[1]; }
    float corner_radius_bl_pct() const { return corner_radii_pct_[2]; }
    float corner_radius_br_pct() const { return corner_radii_pct_[3]; }
    /// Compute effective per-corner radius (px) against given bounds.
    float effective_corner_radius_tl(float w, float h) const { return corner_radii_pct_[0] > 0 ? corner_radii_pct_[0] * 0.01f * std::min(w,h) : corner_radii_[0]; }
    float effective_corner_radius_tr(float w, float h) const { return corner_radii_pct_[1] > 0 ? corner_radii_pct_[1] * 0.01f * std::min(w,h) : corner_radii_[1]; }
    float effective_corner_radius_bl(float w, float h) const { return corner_radii_pct_[2] > 0 ? corner_radii_pct_[2] * 0.01f * std::min(w,h) : corner_radii_[2]; }
    float effective_corner_radius_br(float w, float h) const { return corner_radii_pct_[3] > 0 ? corner_radii_pct_[3] * 0.01f * std::min(w,h) : corner_radii_[3]; }
    /// Per-corner radius accessors. corner_radii_[0..3] = TL, TR, BL, BR.
    bool has_corner_radii() const { return has_corner_radii_; }
    float corner_radius_tl() const { return corner_radii_[0]; }
    float corner_radius_tr() const { return corner_radii_[1]; }
    float corner_radius_bl() const { return corner_radii_[2]; }
    float corner_radius_br() const { return corner_radii_[3]; }

    /// Box shadow (CSS-like: offset, blur, spread, color, inset).
    /// When `inset` is true, the shadow is drawn inside the box bounds (CSS
    /// `inset` keyword); otherwise a drop shadow extends outside the bounds.
    struct BoxShadow {
        float offset_x = 0, offset_y = 2;
        float blur = 4;
        float spread = 0;
        Color color{0, 0, 0, 80};
        bool inset = false;
    };
    void set_box_shadow(float ox, float oy, float blur, float spread, Color c,
                        bool inset = false) {
        shadow_ = {ox, oy, blur, spread, c, inset}; has_shadow_ = true;
    }
    void clear_box_shadow() { has_shadow_ = false; }
    bool has_box_shadow() const { return has_shadow_; }
    const BoxShadow& box_shadow() const { return shadow_; }

    /// pulp #1737 RN-OOS-fixup (audit 2026-05-11) — RN iOS-legacy
    /// shadow* longhand setters. RN 0.71+ added `boxShadow` as the
    /// cross-platform path (which Pulp fully supports), but the four
    /// per-attribute setters still appear in legacy code + the React
    /// Native API surface. Pulp mirrors the per-attribute pattern the
    /// text-shadow longhand uses (set_text_shadow_color / _offset /
    /// _radius — see widget_bridge.cpp:5368-5392): each setter mutates
    /// ONE field of the shared BoxShadow struct so a JSX prop diff
    /// that touches one prop doesn't clobber the others.
    ///
    /// Any of these turns has_shadow_ on (matches React Native's
    /// behavior — setting any shadow prop activates the shadow paint).
    void set_box_shadow_color(Color c) {
        shadow_.color = c; has_shadow_ = true;
    }
    void set_box_shadow_offset(float ox, float oy) {
        shadow_.offset_x = ox; shadow_.offset_y = oy; has_shadow_ = true;
    }
    void set_box_shadow_opacity(float a) {
        // RN's shadowOpacity is 0..1; multiply into the color's alpha
        // channel (preserves any explicit color alpha the author set
        // via shadowColor + still respects the 0..1 opacity slider).
        // Pulp Color stores alpha as 0..255 internally.
        shadow_.color.a = a; has_shadow_ = true;
    }
    void set_box_shadow_radius(float r) {
        shadow_.blur = r; has_shadow_ = true;
    }

    /// Generic click callback (fires on mouse-down, if set).
    std::function<void()> on_click;
    std::function<void(const MouseEvent&)> on_pointer_event;   ///< JS pointer event callback
    std::function<void(Point)> on_drag;   ///< JS pointermove during drag callback
    std::function<void(const GestureEvent&)> on_gesture_cb;    ///< JS gesture event callback

    /// Right-click context menu callback. If set, called on right-click with view-local coords.
    /// Return a list of menu items; an empty return suppresses the menu.
    std::function<void(Point position)> on_context_menu;

    /// Drop callback — fired when files or text are dropped on this view.
    /// Parameters: type ("file"/"text"), data (path or text), x, y
    std::function<void(const std::string& type, const std::string& data, float x, float y)> on_drop;

    /// Hover callbacks (CSS :hover equivalent). Fired by set_hovered().
    std::function<void()> on_hover_enter;
    std::function<void()> on_hover_leave;

    // ── Overlay painting ────────────────────────────────────────────────
    /// Deferred overlay paint callback. If set, called after the entire view
    /// tree is painted. The canvas is in root coordinates. Used by popups/dropdowns.
    struct OverlayRequest {
        std::function<void(canvas::Canvas&)> paint_fn;
        View* owner = nullptr;
    };
    static std::vector<OverlayRequest>& overlay_queue();
    static void paint_overlays(canvas::Canvas& canvas);

    /// Inspector hooks — set by the inspector module to intercept input and paint
    /// without a circular dependency (view doesn't link inspect).
    static void set_inspector_paint_hook(std::function<void(canvas::Canvas&)> hook);
    static void set_inspector_key_hook(std::function<bool(const KeyEvent&)> hook);
    static void set_inspector_mouse_hook(std::function<bool(const MouseEvent&)> hook);
    static bool call_inspector_key_hook(const KeyEvent& e);
    static bool call_inspector_mouse_hook(const MouseEvent& e);

    // ── Generalized overlay-click routing (pulp #1148) ───────────────────
    //
    // ComboBox already uses a `static ComboBox* active_popup_` pointer so
    // the platform window-host layer can short-circuit hit-testing for
    // paint-only overlays (the dropdown items render OUTSIDE the
    // ComboBox's hit_test bounds). React popovers built from
    // `<View position="absolute">` + nested buttons hit the same problem
    // but have no widget-specific shortcut, so clicks fall through to
    // whatever sibling/ancestor pixel is under the overlay.
    //
    // `active_overlay_` is the per-View opt-in equivalent: any View can
    // claim itself as the global click-eligible overlay. The window host
    // checks this AFTER `ComboBox::active_popup_` (so the ComboBox path
    // stays exact-as-was) and BEFORE the regular tree hit_test. If the
    // click falls inside the overlay's window-rect, it routes to the
    // overlay; otherwise the overlay is auto-released and the click
    // continues to the tree.
    //
    // The @pulp/react host config calls `claim_overlay()` from a JSX
    // `<View overlay>` mount and `release_overlay()` from its unmount.
    // The ComboBox path remains untouched and has its own state.
    static View* active_overlay_;
    void claim_overlay() { active_overlay_ = this; }
    /// pulp #1708 — global input-focus slot. The platform window host
    /// (PulpView on mac, equivalent on other platforms) calls
    /// `claim_input_focus()` when a click sets focus to a widget, and
    /// reads `focused_input_` for text-input dispatch. The View destructor
    /// auto-clears the slot if the focused widget is destroyed before
    /// focus is moved off it (e.g., React unmount of an open modal).
    /// Without this, the host's raw View* pointer dangles and the next
    /// keypress segfaults inside dynamic_cast<TextEditor*>(focused).
    static View* focused_input_;
    void claim_input_focus() { focused_input_ = this; }
    void release_input_focus() {
        if (focused_input_ == this) focused_input_ = nullptr;
    }
    /// Clear the global overlay if (and only if) `this` currently holds it.
    /// Idempotent — safe to call on unmount even if claim never happened.
    /// Does NOT fire `on_overlay_dismissed` — used by JSX unmount and the
    /// View destructor where React already knows the popover is closing.
    void release_overlay() {
        if (active_overlay_ == this) active_overlay_ = nullptr;
    }
    /// pulp #1361 — dismiss-path release. Releases the active overlay
    /// (if any) AND fires its `on_overlay_dismissed` callback so React
    /// state can flip `setOpen(false)` to keep the JSX tree in sync.
    /// Called by the platform window host from the ESC keypath and the
    /// outside-click path. No-op if nothing claimed the slot.
    static void dismiss_active_overlay();
    /// pulp #1361 — fired when the active overlay is dismissed via a
    /// framework auto-dismissal path (ESC / outside-click). NOT fired
    /// for `release_overlay()` (JSX unmount / destructor). The bridge
    /// uses this to dispatch `__dispatch__(id, 'dismiss', 0)` so React
    /// `<View overlay onDismissed>` consumers can sync state.
    std::function<void()> on_overlay_dismissed;
    /// Bounds-test in window (root) coordinates. Walks the parent chain
    /// to compute absolute origin and adds local_bounds(). Mirrors the
    /// arithmetic the mac mouseDown path uses for the ComboBox dropdown.
    bool overlay_contains(Point window_pt) const;

    /// Global click callback (fires on any view click with widget id). Set on root.
    std::function<void(const std::string& id, uint16_t modifiers)> on_global_click;

    /// Global key callback. If set on root, called before normal key dispatch.
    /// Return true to consume the event.
    std::function<bool(const KeyEvent&)> on_global_key;

    /// CSS position property
    enum class Position { static_, relative, absolute, fixed, sticky };
    void set_position(Position p) { position_ = p; }
    Position position() const { return position_; }

    // pulp #1434 batch 6 — top/right/bottom/left accept either a px
     // value (the historical path, single-arg setter) or a percent value
     // (new, two-arg setter that records the unit). The Yoga adapter
     // dispatches on `top_unit_` / etc. and routes percent values through
     // YGNodeStyleSetPositionPercent. Mirrors the FlexStyle::dim_width
     // pattern from pulp #1423 (PR #1426) for the View positional fields.
    void set_top(float v) { top_ = v; has_top_ = true; top_unit_ = DimensionUnit::px; }
    void set_right(float v) { right_ = v; has_right_ = true; right_unit_ = DimensionUnit::px; }
    void set_bottom(float v) { bottom_ = v; has_bottom_ = true; bottom_unit_ = DimensionUnit::px; }
    void set_left(float v) { left_ = v; has_left_ = true; left_unit_ = DimensionUnit::px; }
    void set_top(float v, DimensionUnit unit) { top_ = v; has_top_ = true; top_unit_ = unit; }
    void set_right(float v, DimensionUnit unit) { right_ = v; has_right_ = true; right_unit_ = unit; }
    void set_bottom(float v, DimensionUnit unit) { bottom_ = v; has_bottom_ = true; bottom_unit_ = unit; }
    void set_left(float v, DimensionUnit unit) { left_ = v; has_left_ = true; left_unit_ = unit; }
    float top() const { return top_; }
    float right() const { return right_; }
    float bottom() const { return bottom_; }
    float left() const { return left_; }
    DimensionUnit top_unit() const { return top_unit_; }
    DimensionUnit right_unit() const { return right_unit_; }
    DimensionUnit bottom_unit() const { return bottom_unit_; }
    DimensionUnit left_unit() const { return left_unit_; }
    bool has_top() const { return has_top_; }
    bool has_right() const { return has_right_; }
    bool has_bottom() const { return has_bottom_; }
    bool has_left() const { return has_left_; }
    void set_z_index(int z) { z_index_ = z; }
    int z_index() const { return z_index_; }

    /// pulp #972 — return children stably sorted by z_index() ascending.
    /// paint_all paints in this order so higher-z siblings render on top;
    /// hit_test walks the same order in reverse. Exposed so tests can
    /// assert ordering directly without piping through the paint pipeline.
    std::vector<View*> sorted_children_by_z_index() const;

    /// Overflow mode
    /// `scroll` accepted as a third keyword so the yoga `overflow`
    /// compat entry covers all 3 spec values. Paint clipping treats
    /// `scroll` like `hidden` (no scrollbar UI yet); the Yoga layout
    /// path forwards the enum through `YGNodeStyleSetOverflow` so the
    /// engine knows about it for descendant-overflow measurement.
    enum class Overflow { hidden, visible, scroll };
    void set_overflow(Overflow o) { overflow_ = o; }
    Overflow overflow() const { return overflow_; }

    /// CSS transform properties
    void set_scale(float s) { scale_ = s; }
    float scale() const { return scale_; }

    void set_translate(float x, float y) { translate_x_ = x; translate_y_ = y; }
    float translate_x() const { return translate_x_; }
    float translate_y() const { return translate_y_; }

    void set_rotation(float deg) { rotation_deg_ = deg; }
    float rotation() const { return rotation_deg_; }

    void set_skew(float x_deg, float y_deg) { skew_x_ = x_deg; skew_y_ = y_deg; }

    /// Transform origin (0-1 normalized, default 0.5,0.5 = center).
    /// pulp #1026 — also tracks an "explicitly set" flag so the affine
    /// matrix path (issue-930 setTransform) only honors the origin when
    /// the caller has actively chosen one. Without this, every existing
    /// setTransform() call site would silently start anchoring at center.
    void set_transform_origin(float x, float y) {
        origin_x_ = x; origin_y_ = y; origin_explicit_ = true;
    }
    float transform_origin_x() const { return origin_x_; }
    float transform_origin_y() const { return origin_y_; }
    bool transform_origin_explicit() const { return origin_explicit_; }

    /// Full 2D affine transform matrix on the View (issue-930). Mirrors the
    /// CanvasRenderingContext2D.setTransform contract:
    ///   [ a c e ]
    ///   [ b d f ]
    ///   [ 0 0 1 ]
    /// Applied in paint_all() after positioning the canvas at bounds_.{x,y}
    /// but before painting any background, border, or children — so the
    /// matrix multiplies onto the current canvas transform rather than
    /// replacing it (parent transforms still compose). Layout is unaffected:
    /// transforms are paint-only, hit-testing and Yoga still see un-transformed
    /// bounds.
    void set_transform_matrix(float a, float b, float c,
                              float d, float e, float f) {
        transform_matrix_a_ = a;
        transform_matrix_b_ = b;
        transform_matrix_c_ = c;
        transform_matrix_d_ = d;
        transform_matrix_e_ = e;
        transform_matrix_f_ = f;
        has_transform_matrix_ = true;
    }
    void clear_transform_matrix() { has_transform_matrix_ = false; }
    bool has_transform_matrix() const { return has_transform_matrix_; }
    /// Returns the six affine components in (a,b,c,d,e,f) order; meaningful
    /// only when has_transform_matrix() is true.
    void get_transform_matrix(float& a, float& b, float& c,
                              float& d, float& e, float& f) const {
        a = transform_matrix_a_; b = transform_matrix_b_; c = transform_matrix_c_;
        d = transform_matrix_d_; e = transform_matrix_e_; f = transform_matrix_f_;
    }

    /// CSS filter: blur(px) — per-element blur. Legacy single-blur slot
    /// kept for API compatibility; the richer filter chain (Phase A2-4)
    /// lives in `filter_chain_` below and supersedes this when non-empty.
    void set_filter_blur(float radius) { filter_blur_ = radius; }
    float filter_blur() const { return filter_blur_; }

    /// pulp #1434 Phase A2-4 — full CSS filter chain. Each entry is one
    /// filter function (blur / brightness / contrast / grayscale /
    /// hue-rotate / invert / opacity / saturate / sepia / drop-shadow).
    /// The Skia path walks the chain and composes via
    /// `SkImageFilters::Compose` + color-matrix wraps; CG falls back to
    /// blur-only. When the chain is empty, the legacy `filter_blur_`
    /// path stays in effect (back-compat for callers still using
    /// `set_filter_blur` directly).
    struct FilterOp {
        enum class Kind {
            blur,
            brightness,
            contrast,
            grayscale,
            hue_rotate,
            invert,
            opacity,
            saturate,
            sepia,
            drop_shadow,
        };
        Kind kind = Kind::blur;
        float amount = 0.0f;       ///< blur radius in px (blur), 0..1+ amount otherwise
        float angle_deg = 0.0f;    ///< hue-rotate only
        // drop-shadow extras
        float ds_offset_x = 0.0f;
        float ds_offset_y = 0.0f;
        float ds_blur = 0.0f;
        Color ds_color{};
    };
    void set_filter_chain(std::vector<FilterOp> chain) { filter_chain_ = std::move(chain); }
    void clear_filter_chain() { filter_chain_.clear(); }
    const std::vector<FilterOp>& filter_chain() const { return filter_chain_; }
    bool has_filter_chain() const { return !filter_chain_.empty(); }

    /// pulp #1434 Phase A2-1 — CSS transitions + animations.
    /// `set_transitions` accepts the parsed shorthand; `transitions()`
    /// returns the slice for the dispatcher to consult when a property
    /// changes. Per the CSS spec, the matching is a linear walk —
    /// later entries win when properties overlap (CSS cascade order).
    /// PR 2 of the ladder hooks the dispatcher to the rAF idle pump
    /// and starts driving Animation instances; PR 1 establishes the
    /// storage shape so downstream PRs can land independently.
    void set_transitions(std::vector<TransitionSpec> ts) { transitions_ = std::move(ts); }
    void clear_transitions() { transitions_.clear(); }
    const std::vector<TransitionSpec>& transitions() const { return transitions_; }
    bool has_transitions() const { return !transitions_.empty(); }

    /// Find the matching transition for a given property name. Returns
    /// nullptr if no transition applies (CSS spec: the snap path —
    /// commit the new value immediately). `'all'` entries match any
    /// property; named entries match only that exact property.
    const TransitionSpec* find_transition_for(const std::string& property) const {
        const TransitionSpec* match = nullptr;
        for (const auto& t : transitions_) {
            if (t.property_name == "all") match = &t;
            if (t.property_name == property) { match = &t; }
        }
        return match;
    }

    /// Active running animations on this View. Owned here so the
    /// per-View lifetime matches the View's. PR 2 wires the
    /// dispatcher to mutate this list; PR 1 establishes ownership.
    std::vector<CssAnimation>& active_animations() { return active_animations_; }
    const std::vector<CssAnimation>& active_animations() const { return active_animations_; }

    /// Advance every active CSS animation by `dt` seconds (pulp #1434
    /// Wave 3 css.3 — animation-play-state playback driver pause /
    /// resume). When `animation_play_state_ == "paused"`, the call is
    /// a no-op so the animations' `elapsed_seconds` does not advance —
    /// the spec semantic of `animation-play-state: paused`. The default
    /// keyword is `running`; any value other than `paused` advances.
    /// Returns the number of animations that finished this tick (i.e.
    /// flipped `active` to false). The caller is responsible for
    /// committing finished animations; this method only ticks the
    /// timeline.
    int tick_animations(float dt) {
        if (animation_play_state_ == "paused") return 0;
        int finished = 0;
        for (auto& a : active_animations_) {
            if (!a.active) continue;
            const bool was_active = a.active;
            a.tick(dt);
            if (was_active && !a.active) ++finished;
        }
        return finished;
    }

    /// Staged CSS animation control tokens (pulp #1434 — Codex audit on
    /// PR #1508). The web-compat-style-decl shim invokes
    /// `setAnimation(id, "name"|"duration"|"easing"|..., value)` one
    /// control-token at a time — one CSS longhand per call. We
    /// accumulate that state here; when the `name` token resolves
    /// against the keyframes registry, the bridge seeds entries into
    /// `active_animations_` using these accumulated values. Without
    /// this slot the legacy ABI silently drops every web-compat
    /// animation property.
    struct StagedAnimation {
        std::string name;
        float duration_seconds = 0.0f;
        float delay_seconds = 0.0f;
        CssEasing easing{};
        float iterations = 1.0f;
        std::string direction = "normal";
        std::string fill_mode;
    };
    StagedAnimation& staged_animation() { return staged_animation_; }
    const StagedAnimation& staged_animation() const { return staged_animation_; }

    /// CSS backdrop-filter: blur(px) — frosted-glass blur applied to whatever
    /// is behind this View when it paints (issue-926). Zero == no backdrop
    /// filter. Skia maps to `saveLayer(SaveLayerRec{ .fBackdrop = Blur })`.
    void set_backdrop_blur(float radius) { backdrop_blur_ = radius; }
    float backdrop_blur() const { return backdrop_blur_; }

    /// CSS `clip-path: path("...")` (pulp #1515). Stores an SVG-path-d
    /// string; paint_all() installs it as a canvas clip via
    /// `Canvas::clip_path_svg` before painting children. Empty string
    /// clears the slot. URL refs (`url(#id)`) and named shape forms
    /// (`circle()`, `inset()`, `polygon()`) are deferred — only the
    /// `path("...")` form is honored today.
    void set_clip_path(const std::string& svg_path_d) { clip_path_ = svg_path_d; }
    const std::string& clip_path() const { return clip_path_; }
    bool has_clip_path() const { return !clip_path_.empty(); }

    /// CSS `mask-image: ...` (pulp #1515). Storage-only today; the
    /// paint pipeline does not yet composite a shader mask onto a
    /// saveLayer (image-URL fetch + SkBlendMode::kDstIn is the
    /// follow-up paint slice). Solid-color and `none` are tracked so
    /// the value round-trips through View slots and the bridge.
    void set_mask_image(const std::string& value) { mask_image_ = value; }
    const std::string& mask_image() const { return mask_image_; }

    /// CSS `mask` shorthand (pulp #1515). Stored verbatim alongside
    /// `mask_image_`; the longhand fan-out in the JS shim populates
    /// the image slot from this string. Storage-only today.
    void set_mask(const std::string& value) { mask_ = value; }
    const std::string& mask() const { return mask_; }

    /// CSS `mask-size` (pairs with mask-image — pulp #1515 followup).
    /// Stored alongside mask_image_; consumed by the paint slice that
    /// composites the mask shader. Honored values: `auto` (default),
    /// `cover`, `contain`, `<length>`, `<length> <length>`.
    void set_mask_size(const std::string& value) { mask_size_ = value; }
    const std::string& mask_size() const { return mask_size_; }

    /// CSS `appearance` — controls native form-widget rendering.
    /// Pulp paints all widgets custom (no native form widgets), so
    /// this property is observably storage-only — `none` is the
    /// effective default for every Pulp View regardless of slot value.
    /// The slot exists so authors who set `appearance: none` for
    /// reset-style consistency see a no-op (not an unsupported drop)
    /// and the value round-trips through CSSStyleDeclaration.
    void set_appearance(const std::string& value) { appearance_ = value; }
    const std::string& appearance() const { return appearance_; }

    /// CSS `object-fit` — controls how an `<img>` content's intrinsic
    /// size is fitted into its layout box. Storage-only today; the
    /// ImageView paint slice that consumes this needs access to the
    /// decoded image's natural size (planned follow-up). Honored
    /// values in the future paint slice: `fill` (default — current
    /// behavior of stretching to bounds), `contain`, `cover`, `none`,
    /// `scale-down`.
    void set_object_fit(const std::string& value) { object_fit_ = value; }
    const std::string& object_fit() const { return object_fit_; }

    /// CSS `object-position` — alignment of the object inside its
    /// content box when object-fit leaves blank space (e.g. `contain`
    /// in a wider box). Storage-only today; pairs with object-fit.
    void set_object_position(const std::string& value) { object_position_ = value; }
    const std::string& object_position() const { return object_position_; }

    /// CSS background sub-properties (pulp #1517). These slots store the
    /// keyword for round-tripping; paint impact is partial — see notes:
    ///   • background-attachment: only `scroll` is conformant in pulp's
    ///     non-scrolling layout model. `fixed` / `local` need a scroll-
    ///     context coupling we don't have. Catalog: noop.
    ///   • background-clip: `text` would clip the bg paint to text glyphs
    ///     via SkBlendMode::kSrcIn — deferred to a later PR. Other values
    ///     are no-ops on solid bg. Catalog: partial.
    ///   • background-origin: relevant only for repeating gradients (which
    ///     we don't paint per-tile). Catalog: noop.
    void set_background_attachment(std::string kw) { background_attachment_ = std::move(kw); }
    const std::string& background_attachment() const { return background_attachment_; }
    void set_background_clip(std::string kw)       { background_clip_ = std::move(kw); }
    const std::string& background_clip() const     { return background_clip_; }
    void set_background_origin(std::string kw)     { background_origin_ = std::move(kw); }
    const std::string& background_origin() const   { return background_origin_; }

    /// CSS background-position / background-size (Wave 5 css.5). Both are
    /// storage-only slots today: pulp's solid-bg paint path doesn't honor
    /// position or size offsets (these only matter for url()/image-set()
    /// raster backgrounds, which are deferred to the image-loader slice
    /// — see backgroundImage catalog notes). Storing the keyword keeps
    /// the round-trip honest (set → get returns the literal string the
    /// CSS author wrote) so a future image-pipeline PR can honor the
    /// existing value without a JS-side change.
    void set_background_position(std::string kw)   { background_position_ = std::move(kw); }
    const std::string& background_position() const { return background_position_; }
    void set_background_size(std::string kw)       { background_size_ = std::move(kw); }
    const std::string& background_size() const     { return background_size_; }

    // pulp #1434 A4 Bundles 5–7 closure — storage-only slots for CSS
    // properties closed in the css NOT-IMPL sweep. Each slot is round-
    // trippable so harness tests can verify the bridge accepts the
    // value, and so a future paint-time slice can honor it without a
    // JS-side change. Catalog status documents the implementation
    // depth (`partial` / `noop` / `wontfix`).
    void set_text_indent(float v)                  { text_indent_ = v; }
    float text_indent() const                      { return text_indent_; }
    void set_word_break(std::string kw)            { word_break_ = std::move(kw); }
    const std::string& word_break() const          { return word_break_; }

    /// pulp #1737 RN-OOS-fixup (audit 2026-05-11) — CSS scroll-behavior
    /// + overscroll-behavior. Slot storage on every View; consumed by
    /// ScrollView::scroll_by + clamp_scroll_targets (`auto` triggers
    /// instant scroll, `smooth` enables animated scroll; `contain`/
    /// `none` for overscroll match Pulp's existing "clamp at content
    /// bounds, no scroll chaining" behavior). Non-ScrollView views
    /// round-trip the value but the paint pipeline doesn't act on it
    /// — matches CSS semantics: a non-scrollable element ignores
    /// scroll-related properties.
    void set_scroll_behavior(std::string kw)       { scroll_behavior_ = std::move(kw); }
    const std::string& scroll_behavior() const     { return scroll_behavior_; }
    void set_overscroll_behavior(std::string kw)   { overscroll_behavior_ = std::move(kw); }
    const std::string& overscroll_behavior() const { return overscroll_behavior_; }

    /// pulp #1737 RN-OOS-fixup (final sweep) — RN's Android-only
    /// `includeFontPadding`. Pure round-trip storage: Pulp's text-
    /// shaping pipeline doesn't add Android's vestigial vertical
    /// padding regardless of this value, so the bridge accepts the
    /// keyword + stores it (`el.style.includeFontPadding === false`
    /// reads back) and the paint pipeline ignores it. See compat.json
    /// rn/includeFontPadding notes for the honest CSS-subset rationale.
    void set_include_font_padding(bool v) { include_font_padding_ = v; }
    bool include_font_padding() const     { return include_font_padding_; }

    void set_font_variant(std::string kw)          { font_variant_ = std::move(kw); }
    const std::string& font_variant() const        { return font_variant_; }
    void set_writing_mode(std::string kw)          { writing_mode_ = std::move(kw); }
    const std::string& writing_mode() const        { return writing_mode_; }
    void set_isolation(std::string kw)             { isolation_ = std::move(kw); }
    const std::string& isolation() const           { return isolation_; }
    void set_resize(std::string kw)                { resize_ = std::move(kw); }
    const std::string& resize() const              { return resize_; }
    /// Animation play-state. Stored on the staged_animation slot via the
    /// existing setAnimation control-token ABI; this duplicate slot is
    /// for direct round-trip queries.
    void set_animation_play_state(std::string kw)  { animation_play_state_ = std::move(kw); }
    const std::string& animation_play_state() const { return animation_play_state_; }

    /// pulp #1548 — RN textShadow per-attribute storage. Storage-only;
    /// SkPaint shadow integration is the deferred paint-time slice.
    /// Each slot is round-trippable so a commitUpdate that touches only
    /// one of the three preserves the others (mirrors the per-side
    /// border slot pattern). A future combined `set_text_shadow`
    /// helper for the CSS shorthand path can write all three slots
    /// atomically.
    void set_text_shadow_color(std::string c)      { text_shadow_color_ = std::move(c); }
    const std::string& text_shadow_color() const   { return text_shadow_color_; }
    void set_text_shadow_offset(float dx, float dy){ text_shadow_dx_ = dx; text_shadow_dy_ = dy; }
    float text_shadow_offset_x() const             { return text_shadow_dx_; }
    float text_shadow_offset_y() const             { return text_shadow_dy_; }
    void set_text_shadow_radius(float r)           { text_shadow_radius_ = r; }
    float text_shadow_radius() const               { return text_shadow_radius_; }


    /// Force this View's subtree to render into a compositing layer.
    /// Useful for caching, post-effects, or explicit layer isolation.
    void set_needs_layer(bool v) { needs_layer_ = v; }
    bool needs_layer() const { return needs_layer_; }

    /// Attach a GPU post-processing effect to this View's compositing layer.
    void set_effect(std::shared_ptr<canvas::ViewEffect> effect) { effect_ = std::move(effect); }
    const std::shared_ptr<canvas::ViewEffect>& effect() const { return effect_; }

    /// Back-reference to the WindowHost that owns this view tree.
    /// Set by WindowHost when the root view is attached. Propagated to children.
    void set_window_host(WindowHost* host);
    WindowHost* window_host() const { return window_host_; }

    /// Back-reference to the PluginViewHost that owns this editor tree.
    /// Set by PluginViewHost when the root view is attached. Propagated to children.
    void set_plugin_view_host(PluginViewHost* host);
    PluginViewHost* plugin_view_host() const { return plugin_view_host_; }

    /// Background gradient (CSS background: linear-gradient / radial-gradient)
    void set_background_gradient_linear(float x0, float y0, float x1, float y1,
                                         const std::vector<Color>& colors,
                                         const std::vector<float>& positions) {
        bg_gradient_colors_ = colors;
        bg_gradient_positions_ = positions;
        bg_gradient_type_ = 1;  // linear
        bg_grad_x0_ = x0; bg_grad_y0_ = y0;
        bg_grad_x1_ = x1; bg_grad_y1_ = y1;
    }
    void clear_background_gradient() { bg_gradient_type_ = 0; }
    bool has_background_gradient() const { return bg_gradient_type_ > 0; }

    /// Text overflow: ellipsis (CSS text-overflow: ellipsis)
    void set_text_overflow_ellipsis(bool e) { text_ellipsis_ = e; }
    bool text_overflow_ellipsis() const { return text_ellipsis_; }

    /// pulp #1434 Phase A2-3 — writing direction (CSS `direction` /
    /// RN `writingDirection`). LTR is the default; RTL flips text
    /// shaping (Skia paragraph_style.setTextDirection), Yoga's flow
    /// (YGDirectionRTL — flexDirection 'row' visually reverses), and
    /// textAlign 'auto' resolution at paint time. The View::direction_
    /// state propagates inheritance only when the caller plumbs it
    /// through the tree (Phase A2-3 keeps inheritance opt-in;
    /// automatic-cascade is a follow-up). Enum is tri-state — `auto_`
    /// defers to parent / first-strong-character heuristic.
    enum class WritingDirection { ltr, rtl, auto_ };
    void set_direction(WritingDirection d) { direction_ = d; }
    WritingDirection direction() const { return direction_; }
    /// Resolve `auto_` → ltr (LTR fallback, until first-strong-character
    /// detection is wired). Use this at paint time when you need a
    /// concrete direction.
    WritingDirection resolved_direction() const {
        return direction_ == WritingDirection::auto_ ? WritingDirection::ltr : direction_;
    }

    /// White-space: nowrap (CSS `white-space: nowrap`). Pulp #1410. Generic
    /// flag so non-Label widgets (Button, custom text-bearing views) and
    /// text-shaper consumers can react to nowrap without dynamic_casting
    /// to a specific widget type. Label keeps its `multi_line_` flag in
    /// lock-step via WidgetBridge::setWhiteSpace, so existing callers keep
    /// working.
    // pulp #1737 — full CSS `white-space` enum. Replaces the
    // nowrap-only bool with the spec's 6 keywords. Per CSS Text
    // Module Level 3 §3:
    //   normal       collapse  drop_nl   wrap
    //   nowrap       collapse  drop_nl   nowrap
    //   pre          preserve  preserve  nowrap
    //   pre_wrap     preserve  preserve  wrap
    //   pre_line     collapse  preserve  wrap
    //   break_spaces preserve  preserve  wrap (extra break opps)
    enum class WhiteSpaceMode {
        normal, nowrap, pre, pre_wrap, pre_line, break_spaces
    };
    // pulp #1737 (Codex P2 followup on #1786): set_white_space_nowrap()
    // also keeps the WhiteSpaceMode enum in sync so callers using the
    // legacy setter don't leave white_space_mode() stale. The enum is
    // pinned to nowrap (true) or normal (false) — we can't infer
    // pre/pre-wrap/etc. from a bool, so the legacy setter only ever
    // produces these two modes.
    void set_white_space_nowrap(bool n) {
        white_space_nowrap_ = n;
        white_space_mode_ = n ? WhiteSpaceMode::nowrap : WhiteSpaceMode::normal;
    }
    bool white_space_nowrap() const { return white_space_nowrap_; }

    void set_white_space_mode(WhiteSpaceMode m) {
        white_space_mode_ = m;
        white_space_nowrap_ = (m == WhiteSpaceMode::nowrap || m == WhiteSpaceMode::pre);
    }
    WhiteSpaceMode white_space_mode() const { return white_space_mode_; }

    /// CSS / RN `mix-blend-mode` (pulp #1549). The blend mode applied when
    /// this View's compositing layer composites back onto its parent. Stored
    /// as the canvas BlendMode enum so the paint path can pass it straight
    /// through to the layer-paint without a string lookup. Default
    /// `BlendMode::normal` is a paint-time no-op (kSrcOver). Setting any
    /// non-default mode forces a saveLayer() at paint time so the subtree
    /// renders into an offscreen buffer that gets composited with the
    /// requested blend mode — same shape as opacity / filter:blur.
    void set_mix_blend_mode(canvas::Canvas::BlendMode m) { mix_blend_mode_ = m; }
    canvas::Canvas::BlendMode mix_blend_mode() const { return mix_blend_mode_; }
    bool has_non_default_blend_mode() const {
        return mix_blend_mode_ != canvas::Canvas::BlendMode::normal;
    }

    /// Cursor style hint (CSS cursor property)
    enum class CursorStyle {
        default_, pointer, crosshair, text, grab, grabbing, not_allowed,
        invisible,                ///< Hidden cursor (custom drag, knob rotation)
        horizontal_resize,        ///< ↔ Left-right resize (SplitView, column borders)
        vertical_resize,          ///< ↕ Up-down resize (SplitView, row borders)
        top_left_resize,          ///< ↖↘ Diagonal resize (NW-SE)
        top_right_resize,         ///< ↗↙ Diagonal resize (NE-SW)
        bottom_left_resize,       ///< ↗↙ Diagonal resize (alias for top_right)
        bottom_right_resize,      ///< ↖↘ Diagonal resize (alias for top_left)
        multi_directional_resize  ///< ✥ All-direction move/resize
    };
    void set_cursor(CursorStyle c) { cursor_ = c; }
    CursorStyle cursor() const { return cursor_; }

    /// CSS `user-select` (pulp #1538 / #1656 / Tier-2 follow-up).
    /// Stored slot for the keyword the JS shim resolves to. Read by
    /// interactive widgets (TextEditor, Label) that participate in
    /// text selection. `auto_` is the spec default (resolves per-widget
    /// to whatever its native selectability is).
    enum class UserSelect { auto_, none, text, all, contain };
    void set_user_select(UserSelect s) { user_select_ = s; }
    UserSelect user_select() const { return user_select_; }

private:
    /// Seed corner_radii_ from the uniform corner_radius_ on the first
    /// transition into per-corner mode (pulp #1171 Codex P2 on #1044).
    /// Idempotent: subsequent calls (when has_corner_radii_ is already
    /// true) are no-ops.
    void promote_uniform_to_per_corner() {
        if (!has_corner_radii_ && corner_radius_ > 0.0f) {
            corner_radii_[0] = corner_radius_;
            corner_radii_[1] = corner_radius_;
            corner_radii_[2] = corner_radius_;
            corner_radii_[3] = corner_radius_;
        }
    }

    Rect bounds_{};
    FlexStyle flex_{};
    GridStyle grid_{};
    LayoutMode layout_mode_ = LayoutMode::flex;
    Theme theme_;
    View* parent_ = nullptr;
    std::vector<std::unique_ptr<View>> children_;
    std::string id_;
    AccessRole access_role_ = AccessRole::none;
    std::string access_label_;
    std::string access_value_;
    // pulp #1737 — ARIA state attributes (tri-state per spec).
    std::string access_pressed_;
    std::string access_checked_;
    std::string access_disabled_;
    std::string access_hidden_;
    bool visible_ = true;
    bool focusable_ = false;
    bool enabled_ = true;
    bool layout_dirty_ = false;
    bool has_focus_ = false;
    bool hovered_ = false;
    bool hit_testable_ = true;
    PointerEvents pointer_events_ = PointerEvents::auto_;
    bool backface_visible_ = true;
    FrameClock* frame_clock_ = nullptr;

    // Visual properties
    float opacity_ = 1.0f;
    Color bg_color_{};
    bool has_bg_ = false;
    Color border_color_{};
    float border_width_ = 0;
    float corner_radius_ = 0;
    // pulp #1663 — borderRadius % support. When > 0, paint code resolves
    // effective radius as `corner_radius_pct_ * 0.01 * min(width, height)`.
    // 0 means "use plain corner_radius_ in px". Same pattern for the
    // per-corner radii_pct_ slots below.
    float corner_radius_pct_ = 0;
    // pulp #1737 RN-OOS-fixup (#1812) — RN `borderCurve` corner shape.
    BorderCurve border_curve_ = BorderCurve::circular;
    bool has_border_ = false;
    BorderStyle border_style_ = BorderStyle::solid;
    // pulp #1514 — list-style cluster slots. Stored verbatim; paint-
    // time marker rendering is deferred. Defaults match CSS spec
    // (`disc` for the type, `outside` for the position, empty image).
    ListStyleType list_style_type_ = ListStyleType::disc;
    std::string list_style_image_{};
    ListStylePosition list_style_position_ = ListStylePosition::outside;
    // CSS / RN outline cluster (pulp #1519). Defaults: outline_style_
    // is `none` so paint short-circuits unless JS opts in via
    // setOutlineStyle. width=0 also short-circuits as a belt-and-braces
    // guard. Color defaults to fully-transparent black; bridge writes
    // the parsed setter value before paint.
    Color outline_color_{};
    float outline_offset_ = 0.0f;
    float outline_width_ = 0.0f;
    BorderStyle outline_style_ = BorderStyle::none;
    // Per-side borders
    struct BorderSide { Color color{}; float width = 0; };
    BorderSide border_top_{}, border_right_{}, border_bottom_{}, border_left_{};
    bool has_border_sides_ = false;
    // pulp #1566 — per-edge "explicitly set" flags, parallel to has_top_
    // / has_right_ / etc. for inset edges. Required so an explicit
    // borderTopWidth=0 overrides the uniform borderWidth=10 shorthand
    // (CSS / RN semantics). Plain `width == 0` is ambiguous because the
    // BorderSide default-initializes to 0.
    bool border_top_set_ = false;
    bool border_right_set_ = false;
    bool border_bottom_set_ = false;
    bool border_left_set_ = false;
    // Per-corner radii
    float corner_radii_[4] = {0, 0, 0, 0}; // TL, TR, BL, BR
    // pulp #1663 — % support paired with corner_radii_. >0 means use pct
    // resolution, 0 means use the px slot above. Sized 4 (TL, TR, BL, BR).
    float corner_radii_pct_[4] = {0, 0, 0, 0};
    bool has_corner_radii_ = false;
    Position position_ = Position::static_;
    float top_ = 0, right_ = 0, bottom_ = 0, left_ = 0;
    bool has_top_ = false, has_right_ = false, has_bottom_ = false, has_left_ = false;
    // pulp #1434 batch 6 — per-edge inset unit. `px` is the historical
    // default; `percent` flows through to YGNodeStyleSetPositionPercent
    // in yoga_layout.cpp. Other DimensionUnit values (vw/vh/etc.) round
    // down to px at the bridge boundary today (no consumer demand).
    DimensionUnit top_unit_ = DimensionUnit::px;
    DimensionUnit right_unit_ = DimensionUnit::px;
    DimensionUnit bottom_unit_ = DimensionUnit::px;
    DimensionUnit left_unit_ = DimensionUnit::px;
    int z_index_ = 0;
    // pulp #972 — default is `visible` to match CSS. Pulp previously
    // defaulted to `hidden`, which clipped absolutely-positioned children
    // (popovers, dropdowns, tooltips) to the parent's content bounds and
    // made them invisible whenever they extended outside. Plugins that
    // intentionally need clipping must call set_overflow(Overflow::hidden)
    // explicitly — same opt-in as `overflow:hidden` in CSS.
    Overflow overflow_ = Overflow::visible;
    BoxShadow shadow_{};
    bool has_shadow_ = false;
    float scale_ = 1.0f;
    float translate_x_ = 0, translate_y_ = 0;
    float rotation_deg_ = 0;
    float skew_x_ = 0, skew_y_ = 0;
    float origin_x_ = 0.5f, origin_y_ = 0.5f;  // transform-origin (normalized)
    bool origin_explicit_ = false;  // pulp #1026 — has set_transform_origin been called?
    // Full 2D affine matrix (issue-930). Identity by default; only applied
    // when has_transform_matrix_ is true. Stored in CanvasRenderingContext2D
    // (a,b,c,d,e,f) order:  [a c e / b d f / 0 0 1].
    float transform_matrix_a_ = 1.0f, transform_matrix_b_ = 0.0f,
          transform_matrix_c_ = 0.0f, transform_matrix_d_ = 1.0f,
          transform_matrix_e_ = 0.0f, transform_matrix_f_ = 0.0f;
    bool has_transform_matrix_ = false;
    float filter_blur_ = 0;
    std::vector<FilterOp> filter_chain_{};
    float backdrop_blur_ = 0;
    // pulp #1515 — CSS clip-path / mask storage. Paint-time consumption
    // for clip_path_ via Canvas::clip_path_svg (SkPath::FromSVGString
    // on Skia, no-op fallback elsewhere). mask_image_ / mask_ are
    // storage-only today; the saveLayer + SkBlendMode::kDstIn shader
    // composite is a follow-up paint slice.
    std::string clip_path_;
    std::string mask_image_;
    std::string mask_;
    std::string mask_size_;     // pulp #1515 followup
    std::string appearance_;    // CSS appearance — storage-only no-op for Pulp custom widgets
    std::string object_fit_;    // CSS object-fit — storage; ImageView paint follow-up
    std::string object_position_; // CSS object-position — storage; pairs with object-fit
    /// pulp #1434 Phase A2-1 — transition specs + active animations.
    std::vector<TransitionSpec> transitions_{};
    std::vector<CssAnimation> active_animations_{};
    StagedAnimation staged_animation_{};
    std::string background_attachment_;  // pulp #1517 — noop today
    std::string background_clip_;        // pulp #1517 — partial (text deferred)
    std::string background_origin_;      // pulp #1517 — noop today
    std::string background_position_;    // Wave 5 css.5 — storage-only (raster bg deferred)
    std::string background_size_;        // Wave 5 css.5 — storage-only (raster bg deferred)

    // pulp #1434 A4 Bundles 5–7 closure — storage-only catalog slots.
    float       text_indent_ = 0.0f;       // partial (paint deferred)
    std::string word_break_;               // partial (HarfBuzz feature deferred)
    std::string scroll_behavior_;          // pulp #1737 RN-OOS-fixup — ScrollView consumes
    std::string overscroll_behavior_;      // pulp #1737 RN-OOS-fixup — ScrollView clamp consumes
    bool        include_font_padding_ = true;  // pulp #1737 — Android legacy round-trip slot (paint ignores)
    std::string font_variant_;             // partial (HarfBuzz feature deferred)
    std::string writing_mode_;             // noop (Pulp horizontal-only)
    std::string isolation_;                // wontfix (no z-buffer)
    std::string resize_;                   // noop (no resize handles)
    std::string animation_play_state_;     // partial (storage only)
    // pulp #1548 — RN textShadow* per-attribute storage slots. SkPaint
    // shadow integration deferred; storage path is round-trippable.
    std::string text_shadow_color_;
    float       text_shadow_dx_ = 0.0f;
    float       text_shadow_dy_ = 0.0f;
    float       text_shadow_radius_ = 0.0f;
    bool needs_layer_ = false;
    WindowHost* window_host_ = nullptr;
    PluginViewHost* plugin_view_host_ = nullptr;
    std::shared_ptr<canvas::ViewEffect> effect_;
    int bg_gradient_type_ = 0;  // 0=none, 1=linear, 2=radial
    float bg_grad_x0_ = 0, bg_grad_y0_ = 0, bg_grad_x1_ = 0, bg_grad_y1_ = 1;
    std::vector<Color> bg_gradient_colors_;
    std::vector<float> bg_gradient_positions_;
    std::string background_repeat_;  ///< pulp #1552: CSS background-repeat keyword (storage-only)
    bool text_ellipsis_ = false;
    bool white_space_nowrap_ = false;  // pulp #1410
    WhiteSpaceMode white_space_mode_ = WhiteSpaceMode::normal;  // pulp #1737
    CursorStyle cursor_ = CursorStyle::default_;
    UserSelect user_select_ = UserSelect::auto_;  // pulp #1538 / #1656 follow-up
    WritingDirection direction_ = WritingDirection::auto_;  // pulp #1434 A2-3
    // pulp #1549 — CSS / RN mix-blend-mode. Default kSrcOver (canvas
    // BlendMode::normal) is a paint-time no-op; any non-default value
    // forces a saveLayer() at paint time so the subtree composites back
    // through the requested blend mode.
    canvas::Canvas::BlendMode mix_blend_mode_ = canvas::Canvas::BlendMode::normal;

    // Pointer capture: pointer_id → this view receives all events for that pointer
    std::vector<int> captured_pointers_;

    // CSS-style typography inheritance (issue-969). Unset by default; only
    // populated when the bridge / app calls a set_inheritable_* setter.
    // These do not affect the View's own paint — only descendant Labels
    // (and other text widgets) consult them.
    std::optional<Color> inh_text_color_;
    std::optional<float> inh_font_size_;
    std::optional<float> inh_letter_spacing_;
    std::optional<int>   inh_font_weight_;
    std::optional<int>   inh_text_align_;
    std::optional<std::string> inh_font_family_;  // pulp #1434 Phase A2-5
};

} // namespace pulp::view
