#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/sample_asset.hpp>
#include <pulp/audio/sample_stream_loop_voice_reader.hpp>

#include "support/sampler_paged_loop_parity.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace paged = pulp::test::sampler_paged_loop_parity;
namespace loop = pulp::test::sampler_loop_parity;
using pulp::audio::LoopCrossfadeCurve;
using pulp::audio::LoopInterpolationMode;
using pulp::audio::LoopPlaybackMode;
using pulp::audio::LoopRegion;

namespace {

LoopRegion region(LoopPlaybackMode mode,
                  std::uint64_t start,
                  std::uint64_t end,
                  LoopInterpolationMode interpolation =
                      LoopInterpolationMode::None) {
    LoopRegion result;
    result.start_frame = start;
    result.end_frame = end;
    result.source_sample_rate = 48000.0;
    result.playback_mode = mode;
    result.interpolation = interpolation;
    return result;
}

void require_taps(const paged::LoopInterpolationTaps& taps,
                  std::span<const std::uint64_t> expected) {
    REQUIRE(taps.count == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CAPTURE(index);
        REQUIRE(taps.source_frames[index] == expected[index]);
    }
}

paged::PagedLoopOracleConfig config(LoopRegion loop_region,
                                    double playback_rate,
                                    std::uint64_t output_frames,
                                    std::uint64_t total_source_frames,
                                    std::uint64_t page_frames) {
    return {
        .loop = {
            .region = loop_region,
            .playback_rate = playback_rate,
            .output_frames = output_frames,
        },
        .total_source_frames = total_source_frames,
        .page_frames = page_frames,
    };
}

}  // namespace

TEST_CASE("Paged loop oracle derives wrapped interpolation tap sets",
          "[audio][sampler][loop][page-oracle][interpolation]") {
    auto none_region = region(LoopPlaybackMode::Forward, 4, 8);
    const auto none = paged::make_interpolation_taps(none_region, 7.75);
    REQUIRE(none.has_value());
    constexpr std::array<std::uint64_t, 1> none_expected{7};
    require_taps(*none, none_expected);

    auto linear_region = none_region;
    linear_region.interpolation = LoopInterpolationMode::Linear;
    const auto linear = paged::make_interpolation_taps(linear_region, 7.75);
    REQUIRE(linear.has_value());
    constexpr std::array<std::uint64_t, 2> linear_expected{7, 4};
    require_taps(*linear, linear_expected);

    auto cubic_region = none_region;
    cubic_region.interpolation = LoopInterpolationMode::Cubic;
    const auto cubic = paged::make_interpolation_taps(cubic_region, 4.25);
    REQUIRE(cubic.has_value());
    constexpr std::array<std::uint64_t, 4> cubic_expected{7, 4, 5, 6};
    require_taps(*cubic, cubic_expected);
}

TEST_CASE("Paged loop oracle maps ReverseOnce taps and first-use urgency",
          "[audio][sampler][loop][page-oracle][reverse-once]") {
    const auto plan = paged::make_paged_loop_oracle_plan(config(
        region(LoopPlaybackMode::ReverseOnce, 8, 12), 1.0, 6, 16, 2));
    REQUIRE(plan.has_value());
    REQUIRE(plan->frames.size() == 6);
    REQUIRE(plan->frames[0].primary.source_frames[0] == 11);
    REQUIRE(plan->frames[3].primary.source_frames[0] == 8);
    REQUIRE_FALSE(plan->frames[4].active);
    REQUIRE(plan->required_page_demands == 2);
    REQUIRE(plan->page_first_uses[0].page_index == 5);
    REQUIRE(plan->page_first_uses[0].first_use_output_frame == 0);
    REQUIRE(plan->page_first_uses[0].first_source_frame == 11);
    REQUIRE(plan->page_first_uses[1].page_index == 4);
    REQUIRE(plan->page_first_uses[1].first_use_output_frame == 2);
    REQUIRE(plan->within_demand_capacity);
}

TEST_CASE("Paged loop oracle preserves Forward and Reverse first-use ordering",
          "[audio][sampler][loop][page-oracle][direction]") {
    auto forward_region = region(
        LoopPlaybackMode::Forward, 4, 8, LoopInterpolationMode::Linear);
    const auto forward = paged::make_paged_loop_oracle_plan(
        config(forward_region, 1.5, 4, 12, 2));
    REQUIRE(forward.has_value());
    REQUIRE(forward->required_page_demands == 2);
    REQUIRE(forward->page_first_uses[0].page_index == 2);
    REQUIRE(forward->page_first_uses[0].first_use_output_frame == 0);
    REQUIRE(forward->page_first_uses[1].page_index == 3);
    REQUIRE(forward->page_first_uses[1].first_use_output_frame == 1);
    constexpr std::array<std::uint64_t, 2> forward_wrap_taps{7, 4};
    require_taps(forward->frames[2].primary, forward_wrap_taps);

    auto reverse_region = forward_region;
    reverse_region.playback_mode = LoopPlaybackMode::Reverse;
    reverse_region.reverse_entry = true;
    const auto reverse = paged::make_paged_loop_oracle_plan(
        config(reverse_region, 1.5, 4, 12, 2));
    REQUIRE(reverse.has_value());
    REQUIRE(reverse->required_page_demands == 2);
    REQUIRE(reverse->page_first_uses[0].page_index == 3);
    REQUIRE(reverse->page_first_uses[0].first_use_output_frame == 0);
    REQUIRE(reverse->page_first_uses[1].page_index == 2);
    REQUIRE(reverse->page_first_uses[1].first_use_output_frame == 0);
    REQUIRE(reverse->page_first_uses[0].first_use_order <
            reverse->page_first_uses[1].first_use_order);
}

TEST_CASE("Paged loop oracle coalesces short high-rate wraps",
          "[audio][sampler][loop][page-oracle][high-rate]") {
    const auto plan = paged::make_paged_loop_oracle_plan(config(
        region(LoopPlaybackMode::Forward, 20, 24), 10.0, 5, 32, 1));
    REQUIRE(plan.has_value());
    REQUIRE(plan->required_page_demands == 2);
    REQUIRE(plan->page_first_uses[0].page_index == 20);
    REQUIRE(plan->page_first_uses[0].first_use_output_frame == 0);
    REQUIRE(plan->page_first_uses[1].page_index == 22);
    REQUIRE(plan->page_first_uses[1].first_use_output_frame == 1);

    auto reverse_region = region(LoopPlaybackMode::Reverse, 20, 24);
    reverse_region.reverse_entry = true;
    const auto reverse = paged::make_paged_loop_oracle_plan(
        config(reverse_region, 10.0, 5, 32, 1));
    REQUIRE(reverse.has_value());
    REQUIRE(reverse->required_page_demands == 2);
    REQUIRE(reverse->page_first_uses[0].page_index == 23);
    REQUIRE(reverse->page_first_uses[0].first_use_output_frame == 0);
    REQUIRE(reverse->page_first_uses[1].page_index == 21);
    REQUIRE(reverse->page_first_uses[1].first_use_output_frame == 1);
}

TEST_CASE("Paged loop oracle includes both crossfade regions",
          "[audio][sampler][loop][page-oracle][crossfade]") {
    auto forward_region = region(LoopPlaybackMode::Forward, 0, 8);
    forward_region.crossfade_frames = 3;
    forward_region.crossfade_curve = LoopCrossfadeCurve::Linear;
    const auto forward = paged::make_paged_loop_oracle_plan(
        config(forward_region, 3.0, 4, 8, 1));
    REQUIRE(forward.has_value());
    REQUIRE(forward->frames[1].has_blend);
    constexpr std::array<std::uint64_t, 1> forward_primary{3};
    constexpr std::array<std::uint64_t, 1> forward_blend{1};
    require_taps(forward->frames[1].primary, forward_primary);
    require_taps(forward->frames[1].blend, forward_blend);
    REQUIRE(forward->page_first_uses[2].page_index == 1);
    REQUIRE(forward->page_first_uses[2].first_use_output_frame == 1);
    REQUIRE(forward->page_first_uses[2].role == paged::LoopTapRole::Blend);

    auto reverse_region = region(LoopPlaybackMode::Reverse, 0, 8);
    reverse_region.reverse_entry = true;
    reverse_region.crossfade_frames = 3;
    reverse_region.crossfade_curve = LoopCrossfadeCurve::EqualPower;
    const auto reverse = paged::make_paged_loop_oracle_plan(
        config(reverse_region, 3.0, 4, 8, 1));
    REQUIRE(reverse.has_value());
    REQUIRE(reverse->frames[1].has_blend);
    constexpr std::array<std::uint64_t, 1> reverse_primary{4};
    constexpr std::array<std::uint64_t, 1> reverse_blend{6};
    require_taps(reverse->frames[1].primary, reverse_primary);
    require_taps(reverse->frames[1].blend, reverse_blend);
    REQUIRE(reverse->page_first_uses[2].page_index == 6);
    REQUIRE(reverse->page_first_uses[2].first_use_output_frame == 1);
    REQUIRE(reverse->page_first_uses[2].role == paged::LoopTapRole::Blend);
}

TEST_CASE("Paged loop oracle reports the 16-demand overflow boundary",
          "[audio][sampler][loop][page-oracle][capacity]") {
    auto at_capacity = config(
        region(LoopPlaybackMode::Forward, 0, 64), 1.0, 16, 64, 1);
    at_capacity.page_demand_capacity = 16;
    const auto accepted = paged::make_paged_loop_oracle_plan(at_capacity);
    REQUIRE(accepted.has_value());
    REQUIRE(accepted->required_page_demands == 16);
    REQUIRE(accepted->within_demand_capacity);
    REQUIRE(accepted->page_first_uses.back().page_index == 15);
    REQUIRE(accepted->page_first_uses.back().first_use_output_frame == 15);

    auto overflow = at_capacity;
    overflow.loop.output_frames = 17;
    const auto rejected = paged::make_paged_loop_oracle_plan(overflow);
    REQUIRE(rejected.has_value());
    REQUIRE(rejected->required_page_demands == 17);
    REQUIRE_FALSE(rejected->within_demand_capacity);
    REQUIRE(rejected->page_first_uses.back().page_index == 16);
    REQUIRE(rejected->page_first_uses.back().first_use_output_frame == 16);
}

TEST_CASE("Paged loop oracle matches production demand order and urgency",
          "[audio][sampler][loop][page-oracle][differential]") {
    pulp::audio::SampleStreamCacheService service;
    REQUIRE(service.prepare({
        .scheduler_capacity = 32,
        .page_memory_budget_bytes = 32 * sizeof(float),
    }));
    const auto added = service.add_source(
        {
            .token = {700, 1},
            .channels = 1,
            .total_frames = 64,
            .page_frames = 1,
            .cache_page_count = 32,
        },
        [](std::uint64_t, pulp::audio::BufferView<float> destination,
           std::uint64_t frames) {
            std::fill_n(destination.channel_ptr(0), frames, 0.0f);
            return frames;
        });
    REQUIRE(added.added());

    pulp::audio::Buffer<float> preload(1, 18);
    pulp::audio::SampleAsset asset;
    REQUIRE(asset.prepare(
        pulp::audio::SampleAssetConfig{
            .asset = {701, 1},
            .source = {700, 1},
            .channels = 1,
            .total_frames = 64,
            .sample_rate = 48000,
            .preload_frames = 18,
            .preload_contract = pulp::audio::SamplePreloadContract{
                .source_sample_rate = 48000.0,
                .host_sample_rate = 48000.0,
                .maximum_playback_ratio = 2.0,
                .maximum_host_block_frames = 8,
                .interpolation_guard_frames = 2,
                .configured_preload_frames = 18,
            },
            .stream_source = added.view,
        },
        preload.view()));

    auto loop_region = region(
        LoopPlaybackMode::Forward, 18, 64, LoopInterpolationMode::Linear);
    constexpr double playback_rate = 1.5;
    constexpr std::uint32_t output_frames = 8;
    auto oracle_config = config(
        loop_region, playback_rate, output_frames, 64, 1);
    oracle_config.resident_frames = 18;
    const auto oracle = paged::make_paged_loop_oracle_plan(oracle_config);
    REQUIRE(oracle.has_value());
    REQUIRE(oracle->within_demand_capacity);

    pulp::audio::SampleStreamLoopVoiceReader reader;
    const auto view = asset.view();
    REQUIRE(reader.prepare(view, {702, 1}, loop_region, playback_rate));
    const auto production = reader.plan_block(view, output_frames, 48000.0);
    REQUIRE(production.supply == pulp::audio::SampleStreamVoiceSupply::Ready);
    REQUIRE(production.demand_count == oracle->required_page_demands);
    for (std::uint32_t index = 0; index < production.demand_count; ++index) {
        CAPTURE(index);
        const auto& expected = oracle->page_first_uses[index];
        REQUIRE(production.demands[index].page_index == expected.page_index);
        REQUIRE(production.demands[index].resident_source_frames ==
                static_cast<std::uint64_t>(std::floor(
                    static_cast<double>(expected.first_use_output_frame) *
                    playback_rate)));
    }
}
