// MidiParameterMap value scaling: a mapped CC sweep drives its parameter over a
// configurable normalized window (with inversion + clamping) instead of always
// spanning the full range. Covers the scaled-apply math, the inversion path,
// edge cases (unmapped CC ignored, out-of-window clamping, channel filter,
// remove-map), and asserts the audio-thread apply path is allocation-free.

#include <pulp/state/midi_parameter_map.hpp>
#include <pulp/state/store.hpp>
#include <pulp/timeline/automation_lane.hpp>
#include <pulp/timeline/item_id.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;
using pulp::state::MidiMapScale;
using pulp::state::MidiParameterMap;
using pulp::state::StateStore;

namespace {

// A unit-range param (1) and a bipolar param (2) so scaling is visible both in
// normalized space and after denormalization through a non-[0,1] range.
void populate(StateStore& s) {
    s.add_parameter({.id = 1, .name = "Mix", .unit = "", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    s.add_parameter({.id = 2, .name = "Pan", .unit = "", .range = {-1.0f, 1.0f, 0.0f, 0.0f}});
}

} // namespace

TEST_CASE("MidiMapScale maps a CC sweep onto a normalized window",
          "[state][midi-map][scaling]") {
    // A window of [0.25, 0.75] means the full 0..127 CC sweep only covers the
    // middle half of the parameter's normalized range.
    const MidiMapScale scale{0.25f, 0.75f};
    CHECK_THAT(scale.apply(0), WithinAbs(0.25f, 1e-6f));
    CHECK_THAT(scale.apply(127), WithinAbs(0.75f, 1e-6f));
    CHECK_THAT(scale.apply(64), WithinAbs(0.25f + (64.0f / 127.0f) * 0.5f, 1e-6f));

    // The default window is the identity map — unscaled behavior.
    const MidiMapScale full{};
    CHECK_THAT(full.apply(0), WithinAbs(0.0f, 1e-6f));
    CHECK_THAT(full.apply(127), WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("MidiParameterMap applies a scaled window to its parameter",
          "[state][midi-map][scaling]") {
    StateStore store;
    populate(store);
    MidiParameterMap map;
    map.set_mapping(0, 74, 1, MidiMapScale{0.25f, 0.75f});
    map.pump();

    map.handle_cc(store, 0, 74, 0);
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.25f, 1e-3f));
    map.handle_cc(store, 0, 74, 127);
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.75f, 1e-3f));
    map.handle_cc(store, 0, 74, 64);
    CHECK_THAT(store.get_normalized(1),
               WithinAbs(0.25f + (64.0f / 127.0f) * 0.5f, 1e-3f));
}

TEST_CASE("MidiParameterMap scaling drives a non-unit parameter range",
          "[state][midi-map][scaling]") {
    // Param 2 spans [-1, 1]. A full-window mapping should reach both extremes;
    // the normalized 0.5 midpoint lands at 0.0 in real units.
    StateStore store;
    populate(store);
    MidiParameterMap map;
    map.set_mapping(0, 10, 2); // full range
    map.pump();

    map.handle_cc(store, 0, 10, 0);
    CHECK_THAT(store.get_value(2), WithinAbs(-1.0f, 1e-3f));
    map.handle_cc(store, 0, 10, 127);
    CHECK_THAT(store.get_value(2), WithinAbs(1.0f, 1e-3f));

    // Re-map onto the upper half [0.5, 1.0] → real [0.0, 1.0].
    map.set_mapping(0, 10, 2, MidiMapScale{0.5f, 1.0f});
    map.pump();
    map.handle_cc(store, 0, 10, 0);
    CHECK_THAT(store.get_value(2), WithinAbs(0.0f, 1e-3f));
    map.handle_cc(store, 0, 10, 127);
    CHECK_THAT(store.get_value(2), WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("MidiParameterMap inverts when the window runs high-to-low",
          "[state][midi-map][scaling]") {
    StateStore store;
    populate(store);
    MidiParameterMap map;
    map.set_mapping(0, 7, 1, MidiMapScale{1.0f, 0.0f}); // inverted
    map.pump();

    map.handle_cc(store, 0, 7, 0);
    CHECK_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f));
    map.handle_cc(store, 0, 7, 127);
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.0f, 1e-3f));
    map.handle_cc(store, 0, 7, 64);
    CHECK_THAT(store.get_normalized(1),
               WithinAbs(1.0f - (64.0f / 127.0f), 1e-3f));
}

TEST_CASE("MidiParameterMap clamps out-of-range window endpoints",
          "[state][midi-map][scaling]") {
    // Endpoints outside [0, 1] are clamped on insertion, so a caller passing
    // wild values can never drive the parameter past its normalized bounds.
    StateStore store;
    populate(store);
    MidiParameterMap map;
    map.set_mapping(0, 20, 1, MidiMapScale{-5.0f, 5.0f});
    map.pump();

    map.handle_cc(store, 0, 20, 0);
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.0f, 1e-3f)); // clamped from -5
    map.handle_cc(store, 0, 20, 127);
    CHECK_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f)); // clamped from +5
}

TEST_CASE("MidiParameterMap learn carries a scale window",
          "[state][midi-map][scaling]") {
    StateStore store;
    populate(store);
    MidiParameterMap map;
    map.arm_learn(1, MidiMapScale{0.0f, 0.5f});
    map.pump();
    REQUIRE(map.learn_armed());

    // The learned CC binds with the armed window and applies it immediately.
    map.handle_cc(store, 3, 30, 127);
    REQUIRE_FALSE(map.learn_armed());
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.5f, 1e-3f));
    map.handle_cc(store, 3, 30, 0);
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.0f, 1e-3f));
}

TEST_CASE("MidiParameterMap scaled mappings honor unmapped/channel/remove edges",
          "[state][midi-map][scaling]") {
    StateStore store;
    populate(store);
    MidiParameterMap map;
    map.set_mapping(5, 40, 1, MidiMapScale{0.25f, 0.75f}); // channel-specific
    map.pump();

    // Wrong channel: ignored, leaves the parameter at its default.
    map.handle_cc(store, 6, 40, 127);
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.0f, 1e-3f));
    // Right channel: applies the scaled window.
    map.handle_cc(store, 5, 40, 127);
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.75f, 1e-3f));
    // Unmapped CC on the right channel: ignored.
    map.handle_cc(store, 5, 41, 0);
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.75f, 1e-3f));

    // Remove the mapping: further CCs no longer move the parameter.
    map.clear(1);
    map.pump();
    map.handle_cc(store, 5, 40, 0);
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.75f, 1e-3f)); // unchanged
}

TEST_CASE("A hardware CC maps to any engine parameter with zero driver code",
          "[state][midi-map][scaling][timeline]") {
    // The Creative Timeline Engine's only automatable parameter surface is a
    // hosted device parameter, addressed by pulp::timeline::DeviceParameterTarget
    // whose `param_id` is the device's stable 32-bit host-facing ID — the same
    // pulp::state::ParamID a StateStore uses and MidiParameterMap drives. So no
    // per-controller or per-parameter driver code is needed: the engine's
    // parameter identity flows straight into the map as an opaque id.
    //
    // Prove it end to end against that engine type: take the id the engine would
    // automate, register it in the device's store, then learn a CC onto it and
    // drive it through the generic learn/apply loop — carrying a scale window so
    // the same call handles a controller that sweeps only part of the range.
    struct Knob {
        pulp::timeline::DeviceParameterTarget target;
        uint8_t channel;
        uint8_t cc;
        MidiMapScale scale;
    };
    const std::array<Knob, 3> knobs{{
        {{pulp::timeline::ItemId{7}, 21}, 0, 74, MidiMapScale{}},            // full range
        {{pulp::timeline::ItemId{7}, 22}, 3, 1, MidiMapScale{0.25f, 0.75f}}, // middle half
        {{pulp::timeline::ItemId{9}, 5}, 9, 7, MidiMapScale{1.0f, 0.0f}},    // inverted
    }};

    StateStore store;
    for (const auto& k : knobs)
        store.add_parameter({.id = k.target.param_id,
                             .name = "P",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 0.0f}});

    MidiParameterMap map;
    for (const auto& k : knobs) {
        map.arm_learn(k.target.param_id, k.scale);
        map.pump();
        map.handle_cc(store, k.channel, k.cc, 127);
        CHECK_THAT(store.get_normalized(k.target.param_id),
                   WithinAbs(k.scale.apply(127), 1e-3f));
        map.handle_cc(store, k.channel, k.cc, 0);
        CHECK_THAT(store.get_normalized(k.target.param_id),
                   WithinAbs(k.scale.apply(0), 1e-3f));
    }
}

TEST_CASE("MidiParameterMap scaled apply path does not allocate",
          "[state][midi-map][scaling][rt]") {
    StateStore store;
    populate(store);
    MidiParameterMap map;
    map.set_mapping(MidiParameterMap::kOmni, 74, 1, MidiMapScale{0.25f, 0.75f});
    map.set_mapping(0, 10, 2, MidiMapScale{0.5f, 1.0f});
    map.pump();

    // Warm the parameter-index lookups outside the probed region.
    map.handle_cc(store, 0, 74, 64);
    map.handle_cc(store, 0, 10, 64);

    {
        pulp::test::RtAllocationProbe probe;
        map.pump(); // draining an empty command queue must not allocate either
        for (uint8_t v = 0; v <= 120; v += 8) {
            map.handle_cc(store, 0, 74, v);
            map.handle_cc(store, 0, 10, v);
        }
        CHECK(probe.allocation_count() == 0);
    }
}
