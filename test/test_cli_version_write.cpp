// test_cli_version_write.cpp — Unit tests for the `pulp version` file writer.
//
// Guards against the silent-success class of bug (cf. #290): a writer that
// opens an ofstream, streams content, and returns success WITHOUT checking
// the stream state, so a failed/partial write of an important persisted file
// (here CMakeLists.txt's project(VERSION) line and CHANGELOG.md) is reported
// as a successful bump. write_text_file_checked() is the shared primitive
// behind both write sites in tools/cli/cmd_version.cpp. It is an ATOMIC
// temp+rename writer (matching the core/state writers), so these tests pin
// both its failure contract AND the atomicity guarantee: a failed write
// leaves any existing destination unchanged and never leaves a temp orphan.
// Pure-logic, hermetic, no shellout — same isolation contract as
// test_cli_version_diag.cpp.

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

std::string read_all(const fs::path& p) {
    std::ifstream in(p);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The atomic writer stages a sibling "<path>.pulp-tmp"; assert it's cleaned up
// (renamed away on success, removed on failure) so no orphan is ever left.
bool temp_orphan_exists(const fs::path& target) {
    fs::path tmp = target;
    tmp += ".pulp-tmp";
    std::error_code ec;
    return fs::exists(tmp, ec);
}

}  // namespace

TEST_CASE("write_text_file_checked writes content and reports success",
          "[version-write][issue-290]") {
    TempDir dir;
    auto target = dir.path / "CMakeLists.txt";
    const std::string body = "project(Demo VERSION 1.2.3)\n";

    REQUIRE(write_text_file_checked(target, body));
    REQUIRE(fs::exists(target));
    REQUIRE(read_all(target) == body);
    REQUIRE_FALSE(temp_orphan_exists(target));  // temp renamed away, not orphaned
}

TEST_CASE("write_text_file_checked atomically replaces an existing file",
          "[version-write][issue-290]") {
    TempDir dir;
    auto target = dir.path / "CMakeLists.txt";
    REQUIRE(write_text_file_checked(target, "project(Demo VERSION 9.9.9)\n"));
    REQUIRE(write_text_file_checked(target, "project(Demo VERSION 1.0.0)\n"));

    REQUIRE(read_all(target) == "project(Demo VERSION 1.0.0)\n");
    REQUIRE_FALSE(temp_orphan_exists(target));
}

TEST_CASE("write_text_file_checked returns false when the parent is a file",
          "[version-write][issue-290]") {
    // A regular file standing where a directory component is expected makes the
    // staging temp ("<bad_target>.pulp-tmp") unopenable, so the write fails
    // before any rename. This is the reachable failure the old unchecked code
    // reported as a successful version bump.
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

TEST_CASE("write_text_file_checked fails atomically on an undeletable destination",
          "[version-write][issue-290]") {
    // Destination is an existing DIRECTORY: the staging temp opens fine, but the
    // final rename-over-a-directory fails. The writer must (a) return false,
    // (b) leave the original destination UNCHANGED (here: still a directory —
    // the atomicity guarantee, i.e. no truncation of the prior contents), and
    // (c) clean up its staging temp rather than orphaning it.
    TempDir dir;
    auto target = dir.path / "live-dir";
    fs::create_directories(target);
    {
        std::ofstream sentinel(target / "keep.txt");
        sentinel << "untouched";
    }

    REQUIRE_FALSE(write_text_file_checked(target, "project(Demo VERSION 1.2.3)\n"));

    REQUIRE(fs::is_directory(target));                 // original unchanged
    REQUIRE(read_all(target / "keep.txt") == "untouched");
    REQUIRE_FALSE(temp_orphan_exists(target));         // staging temp removed
}
