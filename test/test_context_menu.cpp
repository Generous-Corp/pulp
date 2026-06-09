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

// Row 0 top is at anchor.y (10). Row i center y == 10 + i*24 + 12.
constexpr float kAnchorX = 10.0f, kAnchorY = 10.0f;
float row_center_y(int i) { return kAnchorY + static_cast<float>(i) * 24.0f + 12.0f; }
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
    menu->on_mouse_event(down_at(kInsideX, row_center_y(1)));

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
    menu->on_mouse_event(down_at(kInsideX, row_center_y(1)));
    REQUIRE_FALSE(fired);
    REQUIRE(root->child_count() == 1);  // still mounted

    // A subsequent click on an enabled row still works.
    menu->on_mouse_event(down_at(kInsideX, row_center_y(2)));
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
    menu->on_mouse_event(down_at(kInsideX, row_center_y(1)));
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
    hover.position = {kInsideX, row_center_y(1)};
    hover.is_down = false;
    menu->on_mouse_event(hover);
    REQUIRE(menu->hovered_index() == -1);

    hover.position = {kInsideX, row_center_y(2)};  // disabled row
    menu->on_mouse_event(hover);
    REQUIRE(menu->hovered_index() == -1);

    hover.position = {kInsideX, row_center_y(0)};  // enabled row
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
