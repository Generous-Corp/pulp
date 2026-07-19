#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/loop_renderer.hpp>
#include <pulp/audio/sample_stream_loop_voice_reader.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace pulp::audio;

namespace {

struct LoopStreamFixture {
    SampleStreamCacheService service;
    Buffer<float> preload{1, 48};
    Buffer<float> resident{1, 64};
    SampleAsset asset;

    LoopStreamFixture() {
        REQUIRE(service.prepare({
            .scheduler_capacity = 32,
            .page_memory_budget_bytes = 7 * 4 * sizeof(float),
        }));
        const auto added = service.add_source(
            {
                .token = {200, 1},
                .channels = 1,
                .total_frames = 64,
                .page_frames = 4,
                .cache_page_count = 7,
            },
            [](std::uint64_t start, BufferView<float> destination,
               std::uint64_t frames) {
                for (std::uint64_t frame = 0; frame < frames; ++frame) {
                    destination.channel_ptr(0)[frame] =
                        static_cast<float>(((start + frame) * 37 + 11) % 101) / 53.0f - 1.0f;
                }
                return frames;
            });
        REQUIRE(added.added());
        for (std::uint64_t frame = 0; frame < 64; ++frame) {
            resident.channel(0)[frame] =
                static_cast<float>((frame * 37 + 11) % 101) / 53.0f - 1.0f;
            if (frame < 48) preload.channel(0)[frame] = resident.channel(0)[frame];
        }
        const SampleAssetConfig config{
            .asset = {100, 1},
            .source = {200, 1},
            .channels = 1,
            .total_frames = 64,
            .sample_rate = 48000,
            .preload_frames = 48,
            .preload_contract = SamplePreloadContract{
                .source_sample_rate = 48000.0,
                .host_sample_rate = 48000.0,
                .maximum_playback_ratio = 4.0,
                .maximum_host_block_frames = 8,
                .interpolation_guard_frames = kDefaultSampleSincHalfWidth,
                .loop_prefetch_guard_frames = 0,
                .configured_preload_frames = 48,
            },
            .stream_source = added.view,
        };
        REQUIRE(asset.prepare(config, preload.view()));
    }

    void publish_pages() {
        for (std::uint64_t page = 9; page < 16; ++page) {
            REQUIRE(service.request_page({
                .source = {200, 1},
                .requester = {900, 1},
                .page_index = page,
                .consumption_frames_per_second = 192000.0,
            }) == SampleStreamScheduleStatus::Inserted);
            REQUIRE(service.service_once() == SampleStreamServiceStatus::Published);
        }
    }
};

LoopRegion region(LoopPlaybackMode mode, bool reverse_entry = false) {
    LoopRegion loop;
    loop.start_frame = 0;
    loop.end_frame = 64;
    loop.crossfade_frames = mode == LoopPlaybackMode::Forward ||
                                    mode == LoopPlaybackMode::Reverse
        ? 8
        : 0;
    loop.source_sample_rate = 48000.0;
    loop.playback_mode = mode;
    loop.crossfade_curve = LoopCrossfadeCurve::EqualPower;
    loop.interpolation = LoopInterpolationMode::Linear;
    loop.reverse_entry = reverse_entry;
    return loop;
}

void require_matches_resident(
    LoopPlaybackMode mode,
    bool reverse_entry,
    PreparedSampleInterpolation interpolation = {
        .policy = SampleInterpolationPolicy::Linear},
    double playback_rate = 4.0) {
    LoopStreamFixture fixture;
    fixture.publish_pages();
    const auto asset = fixture.asset.view();
    const auto loop = region(mode, reverse_entry);

    SampleStreamLoopVoiceReader streamed;
    REQUIRE(streamed.prepare(asset, {1, 1}, loop, playback_rate, interpolation));
    LoopRenderer resident;
    REQUIRE(resident.set_region(loop, 64));
    REQUIRE(resident.set_interpolation(interpolation));
    resident.set_playback_rate(playback_rate);
    resident.start();

    const std::array<const float*, 1> input_ptrs{fixture.resident.channel(0).data()};
    BufferView<const float> input(input_ptrs.data(), 1, 64);
    for (int block = 0; block < 5; ++block) {
        Buffer<float> actual(1, 8);
        Buffer<float> expected(1, 8);
        const auto plan = streamed.plan_block(asset, 8, 48000.0);
        REQUIRE(plan.supply == SampleStreamVoiceSupply::Ready);
        const auto result = streamed.render_block(asset, plan, actual.view());
        const auto reference = resident.render(input, expected.view(), 8);
        REQUIRE(result.ready_output_frames == 8);
        REQUIRE(result.supply == (reference.active
            ? SampleStreamVoiceSupply::Ready
            : SampleStreamVoiceSupply::EndOfSource));
        for (std::size_t frame = 0; frame < 8; ++frame)
            REQUIRE(actual.channel(0)[frame] == expected.channel(0)[frame]);
        if (!reference.active) break;
    }
    const auto stats = streamed.starvation_stats();
    REQUIRE(stats.predicted_events == 0);
    REQUIRE(stats.insufficient_lead_events == 0);
    REQUIRE(stats.emergency_events == 0);
    REQUIRE(stats.starved_frames == 0);
}

}  // namespace

TEST_CASE("Loop stream demand declaration is independent of cursor geometry",
          "[audio][sampler][stream-loop][contract][demand]") {
    LoopStreamFixture fixture;
    const auto asset = fixture.asset.view();
    auto streamed_region = region(LoopPlaybackMode::OneShot);
    streamed_region.start_frame = 48;
    SampleStreamLoopVoiceReader reader;
    REQUIRE(reader.prepare(asset, {8, 1}, streamed_region, 1.0));

    const auto declared = reader.plan_block(
        asset, 8, 48000.0,
        {.source_frames_per_second = 96000.0});
    REQUIRE(declared.supply == SampleStreamVoiceSupply::Ready);
    REQUIRE(declared.demand_count != 0);
    REQUIRE(declared.demands[0].consumption_frames_per_second == 96000.0);

    const auto over_cap = reader.plan_block(
        asset, 8, 48000.0,
        {.source_frames_per_second = 192001.0});
    REQUIRE(over_cap.supply == SampleStreamVoiceSupply::InvalidContract);
    REQUIRE(over_cap.demand_count == 0);
}

TEST_CASE("Paged loop reader matches resident forward crossfade traversal",
          "[audio][sampler][stream-loop]") {
    require_matches_resident(LoopPlaybackMode::Forward, false);
}

TEST_CASE("Paged loop reader matches resident reverse crossfade traversal",
          "[audio][sampler][stream-loop]") {
    require_matches_resident(LoopPlaybackMode::Reverse, true);
}

TEST_CASE("Paged loop reader matches resident reverse one-shot traversal",
          "[audio][sampler][stream-loop]") {
    require_matches_resident(LoopPlaybackMode::ReverseOnce, true);
}

TEST_CASE("Paged loop reader matches resident nearest and Lagrange policies",
          "[audio][sampler][stream-loop][interpolation]") {
    require_matches_resident(LoopPlaybackMode::Forward, false,
                             {.policy = SampleInterpolationPolicy::Nearest}, 1.25);
    require_matches_resident(LoopPlaybackMode::Forward, false,
                             {.policy = SampleInterpolationPolicy::CubicLagrange}, 1.25);
}

TEST_CASE("Paged loop reader matches resident ratio-tracking sinc",
          "[audio][sampler][stream-loop][interpolation][sinc]") {
    SampleSincKernelBank bank;
    REQUIRE(bank.build(4));
    const PreparedSampleInterpolation interpolation{
        .policy = SampleInterpolationPolicy::RatioTrackingSinc,
        .sinc = bank.view().select(1.25),
    };
    require_matches_resident(LoopPlaybackMode::OneShot, false,
                             interpolation, 1.25);
}

TEST_CASE("Paged sinc matches resident forward and reverse crossfade seams",
          "[audio][sampler][stream-loop][interpolation][sinc][crossfade]") {
    SampleSincKernelBank bank;
    REQUIRE(bank.build(4));
    const PreparedSampleInterpolation interpolation{
        .policy = SampleInterpolationPolicy::RatioTrackingSinc,
        .sinc = bank.view().select(1.25),
    };
    require_matches_resident(LoopPlaybackMode::Forward, false,
                             interpolation, 1.25);
    require_matches_resident(LoopPlaybackMode::Reverse, true,
                             interpolation, 1.25);
    require_matches_resident(LoopPlaybackMode::ReverseOnce, true,
                             interpolation, 1.25);
}

TEST_CASE("Paged sinc rejects a preload contract with a narrow tap guard",
          "[audio][sampler][stream-loop][interpolation][sinc][contract]") {
    LoopStreamFixture fixture;
    const auto source = fixture.asset.view();
    SampleAsset narrow_asset;
    REQUIRE(narrow_asset.prepare(
        SampleAssetConfig{
            .asset = {101, 1},
            .source = source.source,
            .channels = 1,
            .total_frames = 64,
            .sample_rate = 48000,
            .preload_frames = 48,
            .preload_contract = SamplePreloadContract{
                .source_sample_rate = 48000.0,
                .host_sample_rate = 48000.0,
                .maximum_playback_ratio = 4.0,
                .maximum_host_block_frames = 8,
                .interpolation_guard_frames = 2,
                .configured_preload_frames = 48,
            },
            .stream_source = source.stream_source,
        },
        fixture.preload.view()));

    SampleSincKernelBank bank;
    REQUIRE(bank.build(4));
    const PreparedSampleInterpolation interpolation{
        .policy = SampleInterpolationPolicy::RatioTrackingSinc,
        .sinc = bank.view().select(1.25),
    };
    SampleStreamLoopVoiceReader reader;
    const auto asset = narrow_asset.view();
    REQUIRE(reader.prepare(asset, {2, 1}, region(LoopPlaybackMode::OneShot),
                           1.25, interpolation));
    REQUIRE(reader.plan_block(asset, 8, 48000.0).supply ==
            SampleStreamVoiceSupply::InvalidContract);
}

TEST_CASE("Paged sinc rejects an over-capacity page geometry before note start",
          "[audio][sampler][stream-loop][interpolation][sinc][contract]") {
    SampleStreamCacheService service;
    REQUIRE(service.prepare({
        .scheduler_capacity = 32,
        .page_memory_budget_bytes = 17 * sizeof(float),
    }));
    const auto added = service.add_source(
        {
            .token = {300, 1},
            .channels = 1,
            .total_frames = 82,
            .page_frames = 1,
            .cache_page_count = 17,
        },
        [](std::uint64_t start, BufferView<float> destination,
           std::uint64_t frames) {
            for (std::uint64_t frame = 0; frame < frames; ++frame)
                destination.channel_ptr(0)[frame] =
                    static_cast<float>(start + frame);
            return frames;
        });
    REQUIRE(added.added());

    Buffer<float> preload(1, 65);
    SampleAsset asset_owner;
    REQUIRE(asset_owner.prepare(
        SampleAssetConfig{
            .asset = {301, 1},
            .source = {300, 1},
            .channels = 1,
            .total_frames = 82,
            .sample_rate = 48000,
            .preload_frames = 65,
            .preload_contract = SamplePreloadContract{
                .source_sample_rate = 48000.0,
                .host_sample_rate = 48000.0,
                .maximum_playback_ratio = 1.0,
                .maximum_host_block_frames = 1,
                .interpolation_guard_frames = 64,
                .configured_preload_frames = 65,
            },
            .stream_source = added.view,
        },
        preload.view()));

    SampleSincKernelBank bank;
    REQUIRE(bank.build(1, 64, 16));
    const PreparedSampleInterpolation interpolation{
        .policy = SampleInterpolationPolicy::RatioTrackingSinc,
        .sinc = bank.view().select(1.0),
    };
    auto resident_region = region(LoopPlaybackMode::OneShot);
    resident_region.end_frame = 64;
    SampleStreamLoopVoiceReader resident_reader;
    REQUIRE(resident_reader.prepare(asset_owner.view(), {3, 2},
                                    resident_region, 1.0, interpolation));
    REQUIRE(resident_reader.plan_block(asset_owner.view(), 1, 48000.0)
                .demand_count == 0);

    auto one_shot = region(LoopPlaybackMode::OneShot);
    one_shot.end_frame = 82;
    SampleStreamLoopVoiceReader reader;
    REQUIRE_FALSE(reader.prepare(asset_owner.view(), {3, 1}, one_shot,
                                 1.0, interpolation));
}

TEST_CASE("Paged loop playback-rate changes preserve page-capacity admission",
          "[audio][sampler][stream-loop][contract]") {
    SampleStreamCacheService service;
    REQUIRE(service.prepare({
        .scheduler_capacity = 32,
        .page_memory_budget_bytes = 7 * 4 * sizeof(float),
    }));
    const auto added = service.add_source(
        {
            .token = {400, 1},
            .channels = 1,
            .total_frames = 256,
            .page_frames = 4,
            .cache_page_count = 7,
        },
        [](std::uint64_t, BufferView<float>, std::uint64_t frames) {
            return frames;
        });
    REQUIRE(added.added());
    Buffer<float> preload(1, 48);
    SampleAsset asset_owner;
    REQUIRE(asset_owner.prepare(
        SampleAssetConfig{
            .asset = {401, 1},
            .source = {400, 1},
            .channels = 1,
            .total_frames = 256,
            .sample_rate = 48000,
            .preload_frames = 48,
            .preload_contract = SamplePreloadContract{
                .source_sample_rate = 48000.0,
                .host_sample_rate = 48000.0,
                .maximum_playback_ratio = 4.0,
                .maximum_host_block_frames = 8,
                .interpolation_guard_frames = 2,
                .configured_preload_frames = 48,
            },
            .stream_source = added.view,
        },
        preload.view()));
    const auto asset = asset_owner.view();
    auto loop = region(LoopPlaybackMode::Forward);
    loop.end_frame = 256;
    SampleStreamLoopVoiceReader reader;
    REQUIRE(reader.prepare(asset, {4, 1}, loop,
                           1.0, SampleInterpolationPolicy::Linear));
    REQUIRE(reader.cursor().step() == 1.0);
    REQUIRE_FALSE(reader.set_playback_rate(asset, 4.0));
    REQUIRE(reader.cursor().step() == 1.0);
    REQUIRE_FALSE(reader.set_playback_rate(4.0));
    REQUIRE(reader.cursor().step() == 1.0);
    REQUIRE_FALSE(reader.set_playback_rate(
        asset, std::ldexp(1.0, std::numeric_limits<std::uint64_t>::digits)));
    REQUIRE(reader.cursor().step() == 1.0);

    auto imported = reader.cursor();
    imported.set_playback_rate(4.0);
    REQUIRE_FALSE(reader.synchronize_cursor(asset, imported));
    REQUIRE(reader.cursor().step() == 1.0);
}

TEST_CASE("Paged loop reader snapshots missing crossfade pages and advances time",
          "[audio][sampler][stream-loop][starvation]") {
    LoopStreamFixture fixture;
    const auto asset = fixture.asset.view();
    const auto loop = region(LoopPlaybackMode::Reverse, true);
    SampleStreamLoopVoiceReader reader;
    REQUIRE(reader.prepare(asset, {1, 1}, loop, 4.0));

    Buffer<float> output(1, 8);
    const auto first = reader.plan_block(asset, 8, 48000.0);
    REQUIRE(first.demand_count > 0);
    const auto rendered = reader.render_block(asset, first, output.view());
    REQUIRE(rendered.supply == SampleStreamVoiceSupply::Starved);
    REQUIRE(rendered.outcome == SampleStreamVoiceOutcomeClass::ServiceStarvation);
    REQUIRE(rendered.ready_output_frames == 0);
    REQUIRE(reader.cursor().position() == first.end_cursor.position());
    for (std::size_t frame = 0; frame < output.num_samples(); ++frame)
        REQUIRE(output.channel(0)[frame] == 0.0f);

    auto stats = reader.starvation_stats();
    REQUIRE(stats.predicted_events == 1);
    REQUIRE(stats.insufficient_lead_events == 1);
    REQUIRE(stats.emergency_events == 1);
    REQUIRE(stats.starved_frames == 8);

    fixture.publish_pages();
    Buffer<float> recovered(1, 8);
    const auto recovered_plan = reader.plan_block(asset, 8, 48000.0);
    const auto recovered_result =
        reader.render_block(asset, recovered_plan, recovered.view());
    REQUIRE(recovered_result.ready_output_frames == 8);
    REQUIRE(recovered.channel(0)[0] == 0.0f);
    stats = reader.starvation_stats();
    REQUIRE(stats.recovery_events == 1);
}

TEST_CASE("Paged loop reader reports stale asset generations",
          "[audio][sampler][stream-loop]") {
    LoopStreamFixture fixture;
    const auto asset = fixture.asset.view();
    SampleStreamLoopVoiceReader reader;
    REQUIRE(reader.prepare(asset, {1, 1}, region(LoopPlaybackMode::Forward), 1.0));

    SampleAsset replacement;
    REQUIRE(replacement.prepare(
        SampleAssetConfig{
            .asset = {101, 2},
            .source = {200, 1},
            .channels = 1,
            .total_frames = 64,
            .sample_rate = 48000,
            .preload_frames = 36,
            .preload_contract = SamplePreloadContract{
                .source_sample_rate = 48000.0,
                .host_sample_rate = 48000.0,
                .maximum_playback_ratio = 4.0,
                .maximum_host_block_frames = 8,
                .interpolation_guard_frames = 2,
                .configured_preload_frames = 36,
            },
            .stream_source = asset.stream_source,
        },
        fixture.preload.view()));
    const auto stale = replacement.view();
    const auto stale_plan = reader.plan_block(stale, 8, 48000.0);
    REQUIRE(stale_plan.supply == SampleStreamVoiceSupply::StaleGeneration);

    const auto plan = reader.plan_block(asset, 8, 48000.0);
    Buffer<float> output(1, 8);
    REQUIRE(reader.render_block(stale, plan, output.view()).supply ==
            SampleStreamVoiceSupply::StaleGeneration);
    REQUIRE(reader.render_block(stale, plan, output.view()).outcome ==
            SampleStreamVoiceOutcomeClass::StaleGeneration);
}

TEST_CASE("Paged loop demand urgency includes accumulated lookahead lead",
          "[audio][sampler][stream-loop]") {
    LoopStreamFixture fixture;
    const auto asset = fixture.asset.view();
    SampleStreamLoopVoiceReader reader;
    REQUIRE(reader.prepare(
        asset, {1, 1}, region(LoopPlaybackMode::ReverseOnce, true), 1.0));
    const auto far_plan = reader.plan_block(asset, 8, 48000.0);
    REQUIRE(far_plan.demand_count > 0);

    SampleStreamCommandInbox<32> inbox;
    REQUIRE(reader.enqueue_demands(far_plan, inbox, 0, 100).complete);
    REQUIRE(inbox.demand_page({
                .source = asset.source,
                .requester = {2, 1},
                .page_index = 9,
                .resident_source_frames = 1,
                .consumption_frames_per_second = 48000.0,
                .demand_class = SampleStreamDemandClass::Sustain,
            }) == SampleStreamCommandPushStatus::Enqueued);
    REQUIRE(fixture.service.drain_commands(inbox).commands_drained ==
            far_plan.demand_count + 1);
    REQUIRE(fixture.service.service_once() == SampleStreamServiceStatus::Published);
    REQUIRE(asset.stream_source.window->ready_page_for_frame(
                asset.source.source_generation, 36).valid);
}

TEST_CASE("Paged loop planning and rendering stay allocation-free for 10000 blocks",
          "[audio][sampler][stream-loop][rt]") {
    LoopStreamFixture fixture;
    fixture.publish_pages();
    const auto asset = fixture.asset.view();
    SampleSincKernelBank bank;
    REQUIRE(bank.build(4));
    const PreparedSampleInterpolation interpolation{
        .policy = SampleInterpolationPolicy::RatioTrackingSinc,
        .sinc = bank.view().select(1.25),
    };
    SampleStreamLoopVoiceReader reader;
    REQUIRE(reader.prepare(asset, {1, 1}, region(LoopPlaybackMode::Forward),
                           1.25, interpolation));
    SampleStreamCommandInbox<32> inbox;
    Buffer<float> output(1, 8);

    pulp::test::RtAllocationProbe allocation_probe;
    pulp::runtime::ScopedNoAlloc no_alloc;
    for (int block = 0; block < 10000; ++block) {
        const auto plan = reader.plan_block(asset, 8, 48000.0);
        (void) reader.enqueue_demands(plan, inbox);
        (void) reader.render_block(asset, plan, output.view());
    }
    REQUIRE(allocation_probe.allocation_count() == 0);
}
