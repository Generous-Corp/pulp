#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/view/accessibility_tree.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/virtual_grid.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace pulp::view;

namespace {

struct ProbeCell : View {
    explicit ProbeCell(int* destructs = nullptr) : destructs_(destructs) {}
    ~ProbeCell() override {
        if (destructs_) ++*destructs_;
    }
    void layout_children() override { ++layout_calls; }
    int layout_calls = 0;
    int* destructs_ = nullptr;
};

struct FocusGuard {
    FocusGuard() { View::focused_input_ = nullptr; }
    ~FocusGuard() { View::focused_input_ = nullptr; }
};

MouseEvent click_at(float x, float y, int clicks = 1, std::uint16_t modifiers = 0) {
    MouseEvent ev;
    ev.position = {x, y};
    ev.is_down = true;
    ev.button = MouseButton::left;
    ev.click_count = clicks;
    ev.modifiers = modifiers;
    return ev;
}

MouseEvent wheel(float dy) {
    MouseEvent ev;
    ev.is_wheel = true;
    ev.scroll_delta_y = dy;
    return ev;
}

KeyEvent key_down(KeyCode key, std::uint16_t modifiers = 0) {
    KeyEvent ev;
    ev.key = key;
    ev.modifiers = modifiers;
    ev.is_down = true;
    return ev;
}

std::vector<const View*> cell_identities(const VirtualGrid& grid) {
    std::vector<const View*> ids;
    for (std::size_t i = 0; i < grid.realized_cell_count(); ++i)
        ids.push_back(grid.realized_cell_at_slot(i));
    std::sort(ids.begin(), ids.end());
    return ids;
}

} // namespace

TEST_CASE("VirtualGrid realizes only visible cells across columns plus overscan",
          "[view][virtual-grid]") {
    VirtualGrid grid;
    grid.set_bounds({0, 0, 320, 200});
    grid.set_cell_size(80, 40);   // 320 / 80 => 4 columns
    grid.set_overscan(2);
    grid.set_item_count(10000);

    REQUIRE(grid.columns() == 4);
    // visible_rows = ceil(200/40) = 5; pool_rows = 5 + 1 + 2*2 = 10; cells = 40
    REQUIRE(grid.realized_cell_count() == 40);
    REQUIRE(grid.child_count() == 40);
    REQUIRE(grid.total_rows() == 2500);
    REQUIRE(grid.content_height() == Catch::Approx(100000.0f));
    REQUIRE(grid.realized_cell_count() < 10000);
}

TEST_CASE("VirtualGrid derives column count from width when unset",
          "[view][virtual-grid]") {
    VirtualGrid grid;
    grid.set_cell_size(80, 40);
    grid.set_bounds({0, 0, 320, 200});
    grid.set_item_count(1000);
    REQUIRE(grid.columns() == 4);

    // Narrowing the viewport re-derives fewer columns and re-windows.
    grid.set_bounds({0, 0, 240, 200});
    REQUIRE(grid.columns() == 3);
    REQUIRE(grid.total_rows() == 334);
}

TEST_CASE("VirtualGrid honors an explicit column count over width",
          "[view][virtual-grid]") {
    VirtualGrid grid;
    grid.set_bounds({0, 0, 300, 100});
    grid.set_cell_size(100, 10);   // width would derive 3 columns
    grid.set_item_count(100);
    REQUIRE(grid.columns() == 3);
    REQUIRE(grid.content_height() == Catch::Approx(340.0f)); // ceil(100/3)=34 rows

    grid.set_column_count(5);
    REQUIRE(grid.columns() == 5);
    REQUIRE(grid.total_rows() == 20);
    REQUIRE(grid.content_height() == Catch::Approx(200.0f));

    grid.set_column_count(0);   // back to width-derived
    REQUIRE(grid.columns() == 3);
}

TEST_CASE("VirtualGrid recycles stable cell identities while rebinding row-major indices",
          "[view][virtual-grid]") {
    int destructs = 0;
    int creates = 0;
    std::map<const View*, std::size_t> bound;
    VirtualGrid grid;
    grid.set_bounds({0, 0, 300, 100});
    grid.set_cell_size(100, 10);   // 3 columns
    grid.set_overscan(2);
    grid.set_cell_factory([&](std::size_t) {
        ++creates;
        return std::make_unique<ProbeCell>(&destructs);
    });
    grid.set_cell_binder([&](View& cell, std::size_t index) {
        bound[&cell] = index;
    });
    grid.set_item_count(50000);

    // pool_rows = ceil(100/10) + 1 + 2*2 = 15; cells = 15 * 3 = 45
    REQUIRE(creates == 45);
    auto before = cell_identities(grid);
    grid.clear_dirty();

    grid.set_scroll_y(1000);   // visible_first_row = 100, overscan 2 => row 98
    auto after = cell_identities(grid);
    REQUIRE(after == before);
    REQUIRE(creates == 45);
    REQUIRE(destructs == 0);
    REQUIRE(grid.first_realized_index() == 98 * 3);

    auto indices = grid.realized_indices();
    REQUIRE(indices.front() == 98 * 3);
    REQUIRE(indices.back() == 98 * 3 + 44);
    for (std::size_t i = 0; i < grid.realized_cell_count(); ++i) {
        auto* cell = grid.realized_cell_at_slot(i);
        REQUIRE(cell != nullptr);
        // Row-major: slot i is bound to first_realized_index + i.
        REQUIRE(*grid.bound_index_for_slot(i) == grid.first_realized_index() + i);
        REQUIRE(bound[cell] == *grid.bound_index_for_slot(i));
    }
}

TEST_CASE("VirtualGrid positions cells in a 2D lattice",
          "[view][virtual-grid]") {
    VirtualGrid grid;
    grid.set_bounds({0, 0, 300, 100});
    grid.set_cell_size(100, 10);   // 3 columns
    grid.set_overscan(0);
    grid.set_item_count(100);

    // Index 4 => col 1, row 1 => x = 100, y = 10 (scroll 0).
    View* cell4 = nullptr;
    for (std::size_t i = 0; i < grid.realized_cell_count(); ++i) {
        if (*grid.bound_index_for_slot(i) == 4) cell4 = grid.realized_cell_at_slot(i);
    }
    REQUIRE(cell4 != nullptr);
    REQUIRE(cell4->bounds().x == Catch::Approx(100.0f));
    REQUIRE(cell4->bounds().y == Catch::Approx(10.0f));
    REQUIRE(cell4->bounds().width == Catch::Approx(100.0f));
    REQUIRE(cell4->bounds().height == Catch::Approx(10.0f));
}

TEST_CASE("VirtualGrid releases recycled cell slots when the pool shrinks",
          "[view][virtual-grid]") {
    VirtualGrid grid;
    int releases = 0;
    grid.set_bounds({0, 0, 300, 100});
    grid.set_cell_size(100, 10);   // 3 columns
    grid.set_overscan(2);
    grid.set_cell_releaser([&](View&) { ++releases; });
    grid.set_item_count(50000);

    REQUIRE(grid.realized_cell_count() == 45);
    grid.set_item_count(0);
    REQUIRE(releases == 45);
    REQUIRE(grid.realized_cell_count() == 0);
    REQUIRE(grid.child_count() == 0);
}

TEST_CASE("VirtualGrid releases realized cells when destroyed",
          "[view][virtual-grid]") {
    int releases = 0;
    {
        auto grid = std::make_unique<VirtualGrid>();
        grid->set_bounds({0, 0, 300, 100});
        grid->set_cell_size(100, 10);
        grid->set_overscan(2);
        grid->set_cell_releaser([&](View&) { ++releases; });
        grid->set_item_count(50000);
        REQUIRE(grid->realized_cell_count() == 45);
    }
    REQUIRE(releases == 45);
}

TEST_CASE("VirtualGrid cell release tolerates reentrant layout",
          "[view][virtual-grid]") {
    VirtualGrid grid;
    int releases = 0;
    grid.set_bounds({0, 0, 300, 100});
    grid.set_cell_size(100, 10);
    grid.set_overscan(2);
    grid.set_cell_releaser([&](View&) {
        ++releases;
        grid.layout_children();
    });
    grid.set_item_count(50000);
    REQUIRE(grid.realized_cell_count() == 45);

    grid.set_item_count(0);
    REQUIRE(releases == 45);
    REQUIRE(grid.realized_cell_count() == 0);
    REQUIRE(grid.child_count() == 0);
}

// --- Reentrancy / destruction discipline (mirrors test_virtual_list.cpp:393-475).
// These encode the hard-won alive-token / window-generation guards: a
// binder/releaser/factory that destroys the grid mid-callback must not corrupt
// the pool or dereference freed state.

TEST_CASE("VirtualGrid stops outer binding when a binder mutates the grid",
          "[view][virtual-grid]") {
    VirtualGrid grid;
    bool mutated = false;
    grid.set_bounds({0, 0, 240, 100});
    grid.set_cell_size(240, 10);
    grid.set_column_count(1);
    grid.set_cell_binder([&](View&, std::size_t index) {
        if (!mutated && index == 0) {
            mutated = true;
            grid.set_item_count(0);
        }
    });

    grid.set_item_count(100);
    REQUIRE(mutated);
    REQUIRE(grid.item_count() == 0);
    REQUIRE(grid.realized_cell_count() == 0);
    REQUIRE(grid.child_count() == 0);
}

TEST_CASE("VirtualGrid tolerates cell binder destroying the grid",
          "[view][virtual-grid]") {
    auto grid = std::make_unique<VirtualGrid>();
    auto* raw = grid.get();
    raw->set_bounds({0, 0, 240, 100});
    raw->set_cell_size(240, 10);
    raw->set_column_count(1);
    raw->set_cell_binder([&](View&, std::size_t) {
        grid.reset();
    });

    raw->set_item_count(100);
    REQUIRE(grid == nullptr);
}

TEST_CASE("VirtualGrid tolerates cell binder destroying the grid during focus scroll",
          "[view][virtual-grid]") {
    auto grid = std::make_unique<VirtualGrid>();
    auto* raw = grid.get();
    bool armed = false;
    raw->set_bounds({0, 0, 240, 50});
    raw->set_cell_size(240, 10);
    raw->set_column_count(1);
    raw->set_overscan(0);
    raw->set_cell_binder([&](View&, std::size_t index) {
        if (armed && index > 50) grid.reset();
    });
    raw->set_item_count(100);
    armed = true;

    raw->on_key_event(key_down(KeyCode::end_));
    REQUIRE(grid == nullptr);
}

TEST_CASE("VirtualGrid tolerates cell binder destroying the grid during pending scroll replay",
          "[view][virtual-grid]") {
    auto grid = std::make_unique<VirtualGrid>();
    auto* raw = grid.get();
    raw->set_cell_size(240, 10);
    raw->set_column_count(1);
    raw->set_overscan(0);
    raw->scroll_to_item(90);
    raw->set_bounds({0, 0, 240, 50});
    raw->set_cell_binder([&](View&, std::size_t index) {
        if (index >= 90) grid.reset();
    });

    raw->set_item_count(100);
    REQUIRE(grid == nullptr);
}

TEST_CASE("VirtualGrid tolerates cell releaser destroying the grid during factory replacement",
          "[view][virtual-grid]") {
    auto grid = std::make_unique<VirtualGrid>();
    auto* raw = grid.get();
    raw->set_bounds({0, 0, 240, 50});
    raw->set_cell_size(240, 10);
    raw->set_column_count(1);
    raw->set_item_count(100);
    raw->set_cell_releaser([&](View&) {
        grid.reset();
    });

    raw->set_cell_factory([](std::size_t) {
        return std::make_unique<View>();
    });

    REQUIRE(grid == nullptr);
}

TEST_CASE("VirtualGrid tolerates cell releaser destroying the grid during pool shrink",
          "[view][virtual-grid]") {
    auto grid = std::make_unique<VirtualGrid>();
    auto* raw = grid.get();
    raw->set_bounds({0, 0, 240, 50});
    raw->set_cell_size(240, 10);
    raw->set_column_count(1);
    raw->set_item_count(100);
    raw->set_cell_releaser([&](View&) {
        grid.reset();
    });

    raw->set_item_count(0);

    REQUIRE(grid == nullptr);
}

TEST_CASE("VirtualGrid tolerates cell factory destroying the grid",
          "[view][virtual-grid]") {
    auto grid = std::make_unique<VirtualGrid>();
    auto* raw = grid.get();
    raw->set_bounds({0, 0, 240, 50});
    raw->set_cell_size(240, 10);
    raw->set_column_count(1);
    raw->set_cell_factory([&](std::size_t) {
        grid.reset();
        return std::make_unique<View>();
    });

    raw->set_item_count(100);

    REQUIRE(grid == nullptr);
}

TEST_CASE("VirtualGrid selection callback may remove the grid",
          "[view][virtual-grid]") {
    auto grid = std::make_unique<VirtualGrid>();
    auto* raw = grid.get();
    raw->set_bounds({0, 0, 240, 100});
    raw->set_cell_size(80, 10);
    raw->set_item_count(100);
    raw->on_selection_changed([&](const std::vector<std::size_t>&) {
        grid.reset();
    });

    raw->select_cell(2);
    REQUIRE(grid == nullptr);
}

// --- Hit-testing / pointer routing.

TEST_CASE("VirtualGrid handles pointer events routed through 2D cell hit testing",
          "[view][virtual-grid]") {
    FocusGuard focus;
    VirtualGrid grid;
    grid.set_bounds({0, 0, 240, 100});
    grid.set_cell_size(80, 10);   // 3 columns
    grid.set_item_count(100);

    REQUIRE_FALSE(static_cast<bool>(grid.on_pointer_event));
    // Point (90, 25): col = floor(90/80)=1, row = floor(25/10)=2 => index 7.
    grid.on_mouse_event(click_at(90, 25));
    REQUIRE(grid.selection() == std::vector<std::size_t>{7});
    REQUIRE(grid.focused_index() == 7);
    REQUIRE(View::focused_input_ == &grid);
    REQUIRE(grid.has_focus());
}

TEST_CASE("VirtualGrid hit testing routes non-interactive cells to the grid",
          "[view][virtual-grid]") {
    VirtualGrid grid;
    grid.set_bounds({0, 0, 240, 100});
    grid.set_cell_size(80, 10);
    grid.set_item_count(100);

    REQUIRE(grid.hit_test({5, 25}) == &grid);
}

TEST_CASE("VirtualGrid hit testing respects box-none pointer events",
          "[view][virtual-grid]") {
    VirtualGrid grid;
    grid.set_bounds({0, 0, 240, 100});
    grid.set_cell_size(80, 10);
    grid.set_pointer_events(View::PointerEvents::box_none);
    grid.set_item_count(100);

    REQUIRE(grid.hit_test({5, 25}) == nullptr);
}

TEST_CASE("VirtualGrid hit testing preserves interactive cell children",
          "[view][virtual-grid]") {
    VirtualGrid grid;
    grid.set_bounds({0, 0, 240, 100});
    grid.set_cell_size(80, 10);
    grid.set_cell_factory([](std::size_t) {
        auto cell = std::make_unique<View>();
        auto child = std::make_unique<View>();
        child->set_bounds({0, 0, 30, 10});
        child->on_click = [] {};
        cell->add_child(std::move(child));
        return cell;
    });
    grid.set_item_count(100);

    auto* hit = grid.hit_test({5, 5});
    REQUIRE(hit != nullptr);
    REQUIRE(hit != &grid);
    REQUIRE(static_cast<bool>(hit->on_click));
}

// --- Wheel routing.

TEST_CASE("VirtualGrid participates in native wheel-scroll routing without pointer marker",
          "[view][virtual-grid]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 300, 200});

    auto grid = std::make_unique<VirtualGrid>();
    auto* raw = grid.get();
    grid->set_bounds({10, 10, 120, 50});
    grid->set_cell_size(40, 10);
    grid->set_item_count(100);
    root->add_child(std::move(grid));

    REQUIRE_FALSE(static_cast<bool>(raw->on_pointer_event));
    REQUIRE(raw->wants_wheel_scroll());
    REQUIRE(find_wheel_scroll_view_at(*root, {20, 20}) == raw);

    raw->on_mouse_event(wheel(15));
    REQUIRE(raw->scroll_y() == Catch::Approx(15.0f));
}

TEST_CASE("VirtualGrid tracks newly exposed dirty strip on scroll",
          "[view][virtual-grid]") {
    VirtualGrid grid;
    grid.set_bounds({0, 0, 300, 100});
    grid.set_cell_size(100, 10);
    grid.set_item_count(1000);
    grid.clear_dirty();

    grid.on_mouse_event(wheel(15));
    REQUIRE(grid.scroll_y() == Catch::Approx(15.0f));
    REQUIRE(grid.dirty_tracker().is_dirty());
    REQUIRE_FALSE(grid.dirty_tracker().needs_full_repaint());
    auto bounds = grid.dirty_tracker().bounds();
    REQUIRE(bounds.x == Catch::Approx(0.0f));
    REQUIRE(bounds.y == Catch::Approx(85.0f));
    REQUIRE(bounds.w == Catch::Approx(300.0f));
    REQUIRE(bounds.h == Catch::Approx(15.0f));
}

// --- 2D keyboard navigation.

TEST_CASE("VirtualGrid arrow keys move by 1 horizontally and by column count vertically",
          "[view][virtual-grid]") {
    VirtualGrid grid;
    grid.set_bounds({0, 0, 300, 100});
    grid.set_cell_size(100, 10);   // 3 columns
    grid.set_item_count(100);

    REQUIRE(grid.selection().empty());
    REQUIRE_FALSE(grid.focused_index().has_value());

    // First Down without an anchor selects index 0.
    REQUIRE(grid.on_key_event(key_down(KeyCode::down)));
    REQUIRE(grid.focused_index() == 0);
    REQUIRE(grid.selection() == std::vector<std::size_t>{0});

    // Right => +1.
    REQUIRE(grid.on_key_event(key_down(KeyCode::right)));
    REQUIRE(grid.focused_index() == 1);

    // Down => +columns (1 + 3 = 4).
    REQUIRE(grid.on_key_event(key_down(KeyCode::down)));
    REQUIRE(grid.focused_index() == 4);

    // Up => -columns (4 - 3 = 1).
    REQUIRE(grid.on_key_event(key_down(KeyCode::up)));
    REQUIRE(grid.focused_index() == 1);

    // Left => -1.
    REQUIRE(grid.on_key_event(key_down(KeyCode::left)));
    REQUIRE(grid.focused_index() == 0);

    // Up in the top row stays put (no wrap above the grid).
    REQUIRE(grid.on_key_event(key_down(KeyCode::up)));
    REQUIRE(grid.focused_index() == 0);

    // End jumps to the final item.
    REQUIRE(grid.on_key_event(key_down(KeyCode::end_)));
    REQUIRE(grid.focused_index() == 99);

    // Home returns to the first.
    REQUIRE(grid.on_key_event(key_down(KeyCode::home)));
    REQUIRE(grid.focused_index() == 0);
}

TEST_CASE("VirtualGrid keyboard activation may remove the grid",
          "[view][virtual-grid]") {
    auto grid = std::make_unique<VirtualGrid>();
    auto* raw = grid.get();
    raw->set_bounds({0, 0, 240, 100});
    raw->set_cell_size(80, 10);
    raw->set_item_count(100);
    raw->set_focused_index(2);
    raw->on_cell_activated([&](std::size_t) {
        grid.reset();
    });

    REQUIRE(raw->on_key_event(key_down(KeyCode::enter)));
    REQUIRE(grid == nullptr);
}

// --- Multi-select + accessibility survive recycling.

TEST_CASE("VirtualGrid selection and keyboard focus survive recycling",
          "[view][virtual-grid]") {
    VirtualGrid grid;
    grid.set_bounds({0, 0, 300, 100});
    grid.set_cell_size(100, 10);   // 3 columns
    grid.set_overscan(1);
    grid.set_selection_mode(VirtualGrid::SelectionMode::multi);
    grid.set_item_count(1000);

    // Click index 3 (col 0, row 1).
    grid.on_mouse_event(click_at(5, 15));
    REQUIRE(grid.selection() == std::vector<std::size_t>{3});
    REQUIRE(grid.focused_index() == 3);

    // Shift+Right extends by one column-step to index 4.
    REQUIRE(grid.on_key_event(key_down(KeyCode::right, kModShift)));
    REQUIRE(grid.selection() == std::vector<std::size_t>({3, 4}));
    REQUIRE(grid.focused_index() == 4);

    grid.scroll_to_item(900);
    REQUIRE(grid.is_selected(3));
    REQUIRE(grid.is_selected(4));

    grid.scroll_to_item(0);
    bool saw_checked = false;
    for (const auto& node : snapshot_accessibility_tree(grid)) {
        if (node.label == "cell 4 of 1000") {
            saw_checked = true;
            REQUIRE(node.checked == "true");
        }
    }
    REQUIRE(saw_checked);
}

TEST_CASE("VirtualGrid layouts only realized cell subtrees",
          "[view][virtual-grid]") {
    VirtualGrid grid;
    grid.set_bounds({0, 0, 300, 120});
    grid.set_cell_size(100, 12);   // 3 columns
    grid.set_overscan(1);
    grid.set_cell_factory([](std::size_t) {
        return std::make_unique<ProbeCell>();
    });
    grid.set_item_count(50000);
    grid.layout_children();

    int calls = 0;
    for (std::size_t i = 0; i < grid.realized_cell_count(); ++i) {
        auto* probe = dynamic_cast<ProbeCell*>(grid.realized_cell_at_slot(i));
        REQUIRE(probe != nullptr);
        calls += probe->layout_calls;
    }
    REQUIRE(calls == static_cast<int>(grid.realized_cell_count()));
    // pool_rows = ceil(120/12) + 1 + 2*1 = 13; cells = 13 * 3 = 39.
    REQUIRE(calls == 39);
}

TEST_CASE("VirtualGrid replays selected cell once item count arrives",
          "[view][virtual-grid]") {
    VirtualGrid grid;
    grid.select_cell(5);
    REQUIRE(grid.selection().empty());

    grid.set_item_count(10);

    REQUIRE(grid.selection() == std::vector<std::size_t>{5});
    REQUIRE(grid.focused_index() == 5);
}

TEST_CASE("VirtualGrid re-windows realized cells when the column count changes",
          "[view][virtual-grid]") {
    VirtualGrid grid;
    grid.set_bounds({0, 0, 300, 100});
    grid.set_cell_size(100, 10);
    grid.set_overscan(0);
    grid.set_item_count(100);
    REQUIRE(grid.columns() == 3);

    grid.set_scroll_y(90); // row 9 => index 27 top-left with 3 columns
    const auto before_first = grid.first_realized_index();
    REQUIRE(before_first % 3 == 0);

    grid.set_column_count(4);
    REQUIRE(grid.columns() == 4);
    // Window realigned to a full 4-column row boundary and stays in range.
    REQUIRE(grid.first_realized_index() % 4 == 0);
    const auto indices = grid.realized_indices();
    REQUIRE_FALSE(indices.empty());
    REQUIRE(indices.back() < grid.item_count());
    // Each realized cell now sits at a valid 4-column lattice position.
    for (std::size_t i = 0; i < grid.realized_cell_count(); ++i) {
        auto* cell = grid.realized_cell_at_slot(i);
        REQUIRE(cell != nullptr);
        const auto idx = *grid.bound_index_for_slot(i);
        REQUIRE(cell->bounds().x == Catch::Approx(static_cast<float>(idx % 4) * 100.0f));
    }
}
