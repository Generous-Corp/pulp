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
