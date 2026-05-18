#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/xml.hpp>
#include <pulp/runtime/zip.hpp>
#include <pulp/runtime/temporary_file.hpp>
#include <fstream>
#include <cstring>
#include <string>
#include <vector>

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

TEST_CASE("XmlDocument XPath handles attributes and invalid expressions", "[runtime][xml]") {
    XmlDocument doc;
    REQUIRE(doc.parse(R"(
        <root>
            <plugin id="alpha" enabled="true">Synth</plugin>
            <plugin id="beta" enabled="false">Delay</plugin>
        </root>
    )"));

    auto ids = doc.xpath_strings("//plugin/@id");
    REQUIRE(ids.size() == 2);
    REQUIRE(ids[0] == "alpha");
    REQUIRE(ids[1] == "beta");

    REQUIRE_FALSE(doc.xpath_string("//plugin[@id='missing']").has_value());
    REQUIRE(doc.xpath_strings("//*[broken").empty());
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

TEST_CASE("XmlDocument parse failure marks invalid and later success clears error",
          "[runtime][xml][coverage]") {
    XmlDocument doc;
    REQUIRE(doc.parse(R"(<root name="before"><child>old</child></root>)"));
    REQUIRE(doc.is_valid());
    REQUIRE(doc.root_name() == "root");

    REQUIRE_FALSE(doc.parse("<root><broken></root>"));
    REQUIRE_FALSE(doc.is_valid());
    REQUIRE_FALSE(doc.error().empty());

    REQUIRE(doc.parse(R"(<next name="after"><child>new</child></next>)"));
    REQUIRE(doc.is_valid());
    REQUIRE(doc.error().empty());
    REQUIRE(doc.root_name() == "next");
    REQUIRE(doc.root_attribute("name") == std::optional<std::string>{"after"});
    REQUIRE(doc.xpath_string("//child") == std::optional<std::string>{"new"});
}

TEST_CASE("XmlDocument load_file failure clears prior state and save_file reports bad paths",
          "[runtime][xml][coverage]") {
    TemporaryFile tmp(".xml");

    XmlDocument doc;
    REQUIRE(doc.parse("<settings><theme>dark</theme></settings>"));
    REQUIRE(doc.save_file(tmp.path_string()));

    XmlDocument loaded;
    REQUIRE(loaded.load_file(tmp.path_string()));
    REQUIRE(loaded.xpath_string("//theme") == std::optional<std::string>{"dark"});

    REQUIRE_FALSE(loaded.load_file(tmp.path_string() + ".missing"));
    REQUIRE_FALSE(loaded.is_valid());
    REQUIRE_FALSE(loaded.error().empty());
    REQUIRE(loaded.root_name().empty());

    REQUIRE_FALSE(doc.save_file(tmp.path_string() + "/nested.xml"));
}

TEST_CASE("XmlDocument empty document queries are inert",
          "[runtime][xml][coverage][issue-641]") {
    XmlDocument doc;

    REQUIRE_FALSE(doc.is_valid());
    REQUIRE(doc.root_name().empty());
    REQUIRE_FALSE(doc.root_attribute("missing").has_value());
    REQUIRE_FALSE(doc.xpath_string("//missing").has_value());
    REQUIRE(doc.xpath_strings("//missing").empty());

    int walk_count = 0;
    doc.walk([&](std::string_view, std::string_view) {
        ++walk_count;
    });
    REQUIRE(walk_count == 0);
}

TEST_CASE("XmlDocument move construction and assignment preserve parsed state",
          "[runtime][xml][coverage][issue-641]") {
    XmlDocument original;
    REQUIRE(original.parse(R"(<root name="first"><child>value</child></root>)"));

    XmlDocument moved(std::move(original));
    REQUIRE(moved.is_valid());
    REQUIRE(moved.root_name() == "root");
    REQUIRE(moved.root_attribute("name") == std::optional<std::string>{"first"});
    REQUIRE_FALSE(original.is_valid());
    REQUIRE(original.root_name().empty());

    XmlDocument replacement;
    REQUIRE(replacement.parse(R"(<replacement><leaf>next</leaf></replacement>)"));

    moved = std::move(replacement);
    REQUIRE(moved.is_valid());
    REQUIRE(moved.root_name() == "replacement");
    REQUIRE(moved.xpath_string("//leaf") == std::optional<std::string>{"next"});
    REQUIRE_FALSE(replacement.is_valid());
    REQUIRE(replacement.root_name().empty());
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

TEST_CASE("xml_generate handles empty and repeated elements",
          "[runtime][xml][coverage][issue-641]") {
    auto xml = xml_generate("metadata", {
        {"tag", "alpha"},
        {"tag", "beta"},
        {"empty", ""}
    });

    XmlDocument doc;
    REQUIRE(doc.parse(xml));
    REQUIRE(doc.root_name() == "metadata");

    auto tags = doc.xpath_strings("//tag");
    REQUIRE(tags.size() == 2);
    REQUIRE(tags[0] == "alpha");
    REQUIRE(tags[1] == "beta");

    auto empty = doc.xpath_string("//empty");
    REQUIRE(empty.has_value());
    REQUIRE(empty->empty());
}

TEST_CASE("xml_generate escapes text content", "[runtime][xml]") {
    auto xml = xml_generate("preset", {
        {"name", "A&B <Default>"},
        {"quote", "\"quoted\""}
    });

    XmlDocument doc;
    REQUIRE(doc.parse(xml));
    auto name = doc.xpath_string("//name");
    REQUIRE(name.has_value());
    REQUIRE(*name == "A&B <Default>");
    REQUIRE(xml.find("A&amp;B &lt;Default&gt;") != std::string::npos);
}

TEST_CASE("xml_generate supports empty roots and escaped element names from callers",
          "[runtime][xml][coverage]") {
    auto empty = xml_generate("empty", {});
    XmlDocument empty_doc;
    REQUIRE(empty_doc.parse(empty));
    REQUIRE(empty_doc.root_name() == "empty");
    REQUIRE(empty_doc.xpath_strings("//*").size() == 1);

    auto xml = xml_generate("preset", {
        {"amp", "Tom & Jerry"},
        {"apostrophe", "can't"},
        {"quote", R"("quoted")"},
    });
    XmlDocument doc;
    REQUIRE(doc.parse(xml));
    REQUIRE(doc.xpath_string("//amp") == std::optional<std::string>{"Tom & Jerry"});
    REQUIRE(doc.xpath_string("//apostrophe") == std::optional<std::string>{"can't"});
    REQUIRE(doc.xpath_string("//quote") == std::optional<std::string>{R"("quoted")"});
    REQUIRE(xml.find("Tom &amp; Jerry") != std::string::npos);
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

TEST_CASE("gzip_decompress_string preserves embedded NUL bytes",
          "[runtime][zip][coverage]") {
    const std::vector<uint8_t> original = {
        'p', 'u', 'l', 'p', 0x00, 'b', 'i', 'n', 0x00, 0xff,
    };
    auto compressed = gzip_compress(original.data(), original.size());
    REQUIRE(compressed.has_value());

    auto decompressed = gzip_decompress_string(compressed->data(), compressed->size());
    REQUIRE(decompressed.has_value());
    REQUIRE(decompressed->size() == original.size());
    REQUIRE(std::memcmp(decompressed->data(), original.data(), original.size()) == 0);
}

TEST_CASE("deflate compression levels round-trip edge settings",
          "[runtime][zip][coverage]") {
    const std::string payload = "level check level check level check";
    for (int level : {0, 9}) {
        INFO("level: " << level);
        auto compressed = deflate_compress(
            reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), level);
        REQUIRE(compressed.has_value());

        auto decompressed = deflate_decompress(compressed->data(), compressed->size());
        REQUIRE(decompressed.has_value());
        REQUIRE(std::string(decompressed->begin(), decompressed->end()) == payload);
    }
}

TEST_CASE("zip helpers reject malformed deflate and zlib input",
          "[runtime][zip][coverage][phase3-large]") {
    const uint8_t malformed[] = {0xde, 0xad, 0xbe, 0xef};
    REQUIRE_FALSE(deflate_decompress(malformed, sizeof(malformed)).has_value());
    REQUIRE_FALSE(gzip_decompress(malformed, sizeof(malformed)).has_value());
}

TEST_CASE("deflate empty payload round-trips through raw helpers",
          "[runtime][zip][coverage][phase3-large]") {
    auto compressed = deflate_compress(nullptr, 0);
    REQUIRE(compressed.has_value());

    auto decompressed = deflate_decompress(compressed->data(), compressed->size());
    REQUIRE(decompressed.has_value());
    REQUIRE(decompressed->empty());
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

TEST_CASE("gzip_decompress accepts optional RFC 1952 header fields", "[runtime][zip]") {
    const std::string original = "payload with optional gzip header metadata";
    auto compressed = gzip_compress(original);
    REQUIRE(compressed.has_value());
    REQUIRE(compressed->size() > 18);

    std::vector<uint8_t> with_metadata = {
        0x1f, 0x8b, 0x08, 0x1e,  // FEXTRA | FNAME | FCOMMENT | FHCRC
        0x00, 0x00, 0x00, 0x00,  // MTIME
        0x00, 0xff,              // XFL, OS
        0x02, 0x00,              // XLEN
        0xca, 0xfe,              // FEXTRA bytes
        'p', 'r', 'e', 's', 'e', 't', 0x00,
        'r', 'o', 'u', 'n', 'd', 't', 'r', 'i', 'p', 0x00,
        0x00, 0x00,              // FHCRC placeholder; decoder skips it
    };
    with_metadata.insert(with_metadata.end(), compressed->begin() + 10, compressed->end());

    auto out = gzip_decompress_string(with_metadata.data(), with_metadata.size());
    REQUIRE(out.has_value());
    REQUIRE(*out == original);
}

TEST_CASE("gzip_decompress accepts FTEXT flag and empty optional strings",
          "[runtime][zip][coverage]") {
    const std::string original = "text flag payload";
    auto compressed = gzip_compress(original);
    REQUIRE(compressed.has_value());
    REQUIRE(compressed->size() > 18);

    std::vector<uint8_t> with_flags = {
        0x1f, 0x8b, 0x08, 0x19,  // FTEXT | FNAME | FCOMMENT
        0x00, 0x00, 0x00, 0x00,
        0x00, 0xff,
        0x00,                    // empty FNAME
        0x00,                    // empty FCOMMENT
    };
    with_flags.insert(with_flags.end(), compressed->begin() + 10, compressed->end());

    auto out = gzip_decompress_string(with_flags.data(), with_flags.size());
    REQUIRE(out.has_value());
    REQUIRE(*out == original);
}

TEST_CASE("gzip_decompress rejects reserved RFC 1952 flag bits", "[runtime][zip]") {
    auto compressed = gzip_compress(std::string{"reserved flag check"});
    REQUIRE(compressed.has_value());
    REQUIRE(compressed->size() >= 10);

    (*compressed)[3] = 0x20;
    auto out = gzip_decompress(compressed->data(), compressed->size());
    REQUIRE_FALSE(out.has_value());
}

TEST_CASE("gzip_decompress rejects malformed RFC 1952 optional headers", "[runtime][zip][issue-641]") {
    auto header = [](uint8_t flags) {
        return std::vector<uint8_t>{
            0x1f, 0x8b, 0x08, flags,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0xff,
        };
    };

    std::vector<std::vector<uint8_t>> malformed;

    auto bad_method = header(0x00);
    bad_method[2] = 0x00;
    malformed.push_back(bad_method);

    malformed.push_back(header(0x04));  // FEXTRA set, but XLEN is missing.

    auto oversized_extra = header(0x04);
    oversized_extra.insert(oversized_extra.end(), {0xff, 0xff});
    malformed.push_back(oversized_extra);

    auto unterminated_name = header(0x08);
    unterminated_name.insert(unterminated_name.end(), {'n', 'a', 'm', 'e'});
    malformed.push_back(unterminated_name);

    auto unterminated_comment = header(0x10);
    unterminated_comment.insert(unterminated_comment.end(), {'n', 'o', 't', 'e'});
    malformed.push_back(unterminated_comment);

    malformed.push_back(header(0x02));  // FHCRC set, but the two CRC bytes are missing.

    for (const auto& bytes : malformed) {
        INFO("malformed gzip header length: " << bytes.size());
        REQUIRE_FALSE(gzip_decompress(bytes.data(), bytes.size()).has_value());
    }
}

TEST_CASE("gzip_decompress rejects gzip input with corrupt trailer ISIZE", "[runtime][zip][issue-641]") {
    std::string original = "deterministic ISIZE check payload";
    auto compressed = gzip_compress(original);
    REQUIRE(compressed.has_value());
    REQUIRE(compressed->size() >= 12);

    (*compressed)[compressed->size() - 1] ^= 0x01;
    auto out = gzip_decompress(compressed->data(), compressed->size());
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

TEST_CASE("gzip_decompress rejects an invalid concatenated member header",
          "[runtime][zip][coverage]") {
    auto first = gzip_compress(std::string{"first"});
    REQUIRE(first.has_value());

    std::vector<uint8_t> concat;
    concat.insert(concat.end(), first->begin(), first->end());
    concat.insert(concat.end(), {
        0x1f, 0x8b, 0x00, 0x00,  // bad compression method
        0x00, 0x00, 0x00, 0x00,
        0x00, 0xff,
    });

    REQUIRE_FALSE(gzip_decompress(concat.data(), concat.size()).has_value());
}
