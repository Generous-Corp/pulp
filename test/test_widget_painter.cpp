// Tests for pulp::view::WidgetPainter — the paint delegate.
//
// The seam lets a caller replace how a control renders without subclassing it.
// Three properties make it usable, and each is pinned below:
//
//   * A delegate installed on a view applies to that view AND its subtree, so a
//     panel can be skinned in one call. A descendant that installs its own wins
//     for its own subtree, and clearing falls back to the nearest ancestor.
//   * Every hook is non-pure and returns whether it painted, so a delegate can
//     restyle ONE control and let everything else fall through to the stock look
//     rather than having to reimplement the whole widget set.
//   * The paint state carries the units each shape actually reasons in: a rotary
//     gets a normalized proportion, a linear control gets PIXEL thumb positions.
//     Handing a rotary pixels (or a fader a proportion) is the kind of mistake
//     that renders plausibly and is wrong everywhere.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/scroll_bar.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_painter.hpp>
#include <pulp/view/widgets.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace pulp::view;

namespace {

/// Records which hooks it was asked for, and claims only the ones it was told to.
struct RecordingPainter : WidgetPainter {
    std::vector<std::string>* log = nullptr;
    bool claim_rotary = false;
    bool claim_button_bg = false;
    bool claim_button_text = false;

    // What the seam handed us, so a test can assert the UNITS are right.
    double seen_rotary_position = -1.0;
    float seen_thumb_pixels = -1.0f;

    bool paint_rotary(pulp::canvas::Canvas&, const RotaryPaintState& s,
                      View&) override {
        if (log) log->push_back("rotary");
        seen_rotary_position = s.position;
        return claim_rotary;
    }
    bool paint_linear(pulp::canvas::Canvas&, const LinearPaintState& s,
                      View&) override {
        if (log) log->push_back("linear");
        seen_thumb_pixels = s.thumb_pos;
        return false;
    }
    bool paint_button_background(pulp::canvas::Canvas&, const ButtonPaintState&,
                                 View&) override {
        if (log) log->push_back("button_bg");
        return claim_button_bg;
    }
    bool paint_button_text(pulp::canvas::Canvas&, const ButtonPaintState&,
                           View&) override {
        if (log) log->push_back("button_text");
        return claim_button_text;
    }
};

std::shared_ptr<RecordingPainter> make_painter(std::vector<std::string>* log = nullptr) {
    auto p = std::make_shared<RecordingPainter>();
    p->log = log;
    return p;
}

} // namespace

TEST_CASE("a view with no painter installed resolves to none", "[view][painter]") {
    View v;
    REQUIRE(v.effective_painter() == nullptr);
}

TEST_CASE("a painter installed on a view applies to that view", "[view][painter]") {
    View v;
    auto p = make_painter();
    v.set_painter(p);
    REQUIRE(v.effective_painter() == p.get());
}

TEST_CASE("a painter applies to the whole subtree and not just the view it was set on",
          "[view][painter]") {
    // This is what makes the seam usable: skin a panel in ONE call and every
    // control inside it follows, without touching any of them.
    auto root = std::make_unique<View>();
    auto* root_raw = root.get();

    auto mid = std::make_unique<View>();
    auto* mid_raw = mid.get();
    auto leaf = std::make_unique<View>();
    auto* leaf_raw = leaf.get();

    mid_raw->add_child(std::move(leaf));
    root_raw->add_child(std::move(mid));

    auto p = make_painter();
    root_raw->set_painter(p);

    // Resolved through two levels of parent.
    REQUIRE(root_raw->effective_painter() == p.get());
    REQUIRE(mid_raw->effective_painter() == p.get());
    REQUIRE(leaf_raw->effective_painter() == p.get());
}

TEST_CASE("a descendant's own painter wins for its own subtree only",
          "[view][painter]") {
    auto root = std::make_unique<View>();
    auto* root_raw = root.get();
    auto mid = std::make_unique<View>();
    auto* mid_raw = mid.get();
    auto leaf = std::make_unique<View>();
    auto* leaf_raw = leaf.get();
    auto sibling = std::make_unique<View>();
    auto* sibling_raw = sibling.get();

    mid_raw->add_child(std::move(leaf));
    root_raw->add_child(std::move(mid));
    root_raw->add_child(std::move(sibling));

    auto outer = make_painter();
    auto inner = make_painter();
    root_raw->set_painter(outer);
    mid_raw->set_painter(inner);

    REQUIRE(root_raw->effective_painter() == outer.get());
    REQUIRE(mid_raw->effective_painter() == inner.get());
    REQUIRE(leaf_raw->effective_painter() == inner.get());   // inside mid's subtree
    REQUIRE(sibling_raw->effective_painter() == outer.get()); // outside it
}

TEST_CASE("clearing a painter falls back to the nearest ancestor's",
          "[view][painter]") {
    auto root = std::make_unique<View>();
    auto* root_raw = root.get();
    auto child = std::make_unique<View>();
    auto* child_raw = child.get();
    root_raw->add_child(std::move(child));

    auto outer = make_painter();
    auto inner = make_painter();
    root_raw->set_painter(outer);
    child_raw->set_painter(inner);
    REQUIRE(child_raw->effective_painter() == inner.get());

    child_raw->set_painter(nullptr);
    REQUIRE(child_raw->effective_painter() == outer.get());
}

TEST_CASE("a delegate that declines a hook falls through to the stock look",
          "[view][painter]") {
    // Partial override is the point: a delegate that only restyles a button's
    // face must not be forced to reimplement its text, or the knob, or anything
    // else it never asked about.
    std::vector<std::string> log;
    auto p = make_painter(&log);
    p->claim_button_bg = true;      // claims the face
    p->claim_button_text = false;   // declines the label

    TextButton b("Save");
    b.set_bounds({0, 0, 80, 24});
    b.set_painter(p);

    auto png = pulp::view::render_to_png(b, 80, 24, 1.0f,
                                         pulp::view::ScreenshotBackend::skia);
    REQUIRE_FALSE(png.empty());

    // Both hooks were offered; the delegate took one and left the other.
    bool asked_bg = false, asked_text = false;
    for (const auto& e : log) {
        if (e == "button_bg") asked_bg = true;
        if (e == "button_text") asked_text = true;
    }
    REQUIRE(asked_bg);
    REQUIRE(asked_text);
}

TEST_CASE("the rotary hook receives a normalized proportion rather than pixels",
          "[view][painter]") {
    // A rotary reasons in sweep angle, which is a function of a 0..1 position.
    // Handing it a pixel offset renders plausibly and is wrong at every value.
    auto p = make_painter();
    p->claim_rotary = true;

    Knob k;
    k.set_bounds({0, 0, 64, 64});
    k.set_value(0.25f);
    k.set_painter(p);

    auto png = pulp::view::render_to_png(k, 64, 64, 1.0f,
                                         pulp::view::ScreenshotBackend::skia);
    REQUIRE_FALSE(png.empty());

    REQUIRE(p->seen_rotary_position >= 0.0);
    REQUIRE(p->seen_rotary_position <= 1.0);
    REQUIRE_THAT(p->seen_rotary_position,
                 Catch::Matchers::WithinAbs(0.25, 1e-4));
}

TEST_CASE("every paint hook declines by default", "[view][painter]") {
    // The contract that makes partial override possible: a delegate inherits a
    // "no opinion" answer for every hook it does not care about, so restyling one
    // control never conscripts you into reimplementing the whole widget set.
    //
    // Each hook is called here on a bare delegate. If any of them were pure, or
    // returned true, or drew something, this would not compile or would not pass —
    // and a skin that overrode a single knob would silently blank everything else.
    struct Bare : WidgetPainter {};
    Bare p;

    View v;
    v.set_bounds({0, 0, 100, 30});

    // A real canvas, so a hook that mistakenly tried to draw would be exercised
    // rather than skipped.
    pulp::canvas::RecordingCanvas canvas;

    RotaryPaintState rotary;
    LinearPaintState linear;
    ButtonPaintState button;
    ComboBoxPaintState combo;
    MenuBackgroundPaintState menu_bg;
    MenuItemPaintState menu_item;
    MenuHeaderPaintState menu_header;
    TextFieldPaintState field;
    CaretPaintState caret;

    CHECK_FALSE(p.paint_rotary(canvas, rotary, v));
    CHECK_FALSE(p.paint_linear(canvas, linear, v));
    CHECK_FALSE(p.paint_button_background(canvas, button, v));
    CHECK_FALSE(p.paint_button_text(canvas, button, v));
    CHECK_FALSE(p.paint_combo_box(canvas, combo, v));
    CHECK_FALSE(p.paint_scroll_bar(canvas, linear, v));
    CHECK_FALSE(p.paint_menu_background(canvas, menu_bg, v));
    CHECK_FALSE(p.paint_menu_item(canvas, menu_item, v));
    CHECK_FALSE(p.paint_menu_section_header(canvas, menu_header, v));
    CHECK_FALSE(p.paint_text_field_background(canvas, field, v));
    CHECK_FALSE(p.paint_text_field_outline(canvas, field, v));
    CHECK_FALSE(p.paint_caret(canvas, caret, v));

    // And declining really means declining: not one of them put anything on the
    // canvas. A default that quietly drew a fallback would make "no opinion"
    // indistinguishable from "paint nothing", and stack two looks on top of each
    // other the moment a widget also drew its own.
    REQUIRE(canvas.command_count() == 0);
}

TEST_CASE("the scroll-bar hook receives pixel thumb geometry", "[view][painter]") {
    // A scrollbar skin draws a thumb at a position and a length, both in PIXELS
    // along the bar's axis — it cannot do anything with a 0..1 proportion, and a
    // delegate handed one would draw a thumb pinned to the first pixel of the
    // track. The rotary hook is the mirror image (a proportion, no pixels), and
    // confusing the two renders plausibly and is wrong everywhere.
    struct Recorder : WidgetPainter {
        LinearPaintState seen;
        bool called = false;
        bool paint_scroll_bar(pulp::canvas::Canvas&, const LinearPaintState& s,
                              View&) override {
            seen = s;
            called = true;
            return true;                      // claim it: the stock look must not also draw
        }
    };

    auto p = std::make_shared<Recorder>();

    ScrollBar bar;
    bar.set_orientation(ScrollBar::Orientation::vertical);
    bar.set_bounds({0, 0, 14, 200});
    bar.set_range(0.0f, 100.0f);
    bar.set_page_size(25.0f);                 // a quarter of the range is visible
    bar.set_value(50.0f);
    bar.set_painter(p);

    pulp::canvas::RecordingCanvas canvas;
    bar.paint(canvas);

    REQUIRE(p->called);

    // Pixels, not a proportion: the thumb sits well down a 200px-tall bar, so its
    // offset must be on the order of tens of pixels — not 0.5.
    REQUIRE(p->seen.thumb_pos > 1.0f);
    REQUIRE(p->seen.thumb_size > 1.0f);
    REQUIRE(p->seen.thumb_pos + p->seen.thumb_size <= 200.0f);

    // The track bounds it was told about are pixels on the same axis...
    REQUIRE(p->seen.track_max > p->seen.track_min);
    REQUIRE(p->seen.track_max <= 200.0f);

    // ...while the VALUE and its range travel in the caller's units, unconverted,
    // so a skin can render a numeric readout without having to invert the mapping.
    REQUIRE_THAT(p->seen.value, Catch::Matchers::WithinAbs(50.0, 1e-4));
    REQUIRE_THAT(p->seen.value_min, Catch::Matchers::WithinAbs(0.0, 1e-4));
    REQUIRE_THAT(p->seen.value_max, Catch::Matchers::WithinAbs(100.0, 1e-4));

    REQUIRE_FALSE(p->seen.horizontal);        // it was told which way it points
}

TEST_CASE("a claimed scroll-bar hook suppresses the stock look", "[view][painter]") {
    // If the widget drew its own track and thumb after the delegate already had,
    // every skinned scrollbar would render two thumbs stacked on each other.
    struct Claiming : WidgetPainter {
        bool paint_scroll_bar(pulp::canvas::Canvas&, const LinearPaintState&,
                              View&) override {
            return true;
        }
    };

    ScrollBar bar;
    bar.set_bounds({0, 0, 14, 200});
    bar.set_range(0.0f, 100.0f);

    pulp::canvas::RecordingCanvas stock_canvas;
    bar.paint(stock_canvas);
    const size_t stock_ops = stock_canvas.command_count();
    REQUIRE(stock_ops > 0);                   // the stock look really does draw

    bar.set_painter(std::make_shared<Claiming>());
    pulp::canvas::RecordingCanvas skinned;
    bar.paint(skinned);

    // The delegate drew nothing and claimed the hook, so nothing reached the
    // canvas at all — the widget did not draw over it.
    REQUIRE(skinned.command_count() == 0);
}
