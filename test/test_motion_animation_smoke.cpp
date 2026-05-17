/// @file test_motion_animation_smoke.cpp
/// End-to-end smoke that mirrors a real plugin scenario:
///   1. Create a Tween from Pulp's animation.hpp.
///   2. Bind the motion coordinator to a FrameClock.
///   3. Install a fixture sink that writes JSONL to disk.
///   4. Drive ticks at 60 FPS, advance the tween, publish the value.
///   5. Load the fixture back from disk, extract the scalar series,
///      run the assertion helpers, and verify the shape matches the
///      configured animation.
///
/// This proves end-to-end that a plugin author can wire a real
/// animation into the motion observability system without any
/// hand-rolled instrumentation glue.

#include <pulp/view/animation.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/motion.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#if defined(_WIN32)
#include <process.h>
#define pulp_smoke_getpid() static_cast<long>(::_getpid())
#else
#include <unistd.h>
#define pulp_smoke_getpid() static_cast<long>(::getpid())
#endif

using Catch::Approx;
using pulp::view::FrameClock;
using pulp::view::Tween;
namespace easing = pulp::view::easing;
using namespace pulp::view::motion;

namespace {

std::string smoke_fixture_path() {
    // Cross-platform temp dir: TMPDIR / TMP / TEMP, fallback to /tmp
    // (POSIX) or current dir (Windows when none set).
    const char* tmp = std::getenv("TMPDIR");
    if (!tmp) tmp = std::getenv("TMP");
    if (!tmp) tmp = std::getenv("TEMP");
    if (!tmp) {
#if defined(_WIN32)
        tmp = ".";
#else
        tmp = "/tmp";
#endif
    }
    std::ostringstream ss;
    ss << tmp << "/pulp-motion-anim-smoke-"
       << pulp_smoke_getpid() << "-"
       << std::rand() << ".jsonl";
    return ss.str();
}

}  // namespace

TEST_CASE("End-to-end: real Tween → publish_value → fixture → assertion helpers",
          "[motion][smoke][animation]") {
    Coordinator::instance().reset();

    FrameClock clock;
    Coordinator::instance().bind(clock);
    Coordinator::instance().set_tracing_enabled(true);
    Coordinator::instance().set_firehose(true);

    const auto fixture_path = smoke_fixture_path();
    Coordinator::instance().add_sink(make_fixture_sink(fixture_path));

    // The "plugin animation": opacity tween from 0 → 1 over 500 ms
    // with ease_out_cubic. Mirrors what a Knob hover-glow or a
    // dropdown fade-in would look like inside an editor.
    constexpr float duration_s = 0.5f;
    Tween tween(/*from=*/0.0f, /*to=*/1.0f, duration_s,
                easing::ease_out_cubic);

    // Drive ticks at 60 FPS until the tween settles plus a few
    // stable ticks so the burst End fires.
    constexpr float fps = 60.0f;
    constexpr float dt = 1.0f / fps;
    constexpr int extra_settle_ticks = 5;
    const int total_ticks =
        static_cast<int>(std::ceil(duration_s * fps)) + extra_settle_ticks;

    for (int i = 0; i < total_ticks; ++i) {
        const float v = tween.advance(dt);
        PublishOptions po{};
        po.precision = 3;
        po.epsilon = 0.001;
        publish_value("Gain Knob", "opacity",
                      static_cast<double>(v), po);
        clock.tick(dt);
    }

    // Drop the fixture sink so the file closes cleanly before we read.
    Coordinator::instance().reset();

    // ── Read it back ──────────────────────────────────────────────
    auto events = load_fixture(fixture_path);
    REQUIRE_FALSE(events.empty());

    // Extract the scalar series and run assertion helpers.
    auto series = extract_scalar(events, "Gain Knob", "opacity", "value");
    REQUIRE(series.size() >= 5);

    // The tween is strictly monotonic (ease_out_cubic is monotonic).
    REQUIRE(is_monotonic(series));

    // Final value should land at the tween's target.
    REQUIRE(final_value(series) == Approx(1.0).margin(0.001));

    // Settling time should be close to the configured tween duration.
    // Allow a tolerance because the first non-zero change happens on
    // the first tick where the publish epsilon (0.001) is crossed —
    // that's at t ≈ 1/60 ≈ 0.017 s into the animation, not t=0.
    const double settling = settling_time_seconds(series);
    REQUIRE(settling > duration_s * 0.7);
    REQUIRE(settling < duration_s * 1.3);

    // ease_out_cubic doesn't overshoot.
    REQUIRE(overshoot(series) < 0.01);

    // Frame jitter on a FrameClock driven at a constant dt should be 0.
    REQUIRE(frame_jitter_seconds(series) == Approx(0.0).margin(1e-6));

    // ── Burst framing checks ─────────────────────────────────────
    std::size_t baseline = 0, start = 0, sample = 0, end = 0;
    for (const auto& e : events) {
        if (e.kind == SampleEvent::Kind::Baseline) ++baseline;
        if (e.kind == SampleEvent::Kind::Start)    ++start;
        if (e.kind == SampleEvent::Kind::Sample)   ++sample;
        if (e.kind == SampleEvent::Kind::End)      ++end;
    }
    REQUIRE(baseline == 1);
    REQUIRE(start == 1);
    REQUIRE(sample >= 5);   // many intermediate samples
    REQUIRE(end == 1);      // burst closed after tween settled

    // ── Stable IDs survived the round trip ───────────────────────
    for (const auto& e : events) {
        // Publish channel uses trace_id = 0 (reserved) per Phase 7.
        REQUIRE(e.trace_id == 0);
        REQUIRE(e.metric_id == 0);
        if (e.kind == SampleEvent::Kind::Start ||
            e.kind == SampleEvent::Kind::Sample ||
            e.kind == SampleEvent::Kind::End) {
            REQUIRE(e.burst_id >= 1);
        }
    }

    // Print a human-readable summary so the developer running this
    // by hand can see the data flowed end to end.
    std::cout << "\n[motion smoke] fixture: " << fixture_path << "\n"
              << "[motion smoke] events=" << events.size()
              << " baseline=" << baseline
              << " start=" << start
              << " sample=" << sample
              << " end=" << end << "\n"
              << "[motion smoke] settling_time=" << settling << "s"
              << " final_value=" << final_value(series)
              << " overshoot=" << overshoot(series)
              << " jitter=" << frame_jitter_seconds(series) << "s"
              << " monotonic=" << (is_monotonic(series) ? "yes" : "no") << "\n";

    std::remove(fixture_path.c_str());
}
