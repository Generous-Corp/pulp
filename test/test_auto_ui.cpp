#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/auto_ui.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/canvas/canvas.hpp>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

double motion_component(const pulp::view::motion::SampleEvent& event,
                        std::string_view name) {
    for (const auto& [key, value] : event.components)
        if (key == name) return value;
    return std::numeric_limits<double>::quiet_NaN();
}

class AutoUiMotionCapture {
public:
    AutoUiMotionCapture() {
        auto& coordinator = pulp::view::motion::Coordinator::instance();
        coordinator.reset();
        coordinator.bind(clock);
        coordinator.set_tracing_enabled(true);
        coordinator.add_sink([this](const auto& event) {
            if (event.kind != pulp::view::motion::SampleEvent::Kind::TraceStarted)
                samples.push_back(event);
        });
    }

    ~AutoUiMotionCapture() {
        pulp::view::motion::Coordinator::instance().reset();
    }

    FrameClock clock;
    std::vector<pulp::view::motion::SampleEvent> samples;
};

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

    // Knob text belongs to dedicated rows below the dial instead of being
    // painted over its artwork.
    auto* gain_tile = grid->child_at(0);
    REQUIRE(gain_tile->child_count() == 3);
    auto* gain_knob = dynamic_cast<Knob*>(gain_tile->child_at(0));
    REQUIRE(gain_knob != nullptr);
    CHECK_FALSE(gain_knob->show_label());
    CHECK_FALSE(gain_knob->show_value());
    CHECK(gain_knob->get_value_string() == "0.00 dB");
    auto* gain_name = dynamic_cast<Label*>(gain_tile->child_at(1));
    auto* gain_value = dynamic_cast<Label*>(gain_tile->child_at(2));
    REQUIRE(gain_name != nullptr);
    REQUIRE(gain_value != nullptr);
    CHECK(gain_name->text() == "Gain");
    CHECK(gain_value->text() == "0.00 dB");
}

TEST_CASE("AutoUi renders knob names and values outside the dial",
          "[view][auto_ui][screenshot]") {
    StateStore store;
    store.add_parameter(make_param(1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}));
    store.add_parameter(make_param(2, "Mix", "%", {0.0f, 100.0f, 75.0f}));
    store.add_parameter(make_param(3, "Frequency", "Hz", {20.0f, 20000.0f, 440.0f}));
    store.add_parameter(make_param(4, "Resonance", "%", {0.0f, 100.0f, 35.0f}));

    auto root = AutoUi::build(store);
    REQUIRE(root != nullptr);

    constexpr uint32_t width = 400;
    constexpr uint32_t height = 300;
    root->set_bounds({0.0f, 0.0f, static_cast<float>(width),
                      static_cast<float>(height)});
    auto png = render_to_png(*root, width, height, 2.0f, ScreenshotBackend::skia);
    if (png.empty()) SKIP("Skia raster backend unavailable");
    REQUIRE(png.size() > 2000);

    // Optional durable artifact for local visual review. CI remains clean.
    if (const char* out = std::getenv("PULP_AUTO_UI_SCREENSHOT_OUT")) {
        REQUIRE(render_to_file(*root, width, height, out, 2.0f,
                               ScreenshotBackend::skia));
    }
}

TEST_CASE("AutoUi builds empty parameter grids", "[view][auto_ui]") {
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
    REQUIRE(tile_knob->flex().preferred_height == 112);
    REQUIRE(tile_knob->flex().flex_shrink == 0);  // tiles never shrink
    REQUIRE(tile_knob->child_count() == 3);
    REQUIRE(dynamic_cast<Knob*>(tile_knob->child_at(0)) != nullptr);
    REQUIRE(dynamic_cast<Label*>(tile_knob->child_at(1)) != nullptr);
    REQUIRE(dynamic_cast<Label*>(tile_knob->child_at(2)) != nullptr);

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

TEST_CASE("AutoUi scrolls large parameter sets instead of truncating them",
          "[view][auto_ui][scroll]") {
    StateStore store;
    for (std::uint32_t i = 0; i < 54; ++i) {
        store.add_parameter({i, "P" + std::to_string(i), "",
                             {0.0f, 1.0f, 0.5f}});
    }

    auto root = AutoUi::build(store);
    root->set_bounds({0, 0, 400, 300});
    root->layout_children();

    auto* body = dynamic_cast<ScrollView*>(root->child_at(1));
    REQUIRE(body != nullptr);
    REQUIRE(body->child_count() == 1);
    REQUIRE(body->child_at(0)->child_count() == 54);
    REQUIRE(find_widget<Knob>(*root, "P53") != nullptr);
    CHECK(body->content_size().height > body->local_bounds().height);
    CHECK(body->wants_wheel_scroll());

    AutoUiMotionCapture motion;
    auto trace = pulp::view::motion::Coordinator::instance()
        .trace("AutoUiBody", {60})
        .scroll_geometry(
            "scroll", *body,
            {pulp::view::motion::ScrollProperty::ContentOffsetY,
             pulp::view::motion::ScrollProperty::VisibleRectHeight,
             pulp::view::motion::ScrollProperty::ContentSizeHeight,
             pulp::view::motion::ScrollProperty::ScrollableMaxY})
        .attach();
    motion.clock.tick(1.0f / 60.0f);
    REQUIRE(motion.samples.size() == 1);
    CHECK(motion_component(motion.samples.front(), "contentOffsetY") == 0.0);
    CHECK(motion_component(motion.samples.front(), "contentSizeHeight") >
          motion_component(motion.samples.front(), "visibleRectHeight"));
    CHECK(motion_component(motion.samples.front(), "scrollableMaxY") > 0.0);

    body->scroll_by(0, 100, /*animate=*/false);
    CHECK(body->target_scroll_y() > 0.0f);
    motion.clock.tick(1.0f / 60.0f);
    bool traced_scrolled_offset = false;
    for (const auto& sample : motion.samples) {
        const double offset = motion_component(sample, "contentOffsetY");
        if (!std::isnan(offset) && offset >= 99.5) traced_scrolled_offset = true;
    }
    CHECK(traced_scrolled_offset);
}

TEST_CASE("AutoUi preferred_size opens large enough to fit every parameter",
          "[view][auto_ui][fit]") {
    // The default editor must OPEN at a size that shows all of its generated
    // knobs — the 7-knob synth in Logic clipped its top row because AutoUi
    // never reported a fitting size. At preferred_size the content must fit the
    // viewport (no scroll needed, so nothing is clipped).
    for (std::size_t n : {std::size_t{1}, std::size_t{4}, std::size_t{7},
                          std::size_t{16}, std::size_t{32}}) {
        StateStore store;
        for (std::size_t i = 1; i <= n; ++i) {
            store.add_parameter({static_cast<uint32_t>(i),
                                 "P" + std::to_string(i), "", {0.0f, 1.0f, 0.5f}});
        }

        const auto fit = AutoUi::preferred_size(store);
        INFO("n=" << n << " fit=" << fit.width << "x" << fit.height);
        CHECK(fit.width >= 320);
        CHECK(fit.height >= 240);

        auto root = AutoUi::build(store);
        root->set_bounds({0.0f, 0.0f, static_cast<float>(fit.width),
                          static_cast<float>(fit.height)});
        root->layout_children();

        auto* body = dynamic_cast<ScrollView*>(root->child_at(1));
        REQUIRE(body != nullptr);
        // Content fits the viewport at the fitting size: no wheel scroll, so no
        // clipped row.
        CHECK(body->content_size().height <= body->local_bounds().height + 1.0f);
        CHECK_FALSE(body->wants_wheel_scroll());
        // Every tile made it into the tree.
        CHECK(find_widget<Knob>(*root, "P" + std::to_string(n)) != nullptr);
    }
}

TEST_CASE("AutoUi preferred_size fits grouped parameters",
          "[view][auto_ui][fit][groups]") {
    StateStore store;
    store.add_group({1, "Oscillator", 0});
    store.add_group({2, "Filter", 0});
    for (std::uint32_t i = 0; i < 10; ++i) {
        store.add_parameter({.id = i,
                             .name = "G" + std::to_string(i),
                             .range = {0.0f, 1.0f, 0.5f},
                             .group_id = i < 6 ? 1 : 2});
    }

    const auto fit = AutoUi::preferred_size(store);
    auto root = AutoUi::build(store);
    root->set_bounds({0.0f, 0.0f, static_cast<float>(fit.width),
                      static_cast<float>(fit.height)});
    root->layout_children();

    auto* body = dynamic_cast<ScrollView*>(root->child_at(1));
    REQUIRE(body != nullptr);
    INFO("grouped fit=" << fit.width << "x" << fit.height);
    CHECK(body->content_size().height <= body->local_bounds().height + 1.0f);
    CHECK_FALSE(body->wants_wheel_scroll());
    CHECK(find_widget<Knob>(*root, "G9") != nullptr);
}

TEST_CASE("AutoUi keeps the first row reachable when content overflows",
          "[view][auto_ui][scroll]") {
    // A cramped window (smaller than any fitting size) must still let the user
    // reach the TOP row. `justify_content: center` on an overflowing scroll body
    // strands the first row above the scroll origin — the can't-scroll-to-top
    // bug. On overflow the body top-aligns instead.
    StateStore store;
    for (std::uint32_t i = 0; i < 40; ++i) {
        store.add_parameter({i, "P" + std::to_string(i), "", {0.0f, 1.0f, 0.5f}});
    }

    auto root = AutoUi::build(store);
    root->set_bounds({0.0f, 0.0f, 300.0f, 220.0f});
    root->layout_children();

    auto* body = dynamic_cast<ScrollView*>(root->child_at(1));
    REQUIRE(body != nullptr);
    REQUIRE(body->wants_wheel_scroll());
    // The overflowing grid top-aligns its wrapped rows instead of centering
    // them into negative (unreachable) offsets.
    auto* grid = body->child_at(0);
    CHECK(grid->flex().align_content == FlexAlign::start);
    // First tile sits at the scroll origin (y ~ 0), reachable at scroll_y == 0.
    CHECK(grid->child_at(0)->bounds().y >= -0.5f);
    CHECK(grid->child_at(0)->bounds().y < 1.0f);
}

TEST_CASE("AutoUi renders registered parameter groups as scrollable sections",
          "[view][auto_ui][groups][scroll]") {
    StateStore store;
    store.add_group({1, "Oscillator", 0});
    store.add_group({2, "Envelope", 0});
    for (std::uint32_t i = 0; i < 8; ++i) {
        store.add_parameter({.id = i,
                             .name = "G" + std::to_string(i),
                             .range = {0.0f, 1.0f, 0.5f},
                             .group_id = i < 4 ? 1 : 2});
    }

    auto root = AutoUi::build(store);
    root->set_bounds({0, 0, 320, 240});
    root->layout_children();

    auto* body = dynamic_cast<ScrollView*>(root->child_at(1));
    REQUIRE(body != nullptr);
    REQUIRE(body->child_count() == 1);
    auto* content = body->child_at(0);
    REQUIRE(content->child_count() == 2);
    auto* oscillator = dynamic_cast<GroupBox*>(content->child_at(0));
    auto* envelope = dynamic_cast<GroupBox*>(content->child_at(1));
    REQUIRE(oscillator != nullptr);
    REQUIRE(envelope != nullptr);
    CHECK(oscillator->title() == "Oscillator");
    CHECK(envelope->title() == "Envelope");
    CHECK(find_widget<Knob>(*root, "G0") != nullptr);
    CHECK(find_widget<Knob>(*root, "G7") != nullptr);
    CHECK(body->content_size().height > body->local_bounds().height);
    body->scroll_by(0, 100, /*animate=*/false);
    CHECK(body->target_scroll_y() > 0.0f);
}

TEST_CASE("AutoUi keeps parameters whose group is not registered",
          "[view][auto_ui][groups]") {
    StateStore store;
    store.add_group({1, "Registered", 0});
    store.add_parameter({.id = 1,
                         .name = "Registered param",
                         .range = {0.0f, 1.0f, 0.5f},
                         .group_id = 1});
    store.add_parameter({.id = 2,
                         .name = "Unknown group param",
                         .range = {0.0f, 1.0f, 0.5f},
                         .group_id = 999});
    store.add_parameter({.id = 3,
                         .name = "Ungrouped param",
                         .range = {0.0f, 1.0f, 0.5f}});

    auto root = AutoUi::build(store);
    root->set_bounds({0, 0, 400, 300});
    root->layout_children();

    auto* body = dynamic_cast<ScrollView*>(root->child_at(1));
    REQUIRE(body != nullptr);
    REQUIRE(body->child_count() == 1);
    auto* content = body->child_at(0);
    REQUIRE(content->child_count() == 2);

    auto* registered = dynamic_cast<GroupBox*>(content->child_at(0));
    auto* other = dynamic_cast<GroupBox*>(content->child_at(1));
    REQUIRE(registered != nullptr);
    REQUIRE(other != nullptr);
    CHECK(registered->title() == "Registered");
    CHECK(other->title() == "Other");
    CHECK(find_widget<Knob>(*root, "Registered param") != nullptr);
    CHECK(find_widget<Knob>(*root, "Unknown group param") != nullptr);
    CHECK(find_widget<Knob>(*root, "Ungrouped param") != nullptr);
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

TEST_CASE("AutoUi sync updates the padded value row", "[view][auto_ui]") {
    StateStore store;
    store.add_parameter(make_param(7, "Gain", "dB", {-60.0f, 12.0f, 0.0f}));
    auto root = AutoUi::build(store);
    auto* tile = root->child_at(1)->child_at(0)->child_at(0);
    REQUIRE(tile->child_count() == 3);
    auto* value = dynamic_cast<Label*>(tile->child_at(2));
    REQUIRE(value != nullptr);
    CHECK(value->text() == "0.00 dB");

    store.set_value(7, -24.0f);
    AutoUi::sync(*root, store);
    CHECK(value->text() == "-24.0 dB");
}

TEST_CASE("AutoUi parameter changes preserve settled control geometry",
          "[view][auto_ui][layout][regression]") {
    // Six controls exercise the first-drag failure: the scroll body learned its
    // viewport width during the first pass, but the parent did not consume the
    // resulting wrapped extent until a later value-label invalidation. The
    // visible result was a four-column surface jumping to three columns.
    StateStore store;
    for (std::uint32_t i = 0; i < 6; ++i) {
        store.add_parameter(make_param(
            i, "Control " + std::to_string(i), i == 1 ? "st" : "%",
            i == 1 ? ParamRange{-24.0f, 24.0f, 0.0f, 0.01f}
                   : ParamRange{0.0f, 100.0f, 50.0f, 0.1f}));
    }

    auto root = AutoUi::build(store);
    root->set_bounds({0, 0, 400, 300});
    root->layout_children();
    auto* grid = root->child_at(1)->child_at(0);
    REQUIRE(grid->child_count() == 6);

    const auto title_before = root->child_at(0)->bounds();
    const auto body_before = root->child_at(1)->bounds();
    const auto grid_before = grid->bounds();
    std::vector<Rect> before;
    for (std::size_t i = 0; i < grid->child_count(); ++i)
        before.push_back(grid->child_at(i)->bounds());

    for (const auto& param : store.all_params())
        store.set_normalized(param.id, param.id % 2 ? 0.01f : 0.99f);
    AutoUi::sync(*root, store);
    root->layout_children();

    CHECK(root->child_at(0)->bounds() == title_before);
    CHECK(root->child_at(1)->bounds() == body_before);
    CHECK(grid->bounds() == grid_before);
    for (std::size_t i = 0; i < grid->child_count(); ++i) {
        INFO("control " << i);
        CHECK(grid->child_at(i)->bounds() == before[i]);
    }

    // An actual host resize must settle in that same single public layout call,
    // then remain just as stable when another parameter label changes.
    root->set_bounds({0, 0, 320, 240});
    root->layout_children();
    const auto resized_title = root->child_at(0)->bounds();
    const auto resized_body = root->child_at(1)->bounds();
    const auto resized_grid = grid->bounds();
    std::vector<Rect> resized_controls;
    for (std::size_t i = 0; i < grid->child_count(); ++i)
        resized_controls.push_back(grid->child_at(i)->bounds());

    store.set_normalized(0, 0.5f);
    AutoUi::sync(*root, store);
    root->layout_children();

    CHECK(root->child_at(0)->bounds() == resized_title);
    CHECK(root->child_at(1)->bounds() == resized_body);
    CHECK(grid->bounds() == resized_grid);
    for (std::size_t i = 0; i < grid->child_count(); ++i) {
        INFO("resized control " << i);
        CHECK(grid->child_at(i)->bounds() == resized_controls[i]);
    }
}

TEST_CASE("AutoUi generated controls write changes back to the store",
          "[view][auto_ui][parameters]") {
    StateStore store;
    store.add_parameter(make_param(1, "Frequency", "Hz", {55.0f, 1760.0f, 440.0f}));
    store.add_parameter(make_param(2, "Bypass", "", {0.0f, 1.0f, 0.0f, 1.0f}));

    auto root = AutoUi::build(store);
    REQUIRE(root != nullptr);

    auto* frequency = find_widget<Knob>(*root, "Frequency");
    auto* bypass = find_widget<Toggle>(*root, "Bypass");
    REQUIRE(frequency != nullptr);
    REQUIRE(bypass != nullptr);

    frequency->on_change(0.75f);
    bypass->on_toggle(true);

    REQUIRE_THAT(store.get_normalized(1), WithinAbs(0.75f, 0.001f));
    REQUIRE_THAT(store.get_value(1),
                 WithinAbs(store.all_params()[0].range.denormalize(0.75f),
                           0.001f));
    REQUIRE_THAT(store.get_normalized(2), WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(store.get_value(2), WithinAbs(1.0f, 0.001f));
}

TEST_CASE("AutoUi generated controls expose toggle state and formatted values",
          "[view][auto_ui][issue-493]") {
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

    auto* frequency_value = find_widget<Label>(*root, "__auto_ui_value_1");
    auto* drive_value = find_widget<Label>(*root, "__auto_ui_value_2");
    auto* fine_value = find_widget<Label>(*root, "__auto_ui_value_3");
    REQUIRE(frequency_value != nullptr);
    REQUIRE(drive_value != nullptr);
    REQUIRE(fine_value != nullptr);
    CHECK(frequency_value->text() == "500 Hz");
    CHECK(drive_value->text() == "50.0 dB");
    CHECK(fine_value->text() == "5.00");

    frequency->set_bounds({0, 0, 64, 64});
    drive->set_bounds({0, 0, 64, 64});
    fine->set_bounds({0, 0, 64, 64});
    CHECK_FALSE(paints_text(*frequency, "500 Hz"));
    CHECK_FALSE(paints_text(*drive, "50.0 dB"));
    CHECK_FALSE(paints_text(*fine, "5.00"));
}

TEST_CASE("AutoUi sync updates generated toggles and existing faders",
          "[view][auto_ui][issue-493]") {
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
          "[view][auto_ui]") {
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
