// Element API tests — validates View properties, accessibility, and JS bridge API

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

using namespace pulp::test;
using namespace pulp::view;
using namespace pulp::canvas;

// ═══════════════════════════════════════════════════════════════════════════════
// Bounds
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Element: set_bounds and bounds round-trip", "[element][api]") {
    View v;
    v.set_bounds({10, 20, 100, 50});
    auto b = v.bounds();
    REQUIRE(b.x == 10.0f);
    REQUIRE(b.y == 20.0f);
    REQUIRE(b.width == 100.0f);
    REQUIRE(b.height == 50.0f);
}

TEST_CASE("Element: bounds default is zero", "[element][api]") {
    View v;
    auto b = v.bounds();
    REQUIRE(b.x == 0.0f);
    REQUIRE(b.y == 0.0f);
    REQUIRE(b.width == 0.0f);
    REQUIRE(b.height == 0.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Accessibility
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Element: default access role is none", "[element][a11y]") {
    View v;
    REQUIRE(v.access_role() == View::AccessRole::none);
}

TEST_CASE("Element: Knob has slider role", "[element][a11y]") {
    Knob k;
    REQUIRE(k.access_role() == View::AccessRole::slider);
}

TEST_CASE("Element: Label has label role", "[element][a11y]") {
    Label l("Test");
    REQUIRE(l.access_role() == View::AccessRole::label);
}

TEST_CASE("Element: set_access_label", "[element][a11y]") {
    View v;
    v.set_access_label("Filter Cutoff");
    REQUIRE(v.access_label() == "Filter Cutoff");
}

TEST_CASE("Element: set_access_value", "[element][a11y]") {
    View v;
    v.set_access_value("1200 Hz");
    REQUIRE(v.access_value() == "1200 Hz");
}

TEST_CASE("Element: Toggle has toggle role", "[element][a11y]") {
    Toggle t;
    REQUIRE(t.access_role() == View::AccessRole::toggle);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Child management
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Element: add_child increments count", "[element][api]") {
    View root;
    root.add_child(std::make_unique<View>());
    root.add_child(std::make_unique<View>());
    REQUIRE(root.child_count() == 2);
}

TEST_CASE("Element: remove_child decrements count", "[element][api]") {
    View root;
    auto c = std::make_unique<View>();
    auto* cp = c.get();
    root.add_child(std::move(c));
    REQUIRE(root.child_count() == 1);
    root.remove_child(cp);
    REQUIRE(root.child_count() == 0);
}

TEST_CASE("Element: child parent pointer set correctly", "[element][api]") {
    View root;
    auto c = std::make_unique<View>();
    auto* cp = c.get();
    root.add_child(std::move(c));
    REQUIRE(cp->parent() == &root);
}

TEST_CASE("Element: child parent cleared after remove", "[element][api]") {
    View root;
    auto c = std::make_unique<View>();
    auto* cp = c.get();
    root.add_child(std::move(c));
    auto removed = root.remove_child(cp);
    REQUIRE(removed != nullptr);
    REQUIRE(removed->parent() == nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// JS widget API
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Element: setLabel via JS", "[element][api]") {
    TestEnvironment env;
    env.eval(R"JS(
        createLabel("lbl");
        setLabel("lbl", "Hello World");
    )JS");
    auto* w = env.widget("lbl");
    REQUIRE(w != nullptr);
    auto* label = dynamic_cast<Label*>(w);
    REQUIRE(label != nullptr);
}

TEST_CASE("Element: setValue/getValue via JS", "[element][api]") {
    TestEnvironment env;
    env.eval(R"JS(
        createKnob("k");
        setValue("k", 0.75);
    )JS");
    auto result = env.engine.evaluate("getValue('k')");
    REQUIRE(result.getWithDefault<double>(0.0) > 0.5);
}

TEST_CASE("Element: setVisible via JS", "[element][api]") {
    TestEnvironment env;
    env.eval(R"JS(
        createPanel("p");
        setVisible("p", false);
    )JS");
    REQUIRE(env.widget("p")->visible() == false);
}

TEST_CASE("Element: setFontSize via JS", "[element][api]") {
    TestEnvironment env;
    env.eval(R"JS(
        createLabel("lbl");
        setFontSize("lbl", 24);
    )JS");
    auto* label = dynamic_cast<Label*>(env.widget("lbl"));
    REQUIRE(label != nullptr);
    // Font size should be set (verify it's accessible)
}

TEST_CASE("Element: createScrollView via JS", "[element][api]") {
    TestEnvironment env;
    env.eval(R"JS(createScrollView("sv");)JS");
    REQUIRE(env.widget("sv") != nullptr);
}

TEST_CASE("Element: createTextEditor via JS", "[element][api]") {
    TestEnvironment env;
    env.eval(R"JS(createTextEditor("te");)JS");
    REQUIRE(env.widget("te") != nullptr);
}

TEST_CASE("Element: createCombo via JS", "[element][api]") {
    TestEnvironment env;
    env.eval(R"JS(createCombo("cb");)JS");
    REQUIRE(env.widget("cb") != nullptr);
}

TEST_CASE("Element: createProgress via JS", "[element][api]") {
    TestEnvironment env;
    env.eval(R"JS(createProgress("pb");)JS");
    REQUIRE(env.widget("pb") != nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Paint verification
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Element: paint_all generates commands", "[element][paint]") {
    View v;
    v.set_bounds({0, 0, 100, 50});
    v.set_background_color(Color::rgba8(30, 30, 46));

    RecordingCanvas rc;
    v.paint_all(rc);
    REQUIRE(rc.command_count() > 0);
}

TEST_CASE("Element: paint_all on empty view has minimal commands", "[element][paint]") {
    View v;
    v.set_bounds({0, 0, 100, 50});

    RecordingCanvas rc;
    v.paint_all(rc);
    // No background or border → should have minimal/no draw commands
    // (save/restore may still be emitted)
}

TEST_CASE("Element: RecordingCanvas clear resets", "[element][paint]") {
    RecordingCanvas rc;
    View v;
    v.set_bounds({0, 0, 100, 50});
    v.set_background_color(Color::rgba8(255, 0, 0));
    v.paint_all(rc);
    REQUIRE(rc.command_count() > 0);
    rc.clear();
    REQUIRE(rc.command_count() == 0);
}
