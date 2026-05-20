// test_cli_skew_banner.cpp — Plugin ↔ CLI skew banner smoke tests.
//
// Release-discovery Slice 6 (#551). The helper at
// `tools/scripts/cli_version_check.sh` reads
// `pulp doctor --versions --json`'s `plugin_min_cli` field and prints a
// one-line banner on stderr when the installed CLI is older than that
// minimum. These tests shell out to `bash` with a staged `pulp` shim
// on PATH, mirroring the real execution environment skills run in.
//
// Per CLAUDE.md "Tests ship with fixes — NON-NEGOTIABLE": the helper
// has a stable banner surface and once-per-session semantics; its test
// lives here, not in a follow-up coverage PR.

#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace pulp::platform;

namespace {

constexpr int kHelperTimeoutMs = 30000;

// Walk up from the test binary's CWD to find the repo root (marker:
// `.claude-plugin/plugin.json`). CTest runs tests from <build>/test,
// so the ancestor walk tolerates arbitrary nesting depth.
fs::path find_repo_root() {
    if (const char* env = std::getenv("PULP_REPO_ROOT"); env && *env) {
        return fs::path(env);
    }
    auto cwd = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        auto marker = cwd / ".claude-plugin" / "plugin.json";
        if (fs::exists(marker)) return cwd;
        if (!cwd.has_parent_path() || cwd == cwd.parent_path()) break;
        cwd = cwd.parent_path();
    }
    return {};
}

struct TempDir {
    fs::path path;
    TempDir() {
        auto base = fs::temp_directory_path();
        static std::atomic<int> seq{0};
        int n = seq.fetch_add(1);
        path = base / ("pulp-skew-banner-" +
                       std::to_string(
                           reinterpret_cast<std::uintptr_t>(this)) +
                       "-" + std::to_string(n));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Write a synthetic `pulp` shim that returns a canned
// `doctor --versions --json` payload.
void write_pulp_shim(const fs::path& bin_dir,
                     const std::string& cli_raw,
                     const std::string& plugin_min_raw) {
    fs::create_directories(bin_dir);
    auto shim = bin_dir / "pulp";
    std::ofstream f(shim);
    f << "#!/bin/sh\n";
    f << "if [ \"$1\" = \"doctor\" ] && [ \"$2\" = \"--versions\" ] "
         "&& [ \"$3\" = \"--json\" ]; then\n";
    f << "  cat <<'JSON'\n";
    f << "{\n";
    f << "  \"cli\": {\"raw\": \"" << cli_raw
      << "\", \"comparable\": true},\n";
    f << "  \"plugin\": {\"raw\": \"0.8.0\", \"comparable\": true},\n";
    f << "  \"plugin_min_cli\": {\"raw\": \"" << plugin_min_raw
      << "\", \"comparable\": true},\n";
    f << "  \"findings\": []\n";
    f << "}\n";
    f << "JSON\n";
    f << "  exit 0\n";
    f << "fi\n";
    f << "echo \"unexpected shim invocation: $*\" >&2\n";
    f << "exit 99\n";
    f.close();
    fs::permissions(shim,
                    fs::perms::owner_all | fs::perms::group_read |
                    fs::perms::group_exec | fs::perms::others_read |
                    fs::perms::others_exec,
                    fs::perm_options::replace);
}

// Write a bash driver script that sets env, then sources the helper
// and invokes the function. We do this because platform::exec doesn't
// accept an env block — the driver script owns the env state instead.
//
// `extra_before` lets a test pre-seed side state (e.g. session marker)
// before the helper runs.
fs::path write_driver(const fs::path& sandbox,
                      const fs::path& bin_dir,
                      const fs::path& cache_dir,
                      const fs::path& helper,
                      bool disable,
                      const std::string& extra_before = {}) {
    static std::atomic<int> counter{0};
    auto p = sandbox /
             ("driver-" + std::to_string(counter.fetch_add(1)) + ".sh");
    std::ofstream f(p);
    f << "#!/bin/bash\n";
    f << "set -u\n";
    f << "export PATH=\"" << bin_dir.string() << ":$PATH\"\n";
    f << "export PULP_SKEW_CHECK_CACHE=\"" << cache_dir.string() << "\"\n";
    if (disable) {
        f << "export PULP_SKEW_CHECK_DISABLE=1\n";
    } else {
        f << "export PULP_SKEW_CHECK_DISABLE=0\n";
    }
    if (!extra_before.empty()) {
        f << extra_before << "\n";
    }
    f << "source \"" << helper.string() << "\"\n";
    f << "pulp_cli_version_check\n";
    f.close();
    fs::permissions(p,
                    fs::perms::owner_all |
                    fs::perms::group_read | fs::perms::group_exec |
                    fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace);
    return p;
}

struct HelperCtx {
    TempDir sandbox;
    fs::path helper;
    fs::path bin_dir;
    fs::path cache_dir;
    bool enabled;

    HelperCtx() : helper{}, bin_dir{}, cache_dir{}, enabled(false) {
        auto root = find_repo_root();
        if (root.empty()) return;
        helper = root / "tools" / "scripts" / "cli_version_check.sh";
        if (!fs::exists(helper)) return;
        bin_dir = sandbox.path / "bin";
        cache_dir = sandbox.path / "cache";
        fs::create_directories(cache_dir);
        enabled = true;
    }
};

}  // namespace

TEST_CASE("cli_version_check helper prints banner when CLI is behind plugin min",
          "[cli][shellout][skill][issue-551]") {
    HelperCtx ctx;
    if (!ctx.enabled) {
        SUCCEED("skipped: repo root or helper not available in this run");
        return;
    }
    write_pulp_shim(ctx.bin_dir, "0.20.0", "0.31.0");
    auto driver = write_driver(ctx.sandbox.path, ctx.bin_dir, ctx.cache_dir,
                               ctx.helper, /*disable=*/false);

    auto r = exec("/bin/bash", {driver.string()}, kHelperTimeoutMs);
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    // Stable banner copy — mirrored in the helper's comments, the
    // _common/README.md, and the upgrade skill's references.
    REQUIRE(r.stderr_output.find(
                "[pulp] Claude plugin requires CLI >= v0.31.0") !=
            std::string::npos);
    REQUIRE(r.stderr_output.find("installed CLI is v0.20.0") !=
            std::string::npos);
    REQUIRE(r.stderr_output.find("`pulp upgrade`") != std::string::npos);
}

TEST_CASE("cli_version_check helper is silent when CLI is ahead",
          "[cli][shellout][skill][issue-551]") {
    HelperCtx ctx;
    if (!ctx.enabled) { SUCCEED("skipped"); return; }
    write_pulp_shim(ctx.bin_dir, "0.31.0", "0.20.0");
    auto driver = write_driver(ctx.sandbox.path, ctx.bin_dir, ctx.cache_dir,
                               ctx.helper, /*disable=*/false);
    auto r = exec("/bin/bash", {driver.string()}, kHelperTimeoutMs);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stderr_output.find("Claude plugin requires CLI") ==
            std::string::npos);
}

TEST_CASE("cli_version_check helper banners at most once per session",
          "[cli][shellout][skill][issue-551]") {
    // Same-session semantics: the helper keys its "already checked"
    // marker on $PPID. Within a single bash driver script, $PPID is
    // stable across multiple `pulp_cli_version_check` calls — so a
    // second call in the same driver should NOT re-emit the banner
    // even though the skew condition still holds.
    HelperCtx ctx;
    if (!ctx.enabled) { SUCCEED("skipped"); return; }
    write_pulp_shim(ctx.bin_dir, "0.20.0", "0.31.0");

    // Driver that invokes the check twice and records each run's stderr
    // separately so the test can count banner occurrences precisely.
    static std::atomic<int> counter{0};
    auto driver = ctx.sandbox.path /
                  ("rerun-" + std::to_string(counter.fetch_add(1)) + ".sh");
    std::ofstream f(driver);
    f << "#!/bin/bash\n";
    f << "set -u\n";
    f << "export PATH=\"" << ctx.bin_dir.string() << ":$PATH\"\n";
    f << "export PULP_SKEW_CHECK_CACHE=\"" << ctx.cache_dir.string()
      << "\"\n";
    f << "export PULP_SKEW_CHECK_DISABLE=0\n";
    f << "source \"" << ctx.helper.string() << "\"\n";
    f << "echo '=== RUN 1 ===' >&2\n";
    f << "pulp_cli_version_check\n";
    f << "echo '=== RUN 2 ===' >&2\n";
    f << "pulp_cli_version_check\n";
    f.close();
    fs::permissions(driver,
                    fs::perms::owner_all |
                    fs::perms::group_read | fs::perms::group_exec |
                    fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace);

    auto r = exec("/bin/bash", {driver.string()}, kHelperTimeoutMs);
    REQUIRE(r.exit_code == 0);

    // Split the captured stderr at the two RUN markers and count
    // banner occurrences in each half — must be exactly one, in the
    // first run only.
    auto& err = r.stderr_output;
    auto m1 = err.find("=== RUN 1 ===");
    auto m2 = err.find("=== RUN 2 ===");
    REQUIRE(m1 != std::string::npos);
    REQUIRE(m2 != std::string::npos);
    auto run1 = err.substr(m1, m2 - m1);
    auto run2 = err.substr(m2);
    REQUIRE(run1.find("Claude plugin requires") != std::string::npos);
    REQUIRE(run2.find("Claude plugin requires") == std::string::npos);
}

TEST_CASE("cli_version_check helper honours PULP_SKEW_CHECK_DISABLE",
          "[cli][shellout][skill][issue-551]") {
    HelperCtx ctx;
    if (!ctx.enabled) { SUCCEED("skipped"); return; }
    write_pulp_shim(ctx.bin_dir, "0.20.0", "0.31.0");
    auto driver = write_driver(ctx.sandbox.path, ctx.bin_dir, ctx.cache_dir,
                               ctx.helper, /*disable=*/true);
    auto r = exec("/bin/bash", {driver.string()}, kHelperTimeoutMs);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stderr_output.find("Claude plugin requires") ==
            std::string::npos);
}
