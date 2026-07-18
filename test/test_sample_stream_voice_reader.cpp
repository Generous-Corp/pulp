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
using pulp::audio::SampleStreamVoiceOutcomeClass;
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

TEST_CASE("Stream voice output is bit exact across deterministic page-arrival schedules",
          "[audio][sampler][stream-voice][schedule][parity]") {
    StreamedRampAsset early;
    StreamedRampAsset just_in_time;

    SampleStreamVoiceReader first;
    SampleStreamVoiceReader second;
    REQUIRE(first.prepare(early.asset.view(), {21, 1}));
    REQUIRE(second.prepare(just_in_time.asset.view(), {22, 1}));
    REQUIRE(first.seek(early.asset.view(), 35.0));
    REQUIRE(second.seek(just_in_time.asset.view(), 35.0));

    Buffer<float> first_output(1, 16);
    Buffer<float> second_output(1, 16);

    // The first schedule publishes one callback ahead; the second publishes
    // only the page needed by the callback that is about to render. Both are
    // explicitly in contract before every render, so starvation is not part
    // of this parity proof.
    for (std::uint64_t page = 9; page <= 12; ++page)
        early.publish_logical_page(page);
    just_in_time.publish_logical_page(9);
    for (std::size_t callback = 0; callback < 4; ++callback) {
        if (callback != 0) {
            just_in_time.publish_logical_page(9 + callback);
        }

        auto first_block = first_output.view().slice(callback * 4, 4);
        auto second_block = second_output.view().slice(callback * 4, 4);
        const auto first_plan = first.plan_block(early.asset.view(), 4, 1.0, 48000.0);
        const auto second_plan =
            second.plan_block(just_in_time.asset.view(), 4, 1.0, 48000.0);
        REQUIRE(first_plan.supply == SampleStreamVoiceSupply::Ready);
        REQUIRE(second_plan.supply == SampleStreamVoiceSupply::Ready);
        const auto first_result =
            first.render_block(early.asset.view(), first_plan, first_block);
        const auto second_result =
            second.render_block(just_in_time.asset.view(), second_plan, second_block);
        REQUIRE(first_result.supply == SampleStreamVoiceSupply::Ready);
        REQUIRE(second_result.supply == SampleStreamVoiceSupply::Ready);
        REQUIRE(first_result.ready_output_frames == 4);
        REQUIRE(second_result.ready_output_frames == 4);
        for (std::size_t frame = 0; frame < 4; ++frame) {
            REQUIRE(std::bit_cast<std::uint32_t>(first_block.channel_ptr(0)[frame]) ==
                    std::bit_cast<std::uint32_t>(second_block.channel_ptr(0)[frame]));
        }
        REQUIRE(std::bit_cast<std::uint64_t>(first.source_position()) ==
                std::bit_cast<std::uint64_t>(second.source_position()));
    }

    REQUIRE(first.starvation_stats().starved_frames == 0);
    REQUIRE(second.starvation_stats().starved_frames == 0);
    for (std::size_t frame = 0; frame < first_output.num_samples(); ++frame) {
        REQUIRE(std::bit_cast<std::uint32_t>(first_output.channel(0)[frame]) ==
                std::bit_cast<std::uint32_t>(second_output.channel(0)[frame]));
    }
    REQUIRE(std::bit_cast<std::uint64_t>(first.source_position()) ==
            std::bit_cast<std::uint64_t>(second.source_position()));
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
    REQUIRE(reader.prepare(asset, {4, 1},
                           {.fade_out_frames = 4, .recovery_frames = 4}));
    REQUIRE(reader.seek(asset, 35.0));

    Buffer<float> missed(1, 4);
    const auto missed_plan = reader.plan_block(asset, 4, 1.0, 48000.0);
    const auto missed_result = reader.render_block(asset, missed_plan, missed.view());
    REQUIRE(missed_result.supply == SampleStreamVoiceSupply::Starved);
    REQUIRE(missed_result.outcome == SampleStreamVoiceOutcomeClass::ServiceStarvation);
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
    REQUIRE(recovered_result.outcome == SampleStreamVoiceOutcomeClass::None);
    REQUIRE(recovered_result.ready_output_frames == 2);
    REQUIRE_THAT(recovered.channel(0)[0], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(recovered.channel(0)[1], WithinAbs(20.0f, 1.0e-5f));
    const auto stats = reader.starvation_stats();
    REQUIRE(stats.predicted_events == 1);
    REQUIRE(stats.insufficient_lead_events == 1);
    REQUIRE(stats.emergency_events == 1);
    REQUIRE(stats.starved_frames == 4);
    REQUIRE(stats.recovery_events == 1);
}

TEST_CASE("Stream voice reader fades a valid prefix to zero before a predicted miss",
          "[audio][sampler][stream-voice][starvation][telemetry]") {
    StreamedRampAsset fixture;
    fixture.publish_logical_page(9);
    const auto asset = fixture.asset.view();
    SampleStreamVoiceReader reader;
    REQUIRE(reader.prepare(asset, {13, 1},
                           {.fade_out_frames = 8, .recovery_frames = 4}));
    REQUIRE(reader.seek(asset, 35.0));

    Buffer<float> faded(1, 8);
    const auto faded_plan = reader.plan_block(asset, 8, 1.0, 48000.0);
    const auto faded_result = reader.render_block(asset, faded_plan, faded.view());
    REQUIRE(faded_result.supply == SampleStreamVoiceSupply::Starved);
    REQUIRE(faded_result.outcome == SampleStreamVoiceOutcomeClass::ServiceStarvation);
    REQUIRE(faded_result.ready_output_frames == 4);
    REQUIRE_THAT(faded.channel(0)[0], WithinAbs(35.0f, 1.0e-6f));
    REQUIRE(faded.channel(0)[1] < 36.0f);
    REQUIRE(faded.channel(0)[1] > 0.0f);
    REQUIRE(faded.channel(0)[2] < faded.channel(0)[1]);
    REQUIRE_THAT(faded.channel(0)[3], WithinAbs(0.0f, 1.0e-5f));
    for (std::size_t frame = 4; frame < faded.num_samples(); ++frame)
        REQUIRE(faded.channel(0)[frame] == 0.0f);
    REQUIRE_THAT(faded.channel(0)[3] - faded.channel(0)[4],
                 WithinAbs(0.0f, 1.0e-5f));

    fixture.publish_logical_page(10);
    fixture.publish_logical_page(11);
    Buffer<float> recovered(1, 4);
    const auto recovered_plan = reader.plan_block(asset, 4, 1.0, 48000.0);
    const auto recovered_result =
        reader.render_block(asset, recovered_plan, recovered.view());
    REQUIRE(recovered_result.supply == SampleStreamVoiceSupply::Ready);
    REQUIRE(recovered_result.ready_output_frames == 4);
    REQUIRE(recovered.channel(0)[0] == 0.0f);
    REQUIRE_THAT(faded.channel(0)[7] - recovered.channel(0)[0],
                 WithinAbs(0.0f, 1.0e-7f));
    REQUIRE(recovered.channel(0)[1] > 0.0f);
    REQUIRE_THAT(recovered.channel(0)[3], WithinAbs(46.0f, 1.0e-5f));

    const auto stats = reader.starvation_stats();
    REQUIRE(stats.predicted_events == 1);
    REQUIRE(stats.insufficient_lead_events == 0);
    REQUIRE(stats.emergency_events == 0);
    REQUIRE(stats.starved_frames == 4);
    REQUIRE(stats.recovery_events == 1);
}

TEST_CASE("Stream voice reader right-aligns a short fade to the missing-frame boundary",
          "[audio][sampler][stream-voice][starvation][boundary]") {
    StreamedRampAsset fixture;
    fixture.publish_logical_page(9);
    fixture.publish_logical_page(10);
    const auto asset = fixture.asset.view();
    SampleStreamVoiceReader reader;
    REQUIRE(reader.prepare(asset, {14, 1},
                           {.fade_out_frames = 4, .recovery_frames = 4}));
    REQUIRE(reader.seek(asset, 37.0));

    Buffer<float> output(1, 8);
    const auto plan = reader.plan_block(asset, 8, 1.0, 48000.0);
    const auto result = reader.render_block(asset, plan, output.view());

    REQUIRE(result.supply == SampleStreamVoiceSupply::Starved);
    REQUIRE(result.ready_output_frames == 6);
    // The two valid frames before the four-frame fade remain untouched.
    REQUIRE(output.channel(0)[0] == 37.0f);
    REQUIRE(output.channel(0)[1] == 38.0f);
    // The fade begins at unity on the last four valid frames and reaches zero
    // exactly where the unavailable suffix starts.
    REQUIRE(output.channel(0)[2] == 39.0f);
    REQUIRE(output.channel(0)[3] < 40.0f);
    REQUIRE(output.channel(0)[3] > 0.0f);
    REQUIRE(output.channel(0)[4] < output.channel(0)[3]);
    REQUIRE_THAT(output.channel(0)[5], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE(output.channel(0)[6] == 0.0f);
    REQUIRE(output.channel(0)[7] == 0.0f);
    REQUIRE(reader.starvation_stats().emergency_events == 0);
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
    REQUIRE(result.outcome == SampleStreamVoiceOutcomeClass::StaleGeneration);
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
    const auto invalid_plan = reader.plan_block(asset, 9, 1.0, 48000.0);
    Buffer<float> invalid_output(1, 9);
    REQUIRE(reader.render_block(asset, invalid_plan, invalid_output.view()).outcome ==
            SampleStreamVoiceOutcomeClass::InvalidPreloadContract);
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
    REQUIRE(result.outcome == SampleStreamVoiceOutcomeClass::NormalEndOfSource);
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
