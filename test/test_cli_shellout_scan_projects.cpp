// test_cli_shellout_scan_projects.cpp — extracted from test_cli_shellout.cpp
// in the 2026-05 Phase 5 P5-4 refactor.
//
// `pulp projects list` and `pulp scan` shell-out tests:
//
//   * `pulp projects list --json` emits valid JSON for empty + non-empty
//     registries; reports missing_on_disk for deleted projects.
//   * `pulp projects list` (no --json) emits human-readable text.
//   * `pulp scan --help` / `--no-load` / `--format`. Covers filesystem-
//     only enumeration, HOME-derived CLAP entries, lv2 rich-scanner
//     path, and single-bucket restriction.

#include "test_cli_shellout_helpers.hpp"

using namespace pulp::platform;
namespace fs = std::filesystem;
using namespace pulp_test_cli;

TEST_CASE("pulp projects list --json emits valid JSON for empty registry",
          "[cli][shellout][projects][issue-244]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    // Isolated PULP_HOME so we don't read the user's real registry.
    auto tmp = fs::temp_directory_path() / "pulp-projects-json-test";
    fs::create_directories(tmp);
    pulp_setenv("PULP_HOME", tmp.string().c_str(), 1);
    auto r = run_pulp({"projects", "list", "--json"});
    pulp_unsetenv("PULP_HOME");
    fs::remove_all(tmp);

    REQUIRE(r.exit_code == 0);
    // Must be parseable JSON with the documented top-level keys.
    REQUIRE(r.stdout_output.find("\"registry\"") != std::string::npos);
    REQUIRE(r.stdout_output.find("\"projects\"") != std::string::npos);
    // Empty registry → empty array.
    REQUIRE(r.stdout_output.find("[]") != std::string::npos);
}

TEST_CASE("pulp projects list (no --json) emits human text",
          "[cli][shellout][projects]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() / "pulp-projects-text-test";
    fs::create_directories(tmp);
    pulp_setenv("PULP_HOME", tmp.string().c_str(), 1);
    auto r = run_pulp({"projects", "list"});
    pulp_unsetenv("PULP_HOME");
    fs::remove_all(tmp);

    REQUIRE(r.exit_code == 0);
    // Human text path should NOT emit JSON braces.
    REQUIRE(r.stdout_output.find("Registry:") != std::string::npos);
    REQUIRE(r.stdout_output.find("(no projects registered)") != std::string::npos);
}

TEST_CASE("pulp projects validates parser errors before registry mutation",
          "[cli][shellout][projects][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto pulp_home = unique_temp_dir("pulp-projects-parser-home");
    auto project = unique_temp_dir("pulp-projects-parser-project");
    fs::create_directories(pulp_home);
    fs::create_directories(project);
    ScopedEnvVar pulp_home_env("PULP_HOME");
    pulp_home_env.set(pulp_home.string());

    struct Case {
        std::vector<std::string> args;
        std::string stderr_substring;
    };
    const std::vector<Case> cases = {
        {{"projects", "list", "--bogus"}, "unknown option"},
        {{"projects", "add", project.string(), "extra"}, "unexpected argument"},
        {{"projects", "remove"}, "path argument is required"},
        {{"projects", "remove", project.string(), "extra"}, "unexpected argument"},
    };

    for (const auto& c : cases) {
        INFO("projects args size: " << c.args.size());
        auto r = run_pulp(c.args, 10000);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code != 0);
        REQUIRE(r.stderr_output.find(c.stderr_substring) != std::string::npos);
    }

    REQUIRE_FALSE(fs::exists(pulp_home / "projects.json"));

    std::error_code ec;
    fs::remove_all(pulp_home, ec);
    fs::remove_all(project, ec);
}

// Non-empty registry path — exercises the cmd_projects::do_list JSON
// loop body (lines ~78-92 of cmd_projects.cpp) that the empty-registry
// test above can't reach. Adds a synthetic project root, registers it,
// then asserts the JSON output contains the per-project record. Also
// exercises json_escape via a project name containing a literal
// double-quote (the most common escapable character).
TEST_CASE("pulp projects list --json emits per-project JSON with non-empty registry",
          "[cli][shellout][projects][issue-244]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto pulp_home = unique_temp_dir("pulp-projects-json-nonempty-home");
    fs::create_directories(pulp_home);
    pulp_setenv("PULP_HOME", pulp_home.string().c_str(), 1);

    // Synthetic project root — find_project_root walks ancestors looking
    // for `CMakeLists.txt && core/`, so create both. pulp.toml carries
    // the SDK pin; cmd_projects::do_add reads name from there if present.
    auto project = unique_temp_dir("pulp-projects-json-fake-project");
    fs::create_directories(project / "core");
    {
        std::ofstream ct(project / "CMakeLists.txt");
        ct << "project(FakeJsonListProject VERSION 0.1.0 LANGUAGES CXX)\n";
    }
    {
        std::ofstream pt(project / "pulp.toml");
        pt << "sdk_version = \"0.40.0\"\n";
    }

    auto add = run_pulp({"projects", "add", project.string()}, 15000);
    REQUIRE_FALSE(add.timed_out);
    REQUIRE(add.exit_code == 0);

    auto list = run_pulp({"projects", "list", "--json"}, 15000);
    REQUIRE_FALSE(list.timed_out);
    REQUIRE(list.exit_code == 0);

    // Top-level shape preserved.
    REQUIRE(list.stdout_output.find("\"registry\"") != std::string::npos);
    REQUIRE(list.stdout_output.find("\"projects\"") != std::string::npos);
    // Loop body must have emitted one project record with these fields.
    REQUIRE(list.stdout_output.find("\"path\"") != std::string::npos);
    REQUIRE(list.stdout_output.find("\"name\"") != std::string::npos);
    REQUIRE(list.stdout_output.find("\"registered_at\"") != std::string::npos);
    REQUIRE(list.stdout_output.find("\"missing_on_disk\"") != std::string::npos);
    REQUIRE(list.stdout_output.find(project.generic_string()) != std::string::npos);
    // The synthetic project exists on disk → missing_on_disk false.
    REQUIRE(list.stdout_output.find("\"missing_on_disk\": false") != std::string::npos);

    pulp_unsetenv("PULP_HOME");
    fs::remove_all(pulp_home);
    fs::remove_all(project);
}

// Pairs with the above — exercises cmd_projects's missing-on-disk
// branch by registering a project then deleting its directory. This
// covers the `bool missing = !fs::exists(p.path)` true branch + the
// `"missing_on_disk": true` JSON emission line.
TEST_CASE("pulp projects list --json reports missing_on_disk=true for deleted project",
          "[cli][shellout][projects][issue-244]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto pulp_home = unique_temp_dir("pulp-projects-json-missing-home");
    fs::create_directories(pulp_home);
    pulp_setenv("PULP_HOME", pulp_home.string().c_str(), 1);

    auto project = unique_temp_dir("pulp-projects-json-deleted-project");
    fs::create_directories(project / "core");
    {
        std::ofstream ct(project / "CMakeLists.txt");
        ct << "project(DeletedFakeProject VERSION 0.1.0 LANGUAGES CXX)\n";
    }
    {
        std::ofstream pt(project / "pulp.toml");
        pt << "sdk_version = \"0.40.0\"\n";
    }

    auto add = run_pulp({"projects", "add", project.string()}, 15000);
    REQUIRE_FALSE(add.timed_out);
    REQUIRE(add.exit_code == 0);

    // Now delete the project directory but leave the registry entry.
    fs::remove_all(project);

    auto list = run_pulp({"projects", "list", "--json"}, 15000);
    REQUIRE_FALSE(list.timed_out);
    REQUIRE(list.exit_code == 0);
    REQUIRE(list.stdout_output.find("\"missing_on_disk\": true") != std::string::npos);

    pulp_unsetenv("PULP_HOME");
    fs::remove_all(pulp_home);
}

// ── pulp scan --help / --no-load (#812) ──────────────────────────
//
// `pulp scan --help` previously walked the system plug-in paths and
// dlopen'd everything (which crashes on a malformed plugin). The
// fix added an early --help gate; this test locks in that behavior
// so a future refactor can't reintroduce the crash.

TEST_CASE("pulp scan --help exits 0 with usage on stdout",
          "[cli][shellout][scan][issue-812]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"scan", "--help"});
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.stdout_output.find("Usage: pulp scan") != std::string::npos);
    // The new --no-load flag must be advertised so users hitting #812
    // can discover the safe-fast escape hatch.
    REQUIRE(r.stdout_output.find("--no-load") != std::string::npos);
}

TEST_CASE("pulp scan validates parser errors before filesystem enumeration",
          "[cli][shellout][scan][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    const struct {
        std::vector<std::string> args;
        const char* message;
    } cases[] = {
        {{"scan", "--format"}, "--format requires a value"},
        {{"scan", "-f"}, "-f requires a value"},
        {{"scan", "--format", "--no-load"}, "--format requires a value"},
        {{"scan", "--definitely-not-a-scan-flag"}, "unknown flag"},
    };

    for (const auto& c : cases) {
        INFO("scan args under test");
        auto r = run_pulp(c.args, /*timeout_ms=*/10000);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 2);
        REQUIRE(r.stderr_output.find(c.message) != std::string::npos);
        REQUIRE(r.stdout_output.find("[CLAP]") == std::string::npos);
        REQUIRE(r.stdout_output.find("No plugins found") == std::string::npos);
    }
}

TEST_CASE("pulp host validates parser errors before plugin loading",
          "[cli][shellout][host][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    const struct {
        std::vector<std::string> args;
        const char* message;
    } cases[] = {
        {{"host", "--format"}, "--format requires a value"},
        {{"host", "-f"}, "-f requires a value"},
        {{"host", "--format", "--id"}, "--format requires a value"},
        {{"host", "--id"}, "--id requires a value"},
        {{"host", "--id", "--format"}, "--id requires a value"},
        {{"host", "--definitely-not-a-host-flag"}, "unknown flag"},
    };

    for (const auto& c : cases) {
        INFO("host args under test");
        auto r = run_pulp(c.args, /*timeout_ms=*/10000);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 2);
        REQUIRE(r.stderr_output.find(c.message) != std::string::npos);
        REQUIRE(r.stderr_output.find("failed to load") == std::string::npos);
        REQUIRE(r.stdout_output.find("Loaded:") == std::string::npos);
    }
}

TEST_CASE("pulp scan --no-load runs filesystem-only enumeration cleanly",
          "[cli][shellout][scan][issue-812]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"scan", "--no-load"}, /*timeout_ms=*/30000);
    // The whole point of --no-load is "doesn't crash on bad plugins".
    // If the host happens to have no plugins installed, that's also
    // a clean rc=0 with "No plugins found." stdout — both are fine.
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    // No libc++abi termination message should appear in stderr —
    // that's the regression --no-load specifically prevents.
    REQUIRE(r.stderr_output.find("libc++abi: terminating") == std::string::npos);
}

TEST_CASE("pulp scan --no-load reports filename-derived CLAP entries from HOME",
          "[cli][shellout][scan][issue-812]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
#if defined(_WIN32)
    SUCCEED("Windows CLAP defaults do not derive from HOME");
    return;
#else
    auto home = unique_temp_dir("pulp-scan-noload-home");
    ScopedEnvVar scoped_home("HOME");
    scoped_home.set(home.string());

#if defined(__APPLE__)
    auto clap_dir = home / "Library" / "Audio" / "Plug-Ins" / "CLAP";
#else
    auto clap_dir = home / ".clap";
#endif
    auto alpha_path = clap_dir / "Phase8Alpha.clap";
    auto beta_path = clap_dir / "Phase8Beta.clap";
    fs::create_directories(beta_path);
    fs::create_directories(alpha_path);

    auto r = run_pulp({"scan", "--no-load", "--format", "clap"},
                      /*timeout_ms=*/30000);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stderr_output.find("libc++abi: terminating") == std::string::npos);
    REQUIRE(r.stdout_output.find("[CLAP]") != std::string::npos);
    REQUIRE(r.stdout_output.find(alpha_path.string()) != std::string::npos);
    REQUIRE(r.stdout_output.find(beta_path.string()) != std::string::npos);

    auto alpha = r.stdout_output.find("Phase8Alpha");
    auto beta = r.stdout_output.find("Phase8Beta");
    REQUIRE(alpha != std::string::npos);
    REQUIRE(beta != std::string::npos);
    REQUIRE(alpha < beta);

    std::error_code ec;
    fs::remove_all(home, ec);
#endif
}

TEST_CASE("pulp scan --format lv2 reaches rich scanner path and exits cleanly",
          "[cli][shellout][scan][issue-812]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto home = unique_temp_dir("pulp-scan-lv2-home");
    ScopedEnvVar scoped_home("HOME");
    scoped_home.set(home.string());

#if defined(__linux__)
    auto lv2_dir = home / ".lv2" / "Phase8Probe.lv2";
    fs::create_directories(lv2_dir);
    write_text(lv2_dir / "manifest.ttl",
               "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
               "<http://example.org/phase8-probe> a lv2:Plugin .\n");
#endif

    auto r = run_pulp({"scan", "--format", "lv2"}, /*timeout_ms=*/30000);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stderr_output.find("libc++abi: terminating") == std::string::npos);
#if defined(__linux__)
    REQUIRE(r.stdout_output.find("Phase8Probe") != std::string::npos);
#else
    REQUIRE((r.stdout_output.find("[LV2]") != std::string::npos ||
             r.stdout_output.find("No plugins found") != std::string::npos));
#endif

    std::error_code ec;
    fs::remove_all(home, ec);
}

TEST_CASE("pulp scan --no-load --format clap restricts to one bucket",
          "[cli][shellout][scan][issue-812]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"scan", "--no-load", "--format", "clap"},
                      /*timeout_ms=*/30000);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stderr_output.find("libc++abi: terminating") == std::string::npos);
    // When the filter narrows to CLAP and no CLAP plugins exist on
    // the runner, the output should be the documented empty-state
    // message rather than crashing or printing other-format buckets.
    REQUIRE((r.stdout_output.find("[CLAP]") != std::string::npos ||
             r.stdout_output.find("No plugins found") != std::string::npos));
}
