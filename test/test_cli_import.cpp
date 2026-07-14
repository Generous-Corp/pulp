// SPDX-License-Identifier: MIT
//
// Tests for the `pulp import` substrate: the framework-detection engine, the
// JSON-over-stdio SPI runner, the install-hint path, and a vendor-agnostic
// source guard.
//
// Vendor-agnostic rule (firm): these committed tests name NO vendor and NO
// framework. They use a NEUTRAL framework id ("example-framework") and a
// test-only known-frameworks index written to a temp dir. Real markers live
// only in the shipped DATA file (tools/import/known-frameworks.json), never
// here.

#include <catch2/catch_test_macros.hpp>

#include "tools/cli/import_detect.hpp"
#include "tools/cli/import_spi.hpp"

#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
namespace det = pulp::cli::import_detect;
namespace spi = pulp::cli::import_spi;

namespace {

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& prefix) {
        path = fs::temp_directory_path() /
               (prefix + "-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()) +
                "-" + std::to_string(::rand()));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << content;
}

// A neutral discovery index naming only "example-framework".
const char* kNeutralIndex = R"({
  "schema": "pulp.import.known_frameworks.v0",
  "frameworks": [
    {
      "framework_id": "example-framework",
      "display_name": "Example Framework",
      "importer_tool_id": "import-example-framework",
      "spi_min": 0,
      "spi_max": 0,
      "detection": [
        { "type": "file_glob", "pattern": "**/*.exproj", "weight": 0.6 },
        { "type": "content_match", "pattern": "EXAMPLE_PLUGIN_NAME", "in_glob": "**/example_config.h", "weight": 0.4 }
      ]
    },
    {
      "framework_id": "other-framework",
      "display_name": "Other Framework",
      "importer_tool_id": "import-other-framework",
      "spi_min": 0,
      "spi_max": 0,
      "detection": [
        { "type": "content_match", "pattern": "OTHER_MARKER_TOKEN", "weight": 0.9 }
      ]
    }
  ]
})";

}  // namespace

// ── Detection ──

TEST_CASE("import detect ranks the right candidate from a neutral fixture",
          "[cli][import][detect][issue-290]") {
    TempDir proj("pulp-import-detect");

    // A fake project that trips the example-framework markers.
    write_file(proj.path / "MyPlugin.exproj", "<project/>\n");
    write_file(proj.path / "src" / "example_config.h",
               "#define EXAMPLE_PLUGIN_NAME \"My Plugin\"\n");
    // A red-herring file that should NOT match other-framework.
    write_file(proj.path / "README.md", "Just a readme.\n");

    auto kf = det::parse_index(kNeutralIndex);
    REQUIRE(kf.error.empty());
    REQUIRE(kf.frameworks.size() == 2);

    auto candidates = det::detect(proj.path, kf);
    REQUIRE_FALSE(candidates.empty());

    // example-framework hit both markers → confidence 1.0; other-framework
    // hit nothing → not present.
    REQUIRE(candidates.front().framework_id == "example-framework");
    REQUIRE(candidates.front().confidence > 0.99);
    REQUIRE(candidates.front().importer_tool_id == "import-example-framework");
    REQUIRE_FALSE(candidates.front().evidence.empty());

    for (const auto& c : candidates)
        REQUIRE(c.framework_id != "other-framework");
}

TEST_CASE("import detect partial-confidence when only some markers hit",
          "[cli][import][detect]") {
    TempDir proj("pulp-import-detect-partial");
    // Only the file_glob marker (weight 0.6 of 1.0 total) hits.
    write_file(proj.path / "thing.exproj", "<x/>\n");

    auto kf = det::parse_index(kNeutralIndex);
    auto candidates = det::detect(proj.path, kf);
    REQUIRE_FALSE(candidates.empty());
    REQUIRE(candidates.front().framework_id == "example-framework");
    // 0.6 / (0.6 + 0.4) = 0.6
    REQUIRE(candidates.front().confidence > 0.55);
    REQUIRE(candidates.front().confidence < 0.65);
}

TEST_CASE("import detect returns nothing for an unrelated tree",
          "[cli][import][detect]") {
    TempDir proj("pulp-import-detect-empty");
    write_file(proj.path / "main.cpp", "int main(){return 0;}\n");
    auto kf = det::parse_index(kNeutralIndex);
    REQUIRE(det::detect(proj.path, kf).empty());
}

TEST_CASE("import glob matcher honours ** and segment boundaries",
          "[cli][import][detect]") {
    REQUIRE(det::glob_match("**/*.exproj", "a/b/c.exproj"));
    REQUIRE(det::glob_match("**/*.exproj", "c.exproj"));
    REQUIRE(det::glob_match("**/example_config.h", "src/deep/example_config.h"));
    REQUIRE_FALSE(det::glob_match("*.exproj", "sub/a.exproj"));  // * can't cross '/'
    REQUIRE(det::glob_match("*.exproj", "a.exproj"));
    REQUIRE_FALSE(det::glob_match("**/*.exproj", "a/b/c.txt"));
}

// ── SPI runner: request building + response parsing ──

TEST_CASE("import SPI builds a well-formed request envelope",
          "[cli][import][spi]") {
    auto req = spi::build_request("analyze", "id-1",
                                  R"({"project_dir":"/tmp/p"})");
    REQUIRE(req.find("\"spi_version\":0") != std::string::npos);
    REQUIRE(req.find("\"verb\":\"analyze\"") != std::string::npos);
    REQUIRE(req.find("\"id\":\"id-1\"") != std::string::npos);
    REQUIRE(req.find("\"payload\":{\"project_dir\":\"/tmp/p\"}") != std::string::npos);
}

TEST_CASE("import SPI parses a successful response with a result object",
          "[cli][import][spi]") {
    std::string line =
        R"({"spi_version":0,"id":"x","ok":true,"result":{"schema":"pulp.import.project_ir.v0","parameters":[{"id":"gain"}]},"diagnostics":[{"severity":"info","code":"OK","message":"done"}]})";
    auto r = spi::parse_response(line);
    REQUIRE(r.transport_ok);
    REQUIRE(r.ok);
    REQUIRE(r.spi_version == 0);
    REQUIRE(r.id == "x");
    REQUIRE(r.result_json.find("pulp.import.project_ir.v0") != std::string::npos);
    REQUIRE(r.result_json.find("gain") != std::string::npos);
    REQUIRE(r.diagnostics.size() == 1);
    REQUIRE(r.diagnostics[0].message == "done");
}

TEST_CASE("import SPI surfaces ok:false with error fields",
          "[cli][import][spi]") {
    std::string line =
        R"({"spi_version":0,"id":"x","ok":false,"error":{"code":"E_PARSE","message":"bad project"}})";
    auto r = spi::parse_response(line);
    REQUIRE(r.transport_ok);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error_code == "E_PARSE");
    REQUIRE(r.error_message == "bad project");
}

TEST_CASE("import SPI version check classifies mismatches",
          "[cli][import][spi]") {
    REQUIRE(spi::check_version(0, 0, 0).empty());          // compatible
    REQUIRE(spi::check_version(1, 0, 0).find("upgrade Pulp") != std::string::npos);
    REQUIRE(spi::check_version(0, 1, 2).find("upgrade the importer") != std::string::npos);
}

TEST_CASE("import SPI command splitter honours quotes",
          "[cli][import][spi]") {
    auto a = spi::split_command(R"(python3 /a/b/spi.py --flag "with space")");
    REQUIRE(a.size() == 4);
    REQUIRE(a[0] == "python3");
    REQUIRE(a[1] == "/a/b/spi.py");
    REQUIRE(a[2] == "--flag");
    REQUIRE(a[3] == "with space");
}

// ── SPI runner: round-trip against a mock responder ──

#if !defined(_WIN32)
TEST_CASE("import SPI round-trips against a mock responder",
          "[cli][import][spi][issue-290]") {
    TempDir td("pulp-import-spi-mock");

    // A tiny mock SPI responder: reads one request line on stdin and echoes a
    // valid import-spi-v0 response with a minimal ProjectIR on stdout. No
    // vendor anywhere.
    fs::path mock = td.path / "mock_spi.sh";
    write_file(mock,
        "#!/bin/sh\n"
        "read line\n"
        "printf '%s\\n' "
        "'{\"spi_version\":0,\"id\":\"analyze-1\",\"ok\":true,"
        "\"result\":{\"schema\":\"pulp.import.project_ir.v0\","
        "\"parameters\":[{\"id\":\"gain\"}]},"
        "\"diagnostics\":[{\"severity\":\"info\",\"message\":\"ok\"}]}'\n");
    fs::permissions(mock, fs::perms::owner_all, fs::perm_options::add);

    spi::ImporterInvocation inv;
    inv.command_line = "/bin/sh " + mock.string();
    auto req = spi::build_request("analyze", "analyze-1",
                                  R"({"project_dir":"/tmp/p"})");
    auto r = spi::run(inv, req);

    INFO("transport_error=" << r.transport_error
         << " raw=" << r.raw_stdout);
    REQUIRE(r.transport_ok);
    REQUIRE(r.ok);
    REQUIRE(r.spi_version == 0);
    REQUIRE(r.result_json.find("project_ir") != std::string::npos);
    REQUIRE(r.result_json.find("gain") != std::string::npos);
}

TEST_CASE("import SPI round-trips an ok:false version-mismatch responder",
          "[cli][import][spi]") {
    TempDir td("pulp-import-spi-mock-fail");
    fs::path mock = td.path / "mock_fail.sh";
    write_file(mock,
        "#!/bin/sh\n"
        "read line\n"
        "printf '%s\\n' "
        "'{\"spi_version\":7,\"id\":\"analyze-1\",\"ok\":false,"
        "\"error\":{\"code\":\"E_VER\",\"message\":\"newer\"}}'\n");
    fs::permissions(mock, fs::perms::owner_all, fs::perm_options::add);

    spi::ImporterInvocation inv;
    inv.command_line = "/bin/sh " + mock.string();
    auto r = spi::run(inv, spi::build_request("analyze", "analyze-1", "{}"));
    REQUIRE(r.transport_ok);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.spi_version == 7);
    // A SDK that understands only v0 would reject this.
    REQUIRE_FALSE(spi::check_version(r.spi_version, 0, 0).empty());
}
#endif  // !_WIN32

// ── Install-hint path via the built CLI ──

#if defined(PULP_CLI_BINARY)
namespace {
fs::path cli_binary() { return fs::path(PULP_CLI_BINARY); }

pulp::platform::ProcessResult run_cli(const std::vector<std::string>& args,
                                      const std::string& known_frameworks) {
    pulp::platform::ProcessOptions opts;
    opts.timeout_ms = 60000;
    // Deterministic + offline.
    ::setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);
    ::setenv("PULP_KNOWN_FRAMEWORKS", known_frameworks.c_str(), 1);
    return pulp::platform::ChildProcess::run(cli_binary().string(), args, opts);
}
}  // namespace

TEST_CASE("import inspect with no importer prints the install hint and exits nonzero",
          "[cli][import][shellout][issue-290]") {
    TempDir td("pulp-import-hint");
    fs::path idx = td.path / "known-frameworks.json";
    write_file(idx, kNeutralIndex);

    TempDir proj("pulp-import-hint-proj");
    write_file(proj.path / "x.exproj", "<x/>\n");

    // No importer installed and no --importer-cmd → resolution fails, hint
    // printed, non-zero exit. (PULP_KNOWN_FRAMEWORKS points at our neutral
    // index, and there is no tool-registry entry for this framework.)
    auto r = run_cli({"import", "inspect", "--from", "example-framework",
                      proj.path.string()},
                     idx.string());
    INFO("stdout=" << r.stdout_output << " stderr=" << r.stderr_output);
    REQUIRE(r.exit_code != 0);
    std::string all = r.stdout_output + r.stderr_output;
    REQUIRE(all.find("pulp tool install import-example-framework") != std::string::npos);
    REQUIRE(all.find("pulp import inspect --from example-framework") != std::string::npos);
}

TEST_CASE("import detect via the CLI ranks the neutral fixture",
          "[cli][import][shellout]") {
    TempDir td("pulp-import-detect-cli");
    fs::path idx = td.path / "known-frameworks.json";
    write_file(idx, kNeutralIndex);

    TempDir proj("pulp-import-detect-cli-proj");
    write_file(proj.path / "x.exproj", "<x/>\n");
    write_file(proj.path / "example_config.h",
               "#define EXAMPLE_PLUGIN_NAME \"P\"\n");

    auto r = run_cli({"import", "detect", proj.path.string()}, idx.string());
    INFO("stdout=" << r.stdout_output << " stderr=" << r.stderr_output);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("example-framework") != std::string::npos);
    REQUIRE(r.stdout_output.find("pulp tool install import-example-framework")
            != std::string::npos);
}

TEST_CASE("import inspect end-to-end against a mock responder writes ProjectIR",
          "[cli][import][shellout]") {
    TempDir td("pulp-import-e2e");
    fs::path idx = td.path / "known-frameworks.json";
    write_file(idx, kNeutralIndex);

    TempDir proj("pulp-import-e2e-proj");
    write_file(proj.path / "x.exproj", "<x/>\n");

#if !defined(_WIN32)
    fs::path mock = td.path / "mock_spi.sh";
    write_file(mock,
        "#!/bin/sh\n"
        "read line\n"
        "printf '%s\\n' "
        "'{\"spi_version\":0,\"id\":\"analyze-1\",\"ok\":true,"
        "\"result\":{\"schema\":\"pulp.import.project_ir.v0\",\"parameters\":[]}}'\n");
    fs::permissions(mock, fs::perms::owner_all, fs::perm_options::add);

    fs::path ir_out = td.path / "ir.json";
    auto r = run_cli({"import", "inspect", "--from", "example-framework",
                      "--importer-cmd", "/bin/sh " + mock.string(),
                      "-o", ir_out.string(), proj.path.string()},
                     idx.string());
    INFO("stdout=" << r.stdout_output << " stderr=" << r.stderr_output);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(ir_out));
    std::ifstream f(ir_out);
    std::string ir((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
    REQUIRE(ir.find("pulp.import.project_ir.v0") != std::string::npos);
#endif
}
#endif  // PULP_CLI_BINARY

// ── Vendor-agnostic source guard ──


// ── The SDK ships no detection markers of its own ───────────────────────────
//
// A framework importer is an add-on: installed by URL, bringing its own detection
// markers with it. The SDK knows only the SHAPE of a marker.
//
// The property below is a privacy one, not a plumbing one. With no importer
// installed, detection must match nothing and REVEAL nothing — it must not be
// possible to learn from the SDK which importers exist, or whether any given one
// is public or private. A user with access to some importer and a user without
// must get identical behaviour out of the SDK itself.

TEST_CASE("the SDK ships an EMPTY framework index", "[cli][import][vendor-agnostic]") {
    // If this fails, someone has added a framework's markers to the SDK. The markers
    // belong to the importer that understands them; shipping them here announces
    // which frameworks we anticipate, which is precisely what the add-on model exists
    // to avoid. This replaces the old hardcoded denylist of vendor names — the SDK no
    // longer needs to spell a framework's name in order to be neutral about it.
    const fs::path shipped =
        fs::path(PULP_SOURCE_DIR) / "tools" / "import" / "known-frameworks.json";
    REQUIRE(fs::exists(shipped));

    const auto index = det::load_index(shipped);
    INFO("tools/import/known-frameworks.json must stay EMPTY — markers are the "
         "importer's, not the SDK's");
    CHECK(index.error.empty());
    REQUIRE(index.frameworks.empty());
}

TEST_CASE("with no importer installed, nothing is known and nothing is hinted at",
          "[cli][import][vendor-agnostic]") {
    TempDir tools_root("pulp-tools-empty");   // an empty ~/.pulp/tools

    const auto merged = det::merge_installed_indices(tools_root.path);

    REQUIRE(merged.error.empty());        // absence is not an error...
    REQUIRE(merged.frameworks.empty());   // ...and it is not a hint, either
}

TEST_CASE("an installed importer contributes its own markers",
          "[cli][import][vendor-agnostic]") {
    TempDir tools_root("pulp-tools-one");

    write_file(tools_root.path / "some-importer" / "known-frameworks.json", R"({
      "schema": "pulp.import.known_frameworks.v0",
      "frameworks": [
        { "framework_id": "fw", "display_name": "FW", "importer_tool_id": "some-importer",
          "spi_min": 0, "spi_max": 0,
          "detection": [ { "type": "file_glob", "pattern": "**/*.marker", "weight": 1.0 } ] }
      ]
    })");

    const auto merged = det::merge_installed_indices(tools_root.path);

    REQUIRE(merged.error.empty());
    REQUIRE(merged.frameworks.size() == 1);
    CHECK(merged.frameworks[0].importer_tool_id == "some-importer");
    CHECK(merged.frameworks[0].detection.size() == 1);
}

TEST_CASE("a VERSIONED importer install is not silently invisible",
          "[cli][import][vendor-agnostic]") {
    // An importer lands at ~/.pulp/tools/<id>/ or ~/.pulp/tools/<id>/<version>/.
    // Looking at only one depth would make a versioned install do nothing at all,
    // with no error — the importer would simply never match anything.
    TempDir tools_root("pulp-tools-ver");

    write_file(tools_root.path / "some-importer" / "1.2.3" / "known-frameworks.json", R"({
      "schema": "pulp.import.known_frameworks.v0",
      "frameworks": [
        { "framework_id": "fw", "display_name": "FW", "importer_tool_id": "some-importer",
          "spi_min": 0, "spi_max": 0,
          "detection": [ { "type": "file_glob", "pattern": "**/*.marker", "weight": 1.0 } ] }
      ]
    })");

    const auto merged = det::merge_installed_indices(tools_root.path);
    REQUIRE(merged.frameworks.size() == 1);
}

TEST_CASE("one broken importer index does not blind detection to the others",
          "[cli][import][vendor-agnostic]") {
    TempDir tools_root("pulp-tools-mixed");

    write_file(tools_root.path / "broken" / "known-frameworks.json", "{ not json");
    write_file(tools_root.path / "working" / "known-frameworks.json", R"({
      "schema": "pulp.import.known_frameworks.v0",
      "frameworks": [
        { "framework_id": "fw", "display_name": "FW", "importer_tool_id": "working",
          "spi_min": 0, "spi_max": 0, "detection": [] }
      ]
    })");

    const auto merged = det::merge_installed_indices(tools_root.path);

    // The working importer still works...
    REQUIRE(merged.frameworks.size() == 1);
    CHECK(merged.frameworks[0].importer_tool_id == "working");
    // ...and the broken one is reported rather than swallowed.
    CHECK_FALSE(merged.error.empty());
}
