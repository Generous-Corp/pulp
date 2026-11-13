// XmlPropertiesFile: the canonical byte layout, tolerant reading, the
// duplicate rule, fail-closed loading with one backup, atomic save and
// no-change-no-write.
#include <pulp/state/xml_properties_file.hpp>

#include "support/unique_temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using pulp::state::XmlPropertiesError;
using pulp::state::XmlPropertiesFile;
using Entries = std::vector<XmlPropertiesFile::Entry>;

namespace {

constexpr std::string_view kHead = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n\r\n<PROPERTIES>\r\n";
constexpr std::string_view kTail = "</PROPERTIES>\r\n";

std::string canonical(std::string_view body) {
    return std::string(kHead) + std::string(body) + std::string(kTail);
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void spit(const std::filesystem::path& p, std::string_view bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::size_t entries_in(const std::filesystem::path& dir) {
    std::size_t n = 0;
    for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator(dir))
        ++n;
    return n;
}

struct Scratch {
    std::filesystem::path root = pulp::test::make_unique_temp_dir("pulp_xml_properties");
    ~Scratch() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

} // namespace

TEST_CASE("XmlPropertiesFile writes the canonical layout byte for byte", "[state][xml-properties]") {
    CHECK(XmlPropertiesFile::serialize({}) == canonical(""));
    CHECK(XmlPropertiesFile::serialize({{"volume", "7"}, {"theme", "dark"}}) ==
          canonical("  <VALUE name=\"volume\" val=\"7\"/>\r\n"
                    "  <VALUE name=\"theme\" val=\"dark\"/>\r\n"));
}

TEST_CASE("XmlPropertiesFile escapes like the format's writer", "[state][xml-properties]") {
    // Named entities for & < > ", references for tab and line feed, the
    // apostrophe raw, every non-ASCII character as a decimal reference, and
    // leading and trailing spaces kept.
    const Entries entries = {
        {"markup", "a & b <c> \"d\" 'e'"},
        {"controls", "x\ty\nz"},
        {"accents", "caf\xC3\xA9 \xE2\x82\xAC"},     // café €
        {"astral", "\xF0\x9F\x98\x80"},              // U+1F600
        {"  spaced  ", "  both ends  "},
    };
    const auto bytes = XmlPropertiesFile::serialize(entries);
    CHECK(bytes == canonical("  <VALUE name=\"markup\" val=\"a &amp; b &lt;c&gt; &quot;d&quot; 'e'\"/>\r\n"
                             "  <VALUE name=\"controls\" val=\"x&#9;y&#10;z\"/>\r\n"
                             "  <VALUE name=\"accents\" val=\"caf&#233; &#8364;\"/>\r\n"
                             "  <VALUE name=\"astral\" val=\"&#128512;\"/>\r\n"
                             "  <VALUE name=\"  spaced  \" val=\"  both ends  \"/>\r\n"));
    for (const auto c : bytes)
        CHECK(static_cast<unsigned char>(c) < 0x80);

    // And it reads back to the same entries.
    const auto back = XmlPropertiesFile::parse(bytes);
    REQUIRE(back.has_value());
    CHECK(*back == entries);
}

TEST_CASE("XmlPropertiesFile reads what other writers produce", "[state][xml-properties]") {
    const std::string input =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!-- a comment before the root -->\n"
        "<PROPERTIES>\n"
        "\n"
        "  <!-- a comment inside -->\n"
        "  <VALUE name='single' val='quoted'/>\n"
        "  <VALUE name=\"entities\" val=\"&lt;&amp;&gt;&quot;&apos;&#233;&#x20AC;\"/>\n"
        "  <VALUE name=\"literal\" val=\"caf\xC3\xA9\"/>\n"
        "  <VALUE name=\"extra\" val=\"1\" unit=\"dB\"/>\n"
        "  <VALUE val=\"no name\"/>\n"
        "  <OTHER name=\"ignored\" val=\"x\"/>\n"
        "  <VALUE name=\"no-val\"/>\n"
        "  <VALUE name=\" keep \" val=\" spaces \"></VALUE>\n"
        "</PROPERTIES>\n";
    const auto entries = XmlPropertiesFile::parse(input);
    REQUIRE(entries.has_value());
    CHECK(*entries == Entries{
                          {"single", "quoted"},
                          {"entities", "<&>\"'\xC3\xA9\xE2\x82\xAC"},
                          {"literal", "caf\xC3\xA9"},
                          {"extra", "1"},
                          {"no-val", ""},
                          {" keep ", " spaces "},
                      });
}

TEST_CASE("XmlPropertiesFile keeps a duplicate's first position and last value", "[state][xml-properties]") {
    const auto entries = XmlPropertiesFile::parse(canonical("  <VALUE name=\"a\" val=\"1\"/>\r\n"
                                                            "  <VALUE name=\"b\" val=\"2\"/>\r\n"
                                                            "  <VALUE name=\"a\" val=\"3\"/>\r\n"));
    REQUIRE(entries.has_value());
    CHECK(*entries == Entries{{"a", "3"}, {"b", "2"}});
}

TEST_CASE("XmlPropertiesFile edits in place and appends new names", "[state][xml-properties]") {
    Scratch dir;
    const auto path = dir.root / "Product.xml";
    // LF endings, a comment and an unknown element: all normalized away on save.
    spit(path, "<?xml version=\"1.0\"?>\n<PROPERTIES>\n  <!-- c -->\n  <VALUE name=\"b\" val=\"2\"/>\n"
               "  <OTHER/>\n  <VALUE name=\"a\" val=\"1\"/>\n</PROPERTIES>\n");
    auto file = XmlPropertiesFile::load(path);
    REQUIRE(file.has_value());
    file->set("a", "10");
    file->set("new", "x");
    CHECK(file->dirty());
    REQUIRE(file->save());
    CHECK(slurp(path) == canonical("  <VALUE name=\"b\" val=\"2\"/>\r\n"
                                   "  <VALUE name=\"a\" val=\"10\"/>\r\n"
                                   "  <VALUE name=\"new\" val=\"x\"/>\r\n"));
    CHECK_FALSE(file->dirty());
    // The save leaves only the file itself: no temporary, no backup.
    CHECK(entries_in(dir.root) == 1);

    CHECK(file->remove("b"));
    CHECK_FALSE(file->remove("b"));
    REQUIRE(file->save());
    CHECK(slurp(path) == canonical("  <VALUE name=\"a\" val=\"10\"/>\r\n"
                                   "  <VALUE name=\"new\" val=\"x\"/>\r\n"));
}

TEST_CASE("XmlPropertiesFile writes nothing when nothing changed", "[state][xml-properties]") {
    Scratch dir;
    const auto path = dir.root / "Product.xml";
    const std::string original = "<PROPERTIES>\n  <VALUE name=\"a\" val=\"1\"/>\n</PROPERTIES>\n";
    spit(path, original);
    const auto mtime = std::filesystem::last_write_time(path);
#ifndef _WIN32
    struct stat before {};
    REQUIRE(::stat(path.c_str(), &before) == 0);
#endif

    auto file = XmlPropertiesFile::load(path);
    REQUIRE(file.has_value());
    file->set("a", "1");           // the same value
    REQUIRE(file->save());
    file->set("a", "2");           // changed and changed back
    file->set("a", "1");
    CHECK_FALSE(file->dirty());
    REQUIRE(file->save());

    CHECK(slurp(path) == original);
    CHECK(std::filesystem::last_write_time(path) == mtime);
#ifndef _WIN32
    struct stat after {};
    REQUIRE(::stat(path.c_str(), &after) == 0);
    CHECK(after.st_ino == before.st_ino);
#endif
    CHECK(entries_in(dir.root) == 1);
}

TEST_CASE("XmlPropertiesFile fails closed on a file it cannot read", "[state][xml-properties]") {
    const std::vector<std::string> unreadable = {
        "",                                                        // empty
        "volume=7\n",                                              // not XML
        "<PROPERTIES>\n  <VALUE name=\"a\" val=\"1\"/>\n",         // unterminated
        "<SETTINGS>\n  <VALUE name=\"a\" val=\"1\"/>\n</SETTINGS>\n", // wrong root
        "<PROPERTIES/>\n<PROPERTIES/>\n",                          // two roots
    };
    for (const auto& bytes : unreadable) {
        CAPTURE(bytes);
        XmlPropertiesError error;
        CHECK_FALSE(XmlPropertiesFile::parse(bytes, &error));
        CHECK(error.kind == XmlPropertiesError::Kind::parse);
        CHECK_FALSE(error.message.empty());

        Scratch dir;
        const auto path = dir.root / "Product.xml";
        spit(path, bytes);
        error = {};
        CHECK_FALSE(XmlPropertiesFile::load(path, &error));
        CHECK(error.kind == XmlPropertiesError::Kind::parse);
        // The file is kept as it was, and copied once beside it.
        CHECK(slurp(path) == bytes);
        CHECK(error.backup_path == XmlPropertiesFile::backup_path_for(path));
        CHECK(slurp(error.backup_path) == bytes);
        CHECK(entries_in(dir.root) == 2);
    }
}

TEST_CASE("XmlPropertiesFile never overwrites an existing backup", "[state][xml-properties]") {
    Scratch dir;
    const auto path = dir.root / "Product.xml";
    spit(path, "first broken file");
    REQUIRE_FALSE(XmlPropertiesFile::load(path));
    spit(path, "second broken file");
    XmlPropertiesError error;
    REQUIRE_FALSE(XmlPropertiesFile::load(path, &error));
    CHECK(slurp(XmlPropertiesFile::backup_path_for(path)) == "first broken file");
    CHECK(slurp(path) == "second broken file");
    CHECK(entries_in(dir.root) == 2);
}

TEST_CASE("XmlPropertiesFile creates a missing file only when it has something to write",
          "[state][xml-properties]") {
    Scratch dir;
    const auto path = dir.root / "Product.xml";
    auto file = XmlPropertiesFile::load(path);
    REQUIRE(file.has_value());
    CHECK(file->entries().empty());
    REQUIRE(file->save());
    CHECK_FALSE(std::filesystem::exists(path));
    file->set("a", "1");
    REQUIRE(file->save());
    CHECK(slurp(path) == canonical("  <VALUE name=\"a\" val=\"1\"/>\r\n"));
    CHECK(entries_in(dir.root) == 1);
}

TEST_CASE("XmlPropertiesFile reports a failed save and leaves nothing behind", "[state][xml-properties]") {
    Scratch dir;
    const auto path = dir.root / "missing-dir" / "Product.xml";
    XmlPropertiesFile file(path);
    file.set("a", "1");
    XmlPropertiesError error;
    CHECK_FALSE(file.save(&error));
    CHECK(error.kind == XmlPropertiesError::Kind::io);
    CHECK(file.dirty());
    CHECK(entries_in(dir.root) == 0);
}
