#include <catch2/catch_test_macros.hpp>
#include <pulp/ship/appcast.hpp>

using namespace pulp::ship;

TEST_CASE("Appcast XML generation", "[ship][appcast]") {
    Appcast feed;
    feed.title = "PulpGain Updates";
    feed.link = "https://example.com/appcast.xml";
    feed.description = "Auto-update feed for PulpGain";

    AppcastItem item;
    item.version = "1.2.0";
    item.build_number = "42";
    item.title = "Version 1.2.0";
    item.description = "<p>Bug fixes and improvements</p>";
    item.pub_date = "Mon, 25 Mar 2026 12:00:00 +0000";
    item.download_url = "https://example.com/PulpGain-1.2.0.pkg";
    item.file_size = 1234567;
    item.minimum_os = "12.0";
    feed.items.push_back(item);

    auto xml = feed.to_xml();

    REQUIRE(xml.find("<?xml version") != std::string::npos);
    REQUIRE(xml.find("<rss") != std::string::npos);
    REQUIRE(xml.find("PulpGain Updates") != std::string::npos);
    REQUIRE(xml.find("sparkle:shortVersionString") != std::string::npos);
    REQUIRE(xml.find("1.2.0") != std::string::npos);
    REQUIRE(xml.find("1234567") != std::string::npos);
    REQUIRE(xml.find("<![CDATA[") != std::string::npos);
}

TEST_CASE("Appcast XML round-trip", "[ship][appcast]") {
    Appcast original;
    original.title = "Test Feed";
    original.link = "https://test.com/feed.xml";
    original.description = "Test";

    AppcastItem item;
    item.version = "2.0.0";
    item.build_number = "100";
    item.title = "Version 2.0.0";
    item.pub_date = "Tue, 01 Jan 2026 00:00:00 +0000";
    item.download_url = "https://test.com/update.pkg";
    item.file_size = 9999;
    original.items.push_back(item);

    auto xml = original.to_xml();
    auto parsed = Appcast::from_xml(xml);

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->title == "Test Feed");
    REQUIRE(parsed->items.size() == 1);
    REQUIRE(parsed->items[0].version == "2.0.0");
    REQUIRE(parsed->items[0].download_url == "https://test.com/update.pkg");
    REQUIRE(parsed->items[0].file_size == 9999);
}

TEST_CASE("Appcast from_xml invalid", "[ship][appcast]") {
    auto result = Appcast::from_xml("not xml");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Version comparison", "[ship][version]") {
    REQUIRE(compare_versions("1.0.0", "1.0.0") == 0);
    REQUIRE(compare_versions("1.0.0", "1.0.1") == -1);
    REQUIRE(compare_versions("1.1.0", "1.0.9") == 1);
    REQUIRE(compare_versions("2.0.0", "1.9.9") == 1);
    REQUIRE(compare_versions("1.0", "1.0.0") == 0);
    REQUIRE(compare_versions("1.0.0.1", "1.0.0") == 1);
}

// #295 P0 regression: sign_file_ed25519 MUST NOT silently return an
// empty signature. It must return std::nullopt so callers can hard-
// fail instead of emitting `edSignature=""` into an appcast.
// When the real implementation lands, this test flips to asserting
// a non-empty base64 signature against a known test vector.
TEST_CASE("sign_file_ed25519 never silently returns empty", "[ship][sign][issue-295]") {
    auto result = sign_file_ed25519("/nonexistent/path", "invalid-key");
    // Today: nullopt (no impl linked).
    // Tomorrow: Some(sig) on valid inputs, nullopt on invalid.
    // Either way, never Some("") — that's the #295 P0 regression.
    if (result.has_value()) {
        REQUIRE_FALSE(result->empty());
    }
}
