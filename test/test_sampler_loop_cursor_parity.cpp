#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/sampler_loop_parity.hpp"

#include <pulp/audio/loop_playback_cursor.hpp>

#include <array>
#include <cmath>
#include <cstdint>

namespace oracle = pulp::test::sampler_loop_parity;
using pulp::audio::LoopCrossfadeCurve;
using pulp::audio::LoopInterpolationMode;
using pulp::audio::LoopPlaybackCursor;
using pulp::audio::LoopPlaybackMode;
using pulp::audio::LoopRegion;

namespace {

struct CursorParityScenario {
    const char* name = "";
    LoopRegion region;
    double playback_rate = 1.0;
    std::uint64_t output_frames = 0;
    bool expect_multiple_crossings = false;
    bool expect_probe_blend = false;
};

LoopRegion region(LoopPlaybackMode mode,
                  std::uint64_t start = 10,
                  std::uint64_t end = 14) {
    LoopRegion result;
    result.start_frame = start;
    result.end_frame = end;
    result.source_sample_rate = 48000.0;
    result.playback_mode = mode;
    result.interpolation = LoopInterpolationMode::None;
    return result;
}

void require_near(double actual, double expected) {
    REQUIRE(actual == Catch::Approx(expected).margin(1.0e-12));
}

void require_cursor_parity(const CursorParityScenario& scenario,
                           bool& saw_blend_without_advance_wrap) {
    INFO(scenario.name);
    const auto expected = oracle::make_loop_oracle_schedule({
        .region = scenario.region,
        .playback_rate = scenario.playback_rate,
        .output_frames = scenario.output_frames + 1,
    });
    REQUIRE(expected.has_value());

    LoopPlaybackCursor cursor;
    REQUIRE(cursor.set_region(scenario.region, scenario.region.end_frame));
    cursor.set_playback_rate(scenario.playback_rate);
    cursor.start();

    bool saw_multiple_crossings = false;
    bool saw_probe_blend = false;

    for (std::uint64_t frame = 0; frame < scenario.output_frames; ++frame) {
        CAPTURE(frame);
        const auto& tap = (*expected)[static_cast<std::size_t>(frame)];
        const auto& next_tap = (*expected)[static_cast<std::size_t>(frame + 1)];

        REQUIRE(cursor.active() == tap.active);
        if (!tap.active) continue;

        require_near(cursor.position(), tap.primary_position);
        REQUIRE((cursor.step() < 0.0 ? -1 : 1) == tap.direction);

        const auto plan = cursor.frame_read_plan();
        require_near(plan.read_position, tap.primary_position);
        REQUIRE(plan.blend == tap.blend);
        REQUIRE(plan.wrapped == tap.blend);
        require_near(plan.primary_gain, tap.primary_gain);
        require_near(plan.blend_gain, tap.blend_gain);
        if (tap.blend) require_near(plan.blend_position, tap.blend_position);
        const double fade_start = static_cast<double>(scenario.region.end_frame -
                                                       scenario.region.crossfade_frames);
        const double fade_end = static_cast<double>(scenario.region.start_frame +
                                                     scenario.region.crossfade_frames);
        if (tap.blend &&
            ((tap.direction > 0 && tap.primary_position < fade_start) ||
             (tap.direction < 0 && tap.primary_position >= fade_end))) {
            saw_probe_blend = true;
        }

        const auto advanced = cursor.advance();
        const bool crossed_boundary = tap.boundary_crossings_after > 0;
        REQUIRE(advanced.wrapped == crossed_boundary);
        REQUIRE(advanced.active == next_tap.active);
        if (tap.boundary_crossings_after > 1) saw_multiple_crossings = true;
        if (plan.wrapped && !advanced.wrapped) {
            saw_blend_without_advance_wrap = true;
        }

        if (next_tap.active) {
            require_near(cursor.position(), next_tap.primary_position);
            REQUIRE((cursor.step() < 0.0 ? -1 : 1) == next_tap.direction);
        }
    }
    if (scenario.expect_multiple_crossings) REQUIRE(saw_multiple_crossings);
    if (scenario.expect_probe_blend) REQUIRE(saw_probe_blend);
}

}  // namespace

TEST_CASE("LoopPlaybackCursor matches the independent sampler loop oracle",
          "[audio][sampler][loop][cursor][parity]") {
    auto reverse = region(LoopPlaybackMode::Reverse);
    reverse.reverse_entry = true;

    auto ping_pong = region(LoopPlaybackMode::PingPong, 20, 24);

    auto forward_crossfade = region(LoopPlaybackMode::Forward, 0, 8);
    forward_crossfade.crossfade_frames = 3;
    forward_crossfade.crossfade_curve = LoopCrossfadeCurve::Linear;

    auto reverse_crossfade = region(LoopPlaybackMode::Reverse, 0, 8);
    reverse_crossfade.reverse_entry = true;
    reverse_crossfade.crossfade_frames = 3;
    reverse_crossfade.crossfade_curve = LoopCrossfadeCurve::EqualPower;

    auto forward_then_reverse = region(LoopPlaybackMode::Reverse);
    forward_then_reverse.reverse_entry = false;

    auto reverse_then_forward = region(LoopPlaybackMode::Forward);
    reverse_then_forward.reverse_entry = true;

    const std::array scenarios{
        CursorParityScenario{
            "ReverseOnce stops after the reverse entry",
            region(LoopPlaybackMode::ReverseOnce),
            1.0,
            6,
        },
        CursorParityScenario{
            "short Forward loop normalizes multiple wraps per advance",
            region(LoopPlaybackMode::Forward, 20, 24),
            10.0,
            8,
            true,
        },
        CursorParityScenario{
            "short Reverse loop normalizes multiple wraps per advance",
            reverse,
            9.0,
            8,
            true,
        },
        CursorParityScenario{
            "PingPong folds across multiple boundaries per advance",
            ping_pong,
            10.0,
            8,
            true,
        },
        CursorParityScenario{
            "Forward linear crossfade probes a high-rate fade-zone entry",
            forward_crossfade,
            3.0,
            10,
            false,
            true,
        },
        CursorParityScenario{
            "Reverse equal-power crossfade probes a high-rate fade-zone entry",
            reverse_crossfade,
            3.0,
            10,
            false,
            true,
        },
        CursorParityScenario{
            "oversized forward entry carries into the steady Reverse loop",
            forward_then_reverse,
            10.0,
            8,
            true,
        },
        CursorParityScenario{
            "oversized reverse entry carries into the steady Forward loop",
            reverse_then_forward,
            10.0,
            8,
            true,
        },
    };

    bool saw_blend_without_advance_wrap = false;
    for (const auto& scenario : scenarios) {
        require_cursor_parity(scenario, saw_blend_without_advance_wrap);
    }
    REQUIRE(saw_blend_without_advance_wrap);
}
