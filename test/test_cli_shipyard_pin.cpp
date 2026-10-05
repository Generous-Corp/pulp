// The Shipyard pin is a floor within its major: `pulp pr` and `pulp status`
// accept any installed release at or above tools/shipyard.toml's version
// under the same major, so a fleet that rolls out every release does not
// trip the guard on each patch or minor bump.

#include <catch2/catch_test_macros.hpp>

#include "cli_sdk.hpp"

TEST_CASE("the Shipyard pin is a floor within its major", "[cli][shipyard-pin]") {
    CHECK(shipyard_pin_accepts("v0.269.1", "v0.269.1"));
    CHECK(shipyard_pin_accepts("v0.269.1", "v0.269.2"));
    CHECK(shipyard_pin_accepts("v0.269.1", "v0.270.0"));
    CHECK_FALSE(shipyard_pin_accepts("v0.269.1", "v0.269.0"));
    CHECK_FALSE(shipyard_pin_accepts("v0.269.1", "v0.268.9"));
    CHECK_FALSE(shipyard_pin_accepts("v0.269.1", "v1.0.0"));
    CHECK_FALSE(shipyard_pin_accepts("v1.2.0", "v0.300.0"));
}

TEST_CASE("a version that is not MAJOR.MINOR.PATCH must equal the pin",
          "[cli][shipyard-pin]") {
    CHECK(shipyard_pin_accepts("0.269.1", "v0.269.1"));
    CHECK_FALSE(shipyard_pin_accepts("v0.269.1", "v0.269.1-rc1"));
    CHECK_FALSE(shipyard_pin_accepts("v0.269.1", "v0.269.1.4"));
    CHECK_FALSE(shipyard_pin_accepts("v0.269.1", "v0.269"));
    CHECK_FALSE(shipyard_pin_accepts("v0.269.1", ""));
    CHECK(shipyard_pin_accepts("nightly", "nightly"));
}
