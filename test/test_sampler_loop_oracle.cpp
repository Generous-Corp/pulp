#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/sampler_loop_parity.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace oracle = pulp::test::sampler_loop_parity;
using pulp::audio::LoopCrossfadeCurve;
using pulp::audio::LoopInterpolationMode;
using pulp::audio::LoopPlaybackMode;
using pulp::audio::LoopRegion;

namespace {

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

void require_positions(const std::vector<oracle::LoopOracleTap>& schedule,
                       std::span<const double> expected) {
    REQUIRE(schedule.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        REQUIRE(schedule[i].active);
        REQUIRE(schedule[i].primary_position == expected[i]);
    }
}

}  // namespace

TEST_CASE("Loop oracle fixes forward reverse and two-phase coordinates",
          "[audio][sampler][loop][oracle]") {
    const auto forward = oracle::make_loop_oracle_schedule(
        {.region = region(LoopPlaybackMode::Forward), .output_frames = 6});
    REQUIRE(forward.has_value());
    constexpr std::array forward_expected{10.0, 11.0, 12.0, 13.0, 10.0, 11.0};
    require_positions(*forward, forward_expected);

    auto reverse_region = region(LoopPlaybackMode::Reverse);
    reverse_region.reverse_entry = true;
    const auto reverse = oracle::make_loop_oracle_schedule(
        {.region = reverse_region, .output_frames = 6});
    REQUIRE(reverse.has_value());
    constexpr std::array reverse_expected{13.0, 12.0, 11.0, 10.0, 13.0, 12.0};
    require_positions(*reverse, reverse_expected);

    reverse_region.reverse_entry = false;
    const auto forward_then_reverse = oracle::make_loop_oracle_schedule(
        {.region = reverse_region, .output_frames = 8});
    REQUIRE(forward_then_reverse.has_value());
    constexpr std::array forward_then_reverse_expected{
        10.0, 11.0, 12.0, 13.0, 12.0, 11.0, 10.0, 13.0};
    require_positions(*forward_then_reverse, forward_then_reverse_expected);

    auto forward_region = region(LoopPlaybackMode::Forward);
    forward_region.reverse_entry = true;
    const auto reverse_then_forward = oracle::make_loop_oracle_schedule(
        {.region = forward_region, .output_frames = 8});
    REQUIRE(reverse_then_forward.has_value());
    constexpr std::array reverse_then_forward_expected{
        13.0, 12.0, 11.0, 10.0, 11.0, 12.0, 13.0, 10.0};
    require_positions(*reverse_then_forward, reverse_then_forward_expected);

    reverse_region.reverse_entry = false;
    const auto high_rate_forward_then_reverse = oracle::make_loop_oracle_schedule(
        {.region = reverse_region,
         .playback_rate = 3.0,
         .output_frames = 5});
    REQUIRE(high_rate_forward_then_reverse.has_value());
    constexpr std::array high_rate_forward_then_reverse_expected{
        10.0, 13.0, 10.0, 11.0, 12.0};
    require_positions(*high_rate_forward_then_reverse,
                      high_rate_forward_then_reverse_expected);

    forward_region.reverse_entry = true;
    const auto high_rate_reverse_then_forward = oracle::make_loop_oracle_schedule(
        {.region = forward_region,
         .playback_rate = 3.0,
         .output_frames = 5});
    REQUIRE(high_rate_reverse_then_forward.has_value());
    constexpr std::array high_rate_reverse_then_forward_expected{
        13.0, 10.0, 13.0, 12.0, 11.0};
    require_positions(*high_rate_reverse_then_forward,
                      high_rate_reverse_then_forward_expected);
}

TEST_CASE("Loop oracle fixes ReverseOnce stop and PingPong reflection",
          "[audio][sampler][loop][oracle]") {
    const auto reverse_once = oracle::make_loop_oracle_schedule(
        {.region = region(LoopPlaybackMode::ReverseOnce), .output_frames = 6});
    REQUIRE(reverse_once.has_value());
    constexpr std::array reverse_once_expected{13.0, 12.0, 11.0, 10.0};
    require_positions(
        std::vector<oracle::LoopOracleTap>(reverse_once->begin(), reverse_once->begin() + 4),
        reverse_once_expected);
    REQUIRE_FALSE((*reverse_once)[4].active);
    REQUIRE_FALSE((*reverse_once)[5].active);

    const auto ping_pong = oracle::make_loop_oracle_schedule(
        {.region = region(LoopPlaybackMode::PingPong), .output_frames = 9});
    REQUIRE(ping_pong.has_value());
    constexpr std::array ping_pong_expected{
        10.0, 11.0, 12.0, 13.0, 12.0, 11.0, 10.0, 11.0, 12.0};
    require_positions(*ping_pong, ping_pong_expected);
    REQUIRE((*ping_pong)[3].boundary_crossings_after == 1);
    REQUIRE((*ping_pong)[6].boundary_crossings_after == 1);
}

TEST_CASE("Loop oracle preserves short-loop coordinates at high rates",
          "[audio][sampler][loop][oracle][high-rate]") {
    const auto schedule = oracle::make_loop_oracle_schedule(
        {.region = region(LoopPlaybackMode::Forward, 20, 24),
         .playback_rate = 10.0,
         .output_frames = 5});
    REQUIRE(schedule.has_value());
    constexpr std::array expected{20.0, 22.0, 20.0, 22.0, 20.0};
    require_positions(*schedule, expected);
    REQUIRE((*schedule)[0].boundary_crossings_after == 2);
    REQUIRE((*schedule)[1].boundary_crossings_after == 3);

    auto reverse_region = region(LoopPlaybackMode::Reverse, 20, 24);
    reverse_region.reverse_entry = true;
    const auto reverse = oracle::make_loop_oracle_schedule(
        {.region = reverse_region,
         .playback_rate = 8.0,
         .output_frames = 3});
    REQUIRE(reverse.has_value());
    constexpr std::array reverse_expected{23.0, 23.0, 23.0};
    require_positions(*reverse, reverse_expected);
    REQUIRE((*reverse)[0].boundary_crossings_after == 2);
    REQUIRE((*reverse)[1].boundary_crossings_after == 2);
}

TEST_CASE("Loop oracle derives linear and equal-power wrap gains",
          "[audio][sampler][loop][oracle][crossfade]") {
    auto linear_region = region(LoopPlaybackMode::Forward, 0, 8);
    linear_region.crossfade_frames = 4;
    linear_region.crossfade_curve = LoopCrossfadeCurve::Linear;

    const auto forward_mid =
        oracle::make_loop_crossfade_plan(linear_region, 6.0, 1.0, true);
    REQUIRE(forward_mid.blend);
    REQUIRE(forward_mid.primary_position == 6.0);
    REQUIRE(forward_mid.blend_position == 2.0);
    REQUIRE(forward_mid.primary_gain == 0.5);
    REQUIRE(forward_mid.blend_gain == 0.5);

    const auto entry_plan =
        oracle::make_loop_crossfade_plan(linear_region, 6.0, -1.0, false);
    REQUIRE_FALSE(entry_plan.blend);

    auto equal_power_region = linear_region;
    equal_power_region.playback_mode = LoopPlaybackMode::Reverse;
    equal_power_region.crossfade_curve = LoopCrossfadeCurve::EqualPower;
    const auto reverse_mid =
        oracle::make_loop_crossfade_plan(equal_power_region, 2.0, -1.0, true);
    REQUIRE(reverse_mid.blend);
    REQUIRE(reverse_mid.primary_position == 2.0);
    REQUIRE(reverse_mid.blend_position == 6.0);
    REQUIRE(reverse_mid.primary_gain == Catch::Approx(std::sqrt(0.5)).epsilon(1.0e-12));
    REQUIRE(reverse_mid.blend_gain == Catch::Approx(std::sqrt(0.5)).epsilon(1.0e-12));

    equal_power_region.playback_mode = LoopPlaybackMode::PingPong;
    const auto reflected =
        oracle::make_loop_crossfade_plan(equal_power_region, 2.0, -1.0, true);
    REQUIRE_FALSE(reflected.blend);
}

TEST_CASE("Loop oracle probes high-rate crossfade zone entry",
          "[audio][sampler][loop][oracle][crossfade][high-rate]") {
    auto forward_region = region(LoopPlaybackMode::Forward, 0, 8);
    forward_region.crossfade_frames = 2;
    forward_region.crossfade_curve = LoopCrossfadeCurve::Linear;
    const auto forward =
        oracle::make_loop_crossfade_plan(forward_region, 4.0, 3.0, true);
    REQUIRE(forward.blend);
    REQUIRE(forward.primary_position == 4.0);
    REQUIRE(forward.blend_position == 1.0);
    REQUIRE(forward.primary_gain == 0.5);
    REQUIRE(forward.blend_gain == 0.5);

    auto reverse_region = forward_region;
    reverse_region.playback_mode = LoopPlaybackMode::Reverse;
    reverse_region.crossfade_curve = LoopCrossfadeCurve::EqualPower;
    const auto reverse =
        oracle::make_loop_crossfade_plan(reverse_region, 4.0, -3.0, true);
    REQUIRE(reverse.blend);
    REQUIRE(reverse.primary_position == 4.0);
    REQUIRE(reverse.blend_position == 7.0);
    REQUIRE(reverse.primary_gain == Catch::Approx(std::sqrt(0.5)).epsilon(1.0e-12));
    REQUIRE(reverse.blend_gain == Catch::Approx(std::sqrt(0.5)).epsilon(1.0e-12));

    const auto wrong_direction =
        oracle::make_loop_crossfade_plan(reverse_region, 4.0, 3.0, true);
    REQUIRE_FALSE(wrong_direction.blend);
}

TEST_CASE("Loop oracle rejects invalid standalone configurations",
          "[audio][sampler][loop][oracle]") {
    auto invalid = region(LoopPlaybackMode::Forward);
    invalid.crossfade_frames = 3;
    REQUIRE_FALSE(oracle::make_loop_oracle_schedule(
                      {.region = invalid, .output_frames = 1})
                      .has_value());
    REQUIRE_FALSE(oracle::make_loop_oracle_schedule(
                      {.region = region(LoopPlaybackMode::Forward),
                       .playback_rate = 0.0,
                       .output_frames = 1})
                      .has_value());
    auto invalid_rate = region(LoopPlaybackMode::Forward);
    invalid_rate.source_sample_rate = 0.0;
    REQUIRE_FALSE(oracle::make_loop_oracle_schedule(
                      {.region = invalid_rate, .output_frames = 1})
                      .has_value());
}
