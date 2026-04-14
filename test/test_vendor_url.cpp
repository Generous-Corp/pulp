// PluginDescriptor::vendor_url / vendor_email field tests
// (workstream 01 slice 1.8).

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/processor.hpp>

using namespace pulp::format;

TEST_CASE("vendor_url and vendor_email default empty",
          "[format][vendor-url]") {
    PluginDescriptor d;
    REQUIRE(d.vendor_url.empty());
    REQUIRE(d.vendor_email.empty());
}

TEST_CASE("vendor_url + vendor_email are settable",
          "[format][vendor-url]") {
    PluginDescriptor d;
    d.vendor_url = "https://example.com/plugins";
    d.vendor_email = "dev@example.com";
    REQUIRE(d.vendor_url == "https://example.com/plugins");
    REQUIRE(d.vendor_email == "dev@example.com");
}

TEST_CASE("copy preserves url + email", "[format][vendor-url]") {
    PluginDescriptor a;
    a.manufacturer = "PulpCo";
    a.vendor_url = "https://pulp.audio";
    a.vendor_email = "hello@pulp.audio";

    PluginDescriptor b = a;
    REQUIRE(b.vendor_url == "https://pulp.audio");
    REQUIRE(b.vendor_email == "hello@pulp.audio");
    REQUIRE(b.manufacturer == "PulpCo");
}
