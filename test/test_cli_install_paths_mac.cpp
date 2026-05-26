// Unit tests for install_paths_mac — macOS plugin-install path resolution
// and the bundle-swap state machine that backs `pulp build --install`.
//
// Per CLAUDE.md "Tests ship with fixes — NON-NEGOTIABLE": item 7.4b
// in macos-plugin-authoring-plan adds a `--install` flag to `pulp build`
// that promotes built plugin bundles into `~/Library/Audio/Plug-Ins/...`
// after validation passes. Three things must hold for the user to trust
// the flag:
//
//   1. Path resolution is correct per extension (`.component` → Components/,
//      `.vst3` → VST3/, `.clap` → CLAP/). The plan calls these out
//      explicitly; if we land at the wrong path the DAW won't see the
//      plugin even though `pulp build --install` reported success.
//   2. The swap is idempotent — a re-run replaces, not duplicates.
//      Without the explicit `remove_all` before `copy_recursive` step,
//      `fs::copy(recursive | overwrite_existing)` doesn't actually
//      delete stale files from the destination bundle, so an older
//      plugin binary can survive a fresh install.
//   3. Failure cases (missing $HOME, missing source bundle, validator
//      not on PATH) surface clear errors instead of silently corrupting
//      the user's plug-ins folder.
//
// All I/O is mediated through the `InstallEnv` interface so the tests
// run on any host with no `~/Library/Audio/Plug-Ins/` side effects.

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/install_paths_mac.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace ip = pulp::cli::install_paths_mac;
namespace fs = std::filesystem;

namespace {

// Scenario-grade stub env. Tracks every filesystem mutation the
// install_bundle() call performs so tests can assert order + content.
struct StubEnv {
    ip::InstallEnv env;
    std::set<fs::path> existing;             // paths that exist
    std::vector<fs::path> created_dirs;      // mkdir -p log
    std::vector<fs::path> removed;           // rm -rf log
    std::vector<std::pair<fs::path, fs::path>> copies;
    std::set<std::string> path_tools;        // tools that resolve_in_path finds
    bool create_dir_should_fail = false;
    bool remove_should_fail = false;
    bool copy_should_fail = false;

    StubEnv(const fs::path& home, const std::set<fs::path>& initial = {})
        : existing(initial) {
        env.home_dir = home;
        env.path_exists = [this](const fs::path& p) {
            return existing.count(p) > 0;
        };
        env.create_directories = [this](const fs::path& p) {
            created_dirs.push_back(p);
            if (create_dir_should_fail) return false;
            existing.insert(p);
            return true;
        };
        env.remove_all = [this](const fs::path& p) {
            removed.push_back(p);
            if (remove_should_fail) return false;
            existing.erase(p);
            return true;
        };
        env.copy_recursive = [this](const fs::path& src, const fs::path& dst) {
            copies.push_back({src, dst});
            if (copy_should_fail) return false;
            existing.insert(dst);
            return true;
        };
        env.resolve_in_path = [this](const std::string& name) -> fs::path {
            if (path_tools.count(name) > 0) return fs::path("/usr/local/bin") / name;
            return {};
        };
    }
};

}  // namespace

TEST_CASE("install_paths_mac: classify_bundle maps extensions to kinds",
          "[cli][install][macos]") {
    REQUIRE(ip::classify_bundle("/tmp/Foo.component") == ip::PluginKind::AU);
    REQUIRE(ip::classify_bundle("/tmp/Foo.vst3")      == ip::PluginKind::VST3);
    REQUIRE(ip::classify_bundle("/tmp/Foo.clap")      == ip::PluginKind::CLAP);
    REQUIRE(ip::classify_bundle("/tmp/Foo.app")       == ip::PluginKind::Unknown);
    REQUIRE(ip::classify_bundle("/tmp/Foo")           == ip::PluginKind::Unknown);
}

TEST_CASE("install_paths_mac: validator_for_kind matches CLAUDE.md policy",
          "[cli][install][macos]") {
    REQUIRE(ip::validator_for_kind(ip::PluginKind::AU)   == "auval");
    REQUIRE(ip::validator_for_kind(ip::PluginKind::VST3) == "pluginval");
    REQUIRE(ip::validator_for_kind(ip::PluginKind::CLAP) == "clap-validator");
    REQUIRE(ip::validator_for_kind(ip::PluginKind::Unknown).empty());
}

TEST_CASE("install_paths_mac: destination_for plants bundles in the correct system folder",
          "[cli][install][macos]") {
    fs::path home("/Users/test");
    REQUIRE(ip::destination_for(ip::PluginKind::AU, home, "MyPlugin.component")
            == "/Users/test/Library/Audio/Plug-Ins/Components/MyPlugin.component");
    REQUIRE(ip::destination_for(ip::PluginKind::VST3, home, "MyPlugin.vst3")
            == "/Users/test/Library/Audio/Plug-Ins/VST3/MyPlugin.vst3");
    REQUIRE(ip::destination_for(ip::PluginKind::CLAP, home, "MyPlugin.clap")
            == "/Users/test/Library/Audio/Plug-Ins/CLAP/MyPlugin.clap");
}

TEST_CASE("install_paths_mac: destination_for rejects relative or empty $HOME",
          "[cli][install][macos]") {
    // Both branches matter: install_bundle uses destination_for() and
    // emits an error string when this returns empty. If we accidentally
    // resolved to "Library/Audio/..." (no leading slash) the install
    // would silently land under the user's CWD.
    REQUIRE(ip::destination_for(ip::PluginKind::VST3, fs::path("relative"), "X.vst3").empty());
    REQUIRE(ip::destination_for(ip::PluginKind::VST3, fs::path(), "X.vst3").empty());
    REQUIRE(ip::destination_for(ip::PluginKind::Unknown, fs::path("/abs"), "X").empty());
}

TEST_CASE("install_paths_mac: install_bundle does mkdir → copy on a clean target",
          "[cli][install][macos]") {
    StubEnv s("/Users/test", {"/build/CLAP/Foo.clap"});
    auto r = ip::install_bundle(s.env, "/build/CLAP/Foo.clap", ip::PluginKind::CLAP);
    REQUIRE(r.success);
    REQUIRE_FALSE(r.replaced_existing);
    REQUIRE(r.error.empty());
    REQUIRE(r.destination == "/Users/test/Library/Audio/Plug-Ins/CLAP/Foo.clap");

    // Order must be mkdir → copy, no rm.
    REQUIRE(s.created_dirs.size() == 1);
    REQUIRE(s.created_dirs[0] == "/Users/test/Library/Audio/Plug-Ins/CLAP");
    REQUIRE(s.removed.empty());
    REQUIRE(s.copies.size() == 1);
    REQUIRE(s.copies[0].first  == "/build/CLAP/Foo.clap");
    REQUIRE(s.copies[0].second == "/Users/test/Library/Audio/Plug-Ins/CLAP/Foo.clap");
}

TEST_CASE("install_paths_mac: install_bundle is idempotent — replaces existing",
          "[cli][install][macos]") {
    // Both source bundle AND a stale prior install exist.
    StubEnv s("/Users/test",
              {"/build/VST3/Foo.vst3",
               "/Users/test/Library/Audio/Plug-Ins/VST3/Foo.vst3"});

    auto r = ip::install_bundle(s.env, "/build/VST3/Foo.vst3", ip::PluginKind::VST3);
    REQUIRE(r.success);
    REQUIRE(r.replaced_existing);

    // The rm MUST come before the copy, otherwise stale bytes survive.
    REQUIRE(s.removed.size() == 1);
    REQUIRE(s.removed[0] == "/Users/test/Library/Audio/Plug-Ins/VST3/Foo.vst3");
    REQUIRE(s.copies.size() == 1);
    REQUIRE(s.copies[0].second == "/Users/test/Library/Audio/Plug-Ins/VST3/Foo.vst3");
}

TEST_CASE("install_paths_mac: install_bundle fails loudly on Unknown kind",
          "[cli][install][macos]") {
    StubEnv s("/Users/test", {"/build/Foo.weird"});
    auto r = ip::install_bundle(s.env, "/build/Foo.weird", ip::PluginKind::Unknown);
    REQUIRE_FALSE(r.success);
    REQUIRE_FALSE(r.error.empty());
    // No filesystem mutations should have happened.
    REQUIRE(s.created_dirs.empty());
    REQUIRE(s.removed.empty());
    REQUIRE(s.copies.empty());
}

TEST_CASE("install_paths_mac: install_bundle refuses relative $HOME",
          "[cli][install][macos]") {
    StubEnv s("relative-home", {"/build/CLAP/Foo.clap"});
    auto r = ip::install_bundle(s.env, "/build/CLAP/Foo.clap", ip::PluginKind::CLAP);
    REQUIRE_FALSE(r.success);
    REQUIRE(r.error.find("$HOME") != std::string::npos);
    REQUIRE(s.copies.empty());
}

TEST_CASE("install_paths_mac: install_bundle errors when source bundle is missing",
          "[cli][install][macos]") {
    StubEnv s("/Users/test", {});  // source does NOT exist
    auto r = ip::install_bundle(s.env, "/build/AU/Missing.component",
                                ip::PluginKind::AU);
    REQUIRE_FALSE(r.success);
    REQUIRE(r.error.find("source bundle does not exist") != std::string::npos);
    REQUIRE(s.copies.empty());
}

TEST_CASE("install_paths_mac: install_bundle surfaces remove failure with location",
          "[cli][install][macos]") {
    StubEnv s("/Users/test",
              {"/build/AU/Foo.component",
               "/Users/test/Library/Audio/Plug-Ins/Components/Foo.component"});
    s.remove_should_fail = true;
    auto r = ip::install_bundle(s.env, "/build/AU/Foo.component",
                                ip::PluginKind::AU);
    REQUIRE_FALSE(r.success);
    REQUIRE(r.error.find("failed to remove existing install") != std::string::npos);
    // Copy must NOT have been attempted after the rm failure.
    REQUIRE(s.copies.empty());
}

TEST_CASE("install_paths_mac: install_bundle surfaces copy failure",
          "[cli][install][macos]") {
    StubEnv s("/Users/test", {"/build/CLAP/Foo.clap"});
    s.copy_should_fail = true;
    auto r = ip::install_bundle(s.env, "/build/CLAP/Foo.clap",
                                ip::PluginKind::CLAP);
    REQUIRE_FALSE(r.success);
    REQUIRE(r.error.find("failed to copy bundle") != std::string::npos);
}

TEST_CASE("install_paths_mac: validator_install_hint names the right install path",
          "[cli][install][macos]") {
    REQUIRE(ip::validator_install_hint(ip::PluginKind::AU)
                .find("xcode-select") != std::string::npos);
    REQUIRE(ip::validator_install_hint(ip::PluginKind::VST3)
                .find("pluginval") != std::string::npos);
    REQUIRE(ip::validator_install_hint(ip::PluginKind::CLAP)
                .find("clap-validator") != std::string::npos);
    REQUIRE(ip::validator_install_hint(ip::PluginKind::Unknown).empty());
}

TEST_CASE("install_paths_mac: discover_bundles classifies entries by extension",
          "[cli][install][macos]") {
    // Build a fake build/ tree on the real filesystem (deterministic
    // cleanup) so discover_bundles is exercised end-to-end against
    // std::filesystem iteration. No `~/Library` writes — everything
    // stays under fs::temp_directory_path().
    auto root = fs::temp_directory_path() / "pulp-test-install-paths-discover";
    fs::remove_all(root);
    fs::create_directories(root / "AU"   / "Foo.component");
    fs::create_directories(root / "VST3" / "Bar.vst3");
    fs::create_directories(root / "CLAP" / "Baz.clap");
    // Also a bogus entry that must be ignored.
    fs::create_directories(root / "CLAP" / "ReadmeDir");

    auto bundles = ip::discover_bundles(root);
    REQUIRE(bundles.size() == 3);
    // Discovery order: AU → VST3 → CLAP (deterministic for output).
    REQUIRE(bundles[0].kind == ip::PluginKind::AU);
    REQUIRE(bundles[0].bundle_path.filename() == "Foo.component");
    REQUIRE(bundles[1].kind == ip::PluginKind::VST3);
    REQUIRE(bundles[1].bundle_path.filename() == "Bar.vst3");
    REQUIRE(bundles[2].kind == ip::PluginKind::CLAP);
    REQUIRE(bundles[2].bundle_path.filename() == "Baz.clap");

    fs::remove_all(root);
}

TEST_CASE("install_paths_mac: discover_bundles tolerates a missing build dir",
          "[cli][install][macos]") {
    // `pulp build --install` runs against a build tree that may not
    // have one or more of the format directories. discover_bundles
    // must not throw — it simply returns an empty list.
    auto bundles = ip::discover_bundles("/tmp/does-not-exist-pulp-install-test");
    REQUIRE(bundles.empty());
}
