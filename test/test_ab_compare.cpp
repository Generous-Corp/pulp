// Unit tests for the A/B compare component (workstream 07 slice 7.2).

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/ab_compare.hpp>
#include <pulp/state/store.hpp>

using namespace pulp;
using namespace pulp::view;

namespace {
void populate(state::StateStore& s) {
    s.add_parameter({.id = 1, .name = "Gain", .unit = "dB",
                     .range = {-60.0f, 24.0f, 0.0f, 0.1f}});
    s.add_parameter({.id = 2, .name = "Mix", .unit = "",
                     .range = {0.0f, 1.0f, 0.5f, 0.01f}});
}
} // namespace

TEST_CASE("ABCompare: save + load restores parameters", "[ui][ab-compare]") {
    state::StateStore store; populate(store);
    ABCompare ab(&store);

    store.set_value(1, 6.0f);
    store.set_value(2, 0.25f);
    ab.save_to(ABCompare::Slot::A);
    REQUIRE(ab.has(ABCompare::Slot::A));
    REQUIRE_FALSE(ab.has(ABCompare::Slot::B));

    store.set_value(1, -12.0f);
    store.set_value(2, 0.75f);
    ab.save_to(ABCompare::Slot::B);
    REQUIRE(ab.has(ABCompare::Slot::B));

    REQUIRE(ab.load_from(ABCompare::Slot::A));
    REQUIRE(store.get_value(1) == 6.0f);
    REQUIRE(store.get_value(2) == 0.25f);
    REQUIRE(ab.current() == ABCompare::Slot::A);
}

TEST_CASE("ABCompare: toggle flips between A and B", "[ui][ab-compare]") {
    state::StateStore store; populate(store);
    ABCompare ab(&store);

    store.set_value(1, 3.0f);
    ab.save_to(ABCompare::Slot::A);
    store.set_value(1, -3.0f);
    ab.save_to(ABCompare::Slot::B);

    REQUIRE(ab.load_from(ABCompare::Slot::A));
    REQUIRE(ab.toggle());
    REQUIRE(ab.current() == ABCompare::Slot::B);
    REQUIRE(store.get_value(1) == -3.0f);
    REQUIRE(ab.toggle());
    REQUIRE(ab.current() == ABCompare::Slot::A);
    REQUIRE(store.get_value(1) == 3.0f);
}

TEST_CASE("ABCompare: load from empty slot fails cleanly", "[ui][ab-compare]") {
    state::StateStore store; populate(store);
    ABCompare ab(&store);
    REQUIRE_FALSE(ab.load_from(ABCompare::Slot::A));
    REQUIRE_FALSE(ab.load_from(ABCompare::Slot::B));
}

TEST_CASE("ABCompare: copy + swap", "[ui][ab-compare]") {
    state::StateStore store; populate(store);
    ABCompare ab(&store);

    store.set_value(1, 10.0f);
    ab.save_to(ABCompare::Slot::A);
    ab.copy(ABCompare::Slot::A, ABCompare::Slot::B);
    REQUIRE(ab.has(ABCompare::Slot::B));
    REQUIRE(ab.load_from(ABCompare::Slot::B));
    REQUIRE(store.get_value(1) == 10.0f);

    store.set_value(1, -5.0f);
    ab.save_to(ABCompare::Slot::B);  // B is now -5
    ab.swap();                       // A ↔ B
    REQUIRE(ab.load_from(ABCompare::Slot::A));
    REQUIRE(store.get_value(1) == -5.0f);
}

TEST_CASE("ABCompare: clear + snapshot access", "[ui][ab-compare]") {
    state::StateStore store; populate(store);
    ABCompare ab(&store);
    store.set_value(1, 1.0f);
    ab.save_to(ABCompare::Slot::A);
    REQUIRE(ab.snapshot(ABCompare::Slot::A).size() > 0);
    ab.clear(ABCompare::Slot::A);
    REQUIRE_FALSE(ab.has(ABCompare::Slot::A));
    REQUIRE(ab.snapshot(ABCompare::Slot::A).size() == 0);
}
