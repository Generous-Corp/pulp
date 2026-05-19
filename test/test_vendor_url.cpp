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

TEST_CASE("PluginDescriptor bus helpers follow first bus and empty bus lists",
          "[format][descriptor][coverage][phase3]") {
    PluginDescriptor d;
    REQUIRE(d.default_input_channels() == 2);
    REQUIRE(d.default_output_channels() == 2);

    d.input_buses = {{"Sidechain", 1, true}, {"Ignored", 6, false}};
    d.output_buses = {{"Mono", 1, false}, {"Surround", 8, true}};
    REQUIRE(d.default_input_channels() == 1);
    REQUIRE(d.default_output_channels() == 1);

    d.input_buses.clear();
    d.output_buses.clear();
    REQUIRE(d.default_input_channels() == 0);
    REQUIRE(d.default_output_channels() == 0);

    d.input_buses = {{"Main In", 2, false}};
    d.output_buses = {{"Main Out", 4, false}};
    REQUIRE(d.default_input_channels() == 2);
    REQUIRE(d.default_output_channels() == 4);
}

TEST_CASE("PluginDescriptor modern capability flags default off",
          "[format][vendor-url][coverage][phase3-large]") {
    PluginDescriptor d;
    REQUIRE_FALSE(d.supports_mpe);
    REQUIRE_FALSE(d.supports_ump);
    REQUIRE_FALSE(d.ios_requires_background_audio);

    d.supports_mpe = true;
    d.supports_ump = true;
    d.ios_requires_background_audio = true;
    REQUIRE(d.supports_mpe);
    REQUIRE(d.supports_ump);
    REQUIRE(d.ios_requires_background_audio);
}
