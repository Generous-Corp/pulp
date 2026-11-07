#include <catch2/catch_test_macros.hpp>

#include "support/unique_temp_dir.hpp"

#include <pulp/platform/special_dir.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/log_sink.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace pulp::runtime;
namespace fs = std::filesystem;

namespace {

// A unique scratch directory removed on scope exit.
struct TempDir {
    fs::path path;
    TempDir() {
        path = pulp::test::make_unique_temp_dir("pulp-log-sink");
#if !defined(_WIN32)
        ::chmod(path.c_str(), 0700); // a private parent so the sink's own checks decide
#endif
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::size_t count_lines(const std::string& text) {
    return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
}

#if !defined(_WIN32)
mode_t mode_of(const fs::path& p) {
    struct stat st{};
    REQUIRE(::stat(p.c_str(), &st) == 0);
    return st.st_mode & 07777;
}
#endif

FileLogSinkOptions options_in(const fs::path& dir) {
    FileLogSinkOptions o;
    o.directory = dir;
    o.min_level = LogLevel::Debug;
    return o;
}

} // namespace

// ── set_log_sink ─────────────────────────────────────────────────────────────

TEST_CASE("a sink receives level, category and message", "[log_sink]") {
    CaptureLogSink capture;
    ScopedLogSink scope(capture);

    log_info("hello {}", 42);
    log_warn("careful");
    log_error("broken");
    log_with_category("audio", LogLevel::Info, "underrun {}", 7);

    const auto records = capture.records();
    REQUIRE(records.size() == 4);
    CHECK(records[0].level == LogLevel::Info);
    CHECK(records[0].category == "pulp");
    CHECK(records[0].message == "hello 42");
    CHECK(records[1].level == LogLevel::Warning);
    CHECK(records[2].level == LogLevel::Error);
    CHECK(records[3].category == "audio");
    CHECK(records[3].message == "underrun 7");
}

TEST_CASE("set_log_sink returns the previous sink and an empty sink removes it", "[log_sink]") {
    set_log_sink({});
    int a = 0;
    int b = 0;
    auto prev = set_log_sink([&](LogLevel, std::string_view, std::string_view) { ++a; });
    CHECK_FALSE(static_cast<bool>(prev));
    log_info("one");
    prev = set_log_sink([&](LogLevel, std::string_view, std::string_view) { ++b; });
    CHECK(static_cast<bool>(prev));
    log_info("two");
    set_log_sink({});
    log_info("three");
    CHECK(a == 1);
    CHECK(b == 1);
}

TEST_CASE("ScopedLogSink restores the previous sink", "[log_sink]") {
    CaptureLogSink outer;
    CaptureLogSink inner;
    ScopedLogSink outer_scope(outer);
    {
        ScopedLogSink inner_scope(inner);
        log_info("inside");
    }
    log_info("after");
    CHECK(inner.size() == 1);
    CHECK(outer.size() == 1);
    CHECK(outer.records()[0].message == "after");
}

TEST_CASE("a throwing sink does not reach the caller", "[log_sink]") {
    ScopedLogSink scope([](LogLevel, std::string_view, std::string_view) {
        throw std::runtime_error("sink failure");
    });
    REQUIRE_NOTHROW(log_info("still fine"));
}

TEST_CASE("a sink that logs does not recurse into itself", "[log_sink]") {
    int calls = 0;
    ScopedLogSink scope([&](LogLevel, std::string_view, std::string_view) {
        ++calls;
        log_info("from inside the sink");
    });
    log_info("outer");
    CHECK(calls == 1);
}

TEST_CASE("swapping sinks while threads log never loses or duplicates a record", "[log_sink]") {
    CaptureLogSink a(100000);
    CaptureLogSink b(100000);
    ScopedLogSink scope(a);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 50;
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            while (!go)
                std::this_thread::yield();
            for (int i = 0; i < kPerThread; ++i)
                log_debug("tick");
        });
    }
    go = true;
    for (int i = 0; i < 200; ++i)
        set_log_sink(i % 2 ? b.sink() : a.sink());
    for (auto& t : threads)
        t.join();

    // log_debug is compiled out in release builds; account for either mode.
    const std::size_t total = a.size() + b.size();
    CHECK((total == 0 || total == static_cast<std::size_t>(kThreads * kPerThread)));
}

// ── CaptureLogSink ───────────────────────────────────────────────────────────

TEST_CASE("CaptureLogSink bounds memory and counts what it discards", "[log_sink]") {
    CaptureLogSink capture(3);
    for (int i = 0; i < 5; ++i)
        capture.write(LogLevel::Info, "t", "m" + std::to_string(i));
    const auto records = capture.records();
    REQUIRE(records.size() == 3);
    CHECK(records.front().message == "m2");
    CHECK(records.back().message == "m4");
    CHECK(capture.dropped_count() == 2);
}

TEST_CASE("CaptureLogSink filters, searches, takes and clears", "[log_sink]") {
    CaptureLogSink capture;
    capture.write(LogLevel::Debug, "a", "chatter");
    capture.write(LogLevel::Warning, "a", "disk nearly full");
    capture.write(LogLevel::Error, "b", "disk gone");

    CHECK(capture.contains("nearly"));
    CHECK_FALSE(capture.contains("absent"));
    CHECK(capture.records_at_least(LogLevel::Warning).size() == 2);

    const auto taken = capture.take();
    CHECK(taken.size() == 3);
    CHECK(capture.size() == 0);

    capture.write(LogLevel::Info, "a", "x");
    capture.clear();
    CHECK(capture.size() == 0);
}

TEST_CASE("copies of a CaptureLogSink share one record list", "[log_sink]") {
    CaptureLogSink capture;
    CaptureLogSink copy = capture;
    copy.write(LogLevel::Info, "a", "via copy");
    CHECK(capture.size() == 1);
}

// ── FileLogSink ──────────────────────────────────────────────────────────────

TEST_CASE("FileLogSink writes one well-formed line per record", "[log_sink][file]") {
    TempDir tmp;
    std::string error;
    auto sink = FileLogSink::open(options_in(tmp.path / "logs"), &error);
    REQUIRE(sink);
    INFO(error);

    sink->write(LogLevel::Info, "net", "connected");
    sink->write(LogLevel::Error, "", "no category");

    const std::string text = slurp(sink->path());
    CHECK(count_lines(text) == 2);
    const std::regex line(R"(^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z \[info\] net: connected\n)"
                          R"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z \[error\] no category\n$)");
    CHECK(std::regex_match(text, line));
}

TEST_CASE("FileLogSink escapes line breaks and control characters", "[log_sink][file]") {
    TempDir tmp;
    auto sink = FileLogSink::open(options_in(tmp.path / "logs"));
    REQUIRE(sink);
    sink->write(LogLevel::Info, "c\nat", "line one\nFAKE [error] forged\r\x1b[31m");
    const std::string text = slurp(sink->path());
    CHECK(count_lines(text) == 1);
    CHECK(text.find("line one\\nFAKE [error] forged\\r\\x1b[31m") != std::string::npos);
    CHECK(text.find("c\\nat:") != std::string::npos);
}

TEST_CASE("FileLogSink truncates oversized records", "[log_sink][file]") {
    TempDir tmp;
    auto sink = FileLogSink::open(options_in(tmp.path / "logs"));
    REQUIRE(sink);
    sink->write(LogLevel::Info, "big", std::string(100000, 'x'));
    const std::string text = slurp(sink->path());
    CHECK(count_lines(text) == 1);
    CHECK(text.size() <= 8 * 1024 + 1);
}

TEST_CASE("FileLogSink honours the minimum level", "[log_sink][file]") {
    TempDir tmp;
    auto opts = options_in(tmp.path / "logs");
    opts.min_level = LogLevel::Warning;
    auto sink = FileLogSink::open(opts);
    REQUIRE(sink);
    sink->write(LogLevel::Info, "a", "skipped");
    sink->write(LogLevel::Warning, "a", "kept");
    const std::string text = slurp(sink->path());
    CHECK(text.find("skipped") == std::string::npos);
    CHECK(text.find("kept") != std::string::npos);
}

TEST_CASE("FileLogSink rotates by size and keeps a bounded set of files", "[log_sink][file]") {
    TempDir tmp;
    auto opts = options_in(tmp.path / "logs");
    opts.max_file_bytes = 300;
    opts.max_files = 3;
    auto sink = FileLogSink::open(opts);
    REQUIRE(sink);

    for (int i = 0; i < 40; ++i)
        sink->write(LogLevel::Info, "rot", "record number " + std::to_string(1000 + i));

    CHECK(fs::exists(sink->path()));
    CHECK(fs::exists(sink->rotated_path(1)));
    CHECK(fs::exists(sink->rotated_path(2)));
    CHECK_FALSE(fs::exists(sink->rotated_path(3)));

    for (const auto& p : {sink->path(), sink->rotated_path(1), sink->rotated_path(2)})
        CHECK(fs::file_size(p) <= 300);

    // Newest records are in the live file; older ones moved down the chain.
    CHECK(slurp(sink->path()).find("1039") != std::string::npos);
    const std::string newest_rotated = slurp(sink->rotated_path(1));
    const std::string oldest_rotated = slurp(sink->rotated_path(2));
    CHECK(newest_rotated.find("1039") == std::string::npos);
    CHECK(oldest_rotated.find("1000") == std::string::npos); // aged out entirely
    CHECK(sink->dropped_count() == 0);
}

TEST_CASE("FileLogSink with one file discards on rotation", "[log_sink][file]") {
    TempDir tmp;
    auto opts = options_in(tmp.path / "logs");
    opts.max_file_bytes = 200;
    opts.max_files = 1;
    auto sink = FileLogSink::open(opts);
    REQUIRE(sink);
    for (int i = 0; i < 20; ++i)
        sink->write(LogLevel::Info, "x", "padding padding " + std::to_string(i));
    CHECK(fs::file_size(sink->path()) <= 200);
    CHECK_FALSE(fs::exists(sink->rotated_path(1)));
}

TEST_CASE("FileLogSink appends to an existing log and counts its size toward rotation",
          "[log_sink][file]") {
    TempDir tmp;
    auto opts = options_in(tmp.path / "logs");
    opts.max_file_bytes = 400;
    {
        auto first = FileLogSink::open(opts);
        REQUIRE(first);
        first->write(LogLevel::Info, "a", std::string(150, 'a'));
    }
    auto second = FileLogSink::open(opts);
    REQUIRE(second);
    second->write(LogLevel::Info, "a", std::string(150, 'b'));
    second->write(LogLevel::Info, "a", std::string(150, 'c')); // pushes past 400 bytes
    CHECK(fs::exists(second->rotated_path(1)));
    CHECK(slurp(second->rotated_path(1)).find('a') != std::string::npos);
}

TEST_CASE("FileLogSink installs as the process log sink", "[log_sink][file]") {
    TempDir tmp;
    auto sink = FileLogSink::open(options_in(tmp.path / "logs"));
    REQUIRE(sink);
    {
        ScopedLogSink scope(*sink);
        log_info("through the logger {}", 1);
        log_with_category("db", LogLevel::Warning, "slow query");
    }
    const std::string text = slurp(sink->path());
    CHECK(text.find("pulp: through the logger 1") != std::string::npos);
    CHECK(text.find("[warn] db: slow query") != std::string::npos);
}

TEST_CASE("FileLogSink is safe under concurrent writers", "[log_sink][file]") {
    TempDir tmp;
    auto opts = options_in(tmp.path / "logs");
    opts.max_file_bytes = 100000;
    auto sink = FileLogSink::open(opts);
    REQUIRE(sink);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < 100; ++i)
                sink->write(LogLevel::Info, "t" + std::to_string(t), "n" + std::to_string(i));
        });
    for (auto& t : threads)
        t.join();
    CHECK(count_lines(slurp(sink->path())) == 400);
}

TEST_CASE("FileLogSink resolves its directory from the platform logs folder", "[log_sink][file]") {
    TempDir tmp;
    pulp::platform::set_special_dir_override(pulp::platform::SpecialDir::logs, tmp.path);
    FileLogSinkOptions opts;
    opts.app_name = "demo-app";
    std::string error;
    auto sink = FileLogSink::open(opts, &error);
    pulp::platform::clear_special_dir_overrides();
    REQUIRE(sink);
    INFO(error);
    CHECK(sink->directory() == tmp.path / "demo-app");
    CHECK(fs::is_regular_file(sink->path()));
}

TEST_CASE("FileLogSink rejects names that could escape the log directory", "[log_sink][file]") {
    TempDir tmp;
    std::string error;

    auto bad_file = options_in(tmp.path / "logs");
    bad_file.file_name = "../escape.log";
    CHECK_FALSE(FileLogSink::open(bad_file, &error));
    CHECK_FALSE(error.empty());

    FileLogSinkOptions bad_app;
    bad_app.app_name = "../escape";
    pulp::platform::set_special_dir_override(pulp::platform::SpecialDir::logs, tmp.path);
    CHECK_FALSE(FileLogSink::open(bad_app, &error));
    pulp::platform::clear_special_dir_overrides();
    CHECK_FALSE(fs::exists(tmp.path.parent_path() / "escape"));

    for (const char* name : {"", ".", "..", "a/b", "a\\b"}) {
        auto o = options_in(tmp.path / "logs");
        o.file_name = name;
        CHECK_FALSE(FileLogSink::open(o));
    }
}

#if !defined(_WIN32)

TEST_CASE("FileLogSink creates its directory and file owner-only", "[log_sink][file][posix]") {
    TempDir tmp;
    auto sink = FileLogSink::open(options_in(tmp.path / "logs"));
    REQUIRE(sink);
    sink->write(LogLevel::Info, "a", "b");
    CHECK((mode_of(sink->directory()) & 077) == 0);
    CHECK(mode_of(sink->path()) == 0600);
}

TEST_CASE("FileLogSink tightens a readable log it owns", "[log_sink][file][posix]") {
    TempDir tmp;
    const fs::path dir = tmp.path / "logs";
    fs::create_directories(dir);
    ::chmod(dir.c_str(), 0700);
    {
        std::ofstream(dir / "pulp.log") << "old line\n";
    }
    ::chmod((dir / "pulp.log").c_str(), 0644);

    auto sink = FileLogSink::open(options_in(dir));
    REQUIRE(sink);
    CHECK(mode_of(dir / "pulp.log") == 0600);
    sink->write(LogLevel::Info, "a", "new");
    const std::string text = slurp(dir / "pulp.log");
    CHECK(text.rfind("old line\n", 0) == 0); // existing content preserved
}

TEST_CASE("FileLogSink refuses a symlinked log directory", "[log_sink][file][posix]") {
    TempDir tmp;
    const fs::path real = tmp.path / "real";
    fs::create_directories(real);
    ::chmod(real.c_str(), 0700);
    const fs::path link = tmp.path / "link";
    fs::create_directory_symlink(real, link);

    std::string error;
    CHECK_FALSE(FileLogSink::open(options_in(link), &error));
    CHECK(error.find("symlink") != std::string::npos);
    CHECK(fs::is_empty(real));
}

TEST_CASE("FileLogSink refuses a symlinked log directory named with a trailing slash",
          "[log_sink][file][posix]") {
    TempDir tmp;
    const fs::path real = tmp.path / "real";
    fs::create_directories(real);
    ::chmod(real.c_str(), 0700);
    const fs::path link = tmp.path / "link";
    fs::create_directory_symlink(real, link);

    std::string error;
    CHECK_FALSE(FileLogSink::open(options_in(link / ""), &error));
    CHECK(fs::is_empty(real));
}

TEST_CASE("FileLogSink refuses a world- or group-writable log directory",
          "[log_sink][file][posix]") {
    TempDir tmp;
    const fs::path dir = tmp.path / "logs";
    fs::create_directories(dir);

    ::chmod(dir.c_str(), 0777);
    std::string error;
    CHECK_FALSE(FileLogSink::open(options_in(dir), &error));
    CHECK(error.find("writable") != std::string::npos);

    ::chmod(dir.c_str(), 0770);
    CHECK_FALSE(FileLogSink::open(options_in(dir), &error));

    ::chmod(dir.c_str(), 0700);
    CHECK(FileLogSink::open(options_in(dir)));
}

TEST_CASE("FileLogSink refuses a symlinked log file and never writes through it",
          "[log_sink][file][posix]") {
    TempDir tmp;
    const fs::path dir = tmp.path / "logs";
    fs::create_directories(dir);
    ::chmod(dir.c_str(), 0700);
    const fs::path victim = tmp.path / "victim.txt";
    {
        std::ofstream(victim) << "precious\n";
    }
    fs::create_symlink(victim, dir / "pulp.log");

    std::string error;
    CHECK_FALSE(FileLogSink::open(options_in(dir), &error));
    CHECK(error.find("symlink") != std::string::npos);
    CHECK(slurp(victim) == "precious\n");
}

TEST_CASE("FileLogSink refuses a world-writable or hard-linked log file",
          "[log_sink][file][posix]") {
    TempDir tmp;
    const fs::path dir = tmp.path / "logs";
    fs::create_directories(dir);
    ::chmod(dir.c_str(), 0700);
    const fs::path file = dir / "pulp.log";
    {
        std::ofstream(file) << "x\n";
    }

    ::chmod(file.c_str(), 0666);
    std::string error;
    CHECK_FALSE(FileLogSink::open(options_in(dir), &error));
    CHECK(error.find("writable") != std::string::npos);

    ::chmod(file.c_str(), 0600);
    fs::create_hard_link(file, tmp.path / "alias");
    CHECK_FALSE(FileLogSink::open(options_in(dir), &error));
    CHECK(error.find("hard links") != std::string::npos);
}

TEST_CASE("FileLogSink keeps writing to its directory after the path is swapped for a link",
          "[log_sink][file][posix]") {
    TempDir tmp;
    const fs::path dir = tmp.path / "logs";
    auto sink = FileLogSink::open(options_in(dir));
    REQUIRE(sink);
    sink->write(LogLevel::Info, "a", "before");

    const fs::path moved = tmp.path / "moved";
    fs::rename(dir, moved);
    const fs::path elsewhere = tmp.path / "elsewhere";
    fs::create_directories(elsewhere);
    fs::create_directory_symlink(elsewhere, dir);

    sink->write(LogLevel::Info, "a", "after");
    CHECK(fs::is_empty(elsewhere));
    CHECK(slurp(moved / "pulp.log").find("after") != std::string::npos);
}

#endif
