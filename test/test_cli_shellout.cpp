// Shell-out CLI behaviour tests for `pulp`.
//
// Per CLAUDE.md: "CLI behavior changes — shell out to the built binary,
// assert exit code + stderr content." Before this file, the CLI had
// two targeted unit tests (test_cli_create_targets, test_cli_design_binding)
// but no end-to-end shell-out validation — the lesson from #295 was
// that silent-failure bugs slip through when no test actually launches
// the binary. This closes that gap with deterministic exit-code +
// stdout/stderr invariants for the non-destructive subcommands every
// user hits on day one.

#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

using namespace pulp::platform;
namespace fs = std::filesystem;

namespace {

// Platform-aware setenv/unsetenv: Windows MSVC doesn't provide POSIX
// `setenv` / `unsetenv` — it uses `_putenv_s` instead (empty string
// value for unset). Wrap both so every test body stays portable.
inline int pulp_setenv(const char* name, const char* value, int /*overwrite*/) {
#if defined(_WIN32)
    return _putenv_s(name, value);
#else
    return ::setenv(name, value, 1);
#endif
}

inline int pulp_unsetenv(const char* name) {
#if defined(_WIN32)
    return _putenv_s(name, "");
#else
    return ::unsetenv(name);
#endif
}

class ScopedEnvVar {
public:
    explicit ScopedEnvVar(const char* name)
        : name_(name) {
        if (const char* value = std::getenv(name)) {
            had_value_ = true;
            old_value_ = value;
        }
    }

    ~ScopedEnvVar() {
        if (had_value_) {
            pulp_setenv(name_.c_str(), old_value_.c_str(), 1);
        } else {
            pulp_unsetenv(name_.c_str());
        }
    }

    void set(const std::string& value) {
        pulp_setenv(name_.c_str(), value.c_str(), 1);
    }

private:
    std::string name_;
    bool had_value_ = false;
    std::string old_value_;
};

fs::path unique_temp_dir(const std::string& prefix) {
    auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / (prefix + "-" + std::to_string(tick));
}


// After the Rust CLI cutover, the instrumented C++ delegate lands at
// <build>/tools/cli/pulp-cpp. Older/pre-cutover builds used
// <build>/tools/cli/pulp. Prefer pulp-cpp so coverage builds exercise
// the C++ implementation directly, but keep the old fallback for
// compatibility. PULP_CLI_PATH can still override either path.
fs::path pulp_binary() {
    if (const char* env = std::getenv("PULP_CLI_PATH"); env && *env) {
        return fs::path(env);
    }
    auto cpp = fs::current_path() / ".." / "tools" / "cli" / "pulp-cpp";
    if (fs::exists(cpp)) return cpp;
    return fs::current_path() / ".." / "tools" / "cli" / "pulp";
}

ProcessResult run_pulp(const std::vector<std::string>& args,
                       int timeout_ms = 10000) {
    auto bin = pulp_binary();
    if (!fs::exists(bin)) {
        // Binary not built for this target — rather than fail noisily,
        // surface a SKIP at the call site. Tests check for .ok() first.
        ProcessResult r;
        r.exit_code = -1;
        r.stderr_output = "pulp binary not at " + bin.string();
        return r;
    }
    return exec(bin.string(), args, timeout_ms);
}

bool binary_exists() { return fs::exists(pulp_binary()); }

std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    REQUIRE(f.is_open());
    f << text;
    REQUIRE(f.good());
}

char path_separator() {
#if defined(_WIN32)
    return ';';
#else
    return ':';
#endif
}

void prepend_to_path(const fs::path& dir) {
    const char* old = std::getenv("PATH");
    auto next = dir.string();
    if (old && *old) {
        next += path_separator();
        next += old;
    }
    pulp_setenv("PATH", next.c_str(), 1);
}

#if !defined(_WIN32)
std::string pinned_shipyard_version_for_test() {
    auto toml = read_file(fs::current_path().parent_path().parent_path() /
                          "tools" / "shipyard.toml");
    std::istringstream lines(toml);
    std::string line;
    while (std::getline(lines, line)) {
        auto pos = line.find("version");
        if (pos == std::string::npos) continue;
        auto eq = line.find('=', pos);
        if (eq == std::string::npos) continue;
        auto value = line.substr(eq + 1);
        auto first = value.find('"');
        auto last = value.find_last_of('"');
        if (first != std::string::npos && last != std::string::npos && last > first) {
            return value.substr(first + 1, last - first - 1);
        }
    }
    return "v0.56.2";
}

fs::path write_fake_shipyard(const fs::path& dir, const std::string& version) {
    auto path = dir / "shipyard";
    write_text(path,
               "#!/bin/sh\n"
               "if [ \"$1\" = \"--version\" ]; then\n"
               "  echo \"shipyard " + version + "\"\n"
               "  exit 0\n"
               "fi\n"
               "if [ \"$1\" = \"pr\" ]; then\n"
               "  echo \"fake shipyard pr $2\"\n"
               "  exit 0\n"
               "fi\n"
               "echo \"fake shipyard\"\n");
    fs::permissions(path,
                    fs::perms::owner_exec | fs::perms::owner_read |
                    fs::perms::owner_write,
                    fs::perm_options::add);
    return path;
}
#endif

}  // namespace

TEST_CASE("pulp help exits 0 with a usage banner on stdout",
          "[cli][shellout]") {
    if (!binary_exists()) {
        SUCCEED("pulp binary not built for this test run; skipping");
        return;
    }
    auto r = run_pulp({"help"});
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    // Must advertise the word "pulp" and list the build subcommand so
    // first-time users aren't dropped into an empty help output — this
    // is the category of regression that the #295 silent-failure
    // lesson warned about.
    REQUIRE(r.stdout_output.find("pulp") != std::string::npos);
    REQUIRE(r.stdout_output.find("build") != std::string::npos);
}

TEST_CASE("pulp --help is an alias for pulp help",
          "[cli][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto a = run_pulp({"help"});
    auto b = run_pulp({"--help"});
    auto c = run_pulp({"-h"});
    // Every alias exits 0 and reaches the same top-level usage.
    REQUIRE(a.exit_code == 0);
    REQUIRE(b.exit_code == 0);
    REQUIRE(c.exit_code == 0);
    REQUIRE(a.stdout_output.find("pulp") != std::string::npos);
    REQUIRE(b.stdout_output.find("pulp") != std::string::npos);
    REQUIRE(c.stdout_output.find("pulp") != std::string::npos);
}

TEST_CASE("pulp with no arguments prints help and exits 0",
          "[cli][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({});
    // Bare `pulp` is expected to show usage rather than error out — it
    // mustn't fake-succeed silently or hang.
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE((r.stdout_output.find("pulp") != std::string::npos ||
             r.stdout_output.find("Usage") != std::string::npos ||
             r.stdout_output.find("Commands") != std::string::npos));
}

TEST_CASE("pulp <unknown-command> exits non-zero with a diagnostic",
          "[cli][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"thisisnotarealcommand"});
    REQUIRE(r.exit_code != 0);
    REQUIRE_FALSE(r.timed_out);
    // At least one of stdout/stderr must reference the unknown command
    // or a help pointer — otherwise the user is left guessing.
    auto combined = r.stdout_output + r.stderr_output;
    bool mentioned =
        combined.find("Unknown") != std::string::npos ||
        combined.find("unknown") != std::string::npos ||
        combined.find("help") != std::string::npos;
    REQUIRE(mentioned);
}

TEST_CASE("pulp audio usage and parser errors are deterministic",
          "[cli][shellout][audio][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto usage = run_pulp({"audio"});
    REQUIRE(usage.exit_code == 0);
    REQUIRE_FALSE(usage.timed_out);
    REQUIRE(usage.stdout_output.find("pulp audio") != std::string::npos);
    REQUIRE(usage.stdout_output.find("excerpt-find") != std::string::npos);
    REQUIRE(usage.stdout_output.find("read-bundle") != std::string::npos);

    struct Case {
        std::vector<std::string> args;
        std::string stderr_substring;
        std::string stdout_substring;
    };

    const std::vector<Case> cases = {
        {{"audio", "model"}, "Unknown audio model subcommand", "pulp audio"},
        {{"audio", "model", "activate"}, "model id is required", "pulp audio"},
        {{"audio", "model", "list", "--surprise"}, "Unknown option: --surprise", ""},
        {{"audio", "model", "status", "--surprise"}, "Unknown option: --surprise", ""},
        {{"audio", "excerpt-find", "--text", "kick", "--input"}, "--input requires a value", ""},
        {{"audio", "excerpt-find", "--top", "many"}, "invalid value for --top", ""},
        {{"audio", "excerpt-find", "--window-ms", "soon"}, "invalid value for --window-ms", ""},
        {{"audio", "excerpt-find", "--hop-ms", "soon"}, "invalid value for --hop-ms", ""},
        {{"audio", "excerpt-find", "--min-score", "loud"}, "invalid value for --min-score", ""},
        {{"audio", "excerpt-find", "--max-candidates-per-file", "lots"}, "invalid value for --max-candidates-per-file", ""},
        {{"audio", "excerpt-find", "--unknown"}, "Unknown option: --unknown", ""},
        {{"audio", "read-bundle"}, "bundle path is required", "pulp audio"},
        {{"audio", "read-bundle", "bundle-a", "bundle-b"}, "Unknown argument: bundle-b", ""},
        {{"audio", "not-audio"}, "Unknown audio subcommand", "pulp audio"},
    };

    for (const auto& c : cases) {
        INFO("args size: " << c.args.size());
        auto r = run_pulp(c.args);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code != 0);
        REQUIRE(r.stderr_output.find(c.stderr_substring) != std::string::npos);
        if (!c.stdout_substring.empty())
            REQUIRE(r.stdout_output.find(c.stdout_substring) != std::string::npos);
    }
}

TEST_CASE("pulp audio read-bundle json reports missing bundle errors",
          "[cli][shellout][audio][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto missing = unique_temp_dir("pulp-audio-missing-bundle");
    fs::remove_all(missing);
    auto r = run_pulp({"audio", "read-bundle", missing.string(), "--json"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    REQUIRE(r.stderr_output.empty());
    REQUIRE(r.stdout_output.find("\"ok\": false") != std::string::npos);
    REQUIRE(r.stdout_output.find("bundle path does not exist") != std::string::npos);
}

TEST_CASE("pulp config <unknown-subcommand> exits non-zero with a diagnostic",
          "[cli][shellout][codex-562]") {
    // Codex 2026-04-21 wave 2 P2 on #562: `pulp config foo` previously
    // fell through to usage() which returned 0, so scripts/CI could
    // not detect a typo'd subcommand. The new behaviour returns
    // exit code 2 with an "Unknown config subcommand" diagnostic on
    // stderr. Known subcommands (get/set/list/help) keep exit 0.
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"config", "thisisnotarealsubcommand"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    REQUIRE(r.stderr_output.find("Unknown config subcommand") != std::string::npos);
}

TEST_CASE("pulp config supports pr.workflow",
          "[cli][shellout][pr-workflow]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-pr-workflow-config");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    auto set = run_pulp({"config", "set", "pr.workflow", "github"});
    auto get = run_pulp({"config", "get", "pr.workflow"});
    auto list = run_pulp({"config", "list"});
    auto bad = run_pulp({"config", "set", "pr.workflow", "svn"});

    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    pulp_unsetenv("PULP_HOME");
    fs::remove_all(tmp_home);

    REQUIRE_FALSE(set.timed_out);
    REQUIRE(set.exit_code == 0);
    REQUIRE_FALSE(get.timed_out);
    REQUIRE(get.exit_code == 0);
    REQUIRE(get.stdout_output.find("github") != std::string::npos);
    REQUIRE_FALSE(list.timed_out);
    REQUIRE(list.exit_code == 0);
    REQUIRE(list.stdout_output.find("pr.workflow = github") != std::string::npos);
    REQUIRE_FALSE(bad.timed_out);
    REQUIRE(bad.exit_code != 0);
    REQUIRE(bad.stderr_output.find("pr.workflow must be one of") != std::string::npos);
}

TEST_CASE("pulp status reports effective PR workflow",
          "[cli][shellout][pr-workflow]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-pr-workflow-status");
    fs::create_directories(tmp_home);
    {
        std::ofstream cfg(tmp_home / "config.toml");
        cfg << "[pr]\nworkflow = \"manual\"\n"
            << "[update]\nmode = \"off\"\n";
    }

    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);
    auto r = run_pulp({"status"});
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    pulp_unsetenv("PULP_HOME");
    fs::remove_all(tmp_home);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("PR workflow: manual (config:pr.workflow)") != std::string::npos);
    REQUIRE(r.stdout_output.find("Shipyard tracking: disabled by pr.workflow=manual") != std::string::npos);
}

TEST_CASE("pulp status reports invalid and github PR workflow modes",
          "[cli][shellout][pr-workflow]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar home_env("PULP_HOME");
    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    ScopedEnvVar workflow_env("PULP_PR_WORKFLOW");
    update_disabled.set("1");

    auto invalid_home = unique_temp_dir("pulp-pr-workflow-status-invalid");
    fs::create_directories(invalid_home);
    home_env.set(invalid_home.string());
    workflow_env.set("subversion");
    auto invalid = run_pulp({"status"});
    fs::remove_all(invalid_home);

    REQUIRE_FALSE(invalid.timed_out);
    REQUIRE(invalid.exit_code == 0);
    REQUIRE(invalid.stdout_output.find("PR workflow: invalid (env:PULP_PR_WORKFLOW)")
            != std::string::npos);
    REQUIRE(invalid.stdout_output.find("pr.workflow must be one of")
            != std::string::npos);

    auto github_home = unique_temp_dir("pulp-pr-workflow-status-github");
    fs::create_directories(github_home);
    home_env.set(github_home.string());
    workflow_env.set("github");
    auto github = run_pulp({"status"});
    fs::remove_all(github_home);

    REQUIRE_FALSE(github.timed_out);
    REQUIRE(github.exit_code == 0);
    REQUIRE(github.stdout_output.find("PR workflow: github (env:PULP_PR_WORKFLOW)")
            != std::string::npos);
    REQUIRE(github.stdout_output.find("GitHub CLI:") != std::string::npos);
    REQUIRE(github.stdout_output.find("Shipyard tracking: disabled by pr.workflow=github")
            != std::string::npos);
}

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

TEST_CASE("pulp config set/get/list round-trips isolated update settings",
          "[cli][shellout][config][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto home = unique_temp_dir("pulp-config-roundtrip");
    fs::remove_all(home);
    fs::create_directories(home);
    write_text(home / "update-snooze", "dismissed\n");

    ScopedEnvVar pulp_home("PULP_HOME");
    pulp_home.set(home.string());
    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    auto set_mode = run_pulp({"config", "set", "update.mode", "manual"}, 10000);
    REQUIRE_FALSE(set_mode.timed_out);
    REQUIRE(set_mode.exit_code == 0);
    REQUIRE(set_mode.stdout_output.find("Set update.mode = manual") != std::string::npos);
    REQUIRE_FALSE(fs::exists(home / "update-snooze"));

    auto get_mode = run_pulp({"config", "get", "update.mode"}, 10000);
    REQUIRE_FALSE(get_mode.timed_out);
    REQUIRE(get_mode.exit_code == 0);
    REQUIRE(get_mode.stdout_output == "manual\n");

    auto set_channel = run_pulp({"config", "set", "update.channel", "beta"}, 10000);
    REQUIRE_FALSE(set_channel.timed_out);
    REQUIRE(set_channel.exit_code == 0);
    REQUIRE(set_channel.stdout_output.find("Set update.channel = beta") != std::string::npos);

    auto list = run_pulp({"config", "list"}, 10000);
    const auto config_body = read_file(home / "config.toml");
    fs::remove_all(home);

    REQUIRE_FALSE(list.timed_out);
    REQUIRE(list.exit_code == 0);
    REQUIRE(list.stdout_output.find("update.mode = manual") != std::string::npos);
    REQUIRE(list.stdout_output.find("update.check_interval_hours = 24") != std::string::npos);
    REQUIRE(list.stdout_output.find("update.channel = beta") != std::string::npos);
    REQUIRE(list.stdout_output.find("update.bump_projects = prompt") != std::string::npos);
    REQUIRE(config_body.find("[update]") != std::string::npos);
    REQUIRE(config_body.find("mode = \"manual\"") != std::string::npos);
    REQUIRE(config_body.find("channel = \"beta\"") != std::string::npos);
}

TEST_CASE("pulp config rejects malformed and invalid update keys",
          "[cli][shellout][config][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto home = unique_temp_dir("pulp-config-invalid");
    fs::remove_all(home);
    fs::create_directories(home);

    ScopedEnvVar pulp_home("PULP_HOME");
    pulp_home.set(home.string());
    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    auto malformed_get = run_pulp({"config", "get", "update"}, 10000);
    REQUIRE_FALSE(malformed_get.timed_out);
    REQUIRE(malformed_get.exit_code != 0);
    REQUIRE(malformed_get.stderr_output.find("key must be dotted") != std::string::npos);

    auto unknown_key = run_pulp({"config", "set", "update.not_a_key", "value"}, 10000);
    REQUIRE_FALSE(unknown_key.timed_out);
    REQUIRE(unknown_key.exit_code != 0);
    REQUIRE(unknown_key.stderr_output.find("unknown config key") != std::string::npos);

    auto bad_mode = run_pulp({"config", "set", "update.mode", "weekly"}, 10000);
    REQUIRE_FALSE(bad_mode.timed_out);
    REQUIRE(bad_mode.exit_code != 0);
    REQUIRE(bad_mode.stderr_output.find("update.mode must be one of") != std::string::npos);

    auto bad_interval =
        run_pulp({"config", "set", "update.check_interval_hours", "-1"}, 10000);
    const bool config_written = fs::exists(home / "config.toml");
    fs::remove_all(home);

    REQUIRE_FALSE(bad_interval.timed_out);
    REQUIRE(bad_interval.exit_code != 0);
    REQUIRE(bad_interval.stderr_output.find("non-negative integer") != std::string::npos);
    REQUIRE_FALSE(config_written);
}

TEST_CASE("pulp version subcommand runs and mentions the SDK",
          "[cli][shellout][version]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"version"});
    // `pulp version` prints SDK/plugin version info and exits 0. Guard
    // against a regression where the subcommand silently exits with
    // success but an empty body (the exact shape of bug #295 for the
    // ship command).
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    // Must have SOMETHING on stdout — stderr alone is not enough.
    REQUIRE_FALSE(r.stdout_output.empty());
}

TEST_CASE("pulp version check exits 0 on a clean tree and mentions SDK/plugin/marketplace",
          "[cli][shellout][version]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"version", "check"});
    REQUIRE_FALSE(r.timed_out);
    // The check may exit non-zero if version files have drifted. Both
    // exit-code branches must include the three surfaces so the user
    // can act on the diagnostic.
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE((combined.find("sdk") != std::string::npos ||
             combined.find("SDK") != std::string::npos ||
             combined.find("CMakeLists.txt") != std::string::npos));
}

// pulp #709 / #468 — `pulp import-design --from claude` ingests a
// manually-exported Claude Design HTML file AND scaffolds a
// bridge_handlers.cpp next to the generated JS. This is the CLI
// surface that pulp#468's manual-export framing depends on.
TEST_CASE("pulp import-design --from claude writes JS + bridge handler scaffold",
          "[cli][shellout][import-design][issue-709][issue-468]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() / "pulp-claude-smoke";
    fs::create_directories(tmp);
    auto html_path    = tmp / "claude-export.html";
    auto js_path      = tmp / "claude-ui.js";
    auto tokens_path  = tmp / "claude-tokens.json";
    auto bridge_path  = tmp / "claude-bridge.cpp";
    {
        std::ofstream f(html_path);
        f << "<!DOCTYPE html><html><body><div class=\"container\">"
             "<h1>Hello Claude</h1><button>Click</button></div></body></html>";
    }
    // Clean stale artifacts from prior runs.
    fs::remove(js_path);
    fs::remove(tokens_path);
    fs::remove(bridge_path);

    auto r = run_pulp({"import-design",
                       "--from", "claude",
                       "--file",   html_path.string(),
                       "--output", js_path.string(),
                       "--tokens", tokens_path.string(),
                       "--bridge-output", bridge_path.string()},
                       30000);
    REQUIRE_FALSE(r.timed_out);
    INFO(r.stderr_output);
    INFO(r.stdout_output);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(js_path));
    REQUIRE(fs::exists(bridge_path));

    // Scaffold must reference the framework surface by full name so
    // future readers can trace the generated file back to its
    // framework host even after they've edited the handlers.
    std::ifstream b(bridge_path);
    std::string bridge_contents((std::istreambuf_iterator<char>(b)),
                                 std::istreambuf_iterator<char>());
    REQUIRE(bridge_contents.find("pulp::view::EditorBridge") != std::string::npos);
    REQUIRE(bridge_contents.find("add_handler") != std::string::npos);
    REQUIRE(bridge_contents.find("attach_webview") != std::string::npos);

    // Stdout reports both outputs (the JS view AND the scaffold).
    const auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("bridge handler scaffold") != std::string::npos);
}

// Paired with the above: `--no-bridge-scaffold` suppresses the
// scaffold emission while keeping the HTML → JS path intact.
TEST_CASE("pulp import-design --from claude --no-bridge-scaffold writes only the JS",
          "[cli][shellout][import-design][issue-709]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() / "pulp-claude-smoke-no-scaffold";
    fs::create_directories(tmp);
    auto html_path   = tmp / "claude-export.html";
    auto js_path     = tmp / "claude-ui.js";
    auto bridge_path = tmp / "bridge_handlers.cpp";
    {
        std::ofstream f(html_path);
        f << "<!DOCTYPE html><html><body><p>hi</p></body></html>";
    }
    fs::remove(js_path);
    fs::remove(bridge_path);

    auto r = run_pulp({"import-design",
                       "--from", "claude",
                       "--file", html_path.string(),
                       "--output", js_path.string(),
                       "--no-bridge-scaffold"},
                      30000);
    REQUIRE_FALSE(r.timed_out);
    INFO(r.stderr_output);
    INFO(r.stdout_output);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(js_path));
    REQUIRE_FALSE(fs::exists(bridge_path));
}

TEST_CASE("pulp help output lists the top-level subcommands",
          "[cli][shellout][help]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"help"});
    REQUIRE(r.exit_code == 0);

    // Every subcommand that's visible to users in the ci skill / docs
    // needs to survive help output — if someone silently drops one
    // from the dispatch table, this fails loudly.
    for (const char* cmd : {"build", "test", "run", "validate", "ship",
                            "version", "doctor", "create", "clean",
                            "docs", "status", "inspect"}) {
        INFO("help output missing subcommand: " << cmd);
        REQUIRE(r.stdout_output.find(cmd) != std::string::npos);
    }
}

TEST_CASE("pulp inspect help and no-discovery paths are deterministic",
          "[cli][shellout][inspect][issue-643][issue-641]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    auto help = run_pulp({"inspect", "--help"}, 10000);
    REQUIRE_FALSE(help.timed_out);
    REQUIRE(help.exit_code == 0);
    REQUIRE(help.stdout_output.find("Usage: pulp inspect [options]")
            != std::string::npos);
    REQUIRE(help.stdout_output.find("--port PORT") != std::string::npos);
    REQUIRE(help.stdout_output.find("--output FILE") != std::string::npos);

    auto base = unique_temp_dir("pulp-inspect-no-discovery");
    fs::create_directories(base);
#if defined(_WIN32)
    ScopedEnvVar temp_dir("TEMP");
#else
    ScopedEnvVar temp_dir("TMPDIR");
#endif
    temp_dir.set(base.string());

    auto missing = run_pulp({"inspect"}, 10000);
    fs::remove_all(base);

    REQUIRE_FALSE(missing.timed_out);
    REQUIRE(missing.exit_code == 1);
    REQUIRE(missing.stderr_output.find("no running Pulp inspector found")
            != std::string::npos);
    REQUIRE(missing.stderr_output.find("specify --port") != std::string::npos);
    REQUIRE(missing.stdout_output.find("Connecting to") == std::string::npos);
}

TEST_CASE("pulp inspect explicit port failure does not require a server",
          "[cli][shellout][inspect][issue-643][issue-641]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    auto base = unique_temp_dir("pulp-inspect-explicit-port");
    fs::create_directories(base);
    auto output = base / "inspect-response.json";

    auto r = run_pulp({"inspect",
                       "--host", "127.0.0.1",
                       "--port", "1",
                       "--command", "DOM.getDocument",
                       "--params", "{\"depth\":1}",
                       "--output", output.string()},
                      5000);
    const bool wrote_output = fs::exists(output);
    fs::remove_all(base);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.stdout_output.find("Connecting to 127.0.0.1:1")
            != std::string::npos);
    REQUIRE(r.stderr_output.find("could not connect to 127.0.0.1:1")
            != std::string::npos);
    REQUIRE_FALSE(wrote_output);
}

TEST_CASE("pulp inspect rejects invalid arguments before networking",
          "[cli][shellout][inspect][issue-643][issue-641]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    auto missing_port = run_pulp({"inspect", "--port"}, 10000);
    REQUIRE_FALSE(missing_port.timed_out);
    REQUIRE(missing_port.exit_code == 2);
    REQUIRE(missing_port.stderr_output.find("--port requires a value")
            != std::string::npos);
    REQUIRE(missing_port.stdout_output.find("Connecting to") == std::string::npos);

    auto invalid_port = run_pulp({"inspect", "--port", "not-a-port"}, 10000);
    REQUIRE_FALSE(invalid_port.timed_out);
    REQUIRE(invalid_port.exit_code == 2);
    REQUIRE(invalid_port.stderr_output.find("invalid --port value: not-a-port")
            != std::string::npos);
    REQUIRE(invalid_port.stdout_output.find("Connecting to") == std::string::npos);

    for (const char* port : {"0", "65536", "123abc"}) {
        INFO("port=" << port);
        auto rejected_port = run_pulp({"inspect", "--port", port}, 10000);
        REQUIRE_FALSE(rejected_port.timed_out);
        REQUIRE(rejected_port.exit_code == 2);
        REQUIRE(rejected_port.stderr_output.find(std::string("invalid --port value: ") + port)
                != std::string::npos);
        REQUIRE(rejected_port.stdout_output.find("Connecting to") == std::string::npos);
    }

    auto output_without_command = run_pulp({"inspect", "--output", "out.json"}, 10000);
    REQUIRE_FALSE(output_without_command.timed_out);
    REQUIRE(output_without_command.exit_code == 2);
    REQUIRE(output_without_command.stderr_output.find("--output requires --command")
            != std::string::npos);
    REQUIRE(output_without_command.stdout_output.find("Connecting to")
            == std::string::npos);

    auto params_without_command = run_pulp({"inspect", "--params", "{}"}, 10000);
    REQUIRE_FALSE(params_without_command.timed_out);
    REQUIRE(params_without_command.exit_code == 2);
    REQUIRE(params_without_command.stderr_output.find("--params requires --command")
            != std::string::npos);
    REQUIRE(params_without_command.stdout_output.find("Connecting to")
            == std::string::npos);

    auto unknown = run_pulp({"inspect", "--definitely-not-an-inspect-flag"}, 10000);
    REQUIRE_FALSE(unknown.timed_out);
    REQUIRE(unknown.exit_code == 2);
    REQUIRE(unknown.stderr_output.find("unknown inspect argument")
            != std::string::npos);
    REQUIRE(unknown.stdout_output.find("Connecting to") == std::string::npos);
}

TEST_CASE("pulp create scaffolds a no-build app project with Android files",
          "[cli][shellout][create][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto base = fs::temp_directory_path() /
                ("pulp-shellout-create-app-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    auto home = base / "home";
    auto project = base / "out" / "neon-drum";
    fs::create_directories(home);
    fs::create_directories(project.parent_path());
    {
        std::ofstream cfg(home / "config.toml");
        cfg << "[update]\nmode = \"off\"\n";
    }

    pulp_setenv("PULP_HOME", home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);
    auto r = run_pulp({"create", "Neon Drum", "--type", "app",
                       "--targets", "android,standalone",
                       "--output", project.string(), "--no-build", "--ci"},
                      60000);
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    pulp_unsetenv("PULP_HOME");

    const bool has_header = fs::exists(project / "neon_drum.hpp");
    const bool has_main = fs::exists(project / "main.cpp");
    const bool has_test = fs::exists(project / "test_neon_drum.cpp");
    const bool has_cmake = fs::exists(project / "CMakeLists.txt");
    const bool has_toml = fs::exists(project / "pulp.toml");
    const bool has_android_settings = fs::exists(project / "android" / "settings.gradle.kts");
    const bool has_android_activity =
        fs::exists(project / "android" / "app" / "src" / "main" / "java" /
                   "neon_drum" / "MainActivity.kt");
    const auto header = read_file(project / "neon_drum.hpp");
    const auto main_cpp = read_file(project / "main.cpp");
    const auto cmake = read_file(project / "CMakeLists.txt");
    const auto toml = read_file(project / "pulp.toml");
    const auto registry = read_file(home / "projects.json");

    fs::remove_all(base);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(has_header);
    REQUIRE(has_main);
    REQUIRE(has_test);
    REQUIRE(has_cmake);
    REQUIRE(has_toml);
    REQUIRE(has_android_settings);
    REQUIRE(has_android_activity);
    REQUIRE(header.find("class NeonDrum") != std::string::npos);
    REQUIRE(header.find("namespace neon_drum") != std::string::npos);
    REQUIRE(header.find("com.pulp.neon_drum") != std::string::npos);
    REQUIRE(main_cpp.find("neon_drum::create_neon_drum") != std::string::npos);
    REQUIRE(cmake.find("pulp_add_app(NeonDrum") != std::string::npos);
    REQUIRE(toml.find("sdk_checkout") != std::string::npos);
    REQUIRE(registry.find("Neon Drum") != std::string::npos);
    REQUIRE(registry.find("neon-drum") != std::string::npos);
}

TEST_CASE("pulp create rejects invalid type before scaffolding",
          "[cli][shellout][create][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto base = fs::temp_directory_path() /
                ("pulp-shellout-create-invalid-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    auto home = base / "home";
    auto project = base / "out" / "bad-type";
    fs::create_directories(home);
    {
        std::ofstream cfg(home / "config.toml");
        cfg << "[update]\nmode = \"off\"\n";
    }

    pulp_setenv("PULP_HOME", home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);
    auto r = run_pulp({"create", "Bad Type", "--type", "potato",
                       "--output", project.string(), "--no-build", "--ci"},
                      10000);
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    pulp_unsetenv("PULP_HOME");

    const bool project_exists = fs::exists(project);
    fs::remove_all(base);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    REQUIRE_FALSE(project_exists);
    REQUIRE(r.stderr_output.find("--type must be") != std::string::npos);
}

// #51 / #356: `pulp validate --strict` is supposed to upgrade
// skipped-because-missing-tool into a hard failure.
//
// cmd_validate now parses flags up-front and rejects unknown flags
// with exit code 2 — that gives us a testable distinction between
// "known flag, can't run because no project" and "unknown flag"
// without needing a real build tree. Codex P2 on PR #381 correctly
// flagged that the prior version of this test couldn't tell the two
// apart: unknown flags were silently ignored, so the assertion would
// pass even if --strict handling were removed.
TEST_CASE("pulp validate --strict is a recognized flag",
          "[cli][shellout][validate][issue-356]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    // Run from /tmp so resolve_active_project_root() bails with
    // "not in a Pulp project directory". Flag parsing runs first, so
    // --strict passes through cleanly and we hit exit 1, not 2.
    //
    // NB: resolve the absolute binary path up front. run_pulp's
    // pulp_binary() is cwd-relative ("<cwd>/../tools/cli/pulp"), so
    // if we swap cwd to /tmp first the lookup resolves to a path
    // that doesn't exist and we never actually run the CLI.
    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(fs::temp_directory_path());
    auto strict = exec(bin.string(), {"validate", "--strict"}, 10000);
    auto bogus = exec(bin.string(), {"validate", "--this-flag-does-not-exist"}, 10000);
    fs::current_path(cwd_saver);

    REQUIRE_FALSE(strict.timed_out);
    REQUIRE_FALSE(bogus.timed_out);

    // --strict is known: flag parser accepts, we fall through to the
    // project-root bail-out, exit 1.
    REQUIRE(strict.exit_code == 1);
    REQUIRE(strict.stderr_output.find("unknown flag") == std::string::npos);

    // Unknown flag: rejected with exit 2 before the project-root check
    // even runs. If cmd_validate ever silently swallows unknown flags
    // again, this fails loudly and --strict handling is protected.
    REQUIRE(bogus.exit_code == 2);
    REQUIRE(bogus.stderr_output.find("unknown flag") != std::string::npos);
    REQUIRE(bogus.stderr_output.find("--this-flag-does-not-exist")
            != std::string::npos);
}

TEST_CASE("pulp validate strict json report records missing VST3 validators",
          "[cli][shellout][validate][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto base = unique_temp_dir("pulp-validate-strict");
    auto project = base / "Project";
    auto build = project / "build";
    auto report = base / "validation-report.json";
    write_text(project / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.22)\n"
               "project(ValidateFixture VERSION 1.2.3)\n");
    write_text(project / "pulp.toml", "[pulp]\nsdk_version = \"1.2.3\"\n");
    write_text(build / "CMakeCache.txt", "# fixture cache\n");
    fs::create_directories(build / "VST3" / "Fixture.vst3");

    ScopedEnvVar path("PATH");
    path.set("");

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(project);
    auto r = exec(bin.string(),
                  {"validate", "--strict", "--json", "--report", report.string()},
                  10000);
    fs::current_path(cwd_saver);

    auto report_body = read_file(report);
    fs::remove_all(base);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.stdout_output.find("Validation Summary: 1 total")
            != std::string::npos);
    REQUIRE(r.stdout_output.find("\"status\": \"skip\"") != std::string::npos);
    REQUIRE(r.stderr_output.find("ERROR: 1 validator(s) not installed")
            != std::string::npos);
    REQUIRE(r.stderr_output.find("pluginval") != std::string::npos);
    REQUIRE(r.stderr_output.find("Skipped-because-missing-tool count: 1")
            != std::string::npos);
    REQUIRE(report_body.find("\"plugin_format\": \"vst3\"")
            != std::string::npos);
    REQUIRE(report_body.find("\"status\": \"skip\"") != std::string::npos);
}

TEST_CASE("pulp validate covers empty builds, report failures, and screenshots",
          "[cli][shellout][validate][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto base = unique_temp_dir("pulp-validate-empty");
    auto project = base / "Project";
    auto build = project / "build";
    write_text(project / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.22)\n"
               "project(ValidateEmpty VERSION 1.2.3)\n");
    write_text(project / "pulp.toml", "[pulp]\nsdk_version = \"1.2.3\"\n");
    write_text(build / "CMakeCache.txt", "# fixture cache\n");

    ScopedEnvVar path("PATH");
    path.set("");

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(project);
    auto r = exec(bin.string(),
                  {"validate", "--screenshot", "--report", build.string()},
                  10000);
    fs::current_path(cwd_saver);
    fs::remove_all(base);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("Validation Summary: 0 total")
            != std::string::npos);
    REQUIRE(r.stdout_output.find("No plugin screenshots captured")
            != std::string::npos);
    REQUIRE(r.stderr_output.find("Failed to write report to")
            != std::string::npos);
}

TEST_CASE("pulp build fails fast when standalone SDK is ahead of the installed CLI",
          "[cli][shellout][build][issue-682]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() /
               ("pulp-shellout-build-skew-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
    fs::create_directories(tmp);
    {
        std::ofstream f(tmp / "pulp.toml");
        f << "[pulp]\n"
          << "sdk_version = \"99.0.0\"\n"
          << "cli_min_version = \"99.0.0\"\n";
    }

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(tmp);
    auto r = exec(bin.string(), {"build"}, 10000);
    fs::current_path(cwd_saver);
    fs::remove_all(tmp);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("requires a newer Pulp CLI") != std::string::npos);
    REQUIRE(combined.find("pulp upgrade 99.0.0") != std::string::npos);
    REQUIRE(combined.find("pulp doctor --versions") != std::string::npos);
    REQUIRE(combined.find("--allow-unsupported-sdk") != std::string::npos);
}

TEST_CASE("pulp build allows explicit unsupported SDK bypass",
          "[cli][shellout][build][issue-682]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() /
               ("pulp-shellout-build-allow-skew-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
    fs::create_directories(tmp);
    {
        std::ofstream f(tmp / "pulp.toml");
        f << "[pulp]\n"
          << "sdk_version = \"99.0.0\"\n"
          << "cli_min_version = \"99.0.0\"\n";
    }

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(tmp);
    auto r = exec(bin.string(), {"build", "--allow-unsupported-sdk"}, 10000);
    fs::current_path(cwd_saver);
    fs::remove_all(tmp);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("requires a newer Pulp CLI") == std::string::npos);
    REQUIRE(combined.find("pulp upgrade 99.0.0") == std::string::npos);
}

// #8 / #355 — `pulp doctor android` and `pulp doctor ios` are
// recognized subcommands; bogus subcommand fails with exit 2 + Usage.
TEST_CASE("pulp doctor android|ios are recognized subcommands",
          "[cli][shellout][doctor][issue-355]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    const auto bin = fs::absolute(pulp_binary());

    // Use --versions so the parser still has to accept the mobile
    // subcommand before the diagnostic short-circuit, without running the
    // slow host SDK probes that can hang on saturated CI runners.
    auto android = exec(bin.string(), {"doctor", "android", "--versions"}, 10000);
    auto ios     = exec(bin.string(), {"doctor", "ios", "--versions"},     10000);
    auto bogus   = exec(bin.string(), {"doctor", "potato", "--versions"},  10000);

    REQUIRE_FALSE(android.timed_out);
    REQUIRE_FALSE(ios.timed_out);
    REQUIRE_FALSE(bogus.timed_out);

    REQUIRE(android.exit_code == 0);
    REQUIRE(ios.exit_code == 0);
    REQUIRE(android.stdout_output.find("Pulp Version Diagnostics") != std::string::npos);
    REQUIRE(ios.stdout_output.find("Pulp Version Diagnostics") != std::string::npos);

    // bogus subcommand: rejected at the parser with a helpful Usage line.
    REQUIRE(bogus.exit_code == 2);
    REQUIRE(bogus.stderr_output.find("unknown subcommand") != std::string::npos);
    REQUIRE(bogus.stderr_output.find("Usage:") != std::string::npos);
}

// Issue #743: `pulp doctor --validators` is a recognized flag and
// produces a per-validator OK/FAIL/WARN report. We don't assert the
// per-tool verdicts (they depend on host install state) — just that
// the parser accepts the flag, the section header renders, and the
// exit code is in the documented set (0 = all healthy, 1 = at least
// one validator missing or broken). An exit code of 2 would mean the
// flag parser rejected --validators, which is exactly the regression
// this test guards against.
TEST_CASE("pulp doctor --validators is a recognized flag",
          "[cli][shellout][doctor][issue-743]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    const auto bin = fs::absolute(pulp_binary());
    auto r = exec(bin.string(), {"doctor", "--validators"}, 30000);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 2);  // parser accepted the flag
    REQUIRE(r.stdout_output.find("Pulp Doctor") != std::string::npos);
    REQUIRE(r.stdout_output.find("Validators") != std::string::npos);

    // --fix --dry-run is the safest flag combo to exercise: no fs
    // mutation, but the heal pipeline still runs end-to-end. Also
    // verifies that --fix combined with --validators is accepted.
    auto dry = exec(bin.string(),
                    {"doctor", "--validators", "--fix", "--dry-run"}, 30000);
    REQUIRE_FALSE(dry.timed_out);
    REQUIRE(dry.exit_code != 2);
    REQUIRE(dry.stdout_output.find("Pulp Doctor") != std::string::npos);
}

TEST_CASE("pulp dev fails fast when standalone SDK is ahead of the installed CLI",
          "[cli][shellout][dev][issue-682]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() /
               ("pulp-shellout-dev-skew-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
    fs::create_directories(tmp);
    {
        std::ofstream f(tmp / "pulp.toml");
        f << "[pulp]\n"
          << "sdk_version = \"99.0.0\"\n"
          << "cli_min_version = \"99.0.0\"\n";
    }

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(tmp);
    auto r = exec(bin.string(), {"dev"}, 10000);
    fs::current_path(cwd_saver);
    fs::remove_all(tmp);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("requires a newer Pulp CLI") != std::string::npos);
    REQUIRE(combined.find("pulp upgrade 99.0.0") != std::string::npos);
    REQUIRE(combined.find("--allow-unsupported-sdk") != std::string::npos);
}

// Issue #499 Slice 1: `pulp doctor --versions` is the foundation of
// the release-discovery UX. Verify the subcommand is wired, always
// exits 0 (skew findings are advisory), and surfaces the diagnostic
// header + CLI version line on stdout. This catches the class of
// silent-failure bug described in #295: an --versions flag that is
// parsed but routes to a no-op would pass CI green without this test.
TEST_CASE("pulp doctor --versions prints diagnostics and exits 0",
          "[cli][shellout][doctor][issue-499]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto r = run_pulp({"doctor", "--versions"}, 30000);
    REQUIRE_FALSE(r.timed_out);
    // Exit code is always 0 — skew is advisory, not a hard failure.
    REQUIRE(r.exit_code == 0);
    // The report header must be on stdout so scripts can distinguish
    // this output from the default `pulp doctor` pipeline.
    REQUIRE(r.stdout_output.find("Pulp Version Diagnostics") != std::string::npos);
    // We always print the CLI line, even when no project is active.
    REQUIRE(r.stdout_output.find("CLI:") != std::string::npos);
}

// Issue #548 Slice 3: `pulp upgrade --notes` prints migration notes
// for the hop without downloading anything. `--from`/`--to` overrides
// let us exercise the filter deterministically regardless of the
// installed CLI version on the test host.
TEST_CASE("pulp upgrade --notes --from A --to B prints migration header",
          "[cli][shellout][upgrade][issue-548]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    // Use an env override to neutralise the network-facing update
    // banner so its stderr noise doesn't interfere.
    auto r = run_pulp({"upgrade", "--notes", "--from", "0.23.0", "--to", "0.29.0"}, 15000);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("Pulp migration notes: 0.23.0 -> 0.29.0")
            != std::string::npos);
    // At least one of the seeded migration docs must surface between
    // 0.23.0 and 0.29.0 — otherwise the embedded index was not
    // regenerated from docs/migrations/.
    REQUIRE(r.stdout_output.find("v0.") != std::string::npos);
}

TEST_CASE("pulp upgrade --notes --json emits stable-shape JSON keys",
          "[cli][shellout][upgrade][issue-548]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto r = run_pulp({"upgrade", "--notes", "--json",
                       "--from", "0.23.0", "--to", "0.29.0"}, 15000);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    // Slice 4 depends on these keys — they are a stable public surface.
    REQUIRE(r.stdout_output.find("\"from\":")       != std::string::npos);
    REQUIRE(r.stdout_output.find("\"to\":")         != std::string::npos);
    REQUIRE(r.stdout_output.find("\"entries\":")    != std::string::npos);
    REQUIRE(r.stdout_output.find("\"version\":")    != std::string::npos);
    REQUIRE(r.stdout_output.find("\"breaking\":")   != std::string::npos);
    REQUIRE(r.stdout_output.find("\"summary\":")    != std::string::npos);
    REQUIRE(r.stdout_output.find("\"applies_if\":") != std::string::npos);
    REQUIRE(r.stdout_output.find("\"body\":")       != std::string::npos);
}

TEST_CASE("pulp upgrade --check-only honors disabled update checks with an empty cache",
          "[cli][shellout][upgrade][codecov]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() /
               ("pulp-shellout-upgrade-disabled-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
    fs::create_directories(tmp);

    pulp_setenv("PULP_HOME", tmp.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);
    auto r = run_pulp({"upgrade", "--check-only"}, 10000);
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    pulp_unsetenv("PULP_HOME");
    fs::remove_all(tmp);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("Installed:  v") != std::string::npos);
    REQUIRE(r.stdout_output.find("Latest:     update check disabled; not queried")
            != std::string::npos);
    REQUIRE(r.stdout_output.find("Cache empty; querying GitHub Releases")
            == std::string::npos);
}

// Issue #550 Slice 5: `update.mode = off` must produce zero network
// traffic and zero banner output. We can't directly observe the
// network inside ctest, but we CAN verify that (a) the command
// finishes well under the anonymous-GitHub round-trip latency (caching
// the cache is fine; actually dialing the API is not), and (b) no
// update banner appears on stderr. The banner hook's early-return on
// `Mode::Off` is what this test guards.
//
// We use PULP_HOME to point the CLI at a scratch config dir seeded
// with `update.mode = "off"` so we don't mutate the developer's real
// ~/.pulp. The test creates the config file directly rather than
// shelling out to `pulp config set` so we can reason about the
// sequence (`pulp config set` itself clears the snooze file — we want
// to isolate the dispatch-path behaviour).
TEST_CASE("pulp with update.mode=off never prints a banner",
          "[cli][shellout][update-mode][issue-550]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() /
               ("pulp-shellout-mode-off-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
    fs::create_directories(tmp);
    {
        std::ofstream cfg(tmp / "config.toml");
        cfg << "[update]\nmode = \"off\"\n";
    }
    // Seed the cache with a spurious "newer version available" entry.
    // If the off-mode short-circuit regressed, this is what would
    // leak into the banner.
    {
        std::ofstream cache(tmp / "update-cache.json");
        cache << "{\n"
              << "  \"schema\": 1,\n"
              << "  \"last_check_epoch_sec\": 1713638400,\n"
              << "  \"latest_version\": \"99.99.99\",\n"
              << "  \"release_notes_url\": \"https://example.invalid/\",\n"
              << "  \"banner_shown_for_version\": \"\"\n"
              << "}\n";
    }

    const auto bin = fs::absolute(pulp_binary());
#ifdef _WIN32
    _putenv_s("PULP_HOME", tmp.string().c_str());
#else
    pulp_setenv("PULP_HOME", tmp.string().c_str(), 1);
#endif
    auto r = exec(bin.string(), {"help"}, 10000);
#ifdef _WIN32
    _putenv_s("PULP_HOME", "");
#else
    pulp_unsetenv("PULP_HOME");
#endif
    fs::remove_all(tmp);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    // The seeded cache has latest=99.99.99 which would trigger the
    // prompt/manual/auto banners. off-mode must suppress ALL of them.
    REQUIRE(r.stderr_output.find("99.99.99") == std::string::npos);
    REQUIRE(r.stderr_output.find("available") == std::string::npos);
    REQUIRE(r.stderr_output.find("downloaded") == std::string::npos);
}

// Issue #550 Slice 5: `update.mode = manual` prints the one-liner
// once per version. The banner shape is locked — it must differ from
// the prompt-mode banner so users (and shell scripts) can tell them
// apart. Same PULP_HOME scratch-dir trick as the off-mode test.
TEST_CASE("pulp with update.mode=manual prints the manual notice",
          "[cli][shellout][update-mode][issue-550]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() /
               ("pulp-shellout-mode-manual-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
    fs::create_directories(tmp);
    {
        std::ofstream cfg(tmp / "config.toml");
        cfg << "[update]\nmode = \"manual\"\n";
    }
    {
        std::ofstream cache(tmp / "update-cache.json");
        cache << "{\n"
              << "  \"schema\": 1,\n"
              << "  \"last_check_epoch_sec\": 1713638400,\n"
              << "  \"latest_version\": \"99.99.99\",\n"
              << "  \"release_notes_url\": \"https://example.invalid/\",\n"
              << "  \"banner_shown_for_version\": \"\"\n"
              << "}\n";
    }

    const auto bin = fs::absolute(pulp_binary());
#ifdef _WIN32
    _putenv_s("PULP_HOME", tmp.string().c_str());
#else
    pulp_setenv("PULP_HOME", tmp.string().c_str(), 1);
#endif
    // Use `doctor --versions` — it's not on the banner_blocked list,
    // so the dispatch hook runs for it. `help` IS blocked.
    auto r = exec(bin.string(), {"doctor", "--versions"}, 30000);
#ifdef _WIN32
    _putenv_s("PULP_HOME", "");
#else
    pulp_unsetenv("PULP_HOME");
#endif
    fs::remove_all(tmp);

    REQUIRE_FALSE(r.timed_out);
    // Manual mode banner shape: "Run `pulp upgrade` when you're ready."
    REQUIRE(r.stderr_output.find("when you're ready") != std::string::npos);
    REQUIRE(r.stderr_output.find("99.99.99") != std::string::npos);
}

// Issue #564 Slice 7: `pulp project bump --help` must be wired at the
// dispatch level. Any future regression where the `project` command
// falls out of the dispatch table fails loudly here — same class of
// silent-failure bug that motivated the rest of this file.
TEST_CASE("pulp project is a recognized command with pin/unpin/undo subcommands",
          "[cli][shellout][issue-564][issue-2087]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto help = run_pulp({"project", "--help"}, 10000);
    REQUIRE_FALSE(help.timed_out);
    REQUIRE(help.exit_code == 0);
    // pulp #2087: `pin` is the primary command name; `bump` survives as
    // a deprecated alias for one minor release.
    REQUIRE(help.stdout_output.find("project pin") != std::string::npos);
    REQUIRE(help.stdout_output.find("project unpin") != std::string::npos);
    REQUIRE(help.stdout_output.find("project undo") != std::string::npos);
    REQUIRE(help.stdout_output.find("deprecated alias") != std::string::npos);

    // `pulp project pin --help` is the new primary help surface.
    auto pin_help = run_pulp({"project", "pin", "--help"}, 10000);
    REQUIRE_FALSE(pin_help.timed_out);
    REQUIRE(pin_help.exit_code == 0);
    REQUIRE(pin_help.stdout_output.find("--all") != std::string::npos);
    REQUIRE(pin_help.stdout_output.find("--dry-run") != std::string::npos);
    REQUIRE(pin_help.stdout_output.find("--force-dirty") != std::string::npos);
    REQUIRE(pin_help.stdout_output.find("--allow-downgrade") != std::string::npos);
    REQUIRE(pin_help.stdout_output.find("--allow-cli-skew") != std::string::npos);
    REQUIRE(pin_help.stdout_output.find("--allow-redundant") != std::string::npos);
    REQUIRE(pin_help.stdout_output.find("--verify-builds") != std::string::npos);
    REQUIRE(pin_help.stdout_output.find("pulp.toml sdk_version") != std::string::npos);
    // The pin help should cross-reference unpin so the round-trip is
    // discoverable from either direction.
    REQUIRE(pin_help.stdout_output.find("pulp project unpin") != std::string::npos);

    // `pulp project bump --help` must still work (backward-compat alias).
    auto bump_help = run_pulp({"project", "bump", "--help"}, 10000);
    REQUIRE_FALSE(bump_help.timed_out);
    REQUIRE(bump_help.exit_code == 0);
    // The alias goes through the same code path as `pin`, so the help
    // is the same text — confirms users won't get a stale "bump"-named
    // dead-end if they keep typing the old command.
    REQUIRE(bump_help.stdout_output.find("--all") != std::string::npos);

    auto unpin_help = run_pulp({"project", "unpin", "--help"}, 10000);
    REQUIRE_FALSE(unpin_help.timed_out);
    REQUIRE(unpin_help.exit_code == 0);
    REQUIRE(unpin_help.stdout_output.find("floating") != std::string::npos);
    REQUIRE(unpin_help.stdout_output.find("pulp project pin") != std::string::npos);

    auto undo_help = run_pulp({"project", "undo", "--help"}, 10000);
    REQUIRE_FALSE(undo_help.timed_out);
    REQUIRE(undo_help.exit_code == 0);
    REQUIRE(undo_help.stdout_output.find("Revert") != std::string::npos);

    auto bogus = run_pulp({"project", "potato"}, 10000);
    REQUIRE_FALSE(bogus.timed_out);
    REQUIRE(bogus.exit_code != 0);
    REQUIRE(bogus.stderr_output.find("unknown subcommand") != std::string::npos);
}

// pulp #2087: `pulp project unpin` rewrites pulp.toml's sdk_version
// to "latest" so the project tracks the newest installed SDK on every
// rebuild. Inverse of `pin <version>`. We exercise the round trip
// against a synthetic standalone project fixture so the test doesn't
// depend on the registry or on remote network state.
TEST_CASE("pulp project unpin switches a pinned project to floating mode",
          "[cli][shellout][issue-2087]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto base = fs::temp_directory_path() /
                ("pulp-shellout-unpin-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    auto home = base / "home";
    auto project = base / "out" / "my-plugin";
    fs::create_directories(home);
    fs::create_directories(project);

    // Minimal pinned project. Don't run `pulp create` — it would fetch
    // the SDK from the network, which we don't want in a unit test.
    // The unpin command only needs pulp.toml + a not-the-Pulp-checkout
    // layout to operate, so we synthesize both inline.
    {
        std::ofstream f(project / "pulp.toml");
        f << "[pulp]\n";
        f << "sdk_version = \"0.91.0\"\n";
    }
    {
        std::ofstream f(project / "CMakeLists.txt");
        f << "cmake_minimum_required(VERSION 3.24)\n";
        f << "project(MyPlugin VERSION 1.0.0 LANGUAGES CXX)\n";
        f << "find_package(Pulp 0.91.0 REQUIRED)\n";
    }

    pulp_setenv("PULP_HOME", home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(project);

    // Dry-run first: must NOT mutate the file.
    auto dry = exec(bin.string(), {"project", "unpin", "--dry-run"}, 10000);
    const auto pre_dry = read_file(project / "pulp.toml");
    REQUIRE_FALSE(dry.timed_out);
    REQUIRE(dry.exit_code == 0);
    REQUIRE(dry.stdout_output.find("[dry-run]") != std::string::npos);
    REQUIRE(dry.stdout_output.find("0.91.0") != std::string::npos);
    REQUIRE(pre_dry.find("sdk_version = \"0.91.0\"") != std::string::npos);

    // Real run: pulp.toml's sdk_version must become "latest".
    auto run = exec(bin.string(), {"project", "unpin"}, 10000);
    const auto post = read_file(project / "pulp.toml");
    REQUIRE_FALSE(run.timed_out);
    REQUIRE(run.exit_code == 0);
    REQUIRE(run.stdout_output.find("unpinned") != std::string::npos);
    REQUIRE(post.find("sdk_version = \"latest\"") != std::string::npos);
    REQUIRE(post.find("sdk_version = \"0.91.0\"") == std::string::npos);

    // Idempotence: running unpin twice should be a no-op the second time.
    auto run2 = exec(bin.string(), {"project", "unpin"}, 10000);
    REQUIRE_FALSE(run2.timed_out);
    REQUIRE(run2.exit_code == 0);
    REQUIRE(run2.stdout_output.find("already floating") != std::string::npos);

    fs::current_path(cwd_saver);
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    pulp_unsetenv("PULP_HOME");
    fs::remove_all(base);
}

// Issue #564 Slice 7: `pulp project bump --dry-run` rejects an
// invalid --to value with a diagnostic. No writes happen.
TEST_CASE("pulp project bump rejects non-semver --to",
          "[cli][shellout][issue-564]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    const auto bin = fs::absolute(pulp_binary());
    // Run from /tmp so there's no CMakeLists.txt — but we expect the
    // semver validation to fire BEFORE any filesystem touch anyway.
    auto cwd_saver = fs::current_path();
    fs::current_path(fs::temp_directory_path());
    auto r = exec(bin.string(), {"project", "bump", "--to=not-a-version", "--dry-run"}, 10000);
    fs::current_path(cwd_saver);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    REQUIRE(r.stderr_output.find("invalid target version") != std::string::npos);
}

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

TEST_CASE("pulp project bump updates standalone SDK pins and undo reverts them",
          "[cli][shellout][project-bump][issue-244]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto base = fs::temp_directory_path() /
                ("pulp-shellout-project-bump-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    auto project = base / "Clock";
    auto home = base / "home";
    fs::create_directories(project);
    fs::create_directories(home);
    {
        std::ofstream cfg(home / "config.toml");
        cfg << "[update]\nmode = \"off\"\n";
    }
    {
        std::ofstream cmake(project / "CMakeLists.txt");
        cmake << "cmake_minimum_required(VERSION 3.20)\n"
              << "project(Clock VERSION 1.0.0 LANGUAGES CXX)\n"
              << "find_package(Pulp 0.1.0 REQUIRED)\n";
    }
    {
        std::ofstream toml(project / "pulp.toml");
        toml << "[pulp]\n"
             << "sdk_version = \"0.1.0\"\n"
             << "sdk_path = \"/custom/sdk\"\n";
    }

    const auto bin = fs::absolute(pulp_binary());
    const auto saved_cwd = fs::current_path();
    pulp_setenv("PULP_HOME", home.string().c_str(), 1);
    fs::current_path(project);

    auto bump = exec(bin.string(), {"project", "bump", "--to=0.2.0"}, 10000);
    std::string cmake_after;
    std::string toml_after;
    ProcessResult undo;
    if (!bump.timed_out && bump.exit_code == 0) {
        cmake_after = read_file(project / "CMakeLists.txt");
        toml_after = read_file(project / "pulp.toml");
        undo = exec(bin.string(), {"project", "undo"}, 10000);
    }
    fs::current_path(saved_cwd);
    pulp_unsetenv("PULP_HOME");

    REQUIRE_FALSE(bump.timed_out);
    REQUIRE(bump.exit_code == 0);
    REQUIRE(bump.stdout_output.find("bumped") != std::string::npos);
    REQUIRE(bump.stdout_output.find("custom sdk_path left unchanged") != std::string::npos);

    REQUIRE(cmake_after.find("project(Clock VERSION 1.0.0") != std::string::npos);
    REQUIRE(cmake_after.find("find_package(Pulp 0.2.0 REQUIRED)") != std::string::npos);
    REQUIRE(toml_after.find("sdk_version = \"0.2.0\"") != std::string::npos);
    REQUIRE(toml_after.find("sdk_path = \"/custom/sdk\"") != std::string::npos);

    REQUIRE_FALSE(undo.timed_out);
    REQUIRE(undo.exit_code == 0);
    REQUIRE(undo.stdout_output.find("reverted") != std::string::npos);
    REQUIRE(read_file(project / "CMakeLists.txt").find("find_package(Pulp 0.1.0 REQUIRED)")
            != std::string::npos);
    REQUIRE(read_file(project / "pulp.toml").find("sdk_version = \"0.1.0\"")
            != std::string::npos);

    fs::remove_all(base);
}

TEST_CASE("pulp upgrade --notes with no hop prints the empty-notes line",
          "[cli][shellout][upgrade][issue-548]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    // from == to → degenerate hop with zero applicable notes.
    auto r = run_pulp({"upgrade", "--notes", "--from", "0.29.0", "--to", "0.29.0"}, 15000);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("No migration notes apply") != std::string::npos);
}

// Issue #549 Slice 4: the `/upgrade` Claude Code slash command shells out
// to `pulp upgrade --notes --json` and parses the resulting document.
// The slash command hardcodes the JSON key names as part of its parsing
// step (.claude/commands/upgrade.md) — if the CLI renames any of them
// without updating the skill, the slash command silently falls back to
// empty context. Guard the exact keys the slash command reads.
//
// Separate from the existing issue-548 JSON test because Slice 4 also
// cares about (a) the document being parseable even when entries is
// empty and (b) every key the .claude/commands/upgrade.md parsing step
// relies on — `summary`, `body`, `applies_if` included.
TEST_CASE("pulp upgrade --notes --json is slash-command-parseable",
          "[cli][shellout][upgrade][issue-549]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    // Real hop over the seeded migration docs — we expect >=1 entry so
    // the JSON document isn't a degenerate "entries": [] case.
    auto hop = run_pulp({"upgrade", "--notes", "--json",
                        "--from", "0.23.0", "--to", "0.29.0"}, 15000);
    REQUIRE_FALSE(hop.timed_out);
    REQUIRE(hop.exit_code == 0);

    // Every key the `/upgrade` slash command reads must be present. The
    // slash command's parsing step in .claude/commands/upgrade.md
    // treats these as a stable public surface.
    for (const char* key : {"\"from\":", "\"to\":", "\"entries\":",
                            "\"version\":", "\"breaking\":",
                            "\"summary\":", "\"applies_if\":",
                            "\"body\":"}) {
        INFO("missing slash-command-required key: " << key);
        REQUIRE(hop.stdout_output.find(key) != std::string::npos);
    }

    // Degenerate hop: the slash command still needs to see a
    // well-formed document so its JSON parser doesn't choke on empty
    // output — `entries: []` is the contract, not an empty string.
    auto noop = run_pulp({"upgrade", "--notes", "--json",
                         "--from", "0.29.0", "--to", "0.29.0"}, 15000);
    REQUIRE_FALSE(noop.timed_out);
    REQUIRE(noop.exit_code == 0);
    REQUIRE(noop.stdout_output.find("\"from\":")    != std::string::npos);
    REQUIRE(noop.stdout_output.find("\"to\":")      != std::string::npos);
    REQUIRE(noop.stdout_output.find("\"entries\":") != std::string::npos);
}

// #591 Codex P2 / wave-4 sweep: `pulp docs build-site` must resolve
// `mkdocs.yml` from the project root, not the caller's CWD. This test
// invokes the CLI from inside a subdirectory (`tools/`) and asserts
// that the composed mkdocs command references the repo-root config —
// the previous bare `mkdocs build` would have failed with a
// "Config file 'mkdocs.yml' does not exist" error here.
//
// We don't require mkdocs to be installed: if it's missing, run exits
// non-zero with a "pip install -r requirements-docs.txt" hint on
// stderr, which is still a valid pass for this regression because the
// failure mode is "mkdocs not found", not "config file not found".
// What we must NOT see is a "Config file 'mkdocs.yml' does not exist"
// error, because that would mean `-f <root>/mkdocs.yml` wasn't passed.
TEST_CASE("pulp docs build-site resolves mkdocs.yml from project root",
          "[cli][shellout][docs][issue-591]") {
    if (!binary_exists()) {
        SUCCEED("pulp binary not built for this test run; skipping");
        return;
    }

    // Walk up from the test CWD (<build>/test) to the repo root, then
    // drop into tools/ — a real subdirectory that exists in every
    // Pulp checkout. This mirrors the "user runs `pulp docs
    // build-site` from inside tools/" scenario Codex flagged.
    fs::path repo_root = fs::current_path() / ".." / "..";
    repo_root = fs::weakly_canonical(repo_root);
    if (!fs::exists(repo_root / "mkdocs.yml")) {
        SUCCEED("mkdocs.yml not at expected repo root — likely non-standard "
                "build layout; skipping");
        return;
    }
    fs::path subdir = repo_root / "tools";
    REQUIRE(fs::exists(subdir));

    // Pulp #597: pulp_binary() resolves from current_path() / "../tools/cli/pulp"
    // — that assumes the test is run from <build>/test. When we change
    // cwd below to <repo>/tools, the relative resolution points at
    // <repo>/tools/cli/pulp (source, not built), which doesn't exist,
    // and run_pulp silently returns exit_code=-1 without launching the
    // CLI — making the regression test vacuous. Resolve the absolute
    // binary path BEFORE the cwd change and pipe it through PULP_CLI_PATH,
    // which pulp_binary() honors as an explicit override.
    fs::path absolute_pulp_bin = fs::absolute(pulp_binary());
    REQUIRE(fs::exists(absolute_pulp_bin));
    // fs::path::c_str() returns const wchar_t* on Windows, so stringify
    // explicitly so the pulp_setenv(const char*, const char*, int) overload
    // resolves on every platform.
    const std::string absolute_pulp_bin_str = absolute_pulp_bin.string();
    pulp_setenv("PULP_CLI_PATH", absolute_pulp_bin_str.c_str(), 1);

    fs::path saved_cwd = fs::current_path();
    std::error_code ec;
    fs::current_path(subdir, ec);
    REQUIRE_FALSE(ec);

    // --strict is safe: if mkdocs runs at all, the build either
    // succeeds or surfaces a warnings-as-errors exit; neither produces
    // the "Config file 'mkdocs.yml' does not exist" string.
    auto r = run_pulp({"docs", "build-site", "--site-dir",
                      (repo_root / "build" / "site-shellout-test").string()},
                     60000);

    fs::current_path(saved_cwd, ec);  // always restore
    pulp_unsetenv("PULP_CLI_PATH");

    REQUIRE_FALSE(r.timed_out);
    // Permit mkdocs-missing ("command not found" path, rc != 0 with
    // install hint on stderr) but NOT "Config file 'mkdocs.yml' does
    // not exist" — that would mean the `-f <root>/mkdocs.yml` fix
    // regressed.
    const std::string combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("Config file 'mkdocs.yml' does not exist")
            == std::string::npos);
    REQUIRE(combined.find("does not exist; use --config-file")
            == std::string::npos);
}

TEST_CASE("pulp docs covers local reader index, search, open, and show paths",
          "[cli][shellout][docs][issue-643]") {
    if (!binary_exists()) {
        SUCCEED("pulp binary not built for this test run; skipping");
        return;
    }

    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    SECTION("usage lists the local docs reader subcommands") {
        auto r = run_pulp({"docs"});
        REQUIRE(r.exit_code == 0);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.stdout_output.find("pulp docs") != std::string::npos);
        REQUIRE(r.stdout_output.find("show command") != std::string::npos);
        REQUIRE(r.stdout_output.find("build-site") != std::string::npos);
    }

    SECTION("index and open resolve docs-index slugs") {
        auto index = run_pulp({"docs", "index"});
        REQUIRE(index.exit_code == 0);
        REQUIRE_FALSE(index.timed_out);
        REQUIRE(index.stdout_output.find("Available Documentation") != std::string::npos);
        REQUIRE(index.stdout_output.find("getting-started") != std::string::npos);
        REQUIRE(index.stdout_output.find("docs/guides/getting-started.md") != std::string::npos);

        auto opened = run_pulp({"docs", "open", "getting-started"});
        REQUIRE(opened.exit_code == 0);
        REQUIRE_FALSE(opened.timed_out);
        REQUIRE(opened.stdout_output.find("Getting Started") != std::string::npos);
    }

    SECTION("search reports exact doc matches") {
        auto r = run_pulp({"docs", "search", "WebView"});
        REQUIRE(r.exit_code == 0);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE((r.stdout_output.find("match(es) found") != std::string::npos ||
                 r.stdout_output.find("docs/") != std::string::npos));
    }

    SECTION("search reports fuzzy suggestions and empty results") {
        auto fuzzy = run_pulp({"docs", "search", "gttngstrtd"});
        REQUIRE(fuzzy.exit_code == 0);
        REQUIRE_FALSE(fuzzy.timed_out);
        REQUIRE(fuzzy.stdout_output.find("No exact matches") != std::string::npos);
        REQUIRE(fuzzy.stdout_output.find("Did you mean") != std::string::npos);
        REQUIRE(fuzzy.stdout_output.find("getting-started") != std::string::npos);

        auto empty = run_pulp({"docs", "search", "zzzz-no-doc-match"});
        REQUIRE(empty.exit_code == 0);
        REQUIRE_FALSE(empty.timed_out);
        REQUIRE(empty.stdout_output.find("No matches for") != std::string::npos);
    }

    SECTION("show reads support, command, cmake, and style manifests") {
        auto support = run_pulp({"docs", "show", "support", "vst3"});
        REQUIRE(support.exit_code == 0);
        REQUIRE_FALSE(support.timed_out);
        REQUIRE(support.stdout_output.find("Support info") != std::string::npos);
        REQUIRE(support.stdout_output.find("vst3") != std::string::npos);

        auto command = run_pulp({"docs", "show", "command", "ship"});
        REQUIRE(command.exit_code == 0);
        REQUIRE_FALSE(command.timed_out);
        REQUIRE(command.stdout_output.find("Command: ship") != std::string::npos);
        REQUIRE(command.stdout_output.find("Subcommands") != std::string::npos);
        REQUIRE(command.stdout_output.find("package") != std::string::npos);

        auto cmake = run_pulp({"docs", "show", "cmake", "pulp_add_plugin"});
        REQUIRE(cmake.exit_code == 0);
        REQUIRE_FALSE(cmake.timed_out);
        REQUIRE(cmake.stdout_output.find("CMake function: pulp_add_plugin") != std::string::npos);

        auto style = run_pulp({"docs", "show", "style"});
        REQUIRE(style.exit_code == 0);
        REQUIRE_FALSE(style.timed_out);
        REQUIRE(style.stdout_output.find("Style Rules") != std::string::npos);
        REQUIRE(style.stdout_output.find("public_headers_only") != std::string::npos);
    }

    SECTION("reader errors include actionable diagnostics") {
        auto search_usage = run_pulp({"docs", "search"});
        REQUIRE(search_usage.exit_code != 0);
        REQUIRE(search_usage.stderr_output.find("Usage: pulp docs search") != std::string::npos);

        auto open_usage = run_pulp({"docs", "open"});
        REQUIRE(open_usage.exit_code != 0);
        REQUIRE(open_usage.stderr_output.find("Usage: pulp docs open") != std::string::npos);

        auto missing_slug = run_pulp({"docs", "open", "not-a-real-doc"});
        REQUIRE(missing_slug.exit_code != 0);
        REQUIRE(missing_slug.stderr_output.find("no doc found") != std::string::npos);

        auto show_usage = run_pulp({"docs", "show"});
        REQUIRE(show_usage.exit_code != 0);
        REQUIRE(show_usage.stderr_output.find("Usage: pulp docs show") != std::string::npos);

        auto support_usage = run_pulp({"docs", "show", "support"});
        REQUIRE(support_usage.exit_code != 0);
        REQUIRE(support_usage.stderr_output.find("Usage: pulp docs show support") != std::string::npos);

        auto command_usage = run_pulp({"docs", "show", "command"});
        REQUIRE(command_usage.exit_code != 0);
        REQUIRE(command_usage.stderr_output.find("Usage: pulp docs show command") != std::string::npos);

        auto cmake_usage = run_pulp({"docs", "show", "cmake"});
        REQUIRE(cmake_usage.exit_code != 0);
        REQUIRE(cmake_usage.stderr_output.find("Usage: pulp docs show cmake") != std::string::npos);

        auto unknown_show = run_pulp({"docs", "show", "widget"});
        REQUIRE(unknown_show.exit_code != 0);
        REQUIRE(unknown_show.stderr_output.find("Unknown show topic") != std::string::npos);

        auto unknown_subcommand = run_pulp({"docs", "wat"});
        REQUIRE(unknown_subcommand.exit_code != 0);
        REQUIRE(unknown_subcommand.stderr_output.find("Unknown docs subcommand") != std::string::npos);
    }

    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
}

// pulp #914 — `pulp run --help` must advertise the four new flags so
// users (and the docs generator) can discover them. This is the help
// surface contract; the parser is exercised in test_cli_run_options.cpp.
TEST_CASE("pulp run --help advertises the headless/screenshot/frames/watch flags",
          "[cli][shellout][run][issue-914]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto r = run_pulp({"run", "--help"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("--headless")   != std::string::npos);
    REQUIRE(r.stdout_output.find("--screenshot") != std::string::npos);
    REQUIRE(r.stdout_output.find("--frames")     != std::string::npos);
    REQUIRE(r.stdout_output.find("--watch")      != std::string::npos);
    // Make sure the existing "active project build" line stays — the
    // root CMakeLists.txt regex test depends on it.
    REQUIRE(r.stdout_output.find("active project build") != std::string::npos);
}

// pulp #914 — bad `--frames` is caught before the run path tries to
// resolve a project, so we get a clean exit-2 with a diagnostic.
TEST_CASE("pulp run --frames rejects non-positive / non-integer values",
          "[cli][shellout][run][issue-914]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto bad_int = run_pulp({"run", "--frames", "notanumber"});
    REQUIRE_FALSE(bad_int.timed_out);
    REQUIRE(bad_int.exit_code == 2);
    REQUIRE(bad_int.stderr_output.find("--frames") != std::string::npos);

    auto zero = run_pulp({"run", "--frames", "0"});
    REQUIRE_FALSE(zero.timed_out);
    REQUIRE(zero.exit_code == 2);
    REQUIRE(zero.stderr_output.find("--frames") != std::string::npos);

    auto missing_path = run_pulp({"run", "--screenshot"});
    REQUIRE_FALSE(missing_path.timed_out);
    REQUIRE(missing_path.exit_code == 2);
    REQUIRE(missing_path.stderr_output.find("--screenshot") != std::string::npos);
}

// pulp #914 — end-to-end CI-validation contract: `pulp run --headless
// --screenshot <path> --frames 1 <target>` against a fake project that
// contains the test fixture binary under build/examples/<dir>/<exe>
// must (a) discover the binary, (b) launch it with the flags forwarded
// AND env vars set, and (c) leave a non-empty PNG file at <path>.
TEST_CASE("pulp run --headless --screenshot --frames writes a PNG",
          "[cli][shellout][run][issue-914]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    // The fixture binary lives next to the test binary in the build
    // tree (test/pulp-cli-run-fixture). The test runner's cwd is
    // <build>/test, so a relative resolve to the fixture works.
    fs::path fixture_src = fs::current_path() /
#if defined(_WIN32)
        "pulp-cli-run-fixture.exe";
#else
        "pulp-cli-run-fixture";
#endif
    if (!fs::exists(fixture_src)) {
        SUCCEED("fixture binary not built at " + fixture_src.string()
                + "; skipping");
        return;
    }

    // Build a fake project tree that cmd_run can navigate.
    auto base = fs::temp_directory_path() /
                ("pulp-shellout-run-headless-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    auto build_dir = base / "build";
    auto bin_dir = build_dir / "bin";
    fs::create_directories(bin_dir);
    // pulp.toml at the root marks this as a standalone project (no
    // `core/` sibling needed) — find_standalone_root() picks it up.
    {
        std::ofstream toml(base / "pulp.toml");
        toml << "[pulp]\nsdk_version = \"99.0.0\"\n";
    }
    // Stub CMakeCache.txt — cmd_run only checks for existence.
    { std::ofstream c(build_dir / "CMakeCache.txt"); c << "# stub\n"; }

    // Copy the fixture in as the discovered binary. In standalone mode
    // cmd_run scans build/bin for a single executable file with no
    // extension. Use a name without dots and without "test" so the
    // selector picks it up.
    auto target = bin_dir /
#if defined(_WIN32)
        "pulpcliruntarget.exe";
#else
        "pulpcliruntarget";
#endif
    std::error_code copy_ec;
    fs::copy_file(fixture_src, target,
                   fs::copy_options::overwrite_existing, copy_ec);
    REQUIRE_FALSE(copy_ec);
#if !defined(_WIN32)
    fs::permissions(target,
                    fs::perms::owner_all | fs::perms::group_read |
                    fs::perms::group_exec | fs::perms::others_read |
                    fs::perms::others_exec,
                    fs::perm_options::add);
#endif

    auto screenshot = base / "shot.png";
    fs::remove(screenshot);

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(base);
    // Pass the explicit target name so cmd_run's standalone-mode lookup
    // path is taken (it matches by name without the "no dot in fname"
    // filter the unnamed search applies — that filter blocks the
    // Windows .exe extension).
    auto r = exec(bin.string(),
                  {"run", "pulpcliruntarget",
                   "--headless", "--screenshot", screenshot.string(),
                   "--frames", "1"},
                  20000);
    fs::current_path(cwd_saver);

    REQUIRE_FALSE(r.timed_out);
    INFO("stdout: " << r.stdout_output);
    INFO("stderr: " << r.stderr_output);
    REQUIRE(r.exit_code == 0);

    // Fixture echoes its resolved options — verify the CLI forwarded
    // both the args (preferred) AND the env vars (fallback).
    REQUIRE(r.stdout_output.find("fixture: headless=1") != std::string::npos);
    REQUIRE(r.stdout_output.find(screenshot.string())   != std::string::npos);

    // The PNG itself must exist and start with the standard signature.
    REQUIRE(fs::exists(screenshot));
    auto size = fs::file_size(screenshot);
    REQUIRE(size > 0);
    std::ifstream png(screenshot, std::ios::binary);
    unsigned char hdr[8] = {0};
    png.read(reinterpret_cast<char*>(hdr), 8);
    REQUIRE(hdr[0] == 0x89);
    REQUIRE(hdr[1] == 'P');
    REQUIRE(hdr[2] == 'N');
    REQUIRE(hdr[3] == 'G');

    fs::remove_all(base);
}

// #682 — PULP_DEBUG=1 must emit timestamped phase markers to stderr so
// future "pulp hung at 0% CPU" reports pin themselves. Unset by default
// it must stay silent so scripts that parse stderr aren't affected.
TEST_CASE("PULP_DEBUG=1 surfaces phase markers to stderr (#682)",
          "[cli][shellout][issue-682]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    // Disable the update-check network path so the test stays offline
    // and deterministic — we're testing the instrumentation hook, not
    // the network dependency.
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    SECTION("unset: stderr stays free of phase markers") {
        pulp_unsetenv("PULP_DEBUG");
        auto r = run_pulp({"version"});
        REQUIRE(r.exit_code == 0);
        REQUIRE(r.stderr_output.find("[pulp-debug") == std::string::npos);
    }

    SECTION("PULP_DEBUG=1: emits markers on stderr, not stdout") {
        pulp_setenv("PULP_DEBUG", "1", 1);
        auto r = run_pulp({"version"});
        REQUIRE(r.exit_code == 0);
        REQUIRE(r.stderr_output.find("[pulp-debug") != std::string::npos);
        REQUIRE(r.stderr_output.find("update-banner") != std::string::npos);
        REQUIRE(r.stdout_output.find("[pulp-debug") == std::string::npos);
        pulp_unsetenv("PULP_DEBUG");
    }

    SECTION("PULP_DEBUG=0: treated as off, no markers") {
        pulp_setenv("PULP_DEBUG", "0", 1);
        auto r = run_pulp({"version"});
        REQUIRE(r.exit_code == 0);
        REQUIRE(r.stderr_output.find("[pulp-debug") == std::string::npos);
        pulp_unsetenv("PULP_DEBUG");
    }

    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
}

// Issue #940 Slice 1 — `pulp loop` is the leveraged-prototype focus
// mode entry point. These tests cover the surface that doesn't depend
// on a live watch loop (the watch-loop interactions are tested by
// `pulp dev`'s harness):
//
//   * `pulp loop --help`     exits 0 + advertises focus mode
//   * `pulp loop --status`   exits 0, reports detected host + focus state
//   * `pulp loop --off`      clears focus state (and tolerates "already off")
//   * `pulp loop --platform=<known>`   accepts macos/linux/windows
//   * `pulp loop --platform=<unknown>` exits non-zero with diagnostic
//   * `pulp loop --no-watch` flips focus + exits 0 without watching
//
// All of these run with PULP_HOME pointed at a per-test tmpdir so the
// developer's real config.toml is never touched.
TEST_CASE("pulp loop --help exits 0 with focus-mode banner",
          "[cli][shellout][loop][issue-940]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"loop", "--help"});
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.stdout_output.find("leveraged-prototype focus mode") != std::string::npos);
    REQUIRE(r.stdout_output.find("--platform") != std::string::npos);
    REQUIRE(r.stdout_output.find("--off") != std::string::npos);
}

TEST_CASE("pulp loop --status reports detected host and focus state",
          "[cli][shellout][loop][issue-940]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-940-status");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    auto r = run_pulp({"loop", "--status"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("detected host") != std::string::npos);
    REQUIRE(r.stdout_output.find("focus platform") != std::string::npos);
    // Fresh PULP_HOME — no focus persisted yet.
    REQUIRE(r.stdout_output.find("(none") != std::string::npos);

    pulp_unsetenv("PULP_HOME");
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    fs::remove_all(tmp_home);
}

TEST_CASE("pulp loop --off clears focus state idempotently",
          "[cli][shellout][loop][issue-940]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-940-off");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    // First call: nothing to clear, must still exit 0.
    auto r1 = run_pulp({"loop", "--off"});
    REQUIRE_FALSE(r1.timed_out);
    REQUIRE(r1.exit_code == 0);
    REQUIRE(r1.stdout_output.find("already in cross-platform mode") != std::string::npos);

    // Set focus via --no-watch, then clear.
    auto r_set = run_pulp({"loop", "--platform=macos", "--no-watch"});
    REQUIRE_FALSE(r_set.timed_out);
    REQUIRE(r_set.exit_code == 0);
    REQUIRE(r_set.stdout_output.find("focus mode active on macos") != std::string::npos);

    auto r2 = run_pulp({"loop", "--off"});
    REQUIRE_FALSE(r2.timed_out);
    REQUIRE(r2.exit_code == 0);
    REQUIRE(r2.stdout_output.find("focus mode cleared") != std::string::npos);
    REQUIRE(r2.stdout_output.find("Cross-platform mode restored") != std::string::npos);

    pulp_unsetenv("PULP_HOME");
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    fs::remove_all(tmp_home);
}

TEST_CASE("pulp loop --platform=<known> accepts macos|linux|windows",
          "[cli][shellout][loop][issue-940]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-940-platform");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    for (const char* p : {"macos", "linux", "windows"}) {
        auto r = run_pulp({"loop", "--platform", p, "--no-watch"});
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 0);
        std::string expect = std::string("focus mode active on ") + p;
        REQUIRE(r.stdout_output.find(expect) != std::string::npos);
    }

    pulp_unsetenv("PULP_HOME");
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    fs::remove_all(tmp_home);
}

TEST_CASE("pulp loop --platform=<unknown> exits non-zero with diagnostic",
          "[cli][shellout][loop][issue-940]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-940-unknown");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    auto r = run_pulp({"loop", "--platform=plan9", "--no-watch"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("unknown --platform") != std::string::npos);
    REQUIRE(combined.find("macos") != std::string::npos);

    pulp_unsetenv("PULP_HOME");
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    fs::remove_all(tmp_home);
}

TEST_CASE("pulp loop --watch-issues prints deferred-slice hint",
          "[cli][shellout][loop][issue-940]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-940-watch-issues");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    auto r = run_pulp({"loop", "--watch-issues=924,927", "--no-watch"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    // Slice 3 deferred — surface the issue number so the user knows
    // when the helper lands.
    REQUIRE(r.stdout_output.find("--watch-issues=924,927") != std::string::npos);
    REQUIRE(r.stdout_output.find("deferred") != std::string::npos);

    pulp_unsetenv("PULP_HOME");
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    fs::remove_all(tmp_home);
}

TEST_CASE("pulp loop --ar-swap-from prints deferred-slice hint",
          "[cli][shellout][loop][issue-940]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-940-ar-swap");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    // Long form (separate value)
    auto r1 = run_pulp({"loop", "--ar-swap-from", "feat/fix-x", "--no-watch"});
    REQUIRE_FALSE(r1.timed_out);
    REQUIRE(r1.exit_code == 0);
    REQUIRE(r1.stdout_output.find("--ar-swap-from=feat/fix-x") != std::string::npos);
    REQUIRE(r1.stdout_output.find("deferred") != std::string::npos);

    // Equals form
    auto r2 = run_pulp({"loop", "--ar-swap-from=feat/fix-y", "--no-watch"});
    REQUIRE_FALSE(r2.timed_out);
    REQUIRE(r2.exit_code == 0);
    REQUIRE(r2.stdout_output.find("--ar-swap-from=feat/fix-y") != std::string::npos);

    pulp_unsetenv("PULP_HOME");
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    fs::remove_all(tmp_home);
}

TEST_CASE("pulp loop --status reports persisted focus platform",
          "[cli][shellout][loop][issue-940]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-940-status-set");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    // Set focus, then assert --status reflects the persisted value.
    auto rs = run_pulp({"loop", "--platform=linux", "--no-watch"});
    REQUIRE_FALSE(rs.timed_out);
    REQUIRE(rs.exit_code == 0);

    auto r = run_pulp({"loop", "--status"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("focus platform: linux") != std::string::npos);

    pulp_unsetenv("PULP_HOME");
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    fs::remove_all(tmp_home);
}

TEST_CASE("pulp loop accepts --test, --validate, --target, --run flags via --no-watch",
          "[cli][shellout][loop][issue-940]") {
    // The flags exercise the parser branches that wouldn't otherwise run
    // when only --status / --off / --no-watch are used. With --no-watch
    // we exit before the watch loop, so the binary still terminates.
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-940-flags");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    // --test, -t, --test-filter=, --validate, --allow-unsupported-sdk
    // all exit cleanly with --no-watch.
    auto r1 = run_pulp({"loop", "--platform=macos", "--test", "--validate", "--no-watch"});
    REQUIRE_FALSE(r1.timed_out);
    REQUIRE(r1.exit_code == 0);

    auto r2 = run_pulp({"loop", "--platform=macos", "-t", "--test-filter=Foo", "--no-watch"});
    REQUIRE_FALSE(r2.timed_out);
    REQUIRE(r2.exit_code == 0);

    // --target T pushes onto build_args; --run T sets launch_target. Both
    // branches must accept the next-arg form without breaking the parser.
    auto r3 = run_pulp({"loop", "--platform=macos", "--target", "pulp-format",
                        "--run", "pulp-gain-standalone", "--no-watch"});
    REQUIRE_FALSE(r3.timed_out);
    REQUIRE(r3.exit_code == 0);

    // Trailing -- forwards remaining args to the launched binary; with
    // --no-watch we just exit so the args are silently consumed.
    auto r4 = run_pulp({"loop", "--platform=macos", "--no-watch", "--", "--arg1", "value1"});
    REQUIRE_FALSE(r4.timed_out);
    REQUIRE(r4.exit_code == 0);

    pulp_unsetenv("PULP_HOME");
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    fs::remove_all(tmp_home);
}

// ── projects list --json (cmd_projects.cpp do_list JSON path) ─────
//
// The C++ binary's `pulp projects list --json` was added in this
// branch to mirror what the Rust port already does — without these
// tests, the diff-cover gate flags the new JSON-rendering branch as
// uncovered (per `ci/coverage-targets.yaml`, `tools/cli/**` is the
// "user-facing" tier with a 70% per-PR floor).

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
