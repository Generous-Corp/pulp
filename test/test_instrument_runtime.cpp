#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/instrument_runtime.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <array>
#include <span>

using Catch::Matchers::WithinAbs;
using pulp::audio::InstrumentRuntime;
using pulp::audio::InstrumentTriggerResult;
using pulp::audio::PublishedSampleStore;
using pulp::audio::PublishedSampleStoreConfig;
using pulp::audio::SamplePool;
using pulp::audio::SamplePoolEntry;
using pulp::audio::SampleZone;
using pulp::audio::SampleZoneMap;
using pulp::audio::ZoneSelectionRequest;

namespace {

void prepare_store(PublishedSampleStore& store,
                   std::span<const float> samples,
                   double sample_rate = 48000.0) {
    REQUIRE(store.prepare(PublishedSampleStoreConfig{
        .slot_count = 2,
        .max_channels = 1,
        .max_frames_per_slot = samples.size(),
    }));
    REQUIRE(store.load_mono(samples.data(),
                            static_cast<int>(samples.size()),
                            sample_rate));
}

SamplePool make_pool(std::uint32_t sample_id, PublishedSampleStore& store) {
    SamplePool pool;
    std::array<SamplePoolEntry, 1> entries{SamplePoolEntry{
        .sample_id = sample_id,
        .store = &store,
        .view = store.read_published_view(),
    }};
    REQUIRE(pool.configure(entries));
    return pool;
}

SampleZoneMap make_zone_map(const SampleZone& zone) {
    SampleZoneMap map;
    std::array<SampleZone, 1> zones{zone};
    REQUIRE(map.configure(zones));
    return map;
}

}  // namespace

TEST_CASE("InstrumentRuntime resolves a pool-backed chromatic zone",
          "[audio][sampler][instrument]") {
    std::array<float, 2> samples{0.5f, 0.25f};
    PublishedSampleStore store;
    prepare_store(store, samples);
    auto pool = make_pool(100, store);
    auto zones = make_zone_map(SampleZone{
        .sample_id = 100,
        .root_note = 60,
        .lowest_note = 36,
        .highest_note = 84,
    });

    const auto root = InstrumentRuntime::trigger(
        zones,
        pool,
        ZoneSelectionRequest{.note = 60, .velocity = 100, .host_sample_rate = 48000.0});
    REQUIRE(root.valid);
    REQUIRE(root.sample.valid);
    REQUIRE(root.sample.sample_id == 100);
    REQUIRE(root.zone.zone.sample_id == 100);
    REQUIRE_THAT(root.zone.pitch_ratio, WithinAbs(1.0, 1.0e-12));
    REQUIRE_THAT(root.playback_rate, WithinAbs(1.0, 1.0e-12));
    REQUIRE_THAT(root.zone.playback_rate, WithinAbs(root.playback_rate, 1.0e-12));

    const auto octave = InstrumentRuntime::trigger(
        zones,
        pool,
        ZoneSelectionRequest{.note = 72, .velocity = 100, .host_sample_rate = 44100.0});
    REQUIRE(octave.valid);
    REQUIRE_THAT(octave.zone.pitch_ratio, WithinAbs(2.0, 1.0e-12));
    REQUIRE_THAT(octave.playback_rate, WithinAbs(2.0, 1.0e-12));
    REQUIRE_THAT(octave.zone.playback_rate, WithinAbs(octave.playback_rate, 1.0e-12));
}

TEST_CASE("InstrumentRuntime supports pool-backed fixed-pitch zones",
          "[audio][sampler][instrument]") {
    std::array<float, 2> samples{0.0f, 1.0f};
    PublishedSampleStore store;
    prepare_store(store, samples);
    auto pool = make_pool(200, store);
    auto zones = make_zone_map(SampleZone{
        .sample_id = 200,
        .keytrack_cents_per_key = 0.0,
        .tune_semitones = -12.0,
    });

    const auto low = InstrumentRuntime::trigger(
        zones,
        pool,
        ZoneSelectionRequest{.note = 36, .velocity = 100, .host_sample_rate = 48000.0});
    const auto high = InstrumentRuntime::trigger(
        zones,
        pool,
        ZoneSelectionRequest{.note = 84, .velocity = 100, .host_sample_rate = 48000.0});
    REQUIRE(low.valid);
    REQUIRE(high.valid);
    REQUIRE_THAT(low.playback_rate, WithinAbs(0.5, 1.0e-12));
    REQUIRE_THAT(high.playback_rate, WithinAbs(0.5, 1.0e-12));
}

TEST_CASE("InstrumentRuntime rejects stale pool-backed zone samples",
          "[audio][sampler][instrument][generation]") {
    std::array<float, 2> first{0.1f, 0.2f};
    std::array<float, 2> second{0.3f, 0.4f};
    std::array<float, 2> third{0.5f, 0.6f};
    PublishedSampleStore store;
    prepare_store(store, first);

    auto pool = make_pool(300, store);
    auto zones = make_zone_map(SampleZone{.sample_id = 300});
    REQUIRE(InstrumentRuntime::trigger(
        zones,
        pool,
        ZoneSelectionRequest{.note = 60, .velocity = 100, .host_sample_rate = 48000.0}).valid);

    REQUIRE(store.load_mono(second.data(),
                            static_cast<int>(second.size()),
                            48000.0));
    const auto second_generation = store.read_published_view().generation;
    REQUIRE(store.load_mono(third.data(),
                            static_cast<int>(third.size()),
                            48000.0,
                            second_generation));

    REQUIRE_FALSE(InstrumentRuntime::trigger(
        zones,
        pool,
        ZoneSelectionRequest{.note = 60, .velocity = 100, .host_sample_rate = 48000.0}).valid);
}

TEST_CASE("InstrumentRuntime trigger resolution does not allocate",
          "[audio][sampler][instrument][rt]") {
    std::array<float, 2> samples{0.4f, 0.2f};
    PublishedSampleStore store;
    prepare_store(store, samples);
    auto pool = make_pool(400, store);
    auto zones = make_zone_map(SampleZone{
        .sample_id = 400,
        .root_note = 60,
    });

    InstrumentTriggerResult result;
    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        result = InstrumentRuntime::trigger(
            zones,
            pool,
            ZoneSelectionRequest{.note = 72, .velocity = 100, .host_sample_rate = 48000.0});
    }

    REQUIRE(result.valid);
    REQUIRE(result.sample.sample_id == 400);
    REQUIRE_THAT(result.playback_rate, WithinAbs(2.0, 1.0e-12));
}
