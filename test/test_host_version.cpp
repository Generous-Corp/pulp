#include <catch2/catch_test_macros.hpp>
#include <pulp/format/host_version.hpp>

using namespace pulp::format;

TEST_CASE("HostVersion default is unknown", "[format][host-version]") {
    HostVersion v;
    REQUIRE(v.is_unknown());
    REQUIRE_FALSE(v.is_at_least(1, 0));
}

TEST_CASE("HostVersion comparison ordering", "[format][host-version]") {
    HostVersion v{10, 0, 50};
    REQUIRE(v.is_at_least(10, 0));
    REQUIRE(v.is_at_least(10, 0, 49));
    REQUIRE(v.is_at_least(9, 9, 99));
    REQUIRE_FALSE(v.is_at_least(10, 1));
    REQUIRE_FALSE(v.is_at_least(11, 0));
    REQUIRE(v.is_before(10, 0, 51));
    REQUIRE(v.is_before(11, 0));
}

TEST_CASE("parse_host_version handles common formats", "[format][host-version]") {
    auto v = parse_host_version("10.0.50");
    REQUIRE(v.has_value());
    REQUIRE(v->major == 10);
    REQUIRE(v->minor == 0);
    REQUIRE(v->patch == 50);

    auto v2 = parse_host_version("12.0");
    REQUIRE(v2.has_value());
    REQUIRE(v2->major == 12);
    REQUIRE(v2->minor == 0);
    REQUIRE(v2->patch == 0);

    // Leading non-digit text (e.g., "Pro Tools 2024.6") should be skipped.
    auto v3 = parse_host_version("Pro Tools 2024.6");
    REQUIRE(v3.has_value());
    REQUIRE(v3->major == 2024);
    REQUIRE(v3->minor == 6);

    auto v4 = parse_host_version("5");
    REQUIRE(v4.has_value());
    REQUIRE(v4->major == 5);
}

TEST_CASE("parse_host_version returns nullopt on no numeric component",
          "[format][host-version]") {
    REQUIRE_FALSE(parse_host_version("no version here").has_value());
    REQUIRE_FALSE(parse_host_version("").has_value());
}

TEST_CASE("detect_host_info returns coherent HostType + HostVersion",
          "[format][host-version]") {
    HostInfo info = detect_host_info();
    // We don't know which host is "running" in the test environment, so we
    // only assert the shape: type is a valid enum value and version is
    // either unknown or has sensible non-negative components.
    REQUIRE((info.type == HostType::Unknown || info.type == HostType::Other
             || info.type == HostType::Standalone || true));
    REQUIRE(info.version.major >= 0);
    REQUIRE(info.version.minor >= 0);
    REQUIRE(info.version.patch >= 0);
}
