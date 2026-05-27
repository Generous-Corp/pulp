// BubbleMessageComponent tests
// (closes the gap-doc Phase 3 row
//  "TooltipWindow + BubbleMessageComponent + AlertWindow styled" for
//  the BubbleMessage half).
//
// Validates:
//   - show_for puts the bubble in `showing` immediately + sets text +
//     starts the fade-in,
//   - tick(dt) auto-dismisses after `lifetime()`,
//   - hide() fades out without auto-dismissing,
//   - move_to overrides the source-anchored position,
//   - recompute_anchor projects the source view's bounds per `Side`,
//   - the bubble forgets its source when it returns to idle (transient
//     lifetime — safe against the source view being deleted afterwards).

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <pulp/view/bubble_message.hpp>
#include <pulp/view/view.hpp>

using namespace pulp::view;

namespace {
class TestView : public View {};
}  // namespace

TEST_CASE("BubbleMessageComponent: defaults are idle + invisible",
          "[bubble-message]") {
    BubbleMessageComponent b;
    REQUIRE_FALSE(b.visible());
    REQUIRE(b.phase() == BubbleMessageComponent::Phase::idle);
    REQUIRE(b.opacity() == 0.0f);
    REQUIRE(b.text().empty());
    REQUIRE(b.source_view() == nullptr);
    REQUIRE(b.lifetime() == 3.0f);  // default
}

TEST_CASE("BubbleMessageComponent: show_for goes immediately to showing",
          "[bubble-message]") {
    TestView v;
    v.set_bounds({100, 100, 80, 24});
    BubbleMessageComponent b;
    b.show_for(&v, "saved", 1.0f);

    REQUIRE(b.visible());
    REQUIRE(b.phase() == BubbleMessageComponent::Phase::showing);
    REQUIRE(b.text() == "saved");
    REQUIRE(b.source_view() == &v);
    REQUIRE(b.lifetime() == 1.0f);

    // Initial position derives from the source bounds + default
    // side (above) + default offset (0, -8).
    auto pos = b.position();
    REQUIRE(pos.x == 140.0f);  // bounds center x
    REQUIRE(pos.y == 92.0f);   // bounds.y - 8
}

TEST_CASE("BubbleMessageComponent: lifetime auto-dismisses + fades to idle",
          "[bubble-message]") {
    TestView v;
    BubbleMessageComponent b;
    b.set_fade_duration(0.05f);
    b.show_for(&v, "ack", 0.5f);
    REQUIRE(b.visible());

    // Tick under lifetime — still showing.
    REQUIRE(b.tick(0.3f));
    REQUIRE(b.phase() == BubbleMessageComponent::Phase::showing);

    // Tick past lifetime — flips into hiding.
    REQUIRE(b.tick(0.3f));
    REQUIRE(b.phase() == BubbleMessageComponent::Phase::hiding);

    // Drain the fade — settles into idle.
    while (b.tick(0.05f)) { /* keep ticking */ }
    REQUIRE_FALSE(b.visible());
    REQUIRE(b.phase() == BubbleMessageComponent::Phase::idle);
    REQUIRE(b.source_view() == nullptr);  // transient: source forgotten
}

TEST_CASE("BubbleMessageComponent: hide() fades out without auto-dismiss",
          "[bubble-message]") {
    TestView v;
    BubbleMessageComponent b;
    b.set_fade_duration(0.05f);
    b.show_for(&v, "msg", 0.0f);  // 0 lifetime → no auto-dismiss

    REQUIRE(b.tick(0.5f));  // sits in `showing` since lifetime is off
    REQUIRE(b.phase() == BubbleMessageComponent::Phase::showing);

    b.hide();
    REQUIRE(b.phase() == BubbleMessageComponent::Phase::hiding);
    while (b.tick(0.05f)) { /* drain */ }
    REQUIRE(b.phase() == BubbleMessageComponent::Phase::idle);
}

TEST_CASE("BubbleMessageComponent: move_to overrides source-derived anchor",
          "[bubble-message]") {
    TestView v;
    v.set_bounds({0, 0, 50, 50});
    BubbleMessageComponent b;
    b.show_for(&v, "msg");
    b.move_to({500, 200});
    REQUIRE(b.position().x == 500.0f);
    REQUIRE(b.position().y == 192.0f);  // +offset (0, -8)

    // recompute_anchor honors the explicit anchor.
    b.recompute_anchor();
    REQUIRE(b.position().x == 500.0f);
    REQUIRE(b.position().y == 192.0f);
}

TEST_CASE("BubbleMessageComponent: side enum projects through bounds",
          "[bubble-message]") {
    TestView v;
    v.set_bounds({100, 200, 40, 20});
    BubbleMessageComponent b;
    b.set_offset({0, 0});

    b.set_side(BubbleMessageComponent::Side::above);
    b.show_for(&v, "x");
    REQUIRE(b.position().x == 120.0f);
    REQUIRE(b.position().y == 200.0f);

    b.set_side(BubbleMessageComponent::Side::below);
    b.recompute_anchor();
    REQUIRE(b.position().y == 220.0f);

    b.set_side(BubbleMessageComponent::Side::left);
    b.recompute_anchor();
    REQUIRE(b.position().x == 100.0f);
    REQUIRE(b.position().y == 210.0f);

    b.set_side(BubbleMessageComponent::Side::right);
    b.recompute_anchor();
    REQUIRE(b.position().x == 140.0f);
}

TEST_CASE("BubbleMessageComponent: show_for while visible swaps text + resets",
          "[bubble-message]") {
    TestView v;
    BubbleMessageComponent b;
    b.show_for(&v, "first", 5.0f);
    REQUIRE(b.tick(2.0f));
    REQUIRE(b.elapsed() == 2.0f);

    b.show_for(&v, "second", 1.0f);
    REQUIRE(b.text() == "second");
    REQUIRE(b.lifetime() == 1.0f);
    REQUIRE(b.elapsed() == 0.0f);
}

TEST_CASE("BubbleMessageComponent: tick on idle is a no-op",
          "[bubble-message]") {
    BubbleMessageComponent b;
    REQUIRE_FALSE(b.tick(1.0f));
    REQUIRE(b.phase() == BubbleMessageComponent::Phase::idle);
}
