// Windows plug-in editor host: pointer-mapping and surface-lifecycle logic.
//
// Covers the decisions factored out of
// core/view/platform/win/plugin_view_host_win.cpp into
// pulp/view/platform/win_pointer_input.hpp. These run on EVERY platform on
// purpose: the required Pulp CI gate is macOS, so a _WIN32-gated test of the
// Windows host would never actually execute.
//
// The bugs these pin down were all live at once — the Windows plug-in wndproc
// had no mouse-button routing at all (knobs could not be turned in REAPER),
// and the Dawn presentation surface was created against the hidden WS_POPUP
// HWND that exists before the DAW reparents the editor (black editor until an
// unrelated host repaint).

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/platform/win_pointer_input.hpp>

#include <cstdint>

using namespace pulp::view;
using namespace pulp::view::win_input;

namespace {

// Build an LPARAM the way Windows packs a mouse message: signed 16-bit x in
// the low word, signed 16-bit y in the high word.
constexpr uint32_t pack_lparam(int16_t x, int16_t y) {
    return (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16) |
           static_cast<uint32_t>(static_cast<uint16_t>(x));
}

}  // namespace

TEST_CASE("Windows mouse LPARAM unpacks positive client coordinates",
          "[view][windows][pointer]") {
    const uint32_t lp = pack_lparam(268, 115);
    REQUIRE(lparam_x(lp) == 268);
    REQUIRE(lparam_y(lp) == 115);
}

TEST_CASE("Windows mouse LPARAM unpacks negative coordinates as signed",
          "[view][windows][pointer]") {
    // The regression that matters for knobs: during a CAPTURED drag the
    // pointer can leave the editor past its left/top edge and Windows reports
    // negative client coordinates. Read as unsigned these become ~65500 and a
    // knob slams to the opposite end of its range instead of tracking.
    const uint32_t lp = pack_lparam(-12, -40);
    REQUIRE(lparam_x(lp) == -12);
    REQUIRE(lparam_y(lp) == -40);

    const Point p = lparam_to_logical_point(lp, 1.0f);
    REQUIRE(p.x < 0.0f);
    REQUIRE(p.y < 0.0f);
    REQUIRE(p.x == -12.0f);
    REQUIRE(p.y == -40.0f);
}

TEST_CASE("Windows mouse coordinates convert physical pixels to logical units",
          "[view][windows][pointer]") {
    // Window messages report PHYSICAL device pixels; the view tree is logical.
    const uint32_t lp = pack_lparam(300, 150);

    const Point at_1x = lparam_to_logical_point(lp, 1.0f);
    REQUIRE(at_1x.x == 300.0f);
    REQUIRE(at_1x.y == 150.0f);

    const Point at_2x = lparam_to_logical_point(lp, 2.0f);
    REQUIRE(at_2x.x == 150.0f);
    REQUIRE(at_2x.y == 75.0f);

    const Point at_1_5x = lparam_to_logical_point(lp, 1.5f);
    REQUIRE(at_1_5x.x == 200.0f);
    REQUIRE(at_1_5x.y == 100.0f);
}

TEST_CASE("Windows mouse coordinates fall back to unity for a bad scale",
          "[view][windows][pointer]") {
    const uint32_t lp = pack_lparam(64, 32);
    for (float bad : {0.0f, -1.0f, -0.5f}) {
        const Point p = lparam_to_logical_point(lp, bad);
        REQUIRE(p.x == 64.0f);
        REQUIRE(p.y == 32.0f);
    }
}

TEST_CASE("Windows mouse modifiers map the WPARAM key-state bits",
          "[view][windows][pointer]") {
    REQUIRE(mouse_modifiers(0, false, false) == kModNone);
    REQUIRE((mouse_modifiers(kMkShift, false, false) & kModShift) != 0);
    REQUIRE((mouse_modifiers(kMkControl, false, false) & kModCtrl) != 0);

    // The left-button bit is not a modifier — a plain drag must report none.
    REQUIRE(mouse_modifiers(kMkLButton, false, false) == kModNone);
}

TEST_CASE("Windows mouse modifiers take Alt and Meta from keyboard state",
          "[view][windows][pointer]") {
    // Windows does not pack Alt or the Windows key into the mouse WPARAM, so
    // the host samples GetKeyState and passes them in.
    REQUIRE((mouse_modifiers(0, true, false) & kModAlt) != 0);
    REQUIRE((mouse_modifiers(0, false, true) & kModMeta) != 0);
    REQUIRE((mouse_modifiers(0, true, false) & kModMeta) == 0);
    REQUIRE((mouse_modifiers(0, false, true) & kModAlt) == 0);

    const uint16_t all = mouse_modifiers(kMkShift | kMkControl, true, true);
    REQUIRE((all & kModShift) != 0);
    REQUIRE((all & kModCtrl) != 0);
    REQUIRE((all & kModAlt) != 0);
    REQUIRE((all & kModMeta) != 0);
}

TEST_CASE("Windows drag continues only while the left button is held",
          "[view][windows][pointer]") {
    REQUIRE(drag_continues(kMkLButton));
    REQUIRE(drag_continues(kMkLButton | kMkShift));
    // Plain hover, and post-release moves that mouse capture still delivers.
    REQUIRE_FALSE(drag_continues(0));
    REQUIRE_FALSE(drag_continues(kMkShift));
    REQUIRE_FALSE(drag_continues(kMkRButton));
}

TEST_CASE("Windows button messages and capture masks preserve button identity",
          "[view][windows][pointer]") {
    REQUIRE(mouse_button_from_message(kWmLButtonDown) == MouseButton::left);
    REQUIRE(mouse_button_from_message(kWmRButtonUp) == MouseButton::right);
    REQUIRE(mouse_button_from_message(kWmMButtonDown) == MouseButton::middle);
    REQUIRE(mouse_button_from_message(0) == MouseButton::none);

    REQUIRE(drag_continues(kMkRButton, MouseButton::right));
    REQUIRE_FALSE(drag_continues(kMkLButton, MouseButton::right));
    REQUIRE(drag_continues(kMkMButton | kMkShift, MouseButton::middle));
}

TEST_CASE("Windows capture loss terminalizes an unclaimed gesture candidate",
          "[view][windows][pointer][gesture]") {
    PointerSession session;
    REQUIRE(session.begin(MouseButton::left));
    REQUIRE(session.phase() == PointerSession::Phase::gesture_candidate);

    const auto terminal = session.terminalize();
    CHECK(terminal.cancel_gesture);
    CHECK_FALSE(terminal.was_claimed);
    CHECK(terminal.button == MouseButton::left);
    CHECK(session.phase() == PointerSession::Phase::terminal);

    session.finish_terminal(terminal.generation);
    CHECK_FALSE(session.active());
}

TEST_CASE("Windows gesture claim is reentrancy-visible before handoff callbacks",
          "[view][windows][pointer][gesture]") {
    PointerSession session;
    REQUIRE(session.begin(MouseButton::left));
    session.mark_claimed();  // host publishes this before raw release callbacks

    CHECK(session.claimed());
    const auto nested_cancel = session.terminalize();
    CHECK(nested_cancel.cancel_gesture);
    CHECK(nested_cancel.was_claimed);
    CHECK(session.phase() == PointerSession::Phase::terminal);
}

TEST_CASE("Windows button chord cannot overwrite an open capture bracket",
          "[view][windows][pointer][buttons]") {
    PointerSession session;
    REQUIRE(session.begin(MouseButton::left));
    CHECK_FALSE(session.begin(MouseButton::right));
    CHECK(session.button() == MouseButton::left);

    const auto left = session.terminalize();
    session.finish_terminal(left.generation);
    REQUIRE(session.begin(MouseButton::right));
    CHECK(session.button() == MouseButton::right);
    CHECK(session.phase() == PointerSession::Phase::raw);
}

TEST_CASE("Windows stale terminal cleanup cannot erase a reentrant session",
          "[view][windows][pointer][reentrancy]") {
    PointerSession session;
    REQUIRE(session.begin(MouseButton::left));
    const auto first = session.terminalize();
    session.finish_terminal(first.generation);
    REQUIRE(session.begin(MouseButton::middle));

    // An outer callback frame finishing generation 1 after nested generation 2
    // began must leave the nested bracket intact.
    session.finish_terminal(first.generation);
    CHECK(session.active());
    CHECK(session.button() == MouseButton::middle);
}

TEST_CASE("Windows wheel deltas use Pulp axis conventions",
          "[view][windows][pointer]") {
    const auto pack_wheel = [](int16_t delta) {
        return static_cast<uint32_t>(static_cast<uint16_t>(delta)) << 16;
    };
    REQUIRE(wheel_steps(pack_wheel(120), false) == -1.0f);
    REQUIRE(wheel_steps(pack_wheel(-240), false) == 2.0f);
    REQUIRE(wheel_steps(pack_wheel(120), true) == 1.0f);
}

TEST_CASE("Windows virtual keys map to Pulp key codes",
          "[view][windows][keyboard]") {
    REQUIRE(key_code_from_virtual_key('A') == KeyCode::a);
    REQUIRE(key_code_from_virtual_key('9') == KeyCode::num9);
    REQUIRE(key_code_from_virtual_key(kVkLeft) == KeyCode::left);
    REQUIRE(key_code_from_virtual_key(kVkDelete) == KeyCode::delete_);
    REQUIRE(key_code_from_virtual_key(kVkF12) == KeyCode::f12);
    REQUIRE(key_code_from_virtual_key(kVkOem1) == KeyCode::semicolon);
    REQUIRE(key_code_from_virtual_key(kVkOem7) == KeyCode::apostrophe);
    REQUIRE(key_code_from_virtual_key(0xFF) == KeyCode::unknown);

    const auto mods = key_modifiers(true, true, true, true);
    REQUIRE((mods & kModShift) != 0);
    REQUIRE((mods & kModCtrl) != 0);
    REQUIRE((mods & kModAlt) != 0);
    REQUIRE((mods & kModMeta) != 0);
}

TEST_CASE("Windows editor surfaces are not created before attach",
          "[view][windows][lifecycle]") {
    // Creating the Dawn surface in the host constructor configures it for the
    // hidden WS_POPUP HWND that exists before the DAW reparents the editor.
    SurfaceLifecycle lifecycle;
    REQUIRE_FALSE(lifecycle.surfaces_created());
}

TEST_CASE("Windows editor surfaces are created once on attach",
          "[view][windows][lifecycle]") {
    SurfaceLifecycle lifecycle;
    REQUIRE(lifecycle.note_attached());
    REQUIRE(lifecycle.surfaces_created());
    // A redundant attach must not rebuild a live surface pair.
    REQUIRE_FALSE(lifecycle.note_attached());
    REQUIRE(lifecycle.surfaces_created());
}

TEST_CASE("Windows editor surfaces are rebuilt across a detach and reattach",
          "[view][windows][lifecycle]") {
    // Closing and reopening the editor, or removing and re-adding the effect,
    // reparents the HWND. A surface configured for the previous parent must
    // not survive into the new one.
    SurfaceLifecycle lifecycle;
    REQUIRE(lifecycle.note_attached());
    REQUIRE(lifecycle.note_detached());
    REQUIRE_FALSE(lifecycle.surfaces_created());
    REQUIRE(lifecycle.note_attached());
    REQUIRE(lifecycle.surfaces_created());
}

TEST_CASE("Windows editor detach without surfaces is a no-op",
          "[view][windows][lifecycle]") {
    SurfaceLifecycle lifecycle;
    REQUIRE_FALSE(lifecycle.note_detached());
    REQUIRE_FALSE(lifecycle.surfaces_created());
}

TEST_CASE("Windows editor retries surface creation after a failure",
          "[view][windows][lifecycle]") {
    // Dawn adapter or Skia surface creation can fail. A half-built pair must
    // not read as initialized, or the next attach skips creation and the
    // editor stays black for the life of the plug-in instance.
    SurfaceLifecycle lifecycle;
    REQUIRE(lifecycle.note_attached());
    lifecycle.note_creation_failed();
    REQUIRE_FALSE(lifecycle.surfaces_created());
    REQUIRE(lifecycle.note_attached());
}
