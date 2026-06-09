#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/sample_zone_map.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <array>
#include <cmath>

using Catch::Matchers::WithinAbs;
using pulp::audio::kNoSampleSliceIndex;
using pulp::audio::PublishedSampleView;
using pulp::audio::SampleZone;
using pulp::audio::SampleZoneMap;
using pulp::audio::SliceRegion;
using pulp::audio::ZoneSelectionRequest;
using pulp::audio::ZoneSelector;

namespace {

PublishedSampleView sample(std::uint32_t slot,
                           std::uint64_t generation = 1,
                           double sample_rate = 48000.0) {
    return PublishedSampleView{
        .valid = true,
        .slot_index = slot,
        .generation = generation,
        .num_channels = 2,
        .num_frames = 48000,
        .sample_rate = sample_rate,
    };
}

}  // namespace

TEST_CASE("SampleZoneMap validates key, velocity, source, slice, and loop ranges",
          "[audio][sampler][zones]") {
    SampleZone valid{.sample = sample(0)};
    REQUIRE(SampleZoneMap::zone_valid(valid));

    REQUIRE_FALSE(SampleZoneMap::zone_valid(SampleZone{}));
    REQUIRE_FALSE(SampleZoneMap::zone_valid(SampleZone{
        .sample = PublishedSampleView{.valid = true, .sample_rate = 48000.0},
    }));
    REQUIRE_FALSE(SampleZoneMap::zone_valid(SampleZone{
        .sample = PublishedSampleView{
            .valid = true,
            .num_channels = 1,
            .num_frames = 12,
        },
    }));
    REQUIRE_FALSE(SampleZoneMap::zone_valid(
        SampleZone{.sample = sample(0), .lowest_note = 80, .highest_note = 20}));
    REQUIRE_FALSE(SampleZoneMap::zone_valid(
        SampleZone{.sample = sample(0), .lowest_velocity = 100, .highest_velocity = 1}));
    REQUIRE_FALSE(SampleZoneMap::zone_valid(
        SampleZone{.sample = sample(0), .keytrack_cents_per_key = std::nan("")}));
    REQUIRE_FALSE(SampleZoneMap::zone_valid(SampleZone{
        .sample = sample(0),
        .slice_index = 2,
        .slice_region = SliceRegion{.start_frame = 100, .end_frame = 10},
    }));
    REQUIRE_FALSE(SampleZoneMap::zone_valid(SampleZone{
        .sample = sample(0),
        .has_loop = true,
    }));

    SampleZoneMap map;
    std::array<SampleZone, 1> zones{valid};
    REQUIRE(map.configure(zones));
    REQUIRE(map.size() == 1);

    std::array<SampleZone, 1> invalid{SampleZone{}};
    REQUIRE_FALSE(map.configure(invalid));
    REQUIRE(map.size() == 1);
}

TEST_CASE("ZoneSelector maps one frozen sample chromatically across the keyboard",
          "[audio][sampler][zones]") {
    SampleZoneMap map;
    std::array<SampleZone, 1> zones{SampleZone{
        .sample = sample(3),
        .root_note = 60,
        .lowest_note = 0,
        .highest_note = 127,
    }};
    REQUIRE(map.configure(zones));

    const auto root = ZoneSelector::select(
        map,
        ZoneSelectionRequest{.note = 60, .velocity = 100, .host_sample_rate = 48000.0});
    REQUIRE(root.valid);
    REQUIRE(root.zone.sample.slot_index == 3);
    REQUIRE_THAT(root.pitch_ratio, WithinAbs(1.0, 1.0e-12));
    REQUIRE_THAT(root.playback_rate, WithinAbs(1.0, 1.0e-12));

    const auto octave = ZoneSelector::select(
        map,
        ZoneSelectionRequest{.note = 72, .velocity = 100, .host_sample_rate = 48000.0});
    REQUIRE(octave.valid);
    REQUIRE_THAT(octave.pitch_ratio, WithinAbs(2.0, 1.0e-12));

    const auto down = ZoneSelector::select(
        map,
        ZoneSelectionRequest{.note = 48, .velocity = 100, .host_sample_rate = 44100.0});
    REQUIRE(down.valid);
    REQUIRE_THAT(down.pitch_ratio, WithinAbs(0.5, 1.0e-12));
    REQUIRE_THAT(down.playback_rate, WithinAbs(0.5 * (48000.0 / 44100.0), 1.0e-12));

    const auto selection_only = ZoneSelector::select(
        map,
        ZoneSelectionRequest{.note = 72, .velocity = 100});
    REQUIRE(selection_only.valid);
    REQUIRE_THAT(selection_only.pitch_ratio, WithinAbs(2.0, 1.0e-12));
    REQUIRE_THAT(selection_only.playback_rate, WithinAbs(0.0, 1.0e-12));
}

TEST_CASE("ZoneSelector supports fixed-pitch trigger zones",
          "[audio][sampler][zones]") {
    SampleZoneMap map;
    std::array<SampleZone, 1> zones{SampleZone{
        .sample = sample(4),
        .root_note = 60,
        .keytrack_cents_per_key = 0.0,
        .tune_semitones = 12.0,
    }};
    REQUIRE(map.configure(zones));

    const auto low = ZoneSelector::select(
        map,
        ZoneSelectionRequest{.note = 36, .velocity = 100, .host_sample_rate = 48000.0});
    const auto high = ZoneSelector::select(
        map,
        ZoneSelectionRequest{.note = 84, .velocity = 100, .host_sample_rate = 48000.0});
    REQUIRE(low.valid);
    REQUIRE(high.valid);
    REQUIRE_THAT(low.pitch_ratio, WithinAbs(2.0, 1.0e-12));
    REQUIRE_THAT(high.pitch_ratio, WithinAbs(2.0, 1.0e-12));
}

TEST_CASE("ZoneSelector chooses key and velocity zones by priority",
          "[audio][sampler][zones][velocity]") {
    SampleZoneMap map;
    std::array<SampleZone, 3> zones{
        SampleZone{.sample = sample(1), .lowest_note = 36, .highest_note = 60, .highest_velocity = 80},
        SampleZone{.sample = sample(2), .lowest_note = 36, .highest_note = 60, .lowest_velocity = 81},
        SampleZone{.sample = sample(9), .lowest_note = 36, .highest_note = 60, .priority = 4},
    };
    REQUIRE(map.configure(zones));

    const auto soft = ZoneSelector::select(
        map,
        ZoneSelectionRequest{.note = 48, .velocity = 40, .host_sample_rate = 48000.0});
    REQUIRE(soft.valid);
    REQUIRE(soft.zone.sample.slot_index == 9);

    const auto loud = ZoneSelector::select(
        map,
        ZoneSelectionRequest{.note = 48, .velocity = 110, .host_sample_rate = 48000.0});
    REQUIRE(loud.valid);
    REQUIRE(loud.zone.sample.slot_index == 9);

    REQUIRE_FALSE(ZoneSelector::select(
        map,
        ZoneSelectionRequest{.note = 35, .velocity = 100, .host_sample_rate = 48000.0}).valid);
    REQUIRE_FALSE(ZoneSelector::select(
        map,
        ZoneSelectionRequest{.note = 48, .velocity = 0, .host_sample_rate = 48000.0}).valid);
}

TEST_CASE("ZoneSelector advances deterministic round-robin groups",
          "[audio][sampler][zones][round-robin]") {
    SampleZoneMap map;
    std::array<SampleZone, 4> zones{
        SampleZone{.sample = sample(0), .lowest_note = 60, .highest_note = 60, .priority = 3, .round_robin_group = 7},
        SampleZone{.sample = sample(1), .lowest_note = 60, .highest_note = 60, .priority = 3, .round_robin_group = 7},
        SampleZone{.sample = sample(2), .lowest_note = 60, .highest_note = 60, .priority = 3, .round_robin_group = 7},
        SampleZone{.sample = sample(9), .lowest_note = 60, .highest_note = 60, .priority = 1},
    };
    REQUIRE(map.configure(zones));

    for (std::uint32_t step = 0; step < 6; ++step) {
        const auto selection = ZoneSelector::select(
            map,
            ZoneSelectionRequest{
                .note = 60,
                .velocity = 100,
                .round_robin_step = step,
                .host_sample_rate = 48000.0,
            });
        REQUIRE(selection.valid);
        REQUIRE(selection.zone.sample.slot_index == step % 3);
    }
}

TEST_CASE("ZoneSelector keeps first configured equal-priority non-round-robin match",
          "[audio][sampler][zones][round-robin]") {
    SampleZoneMap map;
    std::array<SampleZone, 3> zones{
        SampleZone{.sample = sample(4), .lowest_note = 60, .highest_note = 60, .priority = 2},
        SampleZone{.sample = sample(5), .lowest_note = 60, .highest_note = 60, .priority = 2, .round_robin_group = 11},
        SampleZone{.sample = sample(6), .lowest_note = 60, .highest_note = 60, .priority = 2, .round_robin_group = 11},
    };
    REQUIRE(map.configure(zones));

    const auto selection = ZoneSelector::select(
        map,
        ZoneSelectionRequest{
            .note = 60,
            .velocity = 100,
            .round_robin_step = 1,
            .host_sample_rate = 48000.0,
        });
    REQUIRE(selection.valid);
    REQUIRE(selection.zone.sample.slot_index == 4);
}

TEST_CASE("SampleZoneMap preserves slice metadata for slice-per-key samplers",
          "[audio][sampler][zones][slice]") {
    SampleZoneMap map;
    std::array<SampleZone, 2> zones{
        SampleZone{
            .sample = sample(5),
            .lowest_note = 60,
            .highest_note = 60,
            .slice_index = 0,
            .slice_region = SliceRegion{.start_frame = 0, .end_frame = 120, .marker_index = 0},
        },
        SampleZone{
            .sample = sample(5),
            .lowest_note = 61,
            .highest_note = 61,
            .slice_index = 1,
            .slice_region = SliceRegion{.start_frame = 120, .end_frame = 240, .marker_index = 1},
        },
    };
    REQUIRE(map.configure(zones));

    const auto second = ZoneSelector::select(
        map,
        ZoneSelectionRequest{.note = 61, .velocity = 100, .host_sample_rate = 48000.0});
    REQUIRE(second.valid);
    REQUIRE(second.zone.slice_index == 1);
    REQUIRE(second.zone.slice_region.start_frame == 120);
    REQUIRE(second.zone.slice_region.end_frame == 240);
    REQUIRE(second.zone.slice_index != kNoSampleSliceIndex);
}

TEST_CASE("ZoneSelector hot operations do not allocate",
          "[audio][sampler][zones][rt]") {
    SampleZoneMap map;
    std::array<SampleZone, 2> zones{
        SampleZone{.sample = sample(6), .root_note = 60, .lowest_note = 36, .highest_note = 72},
        SampleZone{.sample = sample(7), .root_note = 60, .lowest_note = 36, .highest_note = 72, .priority = 2},
    };
    REQUIRE(map.configure(zones));

    pulp::audio::ZoneSelection selection;
    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        selection = ZoneSelector::select(
            map,
            ZoneSelectionRequest{.note = 72, .velocity = 100, .host_sample_rate = 48000.0});
    }

    REQUIRE(selection.valid);
    REQUIRE(selection.zone.sample.slot_index == 7);
    REQUIRE_THAT(selection.pitch_ratio, WithinAbs(2.0, 1.0e-12));
}
