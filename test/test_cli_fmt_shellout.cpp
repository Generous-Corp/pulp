// Shell-out coverage for `pulp fmt`.

#include "test_cli_shellout_helpers.hpp"

using namespace pulp::platform;
namespace fs = std::filesystem;
using namespace pulp_test_cli;

namespace {

fs::path write_fmt_project_fixture(const std::string& prefix,
                                   bool include_clang_format = true) {
    auto root = unique_temp_dir(prefix);
    write_text(root / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.24)\n"
               "project(FmtFixture)\n");
    fs::create_directories(root / "core");
    if (include_clang_format) {
        write_text(root / ".clang-format", "BasedOnStyle: LLVM\n");
    }
    return root;
}

void write_fake_clang_format(const fs::path& bin_dir) {
    fs::create_directories(bin_dir);
#if defined(_WIN32)
    write_text(bin_dir / "clang-format.bat",
               "@echo off\r\n"
               ">>\"%PULP_FMT_FAKE_LOG%\" echo %*\r\n"
               "if not \"%PULP_FMT_FAKE_EXIT%\"==\"\" exit /b %PULP_FMT_FAKE_EXIT%\r\n"
               "exit /b 0\r\n");
#else
    auto script = bin_dir / "clang-format";
    write_text(script,
               "#!/bin/sh\n"
               "printf '%s\\n' \"$*\" >> \"$PULP_FMT_FAKE_LOG\"\n"
               "exit \"${PULP_FMT_FAKE_EXIT:-0}\"\n");
    fs::permissions(script,
                    fs::perms::owner_exec | fs::perms::owner_read |
                        fs::perms::owner_write,
                    fs::perm_options::add);
#endif
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("pulp fmt requires a project .clang-format",
          "[cli][shellout][fmt]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto project = write_fmt_project_fixture("pulp-fmt-missing-config", false);
    auto r = run_pulp_in_directory(project, {"fmt"});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    REQUIRE(contains(r.stderr_output, "pulp fmt: no .clang-format"));
}

TEST_CASE("pulp fmt reports an empty project without invoking clang-format",
          "[cli][shellout][fmt]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto project = write_fmt_project_fixture("pulp-fmt-empty");
    auto r = run_pulp_in_directory(project, {"fmt"});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(contains(r.stdout_output, "pulp fmt: nothing to format."));
}

TEST_CASE("pulp fmt --check invokes clang-format in dry-run mode",
          "[cli][shellout][fmt]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto project = write_fmt_project_fixture("pulp-fmt-check");
    write_text(project / "core" / "demo.cpp", "int main(){return 0;}\n");

    auto fake_bin = project / "fake-bin";
    auto fake_log = project / "clang-format.log";
    write_fake_clang_format(fake_bin);

    ScopedEnvVar path_guard("PATH");
    ScopedEnvVar log_guard("PULP_FMT_FAKE_LOG");
    ScopedEnvVar exit_guard("PULP_FMT_FAKE_EXIT");
    prepend_to_path(fake_bin);
    log_guard.set(fake_log.string());

    auto r = run_pulp_in_directory(project, {"fmt", "--check", "core/demo.cpp"});
    auto log = read_file(fake_log);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(contains(r.stdout_output, "pulp fmt: 1 files (dry run)"));
    REQUIRE(contains(log, "-style=file"));
    REQUIRE(contains(log, "--dry-run"));
    REQUIRE(contains(log, "--Werror"));
    REQUIRE(contains(log, "demo.cpp"));
    REQUIRE_FALSE(contains(log, " -i "));
}

TEST_CASE("pulp fmt rewrites explicit source paths with clang-format -i",
          "[cli][shellout][fmt]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto project = write_fmt_project_fixture("pulp-fmt-rewrite");
    write_text(project / "core" / "demo.cpp", "int main(){return 0;}\n");

    auto fake_bin = project / "fake-bin";
    auto fake_log = project / "clang-format.log";
    write_fake_clang_format(fake_bin);

    ScopedEnvVar path_guard("PATH");
    ScopedEnvVar log_guard("PULP_FMT_FAKE_LOG");
    ScopedEnvVar exit_guard("PULP_FMT_FAKE_EXIT");
    prepend_to_path(fake_bin);
    log_guard.set(fake_log.string());

    auto r = run_pulp_in_directory(project, {"fmt", "core/demo.cpp"});
    auto log = read_file(fake_log);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(contains(r.stdout_output, "pulp fmt: 1 files"));
    REQUIRE_FALSE(contains(r.stdout_output, "dry run"));
    REQUIRE(contains(log, "-style=file"));
    REQUIRE(contains(log, " -i "));
    REQUIRE(contains(log, "demo.cpp"));
    REQUIRE_FALSE(contains(log, "--dry-run"));
}

TEST_CASE("pulp fmt --check propagates clang-format failures",
          "[cli][shellout][fmt]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto project = write_fmt_project_fixture("pulp-fmt-check-fails");
    write_text(project / "core" / "demo.cpp", "int main(){return 0;}\n");

    auto fake_bin = project / "fake-bin";
    auto fake_log = project / "clang-format.log";
    write_fake_clang_format(fake_bin);

    ScopedEnvVar path_guard("PATH");
    ScopedEnvVar log_guard("PULP_FMT_FAKE_LOG");
    ScopedEnvVar exit_guard("PULP_FMT_FAKE_EXIT");
    prepend_to_path(fake_bin);
    log_guard.set(fake_log.string());
    exit_guard.set("1");

    auto r = run_pulp_in_directory(project, {"fmt", "--check", "core/demo.cpp"});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    REQUIRE(contains(r.stderr_output, "pulp fmt --check: formatting issues detected."));
}
