#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/auto_ui.hpp>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

TEST_CASE("AutoUi builds from parameter store", "[view][auto_ui]") {
    StateStore store;
    store.add_parameter({1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}});
    store.add_parameter({2, "Mix", "%", {0.0f, 100.0f, 100.0f}});
    store.add_parameter({3, "Bypass", "", {0.0f, 1.0f, 0.0f, 1.0f}}); // step=1 → toggle

    auto root = AutoUi::build(store);
    REQUIRE(root != nullptr);

    // pulp #97 — new shape: root has [title, body], body has [grid],
    // grid has one tile per param.
    REQUIRE(root->child_count() == 2);
    auto* body = root->child_at(1);
    REQUIRE(body->child_count() == 1);
    auto* grid = body->child_at(0);
    REQUIRE(grid->child_count() == 3);  // Gain + Mix + Bypass
}

// ── pulp #97 — layout invariants for the centered-wrapping-grid design ──
//
// These assertions encode the design contract the visual fix relies on
// at the Yoga-flex level. A regression that flips justify_content back
// to start, or removes flex_wrap, would surface here BEFORE the user
// sees a knob cluster stranded in the top-left of their editor.

TEST_CASE("AutoUi root: column layout with title + body",
          "[view][auto_ui][issue-97]") {
    StateStore store;
    store.add_parameter({1, "G", "dB", {-60.0f, 12.0f, 0.0f}});

    auto root = AutoUi::build(store);
    REQUIRE(root != nullptr);
    REQUIRE(root->flex().direction == FlexDirection::column);
    REQUIRE(root->child_count() == 2);

    // Child 0: title Label "Parameters"
    auto* title = dynamic_cast<Label*>(root->child_at(0));
    REQUIRE(title != nullptr);
    REQUIRE(title->text() == "Parameters");

    // Child 1: body (column, flex_grow=1, centered both axes).
    auto* body = root->child_at(1);
    REQUIRE(body != nullptr);
    REQUIRE(body->flex().direction == FlexDirection::column);
    REQUIRE(body->flex().flex_grow == 1.0f);
    REQUIRE(body->flex().justify_content == FlexJustify::center);
    REQUIRE(body->flex().align_items == FlexAlign::center);
}

TEST_CASE("AutoUi grid: wrapping flex row, centered all three axes",
          "[view][auto_ui][issue-97]") {
    StateStore store;
    store.add_parameter({1, "A", "", {0.0f, 1.0f, 0.5f}});

    auto root = AutoUi::build(store);
    REQUIRE(root != nullptr);
    auto* body = root->child_at(1);
    REQUIRE(body->child_count() == 1);
    auto* grid = body->child_at(0);
    REQUIRE(grid != nullptr);

    // The crux of the fix: row + wrap + center across all three axes
    // (justify_content for the main axis, align_items for the cross
    // axis within a single line, align_content for cross when wrapped).
    REQUIRE(grid->flex().direction == FlexDirection::row);
    REQUIRE(grid->flex().flex_wrap == FlexWrap::wrap);
    REQUIRE(grid->flex().justify_content == FlexJustify::center);
    REQUIRE(grid->flex().align_items == FlexAlign::center);
    REQUIRE(grid->flex().align_content == FlexAlign::center);
    // max_width caps the row on very wide editors so the cluster stays
    // dense rather than spreading edge-to-edge.
    REQUIRE(grid->flex().max_width > 0);
}

TEST_CASE("AutoUi tiles: fixed size, no shrink, knob/toggle inside",
          "[view][auto_ui][issue-97]") {
    StateStore store;
    store.add_parameter({1, "Knob",   "Hz", {20.0f, 20000.0f, 1000.0f}});
    store.add_parameter({2, "Switch", "",   {0.0f, 1.0f, 0.0f, 1.0f}});

    auto root = AutoUi::build(store);
    auto* grid = root->child_at(1)->child_at(0);
    REQUIRE(grid->child_count() == 2);

    // Tile 0 → Knob
    auto* tile_knob = grid->child_at(0);
    REQUIRE(tile_knob->flex().preferred_width == 82);
    REQUIRE(tile_knob->flex().preferred_height == 96);
    REQUIRE(tile_knob->flex().flex_shrink == 0);  // tiles never shrink
    REQUIRE(tile_knob->child_count() == 1);
    REQUIRE(dynamic_cast<Knob*>(tile_knob->child_at(0)) != nullptr);

    // Tile 1 → Toggle (Switch range [0,1] step 1 → toggle path)
    auto* tile_toggle = grid->child_at(1);
    REQUIRE(tile_toggle->child_count() == 1);
    REQUIRE(dynamic_cast<Toggle*>(tile_toggle->child_at(0)) != nullptr);
}

TEST_CASE("AutoUi scales from 1 param up to 16 without losing structure",
          "[view][auto_ui][issue-97]") {
    // Sanity at the extremes: 1, 4, 16 params all produce the same
    // root → body → grid → tiles shape. The wrap/center invariants
    // hold regardless of count, which is what makes the editor look
    // intentional whether the developer defined 1 knob or 16.
    for (size_t n : {1, 4, 16}) {
        StateStore store;
        for (size_t i = 1; i <= n; ++i) {
            store.add_parameter({static_cast<uint32_t>(i),
                                 "P" + std::to_string(i), "",
                                 {0.0f, 1.0f, 0.5f}});
        }

        auto root = AutoUi::build(store);
        REQUIRE(root != nullptr);
        REQUIRE(root->child_count() == 2);
        auto* grid = root->child_at(1)->child_at(0);
        REQUIRE(grid->child_count() == n);
        // Wrap + center invariants survive at every count.
        REQUIRE(grid->flex().flex_wrap == FlexWrap::wrap);
        REQUIRE(grid->flex().justify_content == FlexJustify::center);
        REQUIRE(grid->flex().align_content == FlexAlign::center);
    }
}

TEST_CASE("AutoUi sync updates widgets", "[view][auto_ui]") {
    StateStore store;
    store.add_parameter({1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}});

    auto root = AutoUi::build(store);
    REQUIRE(root != nullptr);

    // Change param value
    store.set_normalized(1, 0.8f);
    AutoUi::sync(*root, store);

    // Find the knob and check value
    std::function<Knob*(View&)> find_knob = [&](View& v) -> Knob* {
        if (auto* k = dynamic_cast<Knob*>(&v)) {
            if (k->id() == "Gain") return k;
        }
        for (size_t i = 0; i < v.child_count(); ++i) {
            if (auto* k = find_knob(*v.child_at(i))) return k;
        }
        return nullptr;
    };

    auto* knob = find_knob(*root);
    REQUIRE(knob != nullptr);
    REQUIRE_THAT(knob->value(), WithinAbs(0.8, 0.01));
}
