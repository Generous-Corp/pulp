#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/sample_pool.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <array>
#include <span>

using pulp::audio::PublishedSampleStore;
using pulp::audio::PublishedSampleStoreConfig;
using pulp::audio::SamplePool;
using pulp::audio::SamplePoolEntry;
using pulp::audio::SamplePoolResolution;
using pulp::audio::kInvalidSamplePoolId;

namespace {

void prepare_store(PublishedSampleStore& store, std::span<const float> samples) {
    REQUIRE(store.prepare(PublishedSampleStoreConfig{
        .slot_count = 2,
        .max_channels = 1,
        .max_frames_per_slot = samples.size(),
    }));
    REQUIRE(store.load_mono(samples.data(),
                            static_cast<int>(samples.size()),
                            48000.0));
}

}  // namespace

TEST_CASE("SamplePool validates entries and rejects duplicate ids",
          "[audio][sampler][pool]") {
    std::array<float, 4> a{0.0f, 0.25f, 0.5f, 0.75f};
    PublishedSampleStore store;
    prepare_store(store, a);

    REQUIRE(SamplePool::entry_valid(SamplePoolEntry{
        .sample_id = 10,
        .store = &store,
        .view = store.read_published_view(),
    }));
    REQUIRE_FALSE(SamplePool::entry_valid(SamplePoolEntry{
        .sample_id = kInvalidSamplePoolId,
        .store = &store,
        .view = store.read_published_view(),
    }));
    REQUIRE_FALSE(SamplePool::entry_valid(SamplePoolEntry{
        .sample_id = 11,
        .view = store.read_published_view(),
    }));
    REQUIRE_FALSE(SamplePool::entry_valid(SamplePoolEntry{
        .sample_id = 12,
        .store = &store,
    }));

    SamplePool pool;
    std::array<SamplePoolEntry, 1> entries{SamplePoolEntry{
        .sample_id = 10,
        .store = &store,
        .view = store.read_published_view(),
    }};
    REQUIRE(pool.configure(entries));
    REQUIRE(pool.size() == 1);

    std::array<SamplePoolEntry, 2> duplicates{
        SamplePoolEntry{.sample_id = 10, .store = &store, .view = store.read_published_view()},
        SamplePoolEntry{.sample_id = 10, .store = &store, .view = store.read_published_view()},
    };
    REQUIRE_FALSE(pool.configure(duplicates));
    REQUIRE(pool.size() == 1);
}

TEST_CASE("SamplePool resolves published samples by stable id",
          "[audio][sampler][pool]") {
    std::array<float, 3> kick{1.0f, 0.5f, 0.0f};
    std::array<float, 3> snare{-1.0f, -0.5f, 0.0f};
    PublishedSampleStore kick_store;
    PublishedSampleStore snare_store;
    prepare_store(kick_store, kick);
    prepare_store(snare_store, snare);

    SamplePool pool;
    std::array<SamplePoolEntry, 2> entries{
        SamplePoolEntry{
            .sample_id = 100,
            .store = &kick_store,
            .view = kick_store.read_published_view(),
        },
        SamplePoolEntry{
            .sample_id = 200,
            .store = &snare_store,
            .view = snare_store.read_published_view(),
        },
    };
    REQUIRE(pool.configure(entries));

    const auto kick_resolution = pool.resolve(100);
    REQUIRE(kick_resolution.valid);
    REQUIRE(kick_resolution.entry_index == 0);
    REQUIRE(kick_resolution.view.num_frames == 3);
    REQUIRE(SamplePool::channel_data(kick_resolution, 0) != nullptr);
    REQUIRE(SamplePool::channel_data(kick_resolution, 0)[0] == 1.0f);

    const auto snare_resolution = pool.resolve(200);
    REQUIRE(snare_resolution.valid);
    REQUIRE(snare_resolution.entry_index == 1);
    REQUIRE(SamplePool::channel_data(snare_resolution, 0)[0] == -1.0f);

    REQUIRE_FALSE(pool.resolve(999).valid);
    REQUIRE_FALSE(pool.resolve(kInvalidSamplePoolId).valid);
}

TEST_CASE("SamplePool populates channel pointers without owning sample storage",
          "[audio][sampler][pool]") {
    std::array<float, 2> samples{0.125f, 0.25f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    SamplePool pool;
    std::array<SamplePoolEntry, 1> entries{SamplePoolEntry{
        .sample_id = 1,
        .store = &store,
        .view = store.read_published_view(),
    }};
    REQUIRE(pool.configure(entries));

    const auto resolution = pool.resolve(1);
    REQUIRE(resolution.valid);

    std::array<const float*, 1> channels{};
    REQUIRE(SamplePool::populate_channel_ptrs(resolution,
                                              channels.data(),
                                              channels.size()));
    REQUIRE(channels[0] != nullptr);
    REQUIRE(channels[0][1] == 0.25f);

    std::array<const float*, 0> too_small{};
    REQUIRE_FALSE(SamplePool::populate_channel_ptrs(resolution,
                                                    too_small.data(),
                                                    too_small.size()));
}

TEST_CASE("SamplePool rejects entries after their published generation expires",
          "[audio][sampler][pool][generation]") {
    std::array<float, 2> first{0.1f, 0.2f};
    std::array<float, 2> second{0.3f, 0.4f};
    std::array<float, 2> third{0.5f, 0.6f};
    PublishedSampleStore store;
    prepare_store(store, first);

    SamplePool pool;
    const auto first_view = store.read_published_view();
    std::array<SamplePoolEntry, 1> entries{SamplePoolEntry{
        .sample_id = 7,
        .store = &store,
        .view = first_view,
    }};
    REQUIRE(pool.configure(entries));
    REQUIRE(pool.resolve(7).valid);

    REQUIRE(store.load_mono(second.data(),
                            static_cast<int>(second.size()),
                            48000.0));
    REQUIRE(pool.resolve(7).valid);

    const auto second_generation = store.read_published_view().generation;
    REQUIRE(store.load_mono(third.data(),
                            static_cast<int>(third.size()),
                            48000.0,
                            second_generation));
    REQUIRE_FALSE(pool.resolve(7).valid);
}

TEST_CASE("SamplePool hot resolve and channel lookup do not allocate",
          "[audio][sampler][pool][rt]") {
    std::array<float, 2> samples{0.2f, 0.4f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    SamplePool pool;
    std::array<SamplePoolEntry, 1> entries{SamplePoolEntry{
        .sample_id = 42,
        .store = &store,
        .view = store.read_published_view(),
    }};
    REQUIRE(pool.configure(entries));

    SamplePoolResolution resolution;
    const float* channel = nullptr;
    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        resolution = pool.resolve(42);
        channel = SamplePool::channel_data(resolution, 0);
    }

    REQUIRE(resolution.valid);
    REQUIRE(channel != nullptr);
    REQUIRE(channel[0] == 0.2f);
}
