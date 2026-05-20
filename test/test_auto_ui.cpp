#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/auto_ui.hpp>
#include <pulp/canvas/canvas.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

namespace {

ParamInfo make_param(ParamID id, std::string name, std::string unit, ParamRange range) {
    ParamInfo info;
    info.id = id;
    info.name = std::move(name);
    info.unit = std::move(unit);
    info.range = range;
    return info;
}

template <typename Widget>
Widget* find_widget(View& view, std::string_view id) {
    if (view.id() == id) {
        if (auto* widget = dynamic_cast<Widget*>(&view)) return widget;
    }
    for (size_t i = 0; i < view.child_count(); ++i) {
        if (auto* widget = find_widget<Widget>(*view.child_at(i), id)) return widget;
    }
    return nullptr;
}

bool paints_text(View& view, std::string_view text) {
    pulp::canvas::RecordingCanvas canvas;
    view.paint(canvas);
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_text &&
            cmd.text == text) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("AutoUi builds from parameter store", "[view][auto_ui]") {
    StateStore store;
    store.add_parameter(make_param(1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}));
    store.add_parameter(make_param(2, "Mix", "%", {0.0f, 100.0f, 100.0f}));
    store.add_parameter(make_param(3, "Bypass", "", {0.0f, 1.0f, 0.0f, 1.0f}));

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

TEST_CASE("AutoUi builds empty parameter grids", "[view][auto_ui][coverage]") {
    StateStore store;
    auto root = AutoUi::build(store);
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 2);

    auto* body = root->child_at(1);
    REQUIRE(body->child_count() == 1);
    auto* grid = body->child_at(0);
    REQUIRE(grid->child_count() == 0);
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
    store.add_parameter(make_param(1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}));

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

TEST_CASE("AutoUi generated controls expose toggle state and formatted values",
          "[view][auto_ui][coverage][issue-493]") {
    StateStore store;
    store.add_parameter(make_param(1, "Frequency", "Hz", {0.0f, 1000.0f, 500.0f}));
    store.add_parameter(make_param(2, "Drive", "dB", {0.0f, 80.0f, 50.0f}));
    store.add_parameter(make_param(3, "Fine", "", {0.0f, 8.0f, 5.0f}));
    store.add_parameter(make_param(4, "Bypass", "", {0.0f, 1.0f, 1.0f, 1.0f}));

    auto root = AutoUi::build(store);
    REQUIRE(root != nullptr);

    auto* frequency = find_widget<Knob>(*root, "Frequency");
    auto* drive = find_widget<Knob>(*root, "Drive");
    auto* fine = find_widget<Knob>(*root, "Fine");
    auto* bypass = find_widget<Toggle>(*root, "Bypass");

    REQUIRE(frequency != nullptr);
    REQUIRE(drive != nullptr);
    REQUIRE(fine != nullptr);
    REQUIRE(bypass != nullptr);
    REQUIRE(bypass->is_on());
    REQUIRE(bypass->label() == "Bypass");

    frequency->set_bounds({0, 0, 80, 80});
    drive->set_bounds({0, 0, 80, 80});
    fine->set_bounds({0, 0, 80, 80});

    REQUIRE(paints_text(*frequency, "500 Hz"));
    REQUIRE(paints_text(*drive, "50.0 dB"));
    REQUIRE(paints_text(*fine, "5.00"));
}

TEST_CASE("AutoUi sync updates generated toggles and existing faders",
          "[view][auto_ui][coverage][issue-493]") {
    StateStore store;
    store.add_parameter(make_param(1, "Bypass", "", {0.0f, 1.0f, 0.0f, 1.0f}));
    store.add_parameter(make_param(2, "Level", "", {0.0f, 1.0f, 0.0f}));

    auto root = AutoUi::build(store);
    REQUIRE(root != nullptr);

    auto* bypass = find_widget<Toggle>(*root, "Bypass");
    REQUIRE(bypass != nullptr);
    REQUIRE_FALSE(bypass->is_on());

    auto fader = std::make_unique<Fader>();
    auto* fader_ptr = fader.get();
    fader->set_id("Level");
    root->add_child(std::move(fader));

    store.set_normalized(1, 1.0f);
    store.set_normalized(2, 0.35f);
    AutoUi::sync(*root, store);

    REQUIRE(bypass->is_on());
    REQUIRE_THAT(fader_ptr->value(), WithinAbs(0.35f, 0.001f));

    store.set_normalized(1, 0.0f);
    AutoUi::sync(*root, store);
    REQUIRE_FALSE(bypass->is_on());
}

TEST_CASE("AutoUi sync ignores unmatched widget identifiers",
          "[view][auto_ui][coverage]") {
    StateStore store;
    store.add_parameter(make_param(1, "Level", "", {0.0f, 1.0f, 0.0f}));

    auto root = AutoUi::build(store);
    auto orphan = std::make_unique<Knob>();
    auto* orphan_ptr = orphan.get();
    orphan->set_id("Missing");
    orphan->set_value(0.25f);
    root->add_child(std::move(orphan));

    store.set_normalized(1, 0.75f);
    AutoUi::sync(*root, store);

    REQUIRE_THAT(orphan_ptr->value(), WithinAbs(0.25f, 0.001f));
}
