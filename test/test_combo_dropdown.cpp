// Automated test for ComboBox dropdown interaction
#include <catch2/catch_test_macros.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/parameter_binding.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/state/store.hpp>
#include <pulp/state/parameter.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace pulp::view;
using pulp::canvas::RecordingCanvas;

// A combo bound via bind_parameter must follow host-automation writes (which
// the editor host drains each vsync via pump_listeners) AND write its own
// selection back to the store — the round trip that makes automation record
// and play back with the on-screen control animating.
TEST_CASE("ComboBox: bind_parameter round-trips with host automation", "[combo]") {
    using namespace pulp::state;
    StateStore store;
    ParamInfo info;
    info.id = 1;
    info.name = "Type";
    info.range = ParamRange{0.0f, 2.0f, 0.0f, 1.0f};  // stepped 0..2
    store.add_parameter(info);

    ComboBox combo;
    combo.set_items({"Res. LP", "Band-Pass", "Peaking"});
    auto pbind = bind_parameter(combo, store, 1);
    REQUIRE(combo.selected() == 0);

    // Host automation writes the parameter; the per-vsync pump drives the widget.
    store.set_value(1, 2.0f);
    store.pump_listeners();
    REQUIRE(combo.selected() == 2);  // followed automation (Peaking)

    // A UI selection writes back to the store (so the host records it).
    combo.set_selected(1);
    REQUIRE(store.get_value(1) == 1.0f);
}

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

// A long list in a short window must clamp its menu inside the window and scroll,
// so every item stays reachable (regression for dropdowns whose lower rows fell
// past the plugin's bottom edge and became unclickable).
TEST_CASE("ComboBox: long list clamps to the window and scrolls to reach items",
          "[combo]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 200, 100});  // deliberately short window

    auto owned = std::make_unique<ComboBox>();
    ComboBox* combo = owned.get();
    combo->set_bounds({0, 0, 120, 24});  // at the top of the window
    std::vector<std::string> items;
    for (int i = 0; i < 12; ++i) items.push_back("Item" + std::to_string(i));
    combo->set_items(items);
    combo->set_selected(10);             // a selection deep in the list
    root->add_child(std::move(owned));

    MouseEvent open_click;
    open_click.position = {60, 12};
    open_click.is_down = true;
    combo->on_mouse_event(open_click);
    REQUIRE(combo->is_open());

    // The menu rect must stay fully inside the window and be shorter than the
    // full list (i.e. it scrolled rather than spilling past the bottom).
    float x = 0, y = 0, w = 0, h = 0;
    REQUIRE(combo->dropdown_window_rect(x, y, w, h));
    REQUIRE(y >= 0.0f);
    REQUIRE(y + h <= 100.0f + 0.5f);         // clamped to the window bottom
    REQUIRE(w <= 200.0f + 0.5f);             // clamped to the window right edge
    REQUIRE(h < 12.0f * 24.0f);              // not the full list → scrolled

    // Opening scrolled the window toward the current selection, so the first
    // visible row is a deep item — clicking it selects an item that would have
    // been off-screen (and thus unselectable) without scrolling.
    MouseEvent pick;
    pick.position = {60, y + 12.0f};         // center of the first visible row
    pick.is_down = true;
    combo->on_mouse_event(pick);
    REQUIRE_FALSE(combo->is_open());
    REQUIRE(combo->selected() >= 4);         // a scrolled-to item, not one of the first rows
}

// Every item must be reachable from every starting selection — including the
// LAST item and moving more than one step (regression for "can't select
// Peaking / can't reach Pass-Through from Whisper" reports).
TEST_CASE("ComboBox: every item selectable from any start (fits in window)",
          "[combo]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 300, 300});  // roomy — the 3-item menu fits, no scroll
    auto owned = std::make_unique<ComboBox>();
    ComboBox* combo = owned.get();
    combo->set_bounds({0, 0, 140, 24});
    combo->set_items({"Res. LP", "Band-Pass", "Peaking"});
    root->add_child(std::move(owned));

    for (int start = 0; start < 3; ++start) {
        for (int target = 0; target < 3; ++target) {
            combo->set_selected(start);
            MouseEvent open_click; open_click.position = {70, 12}; open_click.is_down = true;
            combo->on_mouse_event(open_click);
            REQUIRE(combo->is_open());
            float x = 0, y = 0, w = 0, h = 0;
            REQUIRE(combo->dropdown_window_rect(x, y, w, h));  // y == top_local (combo at origin)
            MouseEvent pick; pick.position = {70, y + target * 24.0f + 12.0f}; pick.is_down = true;
            combo->on_mouse_event(pick);
            INFO("start=" << start << " target=" << target);
            REQUIRE(combo->selected() == target);
        }
    }
}

// A wheel event over an open, overflowing menu scrolls the item window (and is
// consumed) so clipped items become reachable — this is what the mac host's
// scrollWheel active_popup_ bypass routes to.
TEST_CASE("ComboBox: wheel scrolls an overflowing open menu", "[combo]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 200, 100});  // short window → menu clamps + scrolls
    auto owned = std::make_unique<ComboBox>();
    ComboBox* combo = owned.get();
    combo->set_bounds({0, 0, 120, 24});
    std::vector<std::string> items;
    for (int i = 0; i < 12; ++i) items.push_back("Item" + std::to_string(i));
    combo->set_items(items);
    combo->set_selected(0);
    root->add_child(std::move(owned));

    MouseEvent open_click; open_click.position = {60, 12}; open_click.is_down = true;
    combo->on_mouse_event(open_click);
    REQUIRE(combo->is_open());

    // Opened on item 0 → top of the list is visible; scroll down reveals later
    // items. Wheel down a few times, then the last visible row must be a deeper
    // item than fit on the first page.
    float x = 0, y = 0, w = 0, h = 0;
    REQUIRE(combo->dropdown_window_rect(x, y, w, h));
    const int first_page_rows = static_cast<int>(h / 24.0f);

    for (int k = 0; k < 6; ++k) {
        MouseEvent wheel; wheel.is_wheel = true; wheel.scroll_delta_y = 1.0f;
        wheel.position = {60, y + 12.0f};
        combo->on_mouse_event(wheel);
        REQUIRE(combo->is_open());  // wheel must NOT close the menu
    }
    // After scrolling, clicking the first visible row selects an item beyond the
    // first page — i.e. a previously-clipped item became reachable.
    combo->dropdown_window_rect(x, y, w, h);
    MouseEvent pick; pick.position = {60, y + 12.0f}; pick.is_down = true;
    combo->on_mouse_event(pick);
    REQUIRE(combo->selected() >= first_page_rows);
}

// Robotization-editor geometry: a 5-item FFT combo high in a short (~230px)
// single-row panel. The menu must clamp inside the panel and scroll — not spill
// past the bottom edge (the "FFT falls behind the plugin" report).
TEST_CASE("ComboBox: FFT-size menu clamps inside a short editor panel", "[combo]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 788, 230});         // robot editor panel size
    auto owned = std::make_unique<ComboBox>();
    ComboBox* combo = owned.get();
    combo->set_bounds({560, 115, 120, 28});      // FFT combo, upper-middle row
    combo->set_items({"256", "512", "1024", "2048", "4096"});
    root->add_child(std::move(owned));

    MouseEvent open_click; open_click.position = {5, 14}; open_click.is_down = true;
    combo->on_mouse_event(open_click);
    REQUIRE(combo->is_open());

    float x = 0, y = 0, w = 0, h = 0;
    REQUIRE(combo->dropdown_window_rect(x, y, w, h));
    INFO("menu y=" << y << " h=" << h << " bottom=" << (y + h) << " panel=230");
    REQUIRE(y + h <= 230.0f + 0.5f);             // clamped inside the panel
    REQUIRE(h < 5.0f * 24.0f);                    // not all 5 rows → clamped + scrollable
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
TEST_CASE("ComboBox: close_active_popup is focus-independent [issue-68]",
          "[combo][regression][issue-68]") {
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
    // Regression: when closed, Up/Down must step OVER "---" separator rows rather
    // than committing a separator index (which would fire on_change with an empty
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

// hit_test claims the whole open menu (so hover/click reach the combo even
// though the menu paints outside the header bounds), and on_hover_move — the
// path the platform host uses for hover samples over the menu — moves the row
// highlight without committing the selection.
TEST_CASE("ComboBox: hit_test claims the open menu and on_hover_move tracks the row",
          "[combo]") {
    ComboBox::close_active_popup();
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 300, 300});  // roomy — the 3-item menu fits, no scroll
    auto owned = std::make_unique<ComboBox>();
    ComboBox* combo = owned.get();
    combo->set_bounds({0, 0, 140, 24});
    combo->set_items({"Res. LP", "Band-Pass", "Peaking"});
    combo->set_selected(0);
    root->add_child(std::move(owned));

    // Closed: only the header is hit-testable.
    REQUIRE(combo->hit_test({70.0f, 12.0f}) == combo);

    MouseEvent open; open.position = {70.0f, 12.0f}; open.is_down = true;
    combo->on_mouse_event(open);
    REQUIRE(combo->is_open());

    float x = 0, y = 0, w = 0, h = 0;
    REQUIRE(combo->dropdown_window_rect(x, y, w, h));  // y == menu top (combo at origin)

    // Open: hit_test now claims a point over the menu, below the header.
    REQUIRE(combo->hit_test({70.0f, y + 12.0f}) == combo);

    // Hover moves the highlight only — the selection is not committed.
    combo->on_hover_move({70.0f, y + 24.0f + 12.0f});  // row 1
    REQUIRE(combo->hovered_index() == 1);
    REQUIRE(combo->selected() == 0);
    combo->on_hover_move({70.0f, y + 12.0f});          // row 0
    REQUIRE(combo->hovered_index() == 0);

    // A hover outside the menu clears the highlight.
    combo->on_hover_move({70.0f, y + h + 100.0f});
    REQUIRE(combo->hovered_index() == -1);

    ComboBox::close_active_popup();
}

// Keyboard navigation through a menu taller than its window must scroll the
// visible row window so every item — including the last — stays reachable and
// commits correctly (exercises the move_hover scroll-follow path).
TEST_CASE("ComboBox: keyboard nav scrolls the visible window in an overflowing menu",
          "[combo]") {
    ComboBox::close_active_popup();
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 200, 100});  // short → the 12-item menu overflows
    auto owned = std::make_unique<ComboBox>();
    ComboBox* combo = owned.get();
    combo->set_bounds({0, 0, 120, 24});
    std::vector<std::string> items;
    for (int i = 0; i < 12; ++i) items.push_back("Item" + std::to_string(i));
    combo->set_items(items);
    combo->set_selected(0);
    root->add_child(std::move(owned));

    KeyEvent enter; enter.key = KeyCode::enter; enter.is_down = true;
    combo->on_key_event(enter);
    REQUIRE(combo->is_open());
    REQUIRE(combo->hovered_index() == 0);

    KeyEvent down; down.key = KeyCode::down; down.is_down = true;
    for (int k = 0; k < 11; ++k) combo->on_key_event(down);
    REQUIRE(combo->hovered_index() == 11);  // reached the last row despite overflow

    KeyEvent commit; commit.key = KeyCode::enter; commit.is_down = true;
    combo->on_key_event(commit);
    REQUIRE(combo->selected() == 11);
    REQUIRE_FALSE(combo->is_open());

    ComboBox::close_active_popup();
}

// A menu that both overflows its window (scroll carets top+bottom) AND extends
// past the window's right edge (width clamp) must still paint its overlay and
// stay clamped inside the window on both axes.
TEST_CASE("ComboBox: overflowing menu near the right edge paints clamped with carets",
          "[combo]") {
    ComboBox::close_active_popup();
    View::overlay_queue().clear();

    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 300, 120});
    auto owned = std::make_unique<ComboBox>();
    ComboBox* combo = owned.get();
    // Narrow field near the right edge, but fully inside the window. Its items
    // are wide enough that the menu's natural width (dropdown_width_hint) spills
    // past the right edge, so the horizontal clamp must shrink it.
    combo->set_bounds({200, 0, 80, 24});
    std::vector<std::string> items;
    for (int i = 0; i < 12; ++i)
        items.push_back("Really Long Filter Option " + std::to_string(i));
    combo->set_items(items);
    combo->set_selected(6);  // middle selection → carets both above and below
    root->add_child(std::move(owned));

    const float hint = combo->dropdown_width_hint();
    REQUIRE(hint > 80.0f);  // items force the menu wider than the field

    KeyEvent enter; enter.key = KeyCode::enter; enter.is_down = true;
    combo->on_key_event(enter);
    REQUIRE(combo->is_open());

    RecordingCanvas rc;
    combo->paint(rc);  // queues the dropdown overlay on the root's per-root queue (S11)
    REQUIRE(root->interaction().overlay_queue.size() >= 1);
    const auto before = rc.command_count();
    View::paint_overlays(rc, root.get());  // drain the painting root's own queue
    REQUIRE(root->interaction().overlay_queue.empty());
    REQUIRE(rc.command_count() > before);  // the menu (rows + carets) painted

    float x = 0, y = 0, w = 0, h = 0;
    REQUIRE(combo->dropdown_window_rect(x, y, w, h));
    REQUIRE(w < hint);                // horizontal clamp shrank the natural width
    REQUIRE(x + w <= 300.0f + 0.5f);  // clamped inside the right edge
    REQUIRE(y + h <= 120.0f + 0.5f);  // clamped inside the bottom edge
    REQUIRE(h < 12.0f * 24.0f);       // not the full list → scrolled

    ComboBox::close_active_popup();
}

// DesignStepper shares the ComboBox binding contract: automation playback drives
// the selection silently, and a user step writes back through a one-shot gesture.
TEST_CASE("DesignStepper: bind_parameter round-trips with host automation", "[combo]") {
    using namespace pulp::state;
    StateStore store;
    ParamInfo info;
    info.id = 2;
    info.name = "Wave";
    info.range = ParamRange{0.0f, 3.0f, 0.0f, 1.0f};  // stepped 0..3
    store.add_parameter(info);

    DesignStepper stepper({"Sine", "Saw", "Square", "Triangle"}, 0);
    auto sbind = bind_parameter(stepper, store, 2);
    REQUIRE(stepper.selected() == 0);

    // Host automation writes the parameter; the per-vsync pump drives the widget.
    store.set_value(2, 3.0f);
    store.pump_listeners();
    REQUIRE(stepper.selected() == 3);  // followed automation (Triangle)

    // A user step writes back to the store as a gesture (so the host records it).
    stepper.on_select(1);
    REQUIRE(store.get_value(2) == 1.0f);
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
