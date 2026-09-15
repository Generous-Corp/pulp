// Press reachability — does a press at the rect a control PAINTS actually
// reach that control?
//
// The instrument under test has to be able to fail, and these cases are
// arranged so that it demonstrably does. Every positive assertion is paired
// with a negative control on the same instrument and the same tree: a probe
// that must find nothing, and a tree whose defect must be reported. An
// instrument only ever observed succeeding proves nothing about the run where
// it matters.
//
// The defect being modelled is the ANCESTOR one. A wrapper with a zero-area
// box seals off a subtree of correctly sized, correctly wired controls, and
// every one of them still passes a checker that compares a control's own hit
// rect against its own painted rect. `Rect::contains` is half-open, so a 0x0
// box admits no point; the only thing that ever lets a press through such a
// wrapper is the symmetric ~500px slack `View::hit_test` grants an
// `overflow: visible` child, measured from the wrapper's in-flow position
// rather than from where the control paints.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/press_reach.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/view.hpp>

#include <memory>
#include <string>

using pulp::view::audit_press_reach;
using pulp::view::Point;
using pulp::view::PressChannel;
using pulp::view::PressReachOptions;
using pulp::view::reach_of_press;
using pulp::view::rect_in_root;
using pulp::view::View;

namespace {

class TestView : public View {
public:
    void paint(pulp::canvas::Canvas&) override {}
};

// A root the size of the Spectr editor surface, so the coordinates in these
// tests read the same as the ones in the bug reports they model.
std::unique_ptr<TestView> make_root() {
    auto root = std::make_unique<TestView>();
    root->set_bounds({0, 0, 1320, 860});
    root->set_id("root");
    return root;
}

TestView* add_child(View& parent, const char* id, pulp::view::Rect bounds) {
    auto child = std::make_unique<TestView>();
    child->set_id(id);
    child->set_bounds(bounds);
    auto* raw = child.get();
    parent.add_child(std::move(child));
    return raw;
}

}  // namespace

// ── The instrument resolves, and can report nothing ──────────────────────

TEST_CASE("reach_of_press resolves a press to the control under it",
          "[view][press-reach]") {
    auto root = make_root();
    auto* button = add_child(*root, "close", {893, 73, 32, 32});
    bool fired = false;
    button->on_click = [&] { fired = true; };

    const auto reach = reach_of_press(*root, Point{909, 89});
    REQUIRE(reach.hit == button);
    REQUIRE(reach.handler == button);
    REQUIRE_FALSE(fired);  // resolution only — a gate must not fire handlers
}

TEST_CASE("reach_of_press reports nothing where nothing is — negative control",
          "[view][press-reach]") {
    auto root = make_root();
    auto* button = add_child(*root, "close", {893, 73, 32, 32});
    button->on_click = [] {};

    // Far from the only control in the tree, and inside the root, so a null
    // answer here is the instrument saying "nothing owns this point" rather
    // than the probe falling off the window.
    const auto reach = reach_of_press(*root, Point{200, 500});
    REQUIRE(reach.hit == root.get());   // the root itself owns the pixel
    REQUIRE(reach.handler == nullptr);  // and no click handler resolves there
}

TEST_CASE("reach_of_press bubbles the context-menu channel to the wrapper",
          "[view][press-reach][context-menu]") {
    auto root = make_root();
    auto* band = add_child(*root, "band-32", {370, 200, 16, 400});
    auto* cap = add_child(*band, "band-32-cap", {0, 0, 16, 8});
    band->on_context_menu = [](Point) {};

    // The cap wins the hit test and carries no handler of its own. Both
    // channels bubble to the nearest listener, so the menu still resolves to
    // the band -- and the click channel still finds nothing, because nothing
    // on the chain carries `on_click`. Asserting both keeps this test honest
    // about WHICH channel it is measuring.
    const auto click = reach_of_press(*root, Point{378, 204}, PressChannel::click);
    REQUIRE(click.hit == cap);
    REQUIRE(click.handler == nullptr);

    const auto menu =
        reach_of_press(*root, Point{378, 204}, PressChannel::context_menu);
    REQUIRE(menu.hit == cap);
    REQUIRE(menu.handler == band);
    REQUIRE(menu.local.y == 4.0f);  // local to the band the handler sits on

    // Negative control on the same instrument: outside the band, the same
    // channel resolves to nothing.
    const auto away =
        reach_of_press(*root, Point{900, 204}, PressChannel::context_menu);
    REQUIRE(away.handler == nullptr);
}

// ── rect_in_root ────────────────────────────────────────────────────────

TEST_CASE("rect_in_root composes nested offsets", "[view][press-reach]") {
    auto root = make_root();
    auto* header = add_child(*root, "header", {0, 57, 939, 56});
    auto* close = add_child(*header, "close", {893, 16, 32, 32});

    const auto rect = rect_in_root(*close, *root);
    REQUIRE(rect.has_value());
    REQUIRE(rect->x == 893.0f);
    REQUIRE(rect->y == 73.0f);
    REQUIRE(rect->width == 32.0f);
    REQUIRE(rect->height == 32.0f);
    (void)header;
}

TEST_CASE("rect_in_root refuses to guess rather than answering wrongly",
          "[view][press-reach]") {
    auto root = make_root();
    auto* scaled = add_child(*root, "scaled", {100, 100, 200, 200});
    auto* inner = add_child(*scaled, "inner", {10, 10, 40, 40});
    scaled->set_scale(0.5f);

    // The additive walk cannot express the scale, and `point_to_local` — the
    // function the hosts route real presses through — proves the round trip
    // misses. A std::nullopt here is the instrument declining to launder a
    // guess into a measurement.
    REQUIRE_FALSE(rect_in_root(*inner, *root).has_value());
}

// ── The audit, and the defect it exists to catch ────────────────────────

TEST_CASE("audit_press_reach passes a healthy tree, having examined it",
          "[view][press-reach]") {
    auto root = make_root();
    auto* anchor = add_child(*root, "guide-anchor", {0, 0, 1320, 860});
    auto* panel = add_child(*anchor, "guide-panel", {380, 57, 560, 700});
    auto* close = add_child(*panel, "guide-close", {513, 16, 32, 32});
    close->on_click = [] {};

    const auto audit = audit_press_reach(*root);
    INFO(pulp::view::format_press_reach_audit(audit));
    REQUIRE(audit.examined == 1);  // a clean audit over 0 targets is a blind one
    REQUIRE(audit.clean());
    (void)panel;
}

TEST_CASE("audit_press_reach reports a control sealed off by a 0x0 ancestor",
          "[view][press-reach][issue-119]") {
    auto root = make_root();
    // The shipped shape: a correctly sized close button inside a correctly
    // sized panel, inside an anchor whose box collapsed to nothing. Every
    // self-vs-self rect comparison in the repo passes this tree.
    auto* anchor = add_child(*root, "guide-anchor", {0, 804, 0, 0});
    anchor->set_overflow(View::Overflow::visible);
    auto* panel = add_child(*anchor, "guide-panel", {380, -747, 560, 700});
    panel->set_overflow(View::Overflow::visible);
    auto* close = add_child(*panel, "guide-close", {513, 16, 32, 32});
    close->on_click = [] {};

    const auto audit = audit_press_reach(*root);
    INFO(pulp::view::format_press_reach_audit(audit));
    REQUIRE(audit.examined >= 1);
    REQUIRE_FALSE(audit.clean());

    bool named_the_close_button = false;
    for (const auto& finding : audit.unreachable)
        if (finding.id == "guide-close") named_the_close_button = true;
    REQUIRE(named_the_close_button);
}

TEST_CASE("audit_press_reach reports a zero-area press target itself",
          "[view][press-reach]") {
    auto root = make_root();
    auto* ghost = add_child(*root, "ghost", {400, 400, 0, 0});
    ghost->on_click = [] {};

    const auto audit = audit_press_reach(*root);
    INFO(pulp::view::format_press_reach_audit(audit));
    REQUIRE(audit.unreachable.size() == 1);
    REQUIRE(audit.unreachable[0].id == "ghost");
    REQUIRE(audit.unreachable[0].reason.find("zero-area") != std::string::npos);
}

TEST_CASE("audit_press_reach reports a right-click target a SIBLING covers",
          "[view][press-reach][context-menu]") {
    auto root = make_root();
    auto* band = add_child(*root, "band-10", {150, 200, 16, 400});
    band->on_context_menu = [](Point) {};
    // A full-surface sibling painted after the bands -- a scrim, a readout
    // layer, a drag affordance. It is NOT an ancestor of the band, so the
    // context-menu bubble walks ITS chain (scrim -> root) and never reaches
    // the band's handler. This is the shape `dispatch_context_menu`'s bubble
    // cannot rescue, which is exactly why the audit presses rather than
    // inspecting wiring.
    auto* scrim = add_child(*root, "surface-scrim", {0, 0, 1320, 860});

    const auto audit = audit_press_reach(*root);
    INFO(pulp::view::format_press_reach_audit(audit));
    REQUIRE_FALSE(audit.clean());
    REQUIRE(audit.unreachable.size() == 1);
    REQUIRE(audit.unreachable[0].id == "band-10");
    REQUIRE(audit.unreachable[0].channel == PressChannel::context_menu);
    REQUIRE(audit.unreachable[0].reached_id == "surface-scrim");

    // Positive control on the same tree and the same instrument: remove the
    // scrim and the very same audit goes clean, so the finding is about the
    // scrim and not about the probe.
    root->remove_child(scrim);
    const auto after = audit_press_reach(*root);
    INFO(pulp::view::format_press_reach_audit(after));
    REQUIRE(after.examined == 1);
    REQUIRE(after.clean());
}

TEST_CASE("audit census separates skipped from clean", "[view][press-reach]") {
    auto root = make_root();
    auto* hidden = add_child(*root, "hidden", {100, 100, 40, 40});
    hidden->on_click = [] {};
    hidden->set_visible(false);

    auto* away = add_child(*root, "offscreen", {100, 4000, 40, 40});
    away->on_click = [] {};

    const auto audit = audit_press_reach(*root);
    INFO(pulp::view::format_press_reach_audit(audit));
    REQUIRE(audit.examined == 0);
    REQUIRE(audit.skipped_not_interactive == 1);
    REQUIRE(audit.skipped_offscreen == 1);
    // Clean, and worthless as evidence — which is exactly why `clean()` is
    // documented as meaningless without `examined`.
    REQUIRE(audit.clean());
}

// ── The real right-button press, end to end ─────────────────────────────

TEST_CASE("simulate_context_click opens the menu a right-click should",
          "[view][press-reach][context-menu]") {
    auto root = make_root();
    auto* band = add_child(*root, "band-32", {370, 200, 16, 400});
    int opened = 0;
    Point where{-1, -1};
    band->on_context_menu = [&](Point p) { ++opened; where = p; };

    REQUIRE(root->simulate_context_click(Point{378, 300}));
    REQUIRE(opened == 1);
    REQUIRE(where.x == 8.0f);    // local to the band, not the window
    REQUIRE(where.y == 100.0f);

    // Negative control on the same instrument and the same tree: a press where
    // the band is not must open nothing.
    REQUIRE_FALSE(root->simulate_context_click(Point{900, 300}));
    REQUIRE(opened == 1);
}

TEST_CASE("simulate_click with the right button does NOT open a context menu",
          "[view][press-reach][context-menu]") {
    auto root = make_root();
    auto* band = add_child(*root, "band-32", {370, 200, 16, 400});
    int opened = 0;
    band->on_context_menu = [&](Point) { ++opened; };

    View::SimulatedPointer right;
    right.button = pulp::view::MouseButton::right;
    root->simulate_click(Point{378, 300}, right);

    // This is the trap `simulate_context_click` exists to close. If this ever
    // starts passing a menu through, the two verbs have merged and the comment
    // on `View::simulate_context_click` is wrong.
    REQUIRE(opened == 0);
}
