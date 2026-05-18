// test_cli_package_commands.cpp — local-only command tests for
// `tools/cli/package_commands.cpp`.
//
// Coverage tranche for issue #643. These tests exercise the command
// surface against a staged project root, local registry JSON, and local
// lock/config files. They intentionally avoid remote registry fetch,
// archive extraction, or SDK/setup side effects.

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/package_commands.hpp"
#include "../tools/cli/package_registry.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pulp::cli::pkg;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        auto base = fs::temp_directory_path();
        static std::atomic<int> seq{0};
        const int n = seq.fetch_add(1);
        path = base / ("pulp-cli-package-commands-test-" +
                       std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                       "-" + std::to_string(n));
        fs::create_directories(path);
        std::error_code ec;
        auto canon = fs::weakly_canonical(path, ec);
        if (!ec) path = canon;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct ScopedCurrentPath {
    fs::path previous;
    explicit ScopedCurrentPath(const fs::path& next) : previous(fs::current_path()) {
        fs::current_path(next);
    }
    ~ScopedCurrentPath() {
        std::error_code ec;
        fs::current_path(previous, ec);
    }
};

struct StreamCapture {
    std::ostringstream out;
    std::ostringstream err;
    std::streambuf* old_out = nullptr;
    std::streambuf* old_err = nullptr;

    StreamCapture() {
        old_out = std::cout.rdbuf(out.rdbuf());
        old_err = std::cerr.rdbuf(err.rdbuf());
    }

    ~StreamCapture() {
        std::cout.rdbuf(old_out);
        std::cerr.rdbuf(old_err);
    }
};

struct CommandResult {
    int exit_code = 0;
    std::string stdout_text;
    std::string stderr_text;
};

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << body;
}

std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    std::ostringstream out;
    out << f.rdbuf();
    return out.str();
}

void write_registry_fixture(const fs::path& root,
                            const std::string& signalsmith_version = "1.2.0") {
    write_file(root / "tools" / "packages" / "registry.json", R"({
  "registry_version": 1,
  "packages": {
    "signalsmith-dsp": {
      "name": "Signalsmith DSP",
      "version": ")" + signalsmith_version + R"(",
      "description": "Fast DSP helpers for filters and resampling",
      "license": "MIT",
      "category": "DSP",
      "url": "https://example.com/signalsmith",
      "fetch": {
        "method": "header-only",
        "git_repository": "https://example.com/signalsmith.git",
        "git_tag": "v)" + signalsmith_version + R"("
      },
      "cmake": {
        "targets": ["signalsmith::dsp"],
        "header_only": true,
        "include_dir": "include"
      },
      "platforms": {
        "macOS": {"architectures": ["arm64", "x64"]},
        "Windows": {"architectures": ["x64"]},
        "Linux": {"architectures": ["x64"]}
      },
      "rt_safe": true,
      "tags": ["signalsmith", "filter", "dsp"],
      "provides": ["filter", "resampler"],
      "overlaps_with_builtin": {
        "pulp/filter.hpp": "basic filter blocks"
      },
      "unique_value": "specialized DSP utilities",
      "verification": {
        "last_verified": "2026-04-22",
        "verified_version": ")" + signalsmith_version + R"(",
        "build_status": {
          "macOS": "pass",
          "Windows": "pass",
          "Linux": "pass"
        }
      }
    },
    "gpl-filter": {
      "name": "GPL Filter",
      "version": "3.0.0",
      "description": "Copyleft filter implementation",
      "license": "GPL-3.0",
      "category": "DSP",
      "url": "https://example.com/gpl-filter",
      "fetch": {
        "method": "header-only",
        "git_repository": "https://example.com/gpl-filter.git",
        "git_tag": "v3.0.0"
      },
      "cmake": {
        "targets": ["gpl::filter"],
        "header_only": true,
        "include_dir": "include"
      },
      "platforms": {
        "macOS": {"architectures": ["arm64"]},
        "Windows": {"architectures": ["x64"]}
      },
      "tags": ["filter", "gpl"],
      "provides": ["filter"],
      "alternatives": ["signalsmith-dsp"],
      "verification": {
        "last_verified": "2026-04-20",
        "verified_version": "3.0.0",
        "build_status": {
          "macOS": "pass"
        }
      }
    },
    "mpl-analyzer": {
      "name": "MPL Analyzer",
      "version": "0.9.0",
      "description": "Spectral analysis helper",
      "license": "MPL-2.0",
      "category": "Analysis",
      "url": "https://example.com/mpl-analyzer",
      "fetch": {
        "method": "header-only",
        "git_repository": "https://example.com/mpl-analyzer.git",
        "git_tag": "v0.9.0"
      },
      "cmake": {
        "targets": ["mpl::analyzer"],
        "header_only": true,
        "include_dir": "include"
      },
      "platforms": {
        "macOS": {"architectures": ["arm64"]},
        "Windows": {"architectures": ["x64"]}
      },
      "tags": ["analysis", "fft"],
      "provides": ["analysis"],
      "verification": {
        "last_verified": "2026-04-21",
        "verified_version": "0.9.0",
        "build_status": {
          "macOS": "pass",
          "Windows": "pass"
        }
      }
    }
  }
}
)");
}

void write_project_scaffold(const fs::path& root) {
    fs::create_directories(root / "cmake");
    write_file(root / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.22)\nproject(TestPlugin)\n");
    write_file(root / "DEPENDENCIES.md",
               "| Name | Version | License | Source | Notes | Checked |\n"
               "|---|---|---|---|---|---|\n");
    write_file(root / "NOTICE.md", "# Notice\n");
}

CommandResult run_in_project(const fs::path& root, auto&& fn) {
    ScopedCurrentPath cwd(root);
    StreamCapture capture;
    const int rc = fn();
    return {rc, capture.out.str(), capture.err.str()};
}

LockFile make_lock(std::initializer_list<std::pair<const std::string, LockedPackage>> pkgs) {
    LockFile lock;
    lock.version = 1;
    for (const auto& [id, pkg] : pkgs) {
        lock.packages[id] = pkg;
    }
    return lock;
}

std::vector<std::string> target_strings(const fs::path& root) {
    std::vector<std::string> out;
    for (const auto& t : read_project_targets(root)) out.push_back(t.to_string());
    return out;
}

}  // namespace

TEST_CASE("cmd_target manages project targets from local pulp.toml",
          "[cli][package-commands][target][issue-643]") {
    TempDir tmp;
    write_project_scaffold(tmp.path);
    write_registry_fixture(tmp.path);
    REQUIRE(write_project_targets(
        tmp.path,
        {PlatformTarget{"macOS", "arm64"}, PlatformTarget{"Windows", "x64"}}));

    auto help = run_in_project(tmp.path, [&] { return cmd_target({}); });
    REQUIRE(help.exit_code == 0);
    REQUIRE(help.stdout_text.find("Usage: pulp target") != std::string::npos);

    auto listed = run_in_project(tmp.path, [&] { return cmd_target({"list"}); });
    REQUIRE(listed.exit_code == 0);
    REQUIRE(listed.stdout_text.find("macOS-arm64") != std::string::npos);
    REQUIRE(listed.stdout_text.find("Windows-x64") != std::string::npos);

    auto add_linux = run_in_project(tmp.path, [&] { return cmd_target({"add", "Linux-x64"}); });
    REQUIRE(add_linux.exit_code == 0);
    REQUIRE(add_linux.stdout_text.find("Added target: Linux-x64") != std::string::npos);
    const std::vector<std::string> expected_targets = {
        "macOS-arm64", "Windows-x64", "Linux-x64"
    };
    REQUIRE(target_strings(tmp.path) == expected_targets);

    auto duplicate = run_in_project(tmp.path, [&] { return cmd_target({"add", "Linux-x64"}); });
    REQUIRE(duplicate.exit_code == 0);
    REQUIRE(duplicate.stdout_text.find("already configured") != std::string::npos);

    auto invalid = run_in_project(tmp.path, [&] { return cmd_target({"add", "Mars-x64"}); });
    REQUIRE(invalid.exit_code == 1);
    REQUIRE(invalid.stderr_text.find("Invalid target: Mars-x64") != std::string::npos);

    auto remove_linux = run_in_project(tmp.path, [&] { return cmd_target({"remove", "Linux-x64"}); });
    REQUIRE(remove_linux.exit_code == 0);
    REQUIRE(remove_linux.stdout_text.find("Removed target: Linux-x64") != std::string::npos);

    auto remove_missing = run_in_project(tmp.path, [&] { return cmd_target({"remove", "Linux-x64"}); });
    REQUIRE(remove_missing.exit_code == 1);
    REQUIRE(remove_missing.stderr_text.find("is not configured") != std::string::npos);

    REQUIRE(write_project_targets(tmp.path, {PlatformTarget{"macOS", "arm64"}}));
    auto remove_last = run_in_project(tmp.path, [&] { return cmd_target({"remove", "macOS-arm64"}); });
    REQUIRE(remove_last.exit_code == 1);
    REQUIRE(remove_last.stderr_text.find("Cannot remove the last target") != std::string::npos);

    auto add_missing_arg = run_in_project(tmp.path, [&] { return cmd_target({"add"}); });
    REQUIRE(add_missing_arg.exit_code == 1);
    REQUIRE(add_missing_arg.stderr_text.find("Usage: pulp target add") != std::string::npos);

    auto remove_missing_arg = run_in_project(tmp.path, [&] { return cmd_target({"remove"}); });
    REQUIRE(remove_missing_arg.exit_code == 1);
    REQUIRE(remove_missing_arg.stderr_text.find("Usage: pulp target remove") != std::string::npos);

    auto unknown = run_in_project(tmp.path, [&] { return cmd_target({"rename"}); });
    REQUIRE(unknown.exit_code == 1);
    REQUIRE(unknown.stderr_text.find("Unknown target subcommand") != std::string::npos);
}

TEST_CASE("package commands report project-root failures without remote work",
          "[cli][package-commands][errors][issue-643]") {
    TempDir tmp;
    ScopedCurrentPath cwd(tmp.path);

    {
        StreamCapture capture;
        REQUIRE(cmd_target({"list"}) == 1);
        REQUIRE(capture.err.str().find("Not in a Pulp project") != std::string::npos);
    }
    {
        StreamCapture capture;
        REQUIRE(cmd_list({}) == 1);
        REQUIRE(capture.err.str().find("Not in a Pulp project") != std::string::npos);
    }
    {
        StreamCapture capture;
        REQUIRE(cmd_update({}) == 1);
        REQUIRE(capture.err.str().find("Not in a Pulp project") != std::string::npos);
    }
    {
        StreamCapture capture;
        REQUIRE(cmd_suggest({"--description", "filter"}) == 1);
        REQUIRE(capture.err.str().find("Package registry not found") != std::string::npos);
    }
}

TEST_CASE("cmd_target list shows source-checkout defaults without pulp.toml",
          "[cli][package-commands][target][issue-643]") {
    TempDir tmp;
    write_file(tmp.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.22)\n");
    fs::create_directories(tmp.path / "core");

    auto listed = run_in_project(tmp.path, [&] { return cmd_target({"list"}); });
    REQUIRE(listed.exit_code == 0);
    REQUIRE(listed.stdout_text.find("defaults") != std::string::npos);
    REQUIRE(listed.stdout_text.find("macOS-arm64") != std::string::npos);
}

TEST_CASE("cmd_target add warns when installed packages do not support the new target",
          "[cli][package-commands][target][issue-493]") {
    TempDir tmp;
    write_project_scaffold(tmp.path);
    write_registry_fixture(tmp.path);
    REQUIRE(write_project_targets(
        tmp.path,
        {PlatformTarget{"macOS", "arm64"}, PlatformTarget{"Windows", "x64"}}));
    REQUIRE(save_lock_file(tmp.path / "packages.lock.json", make_lock({
        {"mpl-analyzer", {"0.9.0", "https://example.com/mpl-analyzer.git", "", "v0.9.0"}},
    })));

    auto added = run_in_project(tmp.path, [&] { return cmd_target({"add", "Linux-x64"}); });
    REQUIRE(added.exit_code == 0);
    REQUIRE(added.stdout_text.find("Added target: Linux-x64") != std::string::npos);
    REQUIRE(added.stdout_text.find("MPL Analyzer does not support Linux-x64") !=
            std::string::npos);
    const std::vector<std::string> expected_targets = {
        "macOS-arm64", "Windows-x64", "Linux-x64"
    };
    REQUIRE(target_strings(tmp.path) == expected_targets);
}

TEST_CASE("search, list, suggest, and audit commands use staged local data",
          "[cli][package-commands][registry][issue-643]") {
    TempDir tmp;
    write_project_scaffold(tmp.path);
    write_registry_fixture(tmp.path);
    REQUIRE(write_project_targets(
        tmp.path,
        {PlatformTarget{"macOS", "arm64"}, PlatformTarget{"Windows", "x64"}, PlatformTarget{"Linux", "x64"}}));
    REQUIRE(save_lock_file(tmp.path / "packages.lock.json", make_lock({
        {"signalsmith-dsp", {"1.2.0", "https://example.com/signalsmith.git", "", "v1.2.0"}},
        {"mpl-analyzer", {"0.9.0", "https://example.com/mpl-analyzer.git", "", "v0.9.0"}},
        {"missing-pkg", {"0.1.0", "https://example.com/missing.git", "", "deadbeef"}},
    })));
    write_file(tmp.path / "demo.cpp", "#include <signalsmith/filter.hpp>\n");

    auto search_text = run_in_project(tmp.path, [&] { return cmd_search({"filter"}); });
    REQUIRE(search_text.exit_code == 0);
    REQUIRE(search_text.stdout_text.find("signalsmith-dsp") != std::string::npos);
    REQUIRE(search_text.stdout_text.find("gpl-filter") != std::string::npos);

    auto search_json = run_in_project(tmp.path, [&] { return cmd_search({"signalsmith", "--format", "json"}); });
    REQUIRE(search_json.exit_code == 0);
    REQUIRE(search_json.stdout_text.find("\"id\": \"signalsmith-dsp\"") != std::string::npos);

    auto listed = run_in_project(tmp.path, [&] { return cmd_list({}); });
    REQUIRE(listed.exit_code == 0);
    REQUIRE(listed.stdout_text.find("Installed packages (3)") != std::string::npos);
    REQUIRE(listed.stdout_text.find("Signalsmith DSP") != std::string::npos);

    auto list_json = run_in_project(tmp.path, [&] { return cmd_list({"--json"}); });
    REQUIRE(list_json.exit_code == 0);
    REQUIRE(list_json.stdout_text.find("\"signalsmith-dsp\"") != std::string::npos);
    REQUIRE(list_json.stdout_text.find("\"missing-pkg\"") != std::string::npos);

    auto suggest_description = run_in_project(tmp.path, [&] {
        return cmd_suggest({"--description", "filter"});
    });
    REQUIRE(suggest_description.exit_code == 0);
    REQUIRE(suggest_description.stdout_text.find("Suggested packages for") != std::string::npos);
    REQUIRE(suggest_description.stdout_text.find("signalsmith-dsp") != std::string::npos);

    auto suggest_analyze = run_in_project(tmp.path, [&] {
        return cmd_suggest({"--analyze", "demo.cpp"});
    });
    REQUIRE(suggest_analyze.exit_code == 0);
    REQUIRE(suggest_analyze.stdout_text.find("Based on includes in demo.cpp") != std::string::npos);
    REQUIRE(suggest_analyze.stdout_text.find("signalsmith-dsp") != std::string::npos);

    auto suggest_alternative = run_in_project(tmp.path, [&] {
        return cmd_suggest({"--alternative", "gpl-filter"});
    });
    REQUIRE(suggest_alternative.exit_code == 0);
    REQUIRE(suggest_alternative.stdout_text.find("Alternatives to GPL Filter") != std::string::npos);
    REQUIRE(suggest_alternative.stdout_text.find("signalsmith-dsp") != std::string::npos);

    auto audit_pkgs = run_in_project(tmp.path, [&] { return audit_packages(tmp.path); });
    REQUIRE(audit_pkgs.exit_code == 1);
    REQUIRE(audit_pkgs.stdout_text.find("NOT IN REGISTRY") != std::string::npos);

    auto audit_platforms_result = run_in_project(tmp.path, [&] { return audit_platforms(tmp.path); });
    REQUIRE(audit_platforms_result.exit_code == 1);
    REQUIRE(audit_platforms_result.stdout_text.find("platform gap(s) found") != std::string::npos);
    REQUIRE(audit_platforms_result.stdout_text.find("Linux-x64") != std::string::npos);

    auto audit_licenses_result = run_in_project(tmp.path, [&] { return audit_licenses(tmp.path); });
    REQUIRE(audit_licenses_result.exit_code == 1);
    REQUIRE(audit_licenses_result.stdout_text.find("MPL-2.0 REVIEW") != std::string::npos);
}

TEST_CASE("search, list, suggest, and audit commands cover empty and error modes",
          "[cli][package-commands][registry][issue-643]") {
    TempDir tmp;
    write_project_scaffold(tmp.path);
    write_registry_fixture(tmp.path);
    REQUIRE(write_project_targets(tmp.path, {PlatformTarget{"macOS", "arm64"}}));

    auto no_lock_list = run_in_project(tmp.path, [&] { return cmd_list({}); });
    REQUIRE(no_lock_list.exit_code == 0);
    REQUIRE(no_lock_list.stdout_text.find("No packages installed") != std::string::npos);

    REQUIRE(save_lock_file(tmp.path / "packages.lock.json", make_lock({})));
    auto empty_lock_list = run_in_project(tmp.path, [&] { return cmd_list({}); });
    REQUIRE(empty_lock_list.exit_code == 0);
    REQUIRE(empty_lock_list.stdout_text.find("No packages installed") != std::string::npos);

    auto no_hits = run_in_project(tmp.path, [&] { return cmd_search({"zzzz-no-match"}); });
    REQUIRE(no_hits.exit_code == 0);
    REQUIRE(no_hits.stdout_text.find("No packages found matching") != std::string::npos);

    auto search_help = run_in_project(tmp.path, [&] { return cmd_search({}); });
    REQUIRE(search_help.exit_code == 0);
    REQUIRE(search_help.stdout_text.find("Usage: pulp search") != std::string::npos);

    auto search_missing_format_value = run_in_project(tmp.path, [&] {
        return cmd_search({"filter", "--format"});
    });
    REQUIRE(search_missing_format_value.exit_code == 2);
    REQUIRE(search_missing_format_value.stderr_text.find("--format requires a value") !=
            std::string::npos);

    auto search_unknown_flag = run_in_project(tmp.path, [&] {
        return cmd_search({"filter", "--verbose"});
    });
    REQUIRE(search_unknown_flag.exit_code == 2);
    REQUIRE(search_unknown_flag.stderr_text.find("Unknown search option") != std::string::npos);

    auto suggest_help = run_in_project(tmp.path, [&] { return cmd_suggest({}); });
    REQUIRE(suggest_help.exit_code == 0);
    REQUIRE(suggest_help.stdout_text.find("Usage: pulp suggest") != std::string::npos);

    auto suggest_json = run_in_project(tmp.path, [&] {
        return cmd_suggest({"--description", "filter", "--format", "json"});
    });
    REQUIRE(suggest_json.exit_code == 0);
    REQUIRE(suggest_json.stdout_text.find("\"id\": \"signalsmith-dsp\"") != std::string::npos);

    auto suggest_empty_json = run_in_project(tmp.path, [&] {
        return cmd_suggest({"--description", "zzzz-no-match", "--format", "json"});
    });
    REQUIRE(suggest_empty_json.exit_code == 0);
    REQUIRE(suggest_empty_json.stdout_text.find("[]") != std::string::npos);

    auto suggest_missing_file = run_in_project(tmp.path, [&] {
        return cmd_suggest({"--analyze", "missing.cpp"});
    });
    REQUIRE(suggest_missing_file.exit_code == 1);
    REQUIRE(suggest_missing_file.stderr_text.find("Cannot read file") != std::string::npos);

    write_file(tmp.path / "unmatched.cpp", "#include <unrelated/header.hpp>\n");
    auto suggest_unmatched = run_in_project(tmp.path, [&] {
        return cmd_suggest({"--analyze", "unmatched.cpp"});
    });
    REQUIRE(suggest_unmatched.exit_code == 0);
    REQUIRE(suggest_unmatched.stdout_text.find("No package suggestions") != std::string::npos);

    auto suggest_missing_alternative = run_in_project(tmp.path, [&] {
        return cmd_suggest({"--alternative", "missing-pkg"});
    });
    REQUIRE(suggest_missing_alternative.exit_code == 1);
    REQUIRE(suggest_missing_alternative.stderr_text.find("not found") != std::string::npos);

    auto suggest_no_mode = run_in_project(tmp.path, [&] { return cmd_suggest({"--format", "json"}); });
    REQUIRE(suggest_no_mode.exit_code == 1);
    REQUIRE(suggest_no_mode.stderr_text.find("Specify --description") != std::string::npos);

    auto suggest_missing_description = run_in_project(tmp.path, [&] {
        return cmd_suggest({"--description", "--format", "json"});
    });
    REQUIRE(suggest_missing_description.exit_code == 2);
    REQUIRE(suggest_missing_description.stderr_text.find("--description requires a value") !=
            std::string::npos);

    auto suggest_bad_format = run_in_project(tmp.path, [&] {
        return cmd_suggest({"--description", "filter", "--format", "text"});
    });
    REQUIRE(suggest_bad_format.exit_code == 2);
    REQUIRE(suggest_bad_format.stderr_text.find("--format must be json") != std::string::npos);

    auto suggest_unexpected_arg = run_in_project(tmp.path, [&] {
        return cmd_suggest({"filter"});
    });
    REQUIRE(suggest_unexpected_arg.exit_code == 2);
    REQUIRE(suggest_unexpected_arg.stderr_text.find("Unexpected suggest argument") !=
            std::string::npos);

    fs::remove(tmp.path / "packages.lock.json");
    auto audit_no_packages = run_in_project(tmp.path, [&] { return audit_packages(tmp.path); });
    REQUIRE(audit_no_packages.exit_code == 0);
    REQUIRE(audit_no_packages.stdout_text.find("nothing to audit") != std::string::npos);

    REQUIRE(save_lock_file(tmp.path / "packages.lock.json", make_lock({
        {"signalsmith-dsp", {"1.2.0", "https://example.com/signalsmith.git", "", "v1.2.0"}},
    })));
    auto audit_packages_ok = run_in_project(tmp.path, [&] { return audit_packages(tmp.path); });
    REQUIRE(audit_packages_ok.exit_code == 0);
    REQUIRE(audit_packages_ok.stdout_text.find("0 issues") != std::string::npos);

    auto audit_platforms_ok = run_in_project(tmp.path, [&] { return audit_platforms(tmp.path); });
    REQUIRE(audit_platforms_ok.exit_code == 0);
    REQUIRE(audit_platforms_ok.stdout_text.find("All packages support") != std::string::npos);

    auto audit_licenses_ok = run_in_project(tmp.path, [&] { return audit_licenses(tmp.path); });
    REQUIRE(audit_licenses_ok.exit_code == 0);
    REQUIRE(audit_licenses_ok.stdout_text.find("MIT OK") != std::string::npos);

    fs::remove(tmp.path / "tools" / "packages" / "registry.json");
    auto audit_missing_registry = run_in_project(tmp.path, [&] { return audit_packages(tmp.path); });
    REQUIRE(audit_missing_registry.exit_code == 1);
    REQUIRE(audit_missing_registry.stderr_text.find("Package registry not found") != std::string::npos);

    auto audit_platforms_missing_registry = run_in_project(tmp.path, [&] {
        return audit_platforms(tmp.path);
    });
    REQUIRE(audit_platforms_missing_registry.exit_code == 1);
    REQUIRE(audit_platforms_missing_registry.stderr_text.find("Registry not found") != std::string::npos);

    auto audit_licenses_missing_registry = run_in_project(tmp.path, [&] {
        return audit_licenses(tmp.path);
    });
    REQUIRE(audit_licenses_missing_registry.exit_code == 1);
    REQUIRE(audit_licenses_missing_registry.stderr_text.find("Registry not found") != std::string::npos);
}

TEST_CASE("cmd_update updates only local lock and generated cmake",
          "[cli][package-commands][update][issue-643]") {
    TempDir tmp;
    write_project_scaffold(tmp.path);
    write_registry_fixture(tmp.path, "1.2.0");
    REQUIRE(write_project_targets(
        tmp.path,
        {PlatformTarget{"macOS", "arm64"}, PlatformTarget{"Windows", "x64"}}));
    REQUIRE(save_lock_file(tmp.path / "packages.lock.json", make_lock({
        {"signalsmith-dsp", {"1.0.0", "https://example.com/signalsmith.git", "", "v1.0.0"}},
    })));

    auto dry_run = run_in_project(tmp.path, [&] { return cmd_update({}); });
    REQUIRE(dry_run.exit_code == 0);
    REQUIRE(dry_run.stdout_text.find("Signalsmith DSP") != std::string::npos);
    REQUIRE(dry_run.stdout_text.find("Run 'pulp update --apply'") != std::string::npos);
    REQUIRE(load_lock_file(tmp.path / "packages.lock.json").packages["signalsmith-dsp"].version == "1.0.0");

    auto apply = run_in_project(tmp.path, [&] { return cmd_update({"--apply"}); });
    REQUIRE(apply.exit_code == 0);
    REQUIRE(apply.stdout_text.find("Updated packages") != std::string::npos);
    REQUIRE(load_lock_file(tmp.path / "packages.lock.json").packages["signalsmith-dsp"].version == "1.2.0");
    REQUIRE(read_file(tmp.path / "cmake" / "pulp-packages.cmake").find("signalsmith-dsp") != std::string::npos);
}

TEST_CASE("cmd_update reports no-op and missing-registry states",
          "[cli][package-commands][update][issue-643]") {
    TempDir tmp;
    write_project_scaffold(tmp.path);
    write_registry_fixture(tmp.path);
    REQUIRE(write_project_targets(tmp.path, {PlatformTarget{"macOS", "arm64"}}));

    auto no_lock = run_in_project(tmp.path, [&] { return cmd_update({}); });
    REQUIRE(no_lock.exit_code == 0);
    REQUIRE(no_lock.stdout_text.find("No packages installed") != std::string::npos);

    REQUIRE(save_lock_file(tmp.path / "packages.lock.json", make_lock({
        {"signalsmith-dsp", {"1.2.0", "https://example.com/signalsmith.git", "", "v1.2.0"}},
    })));
    auto no_updates = run_in_project(tmp.path, [&] { return cmd_update({}); });
    REQUIRE(no_updates.exit_code == 0);
    REQUIRE(no_updates.stdout_text.find("All packages are up to date") != std::string::npos);

    fs::remove(tmp.path / "tools" / "packages" / "registry.json");
    auto missing_registry = run_in_project(tmp.path, [&] { return cmd_update({}); });
    REQUIRE(missing_registry.exit_code == 1);
    REQUIRE(missing_registry.stderr_text.find("Package registry not found") != std::string::npos);
}

TEST_CASE("cmd_add and cmd_remove stay local on failure and success paths",
          "[cli][package-commands][add-remove][issue-643]") {
    TempDir tmp;
    write_project_scaffold(tmp.path);
    write_registry_fixture(tmp.path);
    REQUIRE(write_project_targets(
        tmp.path,
        {PlatformTarget{"macOS", "arm64"}, PlatformTarget{"Windows", "x64"}}));
    REQUIRE(save_lock_file(tmp.path / "packages.lock.json", make_lock({})));

    auto no_package = run_in_project(tmp.path, [&] { return cmd_add({}); });
    REQUIRE(no_package.exit_code == 0);
    REQUIRE(no_package.stdout_text.find("Usage: pulp add") != std::string::npos);

    auto missing = run_in_project(tmp.path, [&] { return cmd_add({"signalsmith"}); });
    REQUIRE(missing.exit_code == 1);
    REQUIRE(missing.stderr_text.find("not found in registry") != std::string::npos);
    REQUIRE(missing.stdout_text.find("Did you mean") != std::string::npos);

    auto rejected = run_in_project(tmp.path, [&] { return cmd_add({"gpl-filter"}); });
    REQUIRE(rejected.exit_code == 1);
    REQUIRE(rejected.stderr_text.find("GPL-3.0 licensed") != std::string::npos);
    REQUIRE(rejected.stdout_text.find("--accept-license GPL-3.0") != std::string::npos);

    auto add_ok = run_in_project(tmp.path, [&] { return cmd_add({"signalsmith-dsp", "--no-cmake"}); });
    REQUIRE(add_ok.exit_code == 0);
    REQUIRE(add_ok.stdout_text.find("Added Signalsmith DSP v1.2.0") != std::string::npos);
    REQUIRE(load_lock_file(tmp.path / "packages.lock.json").packages.count("signalsmith-dsp") == 1);
    REQUIRE(read_file(tmp.path / "DEPENDENCIES.md").find("Signalsmith DSP") != std::string::npos);
    REQUIRE(read_file(tmp.path / "NOTICE.md").find("## Signalsmith DSP") != std::string::npos);

    auto remove_missing = run_in_project(tmp.path, [&] { return cmd_remove({"missing-pkg"}); });
    REQUIRE(remove_missing.exit_code == 1);
    REQUIRE(remove_missing.stderr_text.find("is not installed") != std::string::npos);

    auto remove_ok = run_in_project(tmp.path, [&] { return cmd_remove({"signalsmith-dsp"}); });
    REQUIRE(remove_ok.exit_code == 0);
    REQUIRE(remove_ok.stdout_text.find("Removed Signalsmith DSP") != std::string::npos);
    REQUIRE(load_lock_file(tmp.path / "packages.lock.json").packages.empty());
}

TEST_CASE("cmd_add writes unguarded generated cmake and cmd_remove deletes last generated file",
          "[cli][package-commands][add-remove][issue-643]") {
    TempDir tmp;
    write_project_scaffold(tmp.path);
    write_registry_fixture(tmp.path);
    REQUIRE(write_project_targets(
        tmp.path,
        {PlatformTarget{"macOS", "arm64"}, PlatformTarget{"Windows", "x64"}}));
    REQUIRE(save_lock_file(tmp.path / "packages.lock.json", make_lock({})));

    auto added = run_in_project(tmp.path, [&] { return cmd_add({"signalsmith-dsp"}); });
    REQUIRE(added.exit_code == 0);
    REQUIRE(added.stdout_text.find("Added Signalsmith DSP v1.2.0") != std::string::npos);
    REQUIRE(added.stdout_text.find("target_link_libraries") != std::string::npos);

    const auto cmake_path = tmp.path / "cmake" / "pulp-packages.cmake";
    REQUIRE(fs::exists(cmake_path));
    auto cmake = read_file(cmake_path);
    REQUIRE(cmake.find("include(FetchContent)") != std::string::npos);
    REQUIRE(cmake.find("FetchContent_Declare(signalsmith-dsp") != std::string::npos);
    REQUIRE(cmake.find("GIT_REPOSITORY https://example.com/signalsmith.git") !=
            std::string::npos);
    REQUIRE(cmake.find("GIT_TAG        v1.2.0") != std::string::npos);
    REQUIRE(cmake.find("add_library(signalsmith::dsp INTERFACE)") != std::string::npos);
    REQUIRE(cmake.find("target_include_directories(signalsmith::dsp INTERFACE") !=
            std::string::npos);
    REQUIRE(read_file(tmp.path / "CMakeLists.txt")
                .find("include(cmake/pulp-packages.cmake OPTIONAL)") != std::string::npos);
    REQUIRE(load_lock_file(tmp.path / "packages.lock.json")
                .packages.count("signalsmith-dsp") == 1);

    auto removed = run_in_project(tmp.path, [&] { return cmd_remove({"signalsmith-dsp"}); });
    REQUIRE(removed.exit_code == 0);
    REQUIRE(removed.stdout_text.find("Removed Signalsmith DSP") != std::string::npos);
    REQUIRE_FALSE(fs::exists(cmake_path));
    REQUIRE(load_lock_file(tmp.path / "packages.lock.json").packages.empty());
    REQUIRE(read_file(tmp.path / "DEPENDENCIES.md").find("Signalsmith DSP") ==
            std::string::npos);
    REQUIRE(read_file(tmp.path / "NOTICE.md").find("## Signalsmith DSP") ==
            std::string::npos);
}

TEST_CASE("cmd_add covers guarded installs and installed-version guards",
          "[cli][package-commands][add-remove][issue-643]") {
    TempDir tmp;
    write_project_scaffold(tmp.path);
    write_registry_fixture(tmp.path);
    REQUIRE(write_project_targets(
        tmp.path,
        {PlatformTarget{"macOS", "arm64"}, PlatformTarget{"Windows", "x64"}, PlatformTarget{"Linux", "x64"}}));
    REQUIRE(save_lock_file(tmp.path / "packages.lock.json", make_lock({})));

    auto missing_id = run_in_project(tmp.path, [&] { return cmd_add({"--no-cmake"}); });
    REQUIRE(missing_id.exit_code == 1);
    REQUIRE(missing_id.stderr_text.find("No package specified") != std::string::npos);

    auto missing_license_value = run_in_project(tmp.path, [&] {
        return cmd_add({"gpl-filter", "--accept-license", "--no-cmake"});
    });
    REQUIRE(missing_license_value.exit_code == 2);
    REQUIRE(missing_license_value.stderr_text.find("--accept-license requires a value") !=
            std::string::npos);

    auto bad_license_override = run_in_project(tmp.path, [&] {
        return cmd_add({"gpl-filter", "--license-override", "internal"});
    });
    REQUIRE(bad_license_override.exit_code == 2);
    REQUIRE(bad_license_override.stderr_text.find("--license-override must be commercial") !=
            std::string::npos);

    auto unknown_add_flag = run_in_project(tmp.path, [&] {
        return cmd_add({"signalsmith-dsp", "--verbose"});
    });
    REQUIRE(unknown_add_flag.exit_code == 2);
    REQUIRE(unknown_add_flag.stderr_text.find("Unknown add option") != std::string::npos);

    auto license_mismatch = run_in_project(tmp.path, [&] {
        return cmd_add({"gpl-filter", "--accept-license", "MIT"});
    });
    REQUIRE(license_mismatch.exit_code == 1);
    REQUIRE(license_mismatch.stderr_text.find("does not match package license") != std::string::npos);

    auto unsupported_without_guard = run_in_project(tmp.path, [&] {
        return cmd_add({"mpl-analyzer"});
    });
    REQUIRE(unsupported_without_guard.exit_code == 1);
    REQUIRE(unsupported_without_guard.stdout_text.find("Use --platform-guard") != std::string::npos);

    auto guarded = run_in_project(tmp.path, [&] {
        return cmd_add({"gpl-filter", "--accept-license", "GPL-3.0", "--platform-guard"});
    });
    REQUIRE(guarded.exit_code == 0);
    REQUIRE(guarded.stdout_text.find("Installing GPL-3.0 package") != std::string::npos);
    REQUIRE(guarded.stdout_text.find("Added GPL Filter v3.0.0") != std::string::npos);
    auto cmake = read_file(tmp.path / "cmake" / "pulp-packages.cmake");
    REQUIRE(cmake.find("if(") != std::string::npos);
    REQUIRE(cmake.find("APPLE") != std::string::npos);
    REQUIRE(cmake.find("WIN32") != std::string::npos);
    REQUIRE(cmake.find("PULP_HAS_GPL_FILTER") != std::string::npos);
    REQUIRE(read_file(tmp.path / "CMakeLists.txt").find("include(cmake/pulp-packages.cmake OPTIONAL)")
            != std::string::npos);

    auto already_current = run_in_project(tmp.path, [&] {
        return cmd_add({"gpl-filter", "--accept-license", "GPL-3.0"});
    });
    REQUIRE(already_current.exit_code == 0);
    REQUIRE(already_current.stdout_text.find("is already installed") != std::string::npos);

    REQUIRE(save_lock_file(tmp.path / "packages.lock.json", make_lock({
        {"signalsmith-dsp", {"1.0.0", "https://example.com/signalsmith.git", "", "v1.0.0"}},
    })));
    auto installed_old = run_in_project(tmp.path, [&] { return cmd_add({"signalsmith-dsp"}); });
    REQUIRE(installed_old.exit_code == 0);
    REQUIRE(installed_old.stdout_text.find("Use 'pulp update' to upgrade") != std::string::npos);
}

TEST_CASE("cmd_add accepts commercial override for proprietary packages",
          "[cli][package-commands][add-remove][coverage]") {
    TempDir tmp;
    write_project_scaffold(tmp.path);
    write_file(tmp.path / "tools" / "packages" / "registry.json", R"({
  "registry_version": 1,
  "packages": {
    "commercial-sdk": {
      "name": "Commercial SDK",
      "version": "2.0.0",
      "description": "Closed-source SDK with a commercial license",
      "license": "Proprietary",
      "category": "SDK",
      "url": "https://example.com/commercial",
      "fetch": {
        "method": "header-only",
        "git_repository": "https://example.com/commercial.git",
        "git_tag": "v2.0.0"
      },
      "cmake": {
        "targets": ["commercial::sdk"],
        "header_only": true,
        "include_dir": "include"
      },
      "platforms": {
        "macOS": {"architectures": ["arm64"]}
      },
      "verification": {
        "last_verified": "2026-04-22",
        "verified_version": "2.0.0",
        "build_status": {"macOS": "pass"}
      }
    }
  }
}
)");
    REQUIRE(write_project_targets(tmp.path, {PlatformTarget{"macOS", "arm64"}}));

    auto rejected = run_in_project(tmp.path, [&] { return cmd_add({"commercial-sdk"}); });
    REQUIRE(rejected.exit_code == 1);
    REQUIRE(rejected.stderr_text.find("cannot be used") != std::string::npos);

    auto accepted = run_in_project(tmp.path, [&] {
        return cmd_add({"commercial-sdk", "--license-override", "commercial", "--no-cmake"});
    });
    REQUIRE(accepted.exit_code == 0);
    REQUIRE(accepted.stdout_text.find("Installing Proprietary package") != std::string::npos);
    REQUIRE(accepted.stdout_text.find("Added Commercial SDK v2.0.0") != std::string::npos);
    REQUIRE(load_lock_file(tmp.path / "packages.lock.json")
                .packages.count("commercial-sdk") == 1);
}

TEST_CASE("cmd_remove handles help and registry-free removal",
          "[cli][package-commands][add-remove][issue-643]") {
    TempDir tmp;
    write_project_scaffold(tmp.path);
    REQUIRE(write_project_targets(tmp.path, {PlatformTarget{"macOS", "arm64"}}));
    REQUIRE(save_lock_file(tmp.path / "packages.lock.json", make_lock({
        {"orphan-pkg", {"0.1.0", "https://example.com/orphan.git", "", "v0.1.0"}},
    })));

    auto help = run_in_project(tmp.path, [&] { return cmd_remove({}); });
    REQUIRE(help.exit_code == 0);
    REQUIRE(help.stdout_text.find("Usage: pulp remove") != std::string::npos);

    auto removed = run_in_project(tmp.path, [&] { return cmd_remove({"orphan-pkg"}); });
    REQUIRE(removed.exit_code == 0);
    REQUIRE(removed.stdout_text.find("Removed orphan-pkg") != std::string::npos);
    REQUIRE(load_lock_file(tmp.path / "packages.lock.json").packages.empty());
}

TEST_CASE("cmd_remove regenerates cmake when other packages remain",
          "[cli][package-commands][add-remove][coverage]") {
    TempDir tmp;
    write_project_scaffold(tmp.path);
    write_registry_fixture(tmp.path);
    REQUIRE(write_project_targets(tmp.path, {PlatformTarget{"macOS", "arm64"}}));
    REQUIRE(save_lock_file(tmp.path / "packages.lock.json", make_lock({
        {"signalsmith-dsp", {"1.2.0", "https://example.com/signalsmith.git", "", "v1.2.0"}},
        {"mpl-analyzer", {"0.9.0", "https://example.com/mpl-analyzer.git", "", "v0.9.0"}},
    })));
    write_file(tmp.path / "cmake" / "pulp-packages.cmake", "stale");

    auto removed = run_in_project(tmp.path, [&] { return cmd_remove({"signalsmith-dsp"}); });
    REQUIRE(removed.exit_code == 0);
    REQUIRE(load_lock_file(tmp.path / "packages.lock.json")
                .packages.count("signalsmith-dsp") == 0);
    auto cmake = read_file(tmp.path / "cmake" / "pulp-packages.cmake");
    REQUIRE(cmake.find("mpl-analyzer") != std::string::npos);
    REQUIRE(cmake.find("signalsmith-dsp") == std::string::npos);
}
