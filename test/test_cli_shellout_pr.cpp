// test_cli_shellout_pr.cpp — extracted from test_cli_shellout.cpp in
// the 2026-05 Phase 5 P5-4 follow-up refactor.
//
// `pulp pr` shell-out tests — 11 TEST_CASEs spread across two contiguous
// clusters in the parent TU. Covers:
//
//   * pr.workflow validation (manual / github / shipyard delegation)
//   * Shipyard binary pin handling
//   * Native fallback path (no shipyard installed)
//   * pulp #643 install guidance, native mode validation, native help

#include "test_cli_shellout_helpers.hpp"

using namespace pulp::platform;
namespace fs = std::filesystem;
using namespace pulp_test_cli;

TEST_CASE("pulp pr validates workflow selection before shipping",
          "[cli][shellout][pr-workflow]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar home_env("PULP_HOME");
    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    ScopedEnvVar workflow_env("PULP_PR_WORKFLOW");
    ScopedEnvVar path_env("PATH");
    ScopedEnvVar os_home_env("HOME");
    update_disabled.set("1");

    auto home = unique_temp_dir("pulp-pr-workflow-pr");
    fs::create_directories(home);
    home_env.set(home.string());
    os_home_env.set(home.string());

    auto missing_value = run_pulp({"pr", "--workflow"}, 10000);
    REQUIRE_FALSE(missing_value.timed_out);
    REQUIRE(missing_value.exit_code == 2);
    REQUIRE(missing_value.stderr_output.find("--workflow requires a value")
            != std::string::npos);

    workflow_env.set("svn");
    auto invalid_env = run_pulp({"pr", "--dry-run"}, 10000);
    REQUIRE_FALSE(invalid_env.timed_out);
    REQUIRE(invalid_env.exit_code == 2);
    REQUIRE(invalid_env.stderr_output.find("invalid PR workflow 'svn' from env:PULP_PR_WORKFLOW")
            != std::string::npos);
    REQUIRE(invalid_env.stderr_output.find("pr.workflow must be one of")
            != std::string::npos);

    workflow_env.set("");
    auto path_dir = home / "empty-path";
    fs::create_directories(path_dir);
    path_env.set(path_dir.string());
    auto missing_shipyard = run_pulp({"pr", "--workflow", "shipyard"}, 10000);
    REQUIRE_FALSE(missing_shipyard.timed_out);
    REQUIRE(missing_shipyard.exit_code == 2);
    REQUIRE(missing_shipyard.stderr_output.find("shipyard is not on PATH")
            != std::string::npos);
    REQUIRE(missing_shipyard.stderr_output.find("pulp config set pr.workflow github")
            != std::string::npos);

    fs::remove_all(home);
}

TEST_CASE("pulp pr manual and github workflows avoid Shipyard mutation",
          "[cli][shellout][pr-workflow]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar home_env("PULP_HOME");
    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    ScopedEnvVar workflow_env("PULP_PR_WORKFLOW");
    update_disabled.set("1");

    auto home = unique_temp_dir("pulp-pr-workflow-manual-github");
    fs::create_directories(home);
    home_env.set(home.string());

    auto manual = run_pulp({"pr", "--workflow=manual", "--base", "origin/main",
                            "--title", "Manual PR Plan"}, 10000);
    REQUIRE_FALSE(manual.timed_out);
    REQUIRE(manual.exit_code == 0);
    REQUIRE(manual.stdout_output.find("manual PR workflow selected")
            != std::string::npos);
    REQUIRE(manual.stdout_output.find("gh pr create --title")
            != std::string::npos);
    REQUIRE(manual.stdout_output.find("Manual and GitHub workflows do not create Shipyard tracking state")
            != std::string::npos);

    auto github_dry_run = run_pulp({"pr", "--workflow", "github", "--dry-run",
                                    "--title", "GitHub PR Plan"}, 10000);
    REQUIRE_FALSE(github_dry_run.timed_out);
    REQUIRE(github_dry_run.exit_code == 0);
    REQUIRE(github_dry_run.stderr_output.find("using github workflow via `gh`")
            != std::string::npos);
    REQUIRE(github_dry_run.stdout_output.find("[dry-run] Plan:")
            != std::string::npos);
    REQUIRE(github_dry_run.stdout_output.find("shipyard ship")
            == std::string::npos);

    fs::remove_all(home);
}

TEST_CASE("pulp pr github workflow requires gh for real PR creation",
          "[cli][shellout][pr-workflow]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar home_env("PULP_HOME");
    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    ScopedEnvVar path_env("PATH");
    ScopedEnvVar os_home_env("HOME");
    update_disabled.set("1");

    auto home = unique_temp_dir("pulp-pr-workflow-gh-missing");
    auto path_dir = home / "empty-path";
    fs::create_directories(path_dir);
    home_env.set(home.string());
    os_home_env.set(home.string());
    path_env.set(path_dir.string());

    auto missing_gh = run_pulp({"pr", "--workflow", "github"}, 10000);
    fs::remove_all(home);

    REQUIRE_FALSE(missing_gh.timed_out);
    REQUIRE(missing_gh.exit_code == 2);
    REQUIRE(missing_gh.stderr_output.find("PR workflow is `github`, but the GitHub CLI (`gh`) is not on PATH")
            != std::string::npos);
    REQUIRE(missing_gh.stderr_output.find("`github` is Pulp's direct GitHub workflow name")
            != std::string::npos);
}

#if !defined(_WIN32)
TEST_CASE("pulp pr delegates shipyard workflow when the pinned binary is present",
          "[cli][shellout][pr-workflow]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar home_env("PULP_HOME");
    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    ScopedEnvVar path_env("PATH");
    ScopedEnvVar os_home_env("HOME");
    update_disabled.set("1");

    auto home = unique_temp_dir("pulp-pr-workflow-shipyard");
    auto bin_dir = home / "bin";
    fs::create_directories(bin_dir);
    home_env.set(home.string());
    os_home_env.set(home.string());

    const std::string pinned = pinned_shipyard_version_for_test();
    write_fake_shipyard(bin_dir, pinned);
    prepend_to_path(bin_dir);

    auto delegated = run_pulp({"pr", "--help"}, 10000);
    fs::remove_all(home);

    REQUIRE_FALSE(delegated.timed_out);
    REQUIRE(delegated.exit_code == 0);
    REQUIRE(delegated.stdout_output.find("Usage: pulp pr [--native]")
            != std::string::npos);
    REQUIRE(delegated.stdout_output.find("fake shipyard pr --help")
            != std::string::npos);
}

TEST_CASE("pulp status reports shipyard version and pin health",
          "[cli][shellout][pr-workflow]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar home_env("PULP_HOME");
    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    ScopedEnvVar path_env("PATH");
    ScopedEnvVar os_home_env("HOME");
    update_disabled.set("1");

    auto home = unique_temp_dir("pulp-pr-workflow-status-shipyard");
    auto bin_dir = home / "bin";
    fs::create_directories(bin_dir);
    home_env.set(home.string());
    os_home_env.set(home.string());

    const std::string pinned = pinned_shipyard_version_for_test();
    auto shipyard = write_fake_shipyard(bin_dir, pinned);
    prepend_to_path(bin_dir);

    auto status = run_pulp({"status"}, 10000);
    fs::remove_all(home);

    REQUIRE_FALSE(status.timed_out);
    REQUIRE(status.exit_code == 0);
    REQUIRE(status.stdout_output.find("PR workflow: shipyard (default)")
            != std::string::npos);
    REQUIRE(status.stdout_output.find("Shipyard: " + shipyard.string())
            != std::string::npos);
    REQUIRE(status.stdout_output.find("(" + pinned + ") pinned " + pinned)
            != std::string::npos);
}
#endif

TEST_CASE("pulp pr without shipyard prints install guidance",
          "[cli][shellout][pr][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar path("PATH");
    path.set("");

    auto r = run_pulp({"pr"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 2);

    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("shipyard is not on PATH") != std::string::npos);
    REQUIRE(combined.find("./tools/install-shipyard.sh") != std::string::npos);
    REQUIRE(combined.find("pulp pr --native") != std::string::npos);
}

TEST_CASE("pulp pr github workflow requires gh instead of falling back from shipyard",
          "[cli][shellout][pr][pr-workflow]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar path("PATH");
    ScopedEnvVar workflow("PULP_PR_WORKFLOW");
    path.set("");
    workflow.set("github");

    auto r = run_pulp({"pr"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 2);

    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("GitHub CLI (`gh`) is not on PATH") != std::string::npos);
    REQUIRE(combined.find("shipyard is not on PATH") == std::string::npos);
}

TEST_CASE("pulp pr manual workflow does not require shipyard",
          "[cli][shellout][pr][pr-workflow]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar path("PATH");
    path.set("");

    auto r = run_pulp({"pr", "--workflow", "manual", "--help"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("Usage: pulp pr [options]") != std::string::npos);
    REQUIRE(r.stderr_output.find("shipyard is not on PATH") == std::string::npos);
}

TEST_CASE("pulp pr native help stays available without shipyard",
          "[cli][shellout][pr][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar path("PATH");
    path.set("");

    auto r = run_pulp({"pr", "--native", "--help"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("Usage: pulp pr [options]") != std::string::npos);
    REQUIRE(r.stdout_output.find("--no-ship") != std::string::npos);
    REQUIRE(r.stdout_output.find("--dry-run") != std::string::npos);
}

TEST_CASE("pulp pr native validates option values before checkout lookup",
          "[cli][shellout][pr][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(fs::temp_directory_path());

    for (const auto& c : std::vector<std::pair<std::vector<std::string>, std::string>>{
             {{"pr", "--native", "--base"}, "--base requires a value"},
             {{"pr", "--native", "--base", "--dry-run"}, "--base requires a value"},
             {{"pr", "--native", "--title"}, "--title requires a value"},
             {{"pr", "--native", "--title", "--no-push"}, "--title requires a value"},
         }) {
        auto r = exec(bin.string(), c.first, 10000);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 2);
        REQUIRE(r.stderr_output.find(c.second) != std::string::npos);
        REQUIRE(r.stderr_output.find("not inside a pulp project") == std::string::npos);
    }

    fs::current_path(cwd_saver);
}

TEST_CASE("pulp pr native mode refuses to run outside a project",
          "[cli][shellout][pr][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar path("PATH");
    path.set("");

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(fs::temp_directory_path());
    auto r = exec(bin.string(), {"pr", "--native", "--no-push"}, 10000);
    fs::current_path(cwd_saver);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.stderr_output.find("not inside a pulp project") != std::string::npos);
}

#if !defined(_WIN32)
TEST_CASE("pulp pr delegates to shipyard with forwarded arguments",
          "[cli][shellout][pr][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto fake_dir = unique_temp_dir("pulp-pr-fake-shipyard");
    fs::create_directories(fake_dir);
    auto fake_shipyard = fake_dir / "shipyard";
    // Report the version the pulp CLI's skew check expects (tools/shipyard.toml
    // pin), stripped of the leading 'v' since that's what `shipyard --version`
    // emits in practice. Reading dynamically keeps the test honest across pin
    // bumps — the old hardcoded "0.46.0" broke on the v0.56.2 bump.
    std::string version = pinned_shipyard_version_for_test();
    if (!version.empty() && version[0] == 'v') version.erase(0, 1);
    {
        std::ofstream f(fake_shipyard);
        f << "#!/bin/sh\n"
             "if [ \"$1\" = \"--version\" ]; then\n"
             "  echo \"shipyard, version "
          << version
          << "\"\n"
             "  exit 0\n"
             "fi\n"
             "printf 'fake shipyard args:'\n"
             "for arg in \"$@\"; do printf ' [%s]' \"$arg\"; done\n"
             "printf '\\n'\n"
             "exit 17\n";
    }
    std::error_code ec;
    fs::permissions(fake_shipyard,
                    fs::perms::owner_read | fs::perms::owner_exec |
                    fs::perms::group_read | fs::perms::group_exec,
                    fs::perm_options::add,
                    ec);
    REQUIRE_FALSE(ec);

    ScopedEnvVar path("PATH");
    path.set(fake_dir.string());

    auto r = run_pulp({"pr", "--help", "--no-ship"});
    fs::remove_all(fake_dir);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 17);
    REQUIRE(r.stdout_output.find("Usage: pulp pr [--native]") != std::string::npos);
    REQUIRE(r.stdout_output.find("fake shipyard args: [pr] [--help] [--no-ship]")
            != std::string::npos);
}
#endif
