// Headless coverage for pulp::view::needs_continuous_frames — the shared
// predicate that decides whether a view tree still needs per-vsync frames.
// The macOS window and plugin-view hosts gate repaint on it; a foreign-host
// embed tick can gate the same way. These tests exercise the tree walk without
// any GPU/host so they run everywhere.

#include <pulp/view/continuous_frames.hpp>

#include <pulp/view/css_animation.hpp>
#include <pulp/view/ui_components.hpp>  // ScrollView
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace pulp::view;

TEST_CASE("needs_continuous_frames is null-safe", "[view][continuous-frames]") {
    REQUIRE_FALSE(needs_continuous_frames(nullptr));
}

TEST_CASE("a static tree needs no continuous frames", "[view][continuous-frames]") {
    View root;
    auto child = std::make_unique<View>();
    child->add_child(std::make_unique<View>());
    root.add_child(std::move(child));

    REQUIRE_FALSE(needs_continuous_frames(&root));
}

TEST_CASE("an opted-in continuous-repaint view is detected", "[view][continuous-frames]") {
    View v;
    REQUIRE_FALSE(needs_continuous_frames(&v));

    v.set_continuous_repaint(true);
    REQUIRE(needs_continuous_frames(&v));

    v.set_continuous_repaint(false);
    REQUIRE_FALSE(needs_continuous_frames(&v));
}

TEST_CASE("the predicate walks descendants", "[view][continuous-frames]") {
    View root;
    auto* leaf = [&]() -> View* {
        auto mid = std::make_unique<View>();
        auto deep = std::make_unique<View>();
        View* raw = deep.get();
        mid->add_child(std::move(deep));
        root.add_child(std::move(mid));
        return raw;
    }();

    REQUIRE_FALSE(needs_continuous_frames(&root));

    // A single deep descendant asking for frames pulls the whole tree live.
    leaf->set_continuous_repaint(true);
    REQUIRE(needs_continuous_frames(&root));

    leaf->set_continuous_repaint(false);
    REQUIRE_FALSE(needs_continuous_frames(&root));
}

TEST_CASE("idle widgets do not force frames", "[view][continuous-frames]") {
    View root;
    root.add_child(std::make_unique<Knob>());
    root.add_child(std::make_unique<Fader>());
    root.add_child(std::make_unique<Toggle>());
    root.add_child(std::make_unique<ScrollView>());

    // Freshly built widgets are static: no hover glow, no time-driven shader,
    // no scroll animation. The predicate must not report them as live.
    REQUIRE_FALSE(needs_continuous_frames(&root));
}

TEST_CASE("a time-driven widget shader keeps the tree live", "[view][continuous-frames]") {
    // A shader whose body samples `time` animates every frame, so the widget's
    // subtree needs continuous frames. This exercises the per-widget
    // dynamic_cast branches (which the generic continuous-repaint opt-in would
    // otherwise short-circuit past).
    SECTION("knob") {
        View root;
        auto knob = std::make_unique<Knob>();
        knob->set_custom_shader("half4 main() { return half4(time); }");
        root.add_child(std::move(knob));
        REQUIRE(needs_continuous_frames(&root));
    }
    SECTION("fader") {
        View root;
        auto fader = std::make_unique<Fader>();
        fader->set_custom_shader("half4 main() { return half4(time); }");
        root.add_child(std::move(fader));
        REQUIRE(needs_continuous_frames(&root));
    }
    SECTION("toggle") {
        View root;
        auto toggle = std::make_unique<Toggle>();
        toggle->set_custom_shader("half4 main() { return half4(time); }");
        root.add_child(std::move(toggle));
        REQUIRE(needs_continuous_frames(&root));
    }
}

TEST_CASE("a running CSS animation keeps frames alive; paused does not",
          "[view][continuous-frames]") {
    View v;
    CssAnimation anim;
    anim.active = true;
    v.active_animations().push_back(anim);

    // Default play state is unset (not "paused"), so an active animation counts.
    REQUIRE(needs_continuous_frames(&v));

    v.set_animation_play_state("paused");
    REQUIRE_FALSE(needs_continuous_frames(&v));

    v.set_animation_play_state("running");
    REQUIRE(needs_continuous_frames(&v));

    // A completed (inactive) animation no longer pins frames.
    v.active_animations().front().active = false;
    REQUIRE_FALSE(needs_continuous_frames(&v));
}
