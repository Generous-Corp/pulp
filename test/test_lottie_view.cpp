// test_lottie_view.cpp — LottieView playback logic. Compiles and passes whether
// or not Lottie support is built (PULP_LOTTIE). When it is, it exercises real
// skottie parsing + the playhead/loop logic; when not, it asserts the graceful
// disabled behavior.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/frame_clock.hpp>
#include <pulp/view/lottie_view.hpp>
#include <pulp/canvas/recording_canvas.hpp>

#include <string>

using pulp::view::LottieView;

namespace {
// Minimal valid Bodymovin: a 100x100, 1-second (30 frames @ 30fps) red solid.
const char* kLottieJson = R"({
  "v":"5.7.0","fr":30,"ip":0,"op":30,"w":100,"h":100,"nm":"t","ddd":0,"assets":[],
  "layers":[{"ddd":0,"ind":1,"ty":1,"nm":"solid","sr":1,
    "ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[50,50,0]},
          "a":{"a":0,"k":[50,50,0]},"s":{"a":0,"k":[100,100,100]}},
    "ao":0,"sw":100,"sh":100,"sc":"#ff0000","ip":0,"op":30,"st":0,"bm":0}]
})";
}  // namespace

TEST_CASE("LottieView graceful behavior when Lottie is not compiled in",
          "[view][lottie]") {
    if (LottieView::supported()) {
        SUCCEED("Lottie compiled in; covered by the playback test");
        return;
    }
    LottieView v;
    REQUIRE_FALSE(v.valid());
    REQUIRE_FALSE(v.set_source_json(kLottieJson));
    REQUIRE(v.duration() == 0.0);
}

// When Lottie is compiled out, painting a LottieView must draw a visible
// placeholder (border + label), not nothing — a silently blank box reads as a
// broken import rather than "unavailable in this build".
TEST_CASE("LottieView paints a placeholder when Lottie is not compiled in",
          "[view][lottie]") {
    if (LottieView::supported()) {
        SUCCEED("Lottie compiled in; the placeholder path is disabled-build only");
        return;
    }
    LottieView v;
    v.set_bounds({0, 0, 120, 90});

    pulp::canvas::RecordingCanvas rc;
    v.paint(rc);

    // A bordered placeholder + label drew — not a silent no-op.
    REQUIRE(rc.count(pulp::canvas::DrawCommand::Type::stroke_rect) >= 1);
    REQUIRE(rc.count(pulp::canvas::DrawCommand::Type::fill_text) >= 1);
    bool found_label = false;
    for (const auto& c : rc.commands()) {
        if (c.type == pulp::canvas::DrawCommand::Type::fill_text &&
            c.text.find("Lottie") != std::string::npos) {
            found_label = true;
        }
    }
    REQUIRE(found_label);
}

TEST_CASE("LottieView parses and advances when Lottie is compiled in",
          "[view][lottie]") {
    if (!LottieView::supported()) {
        SUCCEED("Lottie not compiled in (PULP_LOTTIE off)");
        return;
    }

    LottieView v;
    REQUIRE(v.set_source_json(kLottieJson));
    REQUIRE(v.valid());
    // 30 frames @ 30 fps == 1 second; authored at 100x100.
    REQUIRE(v.duration() > 0.99);
    REQUIRE(v.duration() < 1.01);

    SECTION("playhead advances while playing") {
        v.set_looping(true);
        v.set_speed(1.0f);
        const double before = v.time();
        v.advance(0.25f);
        REQUIRE(v.time() > before);
    }

    SECTION("looping wraps past the end") {
        v.set_looping(true);
        v.seek(v.duration() - 0.01);
        v.advance(0.5f);  // would overshoot the end
        REQUIRE(v.time() < v.duration());
    }

    SECTION("non-looping clamps and stops at the end") {
        v.set_looping(false);
        v.seek(v.duration() - 0.01);
        v.advance(0.5f);
        REQUIRE(v.time() == v.duration());
        REQUIRE_FALSE(v.playing());
    }

    SECTION("speed scales advance") {
        v.set_looping(true);
        v.seek(0.0);
        v.set_speed(2.0f);
        v.advance(0.1f);
        REQUIRE(v.time() > 0.19);  // ~0.2s of content for 0.1s real time
    }
}

TEST_CASE("LottieView resubscribes to the clock after pause/resume",
          "[view][lottie]") {
    if (!LottieView::supported()) {
        SUCCEED("Lottie not compiled in (PULP_LOTTIE off)");
        return;
    }
    // Regression: when the FrameClock auto-removes the subscription (pause, or a
    // non-looping animation reaching its end), the view must be able to
    // resubscribe on resume — otherwise it freezes.
    pulp::view::FrameClock clock;
    LottieView v;
    v.set_frame_clock(&clock);
    v.set_looping(true);
    REQUIRE(v.set_source_json(kLottieJson));  // subscribes (playing + valid)

    clock.tick(0.1f);
    const double t_playing = v.time();
    REQUIRE(t_playing > 0.0);

    v.set_playing(false);
    clock.tick(0.1f);  // lambda returns false → clock drops the subscription
    const double t_paused = v.time();
    clock.tick(0.1f);
    REQUIRE(v.time() == t_paused);  // genuinely paused

    v.set_playing(true);            // must resubscribe
    clock.tick(0.1f);
    REQUIRE(v.time() > t_paused);   // resumed — fails if resubscribe is blocked
}
