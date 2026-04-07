#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/osc/bundle.hpp>

using namespace pulp::osc;
using Catch::Matchers::WithinAbs;

// ── TimeTag ─────────────────────────────────────────────────────────────

TEST_CASE("OSC TimeTag immediate", "[osc][bundle]") {
    auto tt = TimeTag::immediate();
    REQUIRE(tt.seconds == 0);
    REQUIRE(tt.fraction == 1);
}

TEST_CASE("OSC TimeTag from/to unix round-trip", "[osc][bundle]") {
    double unix_time = 1700000000.5;  // 2023-11-14 approx
    auto tt = TimeTag::from_unix(unix_time);
    REQUIRE(tt.seconds > 0);  // NTP epoch is 1900, so seconds > unix seconds
    REQUIRE_THAT(tt.to_unix(), WithinAbs(unix_time, 0.001));
}

TEST_CASE("OSC TimeTag equality", "[osc][bundle]") {
    auto a = TimeTag::from_unix(1000000.0);
    auto b = TimeTag::from_unix(1000000.0);
    REQUIRE(a == b);
}

// ── Bundle construction ─────────────────────────────────────────────────

TEST_CASE("OSC Bundle add message", "[osc][bundle]") {
    Bundle bundle;
    Message msg;
    msg.address = "/test";
    msg.add_int(42);
    bundle.add(std::move(msg));

    REQUIRE(bundle.elements.size() == 1);
    REQUIRE(bundle.elements[0].is_message());
    REQUIRE(bundle.elements[0].message().address == "/test");
}

TEST_CASE("OSC Bundle add nested bundle", "[osc][bundle]") {
    Bundle inner;
    inner.timetag = TimeTag::from_unix(100.0);
    Message msg;
    msg.address = "/inner";
    inner.add(std::move(msg));

    Bundle outer;
    outer.add(std::move(inner));

    REQUIRE(outer.elements.size() == 1);
    REQUIRE(outer.elements[0].is_bundle());
    REQUIRE(outer.elements[0].bundle().elements.size() == 1);
}

// ── Bundle serialization ────────────────────────────────────────────────

TEST_CASE("OSC Bundle serialize produces valid data", "[osc][bundle]") {
    Bundle bundle;
    bundle.timetag = TimeTag::immediate();

    Message msg;
    msg.address = "/gain";
    msg.add_float(0.75f);
    bundle.add(std::move(msg));

    auto data = bundle.serialize();
    REQUIRE(data.size() > 8);  // At minimum: #bundle + timetag

    // Check #bundle header
    std::string header(data.begin(), data.begin() + 8);
    REQUIRE(header == "#bundle\0");
}

TEST_CASE("OSC Bundle serialize/deserialize round-trip", "[osc][bundle]") {
    Bundle original;
    original.timetag = TimeTag::from_unix(1700000000.0);

    Message msg1;
    msg1.address = "/volume";
    msg1.add_float(-6.0f);
    original.add(std::move(msg1));

    Message msg2;
    msg2.address = "/mute";
    msg2.add_int(1);
    original.add(std::move(msg2));

    auto data = original.serialize();
    REQUIRE(data.size() > 0);

    auto restored = Bundle::deserialize(data.data(), data.size());
    REQUIRE(restored.has_value());
    REQUIRE(restored->timetag == original.timetag);
    REQUIRE(restored->elements.size() == 2);
    REQUIRE(restored->elements[0].is_message());
    REQUIRE(restored->elements[0].message().address == "/volume");
}

// ── Address pattern matching ────────────────────────────────────────────

TEST_CASE("OSC address exact match", "[osc][bundle]") {
    REQUIRE(address_matches("/foo/bar", "/foo/bar"));
    REQUIRE_FALSE(address_matches("/foo/bar", "/foo/baz"));
}

TEST_CASE("OSC address wildcard *", "[osc][bundle]") {
    REQUIRE(address_matches("/foo/*", "/foo/bar"));
    REQUIRE(address_matches("/*/bar", "/foo/bar"));
    REQUIRE(address_matches("/*", "/anything"));
}

TEST_CASE("OSC address wildcard ?", "[osc][bundle]") {
    REQUIRE(address_matches("/foo/ba?", "/foo/bar"));
    REQUIRE(address_matches("/foo/ba?", "/foo/baz"));
    REQUIRE_FALSE(address_matches("/foo/ba?", "/foo/ba"));
}
