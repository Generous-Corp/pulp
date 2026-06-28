// Hover event tests — validates mouseenter/mouseleave behavior via set_hovered()

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

using namespace pulp::test;
using namespace pulp::view;

TEST_CASE("Hover: on_hover_enter fires via set_hovered", "[events][hover]") {
    View v;
    v.set_bounds({0, 0, 100, 100});
    bool entered = false;
    v.on_hover_enter = [&]() { entered = true; };

    v.set_hovered(true);
    REQUIRE(entered);
}

TEST_CASE("Hover: on_hover_leave fires via set_hovered", "[events][hover]") {
    View v;
    v.set_bounds({0, 0, 100, 100});
    bool left = false;
    v.on_hover_leave = [&]() { left = true; };

    v.set_hovered(true);
    v.set_hovered(false);
    REQUIRE(left);
}

TEST_CASE("Hover: enter then leave sequence", "[events][hover]") {
    View v;
    v.set_bounds({0, 0, 100, 100});

    int state = 0;
    v.on_hover_enter = [&]() { state = 1; };
    v.on_hover_leave = [&]() { state = 2; };

    v.set_hovered(true);
    REQUIRE(state == 1);
    v.set_hovered(false);
    REQUIRE(state == 2);
}

TEST_CASE("Hover: multiple enter/leave cycles", "[events][hover]") {
    View v;
    v.set_bounds({0, 0, 100, 100});

    int enter_count = 0, leave_count = 0;
    v.on_hover_enter = [&]() { enter_count++; };
    v.on_hover_leave = [&]() { leave_count++; };

    for (int i = 0; i < 5; ++i) {
        v.set_hovered(true);
        v.set_hovered(false);
    }
    REQUIRE(enter_count == 5);
    REQUIRE(leave_count == 5);
}

TEST_CASE("Hover: set_hovered(true) twice only fires once", "[events][hover]") {
    View v;
    int count = 0;
    v.on_hover_enter = [&]() { count++; };

    v.set_hovered(true);
    v.set_hovered(true); // Duplicate — should not fire again
    REQUIRE(count == 1);
}

TEST_CASE("Hover: JS registerHover dispatches events", "[events][hover]") {
    TestEnvironment env(200, 200);
    env.eval(R"JS(
        createPanel("box");
        setFlex("box", "width", 200);
        setFlex("box", "height", 200);
        registerHover("box");
        var entered = false;
        var left = false;
        on("box", "mouseenter", function() { entered = true; });
        on("box", "mouseleave", function() { left = true; });
    )JS");
    env.root.layout_children();

    auto* w = env.widget("box");
    REQUIRE(w != nullptr);

    w->set_hovered(true);
    REQUIRE(env.engine.evaluate("entered").getWithDefault<bool>(false) == true);

    w->set_hovered(false);
    REQUIRE(env.engine.evaluate("left").getWithDefault<bool>(false) == true);
}

TEST_CASE("Hover: no callback set does not crash", "[events][hover]") {
    View v;
    REQUIRE_FALSE(v.is_hovered());

    v.set_hovered(true);
    REQUIRE(v.is_hovered());

    v.set_hovered(false);
    REQUIRE_FALSE(v.is_hovered());
}

TEST_CASE("Hover: hit_test finds correct target for enter", "[events][hover]") {
    View root;
    root.set_bounds({0, 0, 300, 100});

    auto a = std::make_unique<View>();
    a->set_bounds({0, 0, 100, 100});
    auto* ap = a.get();
    auto b = std::make_unique<View>();
    b->set_bounds({100, 0, 100, 100});
    auto* bp = b.get();

    root.add_child(std::move(a));
    root.add_child(std::move(b));

    REQUIRE(root.hit_test({50, 50}) == ap);
    REQUIRE(root.hit_test({150, 50}) == bp);
}
