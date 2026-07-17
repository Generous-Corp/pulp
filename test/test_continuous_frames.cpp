// Headless coverage for pulp::view::needs_continuous_frames — the shared
// predicate that decides whether a view tree still needs per-vsync frames.
// The macOS window and plugin-view hosts gate repaint on it; a foreign-host
// embed tick can gate the same way. These tests exercise the tree walk without
// any GPU/host so they run everywhere.

#include <pulp/view/continuous_frames.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/css_animation.hpp>
#include <pulp/view/eq_curve_view.hpp>
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

TEST_CASE("an EqCurveView mid hover-settle needs continuous frames",
          "[view][continuous-frames][eq_curve]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 320, 160});
    eq.set_sample_rate(48000.0f);
    eq.set_bands({{1000.0f, 6.0f, 2.0f, EqCurveView::FilterType::peak, true}});
    eq.set_hover_animation(true);

    pulp::canvas::RecordingCanvas snap;
    eq.paint(snap);                         // first frame snaps — settled
    REQUIRE_FALSE(eq.hover_animating());
    REQUIRE_FALSE(needs_continuous_frames(&eq));

    // Hovering raises the handle's target radius; the next frame is mid-ease, so
    // the shared predicate must keep the render loop alive.
    auto fs = eq.frequency_scale();
    auto gs = eq.gain_scale();
    eq.on_hover_move({fs.to_x(1000.0f), gs.to_y(6.0f)});
    pulp::canvas::RecordingCanvas easing;
    eq.paint(easing);
    REQUIRE(eq.hover_animating());
    REQUIRE(needs_continuous_frames(&eq));

    // Once it settles the flag clears and the loop is allowed to idle again.
    for (int i = 0; i < 60 && eq.hover_animating(); ++i) {
        pulp::canvas::RecordingCanvas f;
        eq.paint(f);
    }
    REQUIRE_FALSE(eq.hover_animating());
    REQUIRE_FALSE(needs_continuous_frames(&eq));
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

// A shader that declares a `time` uniform animates every frame, so the widget's
// subtree needs continuous frames. Any CustomShaderHost is covered by the single
// dynamic_cast in needs_continuous_frames().
namespace {
// Valid SkSL that really declares `time`. The fixture used to be
// `half4 main() { return half4(time); }`, which declares no uniform and does not
// compile (`main` takes a float2; `time` is undeclared) — it only counted as
// time-driven because the old check was a substring search for "time". That
// pinned the render loop for a shader which could never paint.
constexpr const char* kTimeShader =
    "uniform float time; half4 main(float2 p) { return half4(time); }";
constexpr const char* kStaticShader =
    "half4 main(float2 p) { return half4(1); }";
} // namespace

TEST_CASE("a time-driven widget shader keeps the tree live", "[view][continuous-frames]") {
    SECTION("knob") {
        View root;
        auto knob = std::make_unique<Knob>();
        knob->set_custom_shader(kTimeShader);
        root.add_child(std::move(knob));
        REQUIRE(needs_continuous_frames(&root));
    }
    SECTION("fader") {
        View root;
        auto fader = std::make_unique<Fader>();
        fader->set_custom_shader(kTimeShader);
        root.add_child(std::move(fader));
        REQUIRE(needs_continuous_frames(&root));
    }
    SECTION("toggle") {
        View root;
        auto toggle = std::make_unique<Toggle>();
        toggle->set_custom_shader(kTimeShader);
        root.add_child(std::move(toggle));
        REQUIRE(needs_continuous_frames(&root));
    }
}

// The inverse, which the substring search got wrong: a shader with no `time`
// uniform must NOT pin the render loop, even when the word appears in it.
TEST_CASE("a static widget shader does not keep the tree live",
          "[view][continuous-frames]") {
    SECTION("no time uniform") {
        View root;
        auto knob = std::make_unique<Knob>();
        knob->set_custom_shader(kStaticShader);
        root.add_child(std::move(knob));
        REQUIRE_FALSE(needs_continuous_frames(&root));
    }
    SECTION("a differently-named uniform that merely contains 'time'") {
        View root;
        auto knob = std::make_unique<Knob>();
        knob->set_custom_shader(
            "uniform float timeline; half4 main(float2 p) { return half4(timeline); }");
        root.add_child(std::move(knob));
        REQUIRE_FALSE(needs_continuous_frames(&root));
    }
    SECTION("'time' only in a comment") {
        View root;
        auto knob = std::make_unique<Knob>();
        knob->set_custom_shader(
            "// animates over time\nhalf4 main(float2 p) { return half4(1); }");
        root.add_child(std::move(knob));
        REQUIRE_FALSE(needs_continuous_frames(&root));
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

// begin_host_frame ties the FrameClock activity channel to the repaint decision:
// it pumps wake-from-idle probes, THEN reports whether this frame renders. The
// two cases below assert that ordering — a probe that flips continuous_repaint
// on tick N makes tick N itself render, and an activity subscription on its own
// is never render-liveness. That is the seam that lets an embedded editor idle
// at 0 fps yet wake on the frame a meter starts moving, with no per-View
// host-tick vtable hook.
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/host_frame_pump.hpp>

TEST_CASE("begin_host_frame wakes from idle via an activity probe",
          "[view][continuous-frames][frame-pump]") {
    FrameClock clock;
    View root;
    HostFramePump pump;
    double t = 0.0;
    // Every vsync reaches the pump here (has_idle-style host): the frames the
    // gate would skip are covered by the host-gate tests in test_host_frame_pump.
    auto tick = [&] {
        t += 1.0 / 60.0;
        return begin_host_frame(&root, clock, pump, t, /*needs_repaint=*/false);
    };
    bool liveness = false;  // stands in for "a meter is moving"

    // The view self-subscribes an activity probe that reflects liveness into
    // continuous_repaint each tick — the recommended pattern (no View vtable hook).
    clock.subscribe_activity([&](float) { root.set_continuous_repaint(liveness); });

    // Idle: the probe runs every pump but reports not-moving, so no render. An
    // activity subscription on its own is NOT render-liveness.
    REQUIRE_FALSE(tick().should_render);
    REQUIRE_FALSE(clock.has_active_subscribers());

    // Live: the probe flips continuous_repaint on, so the SAME tick renders
    // (pump runs before the gate — that ordering is the whole point).
    liveness = true;
    REQUIRE(tick().should_render);

    // Back to idle: renders once more to clear, then idles.
    liveness = false;
    REQUIRE_FALSE(tick().should_render);
}

TEST_CASE("begin_host_frame honors the host's own needs_repaint flag",
          "[view][continuous-frames][frame-pump]") {
    FrameClock clock;
    View root;
    HostFramePump pump;
    // No probes, static tree: needs_repaint short-circuits to a render.
    REQUIRE(begin_host_frame(&root, clock, pump, 0.016, /*needs_repaint=*/true).should_render);
    REQUIRE_FALSE(
        begin_host_frame(&root, clock, pump, 0.032, /*needs_repaint=*/false).should_render);
    // Null-safe root.
    REQUIRE_FALSE(
        begin_host_frame(nullptr, clock, pump, 0.048, /*needs_repaint=*/false).should_render);
}
