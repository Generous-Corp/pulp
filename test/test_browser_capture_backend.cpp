// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include "tools/import-design/browser_capture_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace capture = pulp::import_design::browser_capture;

namespace {

class TempTree {
public:
    explicit TempTree(std::string_view label) {
        const auto nonce = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        root_ = fs::temp_directory_path()
            / (std::string("pulp-browser-capture-test-") + std::string(label)
               + "-" + std::to_string(nonce));
        fs::create_directories(root_);
    }

    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    [[nodiscard]] const fs::path& root() const { return root_; }

    fs::path write(std::string_view relative, std::string_view contents) const {
        const auto path = root_ / fs::path(relative);
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        REQUIRE(out);
        out << contents;
        REQUIRE(out.good());
        return path;
    }

private:
    fs::path root_;
};

class ScopedEnv {
public:
    ScopedEnv(const char* name, std::string value) : name_(name) {
        if (const char* old = std::getenv(name); old) old_ = old;
#ifdef _WIN32
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 1);
#endif
    }

    ~ScopedEnv() {
#ifdef _WIN32
        _putenv_s(name_.c_str(), old_ ? old_->c_str() : "");
#else
        if (old_)
            setenv(name_.c_str(), old_->c_str(), 1);
        else
            unsetenv(name_.c_str());
#endif
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string name_;
    std::optional<std::string> old_;
};

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

std::vector<std::string> read_lines(const fs::path& path) {
    std::istringstream input(read_file(path));
    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);) {
        lines.push_back(std::move(line));
    }
    return lines;
}

bool contains_line(const std::vector<std::string>& lines,
                   std::string_view value) {
    return std::find(lines.begin(), lines.end(), value) != lines.end();
}

fs::path managed_browser_name() {
#ifdef _WIN32
    return "chrome.exe";
#else
    return "chrome";
#endif
}

capture::BrowserInstallation fixture_browser() {
    return {
        fs::path(PULP_BROWSER_CAPTURE_FIXTURE_PATH),
        capture::BrowserOrigin::system,
        "Fixture Chromium",
        "123.4.5.6",
        123,
    };
}

capture::CaptureRequest fixture_request(
    const TempTree& tree, const fs::path& script) {
    const auto staged = tree.root() / "authorized root ' $()";
    fs::create_directories(staged / "nested");
    const auto input = staged / "nested" / "Delay editor ' $().html";
    std::ofstream(input) << "<main>Delay</main>";

    capture::CaptureRequest request;
    request.input_file = input;
    request.staged_root = staged;
    request.output_directory = tree.root() / "output ' $()";
    request.node_executable =
        fs::path(PULP_BROWSER_CAPTURE_FIXTURE_PATH);
    request.capture_script = script;
    request.timeout_ms = 10000;
    return request;
}

}  // namespace

TEST_CASE("browser discovery honors explicit, environment, managed, then system",
          "[import-design][browser-capture]") {
    TempTree tree("discovery");
    const std::string platform = "test-platform";
    const fs::path managed_relative =
        fs::path("1.2.3") / platform / "payload" / managed_browser_name();
    const auto managed =
        tree.write((fs::path("managed") / managed_relative).generic_string(),
                   "managed");
    tree.write(
        "managed/current.json",
        "{\"schema\":1,\"version\":\"1.2.3\",\"platform\":\"" +
            platform + "\",\"executable\":\"" +
            managed_relative.generic_string() + "\"}");
    const auto system = tree.write("system-browser", "system");

    capture::BrowserDiscoveryOptions options;
    options.environment_override = "";
    options.mode_override = "";
    options.managed_root = tree.root() / "managed";
    options.system_candidates = {system};
    options.include_default_system_candidates = false;

    const auto ordered = capture::collect_browser_candidates(options);
    REQUIRE(ordered.size() == 2);
    CHECK(ordered[0].executable == managed);
    CHECK(ordered[0].origin == capture::BrowserOrigin::managed);
    CHECK(ordered[1].executable == system);
    CHECK(ordered[1].origin == capture::BrowserOrigin::system);

    SECTION("explicit override is authoritative") {
        options.explicit_path = tree.root() / "explicit-browser";
        options.environment_override = system.string();
        const auto candidates = capture::collect_browser_candidates(options);
        REQUIRE(candidates.size() == 1);
        CHECK(candidates[0].executable == *options.explicit_path);
        CHECK(candidates[0].origin
              == capture::BrowserOrigin::explicit_override);
    }

    SECTION("environment override is authoritative when explicit is absent") {
        options.environment_override = system.string();
        const auto candidates = capture::collect_browser_candidates(options);
        REQUIRE(candidates.size() == 1);
        CHECK(candidates[0].executable == system);
        CHECK(candidates[0].origin
              == capture::BrowserOrigin::environment_override);
    }
}

TEST_CASE("browser modes constrain managed and system discovery",
          "[import-design][browser-capture]") {
    TempTree tree("modes");
    const fs::path managed_relative =
        fs::path("1.2.3/test-platform/payload") /
        managed_browser_name();
    const auto managed = tree.write(
        (fs::path("managed") / managed_relative).generic_string(),
        "managed");
    tree.write(
        "managed/current.json",
        "{\"schema\":1,\"version\":\"1.2.3\","
        "\"platform\":\"test-platform\",\"executable\":\"" +
            managed_relative.generic_string() + "\"}");
    const auto system = tree.write("system-browser", "system");

    capture::BrowserDiscoveryOptions options;
    options.environment_override = "";
    options.managed_root = tree.root() / "managed";
    options.system_candidates = {system};
    options.include_default_system_candidates = false;

    options.mode_override = "managed";
    auto candidates = capture::collect_browser_candidates(options);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0].executable == managed);

    options.mode_override = "system";
    candidates = capture::collect_browser_candidates(options);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0].executable == system);

    options.mode_override = "invalid";
    const auto mode = capture::resolve_browser_mode(options);
    CHECK_FALSE(mode.mode.has_value());
    CHECK(mode.error.find("expected auto, managed, or system") !=
          std::string::npos);
}

TEST_CASE("browser mode precedence is caller then environment then config",
          "[import-design][browser-capture]") {
    TempTree tree("mode-precedence");
    tree.write("config.toml", "[import_design]\nbrowser = \"system\"\n");
    ScopedEnv home("PULP_HOME", tree.root().string());

    capture::BrowserDiscoveryOptions options;
    {
        ScopedEnv environment_mode("PULP_DESIGN_BROWSER_MODE", "managed");
        auto selection = capture::resolve_browser_mode(options);
        REQUIRE(selection.mode.has_value());
        CHECK(*selection.mode == capture::BrowserMode::managed);
        CHECK(selection.source == "env:PULP_DESIGN_BROWSER_MODE");

        options.mode_override = "auto";
        selection = capture::resolve_browser_mode(options);
        REQUIRE(selection.mode.has_value());
        CHECK(*selection.mode == capture::BrowserMode::auto_select);
        CHECK(selection.source == "caller");
    }

    options.mode_override.reset();
    const auto selection = capture::resolve_browser_mode(options);
    REQUIRE(selection.mode.has_value());
    CHECK(*selection.mode == capture::BrowserMode::system);
    CHECK(selection.source == "config:import_design.browser");
}

TEST_CASE("managed discovery trusts only exact current json",
          "[import-design][browser-capture]") {
    TempTree tree("current-authority");
    const auto stray = tree.write(
        (fs::path("managed/9.9.9/host/payload") /
         managed_browser_name()).generic_string(),
        "stray");
    capture::BrowserDiscoveryOptions options;
    options.environment_override = "";
    options.mode_override = "managed";
    options.managed_root = tree.root() / "managed";
    options.include_default_system_candidates = false;
    CHECK(capture::collect_browser_candidates(options).empty());

    tree.write(
        "managed/current.json",
        "{\"schema\":1,\"version\":\"9.9.9\",\"platform\":\"host\","
        "\"executable\":\"9.9.9/host/payload/" +
            managed_browser_name().generic_string() + "\"}");
    const auto candidates = capture::collect_browser_candidates(options);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0].executable == stray);

    SECTION("unknown current schema is rejected") {
        tree.write(
            "managed/current.json",
            "{\"schema\":2,\"version\":\"9.9.9\",\"platform\":\"host\","
            "\"executable\":\"9.9.9/host/payload/" +
                managed_browser_name().generic_string() + "\"}");
        CHECK(capture::collect_browser_candidates(options).empty());
    }

#ifndef _WIN32
    SECTION("current executable symlink cannot escape managed root") {
        const auto outside = tree.write("outside-browser", "outside");
        const auto link = options.managed_root.value() /
            "9.9.9/host/payload/escaped-browser";
        std::error_code ec;
        fs::create_symlink(outside, link, ec);
        REQUIRE_FALSE(ec);
        tree.write(
            "managed/current.json",
            "{\"schema\":1,\"version\":\"9.9.9\",\"platform\":\"host\","
            "\"executable\":\"9.9.9/host/payload/escaped-browser\"}");
        CHECK(capture::collect_browser_candidates(options).empty());
    }
#endif
}

TEST_CASE("browser capture rejects unsafe viewport allocations before launch",
          "[import-design][browser-capture]") {
    TempTree tree("unsafe-viewport");
    const auto script = tree.write("capture.mjs", "// fixture");
    auto request = fixture_request(tree, script);
    request.initial_width = 8192;
    request.initial_height = 8192;

    capture::BrowserInstallation browser;
    browser.executable = request.node_executable.value();
    browser.product = "Chromium";
    browser.version = "123.0.0.0";

    const auto result = capture::capture_document(browser, request);
    REQUIRE_FALSE(result.ok());
    CHECK(result.diagnostic.code == "invalid-capture-options");
    CHECK(result.process.exit_code == -1);
}

TEST_CASE("browser discovery probes in order and never falls through an override",
          "[import-design][browser-capture]") {
    TempTree tree("probe-order");
    const fs::path managed_relative =
        fs::path("1.2.3/test-platform/bin") / managed_browser_name();
    const auto managed =
        tree.write((fs::path("managed") / managed_relative).generic_string(),
                   "managed");
    tree.write(
        "managed/current.json",
        "{\"schema\":1,\"version\":\"1.2.3\","
        "\"platform\":\"test-platform\",\"executable\":\"" +
            managed_relative.generic_string() + "\"}");
    const auto system = tree.write("system-browser", "system");

    capture::BrowserDiscoveryOptions options;
    options.environment_override = "";
    options.mode_override = "";
    options.managed_root = tree.root() / "managed";
    options.system_candidates = {system};
    options.include_default_system_candidates = false;

    std::vector<fs::path> probed;
    auto discovery = capture::discover_browser(
        options, [&](const capture::BrowserCandidate& candidate) {
            probed.push_back(candidate.executable);
            capture::BrowserProbeResult result;
            result.compatible = true;
            result.product = "Chromium";
            result.version = "123.0.0.0";
            result.major_version = 123;
            return result;
        });
    REQUIRE(discovery.ok());
    REQUIRE(probed.size() == 1);
    CHECK(probed[0] == managed);
    CHECK(discovery.selected->origin == capture::BrowserOrigin::managed);

    options.explicit_path = tree.root() / "missing-explicit";
    probed.clear();
    discovery = capture::discover_browser(
        options, [&](const capture::BrowserCandidate& candidate) {
            probed.push_back(candidate.executable);
            capture::BrowserProbeResult result;
            result.failure = "not compatible";
            return result;
        });
    CHECK_FALSE(discovery.ok());
    REQUIRE(probed.size() == 1);
    CHECK(probed[0] == *options.explicit_path);
}

#ifndef _WIN32
TEST_CASE("browser version metadata redacts successful subprocess output",
          "[import-design][browser-capture][security]") {
    TempTree tree("version-redaction");
    const auto browser = tree.write(
        "browser-wrapper",
        "#!/bin/sh\n"
        "echo \"Google Chrome path:/opt/acme secret/browser 123.0.0.0\" >&2\n");
    fs::permissions(
        browser,
        fs::perms::owner_read | fs::perms::owner_write |
            fs::perms::owner_exec);
    const auto script = tree.write("capture.mjs", "// fixture");

    capture::BrowserDiscoveryOptions options;
    options.node_executable = fs::path(PULP_BROWSER_CAPTURE_FIXTURE_PATH);
    options.capture_script = script;
    const auto result = capture::probe_browser(
        {browser, capture::BrowserOrigin::explicit_override}, options);

    INFO(result.failure);
    REQUIRE(result.compatible);
    CHECK(result.product.find("/opt/acme") == std::string::npos);
    CHECK(result.product.find("<local-path>") != std::string::npos);
}
#endif

TEST_CASE("missing browser guidance is actionable and explains its narrow use",
          "[import-design][browser-capture]") {
    capture::BrowserDiscoveryResult discovery;
    discovery.diagnostic = {
        "browser-unavailable",
        "No compatible browser was found.",
        "browser-discovery",
    };
    discovery.probes.push_back({
        {"/not/a/browser", capture::BrowserOrigin::explicit_override},
        false,
        {},
        {},
        0,
        "not executable",
    });

    const auto human = capture::browser_unavailable_human(discovery);
    CHECK(human.find("pulp tool install chrome-for-testing")
          != std::string::npos);
    CHECK(human.find("install system Google Chrome")
          != std::string::npos);
    CHECK(human.find("https://www.google.com/chrome/") != std::string::npos);
    CHECK(human.find("temporary isolated profile") != std::string::npos);
    CHECK(human.find("not embedded") != std::string::npos);
    CHECK(human.find("--offline") != std::string::npos);

    const auto json = capture::browser_unavailable_json(discovery);
    CHECK(json.find("\"code\":\"browser-unavailable\"")
          != std::string::npos);
    CHECK(json.find(
              "\"remediation\":\"install-managed-or-system-browser\"")
          != std::string::npos);
    CHECK(json.find(
              "\"managed_install_command\":\"pulp tool install "
              "chrome-for-testing\"")
          != std::string::npos);
    CHECK(json.find("\"install_url\":\"https://www.google.com/chrome/\"")
          != std::string::npos);
    CHECK(json.find("\"offline_flag\":\"--offline\"")
          != std::string::npos);
    CHECK(json.find("\"origin\":\"explicit\"") != std::string::npos);
}

TEST_CASE("explicit managed-browser guidance remains actionable",
          "[import-design][browser-capture]") {
    capture::BrowserDiscoveryResult discovery;
    discovery.diagnostic = {
        "managed-browser-unavailable",
        "Managed Chrome for Testing is selected but is not installed.",
        "browser-discovery",
    };
    const auto human = capture::browser_unavailable_human(discovery);
    CHECK(human.find("pulp tool install chrome-for-testing")
          != std::string::npos);
    CHECK(human.find("pulp config set import_design.browser system")
          != std::string::npos);
    CHECK(human.find("never downloads a browser during import")
          != std::string::npos);

    const auto json = capture::browser_unavailable_json(discovery);
    CHECK(json.find(
              "\"remediation\":\"pulp-tool-install-chrome-for-testing\"")
          != std::string::npos);
    CHECK(json.find(
              "\"managed_install_command\":\"pulp tool install "
              "chrome-for-testing\"")
          != std::string::npos);
    CHECK(json.find(
              "\"system_mode_command\":\"pulp config set "
              "import_design.browser system\"")
          != std::string::npos);
}

TEST_CASE("prerequisite guidance preserves Node and capture-runtime failures",
          "[import-design][browser-capture]") {
    capture::BrowserDiscoveryResult discovery;
    discovery.diagnostic = {
        "node-incompatible",
        "The installed Node.js is too old; faithful HTML import needs Node.js "
        "22 or newer.",
        "runtime-discovery",
    };
    discovery.probes.push_back({
        {"/Applications/Google Chrome",
         capture::BrowserOrigin::explicit_override},
        false,
        "Google Chrome",
        "123.0.0.0",
        123,
        "Node.js is too old (found 20.0.0, need 22 or newer)",
        capture::BrowserProbeFailure::node_incompatible,
    });

    auto human = capture::browser_unavailable_human(discovery);
    CHECK(human.find("nodejs.org/en/download") != std::string::npos);
    CHECK(human.find("Install Google Chrome") == std::string::npos);
    auto json = capture::browser_unavailable_json(discovery);
    CHECK(json.find("\"code\":\"node-incompatible\"") != std::string::npos);
    CHECK(json.find("\"remediation\":\"install-node-22\"")
          != std::string::npos);

    discovery.diagnostic = {
        "capture-runtime-unavailable",
        "The Pulp browser-capture runtime is missing or incomplete.",
        "runtime-discovery",
    };
    discovery.probes[0].failure_kind =
        capture::BrowserProbeFailure::capture_runtime_unavailable;
    discovery.probes[0].failure = "browser capture script was not found";
    human = capture::browser_unavailable_human(discovery);
    CHECK(human.find("pulp upgrade") != std::string::npos);
    CHECK(human.find("Install Google Chrome") == std::string::npos);
    json = capture::browser_unavailable_json(discovery);
    CHECK(json.find("\"code\":\"capture-runtime-unavailable\"")
          != std::string::npos);
    CHECK(json.find("\"remediation\":\"pulp-upgrade\"")
          != std::string::npos);
}

#ifndef _WIN32
TEST_CASE("browser probe distinguishes missing and old Node from Chrome",
          "[import-design][browser-capture]") {
    TempTree tree("node-prerequisites");
    const auto browser = tree.write(
        "browser-wrapper",
        "#!/bin/sh\n"
        "echo 'Google Chrome 123.0.0.0'\n");
    fs::permissions(
        browser,
        fs::perms::owner_read | fs::perms::owner_write |
            fs::perms::owner_exec);
    const auto capture_script = tree.write("capture.mjs", "// fixture");

    capture::BrowserDiscoveryOptions options;
    options.explicit_path = browser;
    options.capture_script = capture_script;
    options.include_default_system_candidates = false;

    SECTION("missing Node") {
        options.node_executable = tree.root() / "missing-node";
        const auto discovery = capture::discover_browser(options);
        REQUIRE_FALSE(discovery.ok());
        CHECK(discovery.diagnostic.code == "node-unavailable");
        REQUIRE(discovery.probes.size() == 1);
        CHECK(discovery.probes[0].failure_kind ==
              capture::BrowserProbeFailure::node_unavailable);
    }

    SECTION("old Node") {
        const auto node = tree.write(
            "old-node",
            "#!/bin/sh\n"
            "echo 'v20.11.1'\n");
        fs::permissions(
            node,
            fs::perms::owner_read | fs::perms::owner_write |
                fs::perms::owner_exec);
        options.node_executable = node;
        const auto discovery = capture::discover_browser(options);
        REQUIRE_FALSE(discovery.ok());
        CHECK(discovery.diagnostic.code == "node-incompatible");
        REQUIRE(discovery.probes.size() == 1);
        CHECK(discovery.probes[0].failure.find("found 20.11.1")
              != std::string::npos);
    }

    SECTION("missing capture runtime") {
        options.node_executable =
            fs::path(PULP_BROWSER_CAPTURE_FIXTURE_PATH);
        options.capture_script = tree.root() / "missing-capture.mjs";
        const auto discovery = capture::discover_browser(options);
        REQUIRE_FALSE(discovery.ok());
        CHECK(discovery.diagnostic.code == "capture-runtime-unavailable");
        REQUIRE(discovery.probes.size() == 1);
        CHECK(discovery.probes[0].failure_kind ==
              capture::BrowserProbeFailure::capture_runtime_unavailable);
    }
}
#endif

TEST_CASE("capture passes paths as exact argv and cleans its isolated profile",
          "[import-design][browser-capture]") {
    TempTree tree("argv");
    const auto script = tree.write("capture script ' $().mjs", "// fixture");
    auto request = fixture_request(tree, script);
    const auto interactions = tree.write(
        "interaction plan ' $().json",
        R"({"schema":"pulp-browser-interactions-v1","version":1,"actions":[{"action":"click","selector":"#open"}]})");
    request.interaction_plan = interactions;
    request.allow_network = true;
    tree.write(
        "authorized root ' $()/nested/runtime.js",
        "const font = 'https://fonts.example.org/css?family=Inter';");
    std::ofstream(request.input_file)
        << R"(<script src="https://cdn.example.com/icons.js"></script>)"
        << R"(<link href="https://fonts.example.net/font.woff2">)"
        << R"(<img src="https://user:supersecret@private.example/pixel">)";

    const auto result =
        capture::capture_document(fixture_browser(), request);
    INFO(result.diagnostic.message);
    REQUIRE(result.ok());
    REQUIRE(result.process.exit_code == 0);

    const auto output = fs::canonical(request.output_directory);
    const auto args = read_lines(output / "argv.txt");
    CHECK(contains_line(args, script.string()));
    CHECK(contains_line(args, fs::canonical(request.input_file).string()));
    CHECK(contains_line(args, fs::canonical(request.staged_root).string()));
    CHECK(contains_line(args, output.string()));
    CHECK(contains_line(args, "Fixture Chromium"));
    CHECK(contains_line(args, "123.4.5.6"));
    CHECK(contains_line(args, "--interactions"));
    CHECK(contains_line(args, interactions.string()));
    CHECK(contains_line(args, "--allow-network"));
    CHECK(contains_line(args, "--declared-network-origin"));
    CHECK(contains_line(args, "https://cdn.example.com"));
    CHECK(contains_line(args, "https://fonts.example.net"));
    CHECK(contains_line(args, "https://fonts.example.org"));
    CHECK_FALSE(contains_line(args, "https://cdn.example.com/icons.js"));
    CHECK_FALSE(contains_line(args, "https://fonts.example.net/font.woff2"));
    CHECK(std::none_of(args.begin(), args.end(), [](const auto& argument) {
        return argument.find("supersecret") != std::string::npos;
    }));

    auto profile = read_file(output / "profile-path.txt");
    CHECK_FALSE(profile.empty());
    CHECK_FALSE(fs::exists(profile));
    CHECK(fs::exists(output / "capture.json"));
    CHECK(fs::exists(output / "browser.png"));
    CHECK(fs::exists(output / "semantic-report.json"));
    CHECK(fs::exists(output / "dom-snapshot.json"));
}

TEST_CASE("capture preserves the runtime diagnostic code",
          "[import-design][browser-capture]") {
    TempTree tree("runtime-code");
    const auto script =
        tree.write("source-unresolved.mjs", "// fixture");
    const auto result = capture::capture_document(
        fixture_browser(), fixture_request(tree, script));

    REQUIRE_FALSE(result.ok());
    CHECK(result.diagnostic.code == "capture-source-unresolved");
    CHECK(result.diagnostic.message.find("source produced no visible design")
          != std::string::npos);
}

TEST_CASE("capture redacts raw subprocess stderr before returning it",
          "[import-design][browser-capture][security]") {
    TempTree tree("raw-stderr");
    const auto script = tree.write("raw-stderr.mjs", "// fixture");
    const auto result = capture::capture_document(
        fixture_browser(), fixture_request(tree, script));

    REQUIRE_FALSE(result.ok());
    CHECK(result.diagnostic.message.find("/opt/acme") == std::string::npos);
    CHECK(result.process.stdout_output.find("Jane Doe") == std::string::npos);
    CHECK(result.process.stdout_output.find("Private") == std::string::npos);
    CHECK(result.process.stderr_output.find("/opt/acme") == std::string::npos);
    CHECK(result.process.stderr_output.find("Jane Doe") == std::string::npos);
    CHECK(result.process.stderr_output.find("Private") == std::string::npos);
    CHECK(result.diagnostic.message.find("<local-path>") != std::string::npos);
    CHECK(result.process.stderr_output.find("<local-path>")
          != std::string::npos);
}

TEST_CASE("capture redacts successful subprocess stdout before returning it",
          "[import-design][browser-capture][security]") {
    TempTree tree("raw-stdout");
    const auto script = tree.write("raw-stdout.mjs", "// fixture");
    const auto result = capture::capture_document(
        fixture_browser(), fixture_request(tree, script));

    REQUIRE(result.ok());
    CHECK(result.process.stdout_output.find("Jane Doe") == std::string::npos);
    CHECK(result.process.stdout_output.find("Private") == std::string::npos);
    CHECK(result.process.stdout_output.find("<local-path>")
          != std::string::npos);
}

TEST_CASE("capture deadline leaves time for runtime-owned cleanup",
          "[import-design][browser-capture][timeout]") {
    TempTree tree("deadline-cleanup");
    const auto script =
        tree.write("deadline-cleanup.mjs", "// fixture");
    auto request = fixture_request(tree, script);
    request.timeout_ms = 25;

    const auto result =
        capture::capture_document(fixture_browser(), request);

    REQUIRE_FALSE(result.ok());
    CHECK(result.diagnostic.code == "browser-capture-timeout");
    CHECK_FALSE(result.process.timed_out);
    CHECK(result.process.exit_code == 124);
    const auto output = fs::canonical(request.output_directory);
    CHECK(fs::exists(output / "deadline-cleanup-finished"));
    const auto profile = read_file(output / "profile-path.txt");
    REQUIRE_FALSE(profile.empty());
    CHECK_FALSE(fs::exists(profile));
}

TEST_CASE("capture clears known stale artifacts before validating fresh output",
          "[import-design][browser-capture]") {
    TempTree tree("stale");
    const auto script = tree.write("omit-semantic.mjs", "// fixture");
    auto request = fixture_request(tree, script);
    fs::create_directories(request.output_directory);
    for (const auto name : {
             "capture.json",
             "browser.png",
             "semantic-report.json",
             "tokens.json",
             "dom-snapshot.json",
             "capture-error.json",
         }) {
        std::ofstream(request.output_directory / name) << "stale";
    }
    std::ofstream(request.output_directory / "keep.me") << "unrelated";

    const auto result =
        capture::capture_document(fixture_browser(), request);
    REQUIRE_FALSE(result.ok());
    CHECK(result.diagnostic.code == "browser-capture-incomplete");
    CHECK_FALSE(fs::exists(
        request.output_directory / "semantic-report.json"));
    CHECK(fs::exists(request.output_directory / "capture-error.json"));
    CHECK(read_file(request.output_directory / "keep.me") == "unrelated");
}

TEST_CASE("capture never accepts a stale token report",
          "[import-design][browser-capture]") {
    TempTree tree("stale-token");
    const auto script = tree.write("omit-token.mjs", "// fixture");
    auto request = fixture_request(tree, script);
    fs::create_directories(request.output_directory);
    std::ofstream(request.output_directory / "tokens.json") << "stale";

    const auto result =
        capture::capture_document(fixture_browser(), request);
    REQUIRE_FALSE(result.ok());
    CHECK(result.diagnostic.code == "browser-capture-incomplete");
    CHECK(result.diagnostic.message.find("token report")
          != std::string::npos);
    CHECK_FALSE(fs::exists(request.output_directory / "tokens.json"));
}

TEST_CASE("capture reports an output error when a stale artifact cannot clear",
          "[import-design][browser-capture]") {
    TempTree tree("blocked-output");
    const auto script = tree.write("capture.mjs", "// fixture");
    auto request = fixture_request(tree, script);
    fs::create_directories(request.output_directory / "capture.json");
    std::ofstream(request.output_directory / "capture.json" / "child")
        << "blocks removal";

    const auto result =
        capture::capture_document(fixture_browser(), request);
    REQUIRE_FALSE(result.ok());
    CHECK(result.diagnostic.code == "capture-output-error");
    CHECK(result.diagnostic.message.find("capture.json")
          != std::string::npos);
    CHECK_FALSE(fs::exists(request.output_directory / "argv.txt"));
}

TEST_CASE("capture rejects an input outside the staged root",
          "[import-design][browser-capture]") {
    TempTree tree("containment");
    const auto script = tree.write("capture.mjs", "// fixture");
    auto request = fixture_request(tree, script);
    request.input_file = tree.write("outside.html", "<main>outside</main>");

    const auto result =
        capture::capture_document(fixture_browser(), request);
    REQUIRE_FALSE(result.ok());
    CHECK(result.diagnostic.code == "invalid-capture-input");
    CHECK(result.diagnostic.message.find("escapes") != std::string::npos);
    CHECK(fs::exists(request.output_directory / "capture-error.json"));
    CHECK_FALSE(fs::exists(request.output_directory / "capture.json"));
}
