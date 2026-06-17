// test_zone_layer_select.cpp — multi-layer (multi-mic / velocity-layer)
// selection over a SampleZoneMap.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/sample_zone_map.hpp>
#include <pulp/audio/zone_layer_select.hpp>

#include <array>
#include <vector>

using pulp::audio::SampleZone;
using pulp::audio::SampleZoneMap;
using pulp::audio::ZoneSelection;
using pulp::audio::ZoneSelectionRequest;
using pulp::audio::select_layers;

namespace {

SampleZone make_zone(std::uint32_t sample_id, int key_lo, int key_hi,
                     int vel_lo, int vel_hi, std::uint32_t rr_group = 0,
                     std::uint32_t rr_step_index = 0) {
    SampleZone z;
    z.sample_id = sample_id;          // sample_id alone makes a zone valid
    z.root_note = 60;
    z.lowest_note = key_lo;
    z.highest_note = key_hi;
    z.lowest_velocity = vel_lo;
    z.highest_velocity = vel_hi;
    z.round_robin_group = rr_group;
    (void)rr_step_index;
    return z;
}

ZoneSelectionRequest req(int note, int velocity, std::uint32_t rr_step = 0) {
    ZoneSelectionRequest r;
    r.note = note;
    r.velocity = velocity;
    r.round_robin_step = rr_step;
    r.host_sample_rate = 0.0;  // selection/pitch-only
    return r;
}

}  // namespace

TEST_CASE("select_layers emits every matching multi-mic layer", "[audio][zone][multimic]") {
    SampleZoneMap map;
    std::array<SampleZone, 3> zones{
        make_zone(1, 0, 127, 1, 127),   // mic A
        make_zone(2, 0, 127, 1, 127),   // mic B
        make_zone(3, 0, 127, 1, 127),   // mic C
    };
    REQUIRE(map.configure(zones));

    std::array<ZoneSelection, 8> out{};
    const auto n = select_layers(map, req(60, 100), out.data(), out.size());
    REQUIRE(n == 3);
    std::vector<std::uint32_t> ids;
    for (std::size_t i = 0; i < n; ++i) ids.push_back(out[i].zone.sample_id);
    REQUIRE(ids == std::vector<std::uint32_t>{1, 2, 3});
}

TEST_CASE("select_layers picks the matching velocity layer", "[audio][zone][velocity]") {
    SampleZoneMap map;
    std::array<SampleZone, 2> zones{
        make_zone(1, 0, 127, 1, 63),     // soft layer
        make_zone(2, 0, 127, 64, 127),   // loud layer
    };
    REQUIRE(map.configure(zones));

    std::array<ZoneSelection, 4> out{};
    REQUIRE(select_layers(map, req(60, 100), out.data(), out.size()) == 1);
    REQUIRE(out[0].zone.sample_id == 2);
    REQUIRE(select_layers(map, req(60, 30), out.data(), out.size()) == 1);
    REQUIRE(out[0].zone.sample_id == 1);
}

TEST_CASE("select_layers rotates round-robin variants but keeps independent layers",
          "[audio][zone][roundrobin]") {
    SampleZoneMap map;
    std::array<SampleZone, 4> zones{
        make_zone(10, 0, 127, 1, 127, /*rr_group=*/5),   // RR variant 0
        make_zone(11, 0, 127, 1, 127, /*rr_group=*/5),   // RR variant 1
        make_zone(12, 0, 127, 1, 127, /*rr_group=*/5),   // RR variant 2
        make_zone(99, 0, 127, 1, 127, /*rr_group=*/0),   // independent mic layer
    };
    REQUIRE(map.configure(zones));

    std::array<ZoneSelection, 8> out{};
    for (std::uint32_t step = 0; step < 6; ++step) {
        const auto n = select_layers(map, req(60, 100, step), out.data(), out.size());
        // One RR variant + the independent layer.
        REQUIRE(n == 2);
        bool saw_independent = false;
        std::uint32_t rr_id = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (out[i].zone.sample_id == 99) saw_independent = true;
            else rr_id = out[i].zone.sample_id;
        }
        REQUIRE(saw_independent);
        REQUIRE(rr_id == 10 + (step % 3));  // cycles 10,11,12
    }
}

TEST_CASE("select_layers respects max_out and skips non-matching keys",
          "[audio][zone][multimic]") {
    SampleZoneMap map;
    std::array<SampleZone, 3> zones{
        make_zone(1, 60, 60, 1, 127),
        make_zone(2, 60, 60, 1, 127),
        make_zone(3, 72, 72, 1, 127),   // different key — must not match note 60
    };
    REQUIRE(map.configure(zones));

    std::array<ZoneSelection, 1> capped{};
    REQUIRE(select_layers(map, req(60, 100), capped.data(), capped.size()) == 1);

    std::array<ZoneSelection, 8> out{};
    REQUIRE(select_layers(map, req(60, 100), out.data(), out.size()) == 2);
    REQUIRE(select_layers(map, req(72, 100), out.data(), out.size()) == 1);
    REQUIRE(out[0].zone.sample_id == 3);
}
