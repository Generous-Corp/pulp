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

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

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

// EACH WIDGET ANSWERS FOR ITSELF, and the tree walk only asks.
//
// The predicate used to try six `dynamic_cast`s per node per frame, three of
// them through multiple inheritance, and on a real UI tree that RTTI search was
// the largest single CPU cost in an idle window -- more than the drawing it
// gated. This pins the replacement contract rather than only its outcome: a
// widget with an animation of its own has to report it through
// `needs_frames_self()`, because that is the only question the walk now asks.
TEST_CASE("a widget reports its own liveness", "[view][continuous-frames]") {
    SECTION("a plain view is never live on its own") {
        View v;
        REQUIRE_FALSE(v.needs_frames_self());
    }
    SECTION("a time-driven shader makes the widget itself report live") {
        Knob knob;
        REQUIRE_FALSE(knob.needs_frames_self());
        knob.set_custom_shader(
            "uniform float time; half4 main(float2 p) { return half4(time); }");
        REQUIRE(knob.needs_frames_self());
    }
    SECTION("an EQ analyzer with data reports live from the widget") {
        EqCurveView eq;
        eq.set_bounds({0, 0, 320, 160});
        eq.set_sample_rate(48000.0f);
        REQUIRE_FALSE(eq.needs_frames_self());
        const std::vector<float> bins(64, -30.0f);
        eq.set_spectrum(bins.data(), bins.size());
        REQUIRE(eq.needs_frames_self());
    }
}

// EVERY BRANCH THAT KEEPS THE LOOP ALIVE NEEDS A POSITIVE TEST.
//
// `needs_frames_self()` replaced a chain of `dynamic_cast`s, and a replacement
// is only equivalent if each branch it subsumed still answers yes when it
// should. The idle-side coverage below ("idle widgets do not force frames") is
// satisfied by a predicate that returns false unconditionally — so on its own it
// would let a widget whose animation stopped pinning the loop sail through the
// whole suite, and the symptom is a knob whose glow freezes mid-fade rather than
// anything that fails a build. These drive each widget's own animation and
// assert the live window between "started" and "settled", through the shared
// tree walk as well as the widget itself.
//
// One host tick at 60 Hz is 16.7 ms into an 80-150 ms ease: far enough in to be
// unambiguously moving, far short of arriving.
//
// Each predicate carries a DEADBAND, and the settled assertions below are
// written against it rather than against the animation's arithmetic end: a
// fader stops asking once its thumb is within 0.01 of rest, a toggle once its
// thumb is past 0.99. So the last sub-1% of travel is never painted unless
// something else is keeping the loop alive. That is the intended trade -- an
// invisible sliver is not worth a frame -- and it is the reason `settle()` here
// loops on `needs_frames_self()` rather than on the underlying value.
namespace {
constexpr float kTick = 1.0f / 60.0f;

// Run the animation to completion the way a host does, with a bound so a
// predicate that never settles fails as a hang-free assertion instead.
void settle(View& v) {
    for (int i = 0; i < 600 && v.needs_frames_self(); ++i) v.advance_animations(kTick);
}
}  // namespace

TEST_CASE("a knob mid hover-glow keeps the tree live", "[view][continuous-frames]") {
    View root;
    auto owned = std::make_unique<Knob>();
    Knob* knob = owned.get();
    knob->set_bounds({0, 0, 60, 60});
    root.add_child(std::move(owned));
    REQUIRE_FALSE(needs_continuous_frames(&root));

    knob->on_mouse_enter();          // glow eases 0 -> 1
    knob->advance_animations(kTick); // mid-ease
    CHECK(knob->hover_glow() > 0.01f);
    CHECK(knob->hover_glow() < 0.99f);
    CHECK(knob->needs_frames_self());
    CHECK(needs_continuous_frames(&root));

    settle(*knob);
    CHECK(knob->hover_glow() == Catch::Approx(1.0f));
    CHECK_FALSE(needs_continuous_frames(&root));
}

TEST_CASE("a fader mid hover-scale keeps the tree live", "[view][continuous-frames]") {
    View root;
    auto owned = std::make_unique<Fader>();
    Fader* fader = owned.get();
    fader->set_bounds({0, 0, 40, 160});
    root.add_child(std::move(owned));
    REQUIRE_FALSE(needs_continuous_frames(&root));

    fader->on_mouse_enter();          // thumb grows 1.0 -> 1.3
    fader->advance_animations(kTick);
    CHECK(fader->hover_scale() > 1.01f);
    CHECK(fader->needs_frames_self());
    CHECK(needs_continuous_frames(&root));

    // Leaving shrinks it back; once it reaches 1.0 the fader is idle again.
    fader->on_mouse_leave();
    settle(*fader);
    CHECK(fader->hover_scale() <= 1.01f);   // inside the deadband, i.e. at rest
    CHECK_FALSE(needs_continuous_frames(&root));
}

TEST_CASE("a toggle mid thumb-travel keeps the tree live", "[view][continuous-frames]") {
    View root;
    auto owned = std::make_unique<Toggle>();
    Toggle* toggle = owned.get();
    toggle->set_bounds({0, 0, 48, 24});
    root.add_child(std::move(owned));
    REQUIRE_FALSE(needs_continuous_frames(&root));

    toggle->set_on(true, /*animate=*/true);  // thumb travels 0 -> 1
    toggle->advance_animations(kTick);
    CHECK(toggle->thumb_position() > 0.01f);
    CHECK(toggle->thumb_position() < 0.99f);
    CHECK(toggle->needs_frames_self());
    CHECK(needs_continuous_frames(&root));

    settle(*toggle);
    CHECK(toggle->thumb_position() >= 0.99f);  // inside the deadband, i.e. arrived
    CHECK_FALSE(needs_continuous_frames(&root));
}

TEST_CASE("a scroll view mid offset-ease keeps the tree live",
          "[view][continuous-frames]") {
    View root;
    auto owned = std::make_unique<ScrollView>();
    ScrollView* scroll = owned.get();
    scroll->set_bounds({0, 0, 200, 100});
    scroll->set_content_size({200, 800});   // taller than the view, so it can scroll
    root.add_child(std::move(owned));
    REQUIRE_FALSE(needs_continuous_frames(&root));

    // Programmatic scroll eases (wheel input passes animate=false and jumps,
    // which is why this asks for the animated path explicitly).
    scroll->scroll_by(0.0f, 120.0f, /*animate=*/true);
    CHECK(scroll->scroll_animating());
    CHECK(scroll->needs_frames_self());
    CHECK(needs_continuous_frames(&root));

    settle(*scroll);
    CHECK(scroll->scroll_y() == Catch::Approx(120.0f));
    CHECK_FALSE(needs_continuous_frames(&root));
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
// subtree needs continuous frames.
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
