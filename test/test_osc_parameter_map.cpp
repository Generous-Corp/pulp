// OscParameterMap: an OSC address (literal or OSC 1.0 wildcard pattern) binds to
// a parameter and drives it through a configurable input range and normalized
// output window. Covers the scaling math, wildcard vs literal dispatch, learn,
// argument typing, capacity/length limits, and asserts the literal-address
// dispatch path is allocation-free.

#include <pulp/osc/osc.hpp>
#include <pulp/osc/osc_parameter_map.hpp>
#include <pulp/state/store.hpp>
#include <pulp/timeline/automation_lane.hpp>
#include <pulp/timeline/item_id.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::osc::OscMapScale;
using pulp::osc::OscParameterMap;
using pulp::state::StateStore;

namespace {

// A unit-range param (1) and a bipolar param (2) so scaling is visible both in
// normalized space and after denormalization through a non-[0,1] range.
void populate(StateStore& s) {
    s.add_parameter({.id = 1, .name = "Mix", .unit = "", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    s.add_parameter({.id = 2, .name = "Pan", .unit = "", .range = {-1.0f, 1.0f, 0.0f, 0.0f}});
}

pulp::osc::Message float_message(std::string address, float value) {
    pulp::osc::Message msg{std::move(address)};
    msg.add(value);
    return msg;
}

} // namespace

TEST_CASE("OscMapScale maps an input range onto a normalized window", "[osc][osc-map][scaling]") {
    // The default is the identity map for a conventional 0..1 fader.
    const OscMapScale unit{};
    CHECK_THAT(unit.apply(0.0f), WithinAbs(0.0f, 1e-6f));
    CHECK_THAT(unit.apply(1.0f), WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(unit.apply(0.5f), WithinAbs(0.5f, 1e-6f));

    // A surface that sends 0..127 into the middle half of the parameter range.
    const OscMapScale scale{0.0f, 127.0f, 0.25f, 0.75f};
    CHECK_THAT(scale.apply(0.0f), WithinAbs(0.25f, 1e-6f));
    CHECK_THAT(scale.apply(127.0f), WithinAbs(0.75f, 1e-6f));
    CHECK_THAT(scale.apply(63.5f), WithinAbs(0.5f, 1e-6f));
}

TEST_CASE("OscMapScale clamps input outside the declared range", "[osc][osc-map][scaling]") {
    // OSC arguments are unbounded floats, so a sender that overshoots must not
    // be able to push the parameter past the window it was bound to.
    const OscMapScale scale{0.0f, 1.0f, 0.25f, 0.75f};
    CHECK_THAT(scale.apply(-10.0f), WithinAbs(0.25f, 1e-6f));
    CHECK_THAT(scale.apply(10.0f), WithinAbs(0.75f, 1e-6f));

    // A non-finite argument carries no usable position; it pins to the window
    // start rather than writing NaN into the parameter.
    CHECK_THAT(scale.apply(std::nanf("")), WithinAbs(0.25f, 1e-6f));

    // A degenerate (zero-width) input range also pins to the window start.
    const OscMapScale degenerate{1.0f, 1.0f, 0.2f, 0.9f};
    CHECK_THAT(degenerate.apply(0.0f), WithinAbs(0.2f, 1e-6f));
    CHECK_THAT(degenerate.apply(5.0f), WithinAbs(0.2f, 1e-6f));
}

TEST_CASE("OscParameterMap applies a scaled window to its parameter", "[osc][osc-map][scaling]") {
    StateStore store;
    populate(store);
    OscParameterMap map;
    REQUIRE(map.set_mapping("/track/1/fader", 1, OscMapScale{0.0f, 1.0f, 0.25f, 0.75f}));
    map.pump();
    REQUIRE(map.mapping_count() == 1);

    map.handle_message(store, float_message("/track/1/fader", 0.0f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.25f, 1e-3f));
    map.handle_message(store, float_message("/track/1/fader", 1.0f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.75f, 1e-3f));
    map.handle_message(store, float_message("/track/1/fader", 0.5f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.5f, 1e-3f));
}

TEST_CASE("OscParameterMap drives a non-unit parameter range", "[osc][osc-map][scaling]") {
    // Param 2 spans [-1, 1]. A full-window mapping reaches both extremes; the
    // normalized 0.5 midpoint lands at 0.0 in real units.
    StateStore store;
    populate(store);
    OscParameterMap map;
    REQUIRE(map.set_mapping("/pan", 2));
    map.pump();

    map.handle_message(store, float_message("/pan", 0.0f));
    CHECK_THAT(store.get_value(2), WithinAbs(-1.0f, 1e-3f));
    map.handle_message(store, float_message("/pan", 1.0f));
    CHECK_THAT(store.get_value(2), WithinAbs(1.0f, 1e-3f));

    // Re-binding the same address replaces the mapping rather than stacking a
    // second one: the upper half [0.5, 1.0] → real [0.0, 1.0].
    REQUIRE(map.set_mapping("/pan", 2, OscMapScale{0.0f, 1.0f, 0.5f, 1.0f}));
    map.pump();
    REQUIRE(map.mapping_count() == 1);
    map.handle_message(store, float_message("/pan", 0.0f));
    CHECK_THAT(store.get_value(2), WithinAbs(0.0f, 1e-3f));
    map.handle_message(store, float_message("/pan", 1.0f));
    CHECK_THAT(store.get_value(2), WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("OscParameterMap inverts when the output window runs high-to-low",
          "[osc][osc-map][scaling]") {
    StateStore store;
    populate(store);
    OscParameterMap map;
    REQUIRE(map.set_mapping("/gain", 1, OscMapScale{0.0f, 1.0f, 1.0f, 0.0f}));
    map.pump();

    map.handle_message(store, float_message("/gain", 0.0f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f));
    map.handle_message(store, float_message("/gain", 1.0f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.0f, 1e-3f));
    map.handle_message(store, float_message("/gain", 0.25f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.75f, 1e-3f));
}

TEST_CASE("OscParameterMap clamps out-of-range output window endpoints",
          "[osc][osc-map][scaling]") {
    // Output endpoints outside [0, 1] are clamped on insertion, so a caller
    // passing wild values can never drive the parameter past its bounds. This
    // is asserted through route(), which reports the value the map produced —
    // checking it through the store would prove nothing, because StateStore
    // constrains a written value to the parameter's range either way.
    OscParameterMap map;
    REQUIRE(map.set_mapping("/wild", 1, OscMapScale{0.0f, 1.0f, -5.0f, 5.0f}));
    map.pump();

    std::vector<float> routed;
    const auto collect = [&](pulp::state::ParamID, float n) { routed.push_back(n); };
    map.route("/wild", 0.0f, collect);
    map.route("/wild", 1.0f, collect);
    REQUIRE(routed.size() == 2);
    CHECK_THAT(routed[0], WithinAbs(0.0f, 1e-6f)); // clamped from -5
    CHECK_THAT(routed[1], WithinAbs(1.0f, 1e-6f)); // clamped from +5

    // And the clamped window is what actually reaches the parameter.
    StateStore store;
    populate(store);
    map.handle_message(store, float_message("/wild", 1.0f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("OscParameterMap routes wildcard address patterns", "[osc][osc-map][pattern]") {
    // OSC 1.0 pattern syntax lets one binding cover a family of addresses —
    // every track's fader, or a named subset.
    StateStore store;
    populate(store);
    OscParameterMap map;
    REQUIRE(map.set_mapping("/track/*/fader", 1));
    REQUIRE(map.set_mapping("/fx/{delay,reverb}/mix", 2));
    map.pump();
    REQUIRE(map.mapping_count() == 2);

    map.handle_message(store, float_message("/track/7/fader", 1.0f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f));
    map.handle_message(store, float_message("/track/12/fader", 0.0f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.0f, 1e-3f));

    // A wildcard segment does not cross '/', so a deeper address misses.
    map.handle_message(store, float_message("/track/7/eq/fader", 1.0f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.0f, 1e-3f));

    map.handle_message(store, float_message("/fx/reverb/mix", 1.0f));
    CHECK_THAT(store.get_normalized(2), WithinAbs(1.0f, 1e-3f));
    // An alternative outside the set is not routed.
    map.handle_message(store, float_message("/fx/chorus/mix", 0.0f));
    CHECK_THAT(store.get_normalized(2), WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("OscParameterMap classifies literal and wildcard patterns", "[osc][osc-map][pattern]") {
    // Literal addresses are matched by byte compare; only these four characters
    // promote a pattern to the (heavier) OSC matcher.
    CHECK_FALSE(OscParameterMap::is_wildcard("/track/1/fader"));
    CHECK(OscParameterMap::is_wildcard("/track/*/fader"));
    CHECK(OscParameterMap::is_wildcard("/track/?/fader"));
    CHECK(OscParameterMap::is_wildcard("/track/[12]/fader"));
    CHECK(OscParameterMap::is_wildcard("/fx/{a,b}/mix"));
}

TEST_CASE("OscParameterMap learn binds the next incoming address", "[osc][osc-map][learn]") {
    StateStore store;
    populate(store);
    OscParameterMap map;
    map.arm_learn(1, OscMapScale{0.0f, 1.0f, 0.0f, 0.5f});
    map.pump();
    REQUIRE(map.learn_armed());

    // The learned address binds with the armed window and applies immediately.
    map.handle_message(store, float_message("/surface/knob3", 1.0f));
    REQUIRE_FALSE(map.learn_armed());
    REQUIRE(map.mapping_count() == 1);
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.5f, 1e-3f));

    // And keeps responding on that address afterwards.
    map.handle_message(store, float_message("/surface/knob3", 0.0f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.0f, 1e-3f));

    // A different address is not routed — learn bound one address, not all.
    map.handle_message(store, float_message("/surface/knob4", 1.0f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.0f, 1e-3f));
}

TEST_CASE("OscParameterMap honors unmatched, remove, and capacity edges", "[osc][osc-map]") {
    StateStore store;
    populate(store);
    OscParameterMap map;
    REQUIRE(map.set_mapping("/mix", 1, OscMapScale{0.0f, 1.0f, 0.25f, 0.75f}));
    map.pump();

    // An address with no mapping leaves the parameter untouched.
    map.handle_message(store, float_message("/other", 1.0f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.0f, 1e-3f));
    // A prefix of a mapped address is not a match either.
    map.handle_message(store, float_message("/mi", 1.0f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.0f, 1e-3f));
    map.handle_message(store, float_message("/mix", 1.0f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.75f, 1e-3f));

    // Removing the target unbinds every address that drove it.
    map.clear(1);
    map.pump();
    CHECK(map.mapping_count() == 0);
    map.handle_message(store, float_message("/mix", 0.0f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.75f, 1e-3f)); // unchanged

    // A pattern too long for a mapping's fixed storage is rejected outright
    // rather than truncated into a binding that would match the wrong address.
    const std::string too_long(pulp::osc::kOscPatternCapacity, 'x');
    CHECK_FALSE(OscParameterMap::pattern_fits(too_long));
    CHECK_FALSE(map.set_mapping(too_long, 1));
    map.pump();
    CHECK(map.mapping_count() == 0);

    // The longest pattern that does fit is accepted.
    const std::string longest(pulp::osc::kOscPatternCapacity - 1, 'x');
    CHECK(OscParameterMap::pattern_fits(longest));
    CHECK(map.set_mapping(longest, 1));
    map.pump();
    CHECK(map.mapping_count() == 1);

    // A pattern carrying an embedded NUL would be stored truncated, so it is
    // rejected too. An OSC wire address can never contain one.
    const std::string with_nul("/a\0b", 4);
    CHECK_FALSE(OscParameterMap::pattern_fits(with_nul));
    CHECK_FALSE(map.set_mapping(with_nul, 1));
}

TEST_CASE("OscParameterMap learn ignores an address it cannot store", "[osc][osc-map][learn]") {
    // Binding a truncated address would route the wrong messages, so an armed
    // learn skips an address that does not fit and stays armed for the next one.
    StateStore store;
    populate(store);
    OscParameterMap map;
    map.arm_learn(1);
    map.pump();

    const std::string too_long = "/" + std::string(pulp::osc::kOscPatternCapacity, 'x');
    map.handle_message(store, float_message(too_long, 1.0f));
    CHECK(map.learn_armed());
    CHECK(map.mapping_count() == 0);
    CHECK_THAT(store.get_normalized(1), WithinAbs(0.0f, 1e-3f));

    // The next address that does fit binds normally.
    map.handle_message(store, float_message("/knob", 1.0f));
    CHECK_FALSE(map.learn_armed());
    CHECK(map.mapping_count() == 1);
    CHECK_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("OscParameterMap stops binding at its mapping capacity", "[osc][osc-map]") {
    // Storage is fixed, so bindings past the capacity are dropped rather than
    // overrunning it — and the ones already installed keep working.
    StateStore store;
    populate(store);
    OscParameterMap map;
    for (std::size_t i = 0; i < OscParameterMap::kMaxMappings + 4; ++i) {
        // Pump each time so the bound is the mapping storage under test, not
        // the depth of the command queue in front of it.
        CHECK(map.set_mapping("/p/" + std::to_string(i), 1));
        map.pump();
    }
    CHECK(map.mapping_count() == OscParameterMap::kMaxMappings);

    map.handle_message(store, float_message("/p/0", 1.0f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f));
    // An address past the capacity cutoff never bound, so it routes nowhere.
    map.handle_message(
        store, float_message("/p/" + std::to_string(OscParameterMap::kMaxMappings + 1), 0.0f));
    CHECK_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("OscParameterMap reads the message's first numeric argument", "[osc][osc-map]") {
    StateStore store;
    populate(store);
    OscParameterMap map;
    REQUIRE(map.set_mapping("/level", 1, OscMapScale{0.0f, 127.0f, 0.0f, 1.0f}));
    map.pump();

    // int32 arguments (what many hardware-facing bridges send) widen to float.
    pulp::osc::Message as_int{"/level"};
    as_int.add(static_cast<std::int32_t>(127));
    map.handle_message(store, as_int);
    CHECK_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f));

    // A message with no argument, or a non-numeric first argument, carries no
    // control value: it must leave the parameter where it is.
    map.handle_message(store, pulp::osc::Message{"/level"});
    CHECK_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f));
    pulp::osc::Message as_string{"/level"};
    as_string.add(std::string{"loud"});
    map.handle_message(store, as_string);
    CHECK_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("OscParameterMap learn ignores a message with no control value", "[osc][osc-map][learn]") {
    // A surface that emits a bare notification (no numeric argument) must not
    // consume an armed learn — otherwise arming binds to whatever housekeeping
    // address the surface happened to send next.
    StateStore store;
    populate(store);
    OscParameterMap map;
    map.arm_learn(1);
    map.pump();

    map.handle_message(store, pulp::osc::Message{"/surface/ping"});
    CHECK(map.learn_armed());
    CHECK(map.mapping_count() == 0);

    map.handle_message(store, float_message("/surface/knob1", 1.0f));
    CHECK_FALSE(map.learn_armed());
    CHECK(map.mapping_count() == 1);
    CHECK_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("An OSC control surface reaches any engine parameter with zero driver code",
          "[osc][osc-map][timeline]") {
    // The Creative Timeline Engine's only automatable parameter surface is a
    // hosted device parameter, addressed by pulp::timeline::DeviceParameterTarget
    // whose `param_id` is the device's stable 32-bit host-facing ID — the same
    // pulp::state::ParamID a StateStore uses and OscParameterMap drives, and the
    // same id MidiParameterMap binds a CC to. So a tablet control surface needs
    // no per-surface or per-parameter driver code: the engine's parameter
    // identity flows straight into the map as an opaque id, and OSC, MIDI, and
    // authored automation all address one parameter space.
    //
    // Prove it end to end against that engine type: take the ids the engine
    // would automate, register them in the device's store, then learn each one
    // onto an incoming OSC address and drive it through the generic learn/apply
    // loop — each carrying a different input range and output window, so the
    // same call handles surfaces with unrelated conventions.
    struct SurfaceControl {
        pulp::timeline::DeviceParameterTarget target;
        std::string address;
        OscMapScale scale;
    };
    const std::array<SurfaceControl, 3> controls{{
        // A conventional 0..1 tablet fader over the full parameter range.
        {{pulp::timeline::ItemId{7}, 21}, "/track/1/fader", OscMapScale{}},
        // A 0..127 surface sweeping only the middle half.
        {{pulp::timeline::ItemId{7}, 22}, "/track/1/send", OscMapScale{0.0f, 127.0f, 0.25f, 0.75f}},
        // A bipolar -1..1 joystick, inverted.
        {{pulp::timeline::ItemId{9}, 5}, "/pad/x", OscMapScale{-1.0f, 1.0f, 1.0f, 0.0f}},
    }};

    StateStore store;
    for (const auto& c : controls)
        store.add_parameter({.id = c.target.param_id,
                             .name = "P",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 0.0f}});

    OscParameterMap map;
    for (const auto& c : controls) {
        map.arm_learn(c.target.param_id, c.scale);
        map.pump();
        map.handle_message(store, float_message(c.address, c.scale.in_max));
        CHECK_THAT(store.get_normalized(c.target.param_id),
                   WithinAbs(c.scale.apply(c.scale.in_max), 1e-3f));
        map.handle_message(store, float_message(c.address, c.scale.in_min));
        CHECK_THAT(store.get_normalized(c.target.param_id),
                   WithinAbs(c.scale.apply(c.scale.in_min), 1e-3f));
    }
    CHECK(map.mapping_count() == controls.size());

    // Each address reaches only its own target: driving one leaves the others.
    const float before = store.get_normalized(controls[1].target.param_id);
    map.handle_message(store, float_message(controls[0].address, 0.5f));
    CHECK_THAT(store.get_normalized(controls[1].target.param_id), WithinAbs(before, 1e-6f));
}

TEST_CASE("OscParameterMap routes to a caller-supplied sink", "[osc][osc-map]") {
    // route() is the seam a processor uses to apply on the audio thread with
    // set_normalized_rt. It reports the target id and the already-scaled
    // normalized value, once per matching mapping.
    OscParameterMap map;
    REQUIRE(map.set_mapping("/track/*/fader", 1, OscMapScale{0.0f, 1.0f, 0.25f, 0.75f}));
    REQUIRE(map.set_mapping("/track/1/fader", 2)); // both cover /track/1/fader
    map.pump();

    std::vector<std::pair<pulp::state::ParamID, float>> routed;
    map.route("/track/1/fader", 1.0f,
              [&](pulp::state::ParamID id, float n) { routed.emplace_back(id, n); });
    REQUIRE(routed.size() == 2);
    CHECK(routed[0].first == 1u);
    CHECK_THAT(routed[0].second, WithinAbs(0.75f, 1e-6f));
    CHECK(routed[1].first == 2u);
    CHECK_THAT(routed[1].second, WithinAbs(1.0f, 1e-6f));

    // An address matching nothing reports nothing.
    routed.clear();
    map.route("/master/fader", 1.0f,
              [&](pulp::state::ParamID id, float n) { routed.emplace_back(id, n); });
    CHECK(routed.empty());
}

TEST_CASE("OscParameterMap literal-address routing does not allocate", "[osc][osc-map][rt]") {
    // Mappings live in fixed-capacity storage and a literal address is matched
    // by byte compare, so installing and routing a binding never touches the
    // heap — which is what lets a processor drive route() from process().
    // (Wildcard patterns go through pulp::osc::address_matches, which carries no
    // such guarantee — hence the literal fast path.)
    StateStore store;
    populate(store);
    OscParameterMap map;
    REQUIRE(map.set_mapping("/track/1/fader", 1, OscMapScale{0.0f, 1.0f, 0.25f, 0.75f}));
    REQUIRE(map.set_mapping("/pan", 2, OscMapScale{0.0f, 127.0f, 0.5f, 1.0f}));
    map.pump();

    // Warm the parameter-index lookups outside the probed region.
    map.handle_value(store, "/track/1/fader", 0.5f);
    map.handle_value(store, "/pan", 64.0f);

    std::size_t applied = 0;
    // The sink is the RT-safe store write a processor would use on the audio
    // thread, so the probe covers the whole address → parameter path.
    const auto sink = [&](pulp::state::ParamID id, float normalized) {
        store.set_normalized_rt(id, normalized);
        ++applied;
    };

    {
        pulp::test::RtAllocationProbe probe;
        map.pump(); // draining an empty command queue must not allocate either
        for (int i = 0; i <= 16; ++i) {
            const float t = static_cast<float>(i) / 16.0f;
            map.route("/track/1/fader", t, sink);
            map.route("/pan", t * 127.0f, sink);
            map.route("/unmapped/address", t, sink); // miss path too
        }
        CHECK(probe.allocation_count() == 0);
    }
    // Guard against a probe that saw zero allocations because it saw no work.
    CHECK(applied == 34);
}
