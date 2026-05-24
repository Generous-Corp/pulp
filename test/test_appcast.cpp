#include <catch2/catch_test_macros.hpp>
#include <pulp/ship/appcast.hpp>

#include <string>

using namespace pulp::ship;

namespace {

int count_occurrences(const std::string& haystack, const std::string& needle) {
    int count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

} // namespace

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

TEST_CASE("Appcast XML escapes metadata and omits empty optional item fields", "[ship][appcast]") {
    Appcast feed;
    feed.title = R"(Pulp & "Friends" <Beta>)";
    feed.link = R"(https://example.com/appcast.xml?channel="stable"&x=1)";
    feed.description = R"(Ships <fast> & "safe")";

    AppcastItem item;
    item.version = "3.4.5";
    item.title = R"(Release & "Notes" <Here>)";
    item.pub_date = "Wed, 02 Apr 2026 12:00:00 +0000";
    item.download_url = R"(https://example.com/PulpGain.pkg?sig="abc"&v=1)";
    feed.items.push_back(item);

    auto xml = feed.to_xml();

    REQUIRE(xml.find("<title>Pulp &amp; &quot;Friends&quot; &lt;Beta&gt;</title>") != std::string::npos);
    REQUIRE(xml.find("<link>https://example.com/appcast.xml?channel=&quot;stable&quot;&amp;x=1</link>") != std::string::npos);
    REQUIRE(xml.find("<description>Ships &lt;fast&gt; &amp; &quot;safe&quot;</description>") != std::string::npos);
    REQUIRE(xml.find("<title>Release &amp; &quot;Notes&quot; &lt;Here&gt;</title>") != std::string::npos);
    REQUIRE(xml.find("<sparkle:version>3.4.5</sparkle:version>") != std::string::npos);
    REQUIRE(xml.find("url=\"https://example.com/PulpGain.pkg?sig=&quot;abc&quot;&amp;v=1\"") != std::string::npos);
    REQUIRE(xml.find("<![CDATA[") == std::string::npos);
    REQUIRE(xml.find("sparkle:minimumSystemVersion") == std::string::npos);
    REQUIRE(xml.find("sparkle:edSignature=") == std::string::npos);
}

TEST_CASE("Appcast XML emits Sparkle signature when present", "[ship][appcast]") {
    Appcast feed;
    feed.title = "Signed Feed";
    feed.link = "https://example.com/appcast.xml";
    feed.description = "Signed releases";

    AppcastItem item;
    item.version = "4.0.0";
    item.build_number = "400";
    item.title = "Version 4.0.0";
    item.pub_date = "Fri, 04 Apr 2026 12:00:00 +0000";
    item.download_url = "https://example.com/Pulp-4.0.0.pkg";
    item.file_size = 4096;
    item.ed_signature = "base64-signature";
    feed.items.push_back(item);

    auto xml = feed.to_xml();

    REQUIRE(xml.find("<sparkle:version>400</sparkle:version>") != std::string::npos);
    REQUIRE(xml.find("sparkle:edSignature=\"base64-signature\"") != std::string::npos);
}

TEST_CASE("Appcast XML escapes greater-than characters in feed and item fields",
          "[ship][appcast][coverage]") {
    Appcast feed;
    feed.title = "Pulp > Beta";
    feed.link = "https://example.com/feed.xml";
    feed.description = "Updates > previews";

    AppcastItem item;
    item.version = "1.0.0";
    item.title = "Build > 100";
    item.pub_date = "Mon, 01 Jun 2026 12:00:00 +0000";
    item.download_url = "https://example.com/Pulp.pkg?min=>100";
    feed.items.push_back(item);

    auto xml = feed.to_xml();

    REQUIRE(xml.find("<title>Pulp &gt; Beta</title>") != std::string::npos);
    REQUIRE(xml.find("<description>Updates &gt; previews</description>") != std::string::npos);
    REQUIRE(xml.find("<title>Build &gt; 100</title>") != std::string::npos);
    REQUIRE(xml.find("url=\"https://example.com/Pulp.pkg?min=&gt;100\"") != std::string::npos);
}

TEST_CASE("Appcast from_xml parses optional fields across multiple items", "[ship][appcast]") {
    auto parsed = Appcast::from_xml(R"(<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>Ship Feed</title>
    <link>https://example.com/feed.xml</link>
    <description>Stable releases</description>
    <item>
      <title>Version 3.1.0</title>
      <description><![CDATA[<p>Signed update</p>]]></description>
      <pubDate>Thu, 03 Apr 2026 12:00:00 +0000</pubDate>
      <sparkle:version>3100</sparkle:version>
      <sparkle:shortVersionString>3.1.0</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>13.0</sparkle:minimumSystemVersion>
      <enclosure url="https://example.com/Pulp-3.1.0.pkg"
                 length="321"
                 type="application/octet-stream"
                 sparkle:edSignature="abc123" />
    </item>
    <item>
      <title>Version 3.0.9</title>
      <pubDate>Wed, 02 Apr 2026 12:00:00 +0000</pubDate>
      <sparkle:shortVersionString>3.0.9</sparkle:shortVersionString>
      <enclosure url="https://example.com/Pulp-3.0.9.pkg"
                 type="application/octet-stream" />
    </item>
  </channel>
</rss>)");

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->title == "Ship Feed");
    REQUIRE(parsed->link == "https://example.com/feed.xml");
    REQUIRE(parsed->description == "Stable releases");
    REQUIRE(parsed->items.size() == 2);

    REQUIRE(parsed->items[0].title == "Version 3.1.0");
    REQUIRE(parsed->items[0].description == "<p>Signed update</p>");
    REQUIRE(parsed->items[0].build_number == "3100");
    REQUIRE(parsed->items[0].version == "3.1.0");
    REQUIRE(parsed->items[0].minimum_os == "13.0");
    REQUIRE(parsed->items[0].download_url == "https://example.com/Pulp-3.1.0.pkg");
    REQUIRE(parsed->items[0].ed_signature == "abc123");
    REQUIRE(parsed->items[0].file_size == 321);

    REQUIRE(parsed->items[1].title == "Version 3.0.9");
    REQUIRE(parsed->items[1].description.empty());
    REQUIRE(parsed->items[1].build_number.empty());
    REQUIRE(parsed->items[1].version == "3.0.9");
    REQUIRE(parsed->items[1].minimum_os.empty());
    REQUIRE(parsed->items[1].download_url == "https://example.com/Pulp-3.0.9.pkg");
    REQUIRE(parsed->items[1].ed_signature.empty());
    REQUIRE(parsed->items[1].file_size == 0);
}

TEST_CASE("Appcast from_xml invalid", "[ship][appcast]") {
    auto result = Appcast::from_xml("not xml");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Appcast from_xml stops before malformed item", "[ship][appcast]") {
    auto parsed = Appcast::from_xml(R"(<rss version="2.0">
  <channel>
    <title>Broken Feed</title>
    <link>https://example.com/appcast.xml</link>
    <description>Malformed item feed</description>
    <item>
      <title>Version 4.0.0</title>
  </channel>
</rss>)");

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->title == "Broken Feed");
    REQUIRE(parsed->items.empty());
}

TEST_CASE("Appcast from_xml tolerates unterminated optional fields", "[ship][appcast]") {
    auto parsed = Appcast::from_xml(R"(<rss version="2.0">
  <channel>
    <title>Partial Feed</title>
    <link>https://example.com/appcast.xml
    <description>Partial feed</description>
    <item>
      <title>Version 4.1.0</title>
      <description><![CDATA[unterminated notes</description>
      <pubDate>Fri, 04 Apr 2026 12:00:00 +0000</pubDate>
      <sparkle:shortVersionString>4.1.0</sparkle:shortVersionString>
      <enclosure length="410"
                 url="https://example.com/Pulp-4.1.0.pkg />
    </item>
  </channel>
</rss>)");

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->title == "Partial Feed");
    REQUIRE(parsed->link.empty());
    REQUIRE(parsed->description == "Partial feed");
    REQUIRE(parsed->items.size() == 1);
    REQUIRE(parsed->items[0].description.empty());
    REQUIRE(parsed->items[0].download_url.empty());
    REQUIRE(parsed->items[0].file_size == 410);
}

TEST_CASE("Appcast from_xml ignores malformed enclosure length", "[ship][appcast]") {
    auto parsed = Appcast::from_xml(R"(<rss version="2.0">
  <channel>
    <title>Malformed Length Feed</title>
    <item>
      <title>Version 5.0.0</title>
      <sparkle:shortVersionString>5.0.0</sparkle:shortVersionString>
      <enclosure url="https://example.com/Pulp-5.0.0.pkg"
                 length="12oops"
                 type="application/octet-stream" />
    </item>
    <item>
      <title>Version 5.0.1</title>
      <sparkle:shortVersionString>5.0.1</sparkle:shortVersionString>
      <enclosure url="https://example.com/Pulp-5.0.1.pkg"
                 length="999999999999999999999999999999999999"
                 type="application/octet-stream" />
    </item>
  </channel>
</rss>)");

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->items.size() == 2);
    REQUIRE(parsed->items[0].version == "5.0.0");
    REQUIRE(parsed->items[0].file_size == 0);
    REQUIRE(parsed->items[1].version == "5.0.1");
    REQUIRE(parsed->items[1].file_size == 0);
}

TEST_CASE("Appcast XML uses short version as build number when build is empty",
          "[ship][appcast][coverage][issue-644]") {
    Appcast feed;
    feed.title = "Fallback Feed";
    feed.link = "https://example.com/appcast.xml";
    feed.description = "Fallback build number";

    AppcastItem item;
    item.version = "6.2.1";
    item.title = "Version 6.2.1";
    item.pub_date = "Sat, 06 Jun 2026 12:00:00 +0000";
    item.download_url = "https://example.com/Pulp-6.2.1.pkg";
    item.file_size = 621;
    feed.items.push_back(item);

    auto xml = feed.to_xml();

    REQUIRE(xml.find("<sparkle:version>6.2.1</sparkle:version>") != std::string::npos);
    REQUIRE(xml.find("<sparkle:shortVersionString>6.2.1</sparkle:shortVersionString>") != std::string::npos);
}

TEST_CASE("Appcast from_xml accepts rss with item-only metadata",
          "[ship][appcast][coverage][issue-644]") {
    auto parsed = Appcast::from_xml(R"(<rss version="2.0">
  <channel>
    <item>
      <title>Nameless Release</title>
      <sparkle:version>700</sparkle:version>
      <sparkle:shortVersionString>7.0.0</sparkle:shortVersionString>
      <enclosure url="https://example.com/Pulp-7.0.0.pkg" length="700" />
    </item>
  </channel>
</rss>)");

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->title == "Nameless Release");
    REQUIRE(parsed->link.empty());
    REQUIRE(parsed->description.empty());
    REQUIRE(parsed->items.size() == 1);
    REQUIRE(parsed->items[0].title == "Nameless Release");
    REQUIRE(parsed->items[0].build_number == "700");
    REQUIRE(parsed->items[0].version == "7.0.0");
}

TEST_CASE("Appcast from_xml only requires rss marker",
          "[ship][appcast][coverage][issue-644]") {
    auto parsed = Appcast::from_xml("<rss version=\"2.0\"></rss>");

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->items.empty());
}

TEST_CASE("Appcast XML emits stable empty-feed skeleton",
          "[ship][appcast][coverage]") {
    Appcast feed;
    feed.title = "Empty Updates";
    feed.link = "https://example.com/empty.xml";
    feed.description = "No releases yet";

    auto xml = feed.to_xml();

    REQUIRE(xml.find("<?xml version=\"1.0\" encoding=\"utf-8\"?>") == 0);
    REQUIRE(xml.find("<rss version=\"2.0\" xmlns:sparkle=\"http://www.andymatuschak.org/xml-namespaces/sparkle\">") != std::string::npos);
    REQUIRE(xml.find("<title>Empty Updates</title>") != std::string::npos);
    REQUIRE(xml.find("<language>en</language>") != std::string::npos);
    REQUIRE(xml.find("<item>") == std::string::npos);
    REQUIRE(xml.find("</rss>\n") != std::string::npos);
}

TEST_CASE("Appcast XML emits each item branch independently",
          "[ship][appcast][coverage]") {
    Appcast feed;
    feed.title = "Mixed Feed";
    feed.link = "https://example.com/appcast.xml";
    feed.description = "Mixed release metadata";

    AppcastItem fallback;
    fallback.version = "8.0.0";
    fallback.title = "Version 8.0.0";
    fallback.pub_date = "Mon, 08 Jun 2026 12:00:00 +0000";
    fallback.download_url = "https://example.com/Pulp-8.0.0.pkg";
    fallback.file_size = 0;
    feed.items.push_back(fallback);

    AppcastItem full;
    full.version = "8.1.0";
    full.build_number = "810";
    full.title = "Version 8.1.0";
    full.description = "<ul><li>One</li><li>Two</li></ul>";
    full.pub_date = "Tue, 09 Jun 2026 12:00:00 +0000";
    full.download_url = "https://example.com/Pulp-8.1.0.pkg";
    full.file_size = 8192;
    full.minimum_os = "14.0";
    full.ed_signature = "sig810";
    feed.items.push_back(full);

    auto xml = feed.to_xml();

    REQUIRE(count_occurrences(xml, "<item>") == 2);
    REQUIRE(count_occurrences(xml, "<enclosure") == 2);
    REQUIRE(count_occurrences(xml, "<![CDATA[") == 1);
    REQUIRE(xml.find("<sparkle:version>8.0.0</sparkle:version>") != std::string::npos);
    REQUIRE(xml.find("<sparkle:version>810</sparkle:version>") != std::string::npos);
    REQUIRE(xml.find("length=\"0\"") != std::string::npos);
    REQUIRE(xml.find("length=\"8192\"") != std::string::npos);
    REQUIRE(xml.find("<sparkle:minimumSystemVersion>14.0</sparkle:minimumSystemVersion>") != std::string::npos);
    REQUIRE(xml.find("sparkle:edSignature=\"sig810\"") != std::string::npos);
}

TEST_CASE("Appcast from_xml handles empty and zero-valued enclosure attributes",
          "[ship][appcast][coverage]") {
    auto parsed = Appcast::from_xml(R"(<rss version="2.0">
  <channel>
    <title>Zero Feed</title>
    <item>
      <title>Zero Length</title>
      <sparkle:version>900</sparkle:version>
      <sparkle:shortVersionString>9.0.0</sparkle:shortVersionString>
      <enclosure url="" length="0" sparkle:edSignature="" />
    </item>
    <item>
      <title>Missing Attributes</title>
      <sparkle:shortVersionString>9.0.1</sparkle:shortVersionString>
      <enclosure />
    </item>
  </channel>
</rss>)");

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->items.size() == 2);
    REQUIRE(parsed->items[0].title == "Zero Length");
    REQUIRE(parsed->items[0].version == "9.0.0");
    REQUIRE(parsed->items[0].download_url.empty());
    REQUIRE(parsed->items[0].ed_signature.empty());
    REQUIRE(parsed->items[0].file_size == 0);
    REQUIRE(parsed->items[1].version == "9.0.1");
    REQUIRE(parsed->items[1].download_url.empty());
    REQUIRE(parsed->items[1].file_size == 0);
}

TEST_CASE("Appcast from_xml keeps partial item metadata when optional tags are malformed",
          "[ship][appcast][coverage]") {
    auto parsed = Appcast::from_xml(R"(<rss version="2.0">
  <channel>
    <title>Partial Items</title>
    <item>
      <title>Partial Release</title>
      <pubDate>Wed, 10 Jun 2026 12:00:00 +0000
      <sparkle:version>910</sparkle:version>
      <sparkle:shortVersionString>9.1.0</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>13.3
      <enclosure url="https://example.com/Pulp-9.1.0.pkg" length="0910" />
    </item>
  </channel>
</rss>)");

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->items.size() == 1);
    REQUIRE(parsed->items[0].pub_date.empty());
    REQUIRE(parsed->items[0].version == "9.1.0");
    REQUIRE(parsed->items[0].minimum_os.empty());
    REQUIRE(parsed->items[0].download_url == "https://example.com/Pulp-9.1.0.pkg");
    REQUIRE(parsed->items[0].file_size == 910);
}

TEST_CASE("Appcast from_xml ignores single-quoted enclosure attributes",
          "[ship][appcast][coverage]") {
    auto parsed = Appcast::from_xml(R"(<rss version="2.0">
  <channel>
    <title>Quoted Feed</title>
    <item>
      <title>Single Quotes</title>
      <sparkle:shortVersionString>9.2.0</sparkle:shortVersionString>
      <enclosure url='https://example.com/Pulp-9.2.0.pkg' length='920' sparkle:edSignature='sig920' />
    </item>
  </channel>
</rss>)");

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->items.size() == 1);
    REQUIRE(parsed->items[0].version == "9.2.0");
    REQUIRE(parsed->items[0].download_url.empty());
    REQUIRE(parsed->items[0].ed_signature.empty());
    REQUIRE(parsed->items[0].file_size == 0);
}

TEST_CASE("Appcast from_xml captures CDATA with nested markup and brackets",
          "[ship][appcast][coverage]") {
    auto parsed = Appcast::from_xml(R"(<rss version="2.0">
  <channel>
    <title>Notes Feed</title>
    <item>
      <title>Rich Notes</title>
      <description><![CDATA[<p>Fix <strong>A</strong> & keep [B]</p>]]></description>
      <sparkle:shortVersionString>9.3.0</sparkle:shortVersionString>
      <enclosure url="https://example.com/Pulp-9.3.0.pkg" length="930" />
    </item>
  </channel>
</rss>)");

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->items.size() == 1);
    REQUIRE(parsed->items[0].title == "Rich Notes");
    REQUIRE(parsed->items[0].description == "<p>Fix <strong>A</strong> & keep [B]</p>");
    REQUIRE(parsed->items[0].version == "9.3.0");
    REQUIRE(parsed->items[0].download_url == "https://example.com/Pulp-9.3.0.pkg");
    REQUIRE(parsed->items[0].file_size == 930);
}

TEST_CASE("Version comparison", "[ship][version]") {
    REQUIRE(compare_versions("1.0.0", "1.0.0") == 0);
    REQUIRE(compare_versions("1.0.0", "1.0.1") == -1);
    REQUIRE(compare_versions("1.1.0", "1.0.9") == 1);
    REQUIRE(compare_versions("2.0.0", "1.9.9") == 1);
    REQUIRE(compare_versions("1.0", "1.0.0") == 0);
    REQUIRE(compare_versions("1.0.0.1", "1.0.0") == 1);
}

TEST_CASE("Version comparison tolerates non-numeric segments", "[ship][version]") {
    REQUIRE(compare_versions("01.002.0003", "1.2.3") == 0);
    REQUIRE(compare_versions("1.beta.5", "1.0.7") == -1);
    REQUIRE(compare_versions("1..5", "1.0.4") == 1);
}

TEST_CASE("Version comparison pads and orders long mixed versions",
          "[ship][version][coverage][issue-644]") {
    REQUIRE(compare_versions("", "0") == 0);
    REQUIRE(compare_versions("1.0.0", "1.0.0.0.0") == 0);
    REQUIRE(compare_versions("1.0.0.0.1", "1.0.0.0") == 1);
    REQUIRE(compare_versions("2.alpha.0", "2.0.1") == -1);
    REQUIRE(compare_versions("2026.05.15", "2026.5.14") == 1);
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
