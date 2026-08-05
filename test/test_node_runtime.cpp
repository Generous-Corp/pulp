// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include "tools/import-design/node_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace capture = pulp::import_design::browser_capture;

namespace {

class TempTree {
public:
    explicit TempTree(std::string_view label) {
        const auto nonce =
            std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path()
            / (std::string("pulp-node-runtime-test-") + std::string(label) + "-"
               + std::to_string(nonce));
        fs::create_directories(root_);
    }

    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    [[nodiscard]] const fs::path& root() const { return root_; }

    // Writes an executable stand-in for `node`. Nothing runs it: the resolver
    // tests inject their own version probe, so the file only has to exist and
    // carry the executable bit the search filters on.
    fs::path install(const fs::path& relative) const {
        const auto path = root_ / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << "#!/bin/sh\n";
        fs::permissions(path,
                        fs::perms::owner_read | fs::perms::owner_write
                            | fs::perms::owner_exec);
        return path;
    }

private:
    fs::path root_;
};

// Records every executable the resolver probes, and answers with a caller-
// supplied version table. An unlisted path answers nullopt, standing in for an
// executable that cannot run.
struct RecordingProbe {
    std::vector<std::pair<fs::path, std::string>> versions;
    std::shared_ptr<std::vector<fs::path>> probed =
        std::make_shared<std::vector<fs::path>>();

    capture::NodeVersionProbe fn() const {
        auto table = versions;
        auto log = probed;
        return [table, log](const fs::path& path)
                   -> std::optional<std::string> {
            log->push_back(path);
            for (const auto& [candidate, version] : table) {
                if (candidate == path) return version;
            }
            return std::nullopt;
        };
    }
};

capture::NodeSearchOptions hermetic_options() {
    capture::NodeSearchOptions options;
    // Nothing on PATH, no real install locations, no home directory: the
    // search table is exactly what each case declares.
    options.search_path = std::string{};
    options.home = fs::path{};
    options.include_default_locations = false;
    return options;
}

const capture::NodeAttempt* attempt_for(
    const capture::NodeResolution& resolution, const fs::path& path) {
    for (const auto& attempt : resolution.attempts) {
        if (attempt.executable == path) return &attempt;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("node version output parses and rejects unusable output",
          "[import-design][node-runtime]") {
    std::string version;
    int major = 0;

    REQUIRE(capture::parse_node_version("v22.11.0\n", version, major));
    CHECK(version == "22.11.0");
    CHECK(major == 22);

    version.clear();
    major = 0;
    REQUIRE(capture::parse_node_version("26.0.0", version, major));
    CHECK(version == "26.0.0");
    CHECK(major == 26);

    version.clear();
    major = 0;
    CHECK_FALSE(capture::parse_node_version("", version, major));
    CHECK_FALSE(capture::parse_node_version("not a version", version, major));
    CHECK_FALSE(capture::parse_node_version("v0.0.1", version, major));
}

TEST_CASE("node resolves from an install location when PATH holds nothing",
          "[import-design][node-runtime]") {
    TempTree tree("gui-path");
    const auto homebrew = tree.install(fs::path("opt") / "homebrew" / "node");

    auto options = hermetic_options();
    options.extra_locations = {homebrew};

    RecordingProbe probe{{{homebrew, "v26.0.0\n"}}};
    const auto resolution = capture::resolve_node(options, probe.fn());

    REQUIRE(resolution.ok());
    CHECK(*resolution.executable == homebrew);
    CHECK(resolution.version == "26.0.0");
    CHECK(resolution.major_version == 26);
    CHECK(resolution.origin == capture::NodeOrigin::install_location);
    CHECK(resolution.resolved_off_path());
    CHECK(resolution.failure == capture::NodeResolutionFailure::none);
}

TEST_CASE("node on PATH wins over the install locations",
          "[import-design][node-runtime]") {
    TempTree tree("path-precedence");
    const auto on_path_dir = tree.root() / "bin";
    fs::create_directories(on_path_dir);
#ifdef _WIN32
    const auto on_path = tree.install(fs::path("bin") / "node.exe");
#else
    const auto on_path = tree.install(fs::path("bin") / "node");
#endif
    const auto homebrew = tree.install(fs::path("opt") / "homebrew" / "node");

    auto options = hermetic_options();
    options.search_path = on_path_dir.string();
    options.extra_locations = {homebrew};

    RecordingProbe probe{{{on_path, "v22.11.0\n"}, {homebrew, "v26.0.0\n"}}};
    const auto resolution = capture::resolve_node(options, probe.fn());

    REQUIRE(resolution.ok());
    CHECK(*resolution.executable == on_path);
    CHECK(resolution.version == "22.11.0");
    CHECK(resolution.origin == capture::NodeOrigin::search_path);
    CHECK_FALSE(resolution.resolved_off_path());
    // The install locations are never probed once PATH satisfies the floor.
    REQUIRE(probe.probed->size() == 1);
    CHECK((*probe.probed)[0] == on_path);
}

TEST_CASE("an explicit node override wins and is not second-guessed",
          "[import-design][node-runtime]") {
    TempTree tree("explicit-override");
    const auto chosen = tree.install("chosen-node");
    const auto homebrew = tree.install(fs::path("opt") / "homebrew" / "node");

    auto options = hermetic_options();
    options.explicit_path = chosen;
    options.extra_locations = {homebrew};

    RecordingProbe probe{{{chosen, "v22.0.0\n"}, {homebrew, "v26.0.0\n"}}};
    const auto resolution = capture::resolve_node(options, probe.fn());

    REQUIRE(resolution.ok());
    CHECK(*resolution.executable == chosen);
    CHECK(resolution.origin == capture::NodeOrigin::explicit_override);
    REQUIRE(probe.probed->size() == 1);
    CHECK((*probe.probed)[0] == chosen);

    SECTION("an override that does not exist reports not-found, not too-old") {
        options.explicit_path = tree.root() / "absent-node";
        const auto missing = capture::resolve_node(options, probe.fn());
        CHECK_FALSE(missing.ok());
        CHECK(missing.failure == capture::NodeResolutionFailure::not_found);
        CHECK(missing.attempts.empty());
    }
}

TEST_CASE("an install location below the floor is skipped for a newer one",
          "[import-design][node-runtime]") {
    TempTree tree("floor");
    const auto old_node = tree.install(fs::path("usr") / "local" / "node");
    const auto new_node = tree.install(fs::path("opt") / "homebrew" / "node");

    auto options = hermetic_options();
    options.extra_locations = {old_node, new_node};

    RecordingProbe probe{{{old_node, "v20.11.1\n"}, {new_node, "v24.3.0\n"}}};
    const auto resolution = capture::resolve_node(options, probe.fn());

    REQUIRE(resolution.ok());
    CHECK(*resolution.executable == new_node);
    CHECK(resolution.major_version == 24);
    REQUIRE(resolution.attempts.size() == 2);
    const auto* rejected = attempt_for(resolution, old_node);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->version == "20.11.1");
    CHECK(rejected->failure.find("too old") != std::string::npos);
}

TEST_CASE("every installed node below the floor reports too-old, not missing",
          "[import-design][node-runtime]") {
    TempTree tree("all-too-old");
    const auto old_node = tree.install(fs::path("usr") / "local" / "node");

    auto options = hermetic_options();
    options.extra_locations = {old_node};

    RecordingProbe probe{{{old_node, "v18.20.4\n"}}};
    const auto resolution = capture::resolve_node(options, probe.fn());

    CHECK_FALSE(resolution.ok());
    CHECK(resolution.failure == capture::NodeResolutionFailure::incompatible);
    REQUIRE(resolution.attempts.size() == 1);
    CHECK(resolution.attempts[0].version == "18.20.4");

    const auto report = capture::node_search_report(resolution);
    CHECK(report.find("Node.js installations checked") != std::string::npos);
    CHECK(report.find("v18.20.4") != std::string::npos);
    CHECK(report.find(old_node.string()) != std::string::npos);
}

TEST_CASE("no node anywhere reports the locations that were searched",
          "[import-design][node-runtime]") {
    RecordingProbe probe;
    const auto resolution =
        capture::resolve_node(hermetic_options(), probe.fn());

    CHECK_FALSE(resolution.ok());
    CHECK(resolution.failure == capture::NodeResolutionFailure::not_found);
    CHECK(resolution.attempts.empty());
    CHECK(probe.probed->empty());

    const auto report = capture::node_search_report(resolution);
    CHECK(report.find("Searched for Node.js in") != std::string::npos);
    CHECK(report.find("PATH") != std::string::npos);
    // The old message named the browsers it had checked, which says nothing
    // about a missing Node.js.
    CHECK(report.find("Chrome") == std::string::npos);
}

TEST_CASE("the searched-location table names every supported install layout",
          "[import-design][node-runtime]") {
    // Independent of what this machine has installed: the table is built from
    // the search plan, not from what happened to exist.
    capture::NodeSearchOptions options;
    options.search_path = std::string{};
    options.home = fs::path("/nonexistent-home");

    RecordingProbe probe;
    const auto resolution = capture::resolve_node(options, probe.fn());

    const auto names = [&](std::string_view source) {
        return std::any_of(
            resolution.searched.begin(), resolution.searched.end(),
            [&](const capture::NodeSearchLocation& location) {
                return location.source == source;
            });
    };
    const auto lists = [&](std::string_view fragment) {
        return std::any_of(
            resolution.searched.begin(), resolution.searched.end(),
            [&](const capture::NodeSearchLocation& location) {
                return location.location.find(fragment) != std::string::npos;
            });
    };
    CHECK(names("PATH"));
#ifndef _WIN32
    CHECK(names("Homebrew"));
    CHECK(names("Homebrew or installer"));
    CHECK(names("system"));
    CHECK(names("mise"));
    CHECK(names("nvm"));
    CHECK(names("fnm"));
    CHECK(names("asdf"));
    CHECK(lists("/opt/homebrew/bin/node"));
    CHECK(lists("/usr/local/bin/node"));
    CHECK(lists("/usr/bin/node"));
    CHECK(lists(".nvm/versions/node"));
    CHECK(lists("mise/installs/node"));
    CHECK(lists("fnm/node-versions"));
    CHECK(lists(".asdf/installs/nodejs"));
#endif
}

TEST_CASE("the not-found report names each searched location",
          "[import-design][node-runtime]") {
    auto options = hermetic_options();
    // Named but absent, so the search records it without producing a
    // candidate — the report must still show where it looked.
    options.extra_locations = {fs::path("/nonexistent/place/node")};

    RecordingProbe probe;
    const auto resolution = capture::resolve_node(options, probe.fn());

    REQUIRE_FALSE(resolution.ok());
    REQUIRE(resolution.attempts.empty());
    const auto report = capture::node_search_report(resolution);
    INFO(report);
    CHECK(report.find("Searched for Node.js in") != std::string::npos);
    CHECK(report.find("/nonexistent/place/node") != std::string::npos);
}

#ifndef _WIN32
TEST_CASE("version-manager installs are ordered newest first, not lexically",
          "[import-design][node-runtime]") {
    TempTree tree("version-manager");
    const auto home = tree.root() / "home";
    const auto mise_root =
        home / ".local" / "share" / "mise" / "installs" / "node";
    // Lexical order would put "22.22.3" behind "9.1.0" and both behind
    // "latest"; the resolver must order 22 above 9 numerically and push the
    // unversioned names to the back.
    const auto v9 =
        tree.install(fs::path(mise_root.lexically_relative(tree.root()))
                     / "9.1.0" / "bin" / "node");
    const auto v22 =
        tree.install(fs::path(mise_root.lexically_relative(tree.root()))
                     / "22.22.3" / "bin" / "node");
    const auto v24 =
        tree.install(fs::path(mise_root.lexically_relative(tree.root()))
                     / "v24.1.0" / "bin" / "node");
    const auto latest =
        tree.install(fs::path(mise_root.lexically_relative(tree.root()))
                     / "latest" / "bin" / "node");

    capture::NodeSearchOptions options;
    options.search_path = std::string{};
    options.home = home;
    // The real /opt/homebrew/bin/node on a developer machine must not enter
    // this table; `home` alone drives the version-manager roots.
    options.include_default_locations = false;

    const auto candidates = capture::collect_node_candidates(options);
    std::vector<fs::path> mise;
    for (const auto& candidate : candidates) {
        if (candidate.source == "mise") mise.push_back(candidate.executable);
    }
    REQUIRE(mise.size() == 4);
    CHECK(mise[0] == v24);
    CHECK(mise[1] == v22);
    CHECK(mise[2] == v9);
    CHECK(mise[3] == latest);
    for (const auto& candidate : candidates) {
        if (candidate.executable == v24)
            CHECK(candidate.origin == capture::NodeOrigin::version_manager);
    }

    SECTION("the newest install meeting the floor is selected") {
        RecordingProbe probe{{{v9, "v9.1.0\n"},
                              {v22, "v22.22.3\n"},
                              {v24, "v24.1.0\n"},
                              {latest, "v26.0.0\n"}}};
        const auto resolution = capture::resolve_node(options, probe.fn());
        REQUIRE(resolution.ok());
        CHECK(*resolution.executable == v24);
        CHECK(resolution.source == "mise");
        REQUIRE(probe.probed->size() == 1);
    }

    SECTION("a newest install below the floor falls through to the next") {
        RecordingProbe probe{{{v9, "v9.1.0\n"},
                              {v22, "v22.22.3\n"},
                              {v24, "v14.1.0\n"},
                              {latest, "v26.0.0\n"}}};
        const auto resolution = capture::resolve_node(options, probe.fn());
        REQUIRE(resolution.ok());
        CHECK(*resolution.executable == v22);
        CHECK(resolution.major_version == 22);
        REQUIRE(probe.probed->size() == 2);
    }
}

TEST_CASE("each supported version manager layout is discovered",
          "[import-design][node-runtime]") {
    TempTree tree("managers");
    const auto home = tree.root() / "home";
    const auto relative = [&](const fs::path& absolute) {
        return absolute.lexically_relative(tree.root());
    };

    const auto mise = tree.install(relative(
        home / ".local" / "share" / "mise" / "installs" / "node" / "22.1.0"
        / "bin" / "node"));
    const auto nvm = tree.install(relative(
        home / ".nvm" / "versions" / "node" / "v22.2.0" / "bin" / "node"));
    const auto fnm = tree.install(relative(
        home / ".local" / "share" / "fnm" / "node-versions" / "v22.3.0"
        / "installation" / "bin" / "node"));
    const auto asdf = tree.install(relative(
        home / ".asdf" / "installs" / "nodejs" / "22.4.0" / "bin" / "node"));

    capture::NodeSearchOptions options;
    options.search_path = std::string{};
    options.home = home;
    options.include_default_locations = false;

    const auto candidates = capture::collect_node_candidates(options);
    const auto found = [&](const fs::path& executable,
                           std::string_view source) {
        return std::any_of(
            candidates.begin(), candidates.end(),
            [&](const capture::NodeCandidate& candidate) {
                return candidate.executable == executable
                    && candidate.source == source
                    && candidate.origin
                    == capture::NodeOrigin::version_manager;
            });
    };
    CHECK(found(mise, "mise"));
    CHECK(found(nvm, "nvm"));
    CHECK(found(fnm, "fnm"));
    CHECK(found(asdf, "asdf"));

    SECTION("a manager root that holds nothing yields no candidate") {
        capture::NodeSearchOptions empty = options;
        empty.home = tree.root() / "empty-home";
        const auto none = capture::collect_node_candidates(empty);
        CHECK(none.empty());
    }
}

TEST_CASE("the real search finds Node.js under a GUI-launch PATH",
          "[import-design][node-runtime]") {
    // The defect this covers: macOS hands a Finder-launched app
    // `/usr/bin:/bin:/usr/sbin:/sbin`, so a PATH-only lookup reports "not
    // found" for a Node.js that is installed and new enough. This case runs
    // the production search — real locations, real version probe — against
    // that exact PATH.
    capture::NodeSearchOptions gui;
    gui.search_path = std::string("/usr/bin:/bin:/usr/sbin:/sbin");

    capture::NodeSearchOptions shell;

    const auto under_gui_path = capture::resolve_node(gui);
    const auto under_shell_path = capture::resolve_node(shell);

    if (!under_shell_path.ok()) {
        WARN("no Node.js >= 22 on this machine; GUI-PATH parity not asserted");
        // Without a Node.js anywhere the two must at least agree, and the
        // failure must name the locations rather than claim a bare absence.
        CHECK_FALSE(under_gui_path.ok());
        CHECK_FALSE(under_gui_path.searched.empty());
        return;
    }

    INFO("shell PATH resolved " << under_shell_path.executable->string());
    REQUIRE(under_gui_path.ok());
    CHECK(under_gui_path.major_version >= capture::kMinimumNodeMajor);
    CHECK(fs::exists(*under_gui_path.executable));
    CHECK_FALSE(under_gui_path.version.empty());
}
#endif
