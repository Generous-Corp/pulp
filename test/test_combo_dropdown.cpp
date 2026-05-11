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
    // Should be open now (internal state)

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

// ── Regression: dropdown overlay click routing ───────────────────────────
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

TEST_CASE("ComboBox: active_popup_ + hit_test gap forces platform bypass [issue-overlay]",
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

TEST_CASE("ComboBox: notify_global_click on combo itself keeps popup open [issue-overlay]",
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

TEST_CASE("ComboBox: opening a second dropdown closes the first [issue-overlay]",
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

// pulp #1818 — when a ComboBox is destroyed while its dropdown is open,
// `~ComboBox` MUST clear the static `active_popup_` slot. Otherwise the
// platform window host dereferences a dangling pointer on the next
// mouseDown (PAC failure on the vtable load — exact crash signature in
// the issue report). Reproducer: open dropdown (sets active_popup_),
// drop the ComboBox, assert the slot is nullptr.
TEST_CASE("ComboBox: dtor clears active_popup_ when destroyed while open [issue-1818]",
          "[combo][regression][issue-1818]") {
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

// pulp #1818 — same shape for `View::active_overlay_`. This regression
// guard mirrors the pulp #1148 fix already present in `~View()` but
// asserts it explicitly so a future refactor that drops the clear is
// caught immediately rather than at the next user click.
TEST_CASE("View: dtor clears active_overlay_ when destroyed while claimed [issue-1818]",
          "[view][regression][issue-1818]") {
    REQUIRE(pulp::view::View::active_overlay_ == nullptr);

    {
        pulp::view::View v;
        v.claim_overlay();
        REQUIRE(pulp::view::View::active_overlay_ == &v);
    }
    REQUIRE(pulp::view::View::active_overlay_ == nullptr);
}

