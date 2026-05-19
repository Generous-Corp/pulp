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

TEST_CASE("from_text accepts empty/comment-only files and clears previous entries",
          "[host][blacklist][issue-493]") {
    ScanBlacklist bl;
    REQUIRE(bl.from_text("/old|1|2|stale\n"));
    REQUIRE(bl.size() == 1);

    REQUIRE(bl.from_text("# only comments\n\n   \n"));
    REQUIRE(bl.size() == 0);
    REQUIRE_FALSE(bl.is_blacklisted("/old"));
}

TEST_CASE("from_text preserves reasons containing delimiter-like escaped data",
          "[host][blacklist][issue-493]") {
    ScanBlacklist bl;
    REQUIRE(bl.from_text("/plugin.vst3|10|20|first%7Csecond%25third\n"));

    auto entry = bl.entries().find("/plugin.vst3");
    REQUIRE(entry != bl.entries().end());
    REQUIRE(entry->second.mtime == 10);
    REQUIRE(entry->second.size == 20);
    REQUIRE(entry->second.reason == "first|second%third");

    const auto text = bl.to_text();
    REQUIRE(text.find("first%7Csecond%25third") != std::string::npos);
}

TEST_CASE("from_text tolerates signed stamp values from old blacklist files",
          "[host][blacklist][issue-493]") {
    ScanBlacklist bl;
    REQUIRE(bl.from_text("/plugin.vst3|-1|-2|old stamp\n"));

    auto entry = bl.entries().find("/plugin.vst3");
    REQUIRE(entry != bl.entries().end());
    REQUIRE(entry->second.mtime == -1);
    REQUIRE(entry->second.size == -2);
    REQUIRE(entry->second.reason == "old stamp");
}

TEST_CASE("from_text rejects partially parsed numeric stamp fields",
          "[host][blacklist][codecov]") {
    ScanBlacklist bl;
    REQUIRE(bl.from_text(
        "/mtime-junk.vst3|10junk|20|bad mtime\n"
        "/size-junk.vst3|10|20bytes|bad size\n"
        "/spaced.vst3| 10 | 20 \t|ok\n"));

    REQUIRE(bl.entries().count("/mtime-junk.vst3") == 0);
    REQUIRE(bl.entries().count("/size-junk.vst3") == 0);

    auto entry = bl.entries().find("/spaced.vst3");
    REQUIRE(entry != bl.entries().end());
    REQUIRE(entry->second.mtime == 10);
    REQUIRE(entry->second.size == 20);
    REQUIRE(entry->second.reason == "ok");
}

TEST_CASE("blacklisting a missing plugin records a durable manual block",
          "[host][blacklist][codecov]") {
    TempFile f;
    ScanBlacklist bl;

    REQUIRE_FALSE(fs::exists(f.path));
    bl.blacklist(f.path.string(), "manual quarantine");

    auto entry = bl.get(f.path.string());
    REQUIRE(entry.has_value());
    REQUIRE(entry->mtime == 0);
    REQUIRE(entry->size == 0);
    REQUIRE(entry->reason == "manual quarantine");
    REQUIRE(bl.is_blacklisted(f.path.string()));

    f.write("new plugin build");
    REQUIRE_FALSE(bl.is_blacklisted(f.path.string()));
}

TEST_CASE("from_text duplicate blacklist records keep the last valid entry",
          "[host][blacklist][codecov]") {
    ScanBlacklist bl;
    REQUIRE(bl.from_text(
        "/plugin.vst3|1|2|first reason\n"
        "/other.vst3|3|4|other reason\n"
        "/plugin.vst3|5|6|second reason\n"));

    REQUIRE(bl.size() == 2);
    auto entry = bl.entries().find("/plugin.vst3");
    REQUIRE(entry != bl.entries().end());
    REQUIRE(entry->second.mtime == 5);
    REQUIRE(entry->second.size == 6);
    REQUIRE(entry->second.reason == "second reason");
}

TEST_CASE("from_text keeps unknown percent escapes literal",
          "[host][blacklist][codecov]") {
    ScanBlacklist bl;
    REQUIRE(bl.from_text("/plugin%2Fname.vst3|7|8|bad%2Greason%7Cok\n"));

    auto entry = bl.entries().find("/plugin%2Fname.vst3");
    REQUIRE(entry != bl.entries().end());
    REQUIRE(entry->second.reason == "bad%2Greason|ok");

    const auto text = bl.to_text();
    REQUIRE(text.find("/plugin%252Fname.vst3") != std::string::npos);
    REQUIRE(text.find("bad%252Greason%7Cok") != std::string::npos);
}

TEST_CASE("save_to creates nested blacklist parent directories",
          "[host][blacklist][codecov]") {
    TempFile f;
    const auto root = f.path.parent_path() / (f.path.filename().string() + "-dir");
    const auto out_path = root / "nested" / "blacklist.txt";

    ScanBlacklist a;
    a.blacklist("/tmp/missing-plugin.clap", "timeout");
    REQUIRE(a.save_to(out_path.string()));
    REQUIRE(fs::exists(out_path));

    ScanBlacklist b;
    REQUIRE(b.load_from(out_path.string()));
    REQUIRE(b.size() == 1);
    REQUIRE(b.entries().at("/tmp/missing-plugin.clap").reason == "timeout");

    std::error_code ec;
    fs::remove_all(root, ec);
}
