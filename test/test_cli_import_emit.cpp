// SPDX-License-Identifier: MIT
//
// Tests for `pulp import emit` materialisation: the manifest parser, the
// write-plan computation, the clean-room OUTPUT denylist scan (all pure), and
// an end-to-end shell-out through a mock emit responder that writes a real
// scaffold + provenance marker into a temp dir.
//
// Vendor-agnostic rule (firm): these committed tests name NO vendor and NO
// framework. They use neutral framework ids and neutral denylist tokens
// ("forbidden_marker", "frameworkheader.h"). Real markers live only in the
// shipped DATA index (tools/import/known-frameworks.json), never here.

#include <catch2/catch_test_macros.hpp>

#include "tools/cli/import_detect.hpp"
#include "tools/cli/import_emit.hpp"
#include "tools/cli/import_emit_scan.hpp"

#include <pulp/platform/child_process.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
namespace ie = pulp::cli::import_emit;
namespace ies = pulp::cli::import_emit_scan;
namespace det = pulp::cli::import_detect;

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

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7] = {};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

// A neutral EmissionManifest with one generated source, one generated build
// file, and (optionally) a copied-user-file pointing at `copy_from`.
std::string mock_manifest(const std::string& copy_from = {},
                          const std::string& generated_content =
                              "#pragma once\nstruct X {};\n") {
    std::string j =
        "{\"schema\":\"pulp.import.emission_manifest.v0\","
        "\"importer_id\":\"mock-importer\",\"framework\":\"example-framework\","
        "\"files\":[";
    j += "{\"path\":\"src/PluginProcessor.hpp\",\"provenance\":\"generated\","
         "\"classification\":\"source\",\"content\":\"" +
         json_escape(generated_content) + "\"},";
    j += "{\"path\":\"CMakeLists.txt\",\"provenance\":\"generated\","
         "\"classification\":\"build\",\"content\":\"project(Imported)\\n\"}";
    if (!copy_from.empty()) {
        j += ",{\"path\":\"src/Core.h\",\"provenance\":\"copied-user-file\","
             "\"classification\":\"source\",\"copy_from\":\"" + json_escape(copy_from) + "\"}";
    }
    j += "],";
    j += "\"migration_status\":{\"status\":\"unresolved\",\"verdict\":"
         "{\"builds\":\"yes\"}},";
    j += "\"formats\":[\"CLAP\"],\"deferred_formats\":[\"VST3\"],"
         "\"unresolved\":[\"migrate DSP\"],"
         "\"verdict\":\"Builds: yes. Audio parity: no.\"}";
    return j;
}

// A neutral known-frameworks index whose content_match markers double as the
// output denylist tokens. No vendor anywhere.
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
        { "type": "content_match", "pattern": "#include <frameworkheader.h>", "weight": 0.4 },
        { "type": "content_match", "pattern": "FORBIDDEN_MARKER", "weight": 0.3 }
      ]
    }
  ]
})";

}  // namespace

// ── Manifest parsing ──

TEST_CASE("import emit parses a well-formed manifest", "[cli][import][emit]") {
    auto m = ie::parse_manifest(mock_manifest());
    REQUIRE(m.ok);
    REQUIRE(m.schema == "pulp.import.emission_manifest.v0");
    REQUIRE(m.importer_id == "mock-importer");
    REQUIRE(m.framework == "example-framework");
    REQUIRE(m.files.size() == 2);
    REQUIRE(m.files[0].path == "src/PluginProcessor.hpp");
    REQUIRE(m.files[0].provenance == ie::Provenance::Generated);
    REQUIRE(m.files[0].has_content);
    REQUIRE(m.formats == std::vector<std::string>{"CLAP"});
    REQUIRE(m.deferred_formats == std::vector<std::string>{"VST3"});
    REQUIRE(m.unresolved.size() == 1);
    REQUIRE(m.migration_status_json.find("unresolved") != std::string::npos);
}

TEST_CASE("import emit rejects a manifest with no files", "[cli][import][emit]") {
    auto m = ie::parse_manifest(
        R"({"schema":"pulp.import.emission_manifest.v0","files":[]})");
    REQUIRE_FALSE(m.ok);
    REQUIRE(m.parse_error.find("no files") != std::string::npos);
}

TEST_CASE("import emit rejects a wrong-schema (misrouted) response",
          "[cli][import][emit]") {
    // An analyze ProjectIR (or a future incompatible emit_result) carries a
    // different schema marker and must not be parsed as an emission manifest,
    // even though it might happen to satisfy later structural checks.
    auto m = ie::parse_manifest(
        R"({"schema":"pulp.import.project_ir.v0",)"
        R"("files":[{"path":"a","provenance":"generated","content":"x"}]})");
    REQUIRE_FALSE(m.ok);
    REQUIRE(m.parse_error.find("schema") != std::string::npos);
    // Absent schema stays tolerated — the file/provenance/content guards apply.
    auto ok = ie::parse_manifest(
        R"({"files":[{"path":"a","provenance":"generated","content":"x"}]})");
    REQUIRE(ok.ok);
}

TEST_CASE("import emit rejects an unknown provenance", "[cli][import][emit]") {
    auto m = ie::parse_manifest(
        R"({"files":[{"path":"a","provenance":"smuggled","content":"x"}]})");
    REQUIRE_FALSE(m.ok);
    REQUIRE(m.parse_error.find("provenance") != std::string::npos);
}

TEST_CASE("import emit summary points at migration status instead of assumed TODO markers",
          "[cli][import][emit]") {
    auto m = ie::parse_manifest(mock_manifest());
    REQUIRE(m.ok);
    REQUIRE(ie::format_unresolved_summary(m) ==
            "1 item(s) to migrate (see migration_status.json)");
    REQUIRE(ie::format_parity_review_step(m) ==
            "Review migration_status.json for the unresolved DSP/UI parity work.");
    REQUIRE(ie::format_unresolved_summary(m).find("TODO(import)") ==
            std::string::npos);
    REQUIRE(ie::format_parity_review_step(m).find("TODO(import)") ==
            std::string::npos);

    auto no_status = ie::parse_manifest(
        R"({"schema":"pulp.import.emission_manifest.v0",)"
        R"("files":[{"path":"a","provenance":"generated","content":"x"}],)"
        R"("unresolved":["migrate DSP"]})");
    REQUIRE(no_status.ok);
    REQUIRE(ie::format_unresolved_summary(no_status) ==
            "1 item(s) to migrate (no migration_status.json emitted)");
    REQUIRE(ie::format_parity_review_step(no_status) ==
            "Review the importer-reported unresolved DSP/UI parity work.");
}

// ── Write plan ──

TEST_CASE("import emit write-plan resolves dests under the output dir",
          "[cli][import][emit]") {
    TempDir user("pulp-emit-user");
    fs::path src = user.path / "Core.h";
    write_file(src, "// user dsp\n");

    auto m = ie::parse_manifest(mock_manifest(src.string()));
    REQUIRE(m.ok);

    TempDir out("pulp-emit-out");
    auto plan = ie::compute_write_plan(m, out.path);
    REQUIRE(plan.ok);
    REQUIRE(plan.actions.size() == 3);

    bool saw_copy = false, saw_write = false;
    for (const auto& a : plan.actions) {
        // Every dest is under the output dir.
        REQUIRE(a.dest.string().rfind(out.path.string(), 0) == 0);
        if (a.is_copy) { saw_copy = true; REQUIRE(a.source == src); }
        else saw_write = true;
    }
    REQUIRE(saw_copy);
    REQUIRE(saw_write);
}

TEST_CASE("import emit write-plan rejects a path that escapes the output dir",
          "[cli][import][emit]") {
    auto m = ie::parse_manifest(
        R"({"files":[{"path":"../escape.txt","provenance":"generated","content":"x"}]})");
    REQUIRE(m.ok);
    TempDir out("pulp-emit-escape");
    auto plan = ie::compute_write_plan(m, out.path);
    REQUIRE_FALSE(plan.ok);
    REQUIRE(plan.error.find("escapes") != std::string::npos);
}

TEST_CASE("import emit write-plan rejects a copied-user-file without copy_from",
          "[cli][import][emit]") {
    auto m = ie::parse_manifest(
        R"({"files":[{"path":"a.h","provenance":"copied-user-file"}]})");
    REQUIRE(m.ok);
    TempDir out("pulp-emit-nocopy");
    auto plan = ie::compute_write_plan(m, out.path);
    REQUIRE_FALSE(plan.ok);
    REQUIRE(plan.error.find("copy_from") != std::string::npos);
}

// ── Clean-room OUTPUT denylist scan ──

TEST_CASE("import emit denylist comes from the known-frameworks content markers",
          "[cli][import][emit][scan]") {
    auto kf = det::parse_index(kNeutralIndex);
    REQUIRE(kf.error.empty());
    auto dl = ies::denylist_from_known_frameworks(kf);
    // The two content_match markers become tokens (lowercased); the file_glob
    // marker does NOT (it is a path pattern, not output content).
    REQUIRE(dl.size() == 2);
    REQUIRE(std::find(dl.begin(), dl.end(), "#include <frameworkheader.h>")
            != dl.end());
    REQUIRE(std::find(dl.begin(), dl.end(), "forbidden_marker") != dl.end());
    REQUIRE(std::find(dl.begin(), dl.end(), "**/*.exproj") == dl.end());
}

TEST_CASE("import emit scan REJECTS framework source in a generated file",
          "[cli][import][emit][scan]") {
    auto kf = det::parse_index(kNeutralIndex);
    auto dl = ies::denylist_from_known_frameworks(kf);

    // A generated file that smuggles a framework umbrella include.
    std::string m_json = mock_manifest(
        /*copy_from=*/{},
        /*generated_content=*/"#include <FrameworkHeader.h>\nstruct X {};\n");
    auto m = ie::parse_manifest(m_json);
    REQUIRE(m.ok);

    auto scan = ies::scan_manifest(m, dl);
    REQUIRE_FALSE(scan.clean);
    REQUIRE_FALSE(scan.hits.empty());
    REQUIRE(scan.hits[0].path == "src/PluginProcessor.hpp");
    REQUIRE(scan.hits[0].token == "#include <frameworkheader.h>");
}

TEST_CASE("import emit scan ACCEPTS the same content in a copied-user-file",
          "[cli][import][emit][scan]") {
    auto kf = det::parse_index(kNeutralIndex);
    auto dl = ies::denylist_from_known_frameworks(kf);

    // The exact same framework include, but in a copied-user-file (the user's
    // own code) → exempt. Manifest has clean generated files + the exempt copy.
    // copy_from must be absolute; the scan never reads the source, so a path
    // that need not exist is fine for the pure scan.
    std::string m_json =
        R"({"schema":"pulp.import.emission_manifest.v0","files":[)"
        R"({"path":"src/Gen.hpp","provenance":"generated","content":"struct Clean {};"},)"
        R"({"path":"src/Core.h","provenance":"copied-user-file",)"
        R"("copy_from":"/abs/Core.h"})"
        R"(]})";
    auto m = ie::parse_manifest(m_json);
    REQUIRE(m.ok);
    // Inject the forbidden token into the copied-user-file's (unused) content to
    // prove provenance — not content — is what exempts it.
    for (auto& f : m.files)
        if (f.provenance == ie::Provenance::CopiedUserFile)
            f.content = "#include <FrameworkHeader.h>";

    auto scan = ies::scan_manifest(m, dl);
    REQUIRE(scan.clean);
    REQUIRE(scan.scanned_files == 1);
    REQUIRE(scan.exempt_files == 1);
}

// ── End-to-end shell-out through a mock emit responder ──

#if defined(PULP_CLI_BINARY) && !defined(_WIN32)
namespace {
fs::path cli_binary() { return fs::path(PULP_CLI_BINARY); }
}  // namespace

TEST_CASE("import emit end-to-end materialises a scaffold + provenance marker",
          "[cli][import][emit][shellout]") {
    TempDir td("pulp-emit-e2e");
    fs::path idx = td.path / "known-frameworks.json";
    write_file(idx, kNeutralIndex);

    // A source project the importer will be pointed at, plus a user DSP file the
    // manifest copies verbatim.
    TempDir proj("pulp-emit-e2e-proj");
    write_file(proj.path / "x.exproj", "<x/>\n");
    fs::path user_core = proj.path / "Core.h";
    write_file(user_core, "// the user's own DSP core\nstruct Core {};\n");

    // A mock SPI responder: dispatches BOTH verbs the CLI calls. analyze returns
    // a minimal ProjectIR; emit returns a manifest that copies the user core.
    fs::path mock = td.path / "mock_emit.sh";
    std::string manifest = mock_manifest(user_core.string());
    // The mock echoes raw JSON; embed the manifest with single quotes escaped.
    write_file(mock,
        "#!/bin/sh\n"
        "while read line; do\n"
        "  case \"$line\" in\n"
        "    *'\"verb\":\"analyze\"'*)\n"
        "      printf '%s\\n' '{\"spi_version\":0,\"id\":\"analyze-1\",\"ok\":true,"
        "\"result\":{\"schema\":\"pulp.import.project_ir.v0\",\"parameters\":[]}}' ;;\n"
        "    *'\"verb\":\"emit\"'*)\n"
        "      printf '%s\\n' '{\"spi_version\":0,\"id\":\"emit-1\",\"ok\":true,"
        "\"result\":" + manifest + "}' ;;\n"
        "  esac\n"
        "done\n");
    fs::permissions(mock, fs::perms::owner_all, fs::perm_options::add);

    fs::path out = td.path / "scaffold";

    pulp::platform::ProcessOptions opts;
    opts.timeout_ms = 60000;
    ::setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);
    ::setenv("PULP_KNOWN_FRAMEWORKS", idx.string().c_str(), 1);

    auto r = pulp::platform::ChildProcess::run(
        cli_binary().string(),
        {"import", "emit", "--from", "example-framework",
         "--importer-cmd", "/bin/sh " + mock.string(),
         "--output", out.string(), proj.path.string()},
        opts);

    INFO("stdout=" << r.stdout_output << " stderr=" << r.stderr_output);
    REQUIRE(r.exit_code == 0);

    // Scaffold present.
    REQUIRE(fs::exists(out / "src" / "PluginProcessor.hpp"));
    REQUIRE(fs::exists(out / "CMakeLists.txt"));
    REQUIRE(fs::exists(out / "migration_status.json"));

    // The user's DSP file copied verbatim (byte-identical to the source).
    REQUIRE(fs::exists(out / "src" / "Core.h"));
    REQUIRE(read_file(out / "src" / "Core.h") == read_file(user_core));

    // Provenance marker written by the SDK, naming the importer + framework.
    fs::path prov = out / ".pulp-import-provenance.json";
    REQUIRE(fs::exists(prov));
    std::string pj = read_file(prov);
    REQUIRE(pj.find("\"importer_id\": \"mock-importer\"") != std::string::npos);
    REQUIRE(pj.find("\"framework\": \"example-framework\"") != std::string::npos);
    REQUIRE(pj.find("source_dir_hash") != std::string::npos);
    REQUIRE(pj.find("\"provenance\": \"copied-user-file\"") != std::string::npos);

    // Summary surfaced the format split + unresolved count.
    REQUIRE(r.stdout_output.find("Materialised Pulp migration scaffold")
            != std::string::npos);
    REQUIRE(r.stdout_output.find("clean-room scan: passed") != std::string::npos);
}

TEST_CASE("import emit end-to-end fails closed when the importer smuggles "
          "framework source",
          "[cli][import][emit][shellout]") {
    TempDir td("pulp-emit-e2e-fail");
    fs::path idx = td.path / "known-frameworks.json";
    write_file(idx, kNeutralIndex);

    TempDir proj("pulp-emit-e2e-fail-proj");
    write_file(proj.path / "x.exproj", "<x/>\n");

    fs::path mock = td.path / "mock_bad.sh";
    // emit returns a manifest whose generated file embeds a denylist token.
    std::string bad_manifest = mock_manifest(
        /*copy_from=*/{},
        /*generated_content=*/"#include <FrameworkHeader.h>\nstruct X {};\n");
    write_file(mock,
        "#!/bin/sh\n"
        "while read line; do\n"
        "  case \"$line\" in\n"
        "    *'\"verb\":\"analyze\"'*)\n"
        "      printf '%s\\n' '{\"spi_version\":0,\"id\":\"analyze-1\",\"ok\":true,"
        "\"result\":{\"schema\":\"pulp.import.project_ir.v0\",\"parameters\":[]}}' ;;\n"
        "    *'\"verb\":\"emit\"'*)\n"
        "      printf '%s\\n' '{\"spi_version\":0,\"id\":\"emit-1\",\"ok\":true,"
        "\"result\":" + bad_manifest + "}' ;;\n"
        "  esac\n"
        "done\n");
    fs::permissions(mock, fs::perms::owner_all, fs::perm_options::add);

    fs::path out = td.path / "scaffold";

    pulp::platform::ProcessOptions opts;
    opts.timeout_ms = 60000;
    ::setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);
    ::setenv("PULP_KNOWN_FRAMEWORKS", idx.string().c_str(), 1);

    auto r = pulp::platform::ChildProcess::run(
        cli_binary().string(),
        {"import", "emit", "--from", "example-framework",
         "--importer-cmd", "/bin/sh " + mock.string(),
         "--output", out.string(), proj.path.string()},
        opts);

    INFO("stdout=" << r.stdout_output << " stderr=" << r.stderr_output);
    REQUIRE(r.exit_code != 0);
    REQUIRE((r.stdout_output + r.stderr_output).find("clean-room output scan FAILED")
            != std::string::npos);
    // Failed closed: no generated source written.
    REQUIRE_FALSE(fs::exists(out / "src" / "PluginProcessor.hpp"));
}
#endif  // PULP_CLI_BINARY && !_WIN32

// ── Vendor-agnostic source guard ──

TEST_CASE("import emit sources name no vendor token",
          "[cli][import][emit][vendor-agnostic]") {
    const fs::path cli_dir = fs::path(PULP_SOURCE_DIR) / "tools" / "cli";
    REQUIRE(fs::exists(cli_dir));

    const std::vector<std::string> banned = {
        "juce", "iplug", "steinberg", "wdl",
    };
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(::tolower(c));
        return s;
    };

    int scanned = 0;
    for (const std::string name :
         {"import_emit.hpp", "import_emit.cpp",
          "import_emit_scan.hpp", "import_emit_scan.cpp"}) {
        fs::path p = cli_dir / name;
        REQUIRE(fs::exists(p));
        ++scanned;
        std::string lc = lower(read_file(p));
        for (const auto& tok : banned) {
            INFO("file=" << name << " token=" << tok);
            REQUIRE(lc.find(tok) == std::string::npos);
        }
    }
    REQUIRE(scanned == 4);
}
