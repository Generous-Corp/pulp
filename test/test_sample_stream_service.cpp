#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_stream_service.hpp>

#include <cstdint>
#include <limits>

using Catch::Matchers::WithinAbs;
using pulp::audio::Buffer;
using pulp::audio::SampleStreamCacheService;
using pulp::audio::SampleStreamCacheServiceConfig;
using pulp::audio::SampleStreamCacheSourceConfig;
using pulp::audio::SampleStreamPageDemand;
using pulp::audio::SampleStreamRequesterToken;
using pulp::audio::SampleStreamScheduleStatus;
using pulp::audio::SampleStreamServiceStatus;
using pulp::audio::SampleStreamSourceAddStatus;
using pulp::audio::SampleStreamSourceToken;
using pulp::audio::SampleStreamWindowReadRequest;

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
