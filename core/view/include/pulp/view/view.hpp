#pragma once

#include <algorithm>
#include <pulp/view/geometry.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/view_effect.hpp>
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
    virtual ~View() = default;

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

    /// Per-side borders (CSS border-top, border-right, etc.)
    void set_border_top(Color c, float w) { border_top_ = {c, w}; has_border_sides_ = true; }
    void set_border_right(Color c, float w) { border_right_ = {c, w}; has_border_sides_ = true; }
    void set_border_bottom(Color c, float w) { border_bottom_ = {c, w}; has_border_sides_ = true; }
    void set_border_left(Color c, float w) { border_left_ = {c, w}; has_border_sides_ = true; }

    /// Per-corner border-radius (CSS border-top-left-radius, etc.)
    void set_corner_radius_tl(float r) { corner_radii_[0] = r; has_corner_radii_ = true; }
    void set_corner_radius_tr(float r) { corner_radii_[1] = r; has_corner_radii_ = true; }
    void set_corner_radius_bl(float r) { corner_radii_[2] = r; has_corner_radii_ = true; }
    void set_corner_radius_br(float r) { corner_radii_[3] = r; has_corner_radii_ = true; }

    /// Box shadow (CSS-like: offset, blur, spread, color)
    struct BoxShadow {
        float offset_x = 0, offset_y = 2;
        float blur = 4;
        float spread = 0;
        Color color{0, 0, 0, 80};
    };
    void set_box_shadow(float ox, float oy, float blur, float spread, Color c) {
        shadow_ = {ox, oy, blur, spread, c}; has_shadow_ = true;
    }
    void clear_box_shadow() { has_shadow_ = false; }
    bool has_box_shadow() const { return has_shadow_; }

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

    /// Transform origin (0-1 normalized, default 0.5,0.5 = center)
    void set_transform_origin(float x, float y) { origin_x_ = x; origin_y_ = y; }
    float transform_origin_x() const { return origin_x_; }
    float transform_origin_y() const { return origin_y_; }

    /// CSS filter: blur(px) — per-element blur
    void set_filter_blur(float radius) { filter_blur_ = radius; }
    float filter_blur() const { return filter_blur_; }

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
    Overflow overflow_ = Overflow::hidden;
    BoxShadow shadow_{};
    bool has_shadow_ = false;
    float scale_ = 1.0f;
    float translate_x_ = 0, translate_y_ = 0;
    float rotation_deg_ = 0;
    float skew_x_ = 0, skew_y_ = 0;
    float origin_x_ = 0.5f, origin_y_ = 0.5f;  // transform-origin (normalized)
    float filter_blur_ = 0;
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
};

} // namespace pulp::view
