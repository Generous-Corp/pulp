// test_stationary_hover.cpp — hover follows the content under a still pointer.
//
// Scrolling, an animation or a list reorder can slide a different view under a
// pointer that did not move. reevaluate_stationary_hover runs after the frame
// and delivers the enter, exit and positioned move that a jiggle of the mouse
// would have, only when the frame's damage covers the pointer.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/hover_cursor.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>

#include <memory>
#include <vector>

using namespace pulp::view;

namespace {

class Row : public View {
public:
    void on_mouse_enter() override { ++enters; }
    void on_mouse_leave() override { ++leaves; }
    void on_hover_move(Point p) override { last_local = p; ++moves; }
    int enters = 0, leaves = 0, moves = 0;
    Point last_local{-1, -1};
};

struct List {
    View root;
    ScrollView* scroll = nullptr;
    std::vector<Row*> rows;

    explicit List(int count = 10) {
        root.set_bounds({0, 0, 200, 100});
        auto sv = std::make_unique<ScrollView>();
        scroll = sv.get();
        scroll->set_bounds({0, 0, 200, 100});
        for (int i = 0; i < count; ++i) {
            auto row = std::make_unique<Row>();
            row->set_bounds({0, static_cast<float>(i * 20), 200, 20});
            rows.push_back(row.get());
            scroll->add_child(std::move(row));
        }
        scroll->set_content_size({200, static_cast<float>(count * 20)});
        root.add_child(std::move(sv));
    }
};

}  // namespace

TEST_CASE("a scrolled list moves hover to the row now under a still pointer",
          "[view][input][hover]") {
    List list;
    const Point pointer{50, 30};  // over row 1

    deliver_hover_move(list.root, pointer);
    REQUIRE(list.rows[1]->is_hovered());
    REQUIRE(list.rows[1]->enters == 1);

    // Nothing has moved: the frame costs no delivery.
    CHECK_FALSE(reevaluate_stationary_hover(list.root));
    CHECK(list.rows[1]->moves == 1);

    list.scroll->set_scroll(0, 40);  // rows 3 is now under y=30 (30+40=70)
    REQUIRE(reevaluate_stationary_hover(list.root));

    CHECK_FALSE(list.rows[1]->is_hovered());
    CHECK(list.rows[1]->leaves == 1);
    CHECK(list.rows[3]->is_hovered());
    CHECK(list.rows[3]->enters == 1);
    CHECK(list.rows[3]->last_local.y == 10.0f);

    // A second frame with nothing new does nothing.
    CHECK_FALSE(reevaluate_stationary_hover(list.root));
    CHECK(list.rows[3]->enters == 1);
}

TEST_CASE("the same row moving under the pointer gets a positioned move only",
          "[view][input][hover]") {
    List list;
    deliver_hover_move(list.root, {50, 30});
    REQUIRE(list.rows[1]->is_hovered());
    const int moves_before = list.rows[1]->moves;

    list.scroll->set_scroll(0, 5);  // row 1 still under y=30, but 5 px further in
    REQUIRE(reevaluate_stationary_hover(list.root));

    CHECK(list.rows[1]->enters == 1);
    CHECK(list.rows[1]->leaves == 0);
    CHECK(list.rows[1]->moves == moves_before + 1);
    CHECK(list.rows[1]->last_local.y == 15.0f);
}

TEST_CASE("damage that misses the pointer does no work", "[view][input][hover]") {
    List list;
    deliver_hover_move(list.root, {50, 30});
    list.scroll->set_scroll(0, 40);

    CHECK_FALSE(reevaluate_stationary_hover(
        list.root, HoverDamage::region({0, 60, 200, 40})));
    CHECK(list.rows[1]->is_hovered());  // stale: the frame never touched it

    CHECK(reevaluate_stationary_hover(
        list.root, HoverDamage::region({0, 20, 200, 20})));
    CHECK_FALSE(list.rows[1]->is_hovered());
    CHECK(list.rows[3]->is_hovered());
}

TEST_CASE("content that leaves the pointer clears hover", "[view][input][hover]") {
    List list(2);  // two rows, 40 px of content in a 100 px pane
    deliver_hover_move(list.root, {50, 30});
    REQUIRE(list.rows[1]->is_hovered());

    list.rows[1]->set_visible(false);
    REQUIRE(reevaluate_stationary_hover(list.root));
    CHECK(list.rows[1]->leaves == 1);
    CHECK_FALSE(list.rows[1]->is_hovered());
}

TEST_CASE("no hover is synthesized before a pointer exists, after it leaves, or mid-press",
          "[view][input][hover]") {
    List list;
    CHECK_FALSE(reevaluate_stationary_hover(list.root));

    deliver_hover_move(list.root, {50, 30});
    list.root.simulate_hover({-1, -1});  // pointer exit
    list.scroll->set_scroll(0, 40);
    CHECK_FALSE(reevaluate_stationary_hover(list.root));
    CHECK_FALSE(list.rows[3]->is_hovered());

    deliver_hover_move(list.root, {50, 30});
    REQUIRE(deliver_mouse_down(list.root, list.rows[3], {50, 30}, 0));
    list.scroll->set_scroll(0, 80);
    CHECK_FALSE(reevaluate_stationary_hover(list.root));
    deliver_mouse_up(list.root, list.rows[3], {50, 30}, 0, 1, MouseUpHost{});
    CHECK(reevaluate_stationary_hover(list.root));
}

TEST_CASE("the per-frame cursor poll also repairs hover", "[view][input][hover]") {
    List list;
    HoverCursorTracker tracker;
    deliver_hover_move(list.root, {50, 30});
    tracker.set_pointer({50, 30});
    tracker.poll(list.root);

    list.scroll->set_scroll(0, 40);
    tracker.poll(list.root);
    CHECK(list.rows[3]->is_hovered());
    CHECK_FALSE(list.rows[1]->is_hovered());
}
