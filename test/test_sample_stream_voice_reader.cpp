#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/sample_stream_voice_reader.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <cstdint>

using Catch::Matchers::WithinAbs;
using pulp::audio::Buffer;
using pulp::audio::SampleAsset;
using pulp::audio::SampleAssetConfig;
using pulp::audio::SampleAssetView;
using pulp::audio::SampleStreamCacheService;
using pulp::audio::SampleStreamCommandInbox;
using pulp::audio::SampleStreamPageDemand;
using pulp::audio::SampleStreamServiceStatus;
using pulp::audio::SampleStreamVoiceReader;
using pulp::audio::SampleStreamVoiceSupply;

namespace {

struct StreamedRampAsset {
    SampleStreamCacheService service;
    Buffer<float> preload{1, 8};
    SampleAsset asset;

    StreamedRampAsset() {
        REQUIRE(service.prepare({
            .scheduler_capacity = 16,
            .page_memory_budget_bytes = 8 * 4 * sizeof(float),
        }));
        const auto source = service.add_source(
            {
                .token = {200, 1},
                .channels = 1,
                .total_frames = 32,
                .page_frames = 4,
                .cache_page_count = 8,
            },
            [](std::uint64_t start, pulp::audio::BufferView<float> destination,
               std::uint64_t frames) {
                for (std::uint64_t frame = 0; frame < frames; ++frame)
                    destination.channel_ptr(0)[frame] =
                        static_cast<float>(start + frame);
                return frames;
            });
        REQUIRE(source.added());
        for (std::uint64_t frame = 0; frame < 8; ++frame)
            preload.channel(0)[frame] = static_cast<float>(frame);
        const SampleAssetConfig config{
            .asset = {100, 1},
            .source = {200, 1},
            .channels = 1,
            .total_frames = 32,
            .sample_rate = 48000,
            .preload_frames = 8,
            .preload_contract = pulp::audio::SamplePreloadContract{
                .source_sample_rate = 48000.0,
                .host_sample_rate = 48000.0,
                .maximum_playback_ratio = 4.0,
                .maximum_host_block_frames = 1,
                .interpolation_guard_frames = 1,
                .configured_preload_frames = 8,
            },
            .stream_source = source.view,
        };
        REQUIRE(asset.prepare(config, preload.view()));
    }

    void publish_logical_page(std::uint64_t logical_page) {
        REQUIRE(service.request_page(SampleStreamPageDemand{
                    .source = {200, 1},
                    .requester = {900, 1},
                    .page_index = logical_page,
                    .consumption_frames_per_second = 48000.0,
                }) == pulp::audio::SampleStreamScheduleStatus::Inserted);
        REQUIRE(service.service_once() == SampleStreamServiceStatus::Published);
    }

    void publish_all_pages() {
        for (std::uint64_t page = 2; page < 8; ++page) publish_logical_page(page);
    }
};

void require_ramp(const Buffer<float>& output,
                  double start,
                  double ratio,
                  std::size_t frames) {
    for (std::size_t frame = 0; frame < frames; ++frame) {
        REQUIRE_THAT(output.channel(0)[frame],
                     WithinAbs(static_cast<float>(start + frame * ratio), 1.0e-5f));
    }
}

}  // namespace

TEST_CASE("Stream voice reader is exact across preload and pages at pitched ratios",
          "[audio][sampler][stream-voice]") {
    for (const double ratio : {0.25, 1.0, 4.0}) {
        StreamedRampAsset fixture;
        fixture.publish_all_pages();
        const auto asset = fixture.asset.view();
        SampleStreamVoiceReader reader;
        REQUIRE(reader.prepare(asset, {1, 1}));
        const double start = ratio == 4.0 ? 1.0 : 6.5;
        REQUIRE(reader.seek(asset, start));

        Buffer<float> output(1, 6);
        const auto plan = reader.plan_block(asset, 6, ratio, 48000.0);
        REQUIRE(plan.supply == SampleStreamVoiceSupply::Ready);
        const auto rendered = reader.render_block(asset, plan, output.view());
        REQUIRE(rendered.supply == SampleStreamVoiceSupply::Ready);
        REQUIRE(rendered.ready_output_frames == 6);
        require_ramp(output, start, ratio, 6);
    }
}

TEST_CASE("Stream voice reader is block-partition invariant near page boundaries",
          "[audio][sampler][stream-voice][partition]") {
    StreamedRampAsset fixture;
    fixture.publish_all_pages();
    const auto asset = fixture.asset.view();

    SampleStreamVoiceReader whole;
    SampleStreamVoiceReader partitioned;
    REQUIRE(whole.prepare(asset, {2, 1}));
    REQUIRE(partitioned.prepare(asset, {3, 1}));
    REQUIRE(whole.seek(asset, 7.25));
    REQUIRE(partitioned.seek(asset, 7.25));

    Buffer<float> whole_output(1, 8);
    const auto whole_plan = whole.plan_block(asset, 8, 0.75, 48000.0);
    REQUIRE(whole.render_block(asset, whole_plan, whole_output.view()).ready_output_frames == 8);

    Buffer<float> split_output(1, 8);
    auto first = split_output.view().slice(0, 3);
    auto second = split_output.view().slice(3, 5);
    const auto first_plan = partitioned.plan_block(asset, 3, 0.75, 48000.0);
    REQUIRE(partitioned.render_block(asset, first_plan, first).ready_output_frames == 3);
    const auto second_plan = partitioned.plan_block(asset, 5, 0.75, 48000.0);
    REQUIRE(partitioned.render_block(asset, second_plan, second).ready_output_frames == 5);

    for (std::size_t frame = 0; frame < 8; ++frame) {
        REQUIRE_THAT(split_output.channel(0)[frame],
                     WithinAbs(whole_output.channel(0)[frame], 1.0e-6f));
    }
}

TEST_CASE("Stream voice reader starves then recovers on the current source timeline",
          "[audio][sampler][stream-voice][starvation]") {
    StreamedRampAsset fixture;
    const auto asset = fixture.asset.view();
    SampleStreamVoiceReader reader;
    REQUIRE(reader.prepare(asset, {4, 1}));
    REQUIRE(reader.seek(asset, 7.0));

    Buffer<float> missed(1, 4);
    const auto missed_plan = reader.plan_block(asset, 4, 1.0, 48000.0);
    const auto missed_result = reader.render_block(asset, missed_plan, missed.view());
    REQUIRE(missed_result.supply == SampleStreamVoiceSupply::Starved);
    REQUIRE(missed_result.ready_output_frames == 0);
    REQUIRE_THAT(reader.source_position(), WithinAbs(11.0, 1.0e-12));
    for (const auto sample : missed.channel(0)) REQUIRE(sample == 0.0f);

    fixture.publish_logical_page(2);
    fixture.publish_logical_page(3);
    Buffer<float> recovered(1, 2);
    const auto recovered_plan = reader.plan_block(asset, 2, 1.0, 48000.0);
    const auto recovered_result =
        reader.render_block(asset, recovered_plan, recovered.view());
    REQUIRE(recovered_result.supply == SampleStreamVoiceSupply::Ready);
    REQUIRE(recovered_result.ready_output_frames == 2);
    REQUIRE_THAT(recovered.channel(0)[0], WithinAbs(11.0f, 1.0e-6f));
    REQUIRE_THAT(recovered.channel(0)[1], WithinAbs(12.0f, 1.0e-6f));
}

TEST_CASE("Stream voice reader rejects stale generation plans",
          "[audio][sampler][stream-voice][generation]") {
    StreamedRampAsset fixture;
    const auto asset = fixture.asset.view();
    SampleStreamVoiceReader reader;
    REQUIRE(reader.prepare(asset, {5, 1}));
    auto stale = asset;
    ++stale.asset.asset_generation;

    const auto plan = reader.plan_block(stale, 4, 1.0, 48000.0);
    REQUIRE(plan.supply == SampleStreamVoiceSupply::StaleGeneration);
    Buffer<float> output(1, 4);
    const auto result = reader.render_block(stale, plan, output.view());
    REQUIRE(result.supply == SampleStreamVoiceSupply::StaleGeneration);
    REQUIRE(result.ready_output_frames == 0);
    REQUIRE(reader.source_position() == 0.0);
}

TEST_CASE("Stream voice reader reports and clears end of source",
          "[audio][sampler][stream-voice][end]") {
    StreamedRampAsset fixture;
    const auto asset = fixture.asset.view();
    SampleStreamVoiceReader reader;
    REQUIRE(reader.prepare(asset, {9, 1}));
    REQUIRE(reader.seek(asset, 32.0));

    Buffer<float> output(1, 4);
    std::fill(output.channel(0).begin(), output.channel(0).end(), 1.0f);
    const auto plan = reader.plan_block(asset, 4, 1.0, 48000.0);
    REQUIRE(plan.supply == SampleStreamVoiceSupply::EndOfSource);
    const auto result = reader.render_block(asset, plan, output.view());
    REQUIRE(result.supply == SampleStreamVoiceSupply::EndOfSource);
    REQUIRE(result.ready_output_frames == 0);
    for (const auto sample : output.channel(0)) REQUIRE(sample == 0.0f);
}

TEST_CASE("Stream voice planning coalesces linear guard demands before enqueue",
          "[audio][sampler][stream-voice][demand]") {
    StreamedRampAsset fixture;
    const auto asset = fixture.asset.view();
    SampleStreamVoiceReader reader;
    REQUIRE(reader.prepare(asset, {6, 2}));
    REQUIRE(reader.seek(asset, 7.0));

    const auto plan = reader.plan_block(asset, 4, 0.25, 48000.0);
    REQUIRE(plan.supply == SampleStreamVoiceSupply::Ready);
    REQUIRE(plan.demand_count == 1);
    REQUIRE(plan.demands[0].page_index == 2);
    REQUIRE(plan.demands[0].requester.requester_id == 6);
    REQUIRE(plan.demands[0].requester.requester_generation == 2);

    SampleStreamCommandInbox<4> inbox;
    const auto enqueued = reader.enqueue_demands(plan, inbox);
    REQUIRE(enqueued.complete);
    REQUIRE(enqueued.enqueued == 1);
    REQUIRE(inbox.telemetry().pending == 1);

    SampleStreamCommandInbox<1> full_inbox;
    REQUIRE(full_inbox.demand_page(plan.demands[0]) ==
            pulp::audio::SampleStreamCommandPushStatus::Enqueued);
    const auto rejected = reader.enqueue_demands(plan, full_inbox);
    REQUIRE_FALSE(rejected.complete);
    REQUIRE(rejected.enqueued == 0);
    REQUIRE(full_inbox.telemetry().pending == 1);
    REQUIRE(full_inbox.telemetry().overflow_count == 1);
}

TEST_CASE("Stream voice planning rejects fixed demand capacity overflow",
          "[audio][sampler][stream-voice][capacity]") {
    SampleStreamCacheService service;
    REQUIRE(service.prepare({
        .scheduler_capacity = 4,
        .page_memory_budget_bytes = sizeof(float),
    }));
    const auto source = service.add_source(
        {
            .token = {400, 1},
            .channels = 1,
            .total_frames = 100,
            .page_frames = 1,
            .cache_page_count = 1,
        },
        [](std::uint64_t start, pulp::audio::BufferView<float> destination,
           std::uint64_t frames) {
            for (std::uint64_t frame = 0; frame < frames; ++frame)
                destination.channel_ptr(0)[frame] = static_cast<float>(start + frame);
            return frames;
        });
    REQUIRE(source.added());
    Buffer<float> preload(1, 8);
    SampleAsset asset_owner;
    const SampleAssetConfig config{
        .asset = {300, 1},
        .source = {400, 1},
        .channels = 1,
        .total_frames = 100,
        .sample_rate = 48000,
        .preload_frames = 8,
        .preload_contract = pulp::audio::SamplePreloadContract{
            .source_sample_rate = 48000.0,
            .host_sample_rate = 48000.0,
            .maximum_playback_ratio = 4.0,
            .maximum_host_block_frames = 1,
            .interpolation_guard_frames = 1,
            .configured_preload_frames = 8,
        },
        .stream_source = source.view,
    };
    REQUIRE(asset_owner.prepare(config, preload.view()));
    const auto asset = asset_owner.view();
    SampleStreamVoiceReader reader;
    REQUIRE(reader.prepare(asset, {7, 1}));

    const auto plan = reader.plan_block(asset, 20, 4.0, 48000.0);
    REQUIRE(plan.supply == SampleStreamVoiceSupply::InvalidContract);
    REQUIRE(plan.demand_count == 0);
}

TEST_CASE("Stream voice planning and rendering stay allocation-free for 10000 blocks",
          "[audio][sampler][stream-voice][rt]") {
    Buffer<float> preload(1, 8);
    for (std::size_t frame = 0; frame < 8; ++frame)
        preload.channel(0)[frame] = static_cast<float>(frame);
    SampleAsset owner;
    const SampleAssetConfig config{
        .asset = {500, 1},
        .source = {600, 1},
        .channels = 1,
        .total_frames = 8,
        .sample_rate = 48000,
        .preload_frames = 8,
    };
    REQUIRE(owner.prepare(config, preload.view()));
    const auto asset = owner.view();
    SampleStreamVoiceReader reader;
    REQUIRE(reader.prepare(asset, {8, 1}));
    Buffer<float> output(1, 4);

    std::uint64_t ready_frames = 0;
    bool all_seeks_succeeded = true;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe allocation_probe;
        pulp::runtime::ScopedNoAlloc no_alloc;
        for (std::uint32_t iteration = 0; iteration < 10000; ++iteration) {
            all_seeks_succeeded = reader.seek(asset, 1.0) && all_seeks_succeeded;
            const auto plan = reader.plan_block(asset, 4, 0.5, 48000.0);
            ready_frames +=
                reader.render_block(asset, plan, output.view()).ready_output_frames;
        }
        allocations = allocation_probe.allocation_count();
    }
    REQUIRE(all_seeks_succeeded);
    REQUIRE(ready_frames == 40000);
    REQUIRE(allocations == 0);
}
