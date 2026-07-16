#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_stream_service.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <cstdint>
#include <limits>
#include <type_traits>

using Catch::Matchers::WithinAbs;
using pulp::audio::Buffer;
using pulp::audio::SampleStreamCacheService;
using pulp::audio::SampleStreamCacheServiceConfig;
using pulp::audio::SampleStreamCacheSourceConfig;
using pulp::audio::SampleStreamCommand;
using pulp::audio::SampleStreamCommandInbox;
using pulp::audio::SampleStreamCommandPushStatus;
using pulp::audio::SampleStreamPageDemand;
using pulp::audio::SampleStreamRequesterToken;
using pulp::audio::SampleStreamScheduleStatus;
using pulp::audio::SampleStreamServiceStatus;
using pulp::audio::SampleStreamSourceAddStatus;
using pulp::audio::SampleStreamSourceToken;
using pulp::audio::SampleStreamWindowReadRequest;

static_assert(std::is_trivially_copyable_v<SampleStreamCommand>);

namespace {

SampleStreamCacheSourceConfig source_config(std::uint64_t id = 1,
                                            std::uint64_t generation = 1) {
    return {
        .token = {id, generation},
        .channels = 1,
        .total_frames = 16,
        .page_frames = 4,
        .cache_page_count = 2,
    };
}

SampleStreamPageDemand demand(SampleStreamSourceToken source,
                              std::uint64_t requester_id,
                              std::uint64_t page_index = 0) {
    return {
        .source = source,
        .requester = {requester_id, 1},
        .page_index = page_index,
        .resident_source_frames = 32,
        .consumption_frames_per_second = 48000.0,
    };
}

}  // namespace

TEST_CASE("Shared sample stream cache decodes one page for two voices",
          "[audio][sampler][stream-service]") {
    SampleStreamCacheService service;
    REQUIRE(service.prepare({.scheduler_capacity = 8, .page_memory_budget_bytes = 32}));

    std::uint64_t decode_calls = 0;
    const auto added = service.add_source(source_config(),
        [&decode_calls](std::uint64_t start, pulp::audio::BufferView<float> destination,
                        std::uint64_t frames) {
            ++decode_calls;
            for (std::uint64_t frame = 0; frame < frames; ++frame) {
                destination.channel_ptr(0)[frame] = static_cast<float>(start + frame + 1);
            }
            return frames;
        });
    REQUIRE(added.added());

    REQUIRE(service.request_page(demand(added.view.token, 10)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.request_page(demand(added.view.token, 11)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.service_once() == SampleStreamServiceStatus::Published);
    REQUIRE(service.service_once() == SampleStreamServiceStatus::Idle);
    REQUIRE(decode_calls == 1);
    REQUIRE(service.scheduler_stats().coalesced == 1);

    Buffer<float> rendered(1, 4);
    const auto read = added.view.window->read_frames(
        rendered.view(),
        SampleStreamWindowReadRequest{
            .stream_generation = added.view.token.source_generation,
            .start_frame = 0,
            .frames = 4,
        });
    REQUIRE(read.complete);
    REQUIRE_THAT(rendered.channel(0)[3], WithinAbs(4.0f, 1.0e-6f));
}

TEST_CASE("Cancelling one sample stream requester preserves shared demand",
          "[audio][sampler][stream-service]") {
    SampleStreamCacheService service;
    REQUIRE(service.prepare({.scheduler_capacity = 8, .page_memory_budget_bytes = 32}));

    std::uint64_t decode_calls = 0;
    const auto added = service.add_source(source_config(),
        [&decode_calls](std::uint64_t, pulp::audio::BufferView<float>, std::uint64_t frames) {
            ++decode_calls;
            return frames;
        });
    REQUIRE(added.added());

    REQUIRE(service.request_page(demand(added.view.token, 20)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.request_page(demand(added.view.token, 21)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.cancel_requester(SampleStreamRequesterToken{20, 1}) == 1);
    REQUIRE(service.scheduler_stats().pending == 1);
    REQUIRE(service.service_once() == SampleStreamServiceStatus::Published);
    REQUIRE(decode_calls == 1);
}

TEST_CASE("Sample stream RT commands drain in producer order",
          "[audio][sampler][stream-service][commands]") {
    SampleStreamCacheService service;
    REQUIRE(service.prepare({.scheduler_capacity = 8, .page_memory_budget_bytes = 32}));
    const auto added = service.add_source(source_config(),
        [](std::uint64_t start, pulp::audio::BufferView<float> destination,
           std::uint64_t frames) {
            for (std::uint64_t frame = 0; frame < frames; ++frame) {
                destination.channel_ptr(0)[frame] =
                    static_cast<float>(start + frame + 1);
            }
            return frames;
        });
    REQUIRE(added.added());

    SampleStreamCommandInbox<8> inbox;
    REQUIRE(inbox.demand_page(demand(added.view.token, 70, 0)) ==
            SampleStreamCommandPushStatus::Enqueued);
    REQUIRE(inbox.cancel_requester({70, 1}) ==
            SampleStreamCommandPushStatus::Enqueued);
    REQUIRE(inbox.demand_page(demand(added.view.token, 70, 1)) ==
            SampleStreamCommandPushStatus::Enqueued);
    REQUIRE(inbox.demand_page(demand(added.view.token, 71, 2)) ==
            SampleStreamCommandPushStatus::Enqueued);
    REQUIRE(inbox.cancel_source_generation(added.view.token) ==
            SampleStreamCommandPushStatus::Enqueued);
    REQUIRE(inbox.demand_page(demand(added.view.token, 72, 3)) ==
            SampleStreamCommandPushStatus::Enqueued);
    REQUIRE(service.scheduler_stats().pending == 0);

    const auto drained = service.drain_commands(inbox);
    REQUIRE(drained.commands_drained == 6);
    REQUIRE(drained.demand_commands == 4);
    REQUIRE(drained.cancel_requester_commands == 1);
    REQUIRE(drained.cancel_source_commands == 1);
    REQUIRE(drained.demands_inserted == 4);
    REQUIRE(drained.demands_refreshed == 0);
    REQUIRE(drained.demands_rejected_full == 0);
    REQUIRE(drained.demands_invalid == 0);
    REQUIRE(drained.requests_cancelled == 3);
    REQUIRE(inbox.telemetry().pending == 0);
    REQUIRE(service.scheduler_stats().pending == 1);

    REQUIRE(service.service_once() == SampleStreamServiceStatus::Published);
    Buffer<float> rendered(1, 4);
    const auto read = added.view.window->read_frames(
        rendered.view(),
        {
            .stream_generation = added.view.token.source_generation,
            .start_frame = 12,
            .frames = 4,
        });
    REQUIRE(read.complete);
    REQUIRE_THAT(rendered.channel(0)[0], WithinAbs(13.0f, 1.0e-6f));
}

TEST_CASE("Sample stream RT command inbox reports bounded overflow",
          "[audio][sampler][stream-service][commands][rt]") {
    SampleStreamCommandInbox<2> inbox;
    SampleStreamCommandPushStatus demand_status = SampleStreamCommandPushStatus::Full;
    SampleStreamCommandPushStatus cancel_status = SampleStreamCommandPushStatus::Full;
    SampleStreamCommandPushStatus overflow_status = SampleStreamCommandPushStatus::Enqueued;
    pulp::audio::SampleStreamCommandInboxTelemetry full;
    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        demand_status = inbox.demand_page(demand({1, 1}, 80, 0));
        cancel_status = inbox.cancel_requester({80, 1});
        overflow_status = inbox.cancel_source_generation({1, 1});
        full = inbox.telemetry();
    }

    REQUIRE(demand_status == SampleStreamCommandPushStatus::Enqueued);
    REQUIRE(cancel_status == SampleStreamCommandPushStatus::Enqueued);
    REQUIRE(overflow_status == SampleStreamCommandPushStatus::Full);
    REQUIRE(full.pending == 2);
    REQUIRE(full.capacity == 2);
    REQUIRE(full.overflow_count == 1);

    SampleStreamCacheService service;
    REQUIRE(service.prepare({.scheduler_capacity = 4, .page_memory_budget_bytes = 32}));
    const auto added = service.add_source(source_config(),
        [](std::uint64_t, pulp::audio::BufferView<float>, std::uint64_t frames) {
            return frames;
        });
    REQUIRE(added.added());

    const auto drained = service.drain_commands(inbox);
    REQUIRE(drained.commands_drained == 2);
    REQUIRE(drained.demands_inserted == 1);
    REQUIRE(drained.requests_cancelled == 1);
    REQUIRE(service.scheduler_stats().pending == 0);
    const auto empty = inbox.telemetry();
    REQUIRE(empty.pending == 0);
    REQUIRE(empty.overflow_count == 1);
}

TEST_CASE("Prepared sample stream command submission and cancellation do not allocate",
          "[audio][sampler][stream-service][commands][rt]") {
    SampleStreamCacheService service;
    REQUIRE(service.prepare({.scheduler_capacity = 2, .page_memory_budget_bytes = 32}));
    const auto added = service.add_source(source_config(),
        [](std::uint64_t, pulp::audio::BufferView<float>, std::uint64_t frames) {
            return frames;
        });
    REQUIRE(added.added());
    SampleStreamCommandInbox<2> inbox;

    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (std::uint64_t generation = 1; generation <= 10000; ++generation) {
            auto page_demand = demand(added.view.token, 90, 0);
            page_demand.requester.requester_generation = generation;
            (void) inbox.demand_page(page_demand);
            (void) inbox.cancel_requester(page_demand.requester);
            (void) service.drain_commands(inbox);
        }
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
    REQUIRE(service.scheduler_stats().pending == 0);
    REQUIRE(inbox.telemetry().overflow_count == 0);
}

TEST_CASE("Sample stream cache admits exact page budget and rejects overflow",
          "[audio][sampler][stream-service][budget]") {
    SampleStreamCacheService service;
    REQUIRE(service.prepare({.scheduler_capacity = 4, .page_memory_budget_bytes = 32}));

    auto reader = [](std::uint64_t, pulp::audio::BufferView<float>, std::uint64_t frames) {
        return frames;
    };
    REQUIRE(service.add_source(source_config(1), reader).status ==
            SampleStreamSourceAddStatus::Added);
    REQUIRE(service.stats().reserved_page_bytes == 32);
    REQUIRE(service.add_source(source_config(2), reader).status ==
            SampleStreamSourceAddStatus::BudgetExceeded);

    auto overflowing = source_config(3);
    overflowing.channels = std::numeric_limits<std::uint32_t>::max();
    overflowing.page_frames = std::numeric_limits<std::uint64_t>::max();
    REQUIRE(service.add_source(overflowing, reader).status ==
            SampleStreamSourceAddStatus::BudgetExceeded);
}

TEST_CASE("Sample stream cache rejects stale source generations",
          "[audio][sampler][stream-service][generation]") {
    SampleStreamCacheService service;
    REQUIRE(service.prepare({.scheduler_capacity = 4, .page_memory_budget_bytes = 32}));
    const auto added = service.add_source(source_config(),
        [](std::uint64_t, pulp::audio::BufferView<float>, std::uint64_t frames) {
            return frames;
        });
    REQUIRE(added.added());

    auto stale = demand({added.view.token.source_id,
                         added.view.token.source_generation + 1},
                        30);
    REQUIRE(service.request_page(stale) == SampleStreamScheduleStatus::Invalid);
    REQUIRE(service.stats().stale_requests == 1);
    REQUIRE(service.scheduler_stats().pending == 0);
}

TEST_CASE("Sample stream cache retains demand while every page slot is busy",
          "[audio][sampler][stream-service][pressure]") {
    SampleStreamCacheService service;
    REQUIRE(service.prepare({.scheduler_capacity = 4, .page_memory_budget_bytes = 16}));
    auto config = source_config();
    config.cache_page_count = 1;
    const auto added = service.add_source(config,
        [](std::uint64_t start, pulp::audio::BufferView<float> destination,
           std::uint64_t frames) {
            for (std::uint64_t frame = 0; frame < frames; ++frame)
                destination.channel_ptr(0)[frame] = static_cast<float>(start + frame);
            return frames;
        });
    REQUIRE(added.added());

    REQUIRE(service.request_page(demand(added.view.token, 40, 0)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.service_once() == SampleStreamServiceStatus::Published);
    REQUIRE(service.request_page(demand(added.view.token, 40, 1)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.service_once() == SampleStreamServiceStatus::NoPageAvailable);
    REQUIRE(service.scheduler_stats().pending == 1);
    REQUIRE(service.service_once() == SampleStreamServiceStatus::NoPageAvailable);
    REQUIRE(service.scheduler_stats().pending == 1);
}

TEST_CASE("Sample stream cache reuses retired pages only after the audio generation completes",
          "[audio][sampler][stream-service][generation][eviction]") {
    SampleStreamCacheService service;
    REQUIRE(service.prepare({.scheduler_capacity = 4, .page_memory_budget_bytes = 16}));
    REQUIRE(service.update_audio_generations(7, 6));

    auto config = source_config();
    config.cache_page_count = 1;
    const auto added = service.add_source(config,
        [](std::uint64_t start, pulp::audio::BufferView<float> destination,
           std::uint64_t frames) {
            for (std::uint64_t frame = 0; frame < frames; ++frame)
                destination.channel_ptr(0)[frame] = static_cast<float>(start + frame + 1);
            return frames;
        });
    REQUIRE(added.added());

    REQUIRE(service.request_page(demand(added.view.token, 50, 0)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.service_once() == SampleStreamServiceStatus::Published);
    const auto old_view = added.view.window->ready_page_for_frame(
        added.view.token.source_generation, 0);
    REQUIRE(old_view.valid);
    REQUIRE_THAT(added.view.window->ready_channel_data(old_view, 0)[0],
                 WithinAbs(1.0f, 1.0e-6f));

    REQUIRE(service.request_page(demand(added.view.token, 50, 1)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.service_once() == SampleStreamServiceStatus::PageRetired);
    REQUIRE(added.view.window->page_state(0) ==
            pulp::audio::SampleStreamPageState::Retired);
    REQUIRE(service.scheduler_stats().pending == 1);
    REQUIRE_FALSE(added.view.window->ready_page_for_frame(
        added.view.token.source_generation, 0).valid);
    REQUIRE_THAT(added.view.window->ready_channel_data(old_view, 0)[0],
                 WithinAbs(1.0f, 1.0e-6f));

    REQUIRE(service.service_once() ==
            SampleStreamServiceStatus::WaitingForAudioGeneration);
    REQUIRE(service.scheduler_stats().pending == 1);
    REQUIRE_FALSE(service.update_audio_generations(7, 8));
    REQUIRE(service.update_audio_generations(8, 7));
    REQUIRE(service.service_once() == SampleStreamServiceStatus::Published);
    REQUIRE(service.scheduler_stats().pending == 0);
    REQUIRE(added.view.window->ready_channel_data(old_view, 0) == nullptr);

    Buffer<float> rendered(1, 4);
    const auto read = added.view.window->read_frames(
        rendered.view(),
        SampleStreamWindowReadRequest{
            .stream_generation = added.view.token.source_generation,
            .start_frame = 4,
            .frames = 4,
        });
    REQUIRE(read.complete);
    REQUIRE_THAT(rendered.channel(0)[0], WithinAbs(5.0f, 1.0e-6f));

    const auto stats = service.stats();
    REQUIRE(stats.pages_retired == 1);
    REQUIRE(stats.retired_pages_reused == 1);
    REQUIRE(stats.retire_waits == 1);
    REQUIRE(stats.invalid_audio_generation_updates == 1);
}

TEST_CASE("Sample stream cache retires the oldest published page deterministically",
          "[audio][sampler][stream-service][eviction]") {
    SampleStreamCacheService service;
    REQUIRE(service.prepare({.scheduler_capacity = 4, .page_memory_budget_bytes = 32}));
    REQUIRE(service.update_audio_generations(10, 9));
    const auto added = service.add_source(source_config(),
        [](std::uint64_t, pulp::audio::BufferView<float>, std::uint64_t frames) {
            return frames;
        });
    REQUIRE(added.added());

    REQUIRE(service.request_page(demand(added.view.token, 60, 0)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.service_once() == SampleStreamServiceStatus::Published);
    REQUIRE(service.request_page(demand(added.view.token, 60, 1)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.service_once() == SampleStreamServiceStatus::Published);
    REQUIRE(service.request_page(demand(added.view.token, 60, 2)) ==
            SampleStreamScheduleStatus::Inserted);

    REQUIRE(service.service_once() == SampleStreamServiceStatus::PageRetired);
    REQUIRE(added.view.window->page_state(0) ==
            pulp::audio::SampleStreamPageState::Retired);
    REQUIRE(added.view.window->page_state(1) ==
            pulp::audio::SampleStreamPageState::Ready);
    REQUIRE(service.scheduler_stats().pending == 1);
    REQUIRE(service.service_once() ==
            SampleStreamServiceStatus::WaitingForAudioGeneration);
    REQUIRE(added.view.window->page_state(1) ==
            pulp::audio::SampleStreamPageState::Ready);
    REQUIRE(service.stats().pages_retired == 1);
}
