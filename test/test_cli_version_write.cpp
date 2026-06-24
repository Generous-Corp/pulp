// test_cli_version_write.cpp — Unit tests for the `pulp version` file writer.
//
// Guards against the silent-success class of bug (cf. #290): a writer that
// opens an ofstream, streams content, and returns success WITHOUT checking
// the stream state, so a failed/partial write of an important persisted file
// (here CMakeLists.txt's project(VERSION) line and CHANGELOG.md) is reported
// as a successful bump. write_text_file_checked() is the shared primitive
// behind both write sites in tools/cli/cmd_version.cpp; these tests pin its
// failure contract. Pure-logic, hermetic, no shellout — same isolation
// contract as test_cli_version_diag.cpp.

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/cmd_version_internal.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using pulp::cli::version_internal::write_text_file_checked;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        auto base = fs::temp_directory_path();
        static std::atomic<int> seq{0};
        int n = seq.fetch_add(1);
        path = base / ("pulp-version-write-test-" +
                       std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                       "-" + std::to_string(n));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

}  // namespace

TEST_CASE("write_text_file_checked writes content and reports success",
          "[version-write][issue-290]") {
    TempDir dir;
    auto target = dir.path / "CMakeLists.txt";
    const std::string body = "project(Demo VERSION 1.2.3)\n";

    REQUIRE(write_text_file_checked(target, body));
    REQUIRE(fs::exists(target));

    std::ifstream in(target);
    std::string round{std::istreambuf_iterator<char>(in),
                      std::istreambuf_iterator<char>()};
    REQUIRE(round == body);
}

TEST_CASE("write_text_file_checked truncates an existing file",
          "[version-write][issue-290]") {
    TempDir dir;
    auto target = dir.path / "CMakeLists.txt";
    REQUIRE(write_text_file_checked(target, "project(Demo VERSION 9.9.9)\n"));
    REQUIRE(write_text_file_checked(target, "project(Demo VERSION 1.0.0)\n"));

    std::ifstream in(target);
    std::string round{std::istreambuf_iterator<char>(in),
                      std::istreambuf_iterator<char>()};
    REQUIRE(round == "project(Demo VERSION 1.0.0)\n");
}

TEST_CASE("write_text_file_checked returns false when the parent is a file",
          "[version-write][issue-290]") {
    // A regular file standing where a directory component is expected makes
    // the destination path unopenable. This is the reachable failure the old
    // code reported as a successful version bump.
    TempDir dir;
    auto blocker = dir.path / "not-a-dir";
    {
        std::ofstream f(blocker);
        f << "x";
    }
    REQUIRE(fs::is_regular_file(blocker));

    auto bad_target = blocker / "CMakeLists.txt";  // parent is a file, not a dir
    REQUIRE_FALSE(write_text_file_checked(bad_target, "project(Demo VERSION 1.2.3)\n"));
}

TEST_CASE("write_text_file_checked returns false for a directory destination",
          "[version-write][issue-290]") {
    // Opening a directory for writing fails to open the stream; the writer
    // must surface that as false rather than silently succeeding.
    TempDir dir;
    REQUIRE(fs::is_directory(dir.path));
    REQUIRE_FALSE(write_text_file_checked(dir.path, "project(Demo VERSION 1.2.3)\n"));
}
