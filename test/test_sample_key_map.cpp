#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/sample_key_map.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <cmath>

using Catch::Matchers::WithinAbs;
using pulp::audio::SampleKeyMap;
using pulp::audio::SampleKeyMapConfig;
using pulp::audio::SampleKeySliceResolution;
using pulp::audio::SliceMap;
using pulp::audio::SliceMarker;
using pulp::audio::SliceMarkerSource;
using pulp::audio::SliceRegion;

TEST_CASE("SampleKeyMap validates MIDI note ranges", "[audio][sampler][keys]") {
    REQUIRE(SampleKeyMap::config_valid({}));
    REQUIRE_FALSE(SampleKeyMap::config_valid(
        SampleKeyMapConfig{.root_note = -1}));
    REQUIRE_FALSE(SampleKeyMap::config_valid(
        SampleKeyMapConfig{.lowest_note = 80, .highest_note = 20}));
    REQUIRE_FALSE(SampleKeyMap::config_valid(
        SampleKeyMapConfig{.first_slice_note = 128}));

    SampleKeyMap map;
    REQUIRE(map.configure(SampleKeyMapConfig{.lowest_note = 36, .highest_note = 72}));
    REQUIRE(map.accepts_note(36));
    REQUIRE(map.accepts_note(72));
    REQUIRE_FALSE(map.accepts_note(35));
    REQUIRE_FALSE(map.accepts_note(73));

    REQUIRE_FALSE(map.configure(SampleKeyMapConfig{.lowest_note = 90, .highest_note = 30}));
    REQUIRE(map.accepts_note(36));
}

TEST_CASE("SampleKeyMap maps one sample chromatically across keys",
          "[audio][sampler][keys]") {
    SampleKeyMap map;
    REQUIRE(map.configure(SampleKeyMapConfig{.root_note = 60}));

    REQUIRE_THAT(map.playback_rate_for_note(60, 48000.0, 48000.0),
                 WithinAbs(1.0, 1.0e-12));
    REQUIRE_THAT(map.playback_rate_for_note(72, 48000.0, 48000.0),
                 WithinAbs(2.0, 1.0e-12));
    REQUIRE_THAT(map.playback_rate_for_note(48, 48000.0, 48000.0),
                 WithinAbs(0.5, 1.0e-12));
    REQUIRE_THAT(map.playback_rate_for_note(60, 48000.0, 44100.0),
                 WithinAbs(48000.0 / 44100.0, 1.0e-12));
    REQUIRE_THAT(map.playback_rate_for_note(60, 48000.0, 48000.0, 12.0),
                 WithinAbs(2.0, 1.0e-12));
}

TEST_CASE("SampleKeyMap supports keytracking policy without renderer coupling",
          "[audio][sampler][keys]") {
    SampleKeyMap fixed_pitch;
    REQUIRE(fixed_pitch.configure(
        SampleKeyMapConfig{.root_note = 60, .keytrack_cents_per_key = 0.0}));
    REQUIRE_THAT(fixed_pitch.pitch_ratio_for_note(72), WithinAbs(1.0, 1.0e-12));

    SampleKeyMap half_tracking;
    REQUIRE(half_tracking.configure(
        SampleKeyMapConfig{.root_note = 60, .keytrack_cents_per_key = 50.0}));
    REQUIRE_THAT(half_tracking.pitch_ratio_for_note(72),
                 WithinAbs(std::sqrt(2.0), 1.0e-12));
}

TEST_CASE("SampleKeyMap resolves one-slice-per-key maps",
          "[audio][sampler][keys][slice]") {
    SliceMap slices;
    slices.source_generation = 12;
    slices.source_frames = 400;
    slices.source_sample_rate = 48000.0;
    slices.markers = {
        SliceMarker{.frame = 0, .confidence = 1.0, .source = SliceMarkerSource::Onset},
        SliceMarker{.frame = 100, .confidence = 1.0, .source = SliceMarkerSource::Onset},
        SliceMarker{.frame = 240, .confidence = 1.0, .source = SliceMarkerSource::Onset},
    };
    slices.regions = {
        SliceRegion{.start_frame = 0, .end_frame = 100, .marker_index = 0},
        SliceRegion{.start_frame = 100, .end_frame = 240, .marker_index = 1},
        SliceRegion{.start_frame = 240, .end_frame = 400, .marker_index = 2},
    };

    SampleKeyMap map;
    REQUIRE(map.configure(SampleKeyMapConfig{
        .lowest_note = 36,
        .highest_note = 48,
        .first_slice_note = 36,
    }));

    const auto first = map.resolve_slice_for_note(36, slices);
    REQUIRE(first.valid);
    REQUIRE(first.slice_index == 0);
    REQUIRE(first.region.start_frame == 0);
    REQUIRE(first.region.end_frame == 100);

    const auto third = map.resolve_slice_for_note(38, slices);
    REQUIRE(third.valid);
    REQUIRE(third.slice_index == 2);
    REQUIRE(third.region.start_frame == 240);
    REQUIRE(third.region.end_frame == 400);

    REQUIRE_FALSE(map.resolve_slice_for_note(39, slices).valid);
    REQUIRE_FALSE(map.resolve_slice_for_note(35, slices).valid);
}

TEST_CASE("SampleKeyMap slice helper matches resolve and PulpTempo root bounds",
          "[audio][sampler][keys][slice]") {
    SampleKeyMap map;
    REQUIRE(map.configure(SampleKeyMapConfig{
        .root_note = 48,
        .lowest_note = 0,
        .highest_note = 127,
        .first_slice_note = 48,
    }));

    SliceMap slices;
    slices.source_generation = 3;
    slices.source_frames = 400;
    slices.source_sample_rate = 48000.0;
    slices.markers = {
        {0, 1.0, SliceMarkerSource::Onset},
        {100, 1.0, SliceMarkerSource::Onset},
        {200, 1.0, SliceMarkerSource::Onset},
        {300, 1.0, SliceMarkerSource::Onset},
    };
    slices.regions = {
        {0, 100, 0},
        {100, 200, 1},
        {200, 300, 2},
        {300, 400, 3},
    };

    for (int note = 48; note < 52; ++note) {
        const auto index = map.slice_index_for_note(note, slices.regions.size());
        const auto resolved = map.resolve_slice_for_note(note, slices);
        REQUIRE(index);
        REQUIRE(resolved.valid);
        CHECK(*index == resolved.slice_index);
    }
    CHECK_FALSE(map.slice_index_for_note(47, slices.regions.size()));
    CHECK_FALSE(map.slice_index_for_note(52, slices.regions.size()));
    CHECK_FALSE(map.slice_index_for_note(-1, slices.regions.size()));
    CHECK_FALSE(map.slice_index_for_note(128, slices.regions.size()));
    CHECK(map.slice_index_for_note(48, 0) == std::nullopt);
    CHECK_THAT(map.pitch_ratio_for_note(48), WithinAbs(1.0, 1.0e-12));
    CHECK_THAT(map.pitch_ratio_for_note(60), WithinAbs(2.0, 1.0e-12));
}

TEST_CASE("SampleKeyMap hot operations do not require allocation",
          "[audio][sampler][keys][rt]") {
    SliceMap slices;
    slices.source_generation = 1;
    slices.source_frames = 128;
    slices.source_sample_rate = 48000.0;
    slices.markers = {
        SliceMarker{.frame = 0, .confidence = 1.0, .source = SliceMarkerSource::Onset},
        SliceMarker{.frame = 64, .confidence = 1.0, .source = SliceMarkerSource::Onset},
    };
    slices.regions = {
        SliceRegion{.start_frame = 0, .end_frame = 64, .marker_index = 0},
        SliceRegion{.start_frame = 64, .end_frame = 128, .marker_index = 1},
    };

    SampleKeyMap map;
    REQUIRE(map.configure(SampleKeyMapConfig{.first_slice_note = 60}));

    double rate = 0.0;
    SampleKeySliceResolution slice;
    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        rate = map.playback_rate_for_note(72, 48000.0, 48000.0);
        slice = map.resolve_slice_for_note(61, slices);
    }
    REQUIRE_THAT(rate, WithinAbs(2.0, 1.0e-12));
    REQUIRE(slice.valid);
    REQUIRE(slice.region.start_frame == 64);
}
