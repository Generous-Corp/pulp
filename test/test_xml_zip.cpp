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

// ── RFC 1952 gzip wire-format compliance (issue-468 follow-up) ──────────
//
// gzip_compress() must emit a real RFC 1952 stream — magic bytes, deflate
// CM, valid CRC32 and ISIZE trailer — not a zlib (RFC 1950) stream wearing
// the "gzip" name. The bundle parser in PR #731 had to work around that
// silent zlib output by stripping a fake gzip header off zlib data; with
// this fix in place the workaround can be deleted.
TEST_CASE("gzip_compress emits RFC 1952 magic bytes and deflate CM", "[runtime][zip][issue-468]") {
    std::string original = "RFC 1952 compliance check — magic bytes 0x1f 0x8b, CM=8, valid trailer.";
    auto compressed = gzip_compress(original);
    REQUIRE(compressed.has_value());
    REQUIRE(compressed->size() >= 18);  // 10-byte header + at least 8-byte trailer
    CHECK((*compressed)[0] == 0x1f);
    CHECK((*compressed)[1] == 0x8b);
    CHECK((*compressed)[2] == 0x08);  // CM = deflate
    // FLG should clear the reserved high bits.
    CHECK(((*compressed)[3] & 0xE0) == 0);
}

TEST_CASE("gzip_decompress accepts external RFC 1952 input", "[runtime][zip][issue-468]") {
    // A real gzip stream produced by Python's `gzip.compress(b"hello\n", mtime=0)`,
    // matching what `printf "hello\n" | gzip -n` would emit on a typical
    // Unix host. Header: 1f 8b 08 00 00 00 00 00 02 ff  (FLG=0, MTIME=0,
    // XFL=2 max-compression, OS=0xff unknown). Then deflate + CRC32 + ISIZE.
    static constexpr uint8_t kReal[] = {
        0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff,
        0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0xe7, 0x02, 0x00,
        0x20, 0x30, 0x3a, 0x36, 0x06, 0x00, 0x00, 0x00,
    };
    auto out = gzip_decompress_string(kReal, sizeof(kReal));
    REQUIRE(out.has_value());
    REQUIRE(*out == "hello\n");
}

TEST_CASE("gzip_decompress rejects gzip input with corrupt trailer CRC", "[runtime][zip][issue-468]") {
    std::string original = "deterministic CRC check payload";
    auto compressed = gzip_compress(original);
    REQUIRE(compressed.has_value());
    REQUIRE(compressed->size() >= 12);
    // Flip a bit in the CRC32 field (last 8 bytes are CRC32 then ISIZE).
    (*compressed)[compressed->size() - 8] ^= 0x01;
    auto out = gzip_decompress(compressed->data(), compressed->size());
    REQUIRE_FALSE(out.has_value());
}

TEST_CASE("gzip_decompress still accepts legacy zlib input (back-compat)", "[runtime][zip][issue-468]") {
    // A pre-baked zlib (RFC 1950) stream produced by `printf "hello\n" |
    // pigz --zlib -c` (and verified to round-trip through Python's
    // zlib.decompress). The point is to exercise the zlib lane via input
    // that was *not* generated by gzip_compress in this build, since we
    // changed gzip_compress to emit RFC 1952 instead of RFC 1950.
    //
    //   78 9c — zlib header (CMF=0x78 deflate+32K window, FLG=0x9c default
    //           compression, FLG.FCHECK consistent with CMF mod 31 == 0).
    //   cb 48 cd c9 c9 e7 02 00 — deflate block for "hello\n".
    //   08 4b 02 1f — ADLER-32 trailer big-endian over "hello\n".
    static constexpr uint8_t kZlibHello[] = {
        0x78, 0x9c,
        0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0xe7, 0x02, 0x00,
        0x08, 0x4b, 0x02, 0x1f,
    };
    auto out = gzip_decompress_string(kZlibHello, sizeof(kZlibHello));
    REQUIRE(out.has_value());
    REQUIRE(*out == "hello\n");
}

TEST_CASE("gzip_decompress rejects truncated header", "[runtime][zip][issue-468]") {
    static constexpr uint8_t kPartial[] = {0x1f, 0x8b, 0x08};  // missing rest of fixed header
    auto out = gzip_decompress(kPartial, sizeof(kPartial));
    REQUIRE_FALSE(out.has_value());
}

// Codex P2 on PR #747: RFC 1952 §2.2 permits multiple gzip members
// concatenated back-to-back. Tools like pigz, `cat a.gz b.gz`, and
// any producer that splits streams emit them. The decoder must inflate
// each member and concatenate the outputs, not treat the whole input as
// one member.
TEST_CASE("gzip_decompress handles concatenated RFC 1952 members",
          "[runtime][zip][issue-468]") {
    using namespace pulp::runtime;
    const std::string a = "first member payload\n";
    const std::string b = "second member payload\n";
    const std::string c = "third\n";

    auto ga = gzip_compress(a);
    auto gb = gzip_compress(b);
    auto gc = gzip_compress(c);
    REQUIRE(ga.has_value());
    REQUIRE(gb.has_value());
    REQUIRE(gc.has_value());

    // Concatenate the three single-member gzip streams.
    std::vector<uint8_t> concat;
    concat.insert(concat.end(), ga->begin(), ga->end());
    concat.insert(concat.end(), gb->begin(), gb->end());
    concat.insert(concat.end(), gc->begin(), gc->end());

    auto inflated = gzip_decompress(concat.data(), concat.size());
    REQUIRE(inflated.has_value());
    const std::string round_trip(inflated->begin(), inflated->end());
    REQUIRE(round_trip == a + b + c);
}

TEST_CASE("gzip_decompress rejects trailing garbage after the last member",
          "[runtime][zip][issue-468]") {
    using namespace pulp::runtime;
    auto g = gzip_compress(std::string{"hello\n"});
    REQUIRE(g.has_value());

    // Append non-gzip bytes after the trailer; per RFC 1952 a well-formed
    // stream is "concatenated complete members," so a stray suffix must
    // be rejected (silent partial decode would lose the suffix).
    g->push_back('?');
    g->push_back('?');
    auto out = gzip_decompress(g->data(), g->size());
    REQUIRE_FALSE(out.has_value());
}
