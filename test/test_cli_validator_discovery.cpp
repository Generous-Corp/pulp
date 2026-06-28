// test_cli_validator_discovery.cpp — Unit tests for `pulp doctor --validators`
//
// Issue #743. Exercises the pure-logic discovery + healing core of
// `tools/cli/validator_discovery.{hpp,cpp}`, with a hand-rolled
// DiscoveryEnv that stubs `path_exists` / `path_owner_uid` /
// `assess_signature` so the four acceptance scenarios in the spec are
// hermetic — no host filesystem, no `spctl`, no PATH dependence.
//
// Scenarios covered (#743 acceptance list):
//   (a) healthy cask install  → ✓ for each tool, exit 0
//   (b) user-owned broken     → ✗ + auto-fixable; --fix removes it
//   (c) root-owned broken     → ✗ + sudo one-liner; --fix is a no-op
//   (d) missing validator     → ⚠ + install command

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/validator_discovery.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
namespace vd = pulp::cli::validator_discovery;

namespace {

// Scoped tempdir — kept tiny because most scenarios don't need real
// files (the env stubs fake existence). We only materialise files for
// the healing tests where we want to assert `fs::remove` actually ran.
struct TempDir {
    fs::path path;
    TempDir() {
        auto base = fs::temp_directory_path();
        static std::atomic<int> seq{0};
        int n = seq.fetch_add(1);
        path = base / ("pulp-validator-discovery-" + std::to_string(
                          reinterpret_cast<std::uintptr_t>(this)) + "-" +
                       std::to_string(n));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Stub env builder. Callers register virtual existing paths, owners,
// and signature verdicts per path. Everything unregistered is treated
// as non-existent / non-owned / signature-failed.
struct StubEnv {
    std::unordered_set<std::string> existing;
    std::unordered_map<std::string, uint32_t> owners;
    std::unordered_map<std::string, bool> verdicts;
    std::unordered_map<std::string, std::string> verdict_text;
    std::unordered_map<std::string, fs::path> path_resolve;
    uint32_t current_uid = 1000;
    fs::path home = "/home/test";

    vd::DiscoveryEnv build() const {
        vd::DiscoveryEnv e;
        auto ex = existing;
        e.path_exists = [ex](const fs::path& p) {
            return ex.count(p.string()) > 0;
        };
        e.path_executable = [ex](const fs::path& p) {
            return ex.count(p.string()) > 0;
        };
        auto ow = owners;
        e.path_owner_uid = [ow](const fs::path& p) -> uint32_t {
            auto it = ow.find(p.string());
            if (it == ow.end()) return std::numeric_limits<uint32_t>::max();
            return it->second;
        };
        e.current_uid = current_uid;
        auto vv = verdicts;
        auto vt = verdict_text;
        e.assess_signature = [vv, vt](const fs::path& p,
                                      const std::string& /*tool*/,
                                      std::string& verdict_line) {
            auto it = vt.find(p.string());
            verdict_line = (it == vt.end()) ? std::string{} : it->second;
            auto vit = vv.find(p.string());
            return vit != vv.end() && vit->second;
        };
        auto pr = path_resolve;
        e.resolve_in_path = [pr](const std::string& t) -> fs::path {
            auto it = pr.find(t);
            return it == pr.end() ? fs::path{} : it->second;
        };
        e.home_dir = home;
        return e;
    }
};

const vd::ValidatorReport& find_report(
    const std::vector<vd::ValidatorReport>& rs, const std::string& name) {
    for (const auto& r : rs) {
        if (r.name == name) return r;
    }
    FAIL("no report for validator '" + name + "'");
    // unreachable — FAIL throws, but compilers want a return path.
    return rs.front();
}

}  // namespace

// The validator-discovery scenarios are macOS-shaped end-to-end:
// hard-coded POSIX paths (`/usr/bin/auval`, `/usr/local/bin/pluginval`),
// uid-based ownership, and the rip-from-bundle / amfid SIGKILL failure
// mode that motivated #743. None of those map to Windows (no `auval`,
// no Gatekeeper, fs::path stringification flips slashes so the StubEnv
// hash lookups miss). The production code already neutralises Windows
// behaviour (see `make_default_env()`'s _WIN32 branches), so gating
// the tests off on Windows keeps coverage honest without disabling
// anything load-bearing for that platform.
#if !defined(_WIN32)

TEST_CASE("healthy cask install reports OK for every validator",
          "[doctor][validators][issue-743]") {
    // Scenario (a) — each validator's first-priority existing path
    // passes spctl assessment, everything else is absent.
    StubEnv env;
    // auval: /usr/bin/auval, user-owned, accepted
    env.existing.insert("/usr/bin/auval");
    env.owners["/usr/bin/auval"] = env.current_uid;
    env.verdicts["/usr/bin/auval"] = true;
    env.verdict_text["/usr/bin/auval"] =
        "/usr/bin/auval: accepted\nsource=Apple System";

    // pluginval — use the cask bundle path, not the rip target.
    env.existing.insert(
        "/Applications/pluginval.app/Contents/MacOS/pluginval");
    env.owners["/Applications/pluginval.app/Contents/MacOS/pluginval"] = 0;
    env.verdicts["/Applications/pluginval.app/Contents/MacOS/pluginval"] = true;
    env.verdict_text["/Applications/pluginval.app/Contents/MacOS/pluginval"] =
        "accepted\nsource=Notarized Developer ID";

    // clap-validator — cargo install location.
    env.existing.insert("/home/test/.cargo/bin/clap-validator");
    env.owners["/home/test/.cargo/bin/clap-validator"] = env.current_uid;
    env.verdicts["/home/test/.cargo/bin/clap-validator"] = true;
    env.verdict_text["/home/test/.cargo/bin/clap-validator"] = "accepted";

    auto reports = vd::discover_validators(env.build());
    REQUIRE(reports.size() == 3);
    for (const auto& r : reports) {
        REQUIRE(r.status == vd::ValidatorStatus::Healthy);
    }
    REQUIRE(vd::compute_exit_code(reports) == 0);
    REQUIRE_FALSE(vd::has_broken_validator(reports));

    auto text = vd::render_report(reports, /*use_color=*/false);
    REQUIRE(text.find("auval: /usr/bin/auval") != std::string::npos);
    REQUIRE(text.find("pluginval: /Applications/pluginval.app") !=
            std::string::npos);
    REQUIRE(text.find("clap-validator: /home/test/.cargo") !=
            std::string::npos);
    // The ✓ path should not print a remediation line.
    REQUIRE(text.find("fix:") == std::string::npos);
}

TEST_CASE("user-owned ripped-out-of-bundle pluginval is auto-fixable",
          "[doctor][validators][issue-743]") {
    // Scenario (b). pluginval exists at /usr/local/bin/pluginval (the
    // historical Homebrew formula path), owned by the current user,
    // spctl rejects it with the classic "invalid resource directory"
    // verdict. The cask bundle is not installed. auval + clap-validator
    // are also present and healthy so we can assert the exit code
    // flips solely on pluginval.
    StubEnv env;
    env.existing.insert("/usr/local/bin/pluginval");
    env.owners["/usr/local/bin/pluginval"] = env.current_uid;
    env.verdicts["/usr/local/bin/pluginval"] = false;
    env.verdict_text["/usr/local/bin/pluginval"] =
        "/usr/local/bin/pluginval: rejected\n"
        "source=invalid resource directory (directory or signature have been modified)";

    env.existing.insert("/usr/bin/auval");
    env.owners["/usr/bin/auval"] = 0;
    env.verdicts["/usr/bin/auval"] = true;
    env.verdict_text["/usr/bin/auval"] = "accepted";

    env.existing.insert("/home/test/.cargo/bin/clap-validator");
    env.owners["/home/test/.cargo/bin/clap-validator"] = env.current_uid;
    env.verdicts["/home/test/.cargo/bin/clap-validator"] = true;
    env.verdict_text["/home/test/.cargo/bin/clap-validator"] = "accepted";

    auto reports = vd::discover_validators(env.build());
    const auto& pv = find_report(reports, "pluginval");
    REQUIRE(pv.status == vd::ValidatorStatus::Broken);
    REQUIRE(pv.path == fs::path("/usr/local/bin/pluginval"));
    REQUIRE(pv.ownership == vd::PathOwnership::UserOwned);
    REQUIRE(pv.fixable_without_sudo);
    REQUIRE(pv.remediation == "rm /usr/local/bin/pluginval");
    REQUIRE(vd::has_broken_validator(reports));
    REQUIRE(vd::compute_exit_code(reports) == 1);

    auto text = vd::render_report(reports, false);
    REQUIRE(text.find("Auto-fixable") != std::string::npos);
    REQUIRE(text.find("invalid resource directory") != std::string::npos);

    // --fix --dry-run: report the count, don't mutate.
    auto dry = reports;
    auto outcome_dry = vd::apply_fixes(dry, /*dry_run=*/true);
    REQUIRE(outcome_dry.auto_fixed == 1);
    REQUIRE(outcome_dry.needs_sudo == 0);
    // Status should still be Broken (we only fake the apply).
    REQUIRE(find_report(dry, "pluginval").status ==
            vd::ValidatorStatus::Broken);
}

TEST_CASE("apply_fixes on user-owned broken copy actually removes the file",
          "[doctor][validators][issue-743]") {
    // End-to-end heal: materialise a real file so we can assert the
    // in-tree `apply_fixes` call runs `fs::remove`. This is the only
    // scenario that needs a real tempdir.
    TempDir tmp;
    auto fake = tmp.path / "usr-local-bin-pluginval";
    { std::ofstream f(fake); f << "not actually pluginval\n"; }

    std::vector<vd::ValidatorReport> reports;
    vd::ValidatorReport r;
    r.name = "pluginval";
    r.format = "VST3";
    r.status = vd::ValidatorStatus::Broken;
    r.path = fake;
    r.ownership = vd::PathOwnership::UserOwned;
    r.fixable_without_sudo = true;
    r.reason = "signature check failed";
    r.remediation = "rm " + fake.string();
    reports.push_back(std::move(r));

    REQUIRE(fs::exists(fake));
    auto outcome = vd::apply_fixes(reports, /*dry_run=*/false);
    REQUIRE(outcome.auto_fixed == 1);
    REQUIRE_FALSE(fs::exists(fake));
    // After a successful fix the status should flip to Missing so the
    // refreshed render advises reinstall.
    REQUIRE(reports.front().status == vd::ValidatorStatus::Missing);
    REQUIRE(reports.front().remediation.find("brew install") !=
            std::string::npos);
}

TEST_CASE("root-owned ripped-out-of-bundle pluginval needs sudo",
          "[doctor][validators][issue-743]") {
    // Scenario (c). Same shape as (b), but /usr/local/bin/pluginval is
    // owned by uid 0 (root). `apply_fixes` must not remove it even
    // with `--fix`; it must print the sudo one-liner and move on.
    StubEnv env;
    env.existing.insert("/usr/local/bin/pluginval");
    env.owners["/usr/local/bin/pluginval"] = 0;
    env.verdicts["/usr/local/bin/pluginval"] = false;
    env.verdict_text["/usr/local/bin/pluginval"] =
        "/usr/local/bin/pluginval: rejected\n"
        "source=invalid resource directory (directory or signature have been modified)";

    env.existing.insert("/usr/bin/auval");
    env.owners["/usr/bin/auval"] = 0;
    env.verdicts["/usr/bin/auval"] = true;
    env.verdict_text["/usr/bin/auval"] = "accepted";

    env.existing.insert("/home/test/.cargo/bin/clap-validator");
    env.owners["/home/test/.cargo/bin/clap-validator"] = env.current_uid;
    env.verdicts["/home/test/.cargo/bin/clap-validator"] = true;
    env.verdict_text["/home/test/.cargo/bin/clap-validator"] = "accepted";

    auto reports = vd::discover_validators(env.build());
    const auto& pv = find_report(reports, "pluginval");
    REQUIRE(pv.status == vd::ValidatorStatus::Broken);
    REQUIRE(pv.ownership == vd::PathOwnership::RootOrSystem);
    REQUIRE_FALSE(pv.fixable_without_sudo);
    REQUIRE(pv.remediation == "sudo rm /usr/local/bin/pluginval");

    auto text = vd::render_report(reports, false);
    REQUIRE(text.find("Root-owned") != std::string::npos);
    REQUIRE(text.find("sudo rm /usr/local/bin/pluginval") != std::string::npos);

    // --fix does NOT mutate root-owned broken paths.
    auto outcome = vd::apply_fixes(reports, /*dry_run=*/false);
    REQUIRE(outcome.auto_fixed == 0);
    REQUIRE(outcome.needs_sudo == 1);
    // Status must remain Broken — the file wasn't removed.
    REQUIRE(find_report(reports, "pluginval").status ==
            vd::ValidatorStatus::Broken);
}

TEST_CASE("missing validators report install command and contribute to exit",
          "[doctor][validators][issue-743]") {
    // Scenario (d). No validator binaries exist anywhere. Every report
    // is Missing with its install hint as remediation.
    StubEnv env;
    auto reports = vd::discover_validators(env.build());
    REQUIRE(reports.size() == 3);
    for (const auto& r : reports) {
        REQUIRE(r.status == vd::ValidatorStatus::Missing);
        REQUIRE_FALSE(r.remediation.empty());
    }
    REQUIRE(vd::compute_exit_code(reports) == 1);
    REQUIRE_FALSE(vd::has_broken_validator(reports));

    auto text = vd::render_report(reports, false);
    REQUIRE(text.find("auval: not installed") != std::string::npos);
    REQUIRE(text.find("pluginval: not installed") != std::string::npos);
    REQUIRE(text.find("clap-validator: not installed") != std::string::npos);
    REQUIRE(text.find("xcode-select --install") != std::string::npos);
    REQUIRE(text.find("brew install --cask pluginval") != std::string::npos);
    REQUIRE(text.find("cargo install clap-validator") != std::string::npos);
}

TEST_CASE("first-existing priority path wins even when a healthy copy is further down",
          "[doctor][validators][issue-743]") {
    // Regression guard: if the user has a broken /usr/local/bin/pluginval
    // AND a healthy /Applications/pluginval.app, the diagnostic must
    // surface the broken one — that's the path the user's shell will
    // dispatch, so that's the copy that will silently SIGKILL under
    // amfid. Masking it with the healthy cask path would defeat the
    // whole point of the check.
    StubEnv env;
    env.existing.insert("/usr/local/bin/pluginval");
    env.owners["/usr/local/bin/pluginval"] = env.current_uid;
    env.verdicts["/usr/local/bin/pluginval"] = false;
    env.verdict_text["/usr/local/bin/pluginval"] = "rejected: invalid resource directory";

    env.existing.insert(
        "/Applications/pluginval.app/Contents/MacOS/pluginval");
    env.owners["/Applications/pluginval.app/Contents/MacOS/pluginval"] = 0;
    env.verdicts["/Applications/pluginval.app/Contents/MacOS/pluginval"] = true;

    auto reports = vd::discover_validators(env.build());
    const auto& pv = find_report(reports, "pluginval");
    REQUIRE(pv.status == vd::ValidatorStatus::Broken);
    REQUIRE(pv.path == fs::path("/usr/local/bin/pluginval"));
}

TEST_CASE("discovery uses PATH-resolved validator when known paths are absent",
          "[doctor][validators]") {
    StubEnv env;
    env.path_resolve["pluginval"] = "/opt/local/bin/pluginval";
    env.existing.insert("/opt/local/bin/pluginval");
    env.owners["/opt/local/bin/pluginval"] = env.current_uid;
    env.verdicts["/opt/local/bin/pluginval"] = true;
    env.verdict_text["/opt/local/bin/pluginval"] = "accepted\nsource=MacPorts";

    auto reports = vd::discover_validators(env.build());
    const auto& pv = find_report(reports, "pluginval");
    REQUIRE(pv.status == vd::ValidatorStatus::Healthy);
    REQUIRE(pv.path == fs::path("/opt/local/bin/pluginval"));
    REQUIRE(pv.reason.find("source=MacPorts") != std::string::npos);
}

TEST_CASE("broken validator with unknown ownership requires manual sudo path",
          "[doctor][validators]") {
    StubEnv env;
    env.existing.insert("/usr/local/bin/pluginval");
    // Deliberately omit owners entry: classify_ownership returns Unknown.
    env.verdicts["/usr/local/bin/pluginval"] = false;
    env.verdict_text["/usr/local/bin/pluginval"] = "rejected: invalid resource directory";

    auto reports = vd::discover_validators(env.build());
    const auto& pv = find_report(reports, "pluginval");
    REQUIRE(pv.status == vd::ValidatorStatus::Broken);
    REQUIRE(pv.ownership == vd::PathOwnership::Unknown);
    REQUIRE_FALSE(pv.fixable_without_sudo);
    REQUIRE(pv.remediation == "sudo rm /usr/local/bin/pluginval");

    auto outcome = vd::apply_fixes(reports, /*dry_run=*/true);
    REQUIRE(outcome.needs_sudo == 1);
    REQUIRE(outcome.auto_fixed == 0);
}

TEST_CASE("apply_fixes preserves Broken status when fs::remove fails",
          "[doctor][validators][issue-743]") {
    // If fs::remove fails (permission flip mid-doctor, sticky bit,
    // racing process) we MUST NOT report
    // success — the broken binary is still on disk and `pulp validate`
    // will keep aborting. The fix counts as still_missing, the report
    // stays Broken with an "auto-fix failed" reason, and auto_fixed
    // does not increment.
    //
    // Trigger fs::remove failure by passing a non-empty directory:
    // single-arg fs::remove only removes empty dirs/files, so a dir
    // with a child returns ec = "Directory not empty". This is the
    // most portable way to provoke a deterministic ec without needing
    // elevated privileges or permission-bit fiddling.
    TempDir tmp;
    auto fake_dir = tmp.path / "broken-pluginval-dir";
    fs::create_directories(fake_dir);
    { std::ofstream f(fake_dir / "child"); f << "x"; }

    std::vector<vd::ValidatorReport> reports;
    vd::ValidatorReport r;
    r.name = "pluginval";
    r.format = "VST3";
    r.status = vd::ValidatorStatus::Broken;
    r.path = fake_dir;
    r.ownership = vd::PathOwnership::UserOwned;
    r.fixable_without_sudo = true;
    r.reason = "signature check failed";
    r.remediation = "rm " + fake_dir.string();
    reports.push_back(std::move(r));

    auto outcome = vd::apply_fixes(reports, /*dry_run=*/false);
    REQUIRE(outcome.auto_fixed == 0);
    REQUIRE(outcome.still_missing == 1);
    // Path is still on disk — fs::remove couldn't delete a non-empty
    // dir. That's the whole point: the diagnostic must not lie.
    REQUIRE(fs::exists(fake_dir));
    // Status stays Broken so `has_broken_validator` keeps returning
    // true and `pulp validate` keeps aborting until the user fixes
    // the underlying problem.
    REQUIRE(reports.front().status == vd::ValidatorStatus::Broken);
    REQUIRE(reports.front().reason.find("auto-fix failed") !=
            std::string::npos);
    REQUIRE(vd::has_broken_validator(reports));
    REQUIRE(vd::compute_exit_code(reports) == 1);
}

TEST_CASE("discovery skips non-executable candidates and falls through to runnable paths",
          "[doctor][validators][issue-743]") {
    // If a high-priority path exists but is not executable (zero-byte
    // placeholder, world-writable text file someone left behind), discovery
    // should skip it and continue scanning the priority list rather than
    // picking a garbage file and reporting it as Healthy or Broken.
    StubEnv env;
    // pluginval: a stale non-exec file at the rip target, AND a
    // perfectly healthy cask bundle copy further down the list.
    env.existing.insert("/usr/local/bin/pluginval");
    env.owners["/usr/local/bin/pluginval"] = env.current_uid;
    // No entry in env.verdicts; no entry in env.path_executable for
    // this path — the StubEnv treats unregistered paths as
    // non-existent for both predicates, so we need to explicitly
    // mark the cask path as executable but the rip target as not.
    // The build() lambda uses `existing` for both path_exists and
    // path_executable; override path_executable below.

    env.existing.insert(
        "/Applications/pluginval.app/Contents/MacOS/pluginval");
    env.owners["/Applications/pluginval.app/Contents/MacOS/pluginval"] = 0;
    env.verdicts["/Applications/pluginval.app/Contents/MacOS/pluginval"] = true;
    env.verdict_text["/Applications/pluginval.app/Contents/MacOS/pluginval"] =
        "accepted\nsource=Notarized Developer ID";

    auto e = env.build();
    // Override path_executable so /usr/local/bin/pluginval is present
    // but NOT executable, and the cask path IS executable. This mirrors
    // the failure mode the P2 review describes (stale non-exec
    // placeholder masking a runnable cask copy further down).
    e.path_executable = [](const fs::path& p) {
        return p == fs::path(
            "/Applications/pluginval.app/Contents/MacOS/pluginval");
    };

    auto reports = vd::discover_validators(e);
    const auto& pv = find_report(reports, "pluginval");
    // Discovery should have skipped the non-exec placeholder and
    // landed on the healthy cask path.
    REQUIRE(pv.status == vd::ValidatorStatus::Healthy);
    REQUIRE(pv.path == fs::path(
        "/Applications/pluginval.app/Contents/MacOS/pluginval"));
}

TEST_CASE("apply_fixes is a no-op on a fully healthy environment",
          "[doctor][validators][issue-743]") {
    // `pulp doctor --validators --fix` on a healthy env must not mutate
    // anything and must exit 0. This is the acceptance criterion
    // "--fix is a no-op on a healthy env".
    StubEnv env;
    env.existing.insert("/usr/bin/auval");
    env.owners["/usr/bin/auval"] = env.current_uid;
    env.verdicts["/usr/bin/auval"] = true;
    env.verdict_text["/usr/bin/auval"] = "accepted";

    env.existing.insert(
        "/Applications/pluginval.app/Contents/MacOS/pluginval");
    env.owners["/Applications/pluginval.app/Contents/MacOS/pluginval"] = 0;
    env.verdicts["/Applications/pluginval.app/Contents/MacOS/pluginval"] = true;
    env.verdict_text["/Applications/pluginval.app/Contents/MacOS/pluginval"] = "accepted";

    env.existing.insert("/home/test/.cargo/bin/clap-validator");
    env.owners["/home/test/.cargo/bin/clap-validator"] = env.current_uid;
    env.verdicts["/home/test/.cargo/bin/clap-validator"] = true;
    env.verdict_text["/home/test/.cargo/bin/clap-validator"] = "accepted";

    auto reports = vd::discover_validators(env.build());
    auto outcome = vd::apply_fixes(reports, /*dry_run=*/false);
    REQUIRE(outcome.auto_fixed == 0);
    REQUIRE(outcome.needs_sudo == 0);
    REQUIRE(outcome.still_missing == 0);
    REQUIRE(outcome.healthy == 3);
    REQUIRE(vd::compute_exit_code(reports) == 0);
}

TEST_CASE("apply_fixes counts mixed healthy missing and root-owned reports",
          "[doctor][validators]") {
    std::vector<vd::ValidatorReport> reports;

    vd::ValidatorReport healthy;
    healthy.name = "auval";
    healthy.status = vd::ValidatorStatus::Healthy;
    reports.push_back(healthy);

    vd::ValidatorReport missing;
    missing.name = "clap-validator";
    missing.status = vd::ValidatorStatus::Missing;
    reports.push_back(missing);

    vd::ValidatorReport root_broken;
    root_broken.name = "pluginval";
    root_broken.status = vd::ValidatorStatus::Broken;
    root_broken.path = "/usr/local/bin/pluginval";
    root_broken.fixable_without_sudo = false;
    reports.push_back(root_broken);

    auto outcome = vd::apply_fixes(reports, /*dry_run=*/false);
    REQUIRE(outcome.healthy == 1);
    REQUIRE(outcome.still_missing == 1);
    REQUIRE(outcome.needs_sudo == 1);
    REQUIRE(outcome.auto_fixed == 0);
}

TEST_CASE("render_report can emit ANSI color wrappers",
          "[doctor][validators]") {
    vd::ValidatorReport healthy;
    healthy.name = "auval";
    healthy.status = vd::ValidatorStatus::Healthy;
    healthy.path = "/usr/bin/auval";
    healthy.reason = "accepted";

    vd::ValidatorReport missing;
    missing.name = "clap-validator";
    missing.status = vd::ValidatorStatus::Missing;
    missing.remediation = "cargo install clap-validator";

    auto text = vd::render_report({healthy, missing}, /*use_color=*/true);
    REQUIRE(text.find("\033[32m") != std::string::npos);
    REQUIRE(text.find("\033[33m") != std::string::npos);
    REQUIRE(text.find("\033[0m") != std::string::npos);
}

#else  // _WIN32

// Windows has no auval / Gatekeeper, and the priority paths are POSIX-shaped.
// Keep at least one compiled assertion so the binary still builds and links
// cleanly on the Windows CI lane (Catch2 main is provided by the harness).
TEST_CASE("validator discovery is macOS-only on Windows",
          "[doctor][validators][issue-743][windows]") {
    SUCCEED("validator discovery scenarios are gated to non-Windows hosts");
}

#endif  // !_WIN32
