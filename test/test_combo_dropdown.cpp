// Automated test for ComboBox dropdown interaction
#include <catch2/catch_test_macros.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/canvas/canvas.hpp>

using namespace pulp::view;
using pulp::canvas::RecordingCanvas;

TEST_CASE("ComboBox: set_items and selected_text", "[combo]") {
    ComboBox combo;
    combo.set_items({"Lowpass", "Highpass", "Bandpass"});
    combo.set_selected(0);
    REQUIRE(combo.selected_text() == "Lowpass");
    combo.set_selected(2);
    REQUIRE(combo.selected_text() == "Bandpass");
}

TEST_CASE("ComboBox: on_change fires on selection", "[combo]") {
    ComboBox combo;
    combo.set_items({"A", "B", "C"});
    combo.set_selected(0);

    int changed_to = -1;
    combo.on_change = [&](int idx) { changed_to = idx; };

    combo.set_selected(1);
    REQUIRE(changed_to == 1);
}

TEST_CASE("ComboBox: click opens, click item selects", "[combo]") {
    ComboBox combo;
    combo.set_items({"One", "Two", "Three"});
    combo.set_selected(0);
    combo.set_bounds({0, 0, 140, 120}); // tall enough for dropdown

    int changed_to = -1;
    combo.on_change = [&](int idx) { changed_to = idx; };

    // Click to open
    MouseEvent open_click;
    open_click.position = {70, 14}; // center of combo header
    open_click.is_down = true;
    combo.on_mouse_event(open_click);
    // Internal state should be open.

    // Click on second item (y = 30 + 24 = ~54)
    MouseEvent select_click;
    select_click.position = {70, 54};
    select_click.is_down = true;
    combo.on_mouse_event(select_click);

    REQUIRE(changed_to == 1);
    REQUIRE(combo.selected_text() == "Two");
}

TEST_CASE("ComboBox: paints with correct tokens", "[combo]") {
    RecordingCanvas rc;
    ComboBox combo;
    combo.set_theme(Theme::dark());
    combo.set_items({"Test"});
    combo.set_selected(0);
    combo.set_bounds({0, 0, 140, 28});
    combo.paint(rc);
    REQUIRE(rc.commands().size() > 0);
}

TEST_CASE("fit_combo_label shrinks the font to fit, ellipsizes only at the floor", "[combo]") {
    // Font-scaling width model: width = chars * font * 0.5 (the real Skia
    // measure scales with font; RecordingCanvas's does not, which is why this is
    // a pure-logic test). "1/4 Delay" is 9 chars.
    auto width_at = [](const std::string& s, float f) {
        return static_cast<float>(s.size()) * f * 0.5f;
    };

    // Wide box: fits at the base font, full text, no shrink.
    std::string t1 = "1/4 Delay";
    float f1 = pulp::view::fit_combo_label(t1, 100.0f, 12.0f, 9.0f, width_at);
    CHECK(t1 == "1/4 Delay");
    CHECK(f1 == 12.0f);

    // Snug box: too wide at 12 (54px) but fits when shrunk — full text, smaller
    // font, NO ellipsis (this is the "1/4 Delay" truncation the user hit).
    std::string t2 = "1/4 Delay";
    float f2 = pulp::view::fit_combo_label(t2, 50.0f, 12.0f, 9.0f, width_at);
    CHECK(t2 == "1/4 Delay");
    CHECK(f2 < 12.0f);
    CHECK(f2 >= 9.0f);
    CHECK(width_at(t2, f2) <= 50.0f);

    // Tiny box: even the floor font overflows → ellipsize, clamped to min font.
    std::string t3 = "1/4 Delay";
    float f3 = pulp::view::fit_combo_label(t3, 30.0f, 12.0f, 9.0f, width_at);
    CHECK(f3 == 9.0f);
    CHECK(t3.size() >= 3);
    CHECK(t3.substr(t3.size() - 3) == "...");
}

TEST_CASE("ComboBox: keyboard up/down changes selection", "[combo]") {
    ComboBox combo;
    combo.set_items({"A", "B", "C"});
    combo.set_selected(0);

    KeyEvent down;
    down.key = KeyCode::down;
    down.is_down = true;
    combo.on_key_event(down);
    REQUIRE(combo.selected_text() == "B");

    combo.on_key_event(down);
    REQUIRE(combo.selected_text() == "C");

    KeyEvent up;
    up.key = KeyCode::up;
    up.is_down = true;
    combo.on_key_event(up);
    REQUIRE(combo.selected_text() == "B");
}

TEST_CASE("ComboBox: keyboard navigation moves the row highlight while open",
          "[combo]") {
    ComboBox combo;
    combo.set_bounds({0, 0, 120, 24});
    combo.set_items({"A", "B", "C"});
    combo.set_selected(0);

    // Open the dropdown → the current selection is highlighted from the start.
    KeyEvent enter;
    enter.key = KeyCode::enter;
    enter.is_down = true;
    combo.on_key_event(enter);
    REQUIRE(combo.is_open());
    REQUIRE(combo.hovered_index() == 0);

    // While open, arrowing moves the HIGHLIGHT only (like mouse hover); the selection is
    // not committed until Enter — so the user can browse, then accept or cancel.
    KeyEvent down;
    down.key = KeyCode::down;
    down.is_down = true;
    combo.on_key_event(down);
    REQUIRE(combo.hovered_index() == 1);
    REQUIRE(combo.selected_text() == "A");  // not committed yet

    combo.on_key_event(down);
    REQUIRE(combo.hovered_index() == 2);

    KeyEvent up;
    up.key = KeyCode::up;
    up.is_down = true;
    combo.on_key_event(up);
    REQUIRE(combo.hovered_index() == 1);

    // Enter commits the highlighted row and closes; Esc would cancel with no change.
    KeyEvent ret;
    ret.key = KeyCode::enter;
    ret.is_down = true;
    combo.on_key_event(ret);
    REQUIRE(combo.selected_text() == "B");
    REQUIRE_FALSE(combo.is_open());
}

// ── Dropdown overlay click routing ───────────────────────────────────────
//
// The ComboBox dropdown is a paint-only overlay (queued via
// View::overlay_queue() inside paint()). It has no view backing in the
// hit-test tree — `local_bounds()` covers only the header (~28px). When a
// user clicks a dropdown item that paints BELOW the header, the framework's
// hit_test() walks the view tree and lands on whichever sibling/ancestor
// view occupies that pixel — typically a label, panel, or the root —
// NOT the ComboBox.
//
// Each window-host platform layer (window_host_mac.mm, window_host_x11.cpp,
// window_host_win32.cpp) is REQUIRED to consult ComboBox::active_popup_
// BEFORE calling hit_test on a mouse-down, and to route clicks that fall
// inside the dropdown's projected rectangle directly to the popup owner's
// on_mouse_event(). Without that bypass, the popup snaps closed with no
// selection (notify_global_click() fires for the view behind the dropdown).
//
// These tests pin the contract those platform layers depend on:
//   1. active_popup_ tracks the open combo, nulled on close.
//   2. The combo's hit-test bounds DO NOT extend to cover the dropdown
//      (i.e. the bypass is necessary, not redundant — refactoring the
//      dropdown into a real child view would change this and is what the
//      assertion below catches).
//   3. notify_global_click(unrelated_view) closes the popup; passing the
//      combo itself or a descendant keeps it open.
//
// If you change the dropdown's structure (e.g. promote it to a child View
// with real bounds), update this test AND every window_host_*.{mm,cpp}
// that special-cases active_popup_.

TEST_CASE("ComboBox: active_popup_ + hit_test gap forces platform bypass",
          "[combo][regression]") {
    // Reset global state — other tests in this binary may leave it set.
    ComboBox::close_active_popup();
    REQUIRE(ComboBox::active_popup_ == nullptr);

    ComboBox combo;
    combo.set_items({"One", "Two", "Three"});
    combo.set_selected(0);
    combo.set_bounds({0, 0, 140, 28});  // header-only — typical combo size

    REQUIRE_FALSE(combo.is_open());

    // Open via a click on the header.
    MouseEvent open;
    open.position = {70.0f, 14.0f};
    open.is_down = true;
    combo.on_mouse_event(open);

    REQUIRE(combo.is_open());
    REQUIRE(ComboBox::active_popup_ == &combo);

    // Critical invariant: the combo's local bounds still cover ONLY the
    // header (28px tall), even though the dropdown items paint at y >= 30.
    // If this changes (e.g. someone promotes the dropdown to a real child
    // view), the platform-layer bypass becomes redundant — and this test
    // is the trigger to revisit window_host_mac.mm + siblings.
    REQUIRE(combo.local_bounds().height == 28.0f);
    REQUIRE(combo.local_bounds().width  == 140.0f);

    // close_dropdown via the global-click escape hatch the platform layer
    // uses for "click landed on a non-popup view": active_popup_ must be
    // cleared so the next hit_test resumes normal routing.
    pulp::view::View other_view;
    other_view.set_bounds({0, 0, 100, 100});
    ComboBox::notify_global_click(&other_view);

    REQUIRE_FALSE(combo.is_open());
    REQUIRE(ComboBox::active_popup_ == nullptr);
}

TEST_CASE("ComboBox: notify_global_click on combo itself keeps popup open",
          "[combo][regression]") {
    ComboBox::close_active_popup();
    ComboBox combo;
    combo.set_items({"A", "B", "C"});
    combo.set_bounds({0, 0, 140, 28});

    MouseEvent open;
    open.position = {70.0f, 14.0f};
    open.is_down = true;
    combo.on_mouse_event(open);
    REQUIRE(combo.is_open());

    // The platform layer routes "click inside popup bounds" by passing
    // the combo itself as the target — the popup must NOT close.
    ComboBox::notify_global_click(&combo);
    REQUIRE(combo.is_open());
    REQUIRE(ComboBox::active_popup_ == &combo);

    ComboBox::close_active_popup();
}

TEST_CASE("ComboBox: opening a second dropdown closes the first",
          "[combo][regression]") {
    ComboBox::close_active_popup();
    ComboBox a; a.set_items({"x"}); a.set_bounds({0, 0, 100, 28});
    ComboBox b; b.set_items({"y"}); b.set_bounds({0, 40, 100, 28});

    MouseEvent click;
    click.position = {50.0f, 14.0f};
    click.is_down = true;

    a.on_mouse_event(click);
    REQUIRE(a.is_open());
    REQUIRE(ComboBox::active_popup_ == &a);

    b.on_mouse_event(click);
    REQUIRE(b.is_open());
    REQUIRE_FALSE(a.is_open());
    REQUIRE(ComboBox::active_popup_ == &b);

    ComboBox::close_active_popup();
}

// When a ComboBox is destroyed while its dropdown is open, `~ComboBox`
// must clear the static `active_popup_` slot. Otherwise the platform window
// host dereferences a dangling pointer on the next mouseDown. Reproducer:
// open dropdown (sets active_popup_), drop the ComboBox, assert the slot is
// nullptr.
TEST_CASE("ComboBox: dtor clears active_popup_ when destroyed while open",
          "[combo][regression]") {
    ComboBox::close_active_popup();
    REQUIRE(ComboBox::active_popup_ == nullptr);

    {
        ComboBox combo;
        combo.set_items({"one", "two", "three"});
        combo.set_bounds({0, 0, 100, 28});

        MouseEvent click;
        click.position = {50.0f, 14.0f};
        click.is_down = true;
        combo.on_mouse_event(click);

        REQUIRE(combo.is_open());
        REQUIRE(ComboBox::active_popup_ == &combo);
    }
    // Destructor ran — the static must NOT still hold a freed pointer,
    // because any subsequent `notify_global_click` / window-host
    // `active_popup_` deref would crash.
    REQUIRE(ComboBox::active_popup_ == nullptr);

    // notify_global_click is the hot path called from -[PulpView mouseDown:].
    // Must be safe to call after the dropdown owner has been freed.
    ComboBox::notify_global_click(nullptr);
    REQUIRE(ComboBox::active_popup_ == nullptr);
}

// Same cleanup contract for `View::active_overlay_`: assert the `~View()`
// clear explicitly so a future refactor that drops it is caught immediately
// rather than at the next user click.
TEST_CASE("View: dtor clears active_overlay_ when destroyed while claimed",
          "[view][regression]") {
    REQUIRE(pulp::view::View::active_overlay_ == nullptr);

    {
        pulp::view::View v;
        v.claim_overlay();
        REQUIRE(pulp::view::View::active_overlay_ == &v);
    }
    REQUIRE(pulp::view::View::active_overlay_ == nullptr);
}

// pulp #68 — host-level ESC fallback contract. The macOS window host's
// keyDown: dispatches `ComboBox::close_active_popup()` when ESC is
// pressed and `active_popup_` is set, even if the focused view is not
// the ComboBox itself. This covers the React-popover focus-steal case:
// dropdown is open, sibling JS-driven element grabbed focus, ESC must
// still close the still-visible dropdown.
//
// The macOS host code (window_host_mac.mm keyDown:) is the actual
// dispatcher and is not unit-testable in the Catch2 harness yet
// (tracked by pulp #2001 — Mac platform-test harness). What IS
// testable is the contract the host relies on:
//
//   close_active_popup() must close the dropdown unconditionally,
//   regardless of which view currently owns input focus.
//
// A regression that gated close_active_popup on focus ownership would
// silently break the host-level ESC path and surface only as "ESC
// doesn't close popovers" in the live host. This test fences that.
TEST_CASE("ComboBox: close_active_popup is focus-independent",
          "[combo][regression]") {
    ComboBox::close_active_popup();
    REQUIRE(ComboBox::active_popup_ == nullptr);

    ComboBox combo;
    combo.set_items({"one", "two"});
    combo.set_bounds({0, 0, 100, 28});

    // Open via mouse — combo is the implicit focused view in this path.
    MouseEvent click;
    click.position = {50.0f, 14.0f};
    click.is_down = true;
    combo.on_mouse_event(click);
    REQUIRE(combo.is_open());
    REQUIRE(ComboBox::active_popup_ == &combo);

    // Simulate focus being stolen by a sibling — focus_changed(false)
    // is what the host calls when input focus moves elsewhere. The
    // dropdown should remain visually open (active_popup_ unchanged).
    combo.on_focus_changed(false);
    REQUIRE(combo.is_open());
    REQUIRE(ComboBox::active_popup_ == &combo);

    // The host-level ESC fallback simply calls close_active_popup().
    // It MUST close the popup despite combo not owning focus.
    ComboBox::close_active_popup();
    REQUIRE_FALSE(combo.is_open());
    REQUIRE(ComboBox::active_popup_ == nullptr);
}


TEST_CASE("ComboBox: closed-stepper arrow keys skip separators (no separator commit)", "[combo]") {
    // When closed, Up/Down must step OVER "---" separator rows rather than
    // committing a separator index (which would fire on_change with an empty
    // selection). Items: 0=A, 1="---", 2=B.
    ComboBox combo;
    combo.set_items({"A", "---", "B"});
    combo.set_selected(0);

    int changed_to = -1;
    std::string changed_text;
    combo.on_change = [&](int idx) {
        changed_to = idx;
        changed_text = combo.selected_text();
    };

    KeyEvent down;
    down.key = KeyCode::down;
    down.is_down = true;
    REQUIRE(combo.on_key_event(down));   // steps from 0 over the separator to 2
    REQUIRE(changed_to == 2);            // NOT 1 (the "---" separator)
    REQUIRE(changed_text == "B");
    REQUIRE(combo.selected_text() == "B");

    // Going back up lands on A, never on the separator.
    KeyEvent up;
    up.key = KeyCode::up;
    up.is_down = true;
    REQUIRE(combo.on_key_event(up));
    REQUIRE(combo.selected_text() == "A");
}

TEST_CASE("ComboBox: open-menu Enter on a separator does not commit", "[combo]") {
    ComboBox combo;
    combo.set_items({"A", "---", "B"});
    combo.set_selected(0);
    combo.set_bounds({0, 0, 140, 120});

    // Open the menu.
    KeyEvent space;
    space.key = KeyCode::space;
    space.is_down = true;
    REQUIRE(combo.on_key_event(space));

    // Down from selected (0) should skip the separator via move_hover, but even if
    // a separator were highlighted, Enter must not commit it.
    int changed_to = -1;
    combo.on_change = [&](int idx) { changed_to = idx; };

    KeyEvent down;
    down.key = KeyCode::down;
    down.is_down = true;
    combo.on_key_event(down);  // move_hover skips "---" → highlights B (index 2)

    KeyEvent enter;
    enter.key = KeyCode::enter;
    enter.is_down = true;
    REQUIRE(combo.on_key_event(enter));
    REQUIRE(changed_to != 1);  // separator index 1 was never committed
    REQUIRE(combo.selected_text() != "---");
}
