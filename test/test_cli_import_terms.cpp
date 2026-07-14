// SPDX-License-Identifier: MIT
//
// Tests for the IMPORTER_TERMS accept-to-run gate and the mainline-PR
// provenance check.
//
//   (a) `pulp import inspect/emit` BLOCKS with a clear message + nonzero exit
//       when the importer's terms have not been accepted.
//   (b) `--accept-importer-terms` (and a previously-recorded acceptance)
//       unblocks the gate.
//   (c) a changed terms hash re-prompts (the recorded acceptance no longer
//       matches), exercised over the pure acceptance-store logic.
//   (d) the provenance check PASSES a well-formed marker and FAILS a
//       missing / tampered one (shell-out to the Python audit).
//   (e) vendor-agnostic source guard over the new SDK files.
//
// Vendor-agnostic rule (firm): these committed tests name NO vendor and NO
// framework. They use a neutral framework id + neutral terms text.

#include <catch2/catch_test_macros.hpp>

#include "tools/cli/import_terms.hpp"

#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
namespace terms = pulp::cli::import_terms;

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
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << content;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

const char* kNeutralTerms =
    "Example Importer — Terms of Use.\n"
    "Provided AS IS. You are responsible for confirming you have the rights\n"
    "to import this project and to use the generated output.";

}  // namespace

// ── Pure acceptance-store logic ──

TEST_CASE("import terms: hash is stable and version-sensitive",
          "[cli][import][terms]") {
    auto h1 = terms::terms_hash("hello terms");
    auto h2 = terms::terms_hash("hello terms");
    auto h3 = terms::terms_hash("hello terms (v2)");
    REQUIRE(h1 == h2);
    REQUIRE(h1 != h3);
    REQUIRE(h1.rfind("0x", 0) == 0);
}

TEST_CASE("import terms: has_terms() reflects a non-empty body",
          "[cli][import][terms]") {
    terms::TermsDescriptor td;
    REQUIRE_FALSE(terms::has_terms(td));
    td.terms_text = "  \n  ";  // whitespace only → still nothing to accept
    REQUIRE_FALSE(terms::has_terms(td));
    td.terms_text = kNeutralTerms;
    REQUIRE(terms::has_terms(td));
}

TEST_CASE("import terms: acceptance store round-trips", "[cli][import][terms]") {
    terms::AcceptanceRecord r;
    r.importer_id = "import-example";
    r.terms_version = "1";
    r.terms_hash = terms::terms_hash(kNeutralTerms);
    r.vendor_id = "vendor-x";
    r.accepted_at = "2026-06-08T00:00:00Z";
    r.method = "flag";

    auto store = terms::format_acceptance_store({r});
    auto back = terms::parse_acceptance_store(store);
    REQUIRE(back.size() == 1);
    REQUIRE(back[0].importer_id == "import-example");
    REQUIRE(back[0].terms_hash == r.terms_hash);
    REQUIRE(back[0].vendor_id == "vendor-x");
    REQUIRE(back[0].method == "flag");
}

TEST_CASE("import terms: a changed terms hash re-prompts (acceptance no longer matches)",
          "[cli][import][terms]") {
    // Accept v1, then change the terms text → the recorded acceptance must NOT
    // satisfy is_accepted for the new hash. This is the re-prompt trigger.
    terms::AcceptanceRecord r;
    r.importer_id = "import-example";
    r.terms_hash = terms::terms_hash(kNeutralTerms);
    std::vector<terms::AcceptanceRecord> recs = {r};

    REQUIRE(terms::is_accepted(recs, "import-example",
                               terms::terms_hash(kNeutralTerms)));

    const std::string changed = std::string(kNeutralTerms) + "\n(revised)";
    REQUIRE_FALSE(terms::is_accepted(recs, "import-example",
                                     terms::terms_hash(changed)));

    // Re-accepting upserts (one record per importer id, now keyed to the new hash).
    terms::AcceptanceRecord r2 = r;
    r2.terms_hash = terms::terms_hash(changed);
    recs = terms::upsert_acceptance(recs, r2);
    REQUIRE(recs.size() == 1);
    REQUIRE(terms::is_accepted(recs, "import-example", terms::terms_hash(changed)));
    REQUIRE_FALSE(terms::is_accepted(recs, "import-example",
                                     terms::terms_hash(kNeutralTerms)));
}

TEST_CASE("import terms: run_gate accepts via flag, records it, and is idempotent",
          "[cli][import][terms]") {
    TempDir home("pulp-terms-home");
    fs::path store = home.path / "importer-terms-accepted.json";

    terms::TermsDescriptor td;
    td.importer_id = "import-example";
    td.terms_version = "1";
    td.terms_text = kNeutralTerms;
    td.vendor_id = "vendor-x";

    std::istringstream in;
    std::ostringstream out;
    terms::GateIo io{in, out, /*interactive=*/false};

    // First pass: --accept flag records and passes (no prompt).
    auto r1 = terms::run_gate(td, store, /*accept_flag=*/true,
                              "2026-06-08T00:00:00Z", io);
    REQUIRE(r1 == terms::GateResult::Accepted);
    REQUIRE(fs::exists(store));

    // Second pass: already recorded → Accepted even without the flag and
    // non-interactive (no re-prompt).
    auto r2 = terms::run_gate(td, store, /*accept_flag=*/false,
                              "2026-06-08T00:00:00Z", io);
    REQUIRE(r2 == terms::GateResult::Accepted);
}

TEST_CASE("import terms: run_gate blocks non-interactively without the flag",
          "[cli][import][terms]") {
    TempDir home("pulp-terms-block");
    fs::path store = home.path / "importer-terms-accepted.json";

    terms::TermsDescriptor td;
    td.importer_id = "import-example";
    td.terms_text = kNeutralTerms;

    std::istringstream in;
    std::ostringstream out;
    terms::GateIo io{in, out, /*interactive=*/false};
    auto r = terms::run_gate(td, store, /*accept_flag=*/false,
                             "2026-06-08T00:00:00Z", io);
    REQUIRE(r == terms::GateResult::NonInteractive);
    REQUIRE_FALSE(fs::exists(store));
}

TEST_CASE("import terms: run_gate honours an interactive typed 'accept'",
          "[cli][import][terms]") {
    TempDir home("pulp-terms-typed");
    fs::path store = home.path / "importer-terms-accepted.json";

    terms::TermsDescriptor td;
    td.importer_id = "import-example";
    td.terms_text = kNeutralTerms;

    {
        std::istringstream in("accept\n");
        std::ostringstream out;
        terms::GateIo io{in, out, /*interactive=*/true};
        auto r = terms::run_gate(td, store, false, "2026-06-08T00:00:00Z", io);
        REQUIRE(r == terms::GateResult::Accepted);
        REQUIRE(out.str().find("Type \"accept\"") != std::string::npos);
    }
    // A non-"accept" answer declines (use a fresh, never-accepted importer).
    {
        TempDir home2("pulp-terms-decline");
        fs::path store2 = home2.path / "importer-terms-accepted.json";
        std::istringstream in("no\n");
        std::ostringstream out;
        terms::GateIo io{in, out, /*interactive=*/true};
        auto r = terms::run_gate(td, store2, false, "2026-06-08T00:00:00Z", io);
        REQUIRE(r == terms::GateResult::Declined);
    }
}

TEST_CASE("import terms: no terms declared → gate passes through",
          "[cli][import][terms]") {
    TempDir home("pulp-terms-none");
    fs::path store = home.path / "importer-terms-accepted.json";
    terms::TermsDescriptor td;
    td.importer_id = "import-example";  // no terms_text
    std::istringstream in;
    std::ostringstream out;
    terms::GateIo io{in, out, false};
    auto r = terms::run_gate(td, store, false, "2026-06-08T00:00:00Z", io);
    REQUIRE(r == terms::GateResult::NoTermsToAccept);
}

// ── End-to-end gate behavior through the built CLI ──

#if defined(PULP_CLI_BINARY) && !defined(_WIN32)
namespace {
fs::path cli_binary() { return fs::path(PULP_CLI_BINARY); }

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
        { "type": "file_glob", "pattern": "**/*.exproj", "weight": 0.6 }
      ]
    }
  ]
})";

// A mock SPI responder that answers analyze (so inspect would otherwise
// succeed). Used to prove the gate blocks BEFORE the importer is driven.
fs::path write_mock_responder(const fs::path& dir) {
    fs::path mock = dir / "mock_analyze.sh";
    write_file(mock,
        "#!/bin/sh\n"
        "while read line; do\n"
        "  case \"$line\" in\n"
        "    *'\"verb\":\"analyze\"'*)\n"
        "      printf '%s\\n' '{\"spi_version\":0,\"id\":\"analyze-1\",\"ok\":true,"
        "\"result\":{\"schema\":\"pulp.import.project_ir.v0\",\"parameters\":[]}}' ;;\n"
        "  esac\n"
        "done\n");
    fs::permissions(mock, fs::perms::owner_all, fs::perm_options::add);
    return mock;
}

pulp::platform::ProcessResult run_inspect(const fs::path& home,
                                          const fs::path& idx,
                                          const fs::path& mock,
                                          const fs::path& proj,
                                          bool accept_flag) {
    pulp::platform::ProcessOptions opts;
    opts.timeout_ms = 60000;
    ::setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);
    ::setenv("PULP_KNOWN_FRAMEWORKS", idx.string().c_str(), 1);
    ::setenv("PULP_HOME", home.string().c_str(), 1);
    std::vector<std::string> argv = {
        "import", "inspect", "--from", "example-framework",
        "--importer-cmd", "/bin/sh " + mock.string(),
        "--importer-terms-text", kNeutralTerms,
        "--importer-terms-version", "1",
        proj.string()};
    if (accept_flag) argv.insert(argv.begin() + 2, "--accept-importer-terms");
    return pulp::platform::ChildProcess::run(cli_binary().string(), argv, opts);
}
}  // namespace

TEST_CASE("import inspect BLOCKS when terms are not accepted (non-interactive)",
          "[cli][import][terms][shellout]") {
    TempDir td("pulp-terms-e2e-block");
    fs::path idx = td.path / "known-frameworks.json";
    write_file(idx, kNeutralIndex);
    fs::path mock = write_mock_responder(td.path);
    TempDir proj("pulp-terms-e2e-block-proj");
    write_file(proj.path / "x.exproj", "<x/>\n");
    TempDir home("pulp-terms-e2e-block-home");

    auto r = run_inspect(home.path, idx, mock, proj.path, /*accept=*/false);
    INFO("stdout=" << r.stdout_output << " stderr=" << r.stderr_output);
    REQUIRE(r.exit_code != 0);
    REQUIRE((r.stdout_output + r.stderr_output).find("terms of use")
            != std::string::npos);
    // Failed closed: no acceptance recorded.
    REQUIRE_FALSE(fs::exists(home.path / "importer-terms-accepted.json"));
}

TEST_CASE("import inspect UNBLOCKS with --accept-importer-terms and records it",
          "[cli][import][terms][shellout]") {
    TempDir td("pulp-terms-e2e-accept");
    fs::path idx = td.path / "known-frameworks.json";
    write_file(idx, kNeutralIndex);
    fs::path mock = write_mock_responder(td.path);
    TempDir proj("pulp-terms-e2e-accept-proj");
    write_file(proj.path / "x.exproj", "<x/>\n");
    TempDir home("pulp-terms-e2e-accept-home");

    auto r = run_inspect(home.path, idx, mock, proj.path, /*accept=*/true);
    INFO("stdout=" << r.stdout_output << " stderr=" << r.stderr_output);
    REQUIRE(r.exit_code == 0);

    // Acceptance recorded under $PULP_HOME.
    fs::path store = home.path / "importer-terms-accepted.json";
    REQUIRE(fs::exists(store));
    std::string sj = read_file(store);
    REQUIRE(sj.find("import-example-framework") != std::string::npos);
    REQUIRE(sj.find("acceptance_hash") != std::string::npos);

    // A SECOND run without the flag now passes (recorded acceptance unblocks).
    auto r2 = run_inspect(home.path, idx, mock, proj.path, /*accept=*/false);
    INFO("stdout2=" << r2.stdout_output << " stderr2=" << r2.stderr_output);
    REQUIRE(r2.exit_code == 0);
}
#endif  // PULP_CLI_BINARY && !_WIN32

// ── Provenance PR-check (shell-out to the Python audit) ──

#if defined(PULP_SOURCE_DIR) && !defined(_WIN32)
namespace {
fs::path provenance_script() {
    return fs::path(PULP_SOURCE_DIR) / "tools" / "scripts" /
           "check_import_provenance.py";
}

// A well-formed provenance marker matching the SDK's build_provenance_marker
// shape, listing one generated file.
std::string good_marker() {
    return R"({
  "schema": "pulp.import.provenance.v0",
  "importer_id": "mock-importer",
  "framework": "example-framework",
  "spi_version": 0,
  "emitted_at": "2026-06-08T00:00:00Z",
  "source_dir_hash": "0x0123456789abcdef",
  "files": [
    {"path": "src/PluginProcessor.hpp", "provenance": "generated"},
    {"path": "src/Core.h", "provenance": "copied-user-file"}
  ]
}
)";
}

pulp::platform::ProcessResult run_provenance(const fs::path& proj,
                                             const fs::path& idx) {
    pulp::platform::ProcessOptions opts;
    opts.timeout_ms = 60000;
    // The provenance script resolves the denylist from $PULP_KNOWN_FRAMEWORKS.
    // Unset it (rather than inherit a stale value) when no index is supplied so
    // the "no marker" / structural-only paths are exercised cleanly.
    if (!idx.empty()) ::setenv("PULP_KNOWN_FRAMEWORKS", idx.string().c_str(), 1);
    else ::unsetenv("PULP_KNOWN_FRAMEWORKS");
    return pulp::platform::ChildProcess::run(
        "python3",
        {provenance_script().string(), proj.string()}, opts);
}

const char* kScanIndex = R"({
  "schema": "pulp.import.known_frameworks.v0",
  "frameworks": [
    { "framework_id": "example-framework", "display_name": "Example",
      "importer_tool_id": "import-example-framework", "spi_min": 0, "spi_max": 0,
      "detection": [
        { "type": "content_match", "pattern": "#include <frameworkheader.h>", "weight": 0.4 }
      ] }
  ]
})";
}  // namespace

TEST_CASE("provenance check PASSES a well-formed clean-room marker",
          "[cli][import][provenance][shellout]") {
    TempDir proj("pulp-prov-good");
    write_file(proj.path / ".pulp-import-provenance.json", good_marker());
    write_file(proj.path / "src" / "PluginProcessor.hpp", "struct Clean {};\n");
    write_file(proj.path / "src" / "Core.h", "// user dsp\n");
    TempDir idxd("pulp-prov-idx");
    fs::path idx = idxd.path / "known-frameworks.json";
    write_file(idx, kScanIndex);

    auto r = run_provenance(proj.path, idx);
    INFO("stdout=" << r.stdout_output << " stderr=" << r.stderr_output);
    REQUIRE(r.exit_code == 0);
}

TEST_CASE("provenance check FAILS a project missing the marker",
          "[cli][import][provenance][shellout]") {
    TempDir proj("pulp-prov-missing");
    write_file(proj.path / "src" / "PluginProcessor.hpp", "struct X {};\n");
    auto r = run_provenance(proj.path, {});
    INFO("stdout=" << r.stdout_output << " stderr=" << r.stderr_output);
    REQUIRE(r.exit_code != 0);
    REQUIRE(r.stderr_output.find("missing provenance marker") != std::string::npos);
}

TEST_CASE("provenance check FAILS a tampered marker (bad provenance value)",
          "[cli][import][provenance][shellout]") {
    TempDir proj("pulp-prov-tamper");
    // A marker whose file claims an invalid provenance value.
    write_file(proj.path / ".pulp-import-provenance.json", R"({
  "schema": "pulp.import.provenance.v0",
  "importer_id": "mock-importer",
  "framework": "example-framework",
  "spi_version": 0,
  "emitted_at": "2026-06-08T00:00:00Z",
  "source_dir_hash": "0xabc",
  "files": [ {"path": "src/X.hpp", "provenance": "smuggled"} ]
}
)");
    write_file(proj.path / "src" / "X.hpp", "struct X {};\n");
    auto r = run_provenance(proj.path, {});
    INFO("stdout=" << r.stdout_output << " stderr=" << r.stderr_output);
    REQUIRE(r.exit_code != 0);
    REQUIRE(r.stderr_output.find("invalid provenance") != std::string::npos);
}

TEST_CASE("provenance check FAILS framework source in a generated file",
          "[cli][import][provenance][shellout]") {
    TempDir proj("pulp-prov-leak");
    write_file(proj.path / ".pulp-import-provenance.json", good_marker());
    // The generated file smuggles a framework-source marker → contract breach.
    write_file(proj.path / "src" / "PluginProcessor.hpp",
               "#include <FrameworkHeader.h>\nstruct X {};\n");
    write_file(proj.path / "src" / "Core.h", "// user dsp\n");
    TempDir idxd("pulp-prov-leak-idx");
    fs::path idx = idxd.path / "known-frameworks.json";
    write_file(idx, kScanIndex);

    auto r = run_provenance(proj.path, idx);
    INFO("stdout=" << r.stdout_output << " stderr=" << r.stderr_output);
    REQUIRE(r.exit_code != 0);
    REQUIRE(r.stderr_output.find("clean-room contract") != std::string::npos);
}
#endif  // PULP_SOURCE_DIR && !_WIN32

// ── Vendor-agnostic source guard ──

