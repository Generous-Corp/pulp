#include <pulp/audio/sample_memory_governor.hpp>
#include <pulp/audio/sample_asset.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using pulp::audio::SampleMemoryCategory;
using pulp::audio::SampleMemoryGovernor;
using pulp::audio::SampleMemoryReserveStatus;
using pulp::audio::checked_sample_storage_bytes;

static_assert(std::is_move_constructible_v<SampleMemoryGovernor>);
static_assert(!std::is_move_assignable_v<SampleMemoryGovernor>);

TEST_CASE("Sample memory governor enforces one combined preload and page cap",
          "[audio][sampler][memory-governor]") {
    SampleMemoryGovernor governor;
    REQUIRE_FALSE(governor.prepare(0));
    REQUIRE(governor.prepare(100));

    auto preload = governor.reserve(SampleMemoryCategory::Preload, 60);
    REQUIRE(preload.acquired());
    auto page = governor.reserve(SampleMemoryCategory::Page, 40);
    REQUIRE(page.acquired());
    REQUIRE(governor.reserve(SampleMemoryCategory::Page, 1).status ==
            SampleMemoryReserveStatus::BudgetExceeded);
    REQUIRE(governor.reserve(SampleMemoryCategory::Preload, 0).status ==
            SampleMemoryReserveStatus::InvalidByteCount);
    REQUIRE(governor.reserve(static_cast<SampleMemoryCategory>(255), 1).status ==
            SampleMemoryReserveStatus::InvalidCategory);

    const auto full = governor.stats();
    REQUIRE(full.current_preload_bytes == 60);
    REQUIRE(full.current_page_bytes == 40);
    REQUIRE(full.current_total_bytes == 100);
    REQUIRE(full.peak_preload_bytes == 60);
    REQUIRE(full.peak_page_bytes == 40);
    REQUIRE(full.peak_total_bytes == 100);
    REQUIRE(full.rejected_page_count == 1);
    REQUIRE(full.rejected_page_bytes == 1);
    REQUIRE(full.invalid_request_count == 2);

    preload.lease.reset();
    REQUIRE(governor.stats().current_total_bytes == 40);
    auto replacement = governor.reserve(SampleMemoryCategory::Page, 60);
    REQUIRE(replacement.acquired());
    REQUIRE(governor.stats().peak_page_bytes == 100);
    REQUIRE(governor.stats().peak_total_bytes == 100);
}

TEST_CASE("Sample memory leases move and release exactly once",
          "[audio][sampler][memory-governor]") {
    SampleMemoryGovernor governor;
    REQUIRE(governor.prepare(64));

    auto result = governor.reserve(SampleMemoryCategory::Preload, 64);
    REQUIRE(result.acquired());
    auto lease = std::move(result.lease);
    REQUIRE_FALSE(result.lease);
    REQUIRE(lease.bytes() == 64);
    REQUIRE(lease.category() == SampleMemoryCategory::Preload);
    REQUIRE_FALSE(governor.release());
    REQUIRE_FALSE(governor.prepare(128));

    lease.reset();
    REQUIRE(governor.stats().current_total_bytes == 0);
    REQUIRE(governor.release());
    REQUIRE_FALSE(governor.prepared());
    REQUIRE(governor.reserve(SampleMemoryCategory::Page, 1).status ==
            SampleMemoryReserveStatus::NotPrepared);
}

TEST_CASE("Sample memory lease safely outlives its governor facade",
          "[audio][sampler][memory-governor]") {
    pulp::audio::SampleMemoryLease retained;
    {
        SampleMemoryGovernor governor;
        REQUIRE(governor.prepare(32));
        auto result = governor.reserve(SampleMemoryCategory::Page, 32);
        REQUIRE(result.acquired());
        retained = std::move(result.lease);
    }
    REQUIRE(retained);
    retained.reset();
    REQUIRE_FALSE(retained);
}

TEST_CASE("Sample memory handles survive facade destruction and safe move construction",
          "[audio][sampler][memory-governor]") {
    pulp::audio::SampleMemoryGovernorHandle retained_handle;
    {
        SampleMemoryGovernor governor;
        REQUIRE(governor.prepare(32));
        retained_handle = governor.handle();
    }
    auto retained = retained_handle.reserve(SampleMemoryCategory::Page, 32);
    REQUIRE(retained.acquired());
    REQUIRE(retained_handle.stats().current_page_bytes == 32);
    retained.lease.reset();

    SampleMemoryGovernor original;
    REQUIRE(original.prepare(64));
    auto first = original.reserve(SampleMemoryCategory::Preload, 32);
    REQUIRE(first.acquired());
    auto before_move = original.handle();
    SampleMemoryGovernor moved(std::move(original));
    REQUIRE_FALSE(original.prepared());
    auto second = moved.reserve(SampleMemoryCategory::Page, 32);
    REQUIRE(second.acquired());
    REQUIRE(before_move.stats().current_total_bytes == 64);
    REQUIRE(moved.reserve(SampleMemoryCategory::Page, 1).status ==
            SampleMemoryReserveStatus::BudgetExceeded);
}

TEST_CASE("Explicit governor release and reprepare revoke old issuer handles",
          "[audio][sampler][memory-governor]") {
    SampleMemoryGovernor governor;
    REQUIRE(governor.prepare(32));
    auto first_epoch = governor.handle();
    REQUIRE(governor.prepare(64));
    REQUIRE_FALSE(first_epoch.prepared());
    REQUIRE(first_epoch.reserve(SampleMemoryCategory::Page, 1).status ==
            SampleMemoryReserveStatus::NotPrepared);

    auto second_epoch = governor.handle();
    REQUIRE(governor.release());
    REQUIRE_FALSE(second_epoch.prepared());
    REQUIRE(second_epoch.reserve(SampleMemoryCategory::Preload, 1).status ==
            SampleMemoryReserveStatus::NotPrepared);
}

TEST_CASE("Sample stream service retains a shared issuer after facade destruction",
          "[audio][sampler][memory-governor][stream-service]") {
    pulp::audio::SampleStreamCacheService service;
    {
        SampleMemoryGovernor governor;
        REQUIRE(governor.prepare(64));
        REQUIRE(service.prepare({
            .scheduler_capacity = 4,
            .page_memory_budget_bytes = 1,
            .memory_governor = governor.handle(),
        }));
    }

    const auto added = service.add_source(
        {
            .token = {1, 1},
            .channels = 1,
            .total_frames = 16,
            .page_frames = 4,
            .cache_page_count = 2,
        },
        [](std::uint64_t, pulp::audio::BufferView<float>, std::uint64_t frames) {
            return frames;
        });
    REQUIRE(added.added());
    REQUIRE(service.stats().memory.capacity_bytes == 64);
    REQUIRE(service.stats().memory.current_page_bytes == 32);
    service.release();
}

TEST_CASE("Sample memory storage formula rejects every overflow axis",
          "[audio][sampler][memory-governor]") {
    REQUIRE(checked_sample_storage_bytes(2, 4, 3) == 96);
    REQUIRE_FALSE(checked_sample_storage_bytes(0, 4, 3));
    REQUIRE_FALSE(checked_sample_storage_bytes(2, 0, 3));
    REQUIRE_FALSE(checked_sample_storage_bytes(2, 4, 0));
    REQUIRE_FALSE(checked_sample_storage_bytes(2, 4, 3, 0));
    REQUIRE_FALSE(checked_sample_storage_bytes(
        std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint32_t>::max()));
}

TEST_CASE("Sample memory governor serializes concurrent exact-cap admission",
          "[audio][sampler][memory-governor]") {
    SampleMemoryGovernor governor;
    REQUIRE(governor.prepare(64));

    std::atomic<bool> start{false};
    std::atomic<std::uint32_t> admitted{0};
    std::vector<std::thread> workers;
    std::vector<pulp::audio::SampleMemoryLease> leases(16);
    workers.reserve(leases.size());
    for (std::size_t index = 0; index < leases.size(); ++index) {
        workers.emplace_back([&, index] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            auto result = governor.reserve(
                index % 2 == 0 ? SampleMemoryCategory::Preload
                               : SampleMemoryCategory::Page,
                8);
            if (result.acquired()) {
                leases[index] = std::move(result.lease);
                admitted.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();

    REQUIRE(admitted.load(std::memory_order_relaxed) == 8);
    const auto stats = governor.stats();
    REQUIRE(stats.current_total_bytes == 64);
    REQUIRE(stats.peak_total_bytes == 64);
    REQUIRE(stats.rejected_preload_count + stats.rejected_page_count == 8);

    leases.clear();
    REQUIRE(governor.stats().current_total_bytes == 0);
}

TEST_CASE("Sample asset takes move-owned governed preload without a second sample store",
          "[audio][sampler][memory-governor][sample-asset]") {
    SampleMemoryGovernor governor;
    REQUIRE(governor.prepare(32));
    auto reservation = governor.reserve(SampleMemoryCategory::Preload, 32);
    REQUIRE(reservation.acquired());

    pulp::audio::Buffer<float> preload(2, 4);
    preload.channel(0)[3] = 7.0f;
    preload.channel(1)[0] = 11.0f;
    const auto* original_samples = preload.channel(0).data();
    pulp::audio::SampleAsset asset;
    const pulp::audio::SampleAssetConfig config{
        .asset = {1, 1},
        .source = {2, 1},
        .channels = 2,
        .total_frames = 4,
        .sample_rate = 48000.0,
        .preload_frames = 4,
    };
    REQUIRE(asset.prepare_owned(config, std::move(preload),
                                std::move(reservation.lease)));
    REQUIRE(preload.num_channels() == 0);
    REQUIRE(asset.view().preload_channel_data(0) == original_samples);
    REQUIRE(asset.view().preload_channel_data(0)[3] == 7.0f);
    REQUIRE(asset.view().preload_channel_data(1)[0] == 11.0f);
    REQUIRE(governor.stats().current_preload_bytes == 32);

    asset.release();
    REQUIRE(governor.stats().current_total_bytes == 0);
}

TEST_CASE("Sample asset rejects mismatched governed preload leases without leaking",
          "[audio][sampler][memory-governor][sample-asset]") {
    SampleMemoryGovernor governor;
    REQUIRE(governor.prepare(64));
    const pulp::audio::SampleAssetConfig config{
        .asset = {1, 1},
        .source = {2, 1},
        .channels = 2,
        .total_frames = 4,
        .sample_rate = 48000.0,
        .preload_frames = 4,
    };

    auto wrong_category = governor.reserve(SampleMemoryCategory::Page, 32);
    REQUIRE(wrong_category.acquired());
    pulp::audio::SampleAsset asset;
    REQUIRE_FALSE(asset.prepare_owned(config, pulp::audio::Buffer<float>(2, 4),
                                      std::move(wrong_category.lease)));
    REQUIRE(governor.stats().current_total_bytes == 0);

    auto wrong_size = governor.reserve(SampleMemoryCategory::Preload, 16);
    REQUIRE(wrong_size.acquired());
    REQUIRE_FALSE(asset.prepare_owned(config, pulp::audio::Buffer<float>(2, 4),
                                      std::move(wrong_size.lease)));
    REQUIRE(governor.stats().current_total_bytes == 0);

    auto undersized_lease = governor.reserve(SampleMemoryCategory::Preload, 32);
    REQUIRE(undersized_lease.acquired());
    REQUIRE_FALSE(asset.prepare_owned(config, pulp::audio::Buffer<float>(2, 8),
                                      std::move(undersized_lease.lease)));
    REQUIRE(governor.stats().current_total_bytes == 0);

    pulp::audio::Buffer<float> retained_capacity(2, 8);
    retained_capacity.resize(2, 4);
    REQUIRE(retained_capacity.allocated_sample_capacity() > 8);
    auto retained_capacity_lease =
        governor.reserve(SampleMemoryCategory::Preload, 32);
    REQUIRE(retained_capacity_lease.acquired());
    REQUIRE_FALSE(asset.prepare_owned(config, std::move(retained_capacity),
                                      std::move(retained_capacity_lease.lease)));
    REQUIRE(retained_capacity.num_channels() == 0);
    REQUIRE(governor.stats().current_total_bytes == 0);
}

TEST_CASE("Sample asset rejects a preload lease from a different page governor",
          "[audio][sampler][memory-governor][sample-asset]") {
    SampleMemoryGovernor page_governor;
    SampleMemoryGovernor foreign_governor;
    REQUIRE(page_governor.prepare(64));
    REQUIRE(foreign_governor.prepare(64));

    pulp::audio::SampleStreamCacheService service;
    REQUIRE(service.prepare({
        .scheduler_capacity = 4,
        .memory_governor = page_governor.handle(),
    }));
    const pulp::audio::SampleStreamSourceToken source{7, 1};
    const auto added = service.add_source(
        {
            .token = source,
            .channels = 1,
            .total_frames = 16,
            .page_frames = 4,
            .cache_page_count = 2,
        },
        [](std::uint64_t, pulp::audio::BufferView<float>, std::uint64_t frames) {
            return frames;
        });
    REQUIRE(added.added());
    REQUIRE(page_governor.stats().current_page_bytes == 32);

    auto foreign = foreign_governor.reserve(SampleMemoryCategory::Preload, 32);
    REQUIRE(foreign.acquired());
    const pulp::audio::SampleAssetConfig config{
        .asset = {7, 1},
        .source = source,
        .channels = 1,
        .total_frames = 16,
        .sample_rate = 48000.0,
        .preload_frames = 8,
        .preload_contract = pulp::audio::SamplePreloadContract{
            .source_sample_rate = 48000.0,
            .host_sample_rate = 48000.0,
            .maximum_playback_ratio = 1.0,
            .maximum_host_block_frames = 1,
            .interpolation_guard_frames = 1,
            .configured_preload_frames = 8,
        },
        .stream_source = added.view,
    };
    pulp::audio::SampleAsset asset;
    REQUIRE_FALSE(asset.prepare_owned(config,
                                      pulp::audio::Buffer<float>(1, 8),
                                      std::move(foreign.lease)));
    REQUIRE(foreign_governor.stats().current_total_bytes == 0);
    REQUIRE(page_governor.stats().current_total_bytes == 32);
}

TEST_CASE("One and two streamed bundles share the cap and release on rollback and retirement",
          "[audio][sampler][memory-governor][stream-service]") {
    SampleMemoryGovernor governor;
    REQUIRE(governor.prepare(128));
    pulp::audio::SampleStreamCacheService service;
    REQUIRE(service.prepare({
        .scheduler_capacity = 8,
        .page_memory_budget_bytes = 0,
        .memory_governor = governor.handle(),
    }));

    struct Bundle {
        std::unique_ptr<pulp::audio::SampleAsset> asset;
        pulp::audio::SampleStreamSourceToken source{};
    };
    const auto acquire_bundle = [&](std::uint64_t id) -> std::optional<Bundle> {
        auto preload = governor.reserve(SampleMemoryCategory::Preload, 32);
        if (!preload.acquired()) return std::nullopt;
        const pulp::audio::SampleStreamSourceToken source{id, 1};
        const auto added = service.add_source(
            {
                .token = source,
                .channels = 1,
                .total_frames = 16,
                .page_frames = 4,
                .cache_page_count = 2,
            },
            [](std::uint64_t, pulp::audio::BufferView<float>, std::uint64_t frames) {
                return frames;
            });
        if (!added.added()) return std::nullopt;

        auto asset = std::make_unique<pulp::audio::SampleAsset>();
        const pulp::audio::SampleAssetConfig config{
            .asset = {id, 1},
            .source = source,
            .channels = 1,
            .total_frames = 16,
            .sample_rate = 48000.0,
            .preload_frames = 8,
            .preload_contract = pulp::audio::SamplePreloadContract{
                .source_sample_rate = 48000.0,
                .host_sample_rate = 48000.0,
                .maximum_playback_ratio = 1.0,
                .maximum_host_block_frames = 1,
                .interpolation_guard_frames = 1,
                .configured_preload_frames = 8,
            },
            .stream_source = added.view,
        };
        if (!asset->prepare_owned(config,
                                  pulp::audio::Buffer<float>(1, 8),
                                  std::move(preload.lease))) {
            REQUIRE(service.discard_unpublished_source(source));
            return std::nullopt;
        }
        return Bundle{std::move(asset), source};
    };

    auto first = acquire_bundle(1);
    REQUIRE(first);
    auto one = service.stats().memory;
    REQUIRE(one.current_preload_bytes == 32);
    REQUIRE(one.current_page_bytes == 32);
    REQUIRE(one.current_total_bytes == 64);

    auto second = acquire_bundle(2);
    REQUIRE(second);
    const auto two = service.stats().memory;
    REQUIRE(two.current_preload_bytes == 64);
    REQUIRE(two.current_page_bytes == 64);
    REQUIRE(two.current_total_bytes == 128);
    REQUIRE(two.peak_total_bytes == 128);
    REQUIRE_FALSE(acquire_bundle(3));
    REQUIRE(governor.stats().current_total_bytes == 128);
    REQUIRE(governor.stats().rejected_preload_count == 1);

    REQUIRE(service.update_audio_generations(7, 6));
    second->asset->release();
    REQUIRE(service.retire_source_after_asset_unpublish(second->source) ==
            pulp::audio::SampleStreamSourceRetireStatus::Scheduled);
    REQUIRE(service.collect_retired_sources() == 0);
    REQUIRE(governor.stats().current_total_bytes == 96);
    REQUIRE(service.update_audio_generations(7, 7));
    REQUIRE(service.collect_retired_sources() == 1);
    REQUIRE(governor.stats().current_total_bytes == 64);

    first->asset->release();
    REQUIRE(service.discard_unpublished_source(first->source));
    REQUIRE(governor.stats().current_preload_bytes == 0);
    REQUIRE(governor.stats().current_page_bytes == 0);
    REQUIRE(governor.stats().current_total_bytes == 0);
}
