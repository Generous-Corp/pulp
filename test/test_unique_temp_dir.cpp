// make_unique_temp_dir hands every caller its own directory: across processes
// running at once, across threads, and across repeated calls.

#include "support/unique_temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using pulp::test::make_unique_temp_dir;

namespace {

struct ScratchParent {
    fs::path path = make_unique_temp_dir("pulp-unique-temp-dir-test");
    ~ScratchParent() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// The two numeric fields after the prefix: <prefix>-<pid>-<serial>.
bool split_name(const std::string& name, const std::string& prefix, std::uint64_t& pid,
                std::uint64_t& serial) {
    if (name.rfind(prefix + "-", 0) != 0)
        return false;
    std::istringstream rest(name.substr(prefix.size() + 1));
    char dash = 0;
    return static_cast<bool>(rest >> pid >> dash >> serial) && dash == '-' && rest.eof();
}

} // namespace

TEST_CASE("make_unique_temp_dir creates a new directory named for its process and call",
          "[test-support][temp-dir]") {
    ScratchParent parent;
    std::set<fs::path> seen;
    for (int i = 0; i < 20; ++i) {
        const auto dir = make_unique_temp_dir("same-prefix", parent.path);
        REQUIRE(fs::is_directory(dir));
        REQUIRE(fs::is_empty(dir));
        REQUIRE(seen.insert(dir).second);
        std::uint64_t pid = 0, serial = 0;
        REQUIRE(split_name(dir.filename().string(), "same-prefix", pid, serial));
        CHECK(pid == pulp::test::current_process_id());
    }
}

TEST_CASE("make_unique_temp_dir never hands out a directory that already exists",
          "[test-support][temp-dir]") {
    ScratchParent parent;
    // A leftover from an earlier run with the same pid occupies the next names.
    const auto pid = pulp::test::current_process_id();
    for (std::uint64_t serial = 0; serial < 64; ++serial) {
        fs::create_directory(parent.path /
                             pulp::test::unique_temp_dir_name("leftover", pid, serial));
    }
    const auto before = std::distance(fs::directory_iterator(parent.path), {});
    const auto dir = make_unique_temp_dir("leftover", parent.path);
    REQUIRE(std::distance(fs::directory_iterator(parent.path), {}) == before + 1);
    REQUIRE(fs::is_empty(dir));
}

TEST_CASE("make_unique_temp_dir is unique across threads", "[test-support][temp-dir]") {
    ScratchParent parent;
    constexpr int threads = 8, calls = 50;
    std::vector<std::vector<fs::path>> made(threads);
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < calls; ++i)
                made[t].push_back(make_unique_temp_dir("thread", parent.path));
        });
    }
    for (auto& worker : workers)
        worker.join();
    std::set<fs::path> all;
    for (const auto& list : made)
        all.insert(list.begin(), list.end());
    REQUIRE(all.size() == static_cast<std::size_t>(threads * calls));
}

#ifdef PULP_UNIQUE_TEMP_DIR_FIXTURE
TEST_CASE("make_unique_temp_dir is unique across processes running at once",
          "[test-support][temp-dir]") {
    ScratchParent parent;
    constexpr int processes = 8, calls = 50;
    std::vector<pulp::platform::ChildProcess> children(processes);
    pulp::platform::ProcessOptions options;
    options.capture_stdout = true;
    options.capture_stderr = true;
    options.timeout_ms = 60'000;
    for (auto& child : children) {
        REQUIRE(child.start(PULP_UNIQUE_TEMP_DIR_FIXTURE,
                            {parent.path.string(), std::to_string(calls), "proc"}, options));
    }
    std::set<fs::path> all;
    std::set<std::uint64_t> pids;
    for (auto& child : children) {
        const auto result = child.wait();
        INFO(result.stderr_output);
        REQUIRE(result.exit_code == 0);
        std::istringstream lines(result.stdout_output);
        std::string word;
        std::uint64_t child_pid = 0;
        REQUIRE(static_cast<bool>(lines >> word >> child_pid));
        REQUIRE(word == "pid");
        pids.insert(child_pid);
        std::string line;
        std::getline(lines, line);
        int count = 0;
        while (std::getline(lines, line)) {
            if (line.empty())
                continue;
            const fs::path dir = line;
            ++count;
            REQUIRE(fs::is_directory(dir));
            REQUIRE(all.insert(dir).second);
            // Each child creates only names carrying its own pid, so no two
            // processes ever race for the same name.
            std::uint64_t pid = 0, serial = 0;
            REQUIRE(split_name(dir.filename().string(), "proc", pid, serial));
            REQUIRE(pid == child_pid);
        }
        REQUIRE(count == calls);
    }
    REQUIRE(pids.size() == static_cast<std::size_t>(processes));
    REQUIRE(all.size() == static_cast<std::size_t>(processes * calls));
}
#endif
