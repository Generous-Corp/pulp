// Tests for pulp::runtime::FileSearchPath — ordered search semantics,
// duplicate suppression, find vs. find_all, and PATH-style serialization
// round-trip.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <pulp/runtime/file_search_path.hpp>

using pulp::runtime::FileSearchPath;
namespace fs = std::filesystem;

namespace {

// Create a unique scratch dir under temp_directory_path/<token>/ and ensure
// it is removed at scope exit.
struct ScratchDir {
    fs::path root;

    explicit ScratchDir(const std::string& token) {
        static std::atomic<unsigned> counter{0};
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path()
             / ("pulp-file-search-path-test-" + token + "-"
                + std::to_string(stamp) + "-"
                + std::to_string(counter.fetch_add(1)));
        fs::create_directories(root);
    }

    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path mkdir(const std::string& name) {
        auto p = root / name;
        fs::create_directories(p);
        return p;
    }

    void touch(const fs::path& p) {
        std::ofstream(p) << "x";
    }
};

}  // namespace

TEST_CASE("FileSearchPath starts empty", "[runtime][file_search_path]") {
    FileSearchPath path;
    REQUIRE(path.empty());
    REQUIRE(path.size() == 0);
    REQUIRE_FALSE(path.find("anything.txt").has_value());
    REQUIRE(path.find_all("anything.txt").empty());
}

TEST_CASE("FileSearchPath add appends and dedupes",
          "[runtime][file_search_path]") {
    FileSearchPath path;
    path.add("/a");
    path.add("/b");
    path.add("/a");  // duplicate — must not append
    REQUIRE(path.size() == 2);
    REQUIRE(path[0] == fs::path("/a"));
    REQUIRE(path[1] == fs::path("/b"));
}

TEST_CASE("FileSearchPath add_at_front moves existing entry to front",
          "[runtime][file_search_path]") {
    FileSearchPath path;
    path.add("/a");
    path.add("/b");
    path.add("/c");
    path.add_at_front("/c");  // move /c to front
    REQUIRE(path.size() == 3);
    REQUIRE(path[0] == fs::path("/c"));
    REQUIRE(path[1] == fs::path("/a"));
    REQUIRE(path[2] == fs::path("/b"));

    path.add_at_front("/d");  // brand-new entry at front
    REQUIRE(path.size() == 4);
    REQUIRE(path[0] == fs::path("/d"));
}

TEST_CASE("FileSearchPath remove drops indexed entry, OOB no-op",
          "[runtime][file_search_path]") {
    FileSearchPath path;
    path.add("/a");
    path.add("/b");
    path.add("/c");
    path.remove(1);
    REQUIRE(path.size() == 2);
    REQUIRE(path[0] == fs::path("/a"));
    REQUIRE(path[1] == fs::path("/c"));
    path.remove(99);  // OOB — no-op
    REQUIRE(path.size() == 2);
}

TEST_CASE("FileSearchPath find returns first match in search order",
          "[runtime][file_search_path]") {
    ScratchDir scratch("find");
    auto d1 = scratch.mkdir("d1");
    auto d2 = scratch.mkdir("d2");
    auto d3 = scratch.mkdir("d3");
    // Only d2 and d3 hold the target.
    scratch.touch(d2 / "preset.xml");
    scratch.touch(d3 / "preset.xml");

    FileSearchPath path;
    path.add(d1);
    path.add(d2);
    path.add(d3);

    auto hit = path.find("preset.xml");
    REQUIRE(hit.has_value());
    REQUIRE(*hit == d2 / "preset.xml");
}

TEST_CASE("FileSearchPath find returns nullopt when filename missing",
          "[runtime][file_search_path]") {
    ScratchDir scratch("miss");
    auto d1 = scratch.mkdir("d1");
    auto d2 = scratch.mkdir("d2");

    FileSearchPath path;
    path.add(d1);
    path.add(d2);

    REQUIRE_FALSE(path.find("nope.xml").has_value());
    REQUIRE_FALSE(path.find("").has_value());
}

TEST_CASE("FileSearchPath find_all returns every match across roots in order",
          "[runtime][file_search_path]") {
    ScratchDir scratch("findall");
    auto a = scratch.mkdir("a");
    auto b = scratch.mkdir("b");
    auto c = scratch.mkdir("c");
    scratch.touch(a / "font.ttf");
    // intentionally NOT b/font.ttf
    scratch.touch(c / "font.ttf");

    FileSearchPath path;
    path.add(a);
    path.add(b);
    path.add(c);

    auto hits = path.find_all("font.ttf");
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0] == a / "font.ttf");
    REQUIRE(hits[1] == c / "font.ttf");
}

TEST_CASE("FileSearchPath serializes with platform separator and round-trips",
          "[runtime][file_search_path]") {
    const char sep = FileSearchPath::separator();
#if defined(_WIN32) || defined(_WIN64)
    REQUIRE(sep == ';');
#else
    REQUIRE(sep == ':');
#endif

    FileSearchPath path;
    path.add("/usr/local/share");
    path.add("/opt/pulp/presets");

    const std::string serialized = path.to_string();
    const std::string expected =
        std::string("/usr/local/share") + sep + "/opt/pulp/presets";
    REQUIRE(serialized == expected);

    FileSearchPath round_trip(serialized);
    REQUIRE(round_trip == path);
}

TEST_CASE("FileSearchPath from_string drops empty segments and duplicates",
          "[runtime][file_search_path]") {
    const char sep = FileSearchPath::separator();
    // Build: "<sep>/a<sep><sep>/b<sep>/a<sep>"
    std::string s;
    s += sep;
    s += "/a";
    s += sep;
    s += sep;
    s += "/b";
    s += sep;
    s += "/a";  // duplicate
    s += sep;

    FileSearchPath path;
    path.from_string(s);
    REQUIRE(path.size() == 2);
    REQUIRE(path[0] == fs::path("/a"));
    REQUIRE(path[1] == fs::path("/b"));
}

TEST_CASE("FileSearchPath clear empties the list",
          "[runtime][file_search_path]") {
    FileSearchPath path;
    path.add("/a");
    path.add("/b");
    path.clear();
    REQUIRE(path.empty());
    REQUIRE(path.to_string().empty());
}
