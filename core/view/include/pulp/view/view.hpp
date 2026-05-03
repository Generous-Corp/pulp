#pragma once

#include <algorithm>
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
    void set_border_radius(float r) { corner_radius_ = r; }

    /// Per-side borders (CSS border-top, border-right, etc.)
    void set_border_top(Color c, float w) { border_top_ = {c, w}; has_border_sides_ = true; }
    void set_border_right(Color c, float w) { border_right_ = {c, w}; has_border_sides_ = true; }
    void set_border_bottom(Color c, float w) { border_bottom_ = {c, w}; has_border_sides_ = true; }
    void set_border_left(Color c, float w) { border_left_ = {c, w}; has_border_sides_ = true; }
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

    /// Per-corner border-radius (CSS border-top-left-radius, etc.)
    /// pulp #1171 (Codex P2 on #1044) — when transitioning from uniform
    /// `corner_radius_` to per-corner mode, seed the un-overridden
    /// corners from the uniform value so a sequence like
    /// `set_border_radius(10); set_corner_radius_tl(2);` renders as
    /// {2, 10, 10, 10} instead of {2, 0, 0, 0} (which silently
    /// discarded the uniform radius).
    void set_corner_radius_tl(float r) { promote_uniform_to_per_corner(); corner_radii_[0] = r; has_corner_radii_ = true; }
    void set_corner_radius_tr(float r) { promote_uniform_to_per_corner(); corner_radii_[1] = r; has_corner_radii_ = true; }
    void set_corner_radius_bl(float r) { promote_uniform_to_per_corner(); corner_radii_[2] = r; has_corner_radii_ = true; }
    void set_corner_radius_br(float r) { promote_uniform_to_per_corner(); corner_radii_[3] = r; has_corner_radii_ = true; }
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
    /// Clear the global overlay if (and only if) `this` currently holds it.
    /// Idempotent — safe to call on unmount even if claim never happened.
    void release_overlay() {
        if (active_overlay_ == this) active_overlay_ = nullptr;
    }
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

    void set_top(float v) { top_ = v; has_top_ = true; }
    void set_right(float v) { right_ = v; has_right_ = true; }
    void set_bottom(float v) { bottom_ = v; has_bottom_ = true; }
    void set_left(float v) { left_ = v; has_left_ = true; }
    float top() const { return top_; }
    float right() const { return right_; }
    float bottom() const { return bottom_; }
    float left() const { return left_; }
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
    enum class Overflow { hidden, visible };
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

    /// CSS filter: blur(px) — per-element blur
    void set_filter_blur(float radius) { filter_blur_ = radius; }
    float filter_blur() const { return filter_blur_; }

    /// CSS backdrop-filter: blur(px) — frosted-glass blur applied to whatever
    /// is behind this View when it paints (issue-926). Zero == no backdrop
    /// filter. Skia maps to `saveLayer(SaveLayerRec{ .fBackdrop = Blur })`.
    void set_backdrop_blur(float radius) { backdrop_blur_ = radius; }
    float backdrop_blur() const { return backdrop_blur_; }

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
    bool has_border_ = false;
    // Per-side borders
    struct BorderSide { Color color{}; float width = 0; };
    BorderSide border_top_{}, border_right_{}, border_bottom_{}, border_left_{};
    bool has_border_sides_ = false;
    // Per-corner radii
    float corner_radii_[4] = {0, 0, 0, 0}; // TL, TR, BL, BR
    bool has_corner_radii_ = false;
    Position position_ = Position::static_;
    float top_ = 0, right_ = 0, bottom_ = 0, left_ = 0;
    bool has_top_ = false, has_right_ = false, has_bottom_ = false, has_left_ = false;
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
    float backdrop_blur_ = 0;
    bool needs_layer_ = false;
    WindowHost* window_host_ = nullptr;
    PluginViewHost* plugin_view_host_ = nullptr;
    std::shared_ptr<canvas::ViewEffect> effect_;
    int bg_gradient_type_ = 0;  // 0=none, 1=linear, 2=radial
    float bg_grad_x0_ = 0, bg_grad_y0_ = 0, bg_grad_x1_ = 0, bg_grad_y1_ = 1;
    std::vector<Color> bg_gradient_colors_;
    std::vector<float> bg_gradient_positions_;
    bool text_ellipsis_ = false;
    CursorStyle cursor_ = CursorStyle::default_;

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
};

} // namespace pulp::view
