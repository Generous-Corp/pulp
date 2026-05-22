// Automated test for ScrollView (already exists from animation merge)
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/canvas/canvas.hpp>

using namespace pulp::view;

TEST_CASE("ScrollView: scroll_by changes offset", "[scrollview]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 100});
    sv.set_content_size({200, 500});

    sv.scroll_by(0, 50);
    REQUIRE(sv.target_scroll_y() > 0.0f);
}

// WYSIWYG P6 FIX 4 — wheel/trackpad scroll is INSTANT; programmatic scroll
// (the animate=true default) eases. The OS already smooths trackpad input,
// so animating each delta lags behind the fingers.
TEST_CASE("ScrollView: wheel scroll is instant, programmatic is animated",
          "[scrollview]") {
    SECTION("wheel event jumps the offset immediately") {
        ScrollView sv;
        sv.set_bounds({0, 0, 200, 100});
        sv.set_content_size({200, 500});

        MouseEvent wheel;
        wheel.is_wheel = true;
        wheel.scroll_delta_y = 1.0f;
        sv.on_mouse_event(wheel);

        // Offset reached its target with no pending animation.
        REQUIRE(sv.target_scroll_y() > 0.0f);
        REQUIRE(sv.scroll_y() == Catch::Approx(sv.target_scroll_y()));
        REQUIRE_FALSE(sv.scroll_animating());
    }

    SECTION("scroll_by(animate=false) jumps instantly") {
        ScrollView sv;
        sv.set_bounds({0, 0, 200, 100});
        sv.set_content_size({200, 500});

        sv.scroll_by(0, 60, /*animate=*/false);
        REQUIRE(sv.scroll_y() == Catch::Approx(sv.target_scroll_y()));
        REQUIRE_FALSE(sv.scroll_animating());
    }

    SECTION("programmatic scroll_by (default) eases to its target") {
        ScrollView sv;
        sv.set_bounds({0, 0, 200, 100});
        sv.set_content_size({200, 500});

        sv.scroll_by(0, 60);  // animate defaults to true
        // Mid-animation the visible offset has not yet reached the target.
        REQUIRE(sv.target_scroll_y() == Catch::Approx(60.0f));
        REQUIRE(sv.scroll_animating());
        REQUIRE(sv.scroll_y() < sv.target_scroll_y());

        // After enough advance it settles on the target.
        for (int i = 0; i < 60; ++i) sv.advance_animations(1.0f / 60.0f);
        REQUIRE(sv.scroll_y() == Catch::Approx(sv.target_scroll_y()).margin(0.5));
    }
}

// WYSIWYG P6 FIX 4 (part 2) — find_scroll_view_at resolves the ScrollView a
// point is over even when the point sits over EMPTY background inside the
// pane (where hit_test can return null). The mac host uses this to route a
// wheel event to the pane the cursor hovers without a prior click.
TEST_CASE("find_scroll_view_at resolves the pane under a point",
          "[scrollview]") {
    View root;
    root.set_bounds({0, 0, 400, 400});

    auto sv = std::make_unique<ScrollView>();
    sv->set_bounds({50, 60, 300, 200});
    sv->set_content_size({300, 1000});
    ScrollView* sv_raw = sv.get();
    root.add_child(std::move(sv));

    // Point inside the ScrollView's bounds, over empty background (no child).
    REQUIRE(find_scroll_view_at(root, {100, 120}) == sv_raw);
    // Point outside the ScrollView resolves to nothing.
    REQUIRE(find_scroll_view_at(root, {10, 10}) == nullptr);

    SECTION("nested ScrollView returns the deepest one") {
        auto inner = std::make_unique<ScrollView>();
        inner->set_bounds({20, 20, 100, 100});  // local to sv
        inner->set_content_size({100, 400});
        ScrollView* inner_raw = inner.get();
        sv_raw->add_child(std::move(inner));

        // Window point (90,100) → sv-local (40,40) → inside inner (20..120).
        REQUIRE(find_scroll_view_at(root, {90, 100}) == inner_raw);
        // A point inside sv but outside inner stays on the outer one.
        REQUIRE(find_scroll_view_at(root, {300, 250}) == sv_raw);
    }
}

TEST_CASE("ScrollView: scroll wheel event scrolls", "[scrollview]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 100});
    sv.set_content_size({200, 500});

    // macOS: positive delta = scroll up, negative = scroll down
    // scroll_by multiplies by sensitivity (30), so delta_y=1 → scroll_by(0, 30)
    // which increases target_scroll_y (scrolls content up = you see lower content)
    MouseEvent wheel;
    wheel.is_wheel = true;
    wheel.scroll_delta_y = 1.0f;  // Scroll down
    sv.on_mouse_event(wheel);

    REQUIRE(sv.target_scroll_y() > 0.0f);
}

TEST_CASE("ScrollView: scroll clamped to content", "[scrollview]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 100});
    sv.set_content_size({200, 500});

    // Scroll way past max
    sv.scroll_by(0, 10000);
    // Target should be clamped to max (500 - 100 = 400)
    REQUIRE(sv.target_scroll_y() <= 400.0f);

    // Scroll way past min
    sv.set_scroll(0, 0);
    sv.scroll_by(0, -10000);
    REQUIRE(sv.target_scroll_y() >= 0.0f);
}

TEST_CASE("ScrollView: paint_all renders without crash", "[scrollview]") {
    pulp::canvas::RecordingCanvas rc;
    ScrollView sv;
    sv.set_theme(Theme::dark());
    sv.set_bounds({0, 0, 200, 100});
    sv.set_content_size({200, 300});

    auto label = std::make_unique<Label>("Scrollable content");
    label->set_bounds({0, 0, 200, 20});
    sv.add_child(std::move(label));

    sv.paint(rc);
    REQUIRE(rc.commands().size() > 0);
}

TEST_CASE("ScrollView: bar opacity starts at zero", "[scrollview]") {
    ScrollView sv;
    REQUIRE(sv.bar_opacity() == 0.0f);
}

TEST_CASE("ScrollView: hit testing follows scrolled content", "[scrollview]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 100});
    sv.set_content_size({200, 300});

    auto label = std::make_unique<Label>("Scrolled");
    auto* label_ptr = label.get();
    label->set_bounds({0, 120, 120, 20});
    sv.add_child(std::move(label));

    sv.set_scroll(0, 100);

    auto* hit = sv.hit_test({10, 30});
    REQUIRE(hit == label_ptr);
}

// ── pulp #1170: ScrollView honors React Native pointerEvents ───────────────
//
// Codex P1 follow-up on #1044 — ScrollView::hit_test shadowed View::hit_test
// without honoring pointer_events(), so setPointerEvents("box-only"/"box-none"
// /"none") was a silent no-op for any scrollable container.

TEST_CASE("ScrollView::hit_test honors pointerEvents == none (#1170)",
          "[scrollview][hit_test][issue-1170]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 200});
    sv.set_content_size({200, 200});

    auto child = std::make_unique<View>();
    child->set_bounds({50, 50, 100, 100});
    sv.add_child(std::move(child));

    sv.set_pointer_events(View::PointerEvents::none);
    REQUIRE(sv.hit_test({75, 75}) == nullptr);
    REQUIRE(sv.hit_test({10, 10}) == nullptr);
}

TEST_CASE("ScrollView::hit_test honors pointerEvents == box-only (#1170)",
          "[scrollview][hit_test][issue-1170]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 200});
    sv.set_content_size({200, 200});

    auto child = std::make_unique<View>();
    child->set_bounds({50, 50, 100, 100});
    sv.add_child(std::move(child));

    // box_only: descent skipped — hits inside child bounds resolve to the
    // ScrollView itself, not to the child.
    sv.set_pointer_events(View::PointerEvents::box_only);
    REQUIRE(sv.hit_test({75, 75}) == &sv);
    REQUIRE(sv.hit_test({10, 10}) == &sv);
}

TEST_CASE("ScrollView::hit_test honors pointerEvents == box-none (#1170)",
          "[scrollview][hit_test][issue-1170]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 200});
    sv.set_content_size({200, 200});

    auto child = std::make_unique<View>();
    child->set_bounds({50, 50, 100, 100});
    auto* child_ptr = child.get();
    sv.add_child(std::move(child));

    // box_none: child is hit-testable, but a point in the ScrollView's
    // own bounds (not on a child) returns nullptr — never self.
    sv.set_pointer_events(View::PointerEvents::box_none);
    REQUIRE(sv.hit_test({75, 75}) == child_ptr);
    REQUIRE(sv.hit_test({10, 10}) == nullptr);
}

TEST_CASE("ScrollView::hit_test box-none also suppresses scrollbar self-target (#1170)",
          "[scrollview][hit_test][issue-1170]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 100});
    // Content taller than view → vertical scrollbar appears at the right edge.
    sv.set_content_size({200, 400});

    sv.set_pointer_events(View::PointerEvents::box_none);
    // Hit near the right edge would normally land on the v-scrollbar
    // and return self; box_none must suppress that.
    REQUIRE(sv.hit_test({195, 50}) == nullptr);
}

// ── pulp #1148 slice (a) — symmetric overflow:visible hit-test extension ──
//
// ScrollView::hit_test had the same right/down-only asymmetry as
// View::hit_test. Lock the symmetric ±500px slack here too so left- or
// up-extending popovers anchored inside a scroll container hit-test
// correctly.
//
// Same fixture pattern as test_view.cpp: place a popover grandchild
// outside an overflow:visible container in each direction, assert the
// ScrollView routes the click into the popover via the symmetric slack.
namespace {
struct ScrollPopoverFixture {
    ScrollView sv;
    View* container{nullptr};
    View* popover{nullptr};

    ScrollPopoverFixture(float dx, float dy) {
        sv.set_bounds({0, 0, 2000, 2000});
        sv.set_content_size({2000, 2000});
        auto c = std::make_unique<View>();
        c->set_bounds({600, 600, 100, 100});
        c->set_overflow(View::Overflow::visible);
        container = c.get();

        auto p = std::make_unique<View>();
        p->set_bounds({dx, dy, 50, 50});
        popover = p.get();
        c->add_child(std::move(p));
        sv.add_child(std::move(c));
    }
};
} // namespace

TEST_CASE("ScrollView::hit_test extends overflow:visible 500px to the LEFT",
          "[scrollview][hit_test][issue-1148][overflow-symmetric]") {
    ScrollPopoverFixture f(-200, 25);
    REQUIRE(f.sv.hit_test({425, 650}) == f.popover);
}

TEST_CASE("ScrollView::hit_test extends overflow:visible 500px to the RIGHT",
          "[scrollview][hit_test][issue-1148][overflow-symmetric]") {
    ScrollPopoverFixture f(200, 25);
    REQUIRE(f.sv.hit_test({825, 650}) == f.popover);
}

TEST_CASE("ScrollView::hit_test extends overflow:visible 500px UPWARD",
          "[scrollview][hit_test][issue-1148][overflow-symmetric]") {
    ScrollPopoverFixture f(25, -200);
    REQUIRE(f.sv.hit_test({650, 425}) == f.popover);
}

TEST_CASE("ScrollView::hit_test extends overflow:visible 500px DOWNWARD",
          "[scrollview][hit_test][issue-1148][overflow-symmetric]") {
    ScrollPopoverFixture f(25, 200);
    REQUIRE(f.sv.hit_test({650, 825}) == f.popover);
}

TEST_CASE("ScrollView::hit_test does NOT extend overflow:visible past 500px LEFT",
          "[scrollview][hit_test][issue-1148][overflow-symmetric]") {
    ScrollPopoverFixture f(-650, 25);
    REQUIRE(f.sv.hit_test({0, 650}) != f.popover);
}
