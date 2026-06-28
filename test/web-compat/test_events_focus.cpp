// Focus event tests — validates focus/blur, focus traversal, and text input

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

using namespace pulp::test;
using namespace pulp::view;

TEST_CASE("Focus: on_focus_changed fires on gain", "[events][focus]") {
    struct FocusTracker : View {
        bool focused = false;
        void on_focus_changed(bool gained) override {
            View::on_focus_changed(gained);
            focused = gained;
        }
    };

    FocusTracker v;
    v.on_focus_changed(true);
    REQUIRE(v.focused);
    REQUIRE(v.has_focus());
}

TEST_CASE("Focus: on_focus_changed fires on blur", "[events][focus]") {
    struct FocusTracker : View {
        bool blur_fired = false;
        void on_focus_changed(bool gained) override {
            View::on_focus_changed(gained);
            if (!gained) blur_fired = true;
        }
    };

    FocusTracker v;
    v.on_focus_changed(true);
    v.on_focus_changed(false);
    REQUIRE(v.blur_fired);
    REQUIRE_FALSE(v.has_focus());
}

TEST_CASE("Focus: has_focus tracks state", "[events][focus]") {
    View v;
    REQUIRE_FALSE(v.has_focus());
    v.on_focus_changed(true);
    REQUIRE(v.has_focus());
    v.on_focus_changed(false);
    REQUIRE_FALSE(v.has_focus());
}

TEST_CASE("Focus: focus_next finds first focusable", "[events][focus]") {
    View root;
    root.set_bounds({0, 0, 400, 200});

    auto k1 = std::make_unique<Knob>();
    k1->set_bounds({10, 10, 48, 48});
    auto* k1p = k1.get();

    auto k2 = std::make_unique<Knob>();
    k2->set_bounds({70, 10, 48, 48});

    root.add_child(std::move(k1));
    root.add_child(std::move(k2));

    auto* focused = View::focus_next(root, nullptr);
    REQUIRE(focused == k1p);
}

TEST_CASE("Focus: focus_next advances to next widget", "[events][focus]") {
    View root;
    root.set_bounds({0, 0, 400, 200});

    auto k1 = std::make_unique<Knob>();
    k1->set_bounds({10, 10, 48, 48});
    auto* k1p = k1.get();
    auto k2 = std::make_unique<Knob>();
    k2->set_bounds({70, 10, 48, 48});
    auto* k2p = k2.get();

    root.add_child(std::move(k1));
    root.add_child(std::move(k2));

    auto* f1 = View::focus_next(root, nullptr);
    REQUIRE(f1 == k1p);
    auto* f2 = View::focus_next(root, f1);
    REQUIRE(f2 == k2p);
}

TEST_CASE("Focus: focus_prev goes backward", "[events][focus]") {
    View root;
    root.set_bounds({0, 0, 400, 200});

    auto k1 = std::make_unique<Knob>();
    k1->set_bounds({10, 10, 48, 48});
    auto* k1p = k1.get();
    auto k2 = std::make_unique<Knob>();
    k2->set_bounds({70, 10, 48, 48});
    auto* k2p = k2.get();

    root.add_child(std::move(k1));
    root.add_child(std::move(k2));

    auto* prev = View::focus_prev(root, k2p);
    REQUIRE(prev == k1p);
}

TEST_CASE("Focus: TextInputEvent has text field", "[events][focus]") {
    TextInputEvent te;
    te.text = "hello";
    REQUIRE(te.text == "hello");
}

TEST_CASE("Focus: on_text_input default does not crash", "[events][focus]") {
    View v;
    TextInputEvent te;
    te.text = "x";
    v.on_text_input(te); // Should not crash
    REQUIRE(te.text == "x");
    REQUIRE_FALSE(v.has_focus());
}

TEST_CASE("Focus: focus cycle wraps around", "[events][focus]") {
    View root;
    root.set_bounds({0, 0, 400, 200});

    auto k1 = std::make_unique<Knob>();
    auto* k1p = k1.get();
    auto k2 = std::make_unique<Knob>();
    auto* k2p = k2.get();

    root.add_child(std::move(k1));
    root.add_child(std::move(k2));

    // After last widget, focus_next should wrap to first
    auto* after_last = View::focus_next(root, k2p);
    REQUIRE(after_last == k1p);
}
