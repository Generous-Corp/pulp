#include <catch2/catch_test_macros.hpp>
#include <pulp/view/context_menu.hpp>
#include <pulp/canvas/canvas.hpp>

#include <optional>

using namespace pulp::view;
using namespace pulp::canvas;

namespace {

// A root big enough that the menu never flips, anchored at (10,10).
std::unique_ptr<View> make_root() {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 400});
    return root;
}

using Item = ContextMenu::Item;

constexpr float kAnchorX = 10.0f, kAnchorY = 10.0f;

// The center of row `i`, taken from the menu's own resolved geometry.
//
// This used to assume a uniform 24px grid (kAnchorY + i*24 + 12) — which is
// exactly the "divide by a row constant" mistake the menu's rows are laid out to
// prevent. It only ever worked because a separator was secretly sized as a full
// label row; the moment a separator became the hairline it looks like, every
// index past one landed in the wrong row. Ask the menu where its rows are.
float row_center_y(const ContextMenu& menu, int i) {
    const auto rows = menu.layout().rows;
    return rows[static_cast<size_t>(i)].y + rows[static_cast<size_t>(i)].height * 0.5f;
}
constexpr float kInsideX = 50.0f;  // well within the >=120px-wide box at x=10

MouseEvent down_at(float x, float y) {
    MouseEvent e;
    e.position = {x, y};
    e.is_down = true;
    return e;
}

KeyEvent key_down(KeyCode k) {
    KeyEvent e;
    e.key = k;
    e.is_down = true;
    return e;
}

}  // namespace

TEST_CASE("ContextMenu show mounts as last child and set_items round-trips",
          "[view][context-menu]") {
    auto root = make_root();
    auto* menu = ContextMenu::show(root.get(), {kAnchorX, kAnchorY},
                                   {{1, "Cut"}, {2, "Copy"}}, {});
    REQUIRE(menu != nullptr);
    REQUIRE(menu->items().size() == 2);
    REQUIRE(menu->parent() == root.get());
    REQUIRE(menu->bounds().width == 400);
    REQUIRE(menu->bounds().height == 400);
}

TEST_CASE("ContextMenu click on a row selects and removes the menu",
          "[view][context-menu]") {
    auto root = make_root();
    std::optional<int> got;
    bool fired = false;

    auto* menu = ContextMenu::show(
        root.get(), {kAnchorX, kAnchorY},
        {{10, "Cut"}, {20, "Copy"}, {30, "Paste"}},
        [&](std::optional<int> id) { fired = true; got = id; });

    // Click row 1 ("Copy", id=20).
    menu->on_mouse_event(down_at(kInsideX, row_center_y(*menu, 1)));

    REQUIRE(fired);
    REQUIRE(got.has_value());
    REQUIRE(*got == 20);
    // The wrapped on_close detached the menu from root.
    REQUIRE(root->child_count() == 0);
}

TEST_CASE("ContextMenu click outside the box dismisses with nullopt",
          "[view][context-menu]") {
    auto root = make_root();
    std::optional<int> got = 99;  // sentinel
    bool fired = false;

    auto* menu = ContextMenu::show(
        root.get(), {kAnchorX, kAnchorY},
        {{1, "One"}, {2, "Two"}},
        [&](std::optional<int> id) { fired = true; got = id; });

    // Click far outside the menu box (bottom-right corner of the root).
    menu->on_mouse_event(down_at(380.0f, 380.0f));

    REQUIRE(fired);
    REQUIRE_FALSE(got.has_value());
    REQUIRE(root->child_count() == 0);
}

TEST_CASE("ContextMenu keyboard down+enter selects, skipping separators",
          "[view][context-menu]") {
    auto root = make_root();
    std::optional<int> got;

    auto* menu = ContextMenu::show(
        root.get(), {kAnchorX, kAnchorY},
        {{1, "First"}, Item::make_separator(), {3, "Third"}},
        [&](std::optional<int> id) { got = id; });

    // No hover yet → first Down lands on row 0 ("First").
    REQUIRE(menu->hovered_index() == -1);
    REQUIRE(menu->on_key_event(key_down(KeyCode::down)));
    REQUIRE(menu->hovered_index() == 0);

    // Next Down must SKIP the separator (row 1) and land on row 2 ("Third").
    REQUIRE(menu->on_key_event(key_down(KeyCode::down)));
    REQUIRE(menu->hovered_index() == 2);

    REQUIRE(menu->on_key_event(key_down(KeyCode::enter)));
    REQUIRE(got.has_value());
    REQUIRE(*got == 3);
    REQUIRE(root->child_count() == 0);
}

TEST_CASE("ContextMenu Home and End jump to the selectable ends",
          "[view][context-menu]") {
    auto root = make_root();
    std::optional<int> got;

    // Both ends are unselectable on purpose: End must walk INWARD past the
    // trailing separator and Home past the leading disabled row, so neither
    // key can park the cursor somewhere Enter would then refuse.
    Item disabled{1, "Disabled"};
    disabled.enabled = false;
    auto* menu = ContextMenu::show(
        root.get(), {kAnchorX, kAnchorY},
        {disabled, {2, "Second"}, {3, "Third"}, Item::make_separator()},
        [&](std::optional<int> id) { got = id; });

    REQUIRE(menu->hovered_index() == -1);
    REQUIRE(menu->on_key_event(key_down(KeyCode::end_)));
    REQUIRE(menu->hovered_index() == 2);

    REQUIRE(menu->on_key_event(key_down(KeyCode::home)));
    REQUIRE(menu->hovered_index() == 1);

    REQUIRE(menu->on_key_event(key_down(KeyCode::enter)));
    REQUIRE(got.has_value());
    REQUIRE(*got == 2);
}

// The transport, not the handling. `on_key_event` has understood up/down
// since the widget shipped, but BOTH hosts gate navigation keys on
// `accepts_navigation_input()` -- the plugin editor at
// plugin_view_host_mac.mm and the standalone at window_host_mac.mm -- and
// ContextMenu inherited the base `false`. So a focused menu's arrow keys went
// back to the DAW and the handler was never called. Every other keyboard case
// in this file drives `on_key_event` directly and so cannot see that.
TEST_CASE("ContextMenu claims navigation keys while it is open",
          "[view][context-menu]") {
    auto root = make_root();
    std::optional<int> got;

    auto* menu = ContextMenu::show(
        root.get(), {kAnchorX, kAnchorY}, {{1, "First"}, {2, "Second"}},
        [&](std::optional<int> id) { got = id; });

    REQUIRE(menu->accepts_navigation_input());
    // Control: a plain View in the same tree does not claim them, so the
    // predicate is discriminating rather than universally true.
    View plain;
    REQUIRE_FALSE(plain.accepts_navigation_input());

    REQUIRE(menu->on_key_event(key_down(KeyCode::escape)));
    REQUIRE(got.has_value() == false);
    // Dismissed: the menu is detached and destroyed by the close wrapper, so
    // nothing is left in the tree to claim the keys. `menu` is dangling here
    // by design -- do not dereference it; the tree is the observable.
    REQUIRE(root->child_count() == 0);
}

TEST_CASE("ContextMenu keyboard skips disabled rows",
          "[view][context-menu]") {
    auto root = make_root();
    std::optional<int> got;

    auto* menu = ContextMenu::show(
        root.get(), {kAnchorX, kAnchorY},
        {{1, "Enabled"}, {2, "Disabled", /*enabled=*/false}, {3, "AlsoEnabled"}},
        [&](std::optional<int> id) { got = id; });

    REQUIRE(menu->on_key_event(key_down(KeyCode::down)));  // row 0
    REQUIRE(menu->hovered_index() == 0);
    REQUIRE(menu->on_key_event(key_down(KeyCode::down)));  // skip disabled row 1
    REQUIRE(menu->hovered_index() == 2);
    REQUIRE(menu->on_key_event(key_down(KeyCode::enter)));
    REQUIRE(got.has_value());
    REQUIRE(*got == 3);
}

TEST_CASE("ContextMenu Escape dismisses with nullopt",
          "[view][context-menu]") {
    auto root = make_root();
    std::optional<int> got = 99;
    bool fired = false;

    auto* menu = ContextMenu::show(
        root.get(), {kAnchorX, kAnchorY},
        {{1, "One"}},
        [&](std::optional<int> id) { fired = true; got = id; });

    REQUIRE(menu->on_key_event(key_down(KeyCode::escape)));
    REQUIRE(fired);
    REQUIRE_FALSE(got.has_value());
    REQUIRE(root->child_count() == 0);
}

TEST_CASE("ContextMenu click on a disabled row does not select",
          "[view][context-menu]") {
    auto root = make_root();
    std::optional<int> got;
    bool fired = false;

    auto* menu = ContextMenu::show(
        root.get(), {kAnchorX, kAnchorY},
        {{1, "Enabled"}, {2, "Disabled", /*enabled=*/false}, {3, "Third"}},
        [&](std::optional<int> id) { fired = true; got = id; });

    // Click the disabled row (row 1) — must NOT fire, menu stays open.
    menu->on_mouse_event(down_at(kInsideX, row_center_y(*menu, 1)));
    REQUIRE_FALSE(fired);
    REQUIRE(root->child_count() == 1);  // still mounted

    // A subsequent click on an enabled row still works.
    menu->on_mouse_event(down_at(kInsideX, row_center_y(*menu, 2)));
    REQUIRE(fired);
    REQUIRE(got.has_value());
    REQUIRE(*got == 3);
}

TEST_CASE("ContextMenu click on a separator does not select",
          "[view][context-menu]") {
    auto root = make_root();
    bool fired = false;

    auto* menu = ContextMenu::show(
        root.get(), {kAnchorX, kAnchorY},
        {{1, "One"}, Item::make_separator(), {3, "Three"}},
        [&](std::optional<int>) { fired = true; });

    // Click the separator (row 1) — not selectable, menu stays open.
    menu->on_mouse_event(down_at(kInsideX, row_center_y(*menu, 1)));
    REQUIRE_FALSE(fired);
    REQUIRE(root->child_count() == 1);
}

TEST_CASE("ContextMenu hover only highlights enabled non-separator rows",
          "[view][context-menu]") {
    auto root = make_root();
    auto* menu = ContextMenu::show(
        root.get(), {kAnchorX, kAnchorY},
        {{1, "One"}, Item::make_separator(), {3, "Three", /*enabled=*/false}},
        {});

    MouseEvent hover;  // move (not down) over the separator row
    hover.position = {kInsideX, row_center_y(*menu, 1)};
    hover.is_down = false;
    menu->on_mouse_event(hover);
    REQUIRE(menu->hovered_index() == -1);

    hover.position = {kInsideX, row_center_y(*menu, 2)};  // disabled row
    menu->on_mouse_event(hover);
    REQUIRE(menu->hovered_index() == -1);

    hover.position = {kInsideX, row_center_y(*menu, 0)};  // enabled row
    menu->on_mouse_event(hover);
    REQUIRE(menu->hovered_index() == 0);
}

TEST_CASE("ContextMenu on_close fires exactly once per lifecycle",
          "[view][context-menu]") {
    auto root = make_root();
    int calls = 0;
    auto* menu = ContextMenu::show(
        root.get(), {kAnchorX, kAnchorY}, {{1, "One"}},
        [&](std::optional<int>) { ++calls; });

    menu->on_key_event(key_down(KeyCode::escape));
    REQUIRE(calls == 1);
    // The menu is detached now; no further input is delivered in practice, but
    // a stray late event must never re-fire the callback (latched by closed_).
    // (menu pointer is dangling after removal, so we don't touch it again.)
    REQUIRE(root->child_count() == 0);
}

TEST_CASE("ContextMenu paint renders without a window", "[view][context-menu]") {
    auto root = make_root();
    auto* menu = ContextMenu::show(
        root.get(), {kAnchorX, kAnchorY},
        {{1, "Cut"}, {2, "Copy", true, /*checked=*/true},
         Item::make_separator(), {3, "Paste", /*enabled=*/false}},
        {});

    RecordingCanvas canvas;
    auto before = canvas.command_count();
    menu->paint(canvas);
    REQUIRE(canvas.command_count() > before);
}

// ── Up-arrow navigation ──────────────────────────────────────────────────
//
// `move_hover(-1)` had no coverage at all: every keyboard case above drives
// the menu with Down. Up is not the mirror image of Down for free — it has
// its own seeding branch (no hover yet → start from the BOTTOM row) and its
// own edge behaviour (at the top, stay put rather than wrap or go negative),
// and both are reachable only through Up.

TEST_CASE("ContextMenu keyboard up seeds from the bottom and skips separators",
          "[view][context-menu]") {
    auto root = make_root();
    std::optional<int> got;

    auto* menu = ContextMenu::show(
        root.get(), {kAnchorX, kAnchorY},
        {{1, "First"}, Item::make_separator(), {3, "Third"}},
        [&](std::optional<int> id) { got = id; });

    // No hover yet → the first Up seeds from the bottom end.
    REQUIRE(menu->hovered_index() == -1);
    REQUIRE(menu->on_key_event(key_down(KeyCode::up)));
    REQUIRE(menu->hovered_index() == 2);

    // Up must SKIP the separator (row 1) and land on row 0.
    REQUIRE(menu->on_key_event(key_down(KeyCode::up)));
    REQUIRE(menu->hovered_index() == 0);

    // Up at the top edge stays put — it must not wrap or go negative.
    REQUIRE(menu->on_key_event(key_down(KeyCode::up)));
    REQUIRE(menu->hovered_index() == 0);

    // Return commits the highlighted row and closes the menu.
    REQUIRE(menu->on_key_event(key_down(KeyCode::enter)));
    REQUIRE(got.has_value());
    REQUIRE(*got == 1);
    REQUIRE(root->child_count() == 0);
}

TEST_CASE("ContextMenu keyboard up skips disabled rows",
          "[view][context-menu]") {
    auto root = make_root();
    std::optional<int> got;

    auto* menu = ContextMenu::show(
        root.get(), {kAnchorX, kAnchorY},
        {{1, "Enabled"}, {2, "Disabled", /*enabled=*/false}, {3, "AlsoEnabled"}},
        [&](std::optional<int> id) { got = id; });

    REQUIRE(menu->on_key_event(key_down(KeyCode::up)));  // seeds on row 2
    REQUIRE(menu->hovered_index() == 2);
    REQUIRE(menu->on_key_event(key_down(KeyCode::up)));  // skip disabled row 1
    REQUIRE(menu->hovered_index() == 0);
    REQUIRE(menu->on_key_event(key_down(KeyCode::enter)));
    REQUIRE(got.has_value());
    REQUIRE(*got == 1);
}

// The existing outside-click case calls `on_mouse_event` on the menu directly,
// which assumes the routing rather than exercising it. Drive the press from the
// root instead, so the tree is what decides the menu is the target.
TEST_CASE("ContextMenu outside click routed from the root dismisses with nullopt",
          "[view][context-menu]") {
    auto root = make_root();
    std::optional<int> got = 99;  // sentinel
    bool fired = false;

    auto* menu = ContextMenu::show(
        root.get(), {kAnchorX, kAnchorY},
        {{1, "One"}, {2, "Two"}},
        [&](std::optional<int> id) { fired = true; got = id; });

    // Control: the menu really is what the tree resolves at that far corner —
    // its hit_test claims the whole overlay precisely so outside clicks reach it.
    REQUIRE(root->hit_test({380.0f, 380.0f}) == menu);

    root->simulate_click({380.0f, 380.0f});

    REQUIRE(fired);
    REQUIRE_FALSE(got.has_value());
    REQUIRE(root->child_count() == 0);
}

// ── Overflowing menus: cap, scroll, clip ─────────────────────────────────
//
// A band context menu carries 17 rows in a 376px editor. The panel used to
// keep its full content height no matter how short the overlay was: the
// flip-up drove `y` negative, the clamp to 0 pinned the panel to the top
// edge, and every row past the overlay was laid out, hit-tested and PAINTED
// outside the panel, over the app behind it. The trailing rows could not be
// reached at all. The panel is now capped to the overlay and its rows scroll.

namespace {

// Deliberately shorter than the menu mounted into it.
std::unique_ptr<View> make_short_root(float height) {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, height});
    return root;
}

std::vector<Item> numbered_items(int n) {
    std::vector<Item> items;
    items.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        items.push_back(Item{i + 1, "Row " + std::to_string(i + 1)});
    return items;
}

MouseEvent wheel_by(float dy, float x, float y) {
    MouseEvent e;
    e.position = {x, y};
    e.is_wheel = true;
    e.scroll_delta_y = dy;
    return e;
}

// Scroll to the very bottom regardless of how many notches that takes.
void scroll_to_end(ContextMenu& menu) {
    for (int i = 0; i < 200; ++i)
        menu.on_mouse_event(wheel_by(50.0f, kInsideX, 50.0f));
}

constexpr float kShortRoot = 200.0f;
constexpr int kOverflowRows = 20;

} // namespace

TEST_CASE("ContextMenu taller than its overlay is capped to fit", "[view][context-menu]") {
    auto root = make_short_root(kShortRoot);
    auto* menu =
        ContextMenu::show(root.get(), {kAnchorX, kAnchorY}, numbered_items(kOverflowRows), {});
    const auto lay = menu->layout();

    // Control: the scenario must really overflow, or everything below is
    // vacuous — a menu that fits would satisfy the cap assertions trivially.
    REQUIRE(lay.content_height > kShortRoot);

    REQUIRE(lay.box.height <= kShortRoot);
    REQUIRE(lay.box.y >= 0.0f);
    REQUIRE(lay.box.y + lay.box.height <= kShortRoot);
    REQUIRE(lay.max_scroll > 0.0f);
}

TEST_CASE("ContextMenu that already fits neither caps nor scrolls", "[view][context-menu]") {
    // The negative control for the whole feature: on a roomy overlay the
    // panel keeps its natural height and the wheel does nothing.
    auto root = make_root(); // 400x400
    auto* menu = ContextMenu::show(root.get(), {kAnchorX, kAnchorY}, numbered_items(4), {});
    const auto lay = menu->layout();
    REQUIRE(lay.content_height <= 400.0f);
    REQUIRE(lay.box.height == lay.content_height);
    REQUIRE(lay.max_scroll == 0.0f);

    menu->on_mouse_event(wheel_by(120.0f, kInsideX, 50.0f));
    REQUIRE(menu->scroll_offset() == 0.0f);
    REQUIRE(menu->layout().box.height == lay.content_height);
}

TEST_CASE("ContextMenu wheel scrolls a capped menu and clamps at both ends",
          "[view][context-menu]") {
    auto root = make_short_root(kShortRoot);
    auto* menu =
        ContextMenu::show(root.get(), {kAnchorX, kAnchorY}, numbered_items(kOverflowRows), {});
    const float max_scroll = menu->layout().max_scroll;
    REQUIRE(max_scroll > 0.0f); // control

    menu->on_mouse_event(wheel_by(30.0f, kInsideX, 50.0f));
    REQUIRE(menu->scroll_offset() == 30.0f);

    // Scrolling up past the top clamps at 0 rather than going negative.
    menu->on_mouse_event(wheel_by(-500.0f, kInsideX, 50.0f));
    REQUIRE(menu->scroll_offset() == 0.0f);

    // Scrolling down past the end clamps at max_scroll, so the last row
    // lands flush with the bottom instead of running off it.
    scroll_to_end(*menu);
    REQUIRE(menu->scroll_offset() == max_scroll);
}

TEST_CASE("ContextMenu last row of a capped menu is reachable and selects",
          "[view][context-menu]") {
    // The user-visible bug: with the panel overflowing, the trailing rows sat
    // outside the overlay and no click could ever reach them.
    auto root = make_short_root(kShortRoot);
    std::optional<int> got;
    bool fired = false;
    auto* menu = ContextMenu::show(root.get(), {kAnchorX, kAnchorY}, numbered_items(kOverflowRows),
                                   [&](std::optional<int> r) {
                                       got = r;
                                       fired = true;
                                   });

    const int last = kOverflowRows - 1;
    REQUIRE(menu->layout().max_scroll > 0.0f); // control

    scroll_to_end(*menu);

    const auto lay = menu->layout();
    const Rect& row = lay.rows[static_cast<size_t>(last)];
    // Having scrolled to the end, the final row must lie inside the panel.
    REQUIRE(row.y >= lay.box.y);
    REQUIRE(row.y + row.height <= lay.box.y + lay.box.height);

    menu->on_mouse_event(down_at(kInsideX, row.y + row.height * 0.5f));
    REQUIRE(fired);
    REQUIRE(got == kOverflowRows); // ids are 1-based
}

TEST_CASE("ContextMenu End key scrolls the last row into view", "[view][context-menu]") {
    auto root = make_short_root(kShortRoot);
    auto* menu =
        ContextMenu::show(root.get(), {kAnchorX, kAnchorY}, numbered_items(kOverflowRows), {});
    REQUIRE(menu->layout().max_scroll > 0.0f); // control
    REQUIRE(menu->scroll_offset() == 0.0f);

    menu->on_key_event(key_down(KeyCode::end_));

    const auto lay = menu->layout();
    REQUIRE(menu->hovered_index() == kOverflowRows - 1);
    const Rect& row = lay.rows[static_cast<size_t>(kOverflowRows - 1)];
    REQUIRE(row.y >= lay.box.y);
    REQUIRE(row.y + row.height <= lay.box.y + lay.box.height);

    // Home brings it back to the top.
    menu->on_key_event(key_down(KeyCode::home));
    REQUIRE(menu->scroll_offset() == 0.0f);
}

TEST_CASE("ContextMenu clips its rows to the panel", "[view][context-menu]") {
    auto root = make_short_root(kShortRoot);
    auto* menu =
        ContextMenu::show(root.get(), {kAnchorX, kAnchorY}, numbered_items(kOverflowRows), {});
    const auto lay = menu->layout();

    // Control that the clip is load-bearing: at least one row really does
    // fall outside the panel, so without a clip it would paint over the app.
    bool any_row_outside = false;
    for (const auto& r : lay.rows) {
        if (r.height > 0.0f && (r.y < lay.box.y || r.y + r.height > lay.box.y + lay.box.height)) {
            any_row_outside = true;
            break;
        }
    }
    REQUIRE(any_row_outside);

    RecordingCanvas canvas;
    menu->paint(canvas);

    bool clipped_to_panel = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type != DrawCommand::Type::clip_rect)
            continue;
        if (cmd.f[0] == lay.box.x && cmd.f[1] == lay.box.y && cmd.f[2] == lay.box.width &&
            cmd.f[3] == lay.box.height) {
            clipped_to_panel = true;
            break;
        }
    }
    REQUIRE(clipped_to_panel);
}
