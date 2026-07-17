#pragma once

#include <algorithm>
#include <cstdint>
#include <pulp/view/css_animation.hpp>
#include <pulp/view/geometry.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/value_source.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/view_effect.hpp>
#include <optional>
#include <vector>
#include <memory>
#include <string>
#include <string_view>

namespace pulp::view {

class WindowHost;  // Forward declaration for View→Host back-reference
class PluginViewHost;
class HostParamSurface;   // pulp/view/host_param_surface.hpp — runtime param accessor
class HostActionSurface;  // pulp/view/host_param_surface.hpp — host command channel
class GestureArbiter; class GestureRecognizer;
class FrameClock;
class WidgetPainter;     // pulp/view/widget_painter.hpp — pluggable paint delegate
class WidgetMetrics;     // pulp/view/widget_metrics.hpp — pluggable sizing delegate
class FrameClockBinding; // pulp/view/value_source_binding.hpp
struct ViewValueBindings; // pulp/view/src/view.cpp — lazily allocated value-source bindings
struct FileDragRequest;  // pulp/view/drag_drop.hpp
struct ActiveDrag;       // pulp/view/drag_drop.hpp
struct DropData;         // pulp/view/drag_drop.hpp

// Base class for all UI elements
// Views form a tree: each view has zero or more children and one optional parent
class View {
public:
    View();
    virtual ~View();

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

    // ── Reload state (live-swap item 1.4b) ───────────────────────────────
    // A CUSTOM widget overrides these to carry its own state across a
    // scripted-UI hot reload, beyond the built-in knob/fader/combo/segmented/
    // toggle/checkbox/xy coverage the WidgetBridge snapshots by type.
    // save_reload_state writes an opaque blob and returns true to OPT IN;
    // restore_reload_state applies a blob previously written by the same widget
    // type (matched by the script's widget id). Default: no custom state
    // (returns false), so every existing widget is unaffected — the bridge only
    // stores/restores a blob for widgets that opt in.
    virtual bool save_reload_state(std::string& /*out_blob*/) const { return false; }
    virtual bool restore_reload_state(std::string_view /*blob*/) { return false; }

    // ── View recycling (ViewPool, planning 2.3) ──────────────────────────
    // A recyclable View can be parked in a ViewPool when it scrolls out of a
    // virtualized list/grid (or is torn down by a hot reload) and handed back
    // later instead of being destroyed and rebuilt. Recycling is OPT-IN: a
    // subclass must override supports_reuse() to return true, otherwise
    // ViewPool::release() destroys it and adoption stays behavior-neutral.
    //
    // supports_reuse() advertises that prepare_for_reuse() leaves the instance
    // in a safe, blank-slate state — no live callbacks into freed state, no
    // stale bounds/selection/focus. Only override it once that contract holds.
    virtual bool supports_reuse() const { return false; }

    // Scrub every piece of per-instance state that must NOT survive into the
    // next user of a recycled View. The ViewPool calls this on the acquire
    // path, so a pooled view carries no live state while parked. Subclasses
    // that add their own callbacks / cached state MUST override this, reset
    // their own members, then call View::prepare_for_reuse() to run the base
    // reset below. The base impl clears bounds/visibility/opacity/access
    // labels/pointer+hover+focus state AND EVERY base-class callback — a
    // recycled view firing a stale on_click/on_drag/etc. into a torn-down
    // closure is the exact use-after-free this guards against.
    //
    // Precondition: the view must be detached (no parent) — a pooled view is
    // never part of a live tree.
    virtual void prepare_for_reuse();

    // ── Hit testing ──────────────────────────────────────────────────────

    // Find the deepest child that contains the given point (in local coords)
    virtual View* hit_test(Point local_point);

    // ── Theme ────────────────────────────────────────────────────────────

    void set_theme(const Theme& theme) { theme_ = theme; request_repaint(); }
    const Theme& theme() const { return theme_; }

    // Resolve a color: check own theme first, then walk up to parent
    Color resolve_color(const std::string& name, Color fallback = {}) const;

    /// Override ONE named color token on this view without replacing the rest of
    /// its theme. Tokens this view does not carry keep resolving up the parent
    /// chain, so a per-view override is genuinely partial. This is the write side
    /// of `resolve_color`.
    void set_color(std::string name, Color c) {
        theme_.colors[std::move(name)] = c;
        request_repaint();
    }

    // ── Paint delegate ───────────────────────────────────────────────────
    //
    // A WidgetPainter installed here supplies the pixels for the stock controls
    // in this view AND its whole subtree, unless a descendant installs its own.
    // See widget_painter.hpp. Each hook defaults to declining, so a delegate
    // restyles only the controls it implements and the rest keep Pulp's look.

    void set_painter(std::shared_ptr<WidgetPainter> p);
    const std::shared_ptr<WidgetPainter>& painter() const { return painter_; }

    /// The painter that governs THIS view: its own if it has one, else the
    /// nearest ancestor's, else null (meaning "draw the stock look").
    WidgetPainter* effective_painter() const;

    // ── Metrics delegate ─────────────────────────────────────────────────
    //
    // The sizing sibling of the painter: same install, same subtree cascade,
    // same decline-by-default hooks. Consulted during LAYOUT, where there is no
    // canvas — which is why it is a separate object from the painter. Widgets
    // reach it from `intrinsic_width()`/`intrinsic_height()` (the function the
    // layout engine already calls to measure a leaf), or directly when they
    // position themselves. See widget_metrics.hpp.

    void set_metrics(std::shared_ptr<WidgetMetrics> m);
    const std::shared_ptr<WidgetMetrics>& metrics() const { return metrics_; }

    /// The metrics delegate that governs THIS view: its own if it has one,
    /// else the nearest ancestor's, else null (meaning "use the stock size").
    WidgetMetrics* effective_metrics() const;

    // ── CSS-style typography inheritance ─────────────────────────────────
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

    /// Inheritable font-family cascade. Mirrors the font-weight pattern;
    /// Labels read this when set_font_family hasn't been called directly.
    /// Font-manager resolution is independent from the cascade plumbing.
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
    virtual bool owns_child_layout() const { return false; }

    /// Intrinsic content size (override in widgets that know their natural size).
    /// Returns 0 if no intrinsic size (use preferred_width/height instead).
    virtual float intrinsic_width() const { return 0; }
    virtual float intrinsic_height() const;  // default: sum of visible children for containers

    // ── Painting ──────────────────────────────────────────────────────────

    // Paint this view and all children into a canvas
    virtual void paint_all(canvas::Canvas& canvas);

    // Last paint cycle's wall-clock cost.
    // Updated by paint_all() at the end of each paint pass. Exposed so
    // the inspector property panel can show per-view timing without
    // adding new instrumentation per query.
    //
    //   last_paint_self_ns()         — time spent INSIDE this view's
    //                                  paint(canvas) override only.
    //   last_paint_with_children_ns() — time spent INSIDE paint(canvas)
    //                                  PLUS time recursively painting
    //                                  every descendant. The
    //                                  difference (with_children - self)
    //                                  attributes child cost cleanly.
    //
    // Both are 0 until the view has been painted at least once. Updated
    // every frame; readers see the most recent sample. Storage is two
    // uint32_t fields per view (cap at ~4s of paint time which is far
    // beyond any realistic frame budget).
    std::uint32_t last_paint_self_ns() const { return last_paint_self_ns_; }
    std::uint32_t last_paint_with_children_ns() const { return last_paint_with_children_ns_; }

    // ── Lifecycle (override in subclasses) ────────────────────────────────

    virtual void paint(canvas::Canvas&) {}
    virtual void on_resized() {}
    virtual void on_attached() {}
    virtual void on_detached() {}
    /// Advance any per-widget non-CSS animation state (hover glows, thumb
    /// tweens, spinner phase) by `dt` seconds. Default no-op; widgets with
    /// interactive animation override this. Driven every frame by the host
    /// frame pump alongside the CSS `tick_animations()` timeline.
    virtual void advance_animations(float dt) { (void)dt; }
    /// The FrameClock reachable from this view (via `frame_clock()`) may have
    /// changed — a clock was installed on an ancestor, or this subtree was
    /// re-parented. A view that self-subscribes to the clock (e.g. a Meter with
    /// a live source) overrides this to (re)evaluate its subscription. Default
    /// no-op. Fired by `set_frame_clock()` across the whole subtree.
    virtual void on_frame_clock_changed() {}

    // ── Input events (rich, with modifiers and pointer ID) ──────────────

    /// Mouse down with full event context.
    virtual void on_mouse_event(const MouseEvent& event) {
        if (on_pointer_event) on_pointer_event(event);
    }
    /// True if this widget adjusts its VALUE on a scroll-wheel over it (knobs,
    /// faders, sliders, steppers, pan). The host routes the wheel to such a
    /// widget under the cursor instead of scrolling an enclosing scroll view.
    virtual bool wants_wheel_value() const { return false; }
    /// Adjust the widget's value by a scroll-wheel delta (`delta_y` positive =
    /// scrolled down). Only called when wants_wheel_value() is true.
    virtual void on_wheel(float delta_y) { (void)delta_y; }
    /// True while the user is actively manipulating this widget's value with a
    /// pointer drag (knob turn, fader/slider drag, XY-pad move). Value-carrying
    /// widgets that track a drag gesture override this. A declarative
    /// native→widget binding (WidgetBridge::bindWidgetToParam) yields to an
    /// active gesture so a live param push never fights the user mid-drag.
    virtual bool is_gesture_active() const { return false; }
    /// True if this widget is itself a scroll container that should consume
    /// wheel input before an ancestor scroll view.
    virtual bool wants_wheel_scroll() const { return false; }
    /// True if this widget handles mouse down/drag through virtual methods even
    /// when it does not expose JS pointer callbacks or focus.
    virtual bool wants_mouse_input() const { return false; }
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
    GestureRecognizer& add_gesture_recognizer(std::unique_ptr<GestureRecognizer> recognizer);
    void clear_gesture_recognizers(); size_t gesture_recognizer_count() const { return gesture_recognizers_.size(); }
    GestureRecognizer* gesture_recognizer_at(size_t index); const GestureRecognizer* gesture_recognizer_at(size_t index) const;
    bool dispatch_gesture_pointer_event(const MouseEvent& root_event, double timestamp_seconds = -1.0);
    void advance_gesture_recognizers(double timestamp_seconds = -1.0); bool has_time_driven_gestures() const;
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

    /// Pointer moved over this view with no button held, reported in this
    /// view's LOCAL coordinates. Unlike on_mouse_enter/leave (which carry no
    /// position), this fires on every hover sample over the hit target so a
    /// widget can track WHICH sub-region of itself the pointer is over (e.g.
    /// the inspector ToolStrip's per-button tooltip). Dispatched by
    /// simulate_hover() — the same path the platform host's mouse-move handler
    /// drives. Default no-op so no existing widget changes behavior.
    virtual void on_hover_move(Point local_pos) { (void)local_pos; }

    bool is_hovered() const { return hovered_; }
    void set_hovered(bool h);

    // ── Frame clock ─────────────────────────────────────────────────────

    /// Set the frame clock on the root view. Children access via frame_clock().
    /// Hosts typically build the view tree *before* installing the clock, so
    /// this propagates an `on_frame_clock_changed()` notification down the whole
    /// subtree — a descendant that self-subscribes on a reachable clock (a live
    /// Meter, say) gets a chance to (re)subscribe now rather than silently
    /// missing the clock it was built before.
    void set_frame_clock(FrameClock* clock);

    /// Get the frame clock (walks up parent chain to find it).
    FrameClock* frame_clock() const;

    // ── Live host→view value sources ────────────────────────────────────────
    // The seam a view showing a live host value (a meter, a readout, a
    // modulation ring) uses instead of hand-rolling a frame-clock-polled
    // reader. nullptr unbinds. UI thread. Semantics, paint-safety, the
    // frames-alive effect and the FrameClock lifetime contract are documented
    // once in value_source_binding.hpp.

    /// Bind a MeterSource. `channel` is the primary channel — the one
    /// `on_meter_frame` drives a single-value widget from. A view painting
    /// several meters reads the whole snapshot via `meter_frame()` instead.
    void set_meter_source(std::shared_ptr<MeterSource> source, int channel = 0);
    bool has_meter_source() const;
    int meter_source_channel() const;

    /// The latest MeterFrame snapshotted on the clock. All-zero
    /// (`channels == 0`) until the first frame, and again from the moment the
    /// source is changed or unbound. Bound your channel index by
    /// `min(frame.channels, MeterFrame::kMaxChannels)` — `publish()` stores the
    /// frame verbatim and never trusts the count to gate an access.
    const MeterFrame& meter_frame() const;

    /// Bind a ScalarSource — one cached number (a readout, a modulation ring's
    /// modulated position).
    void set_scalar_source(std::shared_ptr<ScalarSource> source);
    bool has_scalar_source() const;

    /// The latest scalar snapshotted on the clock. 0 until the first frame, and
    /// again from the moment the source is changed or unbound.
    float scalar_value() const;

    /// A new MeterFrame was snapshotted this frame, `dt` seconds after the last.
    /// Default no-op — `meter_frame()` already exposes it to `paint()`. A widget
    /// that advances per-frame state from the reading (ballistics, smoothing)
    /// overrides this. UI thread, on the FrameClock.
    virtual void on_meter_frame(const MeterFrame& frame, float dt) {
        (void)frame;
        (void)dt;
    }

    // ── Transient-animation glue ─────────────────────────────────────────────
    // Tween/easing already exist; what a faithful port re-rolls every time is
    // the View-lifecycle glue — self-(un)subscribing a FrameClock callback and
    // repainting. animate() does exactly that: it interpolates a float from
    // `from` to `to` over `duration_s` seconds through `ease`, applies each
    // step via `apply`, and repaints. It auto-unsubscribes on completion AND on
    // View destruction (so a fade that outlives its view can't use-after-free).
    // Returns an animation id, or -1 if no FrameClock is reachable yet
    // (previews/screenshots — the caller should just set the final value).
    // A non-empty `tag` makes the animation self-cancelling: starting a new one
    // with the same tag cancels the prior (e.g. re-hovering before a fade ends).
    // `on_done` fires once when the tween reaches `to`, never when cancelled.
    int animate(std::function<void(float)> apply, float from, float to,
                float duration_s, std::function<float(float)> ease = {},
                std::function<void()> on_done = {}, const std::string& tag = {});

    /// Cancel a running animation by id (no-op if already finished/unknown).
    void cancel_animation(int id);
    /// Number of animations currently running on this view. For tests.
    int active_animation_count() const { return static_cast<int>(animations_.size()); }

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

    /// Does this widget consume printable character keys for text entry?
    /// Default false. Override to true on TextEditor and any future
    /// text-accepting widget. Used by `WidgetBridge::forward_key_event`
    /// to suppress bare-key global shortcuts (e.g. bare `?` for cheatsheet)
    /// while the user is typing — non-text focusables like Knob, Button,
    /// ListBox return false so single-key shortcuts still fire after they
    /// take focus.
    virtual bool accepts_text_input() const { return false; }

    /// CSS :disabled equivalent — blocks input, reduces opacity
    bool enabled() const { return enabled_; }
    void set_enabled(bool e) { enabled_ = e; }

    /// CSS pointer-events: none — view is invisible to hit testing
    bool hit_testable() const { return hit_testable_; }
    void set_hit_testable(bool h) { hit_testable_ = h; }

    /// React Native pointerEvents. Four-valued enum that mirrors RN's contract:
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

    /// React Native backfaceVisibility. Stored on the View for plumbing parity
    /// with `@pulp/react`. Pulp's transform model is currently 2D-affine, so
    /// this behaves as a paint-time no-op.
    bool backface_visible() const { return backface_visible_; }
    void set_backface_visible(bool v) { backface_visible_ = v; }

    /// GPU-host capability signal.
    /// A view that is rendered through the Skia/Dawn/Yoga scripted-UI path —
    /// or any custom `Processor::create_view()` that paints via GPU-backed
    /// widgets (e.g. WebGPU/Three.js canvases, the scripted React UI) — sets
    /// this true so plugin format adapters auto-select the GPU
    /// `PluginViewHost` instead of the CoreGraphics CPU fallback. Default
    /// false: a plain widget tree renders fine on CPU and a Processor that
    /// returns `nullptr` from `create_view()` intentionally selects AutoUi.
    ///
    /// This lives on the View (not the Processor) because `create_view()` can
    /// return a different view per editor instance and the flag must not touch
    /// the node ABI. Adapters read it via `ViewBridge::view()` after
    /// `ViewBridge::open()`.
    bool requires_gpu_host() const { return requires_gpu_host_; }
    void set_requires_gpu_host(bool v) { requires_gpu_host_ = v; }

    /// True when this view (or its subtree) attaches a NATIVE child overlay — a
    /// WebView / NSView / HWND that is composited by the OS window server, NOT
    /// painted into the Pulp Skia canvas by `paint_all()`. Such overlays are
    /// invisible to headless capture (`render_to_png` / `HeadlessSurface`); the
    /// smart capture path (`capture_view`) reads this to refuse with a clear
    /// reason instead of silently returning a blank frame. Generic on purpose —
    /// the owning wrapper view sets it (e.g. a WebView pane), so capture does not
    /// need to know about WebView specifically.
    bool contains_native_overlay() const { return contains_native_overlay_; }
    void set_contains_native_overlay(bool v) { contains_native_overlay_ = v; }

    /// When this view owns a native overlay (a WebView pane), it overrides this to
    /// return an in-process PNG snapshot of that overlay (e.g. WKWebView
    /// takeSnapshot). `capture_view` calls it for `contains_native_overlay()`
    /// subtrees so even native overlays are headlessly capturable instead of
    /// refused. Default empty = "no in-process snapshot available". Main-thread.
    virtual std::vector<uint8_t> capture_native_overlay_png(uint32_t /*width*/,
                                                            uint32_t /*height*/) {
        return {};
    }

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

    /// Bounded repaint: invalidate only `local_dirty` (this view's local
    /// coordinates), mapped to root space, so a live sub-view (meter, grid,
    /// overlay) does not force the whole surface to re-composite. Escalates to a
    /// full request_repaint() when the mapping cannot be trusted (render
    /// transform, pixel-spreading filter, a scrolled ScrollView ancestor, or no
    /// window host) — see the implementation for the exact conditions. Safe by
    /// construction: it only ever repaints the same or a larger region than the
    /// change touched, never smaller. Caller contract: `local_dirty` must cover
    /// out-of-bounds decorations (shadow, glow, focus ring) — pass the padded
    /// rect, not `local_bounds()`, or the overspill is left stale.
    void request_repaint(const Rect& local_dirty);

    /// Drag existing on-disk files out of this view to another app (e.g. drop a
    /// rendered .wav onto a DAW timeline). Routes through the attached host's
    /// native view to the platform backend; call from a pointer handler. False
    /// if no host, no files, or unsupported. See `pulp/view/drag_drop.hpp`.
    bool start_file_drag(const FileDragRequest& request);

    // ── In-tree drag source (drag_drop.hpp) ──────────────────────────────
    //
    // Begin a drag whose payload stays in this process and in this view tree.
    // The source view owns the gesture: start_drag() on press-and-move,
    // drag_to() on each motion tick, drop_here() on release.
    //
    //     void on_mouse_drag(Point p) override {
    //         if (!drag_active()) { DropData d; d.text = id_; start_drag(d); }
    //         drag_to(local_to_root(p));
    //     }
    //     void on_mouse_up(Point p) override { drop_here(local_to_root(p)); }
    //
    // All three take ROOT coordinates, the space the drop dispatch works in.

    bool start_drag(DropData data);
    void drag_to(Point root_pos);
    bool drop_here(Point root_pos);
    void cancel_drag();
    bool drag_active() const;
    bool has_focus() const { return has_focus_; }
    void set_focus(bool f) { has_focus_ = f; }

    // Move focus to next/previous focusable widget
    static View* focus_next(View& root, View* current);
    static View* focus_prev(View& root, View* current);

    // ── Accessibility ────────────────────────────────────────────────────

    /// Screen-reader role. Maps to a concrete platform role on all five
    /// bridges — see platform/{ns_role_mapping,uia_mapping,atspi_mapping}.hpp,
    /// accessibility_ios.mm, and the ROLE_* constants in PulpAccessibility.kt.
    /// ARIA tokens map in via pulp/view/aria_roles.hpp. `toggle` is ARIA
    /// `switch`; `meter` is a read-only gauge, `progress_bar` task progress.
    ///
    /// ORDINALS ARE WIRE FORMAT — the Android JNI bridge marshals the role as
    /// static_cast<int>(role) and PulpAccessibility.kt matches on the integer.
    /// APPEND ONLY; never reorder or remove. Locked by test_accessibility_tree.
    ///
    /// A role NAMES a widget; it does not implement that widget's platform
    /// interaction pattern (combobox expand/collapse, table traversal, invoke).
    /// See docs/guides/modules/view.md for what is and is not wired.
    enum class AccessRole {
        none = 0, slider, toggle, label, group, meter, image,   // frozen 0-6
        button, link, checkbox, radio, text_field, combo_box,
        list, list_item, table, row, cell, tab, tab_list,
        menu, menu_item, progress_bar, dialog, heading, scroll_bar,
    };

    void set_access_role(AccessRole role) { access_role_ = role; }
    AccessRole access_role() const { return access_role_; }

    /// The AUTHOR-SET accessible name (what `aria-label` maps to). An explicit
    /// name WINS over the view's content — ARIA 1.2 §5.2.7 (accname step 2B:
    /// aria-label is consulted before the element's contents). Setting the
    /// empty string clears it and hands the name back to the content, which is
    /// what removing the attribute means.
    void set_access_label(std::string label) { access_label_ = std::move(label); }

    /// The CONTENT-DERIVED accessible name: a widget's visible text
    /// (Label::set_text, Knob/Fader/Toggle::set_label, TextButton::set_label).
    /// It NEVER overwrites an author-set name — otherwise
    ///     el.setAttribute('aria-label', 'Gain in decibels');
    ///     el.textContent = 'dB';
    /// would leave the screen reader announcing "dB", and react-reconciler's
    /// commitTextUpdate re-writes textContent on every re-render, so even a
    /// safe initial order would be clobbered on the next render.
    void set_derived_access_label(std::string label) {
        derived_access_label_ = std::move(label);
    }

    /// The resolved accessible name: author-set if present, else content-derived.
    const std::string& access_label() const {
        return access_label_.empty() ? derived_access_label_ : access_label_;
    }

    /// True when an author explicitly named this view (set_access_label /
    /// aria-label), as opposed to the name being derived from its content.
    bool has_explicit_access_label() const { return !access_label_.empty(); }

    /// The content-derived name on its own, ignoring any author-set name.
    const std::string& derived_access_label() const { return derived_access_label_; }

    void set_access_value(std::string value) { access_value_ = std::move(value); }
    const std::string& access_value() const { return access_value_; }

    // ARIA state attributes. Tri-state semantics per the ARIA 1.2 spec
    // (`true` / `false` / `mixed` / unset). We store the raw string the JS shim
    // hands us so platform accessibility bridges can read it back verbatim.
    // Setting the empty string clears the state.
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

    /// Stable anchor identity from the design-import IR. The inspector uses
    /// this to key tweaks against the originating source element so they
    /// survive re-import.
    ///
    /// Empty by default. Populated by the JS bridge's setAnchor() call
    /// for views constructed from imported designs; user-authored
    /// views that aren't part of an imported tree leave it empty.
    void set_anchor_id(std::string anchor) { anchor_id_ = std::move(anchor); }
    const std::string& anchor_id() const { return anchor_id_; }

    /// Stable for this View object's lifetime. Used by import-binding claim
    /// guards so a rebuilt tree can bind even if the allocator reuses an old
    /// View address.
    std::uint64_t import_binding_instance_id() const { return import_binding_instance_id_; }
    std::weak_ptr<const std::uint64_t> import_binding_lifetime_token() const {
        return import_binding_lifetime_token_;
    }

    /// Authored-source location for this view. Populated from React's
    /// `__source` prop (file name + line + column) by the JS bridge's
    /// `setSource()` call when the view was created by the `@pulp/react`
    /// reconciler from a JSX element. Empty for user-authored / non-imported
    /// views.
    ///
    /// `line` / `col` are 1-based when present, mirroring the editor
    /// URI convention; `0` means "not recorded". The inspector reads
    /// this to open the user's editor at the originating file:line via
    /// `Inspector.jumpToSource`.
    struct SourceLocation {
        std::string file;   ///< Source file path (as authored).
        int line = 0;       ///< 1-based line; 0 = unknown.
        int col = 0;        ///< 1-based column; 0 = unknown.

        bool valid() const { return !file.empty(); }
    };

    void set_source_loc(SourceLocation loc) { source_loc_ = std::move(loc); }
    void clear_source_loc() { source_loc_.reset(); }
    bool has_source_loc() const { return source_loc_.has_value(); }
    /// Returns the recorded source location, or an empty (`!valid()`)
    /// SourceLocation when none has been set.
    const SourceLocation& source_loc() const {
        static const SourceLocation kEmpty{};
        return source_loc_ ? *source_loc_ : kEmpty;
    }

    // ── Visual properties (CSS Box Model) ────────────────────────────────

    /// Opacity (0.0 = transparent, 1.0 = opaque). Applied as layer alpha.
    void set_opacity(float o) { opacity_ = std::clamp(o, 0.0f, 1.0f); }
    float opacity() const { return opacity_; }

    /// Background color (optional — if set, painted before children)
    void set_background_color(Color c) { bg_color_ = c; has_bg_ = true; }
    void clear_background_color() { has_bg_ = false; }
    bool has_background_color() const { return has_bg_; }
    Color background_color() const { return bg_color_; }

    /// CSS `background-repeat` keyword. Storage-only at the View level:
    /// paint() of solid-color backgrounds is a no-op for repeat semantics, but
    /// the value is preserved so image/gradient background paint can honor it
    /// without an API break. Accepted CSS keywords: `repeat`, `repeat-x`,
    /// `repeat-y`, `no-repeat`, `space`, `round`. Unknown / empty = `repeat`
    /// (CSS initial value).
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

    /// Standalone border setters for RN parity. Each setter flips the
    /// has_border_ flag on so paint_all() actually emits the stroke even when
    /// set_border() was never called.
    void set_border_color(Color c) { border_color_ = c; has_border_ = true; }
    void set_border_width(float w) { border_width_ = w; has_border_ = true; }
    void set_border_radius(float r) { corner_radius_ = r; corner_radius_pct_ = 0; }
    /// Set corner radius as percent of min(width,height). Resolved at paint
    /// time. Pass 0 to clear the percent and revert to the plain px slot.
    void set_border_radius_pct(float pct) { corner_radius_pct_ = pct; }
    float corner_radius_pct() const { return corner_radius_pct_; }

    /// RN's `borderCurve`: corner shape selection. `circular` (default) is the
    /// standard quarter-circle rounded corner; `continuous` is the iOS-style
    /// super-ellipse / squircle approximation. View::paint_all picks the
    /// appropriate path generator based on this slot. Slot has no effect when
    /// border-radius is 0.
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

    /// CSS / RN border-style. Skia path effect dispatches on style at paint
    /// time. CG falls through to solid (the cg_canvas.mm path inherits the
    /// canvas-base no-op set_line_dash). Values that Pulp doesn't render
    /// natively (`double` / `groove` / `ridge` / `inset` / `outset`) degrade
    /// to solid. `none` / `hidden` skip the stroke entirely.
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

    /// CSS / RN list-style cluster. Pulp doesn't model HTML <li>/<ul>/<ol>
    /// semantics; these slots store the values the consumer set so a list
    /// semantic surface can honor them. The bridge round-trips the keyword/url;
    /// View itself does not paint markers.
    enum class ListStyleType {
        none,     ///< No marker.
        disc,     ///< Filled circle (default for <ul>).
        circle,   ///< Hollow circle.
        square,   ///< Filled square.
        decimal,  ///< Numeric (default for <ol>) — needs sibling-index, not painted yet.
        // CSS Counter Styles Level 3 keywords. Storage-only today; View itself
        // does not paint the marker glyphs. The slots exist so the bridge can
        // round-trip the consumer's keyword honestly. Underscored C++
        // identifiers map to hyphenated CSS keywords (e.g. lower_roman ↔
        // "lower-roman").
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

    /// CSS / RN outline cluster. Outline is a paint-time ring drawn outside
    /// the border-box; it does not affect Yoga layout. Slotting mirrors
    /// border-* but lives in its own quartet of fields so a JSX prop diff that
    /// touches one outline-* prop preserves the others. Reuses
    /// View::BorderStyle for the line-style enum since CSS keyword sets are
    /// identical. Skia inflates the box by `outline_offset_ + outline_width_ /
    /// 2` and strokes; CG falls through for dashed/dotted same as border-style.
    void set_outline_color(Color c) { outline_color_ = c; }
    void set_outline_offset(float px) { outline_offset_ = px; }
    void set_outline_style(BorderStyle s) { outline_style_ = s; }
    void set_outline_width(float px) { outline_width_ = px; }
    Color outline_color() const { return outline_color_; }
    float outline_offset() const { return outline_offset_; }
    BorderStyle outline_style() const { return outline_style_; }
    float outline_width() const { return outline_width_; }


    /// Per-side borders (CSS border-top, border-right, etc.). Track an explicit
    /// per-edge "set" flag so a width of 0 on one edge can override the
    /// uniform shorthand. CSS / RN semantics: `borderWidth: 10` followed by
    /// `borderTopWidth: 0` must yield a 0-px top border, not the 10-px
    /// shorthand. Without a per-edge `set` bit, the stored 0 is
    /// indistinguishable from "unset" in `apply_border_widths`.
    void set_border_top(Color c, float w) { border_top_ = {c, w}; border_top_set_ = true; has_border_sides_ = true; }
    void set_border_right(Color c, float w) { border_right_ = {c, w}; border_right_set_ = true; has_border_sides_ = true; }
    void set_border_bottom(Color c, float w) { border_bottom_ = {c, w}; border_bottom_set_ = true; has_border_sides_ = true; }
    void set_border_left(Color c, float w) { border_left_ = {c, w}; border_left_set_ = true; has_border_sides_ = true; }
    /// Color-only setters. Setting `borderTopColor` alone must not mark the
    /// top edge's width as explicitly set; that would let a stale 0 override
    /// the uniform `borderWidth` shorthand. Mirrors CSS, where
    /// `border-top-color` and `border-top-width` are independent longhands.
    void set_border_top_color(Color c)    { border_top_.color = c;    has_border_sides_ = true; }
    void set_border_right_color(Color c)  { border_right_.color = c;  has_border_sides_ = true; }
    void set_border_bottom_color(Color c) { border_bottom_.color = c; has_border_sides_ = true; }
    void set_border_left_color(Color c)   { border_left_.color = c;   has_border_sides_ = true; }
    /// Width-only setters. Setting `borderTopWidth` alone preserves the
    /// existing per-edge color and explicitly marks the width as set, including
    /// a width of 0, which then overrides any uniform shorthand on that edge.
    void set_border_top_width(float w)    { border_top_.width = w;    border_top_set_ = true;    has_border_sides_ = true; }
    void set_border_right_width(float w)  { border_right_.width = w;  border_right_set_ = true;  has_border_sides_ = true; }
    void set_border_bottom_width(float w) { border_bottom_.width = w; border_bottom_set_ = true; has_border_sides_ = true; }
    void set_border_left_width(float w)   { border_left_.width = w;   border_left_set_ = true;   has_border_sides_ = true; }
    /// Per-side getters. The standalone setBorderTop/Right/... {Color,Width}
    /// bridge calls need to preserve the unrelated attribute when only one is
    /// being changed by a JSX prop diff.
    Color border_top_color() const { return border_top_.color; }
    float border_top_width() const { return border_top_.width; }
    Color border_right_color() const { return border_right_.color; }
    float border_right_width() const { return border_right_.width; }
    Color border_bottom_color() const { return border_bottom_.color; }
    float border_bottom_width() const { return border_bottom_.width; }
    Color border_left_color() const { return border_left_.color; }
    float border_left_width() const { return border_left_.width; }
    bool has_border_sides() const { return has_border_sides_; }
    /// Per-edge "explicitly set" probes. Yoga wiring uses these to distinguish
    /// "not set on this edge" (fall back to uniform `border_width()`) from
    /// "explicitly set to 0" (zero wins, even if the shorthand says 10).
    bool has_border_top_set() const { return border_top_set_; }
    bool has_border_right_set() const { return border_right_set_; }
    bool has_border_bottom_set() const { return border_bottom_set_; }
    bool has_border_left_set() const { return border_left_set_; }

    /// Per-corner border-radius (CSS border-top-left-radius, etc.). When
    /// transitioning from uniform `corner_radius_` to per-corner mode, seed
    /// the un-overridden corners from the uniform value so a sequence like
    /// `set_border_radius(10); set_corner_radius_tl(2);` renders as
    /// {2, 10, 10, 10} instead of {2, 0, 0, 0}.
    void set_corner_radius_tl(float r) { promote_uniform_to_per_corner(); corner_radii_[0] = r; corner_radii_pct_[0] = 0; has_corner_radii_ = true; }
    void set_corner_radius_tr(float r) { promote_uniform_to_per_corner(); corner_radii_[1] = r; corner_radii_pct_[1] = 0; has_corner_radii_ = true; }
    void set_corner_radius_bl(float r) { promote_uniform_to_per_corner(); corner_radii_[2] = r; corner_radii_pct_[2] = 0; has_corner_radii_ = true; }
    void set_corner_radius_br(float r) { promote_uniform_to_per_corner(); corner_radii_[3] = r; corner_radii_pct_[3] = 0; has_corner_radii_ = true; }
    /// Per-corner percent setters. Same semantics as set_border_radius_pct:
    /// paint time resolves to `pct * 0.01 * min(width, height)`.
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
    /// Replaces the whole stack with one layer. CSS `box-shadow: A` semantics —
    /// a declaration always supersedes the previous one rather than adding to
    /// it, so every existing single-shadow caller keeps its exact behavior.
    void set_box_shadow(float ox, float oy, float blur, float spread, Color c,
                        bool inset = false) {
        shadows_.assign(1, BoxShadow{ox, oy, blur, spread, c, inset});
    }
    /// Appends a layer, preserving CSS author order: `box-shadow: A, B` is
    /// set_box_shadow(A) followed by add_box_shadow(B). A is the first layer
    /// and therefore paints ON TOP of B — see the reverse iteration in
    /// View::paint_all. Layered shadows are how a design expresses depth: a
    /// wide soft halo plus a tight dark contact shadow. Keeping only the first
    /// layer keeps the halo and drops the contact shadow, which reads as flat.
    void add_box_shadow(float ox, float oy, float blur, float spread, Color c,
                        bool inset = false) {
        shadows_.push_back(BoxShadow{ox, oy, blur, spread, c, inset});
    }
    void clear_box_shadow() { shadows_.clear(); }
    bool has_box_shadow() const { return !shadows_.empty(); }
    /// The first (topmost) layer, or a default-constructed shadow when the
    /// stack is empty — callers that only ever set one shadow can keep
    /// treating this as "the" shadow.
    const BoxShadow& box_shadow() const {
        static const BoxShadow kEmpty{};
        return shadows_.empty() ? kEmpty : shadows_.front();
    }
    /// Every layer, in CSS author order (first paints on top).
    const std::vector<BoxShadow>& box_shadows() const { return shadows_; }

    /// RN iOS-legacy shadow* longhand setters. RN 0.71+ added `boxShadow` as
    /// the cross-platform path, but the four per-attribute setters still appear
    /// in legacy code and the React Native API surface. Pulp mirrors the
    /// per-attribute pattern the text-shadow longhand uses: each setter mutates
    /// one field of the shared BoxShadow struct so a JSX prop diff that touches
    /// one prop doesn't clobber the others.
    ///
    /// Any of these activates the shadow paint (matches React Native's
    /// behavior — setting any shadow prop activates it), creating a
    /// default-valued first layer if the stack is empty. RN has no
    /// multi-shadow longhand, so these address the first layer only.
    void set_box_shadow_color(Color c) {
        first_box_shadow().color = c;
    }
    void set_box_shadow_offset(float ox, float oy) {
        auto& s = first_box_shadow(); s.offset_x = ox; s.offset_y = oy;
    }
    void set_box_shadow_opacity(float a) {
        // RN's shadowOpacity is 0..1; overwrite the shadow color's normalized
        // alpha channel while preserving the existing RGB channels.
        first_box_shadow().color.a = a;
    }
    void set_box_shadow_radius(float r) {
        first_box_shadow().blur = r;
    }

    /// Generic click callback (fires on mouse-down, if set).
    std::function<void()> on_click;
    std::function<void(const MouseEvent&)> on_pointer_event;   ///< JS pointer event callback
    std::function<void(Point)> on_drag;   ///< JS pointermove during drag callback
    /// JS pointermove carrying full pointer identity (id + type + pressure).
    /// Distinct from `on_drag`, which collapses every move to
    /// pointerId:0/pointerType:'mouse'. Multi-touch hosts (iOS) call this so a
    /// scripted UI tracking >1 simultaneous pointer — e.g. OrbitControls
    /// pinch-zoom, which keys its dolly on two distinct touch pointerIds —
    /// receives each finger's move under its own identity. Desktop hosts that
    /// only ever deliver one pointer can keep driving `on_drag`.
    std::function<void(const MouseEvent&)> on_pointer_move;
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
    /// Paint queued overlays + the inspector paint hook. `painting_root` is the
    /// root View whose tree was just painted into `canvas` — it is forwarded to
    /// the inspector paint hook so the hook can gate. When a host paints
    /// multiple roots into separate surfaces (e.g. the floating
    /// InspectorWindow's own root vs. the main canvas root), the inspector
    /// overlay's selection box / handles / drop indicators must paint ONLY on
    /// the inspected root, never leak into the inspector window at the overlay's
    /// root coordinates (the "stray box" bug). nullptr means "root unknown" and
    /// the hook paints unconditionally (legacy callers).
    static void paint_overlays(canvas::Canvas& canvas, View* painting_root = nullptr);

    /// Inspector hooks — set by the inspector module to intercept input and paint
    /// without a circular dependency (view doesn't link inspect).
    ///
    /// The paint hook receives the painting root (see paint_overlays) so it can
    /// gate: the in-canvas overlay must paint only when the root being painted
    /// is the inspected root, not the floating inspector window's own root.
    static void set_inspector_paint_hook(
        std::function<void(canvas::Canvas&, View* painting_root)> hook);
    static void set_inspector_key_hook(std::function<bool(const KeyEvent&)> hook);
    static bool call_inspector_key_hook(const KeyEvent& e);

    /// Inspector mouse hook. Like the paint hook, the call carries the event's
    /// root View (`event_root`) so the installed hook can gate to the inspected
    /// canvas root. Without the gate, a secondary window's events (the floating
    /// InspectorWindow) leak into the canvas overlay:
    /// hovering/clicking/scrolling inside the inspector window would
    /// highlight/affect the canvas. The platform host passes the window's own
    /// root (the same source the paint hook uses for that host); the installed
    /// hook does `if (event_root && event_root != &inspector.inspected_root())
    /// return false;`. nullptr means "root unknown" (legacy/headless callers)
    /// and the hook runs unconditionally.
    static void set_inspector_mouse_hook(
        std::function<bool(const MouseEvent&, View* event_root)> hook);
    static bool call_inspector_mouse_hook(const MouseEvent& e,
                                          View* event_root = nullptr);

    /// Inspector text-input hook for inline Text-tool editing.
    /// The platform host routes UTF-8 character input here BEFORE the
    /// focused view so the inspector's inline text edit can consume it
    /// while the Text tool is active. Returns true when the inspector
    /// consumed the text (an inline edit is in progress); false otherwise,
    /// so normal text-input delivery to the focused widget proceeds.
    ///
    /// Carries the event's root View so the installed hook gates to the
    /// inspected canvas root, matching the mouse + paint hooks.
    static void set_inspector_text_hook(
        std::function<bool(const TextInputEvent&, View* event_root)> hook);
    static bool call_inspector_text_hook(const TextInputEvent& e,
                                         View* event_root = nullptr);

    /// Inspector cursor-affordance hook. The inspector overlay
    /// intercepts mouse-move before normal hit-testing, so the platform host
    /// cannot rely on the hit view's own `cursor()` to show move/resize
    /// affordances over a selected element. This hook lets the overlay
    /// override the cursor for the current pointer position: it returns the
    /// chosen `CursorStyle` cast to int when the overlay wants to drive the
    /// cursor (e.g. resize over a handle, move over the body), or -1 to defer
    /// to the normal hit-view `cursor()` path. The platform window host calls
    /// `call_inspector_cursor_hook` in its mouse-move handler and applies the
    /// returned style when it is >= 0. No-op (returns -1) when no hook is set.
    ///
    /// Carries the event's root View so the installed hook gates to the
    /// inspected canvas root, matching the mouse + paint hooks.
    /// Without the gate a mouse-move inside the floating InspectorWindow would
    /// drive the canvas overlay's cursor affordance.
    static void set_inspector_cursor_hook(
        std::function<int(const MouseEvent&, View* event_root)> hook);
    static int call_inspector_cursor_hook(const MouseEvent& e,
                                          View* event_root = nullptr);

    // ── Generalized overlay-click routing ────────────────────────────────
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
    /// Global input-focus slot. The platform window host calls
    /// `claim_input_focus()` when a click sets focus to a widget, and reads
    /// `focused_input_` for text-input dispatch. The View destructor auto-clears
    /// the slot if the focused widget is destroyed before focus is moved off it.
    /// Without this, the host's raw View* pointer dangles and the next keypress
    /// segfaults inside dynamic_cast<TextEditor*>(focused).
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
    /// Dismiss-path release. Releases the active overlay (if any) and fires its
    /// `on_overlay_dismissed` callback so React state can flip `setOpen(false)`
    /// to keep the JSX tree in sync. Called by the platform window host from
    /// the ESC keypath and the outside-click path. No-op if nothing claimed the
    /// slot.
    static void dismiss_active_overlay();
    /// Fired when the active overlay is dismissed via a framework
    /// auto-dismissal path (ESC / outside-click). Not fired for
    /// `release_overlay()` (JSX unmount / destructor). The bridge uses this to
    /// dispatch `__dispatch__(id, 'dismiss', 0)` so React
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

    // top/right/bottom/left accept either a px value (the single-arg setter) or
    // a percent value (the two-arg setter that records the unit). The Yoga
    // adapter dispatches on `top_unit_` / etc. and routes percent values through
    // YGNodeStyleSetPositionPercent, mirroring the FlexStyle::dim_width pattern
    // for the View positional fields.
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

    /// Children stably sorted by z_index() ascending — paint order (higher-z on top); hit_test walks it reversed. Exposed for tests.
    std::vector<View*> sorted_children_by_z_index() const;
    /// True iff children_ is already in that order — paint_all's fast path then iterates children_ directly, no sorted-copy alloc. Exposed for tests.
    bool children_in_z_order() const;

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
    /// Also tracks an "explicitly set" flag so the affine matrix path only
    /// honors the origin when the caller has actively chosen one. Without this,
    /// setTransform() call sites that never set an origin would silently start
    /// anchoring at center.
    void set_transform_origin(float x, float y) {
        origin_x_ = x; origin_y_ = y; origin_explicit_ = true;
    }
    float transform_origin_x() const { return origin_x_; }
    float transform_origin_y() const { return origin_y_; }
    bool transform_origin_explicit() const { return origin_explicit_; }

    /// Full 2D affine transform matrix on the View. Mirrors the
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

    /// True when this view paints under any non-identity render transform
    /// (scale / rotation / translate / skew / explicit matrix). Bounded
    /// invalidation escalates to a full repaint when this holds, since the plain
    /// parent-offset mapping to root space is only correct untransformed.
    bool has_render_transform() const {
        return has_transform_matrix_ || scale_ != 1.0f || rotation_deg_ != 0.0f ||
               translate_x_ != 0.0f || translate_y_ != 0.0f ||
               skew_x_ != 0.0f || skew_y_ != 0.0f;
    }

    /// True when this view paints under a pixel-spreading filter (blur or a
    /// filter-chain op) — it re-samples neighbors, so a child's changed region
    /// spreads past its mapped box. Bounded invalidation escalates to a full
    /// repaint when this holds on the view or an ancestor (no stale halo).
    bool has_filter_effect() const {
        return has_filter_chain() || filter_blur_ > 0.0f;
    }

    /// True when this container translates its CHILDREN's paint in a way a plain
    /// bounds-offset walk cannot model — e.g. a scrolled ScrollView. Bounded
    /// invalidation escalates to full when any ancestor reports true, so a
    /// scrolled sub-view never targets the wrong root rect. Default false: a
    /// flex/grid container positions children at their bounds origin.
    virtual bool applies_child_paint_offset() const { return false; }
    /// Returns the six affine components in (a,b,c,d,e,f) order; meaningful
    /// only when has_transform_matrix() is true.
    void get_transform_matrix(float& a, float& b, float& c,
                              float& d, float& e, float& f) const {
        a = transform_matrix_a_; b = transform_matrix_b_; c = transform_matrix_c_;
        d = transform_matrix_d_; e = transform_matrix_e_; f = transform_matrix_f_;
    }

    /// CSS filter: blur(px) — per-element blur. Legacy single-blur slot kept
    /// for API compatibility; the richer filter chain lives in `filter_chain_`
    /// below and supersedes this when non-empty.
    void set_filter_blur(float radius) { filter_blur_ = radius; }
    float filter_blur() const { return filter_blur_; }

    /// Full CSS filter chain. Each entry is one filter function (blur /
    /// brightness / contrast / grayscale / hue-rotate / invert / opacity /
    /// saturate / sepia / drop-shadow).
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

    /// CSS transitions + animations. `set_transitions` accepts the parsed
    /// shorthand; `transitions()` returns the slice for the dispatcher to
    /// consult when a property changes. Per the CSS spec, matching is a linear
    /// walk; later entries win when properties overlap (CSS cascade order).
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

    /// Active running animations on this View. Owned here so the per-View
    /// lifetime matches the View's.
    std::vector<CssAnimation>& active_animations() { return active_animations_; }
    const std::vector<CssAnimation>& active_animations() const { return active_animations_; }

    /// Advance every active CSS animation by `dt` seconds. When
    /// `animation_play_state_ == "paused"`, the call is
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

    /// Staged CSS animation control tokens. The web-compat-style-decl shim invokes
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
    /// is behind this View when it paints. Zero == no backdrop
    /// filter. Skia maps to `saveLayer(SaveLayerRec{ .fBackdrop = Blur })`.
    void set_backdrop_blur(float radius) { backdrop_blur_ = radius; }
    float backdrop_blur() const { return backdrop_blur_; }

    /// CSS `clip-path: path("...")`. Stores an SVG-path-d string; paint_all()
    /// installs it as a canvas clip via
    /// `Canvas::clip_path_svg` before painting children. Empty string
    /// clears the slot. URL refs (`url(#id)`) and named shape forms
    /// (`circle()`, `inset()`, `polygon()`) are deferred — only the
    /// `path("...")` form is honored today.
    void set_clip_path(const std::string& svg_path_d) { clip_path_ = svg_path_d; }
    const std::string& clip_path() const { return clip_path_; }
    bool has_clip_path() const { return !clip_path_.empty(); }

    /// CSS `mask-image: ...`. View opens a masked compositing layer when this
    /// slot is set to a non-`none` value. Backends without mask support route
    /// through the default save_layer path and preserve the value for bridge
    /// round-tripping.
    void set_mask_image(const std::string& value) { mask_image_ = value; }
    const std::string& mask_image() const { return mask_image_; }

    /// CSS `mask` shorthand. Stored verbatim alongside `mask_image_`; the
    /// longhand fan-out in the JS shim populates the image slot from this
    /// string.
    void set_mask(const std::string& value) { mask_ = value; }
    const std::string& mask() const { return mask_; }

    /// CSS `mask-size` (pairs with mask-image). Stored alongside mask_image_
    /// and passed to the canvas mask layer. Honored values: `auto` (default),
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

    /// CSS `object-fit` — controls how an `<img>` content's intrinsic size is
    /// fitted into its layout box. ImageView consumes this slot at paint time
    /// when the backend can measure the decoded image's natural size; otherwise
    /// it falls back to `fill` and stretches to bounds. Honored values:
    /// `fill` (default), `contain`, `cover`, `none`, and `scale-down`.
    void set_object_fit(const std::string& value) { object_fit_ = value; }
    const std::string& object_fit() const { return object_fit_; }

    /// CSS `object-position` — alignment of the object inside its
    /// content box when object-fit leaves blank space (e.g. `contain`
    /// in a wider box). ImageView consumes it alongside `object-fit` at paint
    /// time when a measurable image is available.
    void set_object_position(const std::string& value) { object_position_ = value; }
    const std::string& object_position() const { return object_position_; }

    /// CSS background sub-properties. These slots store the keyword for
    /// round-tripping; paint impact is partial — see notes:
    ///   • background-attachment: only `scroll` is conformant in pulp's
    ///     non-scrolling layout model. `fixed` / `local` need a scroll-
    ///     context coupling we don't have. Catalog: noop.
    ///   • background-clip: `text` would clip the bg paint to text glyphs
    ///     via SkBlendMode::kSrcIn. Other values are no-ops on solid bg.
    ///     Catalog: partial.
    ///   • background-origin: relevant only for repeating gradients (which
    ///     we don't paint per-tile). Catalog: noop.
    void set_background_attachment(std::string kw) { background_attachment_ = std::move(kw); }
    const std::string& background_attachment() const { return background_attachment_; }
    void set_background_clip(std::string kw)       { background_clip_ = std::move(kw); }
    const std::string& background_clip() const     { return background_clip_; }
    void set_background_origin(std::string kw)     { background_origin_ = std::move(kw); }
    const std::string& background_origin() const   { return background_origin_; }

    /// CSS background-position / background-size. Both are storage-only slots
    /// today: Pulp's solid-bg paint path doesn't honor position or size offsets
    /// (these only matter for url()/image-set() raster backgrounds). Storing
    /// the keyword keeps the round-trip honest so the image pipeline can honor
    /// the existing value without a JS-side change.
    void set_background_position(std::string kw)   { background_position_ = std::move(kw); }
    const std::string& background_position() const { return background_position_; }
    void set_background_size(std::string kw)       { background_size_ = std::move(kw); }
    const std::string& background_size() const     { return background_size_; }

    // Storage-only slots for CSS properties that the bridge accepts but View
    // does not fully consume. Each slot is round-trippable so harness tests can
    // verify the bridge accepts the value, and so a paint path can honor it
    // later without a JS-side change. Catalog status documents the
    // implementation depth (`partial` / `noop` / `wontfix`).
    void set_text_indent(float v)                  { text_indent_ = v; }
    float text_indent() const                      { return text_indent_; }
    void set_word_break(std::string kw)            { word_break_ = std::move(kw); }
    const std::string& word_break() const          { return word_break_; }

    /// CSS scroll-behavior + overscroll-behavior. Slot storage on every View;
    /// consumed by ScrollView::scroll_by + clamp_scroll_targets (`auto`
    /// triggers instant scroll, `smooth` enables animated scroll; `contain` /
    /// `none` for overscroll match Pulp's existing "clamp at content bounds, no
    /// scroll chaining" behavior). Non-ScrollView views round-trip the value
    /// but the paint pipeline doesn't act on it, matching CSS semantics for
    /// non-scrollable elements.
    void set_scroll_behavior(std::string kw)       { scroll_behavior_ = std::move(kw); }
    const std::string& scroll_behavior() const     { return scroll_behavior_; }
    void set_overscroll_behavior(std::string kw)   { overscroll_behavior_ = std::move(kw); }
    const std::string& overscroll_behavior() const { return overscroll_behavior_; }

    /// RN's Android-only `includeFontPadding`. Pure round-trip storage: Pulp's
    /// text-shaping pipeline doesn't add Android's vestigial vertical padding
    /// regardless of this value, so the bridge accepts the keyword + stores it
    /// (`el.style.includeFontPadding === false` reads back) and the paint
    /// pipeline ignores it. See compat.json rn/includeFontPadding notes for the
    /// honest CSS-subset rationale.
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
    /// Animation play-state. Stored separately from staged animation tokens and
    /// consumed by `tick_animations()`; `paused` freezes active animation
    /// timelines while other values advance normally.
    void set_animation_play_state(std::string kw)  { animation_play_state_ = std::move(kw); }
    const std::string& animation_play_state() const { return animation_play_state_; }

    /// Opt a custom view into per-vsync repaints — required for live content
    /// (meters/spectrum, automation-tracking values) or it looks frozen.
    void set_continuous_repaint(bool on) { wants_continuous_repaint_ = on; }
    bool wants_continuous_repaint() const { return wants_continuous_repaint_; }

    /// RN textShadow per-attribute storage. Storage-only; SkPaint shadow
    /// integration is not wired here. Each slot is round-trippable so a
    /// commitUpdate that touches only one of the three preserves the others
    /// (mirrors the per-side border slot pattern). A combined CSS shorthand
    /// helper can write all three slots atomically.
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
    /// Virtual so a widget can react to gaining/losing its host (e.g.
    /// NativeViewHost attaches/detaches its OS child view). An override MUST call
    /// the base to preserve the recursive propagation to children.
    virtual void set_window_host(WindowHost* host);
    WindowHost* window_host() const { return window_host_; }

    /// Back-reference to the PluginViewHost that owns this editor tree.
    /// Set by PluginViewHost when the root view is attached. Propagated to
    /// children. Virtual for the same reason as set_window_host(); an override
    /// MUST call the base to keep the propagation.
    virtual void set_plugin_view_host(PluginViewHost* host);
    PluginViewHost* plugin_view_host() const { return plugin_view_host_; }

    /// The runtime host-parameter accessor for this view tree, or nullptr in
    /// previews/screenshots (a view degrades to local state when null, exactly
    /// like a sandboxed native view). Set by the host that owns the tree —
    /// StateStore-backed natively, or ABI-backed under a foreign-host embed —
    /// and propagated to children like window_host()/plugin_view_host(). See
    /// pulp/view/host_param_surface.hpp for the tick-only call contract.
    void set_host_params(HostParamSurface* surface);
    HostParamSurface* host_params() const { return host_params_; }

    /// The runtime host command channel for this view tree, or nullptr. Same
    /// ownership/propagation/null-degradation semantics as host_params().
    void set_host_actions(HostActionSurface* surface);
    HostActionSurface* host_actions() const { return host_actions_; }

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
    /// Radial gradient. cx/cy are fractions of the box; radius_frac is a
    /// fraction of the larger box dimension (resolved at paint).
    void set_background_gradient_radial(float cx, float cy, float radius_frac,
                                         const std::vector<Color>& colors,
                                         const std::vector<float>& positions) {
        bg_gradient_colors_ = colors;
        bg_gradient_positions_ = positions;
        bg_gradient_type_ = 2;  // radial
        bg_grad_x0_ = cx; bg_grad_y0_ = cy;
        bg_grad_radius_ = radius_frac;
    }
    /// Conic (CSS conic-gradient / Figma angular). cx/cy are fractions of the
    /// box; start_angle is in radians (0 = +x axis, matching the canvas API).
    void set_background_gradient_conic(float cx, float cy, float start_angle,
                                        const std::vector<Color>& colors,
                                        const std::vector<float>& positions) {
        bg_gradient_colors_ = colors;
        bg_gradient_positions_ = positions;
        bg_gradient_type_ = 3;  // conic / sweep
        bg_grad_x0_ = cx; bg_grad_y0_ = cy;
        bg_grad_angle_ = start_angle;
    }
    void clear_background_gradient() { bg_gradient_type_ = 0; }
    bool has_background_gradient() const { return bg_gradient_type_ > 0; }
    /// 0=none, 1=linear, 2=radial, 3=conic. Exposed for tests/inspection.
    int background_gradient_type() const { return bg_gradient_type_; }
    /// Radial radius as a fraction of max(w,h). Exposed for tests/inspection.
    float background_gradient_radius() const { return bg_grad_radius_; }

    /// Text overflow: ellipsis (CSS text-overflow: ellipsis)
    void set_text_overflow_ellipsis(bool e) { text_ellipsis_ = e; }
    bool text_overflow_ellipsis() const { return text_ellipsis_; }

    /// Writing direction (CSS `direction` / RN `writingDirection`). LTR is the
    /// default; RTL flips text shaping (Skia paragraph_style.setTextDirection),
    /// Yoga's flow (YGDirectionRTL — flexDirection 'row' visually reverses),
    /// and textAlign 'auto' resolution at paint time. The View::direction_
    /// state propagates inheritance only when the caller plumbs it through the
    /// tree. Enum is tri-state; `auto_` currently resolves to the LTR fallback.
    enum class WritingDirection { ltr, rtl, auto_ };
    void set_direction(WritingDirection d) { direction_ = d; }
    WritingDirection direction() const { return direction_; }
    /// Resolve `auto_` → ltr (LTR fallback, until first-strong-character
    /// detection is wired). Use this at paint time when you need a
    /// concrete direction.
    WritingDirection resolved_direction() const {
        return direction_ == WritingDirection::auto_ ? WritingDirection::ltr : direction_;
    }

    /// White-space: nowrap (CSS `white-space: nowrap`). Generic
    /// flag so non-Label widgets (Button, custom text-bearing views) and
    /// text-shaper consumers can react to nowrap without dynamic_casting
    /// to a specific widget type. Label keeps its `multi_line_` flag in
    /// lock-step via WidgetBridge::setWhiteSpace, so existing callers keep
    /// working.
    // Full CSS `white-space` enum. Replaces the nowrap-only bool with the
    // spec's 6 keywords. Per CSS Text
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
    // set_white_space_nowrap() also keeps the WhiteSpaceMode enum in sync so
    // callers using the legacy setter don't leave white_space_mode() stale.
    // The enum is pinned to nowrap (true) or normal (false); we can't infer
    // pre/pre-wrap/etc. from a bool, so the legacy setter only ever produces
    // these two modes.
    void set_white_space_nowrap(bool n) {
        white_space_nowrap_ = n;
        white_space_mode_ = n ? WhiteSpaceMode::nowrap : WhiteSpaceMode::normal;
    }
    bool white_space_nowrap() const { return white_space_nowrap_; }

    void set_white_space_mode(WhiteSpaceMode m) {
        white_space_mode_ = m;
        // `pre` also pins nowrap: it preserves newlines and spaces while
        // disabling soft wrapping.
        white_space_nowrap_ = (m == WhiteSpaceMode::nowrap || m == WhiteSpaceMode::pre);
    }
    WhiteSpaceMode white_space_mode() const { return white_space_mode_; }

    /// CSS / RN `mix-blend-mode`. The blend mode applied when
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
        multi_directional_resize, ///< ✥ All-direction move/resize
        // CSS cursor keywords with native macOS NSCursor mappings. Other CSS
        // keywords without dedicated visuals (wait / help / progress / cell)
        // stay catalog-unsupported because no native cursor exists on macOS.
        alias,                    ///< CSS `alias` → NSCursor.dragLinkCursor
        copy,                     ///< CSS `copy` → NSCursor.dragCopyCursor
        zoom_in,                  ///< CSS `zoom-in` → NSCursor.zoomInCursor (macOS 10.15+)
        zoom_out,                 ///< CSS `zoom-out` → NSCursor.zoomOutCursor (macOS 10.15+)
        context_menu              ///< CSS `context-menu` → NSCursor.contextualMenuCursor
    };
    void set_cursor(CursorStyle c) { cursor_ = c; }
    CursorStyle cursor() const { return cursor_; }

    /// CSS `user-select`. Stored slot for the keyword the JS shim resolves to.
    /// Read by interactive widgets (TextEditor, Label) that participate in text
    /// selection. `auto_` is the spec default and resolves per-widget to
    /// whatever its native selectability is.
    enum class UserSelect { auto_, none, text, all, contain };
    void set_user_select(UserSelect s) { user_select_ = s; }
    UserSelect user_select() const { return user_select_; }

private:
    /// Recursively fire `on_frame_clock_changed()` on this view and every
    /// descendant. Used by `set_frame_clock()` so a clock installed on a root
    /// after the subtree was built reaches self-subscribing descendants.
    void notify_frame_clock_changed();

    /// Re-point every FrameClockBinding registered on this view at the
    /// currently reachable clock. Called non-virtually from
    /// `notify_frame_clock_changed()`, and bindings enrol themselves rather
    /// than being listed by hand, so no subclass can strand a binding on a
    /// stale clock by overriding a hook without chaining. No-op when nothing is
    /// bound.
    void sync_value_bindings();

    /// Enrol/withdraw a binding constructed with this view as its owner.
    /// FrameClockBinding calls these from its own ctor/dtor; nothing else does.
    void register_value_binding(FrameClockBinding* b);
    void unregister_value_binding(FrameClockBinding* b);
    friend class FrameClockBinding;

    /// Seed corner_radii_ from the uniform corner_radius_ on the first
    /// transition into per-corner mode. Idempotent: subsequent calls (when
    /// has_corner_radii_ is already true) are no-ops.
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
    std::shared_ptr<WidgetPainter> painter_;
    std::shared_ptr<WidgetMetrics> metrics_;
    /// Only the ROOT's copy is ever populated (start_drag walks up to find it),
    /// so concurrent trees cannot see each other's drag.
    std::unique_ptr<ActiveDrag> active_drag_;
    View* parent_ = nullptr;
    std::vector<std::unique_ptr<View>> children_;
    std::string id_;
    // Design-import anchor identity. Empty for views not constructed from an
    // imported tree. See set_anchor_id().
    std::string anchor_id_;
    std::uint64_t import_binding_instance_id_ = 0;
    std::shared_ptr<const std::uint64_t> import_binding_lifetime_token_;
    // Authored-source location (JSX file:line:col) from the React reconciler's
    // `__source` prop. Unset for non-imported views.
    std::optional<SourceLocation> source_loc_;
    // Last-paint-cycle timing. Both 0 until paint_all() has executed at least
    // once.
    std::uint32_t last_paint_self_ns_ = 0;
    std::uint32_t last_paint_with_children_ns_ = 0;
    AccessRole access_role_ = AccessRole::none;
    std::string access_label_;          // author-set (aria-label) — wins
    std::string derived_access_label_;  // content-derived (visible text)
    std::string access_value_;
    // ARIA state attributes (tri-state per spec).
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
    bool requires_gpu_host_ = false;
    bool contains_native_overlay_ = false;
    FrameClock* frame_clock_ = nullptr;
    // Lazily allocated on the first set_meter_source / set_scalar_source, so a
    // view that shows no live value costs one null pointer.
    std::unique_ptr<ViewValueBindings> value_bindings_;
    // Head of the intrusive list of every FrameClockBinding owned by this view —
    // the two above plus any a subclass holds (DesignFrameView's per-element
    // scalars). Intrusive so enrolling a binding never allocates.
    FrameClockBinding* value_binding_head_ = nullptr;

    // Visual properties
    float opacity_ = 1.0f;
    Color bg_color_{};
    bool has_bg_ = false;
    Color border_color_{};
    float border_width_ = 0;
    float corner_radius_ = 0;
    // borderRadius % support. When > 0, paint code resolves
    // effective radius as `corner_radius_pct_ * 0.01 * min(width, height)`.
    // 0 means "use plain corner_radius_ in px". Same pattern for the
    // per-corner radii_pct_ slots below.
    float corner_radius_pct_ = 0;
    // RN `borderCurve` corner shape.
    BorderCurve border_curve_ = BorderCurve::circular;
    bool has_border_ = false;
    BorderStyle border_style_ = BorderStyle::solid;
    // list-style cluster slots. Stored verbatim; paint-
    // time marker rendering is deferred. Defaults match CSS spec
    // (`disc` for the type, `outside` for the position, empty image).
    ListStyleType list_style_type_ = ListStyleType::disc;
    std::string list_style_image_{};
    ListStylePosition list_style_position_ = ListStylePosition::outside;
    // CSS / RN outline cluster. Defaults: outline_style_
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
    // Per-edge "explicitly set" flags, parallel to has_top_
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
    // % support paired with corner_radii_. >0 means use pct
    // resolution, 0 means use the px slot above. Sized 4 (TL, TR, BL, BR).
    float corner_radii_pct_[4] = {0, 0, 0, 0};
    bool has_corner_radii_ = false;
    Position position_ = Position::static_;
    float top_ = 0, right_ = 0, bottom_ = 0, left_ = 0;
    bool has_top_ = false, has_right_ = false, has_bottom_ = false, has_left_ = false;
    // Per-edge inset unit. `px` is the default; `percent` flows through to
    // YGNodeStyleSetPositionPercent in yoga_layout.cpp. Other DimensionUnit
    // values (vw/vh/etc.) round down to px at the bridge boundary today.
    DimensionUnit top_unit_ = DimensionUnit::px;
    DimensionUnit right_unit_ = DimensionUnit::px;
    DimensionUnit bottom_unit_ = DimensionUnit::px;
    DimensionUnit left_unit_ = DimensionUnit::px;
    int z_index_ = 0;
    // Default is `visible` to match CSS. Pulp previously
    // defaulted to `hidden`, which clipped absolutely-positioned children
    // (popovers, dropdowns, tooltips) to the parent's content bounds and
    // made them invisible whenever they extended outside. Plugins that
    // intentionally need clipping must call set_overflow(Overflow::hidden)
    // explicitly — same opt-in as `overflow:hidden` in CSS.
    Overflow overflow_ = Overflow::visible;
    /// CSS box-shadow layers in author order; empty means no shadow.
    std::vector<BoxShadow> shadows_;
    /// The first layer, materialized with default values when absent. Backs
    /// the RN longhand setters, which mutate one field at a time.
    BoxShadow& first_box_shadow() {
        if (shadows_.empty()) shadows_.emplace_back();
        return shadows_.front();
    }
    float scale_ = 1.0f;
    float translate_x_ = 0, translate_y_ = 0;
    float rotation_deg_ = 0;
    float skew_x_ = 0, skew_y_ = 0;
    float origin_x_ = 0.5f, origin_y_ = 0.5f;  // transform-origin (normalized)
    bool origin_explicit_ = false;  // has set_transform_origin been called?
    // Full 2D affine matrix. Identity by default; only applied
    // when has_transform_matrix_ is true. Stored in CanvasRenderingContext2D
    // (a,b,c,d,e,f) order:  [a c e / b d f / 0 0 1].
    float transform_matrix_a_ = 1.0f, transform_matrix_b_ = 0.0f,
          transform_matrix_c_ = 0.0f, transform_matrix_d_ = 1.0f,
          transform_matrix_e_ = 0.0f, transform_matrix_f_ = 0.0f;
    bool has_transform_matrix_ = false;
    float filter_blur_ = 0;
    std::vector<FilterOp> filter_chain_{};
    float backdrop_blur_ = 0;
    // CSS clip-path / mask storage. Paint-time consumption for clip_path_ via
    // Canvas::clip_path_svg (SkPath::FromSVGString on Skia, no-op fallback
    // elsewhere). mask_image_ / mask_ are consumed by mask-capable backends and
    // round-trip through storage elsewhere.
    std::string clip_path_;
    std::string mask_image_;
    std::string mask_;
    std::string mask_size_;     // CSS mask-size paired with mask_image_
    std::string appearance_;    // CSS appearance — storage-only no-op for Pulp custom widgets
    std::string object_fit_;      // CSS object-fit consumed by ImageView paint
    std::string object_position_; // CSS object-position paired with object-fit
    /// Transition specs + active animations.
    std::vector<TransitionSpec> transitions_{};
    std::vector<CssAnimation> active_animations_{};
    StagedAnimation staged_animation_{};
    std::string background_attachment_;  // noop today
    std::string background_clip_;        // partial (text deferred)
    std::string background_origin_;      // noop today
    std::string background_position_;    // storage-only (raster bg deferred)
    std::string background_size_;        // storage-only (raster bg deferred)

    // Storage-only catalog slots.
    float       text_indent_ = 0.0f;       // partial (paint deferred)
    std::string word_break_;               // partial (HarfBuzz feature deferred)
    std::string scroll_behavior_;          // ScrollView consumes
    std::string overscroll_behavior_;      // ScrollView clamp consumes
    bool        include_font_padding_ = true;  // Android legacy round-trip slot (paint ignores)
    std::string font_variant_;             // partial (HarfBuzz feature deferred)
    std::string writing_mode_;             // noop (Pulp horizontal-only)
    std::string isolation_;                // wontfix (no z-buffer)
    std::string resize_;                   // noop (no resize handles)
    std::string animation_play_state_;     // partial (tick_animations consumes)
    bool wants_continuous_repaint_ = false; // opt-in per-vsync repaint
    // RN textShadow* per-attribute storage slots. SkPaint shadow integration
    // deferred; storage path is round-trippable.
    std::string text_shadow_color_;
    float       text_shadow_dx_ = 0.0f;
    float       text_shadow_dy_ = 0.0f;
    float       text_shadow_radius_ = 0.0f;
    bool needs_layer_ = false;
    WindowHost* window_host_ = nullptr;
    PluginViewHost* plugin_view_host_ = nullptr;
    HostParamSurface* host_params_ = nullptr;
    HostActionSurface* host_actions_ = nullptr;

    // Running animate() tweens: our returned id is the FrameClock subscription
    // id; `tag` supports self-cancelling animations. Unsubscribed in ~View.
    // `clock` is cached at subscribe time so ~View can unsubscribe even after
    // this view is detached (frame_clock() walks parent_, which a removed child
    // has cleared to null — see the detached-child UAF the cache fixes).
    struct RunningAnimation { int clock_id = -1; std::string tag; FrameClock* clock = nullptr; };
    std::vector<RunningAnimation> animations_;
    std::shared_ptr<canvas::ViewEffect> effect_;
    int bg_gradient_type_ = 0;  // 0=none, 1=linear, 2=radial, 3=conic
    float bg_grad_x0_ = 0, bg_grad_y0_ = 0, bg_grad_x1_ = 0, bg_grad_y1_ = 1;
    float bg_grad_radius_ = 0.7071f;  // radial: fraction of max(w,h)
    float bg_grad_angle_ = 0.0f;      // conic: start angle in radians
    std::vector<Color> bg_gradient_colors_;
    std::vector<float> bg_gradient_positions_;
    std::string background_repeat_;  ///< CSS background-repeat keyword (storage-only)
    bool text_ellipsis_ = false;
    bool white_space_nowrap_ = false;
    WhiteSpaceMode white_space_mode_ = WhiteSpaceMode::normal;
    CursorStyle cursor_ = CursorStyle::default_;
    UserSelect user_select_ = UserSelect::auto_;
    WritingDirection direction_ = WritingDirection::auto_;
    // CSS / RN mix-blend-mode. Default kSrcOver (canvas
    // BlendMode::normal) is a paint-time no-op; any non-default value
    // forces a saveLayer() at paint time so the subtree composites back
    // through the requested blend mode.
    canvas::Canvas::BlendMode mix_blend_mode_ = canvas::Canvas::BlendMode::normal;

    // Pointer capture: pointer_id → this view receives all events for that pointer
    std::vector<int> captured_pointers_;
    std::vector<std::unique_ptr<GestureRecognizer>> gesture_recognizers_; std::unique_ptr<GestureArbiter> gesture_arbiter_;

    // CSS-style typography inheritance. Unset by default; only
    // populated when the bridge / app calls a set_inheritable_* setter.
    // These do not affect the View's own paint — only descendant Labels
    // (and other text widgets) consult them.
    std::optional<Color> inh_text_color_;
    std::optional<float> inh_font_size_;
    std::optional<float> inh_letter_spacing_;
    std::optional<int>   inh_font_weight_;
    std::optional<int>   inh_text_align_;
    std::optional<std::string> inh_font_family_;
};

// ── Accessibility exposure gate ──────────────────────────────────────────────
//
// See is_accessibility_element() in <pulp/view/accessibility.hpp> — the single
// predicate every platform bridge calls. It lives there because it has to look
// for the value/text interfaces, which are declared there. This file only
// classifies the ROLES.

/// Structural roles: a container announces the children underneath it, so it is
/// meaningful without a name of its own. Everything else is a leaf control that
/// must announce a name, a value, a state, or text — otherwise it is a role and
/// nothing else, which is a WCAG 4.1.2 (Name, Role, Value) failure.
constexpr bool is_structural_access_role(View::AccessRole role) {
    switch (role) {
        case View::AccessRole::group:
        case View::AccessRole::dialog:
        case View::AccessRole::list:
        case View::AccessRole::table:
        case View::AccessRole::row:
        case View::AccessRole::menu:
        case View::AccessRole::tab_list:
            return true;
        default:
            return false;
    }
}

} // namespace pulp::view
