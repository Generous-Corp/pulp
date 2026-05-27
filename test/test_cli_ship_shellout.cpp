// Shell-out CLI behaviour tests for `pulp ship`.
//
// Per CLAUDE.md the #295 lesson (silent empty Ed25519 signature) came
// from the CLI ship path never being exercised end-to-end. This file
// shells to the built binary for the non-destructive ship branches
// that are safe to run in CI without real signing material.
//
// Issue #901: every test in this file goes through `cmd_ship`, which
// resolves signing config via CLI flag → env var → `~/.pulp/config.toml`
// (`tools/cli/cmd_ship.cpp:20-30`). On developer machines with
// `signing.android.keystore`, `signing.apple.identity`, or related
// env vars set, the ship code takes a different branch and the
// asserted error wording (e.g. "No Android keystore") never appears.
// The `ShipShelloutFixture` below isolates every test from user state
// by pointing `PULP_HOME` at a per-test empty directory and clearing
// the ship-related env vars; it restores prior values on teardown.

#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::platform;
namespace fs = std::filesystem;

namespace {

inline int pulp_setenv(const char* name, const char* value) {
#ifdef _WIN32
    return _putenv_s(name, value);
#else
    return ::setenv(name, value, 1);
#endif
}

inline int pulp_unsetenv(const char* name) {
#ifdef _WIN32
    return _putenv_s(name, "");
#else
    return ::unsetenv(name);
#endif
}

class ScopedEnvVar {
public:
    explicit ScopedEnvVar(const char* name) : name_(name) {
        if (const char* old = std::getenv(name)) {
            old_value_ = old;
        }
        pulp_unsetenv(name_.c_str());
    }

    ScopedEnvVar(const char* name, const std::string& value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            old_value_ = old;
        }
        pulp_setenv(name_.c_str(), value.c_str());
    }

    ~ScopedEnvVar() {
        if (old_value_) {
            pulp_setenv(name_.c_str(), old_value_->c_str());
        } else {
            pulp_unsetenv(name_.c_str());
        }
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    std::string name_;
    std::optional<std::string> old_value_;
};

fs::path pulp_binary() {
    if (const char* env = std::getenv("PULP_CLI_PATH"); env && *env) {
        return fs::path(env);
    }
    auto build_root = fs::current_path() / "..";
    for (const auto& candidate : {
             build_root / "tools" / "cli" / "pulp-cpp",
             build_root / "tools" / "cli" / "pulp",
             build_root / "pulp",
         }) {
        if (fs::exists(candidate)) {
            return candidate;
        }
    }
    return build_root / "tools" / "cli" / "pulp-cpp";
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

std::string read_text_file(const fs::path& path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
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

fs::path make_fake_bundle(const fs::path& root,
                          std::string_view format,
                          std::string_view bundle_name) {
    auto bundle = root / "build" / std::string(format) / std::string(bundle_name);
    fs::create_directories(bundle / "Contents");
    std::ofstream marker(bundle / "Contents" / "Info.plist");
    marker << "<plist><dict></dict></plist>\n";
    return bundle;
}

void write_ship_config(const fs::path& home, std::string_view text) {
    fs::create_directories(home);
    std::ofstream config(home / "config.toml");
    config << text;
}

// Issue #901: isolate every shell-out from the developer's `~/.pulp/config.toml`
// and from any ship-related env vars they may have exported. `cmd_ship.cpp`
// reads `signing.android.*`, `signing.apple.*`, `PULP_SIGN_IDENTITY`,
// `PULP_APPLE_ID`, `PULP_TEAM_ID`, `ANDROID_STORE_PASS`, `ANDROID_KEY_PASS`
// — any of these will silently flip a test onto a different code path
// and break the asserted error wording.
//
// The fixture points `PULP_HOME` at a per-test empty directory (so
// `read_user_config_value()` finds no `config.toml`) and clears the env
// vars `cmd_ship` consults directly. `ScopedEnvVar`'s destructor
// restores prior values, leaving the developer's environment untouched.
fs::path make_isolated_pulp_home() {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = fs::temp_directory_path()
        / ("pulp-ship-home-" + std::to_string(stamp));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

struct ShipShelloutFixture {
    fs::path home_dir;
    ScopedEnvVar pulp_home;
    ScopedEnvVar android_store_pass;
    ScopedEnvVar android_key_pass;
    ScopedEnvVar pulp_sign_identity;
    ScopedEnvVar pulp_apple_id;
    ScopedEnvVar pulp_team_id;
    ScopedEnvVar pulp_notary_env;
    ScopedEnvVar pulp_notary_key_path;
    ScopedEnvVar pulp_notary_key_id;
    ScopedEnvVar pulp_notary_issuer_id;

    ShipShelloutFixture()
        : home_dir(make_isolated_pulp_home()),
          pulp_home("PULP_HOME", home_dir.string()),
          android_store_pass("ANDROID_STORE_PASS"),
          android_key_pass("ANDROID_KEY_PASS"),
          pulp_sign_identity("PULP_SIGN_IDENTITY"),
          pulp_apple_id("PULP_APPLE_ID"),
          pulp_team_id("PULP_TEAM_ID"),
          pulp_notary_env("PULP_NOTARY_ENV"),
          pulp_notary_key_path("PULP_NOTARY_KEY_PATH"),
          pulp_notary_key_id("PULP_NOTARY_KEY_ID"),
          pulp_notary_issuer_id("PULP_NOTARY_ISSUER_ID") {}

    ~ShipShelloutFixture() {
        std::error_code ec;
        fs::remove_all(home_dir, ec);
    }

    ShipShelloutFixture(const ShipShelloutFixture&) = delete;
    ShipShelloutFixture& operator=(const ShipShelloutFixture&) = delete;
};

}  // namespace

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship outside a project directory errors out",
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

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship sign outside a project directory errors cleanly",
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

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship appcast outside a project errors cleanly",
                 "[cli][shellout][ship]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto r = run_pulp_in(fs::temp_directory_path(),
                         {"ship", "appcast"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship notarize outside a project errors cleanly",
                 "[cli][shellout][ship]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto r = run_pulp_in(fs::temp_directory_path(), {"ship", "notarize"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship check outside a project errors cleanly",
                 "[cli][shellout][ship]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto r = run_pulp_in(fs::temp_directory_path(), {"ship", "check"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship package outside a project errors cleanly",
                 "[cli][shellout][ship]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto r = run_pulp_in(fs::temp_directory_path(),
                         {"ship", "package", "--version", "1.0.0"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship help (or default) enumerates every subcommand",
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
        for (const char* sub : {"sign", "notarize", "package", "appcast",
                                "check", "auv3-xcodeproj"}) {
            INFO("ship help missing subcommand: " << sub);
            REQUIRE(contains(r.stdout_output, sub));
        }
    } else {
        REQUIRE(contains(combined, "Pulp project"));
    }
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship rejects unknown subcommands before side effects",
                 "[cli][shellout][ship][help][coverage]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("unknown-subcommand", true);

    auto r = run_pulp_in(root, {"ship", "spaceship", "--identity", "fake-id"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 2);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(contains(combined, "unknown subcommand"));
    REQUIRE(contains(combined, "spaceship"));
    REQUIRE_FALSE(contains(combined, "Subcommands:"));
    REQUIRE_FALSE(contains(combined, "Signing "));

    fs::remove_all(root);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship inside project without build cache reports build guidance",
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

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship sign in project without identity reports signing guidance",
                 "[cli][shellout][ship][issue-643][issue-901]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("missing-identity", true);

    auto r = run_pulp_in(root, {"ship", "sign"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);

    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(contains(combined, "No signing identity specified"));
    REQUIRE(contains(combined, "pulp ship sign --identity"));

    fs::remove_all(root);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship sign discovers desktop bundles via env and config identities",
                 "[cli][shellout][ship][coverage]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("sign-bundles", true);
    make_fake_bundle(root, "VST3", "FakeShipPlugin.vst3");
    make_fake_bundle(root, "CLAP", "FakeShipPlugin.clap");
    make_fake_bundle(root, "AU", "FakeShipPlugin.component");

    {
        ScopedEnvVar identity("PULP_SIGN_IDENTITY", "Developer ID Application: Env");
        auto env_sign = run_pulp_in(root, {"ship", "sign"});
        REQUIRE_FALSE(env_sign.timed_out);
        REQUIRE(env_sign.exit_code != 0);
        auto combined = env_sign.stdout_output + env_sign.stderr_output;
        REQUIRE_FALSE(contains(combined, "No signing identity specified"));
        REQUIRE(contains(combined, "Signing FakeShipPlugin.vst3"));
        REQUIRE(contains(combined, "Signing FakeShipPlugin.clap"));
        REQUIRE(contains(combined, "Signing FakeShipPlugin.component"));
        REQUIRE(contains(combined, "FAILED"));
    }

    write_ship_config(home_dir,
        "[signing.apple]\n"
        "identity = \"Developer ID Application: Config\"\n");
    auto config_sign = run_pulp_in(root, {"ship", "sign"});
    REQUIRE_FALSE(config_sign.timed_out);
    REQUIRE(config_sign.exit_code != 0);
    auto config_combined = config_sign.stdout_output + config_sign.stderr_output;
    REQUIRE_FALSE(contains(config_combined, "No signing identity specified"));
    REQUIRE(contains(config_combined, "Signing FakeShipPlugin.vst3"));

    fs::remove_all(root);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship validates option parser errors before side effects",
                 "[cli][shellout][ship][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("parser-errors", true);

    struct ParserCase {
        std::vector<std::string> args;
        const char* message;
    };
    std::vector<ParserCase> cases = {
        {{"ship", "sign", "--identity"}, "--identity requires a value"},
        {{"ship", "sign", "--bogus"}, "unknown argument"},
        {{"ship", "package", "--version"}, "--version requires a value"},
        {{"ship", "package", "--abi"}, "--abi requires a value"},
        {{"ship", "package", "--bogus"}, "unknown argument"},
        {{"ship", "check", "--target"}, "--target requires a value"},
        {{"ship", "check", "--bogus"}, "unknown argument"},
        {{"ship", "appcast", "--url"}, "--url requires a value"},
        {{"ship", "appcast", "--output"}, "--output requires a value"},
        {{"ship", "appcast", "--bogus"}, "unknown argument"},
    };
#ifdef __APPLE__
    cases.insert(cases.end(), {
        {{"ship", "notarize", "--apple-id"}, "--apple-id requires a value"},
        {{"ship", "notarize", "--team-id"}, "--team-id requires a value"},
        {{"ship", "notarize", "--password"}, "--password requires a value"},
        {{"ship", "notarize", "--bogus"}, "unknown argument"},
        {{"ship", "release", "--target"}, "--target requires a value"},
        {{"ship", "release", "--identity"}, "--identity requires a value"},
        {{"ship", "release", "--apple-id"}, "--apple-id requires a value"},
        {{"ship", "release", "--team-id"}, "--team-id requires a value"},
        {{"ship", "release", "--password"}, "--password requires a value"},
        {{"ship", "release", "--version"}, "--version requires a value"},
    });
#endif

    for (const auto& c : cases) {
        INFO("ship args under test");
        auto r = run_pulp_in(root, c.args);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 2);
        REQUIRE(contains(r.stdout_output + r.stderr_output, c.message));
        REQUIRE_FALSE(contains(r.stdout_output + r.stderr_output, "Signing "));
        REQUIRE_FALSE(contains(r.stdout_output + r.stderr_output, "Packaging "));
        REQUIRE_FALSE(contains(r.stdout_output + r.stderr_output, "Appcast written"));
    }

    fs::remove_all(root);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship check reports desktop bundle signing status without credentials",
                 "[cli][shellout][ship][coverage]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("check-desktop", true);
    make_fake_bundle(root, "VST3", "CheckMe.vst3");
    make_fake_bundle(root, "CLAP", "CheckMe.clap");
    make_fake_bundle(root, "AU", "CheckMe.component");
    {
        std::ofstream ignored(root / "build" / "VST3" / "notes.txt");
        ignored << "not a bundle";
    }

    auto r = run_pulp_in(root, {"ship", "check"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(contains(r.stdout_output, "CheckMe.vst3: unsigned"));
    REQUIRE(contains(r.stdout_output, "CheckMe.clap: unsigned"));
    REQUIRE(contains(r.stdout_output, "CheckMe.component: unsigned"));
    REQUIRE_FALSE(contains(r.stdout_output, "notes.txt"));

    fs::remove_all(root);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship Android validation paths fail before external tooling",
                 "[cli][shellout][ship][android][issue-643][issue-901]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("android-validation", true);
    // PULP_HOME / ANDROID_STORE_PASS / ANDROID_KEY_PASS isolation now
    // lives on the fixture (#901) — no need to scope them locally.

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

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship appcast uses safe defaults and preserves existing invalid feed fallback",
                 "[cli][shellout][ship][appcast][coverage]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("appcast-defaults", true);
    auto feed = root / "artifacts" / "defaults.xml";
    fs::create_directories(feed.parent_path());
    {
        std::ofstream existing(feed);
        existing << "<rss><not-an-appcast></rss>";
    }

    auto r = run_pulp_in(root,
        {"ship", "appcast",
         "--url", "https://example.com/FakeShipPlugin-default.pkg",
         "--output", feed.string()});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(contains(r.stdout_output, "(1 items)"));

    auto xml = read_text_file(feed);
    REQUIRE(contains(xml, "Plugin Updates"));
    REQUIRE(contains(xml, "Version 0.1.0"));
    REQUIRE(contains(xml, "https://example.com/FakeShipPlugin-default.pkg"));
    REQUIRE_FALSE(contains(xml, "not-an-appcast"));

    fs::remove_all(root);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship appcast writes local feed and rejects remote signing",
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

    auto bare_output = run_pulp_in(root,
        {"ship", "appcast",
         "--url", "https://example.com/FakeShipPlugin-2.3.5.pkg",
         "--version", "2.3.5",
         "--output", "appcast.xml"});
    REQUIRE_FALSE(bare_output.timed_out);
    REQUIRE(bare_output.exit_code == 0);
    REQUIRE(fs::exists(root / "appcast.xml"));

    auto remote_sign = run_pulp_in(root,
        {"ship", "appcast",
         "--url", "https://example.com/FakeShipPlugin-2.3.6.pkg",
         "--version", "2.3.6",
         "--sign-key", "not-a-real-key"});
    REQUIRE_FALSE(remote_sign.timed_out);
    REQUIRE(remote_sign.exit_code != 0);
    REQUIRE(contains(remote_sign.stdout_output + remote_sign.stderr_output,
                     "--sign-key requires a local file path"));

    fs::remove_all(root);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship appcast appends local artifact metadata",
                 "[cli][shellout][ship][appcast][coverage]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("appcast-local", true);
    auto feed = root / "artifacts" / "updates.xml";
    auto artifact = root / "artifacts" / "FakeShipPlugin-3.0.0.pkg";
    fs::create_directories(artifact.parent_path());
    {
        std::ofstream out(artifact, std::ios::binary);
        out << "pkgdata";
    }
    REQUIRE(fs::file_size(artifact) == 7);

    auto first = run_pulp_in(root,
        {"ship", "appcast",
         "--url", artifact.string(),
         "--version", "3.0.0",
         "--notes", "local artifact",
         "--title", "Local Feed",
         "--min-os", "14.0",
         "--output", feed.string()});
    REQUIRE_FALSE(first.timed_out);
    REQUIRE(first.exit_code == 0);
    REQUIRE(contains(first.stdout_output, "(1 items)"));
    REQUIRE(fs::exists(feed));

    auto first_xml = read_text_file(feed);
    REQUIRE(contains(first_xml, "Local Feed"));
    REQUIRE(contains(first_xml, "Version 3.0.0"));
    REQUIRE(contains(first_xml, "<sparkle:shortVersionString>3.0.0</sparkle:shortVersionString>"));
    REQUIRE(contains(first_xml, "<p>local artifact</p>"));
    REQUIRE(contains(first_xml, "<sparkle:minimumSystemVersion>14.0</sparkle:minimumSystemVersion>"));
    REQUIRE(contains(first_xml, "length=\"7\""));
    REQUIRE(contains(first_xml, artifact.string()));
    REQUIRE_FALSE(contains(first_xml, "sparkle:edSignature"));

    auto second = run_pulp_in(root,
        {"ship", "appcast",
         "--url", "https://example.com/FakeShipPlugin-3.1.0.pkg",
         "--version", "3.1.0",
         "--notes", "remote artifact",
         "--output", feed.string()});
    REQUIRE_FALSE(second.timed_out);
    REQUIRE(second.exit_code == 0);
    REQUIRE(contains(second.stdout_output, "(2 items)"));

    auto second_xml = read_text_file(feed);
    REQUIRE(contains(second_xml, "Version 3.1.0"));
    REQUIRE(contains(second_xml, "Version 3.0.0"));
    REQUIRE(second_xml.find("Version 3.1.0") < second_xml.find("Version 3.0.0"));
    REQUIRE(contains(second_xml, "https://example.com/FakeShipPlugin-3.1.0.pkg"));
    REQUIRE(contains(second_xml, artifact.string()));
    REQUIRE(contains(second_xml, "length=\"0\""));

    fs::remove_all(root);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship appcast fails closed on invalid local signing keys",
                 "[cli][shellout][ship][appcast][coverage]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("appcast-bad-sign", true);
    auto artifact = root / "artifacts" / "FakeShipPlugin-4.0.0.pkg";
    auto feed = root / "artifacts" / "signed.xml";
    fs::create_directories(artifact.parent_path());
    {
        std::ofstream out(artifact, std::ios::binary);
        out << "pkgdata";
    }
    REQUIRE(fs::file_size(artifact) == 7);

    auto bad_sign = run_pulp_in(root,
        {"ship", "appcast",
         "--url", artifact.string(),
         "--version", "4.0.0",
         "--sign-key", "not-base64",
         "--output", feed.string()});
    REQUIRE_FALSE(bad_sign.timed_out);
    REQUIRE(bad_sign.exit_code != 0);
    REQUIRE(contains(bad_sign.stdout_output + bad_sign.stderr_output,
                     "Ed25519 signing failed"));
    REQUIRE_FALSE(fs::exists(feed));

    fs::remove_all(root);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship android check tolerates empty artifacts directory",
                 "[cli][shellout][ship][android][coverage]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("android-empty-artifacts", true);
    fs::create_directories(root / "artifacts");
    {
        std::ofstream out(root / "artifacts" / "readme.txt");
        out << "not an Android artifact";
    }

    auto r = run_pulp_in(root, {"ship", "check", "--target", "android"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("readme.txt") == std::string::npos);
    REQUIRE(r.stderr_output.empty());

    fs::remove_all(root);
}

#ifndef _WIN32
TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship package with no plugin bundles reports zero artifacts",
                 "[cli][shellout][ship][package][coverage]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("package-empty", true);

    auto r = run_pulp_in(root, {"ship", "package", "--version", "9.9.9"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(contains(r.stdout_output, "Created 0 .pkg and 0 .dmg artifacts"));
    REQUIRE(fs::exists(root / "artifacts"));
    REQUIRE(fs::is_empty(root / "artifacts"));

    fs::remove_all(root);
}
#endif

// Item 7.5 (macos-plugin-authoring-plan): per-artifact .pkg / .dmg
// selection. The parser-level checks belong here next to the other
// `ship package` arg cases; real codesign + pkgbuild integration
// runs on the self-hosted Mac runner.
TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship package rejects mutually-exclusive --pkg + --dmg",
                 "[cli][shellout][ship][package][macos-7.5]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("pkg-vs-dmg", true);

    auto r = run_pulp_in(root,
        {"ship", "package", "--version", "1.0.0", "--pkg", "--dmg"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(contains(combined, "mutually exclusive"));

    fs::remove_all(root);
}

// Item 7.4 (macos-plugin-authoring-plan): `pulp ship release` orchestrates
// sign → package → notarize → staple as one command. The shellout
// covers argv parsing and the macOS-only guard; the real signing /
// notarization round-trip lives on the self-hosted Mac runner because
// it needs Apple credentials.
TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship release argv parsing surfaces flag errors before side effects",
                 "[cli][shellout][ship][release][macos-7.4]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("release-parse", true);

    // Unknown flag — must fail with exit 2 before any pkgbuild invocation.
    auto bogus = run_pulp_in(root, {"ship", "release", "--bogus"});
    REQUIRE_FALSE(bogus.timed_out);
    REQUIRE(bogus.exit_code != 0);
    auto bogus_combined = bogus.stdout_output + bogus.stderr_output;
    REQUIRE(contains(bogus_combined, "unknown argument"));

    // --pkg and --dmg are mutually exclusive at the release level too.
    auto conflict = run_pulp_in(root,
        {"ship", "release", "--target", "macos", "--pkg", "--dmg"});
    REQUIRE_FALSE(conflict.timed_out);
    REQUIRE(conflict.exit_code != 0);
    REQUIRE(contains(conflict.stdout_output + conflict.stderr_output,
                     "mutually exclusive"));

    // Unsupported --target → fail loudly. Only macos is wired for now.
    auto unsupported = run_pulp_in(root,
        {"ship", "release", "--target", "windows"});
    REQUIRE_FALSE(unsupported.timed_out);
    REQUIRE(unsupported.exit_code != 0);

    fs::remove_all(root);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship release aborts before later stages when signing fails",
                 "[cli][shellout][ship][release][macos-7.4][coverage]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("release-sign-fail", true);

    auto r = run_pulp_in(root,
        {"ship", "release", "--target", "macos",
         "--identity", "Developer ID Application: Missing",
         "--skip-package", "--skip-notarize"});
    REQUIRE_FALSE(r.timed_out);
    auto combined = r.stdout_output + r.stderr_output;
#ifdef __APPLE__
    REQUIRE(r.exit_code != 0);
    REQUIRE(contains(combined, "Stage 1/4"));
    REQUIRE(contains(combined, "No plugin bundles found"));
    REQUIRE(contains(combined, "sign stage failed"));
    REQUIRE_FALSE(contains(combined, "Stage 2/4"));
#else
    REQUIRE(r.exit_code != 0);
    REQUIRE(contains(combined, "macOS"));
#endif

    fs::remove_all(root);
}

// Item 7.4 acceptance: when sign + notarize stages can be skipped, the
// `release` orchestration completes without Apple credentials so CI
// can verify the wiring even on hosts that have no signing identity.
// We don't have built plugin bundles in this fake project, so we use
// --skip-sign and --skip-package as well to confirm the orchestration
// short-circuits cleanly. The real e2e runs on the Mac runner.
TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship release --skip-{sign,package,notarize} runs orchestration to clean exit",
                 "[cli][shellout][ship][release][macos-7.4]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("release-skip-all", true);

    auto r = run_pulp_in(root, {"ship", "release", "--target", "macos",
                                "--skip-sign", "--skip-package",
                                "--skip-notarize"});
    REQUIRE_FALSE(r.timed_out);
    // On macOS this should succeed; on non-Apple the orchestration
    // refuses with a clear error. Either is acceptable here — we just
    // need to confirm the argv was parsed and the stages were reached.
    auto combined = r.stdout_output + r.stderr_output;
#ifdef __APPLE__
    REQUIRE(r.exit_code == 0);
    REQUIRE(contains(combined, "Stage 1/4"));
    REQUIRE(contains(combined, "SKIPPED"));
#else
    REQUIRE(r.exit_code != 0);
    REQUIRE(contains(combined, "macOS"));
#endif

    fs::remove_all(root);
}

// Item 7.4b (macos-plugin-authoring-plan): `pulp build --install` exists
// and rejects invalid flag combinations before touching the filesystem.
// The end-to-end install path is unit-tested via install_paths_mac;
// here we cover the CLI-surface contract.
TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp build --skip-validation without --install is rejected",
                 "[cli][shellout][build][install][macos-7.4b]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("build-skip-no-install", true);

    auto r = run_pulp_in(root, {"build", "--skip-validation"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    REQUIRE(contains(r.stdout_output + r.stderr_output,
                     "--skip-validation only applies with --install"));

    fs::remove_all(root);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp build --install + --watch is rejected",
                 "[cli][shellout][build][install][macos-7.4b]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("build-install-watch", true);

    auto r = run_pulp_in(root, {"build", "--install", "--watch"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    REQUIRE(contains(r.stdout_output + r.stderr_output,
                     "--install cannot be combined with --watch"));

    fs::remove_all(root);
}

// ── auv3-xcodeproj (item 3.10 follow-up) ─────────────────────────────────
//
// The Xcode-project flow shells out to `cmake -G Xcode` and so cannot
// actually be exercised end-to-end without Xcode installed. These tests
// pin the argument-parser contract (required target name, --sdk
// validation) and the macOS-only guard. The successful path is covered
// by `--dry-run`, which prints the resolved cmake invocation without
// spawning it — that asserts the SDK + entitlements wiring without
// needing Xcode in CI.

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship auv3-xcodeproj requires a target name",
                 "[cli][shellout][ship][auv3-xcodeproj][macos-3.10]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("auv3-xcodeproj-no-target", true);

    auto r = run_pulp_in(root, {"ship", "auv3-xcodeproj"});
    REQUIRE_FALSE(r.timed_out);
#ifndef __APPLE__
    // Non-Apple platforms short-circuit with a clear OS-gate error
    // before parsing args. Exit non-zero is the contract.
    REQUIRE(r.exit_code != 0);
#else
    REQUIRE(r.exit_code != 0);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(contains(combined, "Usage: pulp ship auv3-xcodeproj"));
#endif
    fs::remove_all(root);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship auv3-xcodeproj rejects unknown --sdk values",
                 "[cli][shellout][ship][auv3-xcodeproj][macos-3.10]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("auv3-xcodeproj-bad-sdk", true);

    auto r = run_pulp_in(root,
        {"ship", "auv3-xcodeproj", "MyPlugin", "--sdk", "bogus"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
#ifdef __APPLE__
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(contains(combined, "unknown --sdk value"));
#endif
    fs::remove_all(root);
}

#ifdef __APPLE__
TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship auv3-xcodeproj parser rejects missing values and extra positionals",
                 "[cli][shellout][ship][auv3-xcodeproj][macos-3.10][coverage]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("auv3-xcodeproj-parser", true);

    const struct {
        std::vector<std::string> args;
        const char* message;
    } cases[] = {
        {{"ship", "auv3-xcodeproj", "MyPlugin", "--sdk"}, "--sdk requires a value"},
        {{"ship", "auv3-xcodeproj", "MyPlugin", "--output"}, "--output requires a value"},
        {{"ship", "auv3-xcodeproj", "MyPlugin", "OtherPlugin"}, "unknown argument"},
        {{"ship", "auv3-xcodeproj", "MyPlugin", "--bogus"}, "unknown argument"},
    };

    for (const auto& c : cases) {
        auto r = run_pulp_in(root, c.args);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 2);
        REQUIRE(contains(r.stdout_output + r.stderr_output, c.message));
        REQUIRE_FALSE(contains(r.stdout_output + r.stderr_output,
                               "generating Xcode project"));
    }

    fs::remove_all(root);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship auv3-xcodeproj --dry-run prints the resolved cmake invocation",
                 "[cli][shellout][ship][auv3-xcodeproj][macos-3.10]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("auv3-xcodeproj-dry-run", true);

    auto r = run_pulp_in(root,
        {"ship", "auv3-xcodeproj", "MyPlugin", "--sdk", "iphonesimulator",
         "--dry-run"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    auto combined = r.stdout_output + r.stderr_output;
    // Resolved cmake invocation must include the Xcode generator, the
    // target name, and the simulator toolchain.
    REQUIRE(contains(combined, "-G Xcode"));
    REQUIRE(contains(combined, "MyPlugin"));
    REQUIRE(contains(combined, "ios.toolchain.cmake"));
    REQUIRE(contains(combined, "SIMULATOR64"));

    fs::remove_all(root);
}

// ── ASC API key notary flow (2026-05-26) ────────────────────────────
//
// `pulp ship notarize --dry-run` prints the resolved `xcrun notarytool`
// argv without contacting Apple. These tests pin the resolution
// precedence (CLI > env > file) and verify that the modern
// `--key/--key-id/--issuer` flags appear in the dry-run output when
// ASC credentials are present, falling back to the legacy
// `--apple-id/--team-id/--password` flags when only those are set.
//
// Build-cache fixture is enough — `--dry-run` short-circuits before
// looking for bundles, but the fake CMakeCache.txt is still required
// by the early `find_project_root()` guard.

#ifdef __APPLE__
TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship notarize --dry-run with --api-key flags emits notarytool ASC argv",
                 "[cli][shellout][ship][notarize][asc-key]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("notarize-asc-cli", true);

    auto r = run_pulp_in(root,
        {"ship", "notarize", "--dry-run",
         "--api-key", "/path/to/AuthKey_FAKE.p8",
         "--api-key-id", "FAKEKEYID",
         "--api-issuer", "fake-issuer-uuid"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(contains(combined, "App Store Connect API key"));
    REQUIRE(contains(combined, "xcrun notarytool submit"));
    REQUIRE(contains(combined, "--key \"/path/to/AuthKey_FAKE.p8\""));
    REQUIRE(contains(combined, "--key-id \"FAKEKEYID\""));
    REQUIRE(contains(combined, "--issuer \"fake-issuer-uuid\""));
    // Must NOT fall through to the legacy flags
    REQUIRE_FALSE(contains(combined, "--apple-id"));
    // Source diagnostics print the resolution origin
    REQUIRE(contains(combined, "(from cli)"));

    fs::remove_all(root);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship notarize --dry-run reads notary.env via --env-file",
                 "[cli][shellout][ship][notarize][asc-key]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("notarize-asc-envfile", true);

    auto env_path = root / "notary.env";
    {
        std::ofstream out(env_path);
        out << "PULP_NOTARY_KEY_ID=\"FILE_KEY_ID\"\n"
            << "PULP_NOTARY_ISSUER_ID=\"file-issuer-uuid\"\n"
            << "PULP_NOTARY_KEY_PATH=\"" << (root / "fake.p8").string() << "\"\n";
    }

    auto r = run_pulp_in(root,
        {"ship", "notarize", "--dry-run",
         "--env-file", env_path.string()});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(contains(combined, "Loaded notary credentials from"));
    REQUIRE(contains(combined, "App Store Connect API key"));
    REQUIRE(contains(combined, "--key-id \"FILE_KEY_ID\""));
    REQUIRE(contains(combined, "--issuer \"file-issuer-uuid\""));
    REQUIRE(contains(combined, "(from file)"));

    fs::remove_all(root);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship notarize --dry-run CLI flag beats env-file value",
                 "[cli][shellout][ship][notarize][asc-key][precedence]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("notarize-asc-precedence", true);

    auto env_path = root / "notary.env";
    {
        std::ofstream out(env_path);
        out << "PULP_NOTARY_KEY_ID=\"FILE_KEY_ID\"\n"
            << "PULP_NOTARY_ISSUER_ID=\"file-issuer\"\n"
            << "PULP_NOTARY_KEY_PATH=\"" << (root / "fake.p8").string() << "\"\n";
    }

    auto r = run_pulp_in(root,
        {"ship", "notarize", "--dry-run",
         "--env-file", env_path.string(),
         "--api-key-id", "CLI_KEY_ID"});  // overrides file value
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(contains(combined, "--key-id \"CLI_KEY_ID\""));
    REQUIRE_FALSE(contains(combined, "--key-id \"FILE_KEY_ID\""));
    // Issuer + key path still come from the file
    REQUIRE(contains(combined, "--issuer \"file-issuer\""));

    fs::remove_all(root);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship notarize with no creds reports both lanes in error",
                 "[cli][shellout][ship][notarize][asc-key]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("notarize-no-creds", true);

    // Point env override at a non-existent file so the resolver finds
    // no notary.env and the user has no env vars set (fixture clears
    // them).
    ScopedEnvVar override("PULP_NOTARY_ENV", (root / "no-such.env").string());

    auto r = run_pulp_in(root, {"ship", "notarize", "--dry-run"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(contains(combined, "no notary credentials resolved"));
    REQUIRE(contains(combined, "App Store Connect API key"));
    REQUIRE(contains(combined, "--api-key"));
    REQUIRE(contains(combined, "--apple-id"));  // legacy lane still documented

    fs::remove_all(root);
}

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship notarize --dry-run falls back to legacy flags when ASC absent",
                 "[cli][shellout][ship][notarize][legacy]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("notarize-legacy", true);
    ScopedEnvVar override("PULP_NOTARY_ENV", (root / "no-such.env").string());

    auto r = run_pulp_in(root,
        {"ship", "notarize", "--dry-run",
         "--apple-id", "you@example.com",
         "--team-id", "ABCDE12345"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(contains(combined, "legacy Apple ID"));
    REQUIRE(contains(combined, "--apple-id \"you@example.com\""));
    REQUIRE(contains(combined, "--team-id \"ABCDE12345\""));
    // Default password fallback should appear
    REQUIRE(contains(combined, "--password \"@keychain:AC_PASSWORD\""));

    fs::remove_all(root);
}
#endif // __APPLE__

TEST_CASE_METHOD(ShipShelloutFixture,
                 "pulp ship auv3-xcodeproj --sdk macosx skips iOS toolchain",
                 "[cli][shellout][ship][auv3-xcodeproj][macos-3.10]") {
    if (!binary_exists()) { SUCCEED("pulp binary not built"); return; }
    auto root = make_fake_project("auv3-xcodeproj-macosx", true);

    auto r = run_pulp_in(root,
        {"ship", "auv3-xcodeproj", "MacPlugin", "--sdk", "macosx",
         "--dry-run"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(contains(combined, "CMAKE_OSX_SYSROOT=macosx"));
    // Must NOT pull in the iOS toolchain when targetting native macOS.
    REQUIRE_FALSE(contains(combined, "ios.toolchain.cmake"));

    fs::remove_all(root);
}
#endif
