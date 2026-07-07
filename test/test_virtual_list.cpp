#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/view/accessibility_tree.hpp>
#include <pulp/view/color_picker.hpp>
#include <pulp/view/file_drop_zone.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/midi_keyboard.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/virtual_list.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace pulp::view;

namespace {

struct ProbeRow : View {
    explicit ProbeRow(int* destructs = nullptr) : destructs_(destructs) {}
    ~ProbeRow() override {
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

std::vector<const View*> row_identities(const VirtualList& list) {
    std::vector<const View*> ids;
    for (std::size_t i = 0; i < list.realized_row_count(); ++i)
        ids.push_back(list.realized_row_at_slot(i));
    std::sort(ids.begin(), ids.end());
    return ids;
}

} // namespace

TEST_CASE("VirtualList realizes only visible rows plus overscan",
          "[view][virtual-list]") {
    VirtualList list;
    list.set_bounds({0, 0, 320, 200});
    list.set_row_height(20);
    list.set_overscan(3);
    list.set_row_count(50000);

    REQUIRE(list.realized_row_count() == 16);
    REQUIRE(list.child_count() == 16);
    REQUIRE(list.content_height() == Catch::Approx(1000000.0f));
    REQUIRE(list.realized_row_count() < 50000);
}

TEST_CASE("VirtualList replays scroll_to_row after bounds and row count are ready",
          "[view][virtual-list]") {
    VirtualList list;
    list.set_row_height(20);
    list.scroll_to_row(40);
    REQUIRE(list.scroll_y() == Catch::Approx(0.0f));

    list.set_row_count(100);
    REQUIRE(list.scroll_y() == Catch::Approx(0.0f));

    list.set_bounds({0, 0, 320, 100});
    REQUIRE(list.scroll_y() == Catch::Approx(720.0f));
    const auto indices = list.realized_indices();
    REQUIRE_FALSE(indices.empty());
    REQUIRE(indices.front() <= 40);
    REQUIRE(indices.back() >= 40);
}

TEST_CASE("VirtualList keeps out-of-range scroll target pending while row count grows",
          "[view][virtual-list]") {
    VirtualList list;
    list.set_bounds({0, 0, 320, 100});
    list.set_row_height(20);
    list.set_row_count(10);

    list.scroll_to_row(40);
    REQUIRE(list.scroll_y() == Catch::Approx(0.0f));

    list.set_row_count(100);
    REQUIRE(list.scroll_y() == Catch::Approx(720.0f));
    const auto indices = list.realized_indices();
    REQUIRE_FALSE(indices.empty());
    REQUIRE(indices.front() <= 40);
    REQUIRE(indices.back() >= 40);
}

TEST_CASE("VirtualList recycles stable row identities while rebinding indices",
          "[view][virtual-list]") {
    VirtualList list;
    int destructs = 0;
    int creates = 0;
    std::map<const View*, std::size_t> bound;
    list.set_bounds({0, 0, 300, 100});
    list.set_row_height(10);
    list.set_overscan(2);
    list.set_row_factory([&](std::size_t) {
        ++creates;
        return std::make_unique<ProbeRow>(&destructs);
    });
    list.set_row_binder([&](View& row, std::size_t index) {
        bound[&row] = index;
    });
    list.set_row_count(50000);

    REQUIRE(creates == 14);
    auto before = row_identities(list);
    list.clear_dirty();
    list.set_scroll_y(1000);
    auto after = row_identities(list);
    REQUIRE(after == before);
    REQUIRE(creates == 14);
    REQUIRE(destructs == 0);
    REQUIRE(list.first_realized_index() == 98);

    auto indices = list.realized_indices();
    REQUIRE(indices.front() == 98);
    REQUIRE(indices.back() == 111);
    for (std::size_t i = 0; i < list.realized_row_count(); ++i) {
        auto* row = list.realized_row_at_slot(i);
        REQUIRE(row != nullptr);
        REQUIRE(bound[row] == *list.bound_index_for_slot(i));
    }
}

TEST_CASE("VirtualList realizes trailing partial rows for pixel scroll offsets",
          "[view][virtual-list]") {
    VirtualList list;
    list.set_bounds({0, 0, 300, 100});
    list.set_row_height(10);
    list.set_overscan(0);
    list.set_row_count(1000);

    REQUIRE(list.realized_row_count() == 10);
    list.set_scroll_y(5);

    const auto indices = list.realized_indices();
    REQUIRE(list.realized_row_count() == 11);
    REQUIRE(indices.front() == 0);
    REQUIRE(indices.back() == 10);
}

TEST_CASE("VirtualList row geometry survives parent Yoga layout",
          "[view][virtual-list]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 300, 120});

    auto list = std::make_unique<VirtualList>();
    auto* raw = list.get();
    list->flex().preferred_width = 300;
    list->flex().preferred_height = 100;
    list->set_row_height(10);
    list->set_overscan(0);
    list->set_row_count(100);
    root->add_child(std::move(list));

    root->layout_children();
    raw->set_scroll_y(30);
    const auto* row = raw->realized_row_at_slot(0);
    REQUIRE(row != nullptr);
    const auto before = row->bounds();

    root->layout_children();

    REQUIRE(row->position() == View::Position::absolute);
    REQUIRE(row->bounds().x == Catch::Approx(before.x));
    REQUIRE(row->bounds().y == Catch::Approx(before.y));
    REQUIRE(row->bounds().width == Catch::Approx(before.width));
    REQUIRE(row->bounds().height == Catch::Approx(before.height));
}

TEST_CASE("VirtualList owns recycled row layout during parent Yoga layout",
          "[view][virtual-list]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 300, 220});

    auto list = std::make_unique<VirtualList>();
    auto* raw = list.get();
    list->flex().preferred_width = 300;
    list->flex().preferred_height = 200;
    list->set_row_height(10);
    list->set_overscan(0);
    list->set_row_count(100);
    root->add_child(std::move(list));

    root->layout_children();
    REQUIRE(raw->realized_row_count() == 20);

    raw->flex().preferred_height = 50;
    root->layout_children();

    REQUIRE(raw->realized_row_count() == 5);
    const auto* row = raw->realized_row_at_slot(0);
    REQUIRE(row != nullptr);
    REQUIRE(row->bounds().height == Catch::Approx(10.0f));
}

TEST_CASE("VirtualList releases recycled row slots when the pool shrinks",
          "[view][virtual-list]") {
    VirtualList list;
    int releases = 0;
    list.set_bounds({0, 0, 300, 100});
    list.set_row_height(10);
    list.set_overscan(2);
    list.set_row_releaser([&](View&) { ++releases; });
    list.set_row_count(50000);

    REQUIRE(list.realized_row_count() == 14);
    list.set_row_count(0);
    REQUIRE(releases == 14);
    REQUIRE(list.realized_row_count() == 0);
    REQUIRE(list.child_count() == 0);
}

TEST_CASE("VirtualList releases realized rows when destroyed",
          "[view][virtual-list]") {
    int releases = 0;
    {
        auto list = std::make_unique<VirtualList>();
        list->set_bounds({0, 0, 300, 100});
        list->set_row_height(10);
        list->set_overscan(2);
        list->set_row_releaser([&](View&) { ++releases; });
        list->set_row_count(50000);
        REQUIRE(list->realized_row_count() == 14);
    }
    REQUIRE(releases == 14);
}

TEST_CASE("VirtualList row release tolerates reentrant layout",
          "[view][virtual-list]") {
    VirtualList list;
    int releases = 0;
    list.set_bounds({0, 0, 300, 100});
    list.set_row_height(10);
    list.set_overscan(2);
    list.set_row_releaser([&](View&) {
        ++releases;
        list.layout_children();
    });
    list.set_row_count(50000);
    REQUIRE(list.realized_row_count() == 14);

    list.set_row_count(0);
    REQUIRE(releases == 14);
    REQUIRE(list.realized_row_count() == 0);
    REQUIRE(list.child_count() == 0);
}

TEST_CASE("VirtualList handles pointer events routed through row hit testing",
          "[view][virtual-list]") {
    FocusGuard focus;
    VirtualList list;
    list.set_bounds({0, 0, 240, 100});
    list.set_row_height(10);
    list.set_row_count(100);

    REQUIRE_FALSE(static_cast<bool>(list.on_pointer_event));
    list.on_mouse_event(click_at(5, 25));
    REQUIRE(list.selection() == std::vector<std::size_t>{2});
    REQUIRE(list.focused_index() == 2);
    REQUIRE(View::focused_input_ == &list);
    REQUIRE(list.has_focus());
}

TEST_CASE("VirtualList participates in native wheel-scroll routing without pointer marker",
          "[view][virtual-list]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 300, 200});

    auto list = std::make_unique<VirtualList>();
    auto* raw = list.get();
    list->set_bounds({10, 10, 120, 50});
    list->set_row_height(10);
    list->set_row_count(100);
    root->add_child(std::move(list));

    REQUIRE_FALSE(static_cast<bool>(raw->on_pointer_event));
    REQUIRE(raw->wants_wheel_scroll());
    REQUIRE(find_wheel_scroll_view_at(*root, {20, 20}) == raw);

    raw->on_mouse_event(wheel(15));
    REQUIRE(raw->scroll_y() == Catch::Approx(15.0f));
}

TEST_CASE("VirtualList wheel lookup honors pointer event opt-outs",
          "[view][virtual-list]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 300, 200});

    auto list = std::make_unique<VirtualList>();
    auto* raw = list.get();
    list->set_bounds({10, 10, 120, 50});
    list->set_row_height(10);
    list->set_row_count(100);
    list->set_pointer_events(View::PointerEvents::box_none);
    root->add_child(std::move(list));

    REQUIRE(raw->wants_wheel_scroll());
    REQUIRE(find_wheel_scroll_view_at(*root, {20, 20}) == nullptr);

    raw->set_pointer_events(View::PointerEvents::none);
    REQUIRE(find_wheel_scroll_view_at(*root, {20, 20}) == nullptr);
}

TEST_CASE("VirtualList stops outer binding when a binder mutates the list",
          "[view][virtual-list]") {
    VirtualList list;
    bool mutated = false;
    list.set_bounds({0, 0, 240, 100});
    list.set_row_height(10);
    list.set_row_binder([&](View&, std::size_t index) {
        if (!mutated && index == 0) {
            mutated = true;
            list.set_row_count(0);
        }
    });

    list.set_row_count(100);
    REQUIRE(mutated);
    REQUIRE(list.row_count() == 0);
    REQUIRE(list.realized_row_count() == 0);
    REQUIRE(list.child_count() == 0);
}

TEST_CASE("VirtualList tolerates row binder destroying the list",
          "[view][virtual-list]") {
    auto list = std::make_unique<VirtualList>();
    auto* raw = list.get();
    raw->set_bounds({0, 0, 240, 100});
    raw->set_row_height(10);
    raw->set_row_binder([&](View&, std::size_t) {
        list.reset();
    });

    raw->set_row_count(100);
    REQUIRE(list == nullptr);
}

TEST_CASE("VirtualList tolerates row binder destroying the list during focus scroll",
          "[view][virtual-list]") {
    auto list = std::make_unique<VirtualList>();
    auto* raw = list.get();
    bool armed = false;
    raw->set_bounds({0, 0, 240, 50});
    raw->set_row_height(10);
    raw->set_overscan(0);
    raw->set_row_binder([&](View&, std::size_t index) {
        if (armed && index > 50) list.reset();
    });
    raw->set_row_count(100);
    armed = true;

    raw->on_key_event(key_down(KeyCode::end_));
    REQUIRE(list == nullptr);
}

TEST_CASE("VirtualList tolerates row binder destroying the list during pending scroll replay",
          "[view][virtual-list]") {
    auto list = std::make_unique<VirtualList>();
    auto* raw = list.get();
    raw->set_row_height(10);
    raw->set_overscan(0);
    raw->scroll_to_row(90);
    raw->set_bounds({0, 0, 240, 50});
    raw->set_row_binder([&](View&, std::size_t index) {
        if (index >= 90) list.reset();
    });

    raw->set_row_count(100);
    REQUIRE(list == nullptr);
}

TEST_CASE("VirtualList tolerates row releaser destroying the list during factory replacement",
          "[view][virtual-list]") {
    auto list = std::make_unique<VirtualList>();
    auto* raw = list.get();
    raw->set_bounds({0, 0, 240, 50});
    raw->set_row_height(10);
    raw->set_row_count(100);
    raw->set_row_releaser([&](View&) {
        list.reset();
    });

    raw->set_row_factory([](std::size_t) {
        return std::make_unique<View>();
    });

    REQUIRE(list == nullptr);
}

TEST_CASE("VirtualList tolerates row releaser destroying the list during pool shrink",
          "[view][virtual-list]") {
    auto list = std::make_unique<VirtualList>();
    auto* raw = list.get();
    raw->set_bounds({0, 0, 240, 50});
    raw->set_row_height(10);
    raw->set_row_count(100);
    raw->set_row_releaser([&](View&) {
        list.reset();
    });

    raw->set_row_count(0);

    REQUIRE(list == nullptr);
}

TEST_CASE("VirtualList tolerates row factory destroying the list",
          "[view][virtual-list]") {
    auto list = std::make_unique<VirtualList>();
    auto* raw = list.get();
    raw->set_bounds({0, 0, 240, 50});
    raw->set_row_height(10);
    raw->set_row_factory([&](std::size_t) {
        list.reset();
        return std::make_unique<View>();
    });

    raw->set_row_count(100);

    REQUIRE(list == nullptr);
}

TEST_CASE("VirtualList hit testing routes non-interactive rows to the list",
          "[view][virtual-list]") {
    VirtualList list;
    list.set_bounds({0, 0, 240, 100});
    list.set_row_height(10);
    list.set_row_count(100);

    REQUIRE(list.hit_test({5, 25}) == &list);
}

TEST_CASE("VirtualList hit testing respects box-none pointer events",
          "[view][virtual-list]") {
    VirtualList list;
    list.set_bounds({0, 0, 240, 100});
    list.set_row_height(10);
    list.set_pointer_events(View::PointerEvents::box_none);
    list.set_row_count(100);

    REQUIRE(list.hit_test({5, 25}) == nullptr);
}

TEST_CASE("VirtualList hit testing preserves interactive row children",
          "[view][virtual-list]") {
    VirtualList list;
    list.set_bounds({0, 0, 240, 100});
    list.set_row_height(10);
    list.set_row_factory([](std::size_t) {
        auto row = std::make_unique<View>();
        auto child = std::make_unique<View>();
        child->set_bounds({0, 0, 30, 10});
        child->on_click = [] {};
        row->add_child(std::move(child));
        return row;
    });
    list.set_row_count(100);

    auto* hit = list.hit_test({5, 5});
    REQUIRE(hit != nullptr);
    REQUIRE(hit != &list);
    REQUIRE(static_cast<bool>(hit->on_click));
}

TEST_CASE("VirtualList hit testing preserves mouse-only native row controls",
          "[view][virtual-list]") {
    VirtualList list;
    list.set_bounds({0, 0, 240, 100});
    list.set_row_height(20);
    list.set_row_factory([](std::size_t) {
        auto row = std::make_unique<View>();
        auto pad = std::make_unique<XYPad>();
        pad->set_bounds({0, 0, 80, 20});
        row->add_child(std::move(pad));
        return row;
    });
    list.set_row_count(10);

    auto* hit = list.hit_test({5, 5});

    REQUIRE(hit != &list);
    REQUIRE(dynamic_cast<XYPad*>(hit) != nullptr);
}

TEST_CASE("VirtualList hit testing preserves existing mouse-only native widgets",
          "[view][virtual-list]") {
    REQUIRE(ColorPicker().wants_mouse_input());
    REQUIRE(DualRangeSlider().wants_mouse_input());
    REQUIRE(FileDropZone().wants_mouse_input());
    REQUIRE(GroupBox().wants_mouse_input());
    REQUIRE(MidiKeyboard().wants_mouse_input());

    VirtualList list;
    list.set_bounds({0, 0, 240, 100});
    list.set_row_height(60);
    list.set_row_factory([](std::size_t) {
        auto row = std::make_unique<View>();
        auto slider = std::make_unique<DualRangeSlider>();
        slider->set_bounds({0, 0, 80, 20});
        row->add_child(std::move(slider));
        auto group = std::make_unique<GroupBox>();
        group->set_bounds({0, 20, 80, 20});
        row->add_child(std::move(group));
        auto drop = std::make_unique<FileDropZone>();
        drop->set_bounds({0, 40, 80, 20});
        row->add_child(std::move(drop));
        return row;
    });
    list.set_row_count(10);

    auto* slider_hit = list.hit_test({5, 5});
    auto* group_hit = list.hit_test({5, 25});
    auto* drop_hit = list.hit_test({5, 45});

    REQUIRE(slider_hit != &list);
    REQUIRE(group_hit != &list);
    REQUIRE(drop_hit != &list);
    REQUIRE(dynamic_cast<DualRangeSlider*>(slider_hit) != nullptr);
    REQUIRE(dynamic_cast<GroupBox*>(group_hit) != nullptr);
    REQUIRE(dynamic_cast<FileDropZone*>(drop_hit) != nullptr);
}

TEST_CASE("VirtualList hit testing routes hover-only row children to the list",
          "[view][virtual-list]") {
    FocusGuard focus;
    VirtualList list;
    list.set_bounds({0, 0, 240, 100});
    list.set_row_height(10);
    list.set_row_factory([](std::size_t) {
        auto row = std::make_unique<View>();
        auto child = std::make_unique<View>();
        child->set_bounds({0, 0, 30, 10});
        child->on_hover_enter = [] {};
        row->add_child(std::move(child));
        return row;
    });
    list.set_row_count(100);

    auto* hit = list.hit_test({5, 5});
    REQUIRE(hit != nullptr);
    REQUIRE(hit == &list);

    list.on_mouse_event(click_at(5, 5));
    REQUIRE(list.selection() == std::vector<std::size_t>{0});
    REQUIRE(View::focused_input_ == &list);
}

TEST_CASE("VirtualList box-none hit testing preserves interactive row children",
          "[view][virtual-list]") {
    VirtualList list;
    list.set_bounds({0, 0, 240, 100});
    list.set_row_height(10);
    list.set_pointer_events(View::PointerEvents::box_none);
    list.set_row_factory([](std::size_t) {
        auto row = std::make_unique<View>();
        auto child = std::make_unique<View>();
        child->set_bounds({0, 0, 30, 10});
        child->on_click = [] {};
        row->add_child(std::move(child));
        return row;
    });
    list.set_row_count(100);

    auto* hit = list.hit_test({5, 5});
    REQUIRE(hit != nullptr);
    REQUIRE(hit != &list);
    REQUIRE(static_cast<bool>(hit->on_click));
}

TEST_CASE("VirtualList selection callback may remove the list",
          "[view][virtual-list]") {
    auto list = std::make_unique<VirtualList>();
    auto* raw = list.get();
    raw->set_bounds({0, 0, 240, 100});
    raw->set_row_height(10);
    raw->set_row_count(100);
    raw->on_selection_changed([&](const std::vector<std::size_t>&) {
        list.reset();
    });

    raw->select_row(2);
    REQUIRE(list == nullptr);
}

TEST_CASE("VirtualList double-click activation stops if selection removes the list",
          "[view][virtual-list]") {
    auto list = std::make_unique<VirtualList>();
    auto* raw = list.get();
    raw->set_bounds({0, 0, 240, 100});
    raw->set_row_height(10);
    raw->set_row_count(100);
    bool activated = false;
    raw->on_selection_changed([&](const std::vector<std::size_t>&) {
        list.reset();
    });
    raw->on_row_activated([&](std::size_t) {
        activated = true;
    });

    raw->on_mouse_event(click_at(5, 25, 2));

    REQUIRE(list == nullptr);
    REQUIRE_FALSE(activated);
}

TEST_CASE("VirtualList replays selected row once row count arrives",
          "[view][virtual-list]") {
    VirtualList list;
    list.select_row(5);
    REQUIRE(list.selection().empty());

    list.set_row_count(10);

    REQUIRE(list.selection() == std::vector<std::size_t>{5});
    REQUIRE(list.focused_index() == 5);
}

TEST_CASE("VirtualList keeps out-of-range selected row pending while row count grows",
          "[view][virtual-list]") {
    VirtualList list;
    list.set_row_count(10);
    list.select_row(2);
    REQUIRE(list.selection() == std::vector<std::size_t>{2});

    list.select_row(50);
    REQUIRE(list.selection().empty());

    list.set_row_count(100);
    REQUIRE(list.selection() == std::vector<std::size_t>{50});
    REQUIRE(list.focused_index() == 50);
}

TEST_CASE("VirtualList keyboard activation may remove the list",
          "[view][virtual-list]") {
    auto list = std::make_unique<VirtualList>();
    auto* raw = list.get();
    raw->set_bounds({0, 0, 240, 100});
    raw->set_row_height(10);
    raw->set_row_count(100);
    raw->set_focused_index(2);
    raw->on_row_activated([&](std::size_t) {
        list.reset();
    });

    REQUIRE(raw->on_key_event(key_down(KeyCode::enter)));
    REQUIRE(list == nullptr);
}

TEST_CASE("VirtualList type-to-search callback may remove the list",
          "[view][virtual-list]") {
    auto list = std::make_unique<VirtualList>();
    auto* raw = list.get();
    raw->set_bounds({0, 0, 240, 100});
    raw->set_row_height(10);
    raw->set_row_count(100);
    raw->set_type_to_search([&](std::string_view, std::size_t) -> std::optional<std::size_t> {
        list.reset();
        return 4;
    });

    raw->on_text_input({"s"});

    REQUIRE(list == nullptr);
}

TEST_CASE("VirtualList first Down key selects the first row without an anchor",
          "[view][virtual-list]") {
    VirtualList list;
    list.set_bounds({0, 0, 240, 100});
    list.set_row_height(10);
    list.set_row_count(100);

    REQUIRE(list.selection().empty());
    REQUIRE_FALSE(list.focused_index().has_value());

    REQUIRE(list.on_key_event(key_down(KeyCode::down)));
    REQUIRE(list.focused_index() == 0);
    REQUIRE(list.selection() == std::vector<std::size_t>{0});

    REQUIRE(list.on_key_event(key_down(KeyCode::down)));
    REQUIRE(list.focused_index() == 1);
    REQUIRE(list.selection() == std::vector<std::size_t>{1});
}

TEST_CASE("VirtualList rebinds realized rows when selection changes",
          "[view][virtual-list]") {
    VirtualList list;
    int selected_binds = 0;
    list.set_bounds({0, 0, 240, 100});
    list.set_row_height(10);
    list.set_row_binder([&](View&, std::size_t index) {
        if (index == 2 && list.is_selected(index)) ++selected_binds;
    });
    list.set_row_count(100);

    REQUIRE(selected_binds == 0);
    list.select_row(2);
    REQUIRE(selected_binds > 0);
}

TEST_CASE("VirtualList layouts only realized row subtrees",
          "[view][virtual-list]") {
    VirtualList list;
    list.set_bounds({0, 0, 300, 120});
    list.set_row_height(12);
    list.set_overscan(1);
    list.set_row_factory([](std::size_t) {
        return std::make_unique<ProbeRow>();
    });
    list.set_row_count(50000);
    list.layout_children();

    int calls = 0;
    for (std::size_t i = 0; i < list.realized_row_count(); ++i) {
        auto* probe = dynamic_cast<ProbeRow*>(list.realized_row_at_slot(i));
        REQUIRE(probe != nullptr);
        calls += probe->layout_calls;
    }
    REQUIRE(calls == static_cast<int>(list.realized_row_count()));
    REQUIRE(calls == 12);
}

TEST_CASE("VirtualList selection and keyboard focus survive recycling",
          "[view][virtual-list]") {
    VirtualList list;
    list.set_bounds({0, 0, 240, 100});
    list.set_row_height(10);
    list.set_overscan(1);
    list.set_selection_mode(VirtualList::SelectionMode::multi);
    list.set_row_count(1000);

    list.on_mouse_event(click_at(5, 15)); // row 1
    REQUIRE(list.selection() == std::vector<std::size_t>{1});
    REQUIRE(list.focused_index() == 1);

    REQUIRE(list.on_key_event(key_down(KeyCode::down, kModShift)));
    REQUIRE(list.selection() == std::vector<std::size_t>({1, 2}));
    REQUIRE(list.focused_index() == 2);

    list.scroll_to_row(900);
    REQUIRE(list.is_selected(1));
    REQUIRE(list.is_selected(2));

    list.scroll_to_row(1);
    bool saw_checked = false;
    for (const auto& node : snapshot_accessibility_tree(list)) {
        if (node.label == "row 2 of 1000") {
            saw_checked = true;
            REQUIRE(node.checked == "true");
        }
    }
    REQUIRE(saw_checked);
}

TEST_CASE("VirtualList preserves row accessibility labels from the binder",
          "[view][virtual-list]") {
    VirtualList list;
    bool rich_labels = true;
    list.set_bounds({0, 0, 240, 40});
    list.set_row_height(10);
    list.set_overscan(0);
    list.set_row_binder([&](View& row, std::size_t index) {
        if (rich_labels) row.set_access_label("Sample " + std::to_string(index));
    });
    list.set_row_count(100);

    auto* row = list.realized_row_at_slot(0);
    REQUIRE(row != nullptr);
    REQUIRE(row->access_label() == "Sample 0");
    REQUIRE(row->access_value() == "row 1 of 100");

    list.set_scroll_y(50);
    REQUIRE(list.realized_row_at_slot(0) == row);
    REQUIRE(row->access_label() == "Sample 5");
    REQUIRE(row->access_value() == "row 6 of 100");

    rich_labels = false;
    list.refresh_rows();
    REQUIRE(row->access_label() == "row 6 of 100");
    REQUIRE(row->access_value() == "row 6 of 100");
}

TEST_CASE("VirtualList reports selection changes when row count truncates selection",
          "[view][virtual-list]") {
    VirtualList list;
    list.set_bounds({0, 0, 240, 100});
    list.set_row_height(10);
    list.set_selection_mode(VirtualList::SelectionMode::multi);
    list.set_row_count(100);

    std::vector<std::size_t> last_selection;
    int callback_count = 0;
    list.on_selection_changed([&](const std::vector<std::size_t>& selection) {
        last_selection = selection;
        ++callback_count;
    });

    list.select_row(90);
    REQUIRE(callback_count == 1);
    REQUIRE(last_selection == std::vector<std::size_t>{90});

    list.set_row_count(50);
    REQUIRE(callback_count == 2);
    REQUIRE(last_selection.empty());
    REQUIRE(list.selection().empty());
}

TEST_CASE("VirtualList activation and type-to-search use model indices",
          "[view][virtual-list]") {
    VirtualList list;
    list.set_bounds({0, 0, 240, 100});
    list.set_row_height(10);
    list.set_row_count(100);

    std::size_t activated = 9999;
    list.on_row_activated([&](std::size_t index) { activated = index; });
    list.on_mouse_event(click_at(5, 25, 2));
    REQUIRE(activated == 2);

    list.set_type_to_search([](std::string_view q, std::size_t) -> std::optional<std::size_t> {
        if (q == "s") return 42;
        return std::nullopt;
    });
    list.on_text_input({"s"});
    REQUIRE(list.focused_index() == 42);
    REQUIRE(list.selection() == std::vector<std::size_t>{42});
    REQUIRE(list.scroll_y() == Catch::Approx(330.0f));
}

TEST_CASE("VirtualList tracks newly exposed dirty strip on scroll",
          "[view][virtual-list]") {
    VirtualList list;
    list.set_bounds({0, 0, 300, 100});
    list.set_row_height(10);
    list.set_row_count(1000);
    list.clear_dirty();

    list.on_mouse_event(wheel(15));
    REQUIRE(list.scroll_y() == Catch::Approx(15.0f));
    REQUIRE(list.dirty_tracker().is_dirty());
    REQUIRE_FALSE(list.dirty_tracker().needs_full_repaint());
    auto bounds = list.dirty_tracker().bounds();
    REQUIRE(bounds.x == Catch::Approx(0.0f));
    REQUIRE(bounds.y == Catch::Approx(85.0f));
    REQUIRE(bounds.w == Catch::Approx(300.0f));
    REQUIRE(bounds.h == Catch::Approx(15.0f));
}

TEST_CASE("VirtualList closes open combo boxes before scroll recycling",
          "[view][virtual-list]") {
    VirtualList list;
    list.set_bounds({0, 0, 240, 60});
    list.set_row_height(20);
    list.set_overscan(0);
    list.set_row_factory([](std::size_t) {
        auto row = std::make_unique<View>();
        auto combo = std::make_unique<ComboBox>();
        combo->set_items({"A", "B", "C"});
        combo->set_bounds({0, 0, 80, 18});
        row->add_child(std::move(combo));
        return row;
    });
    list.set_row_count(100);

    auto* row = list.realized_row_at_slot(0);
    REQUIRE(row != nullptr);
    auto* combo = dynamic_cast<ComboBox*>(row->child_at(0));
    REQUIRE(combo != nullptr);

    combo->on_mouse_event(click_at(2, 2));
    REQUIRE(combo->is_open());

    list.set_scroll_y(80);
    REQUIRE_FALSE(combo->is_open());
}
