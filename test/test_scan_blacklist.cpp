// Scan blacklist tests (workstream 03 slice 3.3a).

#include <catch2/catch_test_macros.hpp>
#include <pulp/host/scan_blacklist.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace pulp::host;
namespace fs = std::filesystem;

namespace {

struct TempFile {
    fs::path path;
    TempFile() {
        auto stem = "pulp-blacklist-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path = fs::temp_directory_path() / stem;
    }
    ~TempFile() { std::error_code ec; fs::remove_all(path, ec); }
    void write(const std::string& s) {
        std::ofstream(path, std::ios::binary) << s;
    }
};

} // namespace

TEST_CASE("unblacklisted path returns nullopt", "[host][blacklist]") {
    ScanBlacklist bl;
    REQUIRE_FALSE(bl.get("/not/blacklisted").has_value());
    REQUIRE_FALSE(bl.is_blacklisted("/not/blacklisted"));
}

TEST_CASE("blacklist + get round-trip", "[host][blacklist]") {
    TempFile f;
    f.write("bad plugin");
    ScanBlacklist bl;
    bl.blacklist(f.path.string(), "SIGSEGV in factory");
    REQUIRE(bl.is_blacklisted(f.path.string()));
    auto e = bl.get(f.path.string());
    REQUIRE(e.has_value());
    REQUIRE(e->reason == "SIGSEGV in factory");
}

TEST_CASE("rebuilt plugin is not blacklisted", "[host][blacklist]") {
    TempFile f;
    f.write("original");
    ScanBlacklist bl;
    bl.blacklist(f.path.string(), "crash");
    REQUIRE(bl.is_blacklisted(f.path.string()));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    f.write("rebuilt with very different content");
    REQUIRE_FALSE(bl.is_blacklisted(f.path.string()));
}

TEST_CASE("deleted plugin entry remains blacklisted", "[host][blacklist]") {
    TempFile f;
    f.write("original");
    ScanBlacklist bl;
    bl.blacklist(f.path.string(), "crash");
    REQUIRE(bl.is_blacklisted(f.path.string()));

    std::error_code ec;
    fs::remove(f.path, ec);
    REQUIRE_FALSE(ec);

    auto e = bl.get(f.path.string());
    REQUIRE(e.has_value());
    REQUIRE(e->reason == "crash");
}

TEST_CASE("clear unconditionally removes", "[host][blacklist]") {
    TempFile f;
    f.write("x");
    ScanBlacklist bl;
    bl.blacklist(f.path.string(), "r");
    bl.clear(f.path.string());
    REQUIRE_FALSE(bl.is_blacklisted(f.path.string()));
}

TEST_CASE("text round-trip handles pipes, newlines, and percents",
          "[host][blacklist]") {
    ScanBlacklist a;
    BlacklistEntry e;
    e.mtime = 12345;
    e.size = 678;
    e.reason = "crash at addr 0x42 | %ABORT%\nstack trace";
    // Bypass file-stamp lookup for this synthetic entry — direct map
    // access. blacklist() over a non-existent path would zero the stamp.
    // We need access to internal entries; use the friend-less public
    // blacklist() + manually rewrite mtime/size via round-trip.
    a.blacklist("/x|y/ok\npath", e.reason);
    auto text = a.to_text();
    ScanBlacklist b;
    REQUIRE(b.from_text(text));
    auto got = b.get("/x|y/ok\npath");
    REQUIRE(got.has_value());
    REQUIRE(got->reason == e.reason);
}

TEST_CASE("from_text skips malformed lines", "[host][blacklist]") {
    std::string mixed =
        "# comment line\n"
        "\n"
        "/valid/path|100|200|ok\n"
        "not-enough-fields\n"
        "/another|abc|200|bad-mtime\n"
        "/bad-size|100|not-a-size|bad-size\n"
        "/good|1|2|fine\n";
    ScanBlacklist bl;
    REQUIRE(bl.from_text(mixed));
    REQUIRE(bl.entries().count("/valid/path") == 1);
    REQUIRE(bl.entries().count("/good") == 1);
    REQUIRE(bl.entries().count("/another") == 0);
    REQUIRE(bl.entries().count("/bad-size") == 0);
    REQUIRE(bl.entries().count("not-enough-fields") == 0);
}

TEST_CASE("save_to + load_from via disk", "[host][blacklist]") {
    TempFile f;
    auto out_path = (f.path.parent_path() /
                     (f.path.filename().string() + ".txt")).string();
    ScanBlacklist a;
    a.blacklist("/tmp/nonexistent/plugin.vst3", "first");
    REQUIRE(a.save_to(out_path));
    ScanBlacklist b;
    REQUIRE(b.load_from(out_path));
    REQUIRE(b.size() == 1);
    std::error_code ec;
    fs::remove(out_path, ec);
}

TEST_CASE("load_from and save_to report unavailable paths",
          "[host][blacklist]") {
    TempFile f;
    ScanBlacklist bl;

    REQUIRE_FALSE(bl.load_from(f.path.string()));

    fs::create_directories(f.path);
    REQUIRE_FALSE(bl.save_to(f.path.string()));
}
