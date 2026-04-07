#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/xml.hpp>
#include <pulp/runtime/zip.hpp>
#include <pulp/runtime/temporary_file.hpp>
#include <fstream>
#include <cstring>

using namespace pulp::runtime;

// ── XML ─────────────────────────────────────────────────────────────────

TEST_CASE("XmlDocument parse and query", "[runtime][xml]") {
    XmlDocument doc;
    REQUIRE(doc.parse(R"(
        <plugin name="TestPlugin" version="1.0">
            <param>Gain</param>
            <param>Mix</param>
        </plugin>
    )"));

    REQUIRE(doc.is_valid());
    REQUIRE(doc.root_name() == "plugin");

    auto name = doc.root_attribute("name");
    REQUIRE(name.has_value());
    REQUIRE(*name == "TestPlugin");
}

TEST_CASE("XmlDocument XPath", "[runtime][xml]") {
    XmlDocument doc;
    doc.parse(R"(
        <root>
            <item>one</item>
            <item>two</item>
            <item>three</item>
        </root>
    )");

    auto items = doc.xpath_strings("//item");
    REQUIRE(items.size() == 3);
    REQUIRE(items[0] == "one");
    REQUIRE(items[2] == "three");

    auto first = doc.xpath_string("//item[1]");
    REQUIRE(first.has_value());
    REQUIRE(*first == "one");
}

TEST_CASE("XmlDocument walk", "[runtime][xml]") {
    XmlDocument doc;
    doc.parse("<root><a>1</a><b>2</b></root>");

    std::vector<std::pair<std::string, std::string>> elements;
    doc.walk([&](std::string_view name, std::string_view text) {
        elements.push_back({std::string(name), std::string(text)});
    });

    REQUIRE(elements.size() == 3);  // root, a, b
}

TEST_CASE("XmlDocument to_string round-trip", "[runtime][xml]") {
    XmlDocument doc;
    doc.parse("<root><child>value</child></root>");

    auto xml_str = doc.to_string();
    REQUIRE(xml_str.find("<child>value</child>") != std::string::npos);

    XmlDocument doc2;
    REQUIRE(doc2.parse(xml_str));
    REQUIRE(doc2.root_name() == "root");
}

TEST_CASE("XmlDocument file I/O", "[runtime][xml]") {
    TemporaryFile tmp(".xml");

    XmlDocument doc;
    doc.parse("<settings><theme>dark</theme></settings>");
    REQUIRE(doc.save_file(tmp.path_string()));

    XmlDocument doc2;
    REQUIRE(doc2.load_file(tmp.path_string()));
    auto theme = doc2.xpath_string("//theme");
    REQUIRE(theme.has_value());
    REQUIRE(*theme == "dark");
}

TEST_CASE("XmlDocument invalid XML", "[runtime][xml]") {
    XmlDocument doc;
    REQUIRE_FALSE(doc.parse("not xml at all <<<"));
    REQUIRE_FALSE(doc.is_valid());
    REQUIRE_FALSE(doc.error().empty());
}

TEST_CASE("xml_generate creates document", "[runtime][xml]") {
    auto xml = xml_generate("preset", {
        {"name", "Default"},
        {"gain", "0.5"},
        {"mix", "1.0"}
    });

    XmlDocument doc;
    REQUIRE(doc.parse(xml));
    REQUIRE(doc.root_name() == "preset");
    auto name = doc.xpath_string("//name");
    REQUIRE(name.has_value());
    REQUIRE(*name == "Default");
}

// ── ZIP/GZIP compression ────────────────────────────────────────────────

TEST_CASE("gzip compress/decompress round-trip", "[runtime][zip]") {
    std::string original = "Hello, this is a test string for compression. "
                          "It should compress well because it has repeated patterns. "
                          "Repeated patterns, yes, repeated patterns!";

    auto compressed = gzip_compress(original);
    REQUIRE(compressed.has_value());
    REQUIRE(compressed->size() < original.size());

    auto decompressed = gzip_decompress_string(compressed->data(), compressed->size());
    REQUIRE(decompressed.has_value());
    REQUIRE(*decompressed == original);
}

TEST_CASE("gzip empty input", "[runtime][zip]") {
    auto compressed = gzip_compress("", 6);
    REQUIRE(compressed.has_value());

    auto decompressed = gzip_decompress_string(compressed->data(), compressed->size());
    REQUIRE(decompressed.has_value());
    REQUIRE(decompressed->empty());
}

TEST_CASE("deflate compress/decompress round-trip", "[runtime][zip]") {
    std::string original = "Deflate test data with some repeated content repeated content!";
    auto data = reinterpret_cast<const uint8_t*>(original.data());

    auto compressed = deflate_compress(data, original.size());
    REQUIRE(compressed.has_value());

    auto decompressed = deflate_decompress(compressed->data(), compressed->size());
    REQUIRE(decompressed.has_value());

    std::string result(decompressed->begin(), decompressed->end());
    REQUIRE(result == original);
}

TEST_CASE("gzip binary data round-trip", "[runtime][zip]") {
    // Generate binary data
    std::vector<uint8_t> data(1000);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>(i % 256);

    auto compressed = gzip_compress(data.data(), data.size());
    REQUIRE(compressed.has_value());

    auto decompressed = gzip_decompress(compressed->data(), compressed->size());
    REQUIRE(decompressed.has_value());
    REQUIRE(*decompressed == data);
}
