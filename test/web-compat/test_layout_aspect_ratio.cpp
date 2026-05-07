// pulp #1434 batch 5 — aspect-ratio support for Yoga layout.
//
// Validates that:
//   - FlexStyle::aspect_ratio (std::optional<float>) propagates through to
//     Yoga via YGNodeStyleSetAspectRatio.
//   - When set, Yoga sizes the cross axis from the constrained axis:
//     `width:100, aspectRatio:1.5` -> height = 100/1.5 ≈ 66.67.
//   - The bridge `setFlex(id, "aspect_ratio", v)` path mutates the same
//     slot and triggers re-layout.
//   - The CSS shim parses 3 value forms (number, w/h ratio, "auto").
//   - Non-positive / non-finite values clear the slot (matches CSS
//     `aspect-ratio: auto`).
//
// All tests carry the [issue-1434-batch-5] tag for the umbrella issue.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"

using namespace pulp::test;
using namespace pulp::view;
using Catch::Matchers::WithinAbs;

// ═══════════════════════════════════════════════════════════════════════════════
// FlexStyle field defaults
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("FlexStyle: aspect_ratio defaults to unset",
          "[layout][flex][aspect-ratio][issue-1434-batch-5]") {
    FlexStyle f;
    REQUIRE_FALSE(f.aspect_ratio.has_value());
}

TEST_CASE("FlexStyle: aspect_ratio is settable to a finite positive value",
          "[layout][flex][aspect-ratio][issue-1434-batch-5]") {
    FlexStyle f;
    f.aspect_ratio = 1.5f;
    REQUIRE(f.aspect_ratio.has_value());
    REQUIRE_THAT(*f.aspect_ratio, WithinAbs(1.5f, 1e-6f));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Yoga dispatch: width pinned, height derived
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("aspect-ratio: width 100 + ratio 1.5 -> height ~= 66.67",
          "[layout][flex][aspect-ratio][issue-1434-batch-5]") {
    View root;
    root.set_bounds({0, 0, 400, 400});
    root.flex().direction = FlexDirection::row;

    auto child = std::make_unique<View>();
    child->flex().preferred_width = 100.0f;
    child->flex().aspect_ratio = 1.5f;
    auto* raw = child.get();
    root.add_child(std::move(child));
    root.layout_children();

    REQUIRE_THAT(raw->bounds().width,  WithinAbs(100.0f, 1.0f));
    REQUIRE_THAT(raw->bounds().height, WithinAbs(66.6667f, 0.5f));
}

TEST_CASE("aspect-ratio: width 200 + ratio 1.5 -> height ~= 133.33",
          "[layout][flex][aspect-ratio][issue-1434-batch-5]") {
    View root;
    root.set_bounds({0, 0, 400, 400});
    root.flex().direction = FlexDirection::row;

    auto child = std::make_unique<View>();
    child->flex().preferred_width = 200.0f;
    child->flex().aspect_ratio = 1.5f;
    auto* raw = child.get();
    root.add_child(std::move(child));
    root.layout_children();

    REQUIRE_THAT(raw->bounds().width,  WithinAbs(200.0f, 1.0f));
    REQUIRE_THAT(raw->bounds().height, WithinAbs(133.3333f, 0.5f));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Yoga dispatch: cross-axis (height pinned, width derived)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("aspect-ratio: height 100 + ratio 0.5 -> width = 50 (cross-axis)",
          "[layout][flex][aspect-ratio][issue-1434-batch-5]") {
    View root;
    root.set_bounds({0, 0, 400, 400});
    root.flex().direction = FlexDirection::row;
    // Avoid stretch on cross axis from clamping the child to root height.
    root.flex().align_items = FlexAlign::start;

    auto child = std::make_unique<View>();
    child->flex().preferred_height = 100.0f;
    child->flex().aspect_ratio = 0.5f;
    auto* raw = child.get();
    root.add_child(std::move(child));
    root.layout_children();

    REQUIRE_THAT(raw->bounds().height, WithinAbs(100.0f, 1.0f));
    REQUIRE_THAT(raw->bounds().width,  WithinAbs(50.0f,  1.0f));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Bridge setter: setFlex(id, "aspect_ratio", value)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("setFlex aspect_ratio: positive value populates FlexStyle slot",
          "[layout][flex][aspect-ratio][bridge][issue-1434-batch-5]") {
    TestEnvironment env;
    env.eval(R"(
        createCol('box', '');
        setFlex('box', 'aspect_ratio', 1.5);
    )");
    auto* w = env.widget("box");
    REQUIRE(w != nullptr);
    REQUIRE(w->flex().aspect_ratio.has_value());
    REQUIRE_THAT(*w->flex().aspect_ratio, WithinAbs(1.5f, 1e-6f));
}

TEST_CASE("setFlex aspect_ratio: zero clears the slot",
          "[layout][flex][aspect-ratio][bridge][issue-1434-batch-5]") {
    TestEnvironment env;
    env.eval(R"(
        createCol('box', '');
        setFlex('box', 'aspect_ratio', 1.5);
        setFlex('box', 'aspect_ratio', 0);
    )");
    auto* w = env.widget("box");
    REQUIRE(w != nullptr);
    REQUIRE_FALSE(w->flex().aspect_ratio.has_value());
}

TEST_CASE("setFlex aspect_ratio: negative value clears the slot",
          "[layout][flex][aspect-ratio][bridge][issue-1434-batch-5]") {
    TestEnvironment env;
    env.eval(R"(
        createCol('box', '');
        setFlex('box', 'aspect_ratio', 2.0);
        setFlex('box', 'aspect_ratio', -1);
    )");
    auto* w = env.widget("box");
    REQUIRE(w != nullptr);
    REQUIRE_FALSE(w->flex().aspect_ratio.has_value());
}

TEST_CASE("setFlex aspect_ratio: end-to-end layout through bridge + Yoga",
          "[layout][flex][aspect-ratio][bridge][issue-1434-batch-5]") {
    TestEnvironment env(400, 400);
    env.run(R"(
        createCol('box', '');
        setFlex('box', 'width', 120);
        setFlex('box', 'aspect_ratio', 2.0);
    )");
    auto* w = env.widget("box");
    REQUIRE(w != nullptr);
    REQUIRE_THAT(w->bounds().width,  WithinAbs(120.0f, 1.0f));
    // 120 / 2.0 = 60
    REQUIRE_THAT(w->bounds().height, WithinAbs(60.0f, 1.0f));
}

// ═══════════════════════════════════════════════════════════════════════════════
// CSS shim translator: web-compat-style-decl.js parses 3 value forms.
// Goes through the document.createElement / appendChild path so the JS shim
// (with its 3-form parser and "auto" handling) is exercised end-to-end.
// ═══════════════════════════════════════════════════════════════════════════════

// Helper: snapshot the native widget id (`_id`, distinct from the HTML
// `id` attribute) so the C++ side can look the widget up by the same key
// the JS shim used.
static std::string get_native_id(TestEnvironment& env, const std::string& js_var) {
    auto v = env.engine.evaluate(js_var + "._id");
    return std::string(v.getWithDefault<std::string_view>(""));
}

TEST_CASE("CSS aspect-ratio: plain number form via document.createElement",
          "[layout][flex][aspect-ratio][css][issue-1434-batch-5]") {
    TestEnvironment env;
    env.eval(R"(
        var arn = document.createElement('div');
        document.body.appendChild(arn);
        arn.style.aspectRatio = "1.5";
    )");
    auto id = get_native_id(env, "arn");
    REQUIRE_FALSE(id.empty());
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    REQUIRE(w->flex().aspect_ratio.has_value());
    REQUIRE_THAT(*w->flex().aspect_ratio, WithinAbs(1.5f, 1e-5f));
}

TEST_CASE("CSS aspect-ratio: width/height ratio form (16/9)",
          "[layout][flex][aspect-ratio][css][issue-1434-batch-5]") {
    TestEnvironment env;
    env.eval(R"(
        var arratio = document.createElement('div');
        document.body.appendChild(arratio);
        arratio.style.aspectRatio = "16/9";
    )");
    auto id = get_native_id(env, "arratio");
    REQUIRE_FALSE(id.empty());
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    REQUIRE(w->flex().aspect_ratio.has_value());
    // 16/9 ≈ 1.7778
    REQUIRE_THAT(*w->flex().aspect_ratio, WithinAbs(16.0f / 9.0f, 1e-4f));
}

TEST_CASE("CSS aspect-ratio: 'auto' clears the slot",
          "[layout][flex][aspect-ratio][css][issue-1434-batch-5]") {
    TestEnvironment env;
    env.eval(R"(
        var arauto = document.createElement('div');
        document.body.appendChild(arauto);
        arauto.style.aspectRatio = "1.5";
        arauto.style.aspectRatio = "auto";
    )");
    auto id = get_native_id(env, "arauto");
    REQUIRE_FALSE(id.empty());
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    REQUIRE_FALSE(w->flex().aspect_ratio.has_value());
}

TEST_CASE("CSS aspect-ratio: kebab-case via setProperty",
          "[layout][flex][aspect-ratio][css][issue-1434-batch-5]") {
    TestEnvironment env;
    env.eval(R"(
        var arkebab = document.createElement('div');
        document.body.appendChild(arkebab);
        arkebab.style.setProperty("aspect-ratio", "2");
    )");
    auto id = get_native_id(env, "arkebab");
    REQUIRE_FALSE(id.empty());
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    REQUIRE(w->flex().aspect_ratio.has_value());
    REQUIRE_THAT(*w->flex().aspect_ratio, WithinAbs(2.0f, 1e-6f));
}
