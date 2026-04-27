// Shell-out CLI behaviour tests for `pulp ship`.
//
// Per CLAUDE.md the #295 lesson (silent empty Ed25519 signature) came
// from the CLI ship path never being exercised end-to-end. This file
// shells to the built binary for the non-destructive ship branches
// that are safe to run in CI without real signing material.

#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::platform;
namespace fs = std::filesystem;

namespace {

fs::path pulp_binary() {
    if (const char* env = std::getenv("PULP_CLI_PATH"); env && *env) {
        return fs::path(env);
    }
    return fs::current_path() / ".." / "tools" / "cli" / "pulp";
}

bool binary_exists() { return fs::exists(pulp_binary()); }

ProcessResult run_pulp_in(const fs::path& cwd,
                          const std::vector<std::string>& args,
                          int timeout_ms = 10000) {
    auto bin = pulp_binary();
    ProcessOptions opts;
    opts.timeout_ms = timeout_ms;
    opts.working_directory = cwd.string();
    return ChildProcess::run(bin.string(), args, opts);
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

fs::path make_fake_project(std::string_view name, bool with_build_cache) {
    auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    auto root = fs::temp_directory_path()
        / ("pulp-ship-" + std::string(name) + "-" + std::to_string(unique));
    fs::remove_all(root);
    fs::create_directories(root / "core");

    {
        std::ofstream cmake(root / "CMakeLists.txt");
        cmake << "cmake_minimum_required(VERSION 3.22)\n"
              << "project(FakeShipPlugin VERSION 2.3.4)\n";
    }

    if (with_build_cache) {
        fs::create_directories(root / "build");
        std::ofstream cache(root / "build" / "CMakeCache.txt");
        cache << "CMAKE_HOME_DIRECTORY:INTERNAL=" << root.string() << "\n";
    }

    return root;
}

}  // namespace

TEST_CASE("pulp ship outside a project directory errors out",
          "[cli][shellout][ship]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto r = run_pulp_in(fs::temp_directory_path(), {"ship"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    auto combined = r.stdout_output + r.stderr_output;
    // The handler early-exits with "not in a Pulp project directory"
    // — that wording is the contract hosts/users rely on.
    REQUIRE(contains(combined, "Pulp project"));
}

TEST_CASE("pulp ship sign outside a project directory errors cleanly",
          "[cli][shellout][ship]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto r = run_pulp_in(fs::temp_directory_path(),
                         {"ship", "sign", "--identity", "fake-id"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    // Must not fake-succeed: before #295 was closed, similar silent
    // paths let bad CLI args write empty artifacts.
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(contains(combined, "Pulp project"));
}

TEST_CASE("pulp ship appcast outside a project errors cleanly",
          "[cli][shellout][ship]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto r = run_pulp_in(fs::temp_directory_path(),
                         {"ship", "appcast"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
}

TEST_CASE("pulp ship notarize outside a project errors cleanly",
          "[cli][shellout][ship]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto r = run_pulp_in(fs::temp_directory_path(), {"ship", "notarize"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
}

TEST_CASE("pulp ship check outside a project errors cleanly",
          "[cli][shellout][ship]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto r = run_pulp_in(fs::temp_directory_path(), {"ship", "check"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
}

TEST_CASE("pulp ship package outside a project errors cleanly",
          "[cli][shellout][ship]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto r = run_pulp_in(fs::temp_directory_path(),
                         {"ship", "package", "--version", "1.0.0"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
}

TEST_CASE("pulp ship help (or default) enumerates every subcommand",
          "[cli][shellout][ship][help]") {
    // Run from the build tree — find_project_root() walks up to the
    // worktree root, and build/CMakeCache.txt exists since the test
    // harness itself was built. That puts us in the fallthrough help
    // branch regardless of whether we pass "help" or bogus args.
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto r = run_pulp_in(fs::current_path(), {"ship"});
    REQUIRE_FALSE(r.timed_out);

    // If the current-path walk doesn't find the project (some CI
    // layouts drop tests outside the tree), we accept the non-zero
    // branch provided the stderr mentions the project — that's the
    // same invariant the other cases assert.
    auto combined = r.stdout_output + r.stderr_output;
    if (r.exit_code == 0) {
        // Help branch reached — every shipping subcommand must be listed.
        for (const char* sub : {"sign", "notarize", "package", "appcast", "check"}) {
            INFO("ship help missing subcommand: " << sub);
            REQUIRE(contains(r.stdout_output, sub));
        }
    } else {
        REQUIRE(contains(combined, "Pulp project"));
    }
}

TEST_CASE("pulp ship inside project without build cache reports build guidance",
          "[cli][shellout][ship][issue-643]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("missing-build", false);

    auto r = run_pulp_in(root, {"ship", "check"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);

    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(contains(combined, "Build directory not found"));
    REQUIRE(contains(combined, "pulp build"));

    fs::remove_all(root);
}

TEST_CASE("pulp ship Android validation paths fail before external tooling",
          "[cli][shellout][ship][android][issue-643]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("android-validation", true);

    auto sign = run_pulp_in(root, {"ship", "sign", "--target", "android"});
    REQUIRE_FALSE(sign.timed_out);
    REQUIRE(sign.exit_code != 0);
    REQUIRE(contains(sign.stdout_output + sign.stderr_output, "No Android keystore"));

    auto check = run_pulp_in(root, {"ship", "check", "--target", "android"});
    REQUIRE_FALSE(check.timed_out);
    REQUIRE(check.exit_code != 0);
    REQUIRE(contains(check.stdout_output + check.stderr_output, "No artifacts/ directory"));

    auto conflicting = run_pulp_in(root,
        {"ship", "package", "--target", "android", "--apk-only", "--aab-only"});
    REQUIRE_FALSE(conflicting.timed_out);
    REQUIRE(conflicting.exit_code != 0);
    REQUIRE(contains(conflicting.stdout_output + conflicting.stderr_output,
                     "mutually exclusive"));

    auto missing_android = run_pulp_in(root, {"ship", "package", "--target", "android"});
    REQUIRE_FALSE(missing_android.timed_out);
    REQUIRE(missing_android.exit_code != 0);
    REQUIRE(contains(missing_android.stdout_output + missing_android.stderr_output,
                     "No android/ project found"));

    fs::remove_all(root);
}

TEST_CASE("pulp ship appcast writes local feed and rejects remote signing",
          "[cli][shellout][ship][appcast][issue-643]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("appcast", true);
    auto feed = root / "artifacts" / "updates.xml";

    auto write = run_pulp_in(root,
        {"ship", "appcast",
         "--url", "https://example.com/FakeShipPlugin-2.3.4.pkg",
         "--version", "2.3.4",
         "--notes", "coverage tranche",
         "--title", "Fake Ship Updates",
         "--output", feed.string()});
    REQUIRE_FALSE(write.timed_out);
    REQUIRE(write.exit_code == 0);
    REQUIRE(fs::exists(feed));

    std::ifstream in(feed);
    std::string xml((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    REQUIRE(contains(xml, "Fake Ship Updates"));
    REQUIRE(contains(xml, "2.3.4"));
    REQUIRE(contains(xml, "https://example.com/FakeShipPlugin-2.3.4.pkg"));

    auto remote_sign = run_pulp_in(root,
        {"ship", "appcast",
         "--url", "https://example.com/FakeShipPlugin-2.3.5.pkg",
         "--version", "2.3.5",
         "--sign-key", "not-a-real-key"});
    REQUIRE_FALSE(remote_sign.timed_out);
    REQUIRE(remote_sign.exit_code != 0);
    REQUIRE(contains(remote_sign.stdout_output + remote_sign.stderr_output,
                     "--sign-key requires a local file path"));

    fs::remove_all(root);
}
