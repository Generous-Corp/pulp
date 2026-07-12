#pragma once

/// @file web_event_translate.hpp
/// Browser-event translation and the CSS-pixel ↔ root-coordinate mapping used
/// by the Emscripten `BrowserWindowHost` (core/view/platform/web/).
///
/// Nothing here depends on Emscripten. The coordinate math, the event-shape
/// mapping, and the View-tree routing are plain C++ so they compile and are
/// unit-testable on a native host (test/test_window_host_web.cpp);
/// `web_input.cpp` only owns the DOM listener plumbing that feeds them.

#include <pulp/view/input_events.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>

namespace pulp::view::web {

// ── Coordinate mapping ──────────────────────────────────────────────────────

/// Maps CSS pixels — the unit every browser pointer event reports — to
/// root-view coordinates.
///
/// HiDPI is deliberately absent from this transform. The canvas backing store
/// is sized `css * devicePixelRatio` and Skia re-applies the same factor at
/// paint time (`SkiaSurface` scale factor), so root coordinates stay in CSS
/// pixels: dividing pointer coordinates by the ratio here would scale the
/// input twice.
///
/// With a design viewport (`design_width`/`design_height` > 0) the root is
/// pinned at design size and letterboxed into the canvas, exactly as the macOS
/// GPU host does, so the mapping becomes the inverse of
/// `WindowHost::compute_design_viewport_transform()`.
struct ViewportMapping {
    float css_width = 0;
    float css_height = 0;
    float design_width = 0;
    float design_height = 0;
    bool top_align = false;

    bool has_design_viewport() const {
        return design_width > 0.0f && design_height > 0.0f &&
               css_width > 0.0f && css_height > 0.0f;
    }

    /// Root → CSS transform, or false when no design viewport is active
    /// (identity; outputs untouched).
    bool transform(float& sx, float& sy, float& tx, float& ty) const {
        if (!has_design_viewport()) return false;
        return WindowHost::compute_design_viewport_transform(
            css_width, css_height, design_width, design_height, sx, sy, tx, ty,
            top_align);
    }
};

/// CSS-pixel point → root-view point. Identity without a design viewport.
inline Point css_to_root(const ViewportMapping& mapping, Point css) {
    float sx = 1, sy = 1, tx = 0, ty = 0;
    if (!mapping.transform(sx, sy, tx, ty) || sx <= 0.0f || sy <= 0.0f)
        return css;
    return {(css.x - tx) / sx, (css.y - ty) / sy};
}

/// Root-view point → CSS-pixel point. The exact inverse of css_to_root().
inline Point root_to_css(const ViewportMapping& mapping, Point root) {
    float sx = 1, sy = 1, tx = 0, ty = 0;
    if (!mapping.transform(sx, sy, tx, ty) || sx <= 0.0f || sy <= 0.0f)
        return root;
    return {root.x * sx + tx, root.y * sy + ty};
}

// ── Backing store ───────────────────────────────────────────────────────────

/// Canvas backing-store size and the scale factor handed to the Skia surface.
struct BackingStore {
    uint32_t width = 0;         ///< canvas.width  (device pixels)
    uint32_t height = 0;        ///< canvas.height (device pixels)
    float scale_factor = 1.0f;  ///< SkiaSurface scale — the devicePixelRatio
};

/// The one place the HiDPI factor is applied. `canvas.style.{width,height}`
/// stay at the CSS size, `canvas.{width,height}` become the returned device
/// size, and `scale_factor` is the ratio — so paint scales up while input
/// coordinates stay in CSS pixels.
inline BackingStore compute_backing_store(float css_width, float css_height,
                                          float device_pixel_ratio) {
    const float dpr = device_pixel_ratio > 0.0f ? device_pixel_ratio : 1.0f;
    BackingStore store;
    store.width = static_cast<uint32_t>(
        std::lround(static_cast<double>(std::max(0.0f, css_width)) * dpr));
    store.height = static_cast<uint32_t>(
        std::lround(static_cast<double>(std::max(0.0f, css_height)) * dpr));
    store.scale_factor = dpr;
    return store;
}

// ── Raw browser event shapes ────────────────────────────────────────────────

/// W3C PointerEvent phase.
enum class BrowserPointerPhase : uint8_t { down, move, up, cancel };

/// W3C PointerEvent.pointerType.
enum class BrowserPointerType : uint8_t { mouse, touch, pen };

/// W3C WheelEvent.deltaMode.
enum class BrowserDeltaMode : uint8_t { pixel = 0, line = 1, page = 2 };

struct BrowserModifiers {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool meta = false;
};

struct BrowserPointerEvent {
    float css_x = 0;
    float css_y = 0;
    int button = 0;   ///< MouseEvent.button — 0 left, 1 middle, 2 right
    int buttons = 0;  ///< MouseEvent.buttons bitmask (0 = no button held)
    int pointer_id = 0;
    bool is_primary = true;
    int click_count = 1;  ///< MouseEvent.detail
    BrowserPointerPhase phase = BrowserPointerPhase::down;
    BrowserPointerType pointer_type = BrowserPointerType::mouse;
    float pressure = 0.5f;
    BrowserModifiers modifiers;
};

struct BrowserWheelEvent {
    float css_x = 0;
    float css_y = 0;
    float delta_x = 0;
    float delta_y = 0;
    BrowserDeltaMode delta_mode = BrowserDeltaMode::pixel;
    BrowserModifiers modifiers;
};

struct BrowserKeyEvent {
    std::string key;  ///< KeyboardEvent.key ("a", "Shift", "ArrowLeft", " ", …)
    bool is_down = true;
    bool is_repeat = false;
    BrowserModifiers modifiers;
};

// ── Translation ─────────────────────────────────────────────────────────────

/// A wheel line is one text line; a wheel page falls back to this when the
/// mapping carries no CSS height.
inline constexpr float kWheelLinePixels = 16.0f;
inline constexpr float kWheelPageFallbackPixels = 400.0f;

/// `metaKey` is Command on a Mac browser and the Super/Windows key elsewhere,
/// so it sets both kModMeta and kModCmd. A wasm build is not `__APPLE__`, so
/// `is_main_modifier()` resolves to Ctrl there — a shortcut that must follow
/// the host OS reads isCmdDown()/isCtrlDown() explicitly.
inline uint16_t translate_modifiers(const BrowserModifiers& mods) {
    uint16_t out = kModNone;
    if (mods.shift) out |= kModShift;
    if (mods.ctrl) out |= kModCtrl;
    if (mods.alt) out |= kModAlt;
    if (mods.meta) out |= static_cast<uint16_t>(kModMeta | kModCmd);
    return out;
}

inline MouseButton translate_button(int w3c_button) {
    switch (w3c_button) {
        case 1: return MouseButton::middle;
        case 2: return MouseButton::right;
        case 0: return MouseButton::left;
        default: return MouseButton::left;
    }
}

inline PointerType translate_pointer_type(BrowserPointerType type) {
    switch (type) {
        case BrowserPointerType::touch: return PointerType::touch;
        case BrowserPointerType::pen: return PointerType::pen;
        default: return PointerType::mouse;
    }
}

/// Browser pointer event → Pulp MouseEvent in ROOT coordinates.
///
/// Both `position` and `window_position` carry the root point. The host
/// overwrites `position` with the hit view's local coordinates before dispatch
/// (the macOS host's convention), which it can only do once hit-testing has
/// picked a target.
inline MouseEvent translate_pointer(const BrowserPointerEvent& event,
                                    const ViewportMapping& mapping) {
    const Point root = css_to_root(mapping, {event.css_x, event.css_y});
    const bool button_held = event.buttons != 0;

    MouseEvent out;
    out.position = root;
    out.window_position = root;
    out.button = translate_button(event.button);
    out.modifiers = translate_modifiers(event.modifiers);
    out.pointer_id = event.is_primary ? 0 : event.pointer_id;
    out.click_count = std::max(1, event.click_count);
    out.pointer_type = translate_pointer_type(event.pointer_type);
    out.pressure = event.pressure;

    switch (event.phase) {
        case BrowserPointerPhase::down:
            out.is_down = true;
            out.phase = MousePhase::press;
            break;
        case BrowserPointerPhase::move:
            out.is_down = button_held;
            out.phase = button_held ? MousePhase::drag : MousePhase::hover;
            break;
        case BrowserPointerPhase::up:
            out.is_down = false;
            out.phase = MousePhase::release;
            break;
        case BrowserPointerPhase::cancel:
            out.is_down = false;
            out.phase = MousePhase::release;
            out.is_cancelled = true;
            break;
    }
    return out;
}

/// Browser wheel event → Pulp wheel MouseEvent in ROOT coordinates.
///
/// Sign is preserved: WheelEvent.deltaY is positive when the content scrolls
/// DOWN, and Pulp's scroll_delta_y is positive-is-down too (ScrollView adds it
/// straight onto its scroll offset). Non-pixel delta modes are normalized to
/// CSS pixels.
inline MouseEvent translate_wheel(const BrowserWheelEvent& event,
                                  const ViewportMapping& mapping) {
    float unit = 1.0f;
    if (event.delta_mode == BrowserDeltaMode::line) {
        unit = kWheelLinePixels;
    } else if (event.delta_mode == BrowserDeltaMode::page) {
        unit = mapping.css_height > 0.0f ? mapping.css_height
                                         : kWheelPageFallbackPixels;
    }

    const Point root = css_to_root(mapping, {event.css_x, event.css_y});

    MouseEvent out;
    out.position = root;
    out.window_position = root;
    out.modifiers = translate_modifiers(event.modifiers);
    out.is_wheel = true;
    out.scroll_delta_x = event.delta_x * unit;
    out.scroll_delta_y = event.delta_y * unit;
    return out;
}

/// KeyboardEvent.key → KeyCode. Unmapped names (modifier keys, media keys, …)
/// return KeyCode::unknown, which still reaches the view tree as a KeyEvent so
/// a widget can observe the modifier state.
inline KeyCode translate_key_code(const std::string& key) {
    if (key.size() == 1) {
        const unsigned char ch = static_cast<unsigned char>(key[0]);
        if (ch >= 'A' && ch <= 'Z')
            return static_cast<KeyCode>(ch - 'A' + 'a');
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == ' ' ||
            ch == ';' || ch == '\'')
            return static_cast<KeyCode>(ch);
        return KeyCode::unknown;
    }
    if (key == "ArrowLeft") return KeyCode::left;
    if (key == "ArrowRight") return KeyCode::right;
    if (key == "ArrowUp") return KeyCode::up;
    if (key == "ArrowDown") return KeyCode::down;
    if (key == "Home") return KeyCode::home;
    if (key == "End") return KeyCode::end_;
    if (key == "PageUp") return KeyCode::page_up;
    if (key == "PageDown") return KeyCode::page_down;
    if (key == "Backspace") return KeyCode::backspace;
    if (key == "Delete") return KeyCode::delete_;
    if (key == "Tab") return KeyCode::tab;
    if (key == "Enter") return KeyCode::enter;
    if (key == "Escape") return KeyCode::escape;
    if (key.size() >= 2 && key[0] == 'F') {
        const std::string digits = key.substr(1);
        if (digits.find_first_not_of("0123456789") == std::string::npos) {
            const int n = std::stoi(digits);
            if (n >= 1 && n <= 12)
                return static_cast<KeyCode>(static_cast<int>(KeyCode::f1) + n - 1);
        }
    }
    return KeyCode::unknown;
}

inline KeyEvent translate_key(const BrowserKeyEvent& event) {
    KeyEvent out;
    out.key = translate_key_code(event.key);
    out.modifiers = translate_modifiers(event.modifiers);
    out.is_down = event.is_down;
    out.is_repeat = event.is_repeat;
    return out;
}

/// The text a key-down should also deliver as a TextInputEvent, or "" when the
/// key produces no character.
///
/// KeyboardEvent.key is the printable character itself for character keys and a
/// multi-character NAME ("Enter", "ArrowLeft") otherwise; a non-ASCII character
/// is a multi-byte UTF-8 string whose first byte has the high bit set, which is
/// how it stays distinguishable from a name. Ctrl/Meta chords are commands, not
/// text.
inline std::string text_for_key(const BrowserKeyEvent& event) {
    if (!event.is_down) return {};
    if (event.modifiers.ctrl || event.modifiers.meta) return {};
    if (event.key.empty()) return {};
    if (event.key.size() == 1) {
        const unsigned char ch = static_cast<unsigned char>(event.key[0]);
        return (ch >= 0x20 && ch != 0x7f) ? event.key : std::string{};
    }
    if (static_cast<unsigned char>(event.key[0]) & 0x80) return event.key;
    return {};
}

// ── View-tree routing ───────────────────────────────────────────────────────

/// Routes translated browser events into a live View tree.
///
/// Every coordinate passes through `css_to_root()` exactly once, at the top of
/// each handler — the single chokepoint that keeps hit-testing aligned with
/// paint under a design viewport.
class WebInputRouter {
public:
    explicit WebInputRouter(View& root) : root_(root) {}

    void set_mapping(const ViewportMapping& mapping) { mapping_ = mapping; }
    const ViewportMapping& mapping() const { return mapping_; }

    /// Called after any handler that changed view state, so the host can mark
    /// the surface dirty.
    void set_dirty_callback(std::function<void()> cb) {
        on_dirty_ = std::move(cb);
    }

    /// Forget cached drag / hover / focus targets. The host calls this when the
    /// view tree is rebuilt and the raw pointers may dangle.
    void invalidate() {
        drag_target_ = nullptr;
        hover_target_ = nullptr;
        focus_target_ = nullptr;
    }

    View* focused_view() const { return focus_target_; }
    View* drag_target() const { return drag_target_; }

    bool handle_pointer(const BrowserPointerEvent& event) {
        const MouseEvent me = translate_pointer(event, mapping_);
        switch (event.phase) {
            case BrowserPointerPhase::down: return pointer_down(me);
            case BrowserPointerPhase::move: return pointer_move(me);
            case BrowserPointerPhase::up: return pointer_up(me, false);
            case BrowserPointerPhase::cancel: return pointer_up(me, true);
        }
        return false;
    }

    bool handle_wheel(const BrowserWheelEvent& event) {
        MouseEvent me = translate_wheel(event, mapping_);
        View* target = root_.hit_test(me.position);
        if (!target) {
            // An empty patch of a scroll pane has no hit-testable child under
            // it; route to the scroll container the pointer is over so the
            // wheel works anywhere in the pane without clicking first.
            target = find_wheel_scroll_view_at(root_, me.position);
            if (!target) return false;
            target->on_mouse_event(me);
            target->layout_children();
            mark_dirty();
            return true;
        }
        target->on_mouse_event(me);
        target->on_wheel(me.scroll_delta_y);
        mark_dirty();
        return true;
    }

    bool handle_key(const BrowserKeyEvent& event) {
        View* target = focus_target_ ? focus_target_ : &root_;
        const KeyEvent ke = translate_key(event);
        bool handled = target->on_key_event(ke);

        const std::string text = text_for_key(event);
        if (!text.empty() && focus_target_) {
            TextInputEvent te;
            te.text = text;
            focus_target_->on_text_input(te);
            handled = true;
        }
        if (handled) mark_dirty();
        return handled;
    }

    /// Composed text from a DOM `input` / IME commit.
    ///
    /// NOT DRIVEN BY THE BROWSER YET. `web_input.cpp` exports the C entry point
    /// (`pulp_web_on_text`) but installs no `input` / `compositionend` listener,
    /// so nothing in-tree feeds this. The only text that reaches a focused view
    /// today is `handle_key()`'s printable-key fallback above, which cannot
    /// express an IME composition. This is the seam an IME implementation plugs
    /// into (attach a hidden contenteditable / `<input>`, forward its committed
    /// text to `pulp_web_on_text`); until then it is exercised only by
    /// test_window_host_web.cpp.
    bool handle_text(const std::string& text) {
        if (text.empty() || !focus_target_) return false;
        TextInputEvent te;
        te.text = text;
        focus_target_->on_text_input(te);
        mark_dirty();
        return true;
    }

private:
    void mark_dirty() {
        if (on_dirty_) on_dirty_();
    }

    void set_focus(View* target) {
        if (focus_target_ == target) return;
        if (focus_target_) {
            focus_target_->on_focus_changed(false);
            focus_target_->release_input_focus();
        }
        focus_target_ = target;
        if (focus_target_) {
            focus_target_->on_focus_changed(true);
            focus_target_->claim_input_focus();
        }
    }

    bool pointer_down(const MouseEvent& me) {
        // An active overlay (ComboBox popup, claimed popover) hit-tests first;
        // a click outside it dismisses it and then falls through to the view
        // underneath, matching the macOS host.
        if (auto* overlay = View::active_overlay_) {
            if (!overlay->overlay_contains(me.window_position))
                View::dismiss_active_overlay();
        }

        if (me.button == MouseButton::right &&
            dispatch_context_menu(root_, me.window_position)) {
            mark_dirty();
            return true;
        }

        View* target = root_.hit_test(me.window_position);
        ComboBox::notify_global_click(target);
        if (!target) {
            set_focus(nullptr);
            mark_dirty();
            return false;
        }

        set_focus(target->focusable() ? target : nullptr);

        MouseEvent local_event = me;
        local_event.position = point_to_local(me.window_position, target, &root_);
        target->on_mouse_event(local_event);
        target->on_mouse_down(local_event.position);
        drag_target_ = target;
        mark_dirty();
        return true;
    }

    bool pointer_move(const MouseEvent& me) {
        if (drag_target_) {
            MouseEvent local_event = me;
            local_event.position =
                point_to_local(me.window_position, drag_target_, &root_);
            local_event.is_down = true;
            local_event.phase = MousePhase::drag;
            drag_target_->on_mouse_event(local_event);
            drag_target_->on_mouse_drag(local_event.position);
            mark_dirty();
            return true;
        }

        View* target = root_.hit_test(me.window_position);
        if (target != hover_target_) {
            if (hover_target_) hover_target_->set_hovered(false);
            hover_target_ = target;
            if (hover_target_) hover_target_->set_hovered(true);
            mark_dirty();
        }
        if (!target) return false;

        MouseEvent local_event = me;
        local_event.position = point_to_local(me.window_position, target, &root_);
        target->on_mouse_event(local_event);
        target->on_hover_move(local_event.position);
        mark_dirty();
        return true;
    }

    bool pointer_up(const MouseEvent& me, bool cancelled) {
        if (!drag_target_) return false;
        View* target = drag_target_;
        drag_target_ = nullptr;

        MouseEvent local_event = me;
        local_event.position = point_to_local(me.window_position, target, &root_);
        target->on_mouse_event(local_event);
        if (cancelled)
            target->on_mouse_cancel(local_event.position);
        else
            target->on_mouse_up(local_event.position);
        mark_dirty();
        return true;
    }

    View& root_;
    ViewportMapping mapping_;
    std::function<void()> on_dirty_;
    View* drag_target_ = nullptr;
    View* hover_target_ = nullptr;
    View* focus_target_ = nullptr;
};

// ── Host installation (Emscripten only) ─────────────────────────────────────

/// Register the Emscripten `BrowserWindowHost` factory so `WindowHost::create()`
/// returns a canvas-backed host. `canvas_selector` is a CSS selector for the
/// target HTMLCanvasElement. Defined in `platform/web/window_host_web.cpp`,
/// which is compiled only for Emscripten targets.
void install_browser_window_host(std::string canvas_selector = "#canvas");

/// Attach/detach the DOM listeners (pointer, wheel, key, resize) that feed
/// `router`. Defined in `platform/web/web_input.cpp` — Emscripten only.
void install_web_input_listeners(const std::string& canvas_selector,
                                 WebInputRouter& router);
void remove_web_input_listeners(const std::string& canvas_selector);

/// The canvas element's CSS size changed. Called by the resize listener in
/// `web_input.cpp`; handled by the live host in `window_host_web.cpp`.
void notify_browser_host_resized(float css_width, float css_height);

/// Is the live browser host's GPU surface renderable RIGHT NOW?
///
/// False while the WebGL context is lost — the browser can revoke it at any
/// time (context-cap eviction, GPU-process crash, backgrounded mobile tab), and
/// the Ganesh surface then abandons its Skia context and refuses to paint until
/// `webglcontextrestored` arrives. Exposed so a host page (and the browser
/// pixel fixture) can observe and assert that recovery instead of guessing from
/// a frozen canvas. False when no host is installed. Emscripten only.
bool browser_host_gpu_available();

/// Current devicePixelRatio, and the canvas element's CSS size. Emscripten only.
float device_pixel_ratio();
bool canvas_css_size(const std::string& canvas_selector, float& css_width,
                     float& css_height);

/// Size the canvas element: backing store in device pixels, CSS box in CSS
/// pixels. Emscripten only.
void set_canvas_size(const std::string& canvas_selector,
                     const BackingStore& backing, float css_width,
                     float css_height);

/// Show/hide the canvas element (`style.display`). Emscripten only.
void set_canvas_visible(const std::string& canvas_selector, bool visible);

}  // namespace pulp::view::web
