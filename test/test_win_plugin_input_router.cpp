// test_win_plugin_input_router.cpp — the Windows editor's pointer/keyboard
// state machine (WAH-6).
//
// This is the point of the extraction. The logic under test used to live in a
// `_WIN32`-only TU, so on Pulp's required macOS gate it was never compiled, let
// alone run: the capture bracket, the generation guard against synchronous
// re-entry, the terminal protocol, and the surrogate-pair text path were all
// verified only by shipping them.
//
// `win_plugin_input_router.hpp` has no Win32 dependency — the native side
// effects are methods on `InputRouterHost` — so a recorder host lets every one
// of those rules run here, on every platform.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/platform/win_plugin_input_router.hpp>
#include <pulp/view/gesture.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/text_editor.hpp>

#include <string>
#include <vector>

using namespace pulp::view;
using pulp::view::win_input::InputRouterHost;
using pulp::view::win_input::PluginInputRouter;
using pulp::view::win_input::utf16_code_point_to_utf8;

namespace {

/// Records the native operations the router asks for, so tests assert on the
/// sequence rather than on whether Windows happened to be underneath.
class RecordingHost : public InputRouterHost {
public:
    explicit RecordingHost(View& root) : root_(root) {}

    View& input_root() noexcept override { return root_; }
    void input_capture_pointer() override {
        ops.emplace_back("capture");
        capture_held = true;
        if (on_capture) on_capture();
    }
    void input_release_pointer_capture() override {
        ops.emplace_back("release");
        capture_held = false;
    }
    void input_take_keyboard_focus() override {
        ops.emplace_back("focus");
        if (on_focus) on_focus();
    }
    bool input_begin_mouse_leave_tracking() override {
        ops.emplace_back("track-leave");
        return leave_tracking_succeeds;
    }
    void input_request_repaint() override { ++repaints; }

    /// Count of a given op in the recorded sequence.
    std::size_t count(const std::string& op) const {
        std::size_t n = 0;
        for (const auto& o : ops)
            if (o == op) ++n;
        return n;
    }

    std::vector<std::string> ops;
    int repaints = 0;
    bool capture_held = false;
    bool leave_tracking_succeeds = true;
    /// Re-entrancy hook: Windows delivers messages from inside SetCapture.
    std::function<void()> on_capture;
    /// Re-entrancy hook: focus transfer can synchronously pump host callbacks.
    std::function<void()> on_focus;

private:
    View& root_;
};

/// A view that counts what it receives and can run a callback mid-dispatch.
class ProbeView : public View {
public:
    // The MODERN channel sees every button; the legacy callbacks below are
    // left-only by design, so `presses` is what a non-left gesture shows up in.
    void on_mouse_event(const MouseEvent& e) override {
        if (e.phase == MousePhase::press) presses.push_back(e.button);
        if (e.is_cancelled) ++cancelled_events;
    }
    void on_mouse_down(Point) override {
        ++downs;
        if (on_down_cb) on_down_cb();
    }
    void on_mouse_up(Point) override { ++ups; }
    void on_mouse_cancel(Point) override { ++cancels; }
    void on_mouse_drag(Point) override { ++drags; }

    std::vector<MouseButton> presses;
    int downs = 0, ups = 0, cancels = 0, cancelled_events = 0, drags = 0;
    std::function<void()> on_down_cb;
};

class ClaimOnPressRecognizer final : public GestureRecognizer {
protected:
    void on_pointer_event(const MouseEvent& event,
                          const GestureContext&) override {
        if (event.phase == MousePhase::press)
            transition_to(GestureState::began);
    }
};

class CallbackClaimRecognizer final : public GestureRecognizer {
public:
    std::function<void()> callback;

protected:
    void on_pointer_event(const MouseEvent& event,
                          const GestureContext&) override {
        if (event.phase != MousePhase::press) return;
        transition_to(GestureState::began);
        if (callback) callback();
    }
};

class ClaimOnSecondPressRecognizer final : public GestureRecognizer {
protected:
    void on_pointer_event(const MouseEvent& event,
                          const GestureContext&) override {
        if (event.phase == MousePhase::press && ++presses_ == 2)
            transition_to(GestureState::began);
    }

private:
    int presses_ = 0;
};

/// Root + one hit-testable probe child, the shape almost every case needs.
struct Fixture {
    View root;
    ProbeView* probe = nullptr;

    Fixture() {
        root.set_bounds({0, 0, 200, 200});
        auto child = std::make_unique<ProbeView>();
        probe = child.get();
        probe->set_bounds({0, 0, 200, 200});
        root.add_child(std::move(child));
    }
};

constexpr std::uint32_t kLeftHeld = 0x0001;  // MK_LBUTTON
constexpr std::uint32_t kNoButtons = 0;

}  // namespace

// ── The capture bracket ─────────────────────────────────────────────────────

TEST_CASE("a press captures and a release frees exactly once",
          "[win-input-router][wah-6]") {
    Fixture fx;
    auto* probe = fx.probe;
    RecordingHost host(fx.root);
    PluginInputRouter router(host);

    router.on_mouse_down({10, 10}, MouseButton::left, 0);
    REQUIRE(router.gesture_active());
    REQUIRE(host.count("capture") == 1);
    REQUIRE(host.capture_held);
    REQUIRE(probe->downs == 1);

    router.on_mouse_up({10, 10}, MouseButton::left, 0);
    REQUIRE_FALSE(router.gesture_active());
    REQUIRE(host.count("release") == 1);
    REQUIRE_FALSE(host.capture_held);
    REQUIRE(probe->ups == 1);
}

TEST_CASE("a press takes keyboard focus before delivering the down",
          "[win-input-router][wah-6]") {
    // Order matters: a widget's mouse-down handler may inspect focus, and on
    // Windows SetFocus is what makes subsequent WM_CHAR reach this window.
    Fixture fx;
    auto* probe = fx.probe;
    RecordingHost host(fx.root);
    PluginInputRouter router(host);

    std::vector<std::string> ops_at_down;
    probe->on_down_cb = [&] { ops_at_down = host.ops; };

    router.on_mouse_down({10, 10}, MouseButton::left, 0);

    REQUIRE(probe->downs == 1);
    REQUIRE_FALSE(ops_at_down.empty());
    REQUIRE(ops_at_down.front() == "focus");
}

TEST_CASE("a press that hits nothing captures nothing",
          "[win-input-router][wah-6]") {
    // Reachable in production: with a design viewport the host letterboxes, so
    // window_to_root_point maps a click in the bars to a root-space point
    // outside the tree. That must not latch a capture bracket, or the next
    // release anywhere in the editor would be attributed to it.
    Fixture fx;
    RecordingHost host(fx.root);
    PluginInputRouter router(host);

    router.on_mouse_down({-40, -40}, MouseButton::left, 0);

    REQUIRE_FALSE(router.gesture_active());
    REQUIRE(host.count("capture") == 0);
    REQUIRE_FALSE(router.has_captured_target());
    REQUIRE(fx.probe->downs == 0);
}

TEST_CASE("a target removed during native focus transfer is not dereferenced",
          "[win-input-router][runtime-eval]") {
    Fixture fx;
    RecordingHost host(fx.root);
    PluginInputRouter router(host);
    host.on_focus = [&] {
        auto retired = fx.root.remove_child(fx.probe);
        fx.probe = nullptr;
        retired.reset();
    };

    router.on_mouse_down({10, 10}, MouseButton::left, 0);

    CHECK_FALSE(router.gesture_active());
    CHECK_FALSE(router.has_captured_target());
}

TEST_CASE("a stale gesture callback frame preserves its replacement press",
          "[win-input-router][runtime-eval]") {
    View root;
    root.set_bounds({0, 0, 200, 100});
    auto first_owned = std::make_unique<ProbeView>();
    auto* first = first_owned.get();
    first->set_bounds({0, 0, 100, 100});
    auto second_owned = std::make_unique<ProbeView>();
    auto* second = second_owned.get();
    second->set_bounds({100, 0, 100, 100});
    root.add_child(std::move(first_owned));
    root.add_child(std::move(second_owned));

    RecordingHost host(root);
    PluginInputRouter router(host);
    auto recognizer = std::make_unique<CallbackClaimRecognizer>();
    auto* recognizer_ptr = recognizer.get();
    recognizer->callback = [&] {
        recognizer_ptr->callback = {};
        router.on_mouse_down({150, 50}, MouseButton::left, 0);
    };
    first->add_gesture_recognizer(std::move(recognizer));

    router.on_mouse_down({50, 50}, MouseButton::left, 0);

    REQUIRE(router.gesture_active());
    REQUIRE(router.captured_target() == second);
    REQUIRE(second->downs == 1);
}

TEST_CASE("a right click routes its context menu to the active overlay",
          "[win-input-router][runtime-eval]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    // The claiming wrapper is small; its overflow-visible child paints over
    // a later sibling outside that wrapper because claimed overlays have their
    // own paint pass. A regular root hit-test follows tree order and resolves
    // the later sibling, while overlay-aware routing resolves the painted
    // child.
    auto overlay_owned = std::make_unique<ProbeView>();
    auto* overlay = overlay_owned.get();
    overlay->set_bounds({100, 100, 20, 20});
    overlay->set_overflow(View::Overflow::visible);
    auto popup_owned = std::make_unique<ProbeView>();
    auto* popup = popup_owned.get();
    popup->set_bounds({-80, -80, 60, 60});
    int overlay_menus = 0;
    popup->on_context_menu = [&](Point) { ++overlay_menus; };
    overlay->add_child(std::move(popup_owned));
    root.add_child(std::move(overlay_owned));
    overlay->claim_overlay();

    auto under_owned = std::make_unique<ProbeView>();
    auto* under = under_owned.get();
    under->set_bounds({0, 0, 200, 200});
    int under_menus = 0;
    under->on_context_menu = [&](Point) { ++under_menus; };
    root.add_child(std::move(under_owned));

    RecordingHost host(root);
    PluginInputRouter router(host);
    router.on_mouse_down({50, 50}, MouseButton::right, 0);

    REQUIRE(overlay_menus == 1);
    REQUIRE(under_menus == 0);
}

TEST_CASE("an underlay recognizer cannot claim a routed overlay press",
          "[win-input-router][runtime-eval]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto overlay_owned = std::make_unique<ProbeView>();
    auto* overlay = overlay_owned.get();
    overlay->set_bounds({0, 0, 200, 200});
    root.add_child(std::move(overlay_owned));
    overlay->claim_overlay();

    auto under_owned = std::make_unique<ProbeView>();
    auto* under = under_owned.get();
    under->set_bounds({0, 0, 200, 200});
    under->add_gesture_recognizer(std::make_unique<ClaimOnPressRecognizer>());
    root.add_child(std::move(under_owned));

    RecordingHost host(root);
    PluginInputRouter router(host);
    router.on_mouse_down({50, 50}, MouseButton::left, 0);

    REQUIRE(overlay->downs == 1);
    REQUIRE(under->downs == 0);
    REQUIRE(router.captured_target() == overlay);
}

TEST_CASE("an overlay dismissal callback may replace the pointer bracket",
          "[win-input-router][runtime-eval]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto overlay_owned = std::make_unique<ProbeView>();
    auto* overlay = overlay_owned.get();
    overlay->set_bounds({0, 0, 40, 40});
    root.add_child(std::move(overlay_owned));
    overlay->claim_overlay();

    auto target_owned = std::make_unique<ProbeView>();
    auto* replacement = target_owned.get();
    replacement->set_bounds({0, 0, 200, 200});
    replacement->add_gesture_recognizer(
        std::make_unique<ClaimOnSecondPressRecognizer>());
    root.add_child(std::move(target_owned));

    RecordingHost host(root);
    PluginInputRouter router(host);
    overlay->on_overlay_dismissed = [&] {
        router.on_mouse_down({100, 100}, MouseButton::left, 0);
    };

    router.on_mouse_down({150, 150}, MouseButton::left, 0);

    REQUIRE(router.gesture_active());
    REQUIRE(router.captured_target() == replacement);
    REQUIRE(replacement->downs == 1);
}

TEST_CASE("a second button closes the first bracket rather than overwriting it",
          "[win-input-router][wah-6]") {
    // Chorded buttons: this host owns ONE capture bracket. The first target
    // must receive its release, not be silently dropped.
    Fixture fx;
    auto* probe = fx.probe;
    RecordingHost host(fx.root);
    PluginInputRouter router(host);

    router.on_mouse_down({10, 10}, MouseButton::left, 0);
    REQUIRE(probe->downs == 1);
    const int cancels_before = probe->cancels;

    router.on_mouse_down({12, 12}, MouseButton::right, 0);

    // The left bracket was cancelled, and the
    // right one is now the live gesture.
    REQUIRE(probe->cancels == cancels_before + 1);
    REQUIRE(probe->cancelled_events == 1);
    REQUIRE(router.gesture_active());
    REQUIRE(router.gesture_button() == MouseButton::right);
}

TEST_CASE("a release for a button that is not the captured one is ignored",
          "[win-input-router][wah-6]") {
    // A right-button up while the LEFT button owns the bracket must not tear
    // down the left drag — the user is still holding it.
    Fixture fx;
    auto* probe = fx.probe;
    RecordingHost host(fx.root);
    PluginInputRouter router(host);

    router.on_mouse_down({10, 10}, MouseButton::left, 0);
    REQUIRE(router.gesture_active());

    router.on_mouse_up({10, 10}, MouseButton::right, 0);

    REQUIRE(router.gesture_active());
    REQUIRE(router.gesture_button() == MouseButton::left);
    REQUIRE(probe->ups == 0);
    REQUIRE(host.count("release") == 0);
}

// ── Re-entrancy: Windows delivers messages from inside our own calls ────────

TEST_CASE("a gesture superseded from inside SetCapture does not resume",
          "[win-input-router][wah-6]") {
    // THE generation-guard case. SetCapture synchronously delivers messages, so
    // a NEW press can begin while the outer one is still mid-flight. Once that
    // happens the outer frame is stale: if it kept going it would deliver its
    // press a second time and re-capture for a bracket that no longer exists,
    // stamping the old button's result onto the new session.
    Fixture fx;
    auto* probe = fx.probe;
    RecordingHost host(fx.root);
    PluginInputRouter router(host);

    bool reentered = false;
    host.on_capture = [&] {
        if (reentered) return;
        reentered = true;
        router.on_mouse_down({50, 50}, MouseButton::right, 0);
    };

    router.on_mouse_down({10, 10}, MouseButton::left, 0);

    REQUIRE(reentered);
    // The inner bracket is the live one...
    REQUIRE(router.gesture_active());
    REQUIRE(router.gesture_button() == MouseButton::right);
    // ...and the outer frame stopped rather than delivering its press after
    // being superseded. Exactly one press reached the widget, the inner one.
    REQUIRE(probe->presses.size() == 1);
    REQUIRE(probe->presses.front() == MouseButton::right);
    REQUIRE(host.count("capture") == 2);  // one per press
}

TEST_CASE("a gesture cancelled from inside SetCapture delivers no press",
          "[win-input-router][wah-6]") {
    // WM_CAPTURECHANGED can arrive from inside our own SetCapture. The bracket
    // dies before the press is delivered, so the widget must never see it.
    Fixture fx;
    auto* probe = fx.probe;
    RecordingHost host(fx.root);
    PluginInputRouter router(host);

    bool reentered = false;
    host.on_capture = [&] {
        if (reentered) return;
        reentered = true;
        router.on_capture_lost();
    };

    router.on_mouse_down({10, 10}, MouseButton::left, 0);

    REQUIRE(reentered);
    REQUIRE_FALSE(router.gesture_active());
    REQUIRE(probe->downs == 0);
    REQUIRE_FALSE(router.has_captured_target());
}

TEST_CASE("capture lost mid-drag ends the gesture and balances the target",
          "[win-input-router][wah-6]") {
    // Alt+Tab, a host modal dialog, or another HWND taking capture. Without
    // this the widget stays latched in a drag until the editor is reopened.
    Fixture fx;
    auto* probe = fx.probe;
    RecordingHost host(fx.root);
    PluginInputRouter router(host);

    router.on_mouse_down({10, 10}, MouseButton::left, 0);
    router.on_mouse_move({20, 20}, kLeftHeld, 0);
    REQUIRE(probe->drags == 1);

    router.on_capture_lost();

    REQUIRE_FALSE(router.gesture_active());
    REQUIRE(probe->cancels == 1);  // cancelled, not abandoned
    REQUIRE(probe->cancelled_events == 1);
    REQUIRE_FALSE(router.has_captured_target());
}

TEST_CASE("a target retired between pointer messages is not dereferenced",
          "[win-input-router][runtime-eval]") {
    Fixture fx;
    RecordingHost host(fx.root);
    PluginInputRouter router(host);

    router.on_mouse_down({10, 10}, MouseButton::left, 0);
    REQUIRE(router.gesture_active());
    auto retired = fx.root.remove_child(fx.probe);
    fx.probe = nullptr;
    retired.reset();

    router.on_mouse_move({20, 20}, kLeftHeld, 0);
    router.on_mouse_up({20, 20}, MouseButton::left, 0);

    CHECK_FALSE(router.gesture_active());
    CHECK_FALSE(router.has_captured_target());
    CHECK(host.count("release") == 1);
}

TEST_CASE("a button released outside the window ends the drag on the next move",
          "[win-input-router][wah-6]") {
    // The button-up can be delivered to a different window entirely. The move
    // message's MK_* mask is the only evidence we get.
    Fixture fx;
    auto* probe = fx.probe;
    RecordingHost host(fx.root);
    PluginInputRouter router(host);

    router.on_mouse_down({10, 10}, MouseButton::left, 0);
    router.on_mouse_move({20, 20}, kLeftHeld, 0);
    REQUIRE(router.gesture_active());

    router.on_mouse_move({30, 30}, kNoButtons, 0);

    REQUIRE_FALSE(router.gesture_active());
    REQUIRE(probe->cancels == 1);
    REQUIRE(probe->cancelled_events == 1);
    REQUIRE(host.count("release") == 1);
}

// ── Hover and leave tracking ────────────────────────────────────────────────

TEST_CASE("leave tracking is armed once and re-armed after a leave",
          "[win-input-router][wah-6]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    RecordingHost host(root);
    PluginInputRouter router(host);

    router.on_mouse_move({10, 10}, kNoButtons, 0);
    router.on_mouse_move({11, 11}, kNoButtons, 0);
    REQUIRE(host.count("track-leave") == 1);  // not re-armed every move
    REQUIRE(router.tracking_mouse_leave());

    router.on_mouse_leave();
    REQUIRE_FALSE(router.tracking_mouse_leave());

    router.on_mouse_move({12, 12}, kNoButtons, 0);
    REQUIRE(host.count("track-leave") == 2);
}

TEST_CASE("failed leave tracking is retried rather than assumed",
          "[win-input-router][wah-6]") {
    // TrackMouseEvent can fail. Treating a failure as success would leave the
    // hover state stuck on whatever widget the pointer left.
    View root;
    root.set_bounds({0, 0, 200, 200});
    RecordingHost host(root);
    host.leave_tracking_succeeds = false;
    PluginInputRouter router(host);

    router.on_mouse_move({10, 10}, kNoButtons, 0);
    router.on_mouse_move({11, 11}, kNoButtons, 0);

    REQUIRE(host.count("track-leave") == 2);
    REQUIRE_FALSE(router.tracking_mouse_leave());
}

TEST_CASE("a leave during a captured drag does not clear hover",
          "[win-input-router][wah-6]") {
    // Capture deliberately keeps the drag alive outside the HWND; clearing
    // hover here would make a knob drag flicker when the pointer exits.
    Fixture fx;
    auto* probe = fx.probe;
    RecordingHost host(fx.root);
    PluginInputRouter router(host);

    // Establish hover, then start a drag and leave the window.
    router.on_mouse_move({10, 10}, kNoButtons, 0);
    REQUIRE(probe->is_hovered());
    router.on_mouse_down({10, 10}, MouseButton::left, 0);

    router.on_mouse_leave();

    REQUIRE(router.gesture_active());
    // Hover survives: clearing it here is what makes a knob flicker to its
    // un-hovered look the moment the pointer crosses the editor edge mid-drag.
    REQUIRE(probe->is_hovered());
}

// ── Text input: surrogate pairing ───────────────────────────────────────────

TEST_CASE("utf16_code_point_to_utf8 encodes each range",
          "[win-input-router][wah-6]") {
    REQUIRE(utf16_code_point_to_utf8(u'A', 0) == "A");
    // Numeric code points keep this independent of MSVC's source codepage.
    REQUIRE(utf16_code_point_to_utf8(char16_t{0x00e9}, 0) == "\xc3\xa9");
    REQUIRE(utf16_code_point_to_utf8(char16_t{0x4e2d}, 0) == "\xe4\xb8\xad");
    // U+1F600 GRINNING FACE — the case that needs both halves.
    REQUIRE(utf16_code_point_to_utf8(0xD83D, 0xDE00) == "\xf0\x9f\x98\x80");
}

TEST_CASE("an unpaired surrogate never reaches the text channel",
          "[win-input-router][wah-6]") {
    // Emitting one would put invalid UTF-8 into a TextEditor's buffer.
    REQUIRE(utf16_code_point_to_utf8(0xD83D, 0).empty());   // lone high
    REQUIRE(utf16_code_point_to_utf8(0xDE00, 0).empty());   // lone low
    REQUIRE(utf16_code_point_to_utf8(0xD83D, u'A').empty());  // bad pair
}

TEST_CASE("a surrogate pair arrives as one code point",
          "[win-input-router][wah-6]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto owned = std::make_unique<TextEditor>();
    auto* editor = owned.get();
    editor->set_bounds({0, 0, 100, 30});
    root.add_child(std::move(owned));
    RecordingHost host(root);
    PluginInputRouter router(host);
    REQUIRE(transfer_input_focus(root, editor));

    REQUIRE(router.on_text_unit(0xD83D));
    REQUIRE(router.has_pending_high_surrogate());
    REQUIRE(editor->text().empty());  // nothing emitted from half a pair

    REQUIRE(router.on_text_unit(0xDE00));
    REQUIRE_FALSE(router.has_pending_high_surrogate());
    REQUIRE(editor->text() == "\xf0\x9f\x98\x80");
}

TEST_CASE("control code units are swallowed, not inserted",
          "[win-input-router][wah-6]") {
    // WM_CHAR repeats Backspace/Tab/Return/Escape as C0 units; they are already
    // delivered as KeyEvents, so inserting them would double-handle them.
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto owned = std::make_unique<TextEditor>();
    auto* editor = owned.get();
    editor->set_bounds({0, 0, 100, 30});
    root.add_child(std::move(owned));
    RecordingHost host(root);
    PluginInputRouter router(host);
    REQUIRE(transfer_input_focus(root, editor));

    REQUIRE(router.on_text_unit(u'\b'));
    REQUIRE(router.on_text_unit(u'\r'));
    REQUIRE(router.on_text_unit(0x1B));
    REQUIRE(editor->text().empty());
}

TEST_CASE("losing focus drops a half-typed surrogate pair",
          "[win-input-router][wah-6]") {
    // The low half cannot arrive from another window's keystrokes; keeping the
    // high half would pair it with whatever is typed after focus returns.
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto owned = std::make_unique<TextEditor>();
    auto* editor = owned.get();
    editor->set_bounds({0, 0, 100, 30});
    root.add_child(std::move(owned));
    RecordingHost host(root);
    PluginInputRouter router(host);
    REQUIRE(transfer_input_focus(root, editor));

    REQUIRE(router.on_text_unit(0xD83D));
    REQUIRE(router.has_pending_high_surrogate());

    router.on_focus_changed(false);
    REQUIRE_FALSE(router.has_pending_high_surrogate());
}

TEST_CASE("text with nothing focused is not consumed",
          "[win-input-router][wah-6]") {
    // A false return lets the host give the message default processing rather
    // than silently eating a keystroke the editor had no use for.
    View root;
    root.set_bounds({0, 0, 200, 200});
    RecordingHost host(root);
    PluginInputRouter router(host);

    REQUIRE_FALSE(router.on_text_unit(u'a'));
}

// ── Keys ────────────────────────────────────────────────────────────────────

TEST_CASE("an unknown virtual key is never dispatched",
          "[win-input-router][wah-6]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto owned = std::make_unique<TextEditor>();
    auto* editor = owned.get();
    editor->set_bounds({0, 0, 100, 30});
    root.add_child(std::move(owned));
    RecordingHost host(root);
    PluginInputRouter router(host);
    REQUIRE(transfer_input_focus(root, editor));

    REQUIRE_FALSE(router.on_key(KeyCode::unknown, 0, true, false));
}

TEST_CASE("keys with nothing focused are not consumed",
          "[win-input-router][wah-6]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    RecordingHost host(root);
    PluginInputRouter router(host);

    REQUIRE_FALSE(router.on_key(KeyCode::left, 0, true, false));
}

// ── Teardown ────────────────────────────────────────────────────────────────

TEST_CASE("cancelling with no live gesture is a no-op",
          "[win-input-router][wah-6]") {
    // detach() calls this unconditionally, so a quiet editor must not emit a
    // spurious release or repaint.
    View root;
    root.set_bounds({0, 0, 200, 200});
    RecordingHost host(root);
    PluginInputRouter router(host);

    router.cancel_gesture();

    REQUIRE(host.ops.empty());
    REQUIRE(host.repaints == 0);
}

TEST_CASE("cancelling a live gesture releases capture and balances the target",
          "[win-input-router][wah-6]") {
    Fixture fx;
    auto* probe = fx.probe;
    RecordingHost host(fx.root);
    PluginInputRouter router(host);

    router.on_mouse_down({10, 10}, MouseButton::left, 0);
    router.cancel_gesture();

    REQUIRE_FALSE(router.gesture_active());
    REQUIRE(probe->cancels == 1);
    REQUIRE(probe->cancelled_events == 1);
    REQUIRE(host.count("release") == 1);
    REQUIRE_FALSE(host.capture_held);
}

TEST_CASE("cancelling twice does not double-release capture",
          "[win-input-router][wah-6]") {
    Fixture fx;
    RecordingHost host(fx.root);
    PluginInputRouter router(host);

    router.on_mouse_down({10, 10}, MouseButton::left, 0);
    router.cancel_gesture();
    router.cancel_gesture();

    REQUIRE(host.count("release") == 1);
}
