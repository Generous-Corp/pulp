/// Hit slop: the area a control accepts pointers in, grown past the area it
/// paints.
///
/// Every assertion here is stated as a coordinate that is OUTSIDE the painted
/// box. A test that only pressed inside the box would pass with the feature
/// deleted, so each case is paired with a control press further out that must
/// still miss -- an unbounded hit area would satisfy "the press landed" just as
/// well as a correct one, and only the miss distinguishes them.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

namespace {

/// A leaf that reports a hit and nothing else.
class Target : public View {
public:
    bool wants_mouse_input() const override { return true; }
};

} // namespace

TEST_CASE("hit slop grows the accepted area without moving the painted box",
          "[view][hit-slop]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    // A layout pass-through, so a miss on the target reports nullptr rather
    // than the container -- otherwise every "must still miss" control below
    // would read as a hit on the root and prove nothing.
    root.set_pointer_events(View::PointerEvents::box_none);
    auto owned_target = std::make_unique<Target>();
    auto* target = owned_target.get();
    root.add_child(std::move(owned_target));
    target->set_bounds({50, 50, 40, 20});  // a 40x20 switch, the Spectr size

    SECTION("without slop, a press one point outside the box misses") {
        REQUIRE(root.hit_test({70, 49}) == nullptr);
        REQUIRE(root.hit_test({70, 71}) == nullptr);
        REQUIRE(root.hit_test({49, 60}) == nullptr);
        REQUIRE(root.hit_test({91, 60}) == nullptr);
        // Control: the same instrument DOES find the target inside the box.
        REQUIRE(root.hit_test({70, 60}) == target);
    }

    SECTION("12pt of slop accepts a press 12pt outside on every edge") {
        target->set_hit_slop(12.0f);

        // Painted box is x:[50,90] y:[50,70]. Hit box is x:[38,102] y:[38,82].
        CHECK(root.hit_test({70, 39}) == target);   // 11pt above the box
        CHECK(root.hit_test({70, 81}) == target);   // 11pt below
        CHECK(root.hit_test({39, 60}) == target);   // 11pt left
        CHECK(root.hit_test({101, 60}) == target);  // 11pt right

        // Control: slop is BOUNDED. One point past it must still miss, or the
        // four hits above would prove nothing.
        CHECK(root.hit_test({70, 37}) == nullptr);
        CHECK(root.hit_test({70, 83}) == nullptr);
        CHECK(root.hit_test({37, 60}) == nullptr);
        CHECK(root.hit_test({103, 60}) == nullptr);
    }

    SECTION("slop does not move the painted box or the layout") {
        target->set_hit_slop(12.0f);
        CHECK(target->bounds().x == 50.0f);
        CHECK(target->bounds().y == 50.0f);
        CHECK(target->bounds().width == 40.0f);
        CHECK(target->bounds().height == 20.0f);
        CHECK(target->local_bounds().width == 40.0f);
        CHECK(target->local_bounds().height == 20.0f);
    }

    SECTION("per-edge slop is honored independently") {
        target->set_hit_slop(10.0f, 0.0f, 0.0f, 0.0f);  // top only
        CHECK(root.hit_test({70, 42}) == target);
        CHECK(root.hit_test({70, 75}) == nullptr);  // bottom got none
        CHECK(root.hit_test({45, 60}) == nullptr);  // left got none
    }

    SECTION("hit_bounds reports the accepted rect") {
        target->set_hit_slop(4.0f, 8.0f, 12.0f, 16.0f);
        const auto hb = target->hit_bounds();
        CHECK_THAT(hb.x, WithinAbs(-16.0, 1e-4));
        CHECK_THAT(hb.y, WithinAbs(-4.0, 1e-4));
        CHECK_THAT(hb.width, WithinAbs(40.0 + 16.0 + 8.0, 1e-4));
        CHECK_THAT(hb.height, WithinAbs(20.0 + 4.0 + 12.0, 1e-4));
    }

    SECTION("pointer-events:none still wins over slop") {
        target->set_hit_slop(12.0f);
        target->set_pointer_events(View::PointerEvents::none);
        CHECK(root.hit_test({70, 39}) == nullptr);
        CHECK(root.hit_test({70, 60}) == nullptr);
    }
}

TEST_CASE("a child's slop is consulted by the parent's descent, not just its own"
          " terminal check", "[view][hit-slop]") {
    // The regression this guards: inflating only the self-bounds check leaves
    // the parent refusing to descend into a child the point missed, so the slop
    // is never reached and the feature silently does nothing.
    View root;
    root.set_bounds({0, 0, 200, 200});
    root.set_pointer_events(View::PointerEvents::box_none);
    auto owned_mid = std::make_unique<View>();
    auto* mid = owned_mid.get();
    root.add_child(std::move(owned_mid));
    mid->set_bounds({0, 0, 200, 200});
    mid->set_pointer_events(View::PointerEvents::box_none);
    auto owned_leaf = std::make_unique<Target>();
    auto* leaf = owned_leaf.get();
    mid->add_child(std::move(owned_leaf));
    leaf->set_bounds({80, 80, 10, 10});

    REQUIRE(root.hit_test({85, 85}) == leaf);  // control: inside
    REQUIRE(root.hit_test({85, 74}) == nullptr);
    leaf->set_hit_slop(14.0f);
    CHECK(root.hit_test({85, 74}) == leaf);
    CHECK(root.hit_test({85, 64}) == nullptr);  // bounded
}

TEST_CASE("RangeSlider presents a 44pt touch target from a 16pt box",
          "[view][hit-slop][range-slider]") {
    View root;
    root.set_bounds({0, 0, 300, 100});
    root.set_pointer_events(View::PointerEvents::box_none);
    auto owned_slider = std::make_unique<RangeSlider>();
    auto* slider = owned_slider.get();
    root.add_child(std::move(owned_slider));
    // The Spectr settings row: a full-width slider on its intrinsic height.
    slider->set_bounds({20, 40, 200, RangeSlider::kMinorAxisExtent});

    SECTION("the hit box is 44pt tall while the painted box stays 16pt") {
        CHECK_THAT(slider->local_bounds().height,
                   WithinAbs(RangeSlider::kMinorAxisExtent, 1e-4));
        CHECK_THAT(slider->hit_bounds().height, WithinAbs(44.0, 1e-4));
        CHECK_THAT(slider->hit_slop().top, WithinAbs(14.0, 1e-4));
    }

    SECTION("a press 12pt above the painted track still lands on the slider") {
        // Painted y:[40,56]. Hit y:[26,70].
        CHECK(root.hit_test({120, 28}) == slider);
        CHECK(root.hit_test({120, 68}) == slider);
        // Control: bounded.
        CHECK(root.hit_test({120, 24}) == nullptr);
        CHECK(root.hit_test({120, 72}) == nullptr);
    }

    SECTION("the thumb at either travel extreme is grabbable") {
        // Half the 24pt-wide thumb hangs off each end of the track.
        CHECK(root.hit_test({12, 48}) == slider);   // 8pt left of x=20
        CHECK(root.hit_test({228, 48}) == slider);  // 8pt right of x=220
        CHECK(root.hit_test({4, 48}) == nullptr);   // bounded
    }
}

TEST_CASE("RangeSlider thumb grows on pointer proximity", "[range-slider][hover]") {
    // The morph slider's enlarge behavior, asserted on the painted extent
    // rather than on the animation object, because the extent is what the user
    // sees and what a re-implementation would have to reproduce.
    RangeSlider slider;
    slider.set_bounds({0, 0, 200, RangeSlider::kMinorAxisExtent});

    const float rest = slider.hover_scale();
    CHECK_THAT(rest, WithinAbs(1.0, 1e-4));

    slider.on_mouse_enter();
    // The growth is animated, so drive it to completion the way the frame pump
    // does. motion.duration.fast is 0.08s; 0.5s is comfortably past it.
    for (int i = 0; i < 50; ++i) slider.advance_animations(0.01f);

    const float hovered = slider.hover_scale();
    CHECK(hovered > rest);
    CHECK_THAT(hovered, WithinAbs(1.3, 1e-3));

    // paint() derives the thumb from hover_scale, so state the pixels:
    const float thumb_minor_rest = RangeSlider::kMinorAxisExtent * rest;
    const float thumb_minor_hover = RangeSlider::kMinorAxisExtent * hovered;
    CHECK_THAT(thumb_minor_rest, WithinAbs(16.0, 1e-3));
    CHECK_THAT(thumb_minor_hover, WithinAbs(20.8, 1e-2));

    slider.on_mouse_leave();
    for (int i = 0; i < 50; ++i) slider.advance_animations(0.01f);
    CHECK_THAT(slider.hover_scale(), WithinAbs(1.0, 1e-3));
}
