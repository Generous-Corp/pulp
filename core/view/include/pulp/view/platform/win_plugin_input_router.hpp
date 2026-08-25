#pragma once

// win_plugin_input_router.hpp — the Windows plug-in editor's pointer/keyboard
// state machine, lifted out of plugin_view_host_win.cpp (WAH-6).
//
// ── Why this header contains no Win32 ────────────────────────────────────────
//
// This is the most re-entrancy-sensitive code in the Windows host. Every one of
// these handlers can be re-entered synchronously from inside itself: `SetFocus`,
// `SetCapture` and `ReleaseCapture` send messages back through the same wndproc
// on the same thread, and a widget's gesture callback may pump a nested message
// loop. The generation/terminal protocol in `PointerSession` exists precisely to
// survive that, and it is exactly the kind of logic that fails in the field
// rather than at compile time.
//
// It used to be unreachable by any test, because it sat in a `_WIN32`-only TU
// and Pulp's required CI gate is macOS. So the split here is deliberately NOT
// "move the code to another Windows file":
//
//   * the ORDERING and RE-ENTRANCY rules live here, platform-free, and run on
//     every macOS CI job through `test_win_plugin_input_router.cpp`;
//   * the native side effects — `SetCapture`, `SetFocus`, `ReleaseCapture`,
//     `TrackMouseEvent` — are named operations on `InputRouterHost`, which the
//     Windows host implements with the real Win32 calls and a test implements
//     with a recorder;
//   * message DECODING (LPARAM unpack, `MK_*` masks, virtual-key mapping, wheel
//     deltas, the DPI/design-viewport transform) stays in the host, on top of
//     the pure helpers in `win_pointer_input.hpp`.
//
// The rule for anything added here: if it needs `<windows.h>`, it belongs in the
// host or behind an `InputRouterHost` method — not in this file.

#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/platform/win_pointer_input.hpp>
#include <pulp/view/ui_components.hpp>  // ComboBox::notify_global_click
#include <pulp/view/view.hpp>
#include <pulp/runtime/log.hpp>

#include <cstdint>
#include <exception>
#include <string>

namespace pulp::view::win_input {

/// The native operations the router needs but must not perform itself.
///
/// Every method is a side effect on the owning HWND, and every one of them can
/// re-enter the router synchronously on Windows — `input_capture_pointer()` and
/// `input_release_pointer_capture()` both deliver `WM_CAPTURECHANGED` before
/// they return, and `input_take_keyboard_focus()` delivers `WM_SETFOCUS`. The
/// router is written to tolerate that; an implementation must not try to hide
/// it by deferring the call.
class InputRouterHost {
public:
    virtual ~InputRouterHost() = default;

    /// Root of the view tree events are dispatched into.
    virtual View& input_root() noexcept = 0;

    /// `SetCapture(hwnd)` — route pointer input here until capture is released.
    virtual void input_capture_pointer() = 0;

    /// `ReleaseCapture()`, but ONLY if this host currently holds it. Releasing
    /// capture we do not own would steal it from whoever does.
    virtual void input_release_pointer_capture() = 0;

    /// `SetFocus(hwnd)` — take native keyboard focus for the editor window.
    virtual void input_take_keyboard_focus() = 0;

    /// `TrackMouseEvent(TME_LEAVE)`. Returns whether tracking is now armed; a
    /// false return makes the router retry on the next move rather than assume
    /// a leave will arrive.
    virtual bool input_begin_mouse_leave_tracking() = 0;

    /// Coalesced repaint request, called after any input that can change pixels.
    virtual void input_request_repaint() = 0;
};

/// Encode one Unicode code point, supplied as one or two UTF-16 code units,
/// as UTF-8. Returns empty for an unpaired surrogate or an out-of-range value.
///
/// Hand-rolled rather than routed through `WideCharToMultiByte` so the text
/// path stays platform-free and testable. The input is at most one code point,
/// so this is the whole conversion — not a general UTF-16 decoder.
inline std::string utf16_code_point_to_utf8(char16_t high, char16_t low) {
    std::uint32_t cp = 0;
    if (low != 0) {
        const bool valid_pair = high >= 0xD800 && high <= 0xDBFF &&
                                low >= 0xDC00 && low <= 0xDFFF;
        if (!valid_pair) return {};
        cp = 0x10000u + ((static_cast<std::uint32_t>(high) - 0xD800u) << 10) +
             (static_cast<std::uint32_t>(low) - 0xDC00u);
    } else {
        // A lone surrogate is not a code point; dropping it is correct and
        // keeps invalid UTF-8 out of the text channel.
        if (high >= 0xD800 && high <= 0xDFFF) return {};
        cp = high;
    }

    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0u | (cp >> 6));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0u | (cp >> 12));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else {
        out += static_cast<char>(0xF0u | (cp >> 18));
        out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    }
    return out;
}

/// Pointer/keyboard/text routing for one plug-in editor window.
///
/// Owns the gesture state — the `PointerSession`, the captured drag target, the
/// last pointer position, leave-tracking and the pending high surrogate — so
/// the host no longer carries five loose fields whose invariants were only
/// expressed by the order of statements in its handlers.
///
/// All methods run on the UI thread. Points are already in ROOT (layout)
/// coordinates: the DPI scale and the design-viewport transform are the host's,
/// so the router never sees a physical pixel.
class PluginInputRouter {
public:
    explicit PluginInputRouter(InputRouterHost& host) noexcept : host_(host) {}

    PluginInputRouter(const PluginInputRouter&) = delete;
    PluginInputRouter& operator=(const PluginInputRouter&) = delete;

    // ── Pointer ─────────────────────────────────────────────────────────────

    void on_mouse_down(Point pt, MouseButton button, std::uint16_t modifiers) {
        std::uint64_t generation = 0;
        try {
            // This host owns one capture bracket. Close a prior button before
            // accepting a chord mate instead of overwriting its target.
            if (session_.active()) cancel_gesture();
            if (!session_.begin(button)) return;
            generation = session_.generation();
            const auto accepts_original = [this, generation, button] {
                return session_.accepts(generation, button);
            };
            last_point_ = pt;
            View& root = host_.input_root();
            // Outside presses must dismiss this editor's generalized overlay
            // even when a gesture recognizer consumes the press below.
            const auto overlay_press = route_press_to_active_overlay(root, pt);
            if (overlay_press.routing == OverlayPressRouting::routed)
                drag_target_.set(overlay_press.target);
            MouseEvent gesture_event;
            gesture_event.position = pt;
            gesture_event.window_position = pt;
            gesture_event.button = button;
            gesture_event.modifiers = modifiers;
            gesture_event.is_down = true;
            gesture_event.phase = MousePhase::press;
            if (yield_to_gesture(gesture_event)) {
                // A synchronous gesture callback may have terminalized this
                // bracket. Capture only for the still-current claimed session.
                if (accepts_original()) host_.input_capture_pointer();
                return;
            }
            if (!accepts_original()) return;

            // Consult the generalized overlay slot before the regular hit
            // test, so a React / imported-design popover both receives clicks
            // aimed at it and is dismissed by a click outside it. This host
            // previously handled only the native ComboBox mechanism below,
            // which left such a popover open forever.
            if (overlay_press.routing != OverlayPressRouting::routed)
                drag_target_.set(root.hit_test(pt));
            View* drag_target = drag_target_.live_in(root);
            if (button == MouseButton::left)
                ComboBox::notify_global_click(drag_target);
            if (!accepts_original()) return;
            drag_target = drag_target_.live_in(root);
            if (!drag_target) {
                cancel_gesture();
                return;
            }
            host_.input_take_keyboard_focus();
            if (!accepts_original()) return;
            drag_target = drag_target_.live_in(root);
            if (!drag_target) {
                cancel_gesture();
                return;
            }
            if (!transfer_input_focus(root, drag_target)) {
                if (accepts_original()) {
                    drag_target_.reset();
                    cancel_gesture();
                }
                return;
            }
            if (!accepts_original()) return;
            host_.input_capture_pointer();
            if (!accepts_original()) return;
            MouseDownHost down_host;
            down_host.should_continue = accepts_original;
            const bool target_alive =
                deliver_mouse_down(root, drag_target_.live_in(root), pt, modifiers, 1, true,
                                   button, down_host);
            // Only this generation may mutate its captured target. A modern
            // callback may have synchronously cancelled/replaced the session.
            if (!accepts_original()) return;
            if (!target_alive) drag_target_.reset();
            if (button == MouseButton::right)
                dispatch_context_menu(root, drag_target_.live_in(root), pt);
            if (!accepts_original()) return;
            host_.input_request_repaint();
        } catch (const std::exception& e) {
            runtime::log_warn("WinPluginInputRouter: mouse down handler threw: {}",
                              e.what());
            if (session_.accepts(generation, button)) cancel_gesture();
        } catch (...) {
            runtime::log_warn("WinPluginInputRouter: mouse down handler threw");
            if (session_.accepts(generation, button)) cancel_gesture();
        }
    }

    /// `button_mask` is the raw `MK_*` word from the move message; the router
    /// asks `drag_continues()` whether the pressed button is still down, which
    /// is how a button released outside the window ends its gesture.
    void on_mouse_move(Point pt, std::uint32_t button_mask,
                       std::uint16_t modifiers) {
        try {
            last_point_ = pt;
            if (!tracking_leave_)
                tracking_leave_ = host_.input_begin_mouse_leave_tracking();
            View& root = host_.input_root();
            const bool held = drag_continues(button_mask, session_.button());
            if (session_.active() && !held) cancel_gesture();
            if (!session_.active()) {
                root.simulate_hover(pt);
                host_.input_request_repaint();
                return;
            }
            MouseEvent gesture_event;
            gesture_event.position = pt;
            gesture_event.window_position = pt;
            gesture_event.button = session_.button();
            gesture_event.modifiers = modifiers;
            gesture_event.is_down = true;
            gesture_event.phase = MousePhase::drag;
            if (!drag_target_.live_in(root)) drag_target_.reset();
            const bool gesture_yielded = yield_to_gesture(gesture_event);
            if (!gesture_yielded) {
                View* target = drag_target_.live_in(root);
                if (target)
                    deliver_mouse_drag(root, target, pt, modifiers, 1,
                                       PointerType::mouse, 0.5f,
                                       session_.button());
            }
            host_.input_request_repaint();
        } catch (const std::exception& e) {
            runtime::log_warn("WinPluginInputRouter: mouse move handler threw: {}",
                              e.what());
            cancel_gesture();
        } catch (...) {
            runtime::log_warn("WinPluginInputRouter: mouse move handler threw");
            cancel_gesture();
        }
    }

    void on_mouse_up(Point pt, MouseButton button, std::uint16_t modifiers) {
        if (!session_.accepts(button)) return;
        try {
            // Publish terminal before gesture/raw callbacks. Those callbacks
            // can synchronously pump capture-change or another button-up.
            const auto terminal = session_.terminalize();
            last_point_ = pt;
            View& root = host_.input_root();
            if (!drag_target_.live_in(root)) drag_target_.reset();
            MouseEvent gesture_event;
            gesture_event.position = pt;
            gesture_event.window_position = pt;
            gesture_event.button = button;
            gesture_event.modifiers = modifiers;
            gesture_event.is_down = false;
            gesture_event.phase = MousePhase::release;
            const bool gesture_yielded = should_yield_to_gesture(root, gesture_event);
            // A callback above may have synchronously cancelled this generation
            // (and closed drag_target_). Never resume the stale outer release.
            if (session_.phase() != PointerSession::Phase::terminal ||
                session_.generation() != terminal.generation)
                return;
            View* target = drag_target_.live_in(root);
            drag_target_.reset();
            if (gesture_yielded) {
                if (target)
                    deliver_gesture_handoff(root, target, pt, modifiers, 1);
            } else if (target) {
                MouseUpHost up_host;
                if (button == MouseButton::left) {
                    up_host.fire_click =
                        [](const std::function<void()>& click_handler,
                           const std::string&, std::uint16_t) {
                            if (click_handler) click_handler();
                        };
                }
                deliver_mouse_up(root, target, pt, modifiers, 1, up_host, button);
            }
            if (session_.phase() == PointerSession::Phase::terminal &&
                session_.generation() == terminal.generation) {
                host_.input_release_pointer_capture();
                session_.finish_terminal(terminal);
            }
            host_.input_request_repaint();
        } catch (const std::exception& e) {
            runtime::log_warn("WinPluginInputRouter: mouse up handler threw: {}",
                              e.what());
            cancel_gesture();
        } catch (...) {
            runtime::log_warn("WinPluginInputRouter: mouse up handler threw");
            cancel_gesture();
        }
    }

    void on_capture_lost() noexcept {
        // WM_CAPTURECHANGED/WM_CANCELMODE can arrive without a matching button
        // up (host modal UI, Alt+Tab, another HWND taking capture). End the
        // gesture explicitly so value widgets and the gesture arbiter cannot
        // remain latched in a drag until the editor is reopened.
        cancel_gesture();
    }

    void on_mouse_leave() {
        tracking_leave_ = false;
        if (session_.active())
            return;  // capture keeps a drag alive outside the window
        host_.input_root().simulate_hover({-1000000.0f, -1000000.0f});
        host_.input_request_repaint();
    }

    void on_mouse_wheel(Point pt, float dx, float dy,
                        std::uint16_t modifiers = 0) {
        WheelHost wheel_host;
        wheel_host.request_repaint = [this] { host_.input_request_repaint(); };
        deliver_mouse_wheel(host_.input_root(), pt, dx, dy, modifiers,
                            wheel_host);
    }

    // ── Keyboard and text ───────────────────────────────────────────────────

    bool on_key(KeyCode key, std::uint16_t modifiers, bool is_down,
                bool is_repeat) {
        auto* focused = focused_input_under_root(host_.input_root());
        if (!focused) return false;
        if (key == KeyCode::unknown) return false;
        KeyEvent event;
        event.key = key;
        event.modifiers = modifiers;
        event.is_down = is_down;
        event.is_repeat = is_repeat;
        const bool consumed = focused->on_key_event(event);
        if (consumed) host_.input_request_repaint();
        return consumed;
    }

    /// One UTF-16 code unit from `WM_CHAR`. Returns whether the unit was
    /// consumed by the text channel — a `false` return means no focused view
    /// accepts text, so the host should let default processing have it.
    bool on_text_unit(char16_t unit) {
        auto* focused = focused_input_under_root(host_.input_root());
        if (!focused || !focused->accepts_text_input()) return false;
        // Editing/navigation controls are already delivered through KeyEvent.
        // WM_CHAR repeats them as C0 code units; inserting those into the text
        // channel would double-handle Backspace/Tab/Return/Escape.
        if (unit < 0x20) return true;

        std::string text;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            // Hold the high half; the low half arrives in the next WM_CHAR.
            pending_high_surrogate_ = unit;
            return true;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF && pending_high_surrogate_ != 0) {
            text = utf16_code_point_to_utf8(pending_high_surrogate_, unit);
        } else {
            text = utf16_code_point_to_utf8(unit, 0);
        }
        pending_high_surrogate_ = 0;
        if (text.empty()) return true;
        focused->on_text_input(TextInputEvent{.text = text});
        host_.input_request_repaint();
        return true;
    }

    void on_focus_changed(bool gained) {
        if (gained) return;
        // A half-typed surrogate pair cannot be completed by a different
        // window's keystrokes, so drop it rather than pairing it with whatever
        // arrives after focus returns.
        pending_high_surrogate_ = 0;
        View& root = host_.input_root();
        if (focused_input_under_root(root)) {
            transfer_input_focus(root, nullptr);
            host_.input_request_repaint();
        }
    }

    // ── Teardown ────────────────────────────────────────────────────────────

    /// End any live gesture, delivering the cancellation the widgets need.
    /// `noexcept` because detach/destroy call it and a throw there would cross
    /// a host boundary.
    void cancel_gesture() noexcept {
        const auto terminal = session_.terminalize();
        try {
            View& root = host_.input_root();
            // Unpublish the cancelled generation before any gesture/user
            // callback. PointerSession remains terminal until those callbacks
            // return, so a nested button-down cannot overwrite this bracket;
            // the local capture also survives removal/replacement of the view.
            ViewCapture cancelled_target = drag_target_;
            drag_target_.reset();
            View* target = cancelled_target.live_in(root);
            if (!target && !terminal.cancel_gesture && !terminal.owns_terminal)
                return;
            if (terminal.cancel_gesture) {
                MouseEvent event;
                event.position = last_point_;
                event.window_position = last_point_;
                event.button = terminal.button;
                event.is_down = false;
                event.phase = MousePhase::release;
                event.is_cancelled = true;
                root.dispatch_gesture_pointer_event(event);
            }
            target = cancelled_target.live_in(root);
            if (target) {
                deliver_mouse_cancel(root, target, last_point_, 0, 1,
                                     terminal.button);
            }
        } catch (const std::exception& e) {
            runtime::log_warn("WinPluginInputRouter: pointer cancellation threw: {}",
                              e.what());
        } catch (...) {
            runtime::log_warn("WinPluginInputRouter: pointer cancellation threw");
        }
        if (terminal.owns_terminal &&
            session_.phase() == PointerSession::Phase::terminal &&
            session_.generation() == terminal.generation) {
            host_.input_release_pointer_capture();
            session_.finish_terminal(terminal);
        }
        host_.input_request_repaint();
    }

    // ── Observation (host + tests) ──────────────────────────────────────────

    bool gesture_active() const noexcept { return session_.active(); }
    MouseButton gesture_button() const noexcept { return session_.button(); }
    bool has_captured_target() const noexcept {
        return drag_target_.has_value();
    }
    const View* captured_target() const noexcept {
        return drag_target_.live_in(host_.input_root());
    }
    Point last_point() const noexcept { return last_point_; }
    bool tracking_mouse_leave() const noexcept { return tracking_leave_; }
    bool has_pending_high_surrogate() const noexcept {
        return pending_high_surrogate_ != 0;
    }

private:
    /// Offer a press/drag to the gesture arbiter. Returns whether the arbiter
    /// took the event, in which case raw delivery is suppressed for it.
    bool yield_to_gesture(const MouseEvent& event) {
        if (event.button != MouseButton::left) return false;
        View& root = host_.input_root();
        const std::uint64_t generation = session_.generation();
        const bool yielded = should_yield_to_gesture(root, event);
        // Gesture callbacks may pump a nested message. Stop this outer frame
        // if that message terminalized/replaced the session; never stamp its
        // result onto the newer generation.
        if (session_.generation() != generation || !session_.active())
            return true;
        if (!yielded) return false;

        // Publish claimed and clear raw delivery BEFORE synchronous handoff.
        session_.mark_claimed();
        View* handoff_target = drag_target_.live_in(root);
        drag_target_.reset();
        if (event.phase != MousePhase::press)
            deliver_gesture_handoff(root, handoff_target, event.window_position,
                                    event.modifiers, 1);
        return true;
    }

    InputRouterHost& host_;
    PointerSession session_;
    ViewCapture drag_target_;
    Point last_point_{};
    bool tracking_leave_ = false;
    char16_t pending_high_surrogate_ = 0;
};

}  // namespace pulp::view::win_input
