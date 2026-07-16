#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/sample_stream_voice_reader.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <bit>
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
    Buffer<float> preload{1, 36};
    SampleAsset asset;

    StreamedRampAsset() {
        REQUIRE(service.prepare({
            .scheduler_capacity = 16,
            .page_memory_budget_bytes = 7 * 4 * sizeof(float),
        }));
        const auto source = service.add_source(
            {
                .token = {200, 1},
                .channels = 1,
                .total_frames = 64,
                .page_frames = 4,
                .cache_page_count = 7,
            },
            [](std::uint64_t start, pulp::audio::BufferView<float> destination,
               std::uint64_t frames) {
                for (std::uint64_t frame = 0; frame < frames; ++frame)
                    destination.channel_ptr(0)[frame] =
                        static_cast<float>(start + frame);
                return frames;
            });
        REQUIRE(source.added());
        for (std::uint64_t frame = 0; frame < 36; ++frame)
            preload.channel(0)[frame] = static_cast<float>(frame);
        const SampleAssetConfig config{
            .asset = {100, 1},
            .source = {200, 1},
            .channels = 1,
            .total_frames = 64,
            .sample_rate = 48000,
            .preload_frames = 36,
            .preload_contract = pulp::audio::SamplePreloadContract{
                .source_sample_rate = 48000.0,
                .host_sample_rate = 48000.0,
                .maximum_playback_ratio = 4.0,
                .maximum_host_block_frames = 8,
                .interpolation_guard_frames = 1,
                .configured_preload_frames = 36,
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
        for (std::uint64_t page = 9; page < 16; ++page) publish_logical_page(page);
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

float nonlinear_sample(std::uint64_t frame) {
    const auto folded = static_cast<std::int32_t>((frame * 37 + frame * frame * 3) % 101);
    return static_cast<float>(folded - 50) / 53.0f;
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
        const double start = ratio == 4.0 ? 17.0 : 34.5;
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
    REQUIRE(whole.seek(asset, 35.25));
    REQUIRE(partitioned.seek(asset, 35.25));

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

TEST_CASE("Resident and streamed voice reads are bit exact for nonlinear material",
          "[audio][sampler][stream-voice][parity]") {
    SampleStreamCacheService service;
    REQUIRE(service.prepare({
        .scheduler_capacity = 16,
        .page_memory_budget_bytes = 7 * 4 * sizeof(float),
    }));
    const auto source = service.add_source(
        {.token = {701, 1},
         .channels = 1,
         .total_frames = 64,
         .page_frames = 4,
         .cache_page_count = 7},
        [](std::uint64_t start,
           pulp::audio::BufferView<float> destination,
           std::uint64_t frames) {
            for (std::uint64_t frame = 0; frame < frames; ++frame)
                destination.channel_ptr(0)[frame] = nonlinear_sample(start + frame);
            return frames;
        });
    REQUIRE(source.added());

    Buffer<float> preload(1, 36);
    Buffer<float> resident_audio(1, 64);
    for (std::uint64_t frame = 0; frame < 64; ++frame) {
        resident_audio.channel(0)[frame] = nonlinear_sample(frame);
        if (frame < 36) preload.channel(0)[frame] = nonlinear_sample(frame);
    }
    const pulp::audio::SamplePreloadContract contract{
        .source_sample_rate = 48000.0,
        .host_sample_rate = 48000.0,
        .maximum_playback_ratio = 4.0,
        .maximum_host_block_frames = 5,
        .interpolation_guard_frames = 1,
        .configured_preload_frames = 36,
    };
    SampleAsset streamed_owner;
    REQUIRE(streamed_owner.prepare(
        {.asset = {801, 1},
         .source = {701, 1},
         .channels = 1,
         .total_frames = 64,
         .sample_rate = 48000,
         .preload_frames = 36,
         .preload_contract = contract,
         .stream_source = source.view},
        preload.view()));
    SampleAsset resident_owner;
    REQUIRE(resident_owner.prepare(
        {.asset = {802, 1},
         .source = {702, 1},
         .channels = 1,
         .total_frames = 64,
         .sample_rate = 48000,
         .preload_frames = 64},
        resident_audio.view()));
    for (std::uint64_t page = 9; page < 16; ++page) {
        REQUIRE(service.request_page({.source = {701, 1},
                                      .requester = {901, 1},
                                      .page_index = page,
                                      .consumption_frames_per_second = 48000.0}) ==
                pulp::audio::SampleStreamScheduleStatus::Inserted);
        REQUIRE(service.service_once() == SampleStreamServiceStatus::Published);
    }

    for (const double ratio : {0.25, 1.0, 4.0}) {
        SampleStreamVoiceReader streamed;
        SampleStreamVoiceReader resident;
        REQUIRE(streamed.prepare(streamed_owner.view(), {902, 1}));
        REQUIRE(resident.prepare(resident_owner.view(), {903, 1}));
        const auto start = ratio == 4.0 ? 14.25 : 34.5;
        REQUIRE(streamed.seek(streamed_owner.view(), start));
        REQUIRE(resident.seek(resident_owner.view(), start));

        for (const std::uint32_t frames : {3, 5, 4}) {
            Buffer<float> streamed_output(1, frames);
            Buffer<float> resident_output(1, frames);
            const auto streamed_plan = streamed.plan_block(
                streamed_owner.view(), frames, ratio, 48000.0);
            const auto resident_plan = resident.plan_block(
                resident_owner.view(), frames, ratio, 48000.0);
            REQUIRE(streamed.render_block(streamed_owner.view(),
                                          streamed_plan,
                                          streamed_output.view()).supply ==
                    SampleStreamVoiceSupply::Ready);
            REQUIRE(resident.render_block(resident_owner.view(),
                                          resident_plan,
                                          resident_output.view()).supply ==
                    SampleStreamVoiceSupply::Ready);
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                REQUIRE(std::bit_cast<std::uint32_t>(streamed_output.channel(0)[frame]) ==
                        std::bit_cast<std::uint32_t>(resident_output.channel(0)[frame]));
            }
        }
    }
}

TEST_CASE("Stream voice reader starves then recovers on the current source timeline",
          "[audio][sampler][stream-voice][starvation]") {
    StreamedRampAsset fixture;
    const auto asset = fixture.asset.view();
    SampleStreamVoiceReader reader;
    REQUIRE(reader.prepare(asset, {4, 1}));
    REQUIRE(reader.seek(asset, 35.0));

    Buffer<float> missed(1, 4);
    const auto missed_plan = reader.plan_block(asset, 4, 1.0, 48000.0);
    const auto missed_result = reader.render_block(asset, missed_plan, missed.view());
    REQUIRE(missed_result.supply == SampleStreamVoiceSupply::Starved);
    REQUIRE(missed_result.ready_output_frames == 0);
    REQUIRE_THAT(reader.source_position(), WithinAbs(39.0, 1.0e-12));
    for (const auto sample : missed.channel(0)) REQUIRE(sample == 0.0f);

    fixture.publish_logical_page(9);
    fixture.publish_logical_page(10);
    Buffer<float> recovered(1, 2);
    const auto recovered_plan = reader.plan_block(asset, 2, 1.0, 48000.0);
    const auto recovered_result =
        reader.render_block(asset, recovered_plan, recovered.view());
    REQUIRE(recovered_result.supply == SampleStreamVoiceSupply::Ready);
    REQUIRE(recovered_result.ready_output_frames == 2);
    REQUIRE_THAT(recovered.channel(0)[0], WithinAbs(39.0f, 1.0e-6f));
    REQUIRE_THAT(recovered.channel(0)[1], WithinAbs(40.0f, 1.0e-6f));
}

TEST_CASE("Stream voice reader rejects stale generation plans",
          "[audio][sampler][stream-voice][generation]") {
    StreamedRampAsset fixture;
    const auto asset = fixture.asset.view();
    SampleStreamVoiceReader reader;
    REQUIRE(reader.prepare(asset, {5, 1}));
    SampleAsset replacement;
    const SampleAssetConfig replacement_config{
        .asset = {100, 2},
        .source = {200, 1},
        .channels = 1,
        .total_frames = 64,
        .sample_rate = 48000,
        .preload_frames = 36,
        .preload_contract = pulp::audio::SamplePreloadContract{
            .source_sample_rate = 48000.0,
            .host_sample_rate = 48000.0,
            .maximum_playback_ratio = 4.0,
            .maximum_host_block_frames = 8,
            .interpolation_guard_frames = 1,
            .configured_preload_frames = 36,
        },
        .stream_source = asset.stream_source,
    };
    REQUIRE(replacement.prepare(replacement_config, fixture.preload.view()));
    const auto stale = replacement.view();

    const auto plan = reader.plan_block(stale, 4, 1.0, 48000.0);
    REQUIRE(plan.supply == SampleStreamVoiceSupply::StaleGeneration);
    Buffer<float> output(1, 4);
    std::fill(output.channel(0).begin(), output.channel(0).end(), 1.0f);
    const auto result = reader.render_block(stale, plan, output.view());
    REQUIRE(result.supply == SampleStreamVoiceSupply::StaleGeneration);
    REQUIRE(result.ready_output_frames == 0);
    REQUIRE(reader.source_position() == 0.0);
    for (const auto sample : output.channel(0)) REQUIRE(sample == 0.0f);
}

TEST_CASE("Stream voice reader enforces the asset preload capability",
          "[audio][sampler][stream-voice][contract]") {
    StreamedRampAsset fixture;
    const auto asset = fixture.asset.view();
    SampleStreamVoiceReader reader;
    REQUIRE(reader.prepare(asset, {12, 1}));

    REQUIRE(reader.plan_block(asset, 9, 1.0, 48000.0).supply ==
            SampleStreamVoiceSupply::InvalidContract);
    REQUIRE(reader.plan_block(asset, 8, 4.01, 48000.0).supply ==
            SampleStreamVoiceSupply::InvalidContract);
    REQUIRE(reader.plan_block(asset, 8, 1.0, 44100.0).supply ==
            SampleStreamVoiceSupply::InvalidContract);
}

TEST_CASE("Stream voice reader reports and clears end of source",
          "[audio][sampler][stream-voice][end]") {
    StreamedRampAsset fixture;
    const auto asset = fixture.asset.view();
    SampleStreamVoiceReader reader;
    REQUIRE(reader.prepare(asset, {9, 1}));
    REQUIRE(reader.seek(asset, 64.0));

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
    REQUIRE(reader.seek(asset, 35.0));

    const auto plan = reader.plan_block(asset, 4, 0.25, 48000.0);
    REQUIRE(plan.supply == SampleStreamVoiceSupply::Ready);
    REQUIRE(plan.demand_count == 1);
    REQUIRE(plan.demands[0].page_index == 9);
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

TEST_CASE("Stream voice demand urgency measures from the current timeline",
          "[audio][sampler][stream-voice][demand][urgency]") {
    StreamedRampAsset fixture;
    const auto asset = fixture.asset.view();
    SampleStreamVoiceReader reader;
    REQUIRE(reader.prepare(asset, {10, 1}));
    REQUIRE(reader.seek(asset, 35.0));

    const auto plan = reader.plan_block(asset, 8, 1.0, 48000.0);
    REQUIRE(plan.supply == SampleStreamVoiceSupply::Ready);
    REQUIRE(plan.demand_count == 2);
    REQUIRE(plan.demands[0].page_index == 9);
    REQUIRE(plan.demands[0].resident_source_frames == 1);
    REQUIRE(plan.demands[1].page_index == 10);
    REQUIRE(plan.demands[1].resident_source_frames == 5);

    REQUIRE(fixture.service.request_page(plan.demands[1]) ==
            pulp::audio::SampleStreamScheduleStatus::Inserted);
    REQUIRE(fixture.service.request_page(plan.demands[0]) ==
            pulp::audio::SampleStreamScheduleStatus::Inserted);
    REQUIRE(fixture.service.service_once() == SampleStreamServiceStatus::Published);
    REQUIRE(asset.stream_source.window->ready_page_for_frame(1, 36).valid);
    REQUIRE_FALSE(asset.stream_source.window->ready_page_for_frame(1, 40).valid);
}

TEST_CASE("Stream voice demand enqueue resumes after a bounded partial push",
          "[audio][sampler][stream-voice][demand][pressure]") {
    StreamedRampAsset fixture;
    const auto asset = fixture.asset.view();
    SampleStreamVoiceReader reader;
    REQUIRE(reader.prepare(asset, {11, 1}));
    REQUIRE(reader.seek(asset, 35.0));
    const auto plan = reader.plan_block(asset, 8, 1.0, 48000.0);
    REQUIRE(plan.demand_count == 2);

    SampleStreamCommandInbox<1> inbox;
    const auto first = reader.enqueue_demands(plan, inbox);
    REQUIRE_FALSE(first.complete);
    REQUIRE(first.enqueued == 1);
    REQUIRE(first.next_demand_index == 1);
    const auto first_drain = fixture.service.drain_commands(inbox);
    REQUIRE(first_drain.demands_inserted == 1);

    const auto second = reader.enqueue_demands(plan, inbox, first.next_demand_index);
    REQUIRE(second.complete);
    REQUIRE(second.enqueued == 1);
    REQUIRE(second.next_demand_index == 2);
    const auto second_drain = fixture.service.drain_commands(inbox);
    REQUIRE(second_drain.demands_inserted == 1);
    REQUIRE(fixture.service.scheduler_stats().pending == 2);
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
            .total_frames = 200,
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
    Buffer<float> preload(1, 84);
    SampleAsset asset_owner;
    const SampleAssetConfig config{
        .asset = {300, 1},
        .source = {400, 1},
        .channels = 1,
        .total_frames = 200,
        .sample_rate = 48000,
        .preload_frames = 84,
        .preload_contract = pulp::audio::SamplePreloadContract{
            .source_sample_rate = 48000.0,
            .host_sample_rate = 48000.0,
            .maximum_playback_ratio = 4.0,
            .maximum_host_block_frames = 20,
            .interpolation_guard_frames = 1,
            .configured_preload_frames = 84,
        },
        .stream_source = source.view,
    };
    REQUIRE(asset_owner.prepare(config, preload.view()));
    const auto asset = asset_owner.view();
    SampleStreamVoiceReader reader;
    REQUIRE(reader.prepare(asset, {7, 1}));
    REQUIRE(reader.seek(asset, 83.0));

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

TEST_CASE("Prepared streamed page rendering stays allocation-free for 10000 blocks",
          "[audio][sampler][stream-voice][rt][pages]") {
    StreamedRampAsset fixture;
    fixture.publish_all_pages();
    const auto asset = fixture.asset.view();
    SampleStreamVoiceReader reader;
    REQUIRE(reader.prepare(asset, {13, 1}));
    Buffer<float> output(1, 8);

    std::uint64_t ready_frames = 0;
    bool all_seeks_succeeded = true;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe allocation_probe;
        pulp::runtime::ScopedNoAlloc no_alloc;
        for (std::uint32_t iteration = 0; iteration < 10000; ++iteration) {
            all_seeks_succeeded = reader.seek(asset, 35.0) && all_seeks_succeeded;
            const auto plan = reader.plan_block(asset, 8, 1.0, 48000.0);
            ready_frames +=
                reader.render_block(asset, plan, output.view()).ready_output_frames;
        }
        allocations = allocation_probe.allocation_count();
    }
    REQUIRE(all_seeks_succeeded);
    REQUIRE(ready_frames == 80000);
    REQUIRE(allocations == 0);
}
