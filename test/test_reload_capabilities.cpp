// Capability model for the scripted-UI bridge. Verifies the capability set's name
// round-trip, fail-closed parsing of an unknown token, and the empty-default /
// all() postures that back the restricted-vs-full-featured bridge modes.
#include <catch2/catch_test_macros.hpp>

#include <pulp/view/reload_capabilities.hpp>

using pulp::view::CapabilitySet;
using pulp::view::ReloadCapability;
using pulp::view::capability_from_name;
using pulp::view::capability_name;

TEST_CASE("capability names round-trip", "[reload][capabilities]") {
    for (auto c : {ReloadCapability::Exec, ReloadCapability::Clipboard,
                   ReloadCapability::Filesystem, ReloadCapability::Storage,
                   ReloadCapability::Ai, ReloadCapability::RuntimeImport,
                   ReloadCapability::Network}) {
        ReloadCapability parsed;
        REQUIRE(capability_from_name(capability_name(c), parsed));
        REQUIRE(parsed == c);
    }
}

TEST_CASE("default CapabilitySet is empty (consumer-default = most restrictive)",
          "[reload][capabilities]") {
    CapabilitySet s;
    REQUIRE(s.empty());
    REQUIRE_FALSE(s.has(ReloadCapability::Filesystem));
    REQUIRE_FALSE(s.has(ReloadCapability::Exec));
}

TEST_CASE("all() grants every capability (dev-unenforced posture)",
          "[reload][capabilities]") {
    auto s = CapabilitySet::all();
    REQUIRE_FALSE(s.empty());
    REQUIRE(s.has(ReloadCapability::Exec));
    REQUIRE(s.has(ReloadCapability::Filesystem));
    REQUIRE(s.has(ReloadCapability::RuntimeImport));
    REQUIRE(s.has(ReloadCapability::Network));
}

TEST_CASE("parse builds the declared set; unknown token fails closed",
          "[reload][capabilities]") {
    CapabilitySet s;
    REQUIRE(CapabilitySet::parse({"filesystem", "storage"}, s));
    REQUIRE(s.has(ReloadCapability::Filesystem));
    REQUIRE(s.has(ReloadCapability::Storage));
    REQUIRE_FALSE(s.has(ReloadCapability::Network));

    // An unrecognized capability rejects the WHOLE set (never silently drop it).
    CapabilitySet bad;
    REQUIRE_FALSE(CapabilitySet::parse({"filesystem", "teleport"}, bad));
    REQUIRE(bad.empty());  // untouched → still most-restrictive
}
