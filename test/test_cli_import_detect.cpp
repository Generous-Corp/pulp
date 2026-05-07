// SPDX-License-Identifier: MIT
//
// pulp #1031 — versioned import-design detection.
//
// Two layers of coverage:
//   1. Unit tests over the manifest parser and clause matcher.
//   2. Fixture-driven detection against test/fixtures/imports/<source>/
//      <format-version>/, asserting (source, format-version,
//      parser-version, match counts, confidence) match the
//      `expected.json` sidecar.
//
// The fixture loop doubles as the CI gate from the issue body — every
// new fixture adds one assertion automatically. No need for a separate
// CI workflow step beyond ctest --test-dir build, which CI already runs.

#include <catch2/catch_test_macros.hpp>

#include "tools/import-design/import_detect.hpp"
#include "tools/cli/json_parser.hpp"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace det = pulp::import_detect;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// `compat.json` lives at the repo root; tests run with cwd inside the
// build tree, so walk up until we find it.
fs::path locate_compat_json() {
    return det::find_compat_json(fs::current_path());
}

fs::path fixtures_root() {
    // Walk up from the compat.json directory; fixtures live alongside
    // it under test/fixtures/imports/.
    auto compat = locate_compat_json();
    if (compat.empty()) return {};
    return compat.parent_path() / "test" / "fixtures" / "imports";
}

// Tiny JSON helper that piggy-backs on the CLI registry parser.
pulp::cli::pkg::JsonValue parse_expected(const fs::path& p) {
    auto text = slurp(p);
    pulp::cli::pkg::JsonParser parser{text, 0};
    return parser.parse();
}

}  // namespace

TEST_CASE("compat.json parses cleanly with the imports section", "[cli][import-detect][issue-1031]") {
    auto compat = locate_compat_json();
    REQUIRE_FALSE(compat.empty());
    auto manifest = det::parse_compat_json(slurp(compat));
    REQUIRE(manifest.has_value());
    CHECK(manifest->compat_schema_version == "0.2");
    CHECK_FALSE(manifest->sources.empty());

    bool saw_stitch = false;
    bool saw_claude = false;
    for (const auto& src : manifest->sources) {
        if (src.source == "stitch") {
            saw_stitch = true;
            CHECK(src.parser_version == "1.0");
            REQUIRE_FALSE(src.formats.empty());
            CHECK(src.formats.front().format_version == "2025.04");
            // Stitch 2025.04 has 3 fingerprint clauses.
            CHECK(src.formats.front().fingerprint.size() == 3);
        } else if (src.source == "claude") {
            saw_claude = true;
            CHECK(src.parser_version == "2.1");
            REQUIRE_FALSE(src.formats.empty());
            CHECK(src.formats.front().format_version == "2024.10");
        }
    }
    CHECK(saw_stitch);
    CHECK(saw_claude);
}

TEST_CASE("parse_compat_json rejects malformed input", "[cli][import-detect][issue-1031]") {
    CHECK_FALSE(det::parse_compat_json("").has_value());
    CHECK_FALSE(det::parse_compat_json("not json").has_value());
    // Missing `imports` is a hard error — the manifest is meaningless
    // to the detector without it.
    CHECK_FALSE(det::parse_compat_json("{\"compat-schema-version\":\"0.2\"}").has_value());
}

TEST_CASE("clause matcher honors the fingerprint vocabulary", "[cli][import-detect][issue-1031]") {
    det::InputSnapshot snap;
    snap.is_directory = true;
    snap.directory_basenames = {"code.html", "DESIGN.md", "screen.png", "extras"};
    snap.script_srcs = {"https://cdn.tailwindcss.com?plugins=forms,container-queries"};
    snap.script_types = {"module", "__bundler/template"};
    snap.tailwind_tokens = {"primary", "on-primary", "surface-container"};

    SECTION("directory-files clause requires every listed file") {
        det::FingerprintClause c;
        c.kind = det::FingerprintClause::Kind::directory_files;
        c.files = {"code.html", "DESIGN.md", "screen.png"};
        CHECK(det::match_clause(c, snap));

        c.files.push_back("does-not-exist");
        CHECK_FALSE(det::match_clause(c, snap));
    }

    SECTION("html-script-src clause uses ECMAScript regex semantics") {
        det::FingerprintClause c;
        c.kind = det::FingerprintClause::Kind::html_script_src;
        c.regex = R"(cdn\.tailwindcss\.com\?plugins=forms,container-queries)";
        CHECK(det::match_clause(c, snap));

        c.regex = "no-such-cdn";
        CHECK_FALSE(det::match_clause(c, snap));
    }

    SECTION("html-script-type clause is case-insensitive trim") {
        det::FingerprintClause c;
        c.kind = det::FingerprintClause::Kind::html_script_type;
        c.value = "__bundler/template";
        CHECK(det::match_clause(c, snap));

        c.value = "missing/type";
        CHECK_FALSE(det::match_clause(c, snap));
    }

    SECTION("tailwind-config-token clause matches if any-of token is present") {
        det::FingerprintClause c;
        c.kind = det::FingerprintClause::Kind::tailwind_config_token;
        c.any_of = {"surface-container", "rainbow-gradient"};
        CHECK(det::match_clause(c, snap));

        c.any_of = {"never-emitted-token"};
        CHECK_FALSE(det::match_clause(c, snap));
    }

    SECTION("unknown clause kinds never match") {
        det::FingerprintClause c;
        c.kind = det::FingerprintClause::Kind::unknown;
        CHECK_FALSE(det::match_clause(c, snap));
    }
}

TEST_CASE("detect picks the highest-confidence format", "[cli][import-detect][issue-1031]") {
    auto compat = locate_compat_json();
    REQUIRE_FALSE(compat.empty());
    auto manifest = det::parse_compat_json(slurp(compat));
    REQUIRE(manifest.has_value());

    SECTION("empty snapshot produces no match") {
        det::InputSnapshot snap;
        auto r = det::detect(*manifest, snap);
        CHECK(r.ok);
        CHECK(r.source.empty());
        CHECK(r.matched_clauses == 0);
    }

    SECTION("synthetic stitch snapshot matches all 3 clauses") {
        det::InputSnapshot snap;
        snap.is_directory = true;
        snap.directory_basenames = {"code.html", "DESIGN.md", "screen.png"};
        snap.script_srcs = {"https://cdn.tailwindcss.com?plugins=forms,container-queries"};
        snap.tailwind_tokens = {"surface-container", "on-primary"};
        auto r = det::detect(*manifest, snap);
        CHECK(r.source == "stitch");
        CHECK(r.format_version == "2025.04");
        CHECK(r.parser_version == "1.0");
        CHECK(r.matched_clauses == 3);
        CHECK(r.total_clauses == 3);
        CHECK(r.confidence_pct == 100);
    }

    SECTION("synthetic claude snapshot matches the bundler-template clause") {
        det::InputSnapshot snap;
        snap.script_types = {"__bundler/template"};
        auto r = det::detect(*manifest, snap);
        CHECK(r.source == "claude");
        CHECK(r.format_version == "2024.10");
        CHECK(r.parser_version == "2.1");
        CHECK(r.matched_clauses == 1);
        CHECK(r.confidence_pct == 100);
    }
}

TEST_CASE("snapshot_input handles file and directory inputs", "[cli][import-detect][issue-1031]") {
    auto root = fixtures_root();
    REQUIRE_FALSE(root.empty());
    REQUIRE(fs::exists(root));

    SECTION("directory snapshot collects basenames + html") {
        auto snap = det::snapshot_input(root / "stitch" / "2025.04");
        CHECK(snap.is_directory);
        CHECK_FALSE(snap.directory_basenames.empty());
        // code.html should be parsed → script_srcs populated.
        CHECK_FALSE(snap.script_srcs.empty());
        // tailwind-config-token tokens scraped from the inline config.
        bool saw_surface = false;
        for (const auto& t : snap.tailwind_tokens)
            if (t == "surface-container") saw_surface = true;
        CHECK(saw_surface);
    }

    SECTION("file snapshot uses the file as html_text") {
        auto snap = det::snapshot_input(root / "claude" / "2024.10" / "example.html");
        CHECK_FALSE(snap.is_directory);
        bool saw_bundler = false;
        for (const auto& t : snap.script_types)
            if (t == "__bundler/template") saw_bundler = true;
        CHECK(saw_bundler);
    }

    SECTION("missing path returns an empty snapshot, not a crash") {
        auto snap = det::snapshot_input(root / "does" / "not" / "exist");
        CHECK_FALSE(snap.is_directory);
        CHECK(snap.html_text.empty());
        CHECK(snap.directory_basenames.empty());
    }
}

// ── Fixture-driven gate ────────────────────────────────────────────────

namespace {

struct Expected {
    std::string source;
    std::string format_version;
    std::string parser_version;
    int matched_clauses = 0;
    int total_clauses = 0;
    int min_confidence_pct = 0;
};

Expected load_expected(const fs::path& p) {
    Expected e;
    auto v = parse_expected(p);
    if (v.type != pulp::cli::pkg::JsonValue::Object) return e;
    if (auto* s = v.get("source"); s) e.source = s->as_string();
    if (auto* fv = v.get("format-version"); fv) e.format_version = fv->as_string();
    if (auto* pv = v.get("parser-version"); pv) e.parser_version = pv->as_string();
    if (auto* m = v.get("matched-clauses"); m) e.matched_clauses = m->as_int();
    if (auto* t = v.get("total-clauses"); t) e.total_clauses = t->as_int();
    if (auto* c = v.get("min-confidence-pct"); c) e.min_confidence_pct = c->as_int();
    return e;
}

void run_fixture(const fs::path& fixture_dir,
                 const det::ImportsManifest& manifest) {
    INFO("fixture: " << fixture_dir);

    auto expected_path = fixture_dir / "expected.json";
    REQUIRE(fs::exists(expected_path));
    auto exp = load_expected(expected_path);

    // Choose the input target deterministically. Prefer directory scans
    // for fixtures that model directory exports; otherwise scan the
    // first HTML payload by sorted path so Linux/macOS directory order
    // cannot change the expected detector result.
    fs::path scan_target = fixture_dir;
    if (!fs::exists(fixture_dir / "code.html")) {
        std::vector<fs::path> html_payloads;
        for (auto& entry : fs::directory_iterator(fixture_dir)) {
            auto fname = entry.path().filename().string();
            if (fname == "expected.json") continue;
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (entry.is_regular_file() && (ext == ".html" || ext == ".htm"))
                html_payloads.push_back(entry.path());
        }
        std::sort(html_payloads.begin(), html_payloads.end());
        if (!html_payloads.empty())
            scan_target = html_payloads.front();
    }

    auto snap = det::snapshot_input(scan_target);
    auto result = det::detect(manifest, snap);

    CHECK(result.ok);
    CHECK(result.source == exp.source);
    CHECK(result.format_version == exp.format_version);
    CHECK(result.parser_version == exp.parser_version);
    CHECK(result.matched_clauses == exp.matched_clauses);
    CHECK(result.total_clauses == exp.total_clauses);
    CHECK(result.confidence_pct >= exp.min_confidence_pct);
}

}  // namespace

TEST_CASE("import-detect fixtures match expected.json sidecars", "[cli][import-detect][issue-1031][fixtures]") {
    auto compat = locate_compat_json();
    REQUIRE_FALSE(compat.empty());
    auto manifest = det::parse_compat_json(slurp(compat));
    REQUIRE(manifest.has_value());

    auto root = fixtures_root();
    REQUIRE_FALSE(root.empty());
    REQUIRE(fs::exists(root));

    int fixture_count = 0;
    for (auto& source_dir : fs::directory_iterator(root)) {
        if (!source_dir.is_directory()) continue;
        auto source_name = source_dir.path().filename().string();

        // The `_unknown/` fixture is a flat directory (no per-version
        // subdirs) — run it directly.
        if (source_name == "_unknown") {
            run_fixture(source_dir.path(), *manifest);
            ++fixture_count;
            continue;
        }

        for (auto& version_dir : fs::directory_iterator(source_dir.path())) {
            if (!version_dir.is_directory()) continue;
            run_fixture(version_dir.path(), *manifest);
            ++fixture_count;
        }
    }
    CHECK(fixture_count >= 3);  // stitch + claude + _unknown
}

TEST_CASE("report-new-format diffs unknown tokens against the closest match",
          "[cli][import-detect][issue-1031]") {
    auto compat = locate_compat_json();
    REQUIRE_FALSE(compat.empty());
    auto manifest = det::parse_compat_json(slurp(compat));
    REQUIRE(manifest.has_value());

    // Synthesize a "newer" Stitch export: matches the html-script-src
    // clause, ships unknown tokens.
    det::InputSnapshot snap;
    snap.is_directory = true;
    snap.directory_basenames = {"code.html", "DESIGN.md", "screen.png"};
    snap.script_srcs = {"https://cdn.tailwindcss.com?plugins=forms,container-queries"};
    snap.tailwind_tokens = {"surface-container", "on-primary",
                            "rainbow-gradient", "pulse-spring"};

    auto closest = det::detect(*manifest, snap);
    REQUIRE(closest.source == "stitch");
    auto report = det::build_new_format_report(*manifest, snap, closest);

    CHECK(report.candidate_source == "stitch");
    CHECK(report.based_on_format_version == "2025.04");
    // The two new tokens should appear in additions; the known ones
    // should not.
    bool saw_rainbow = false;
    bool saw_pulse = false;
    bool saw_known = false;
    for (const auto& a : report.additions) {
        if (a == "rainbow-gradient") saw_rainbow = true;
        if (a == "pulse-spring") saw_pulse = true;
        if (a == "surface-container" || a == "on-primary") saw_known = true;
    }
    CHECK(saw_rainbow);
    CHECK(saw_pulse);
    CHECK_FALSE(saw_known);

    auto json = det::render_new_format_json(report);
    CHECK(json.find("\"candidate-source\": \"stitch\"") != std::string::npos);
    CHECK(json.find("rainbow-gradient") != std::string::npos);
    CHECK(json.find("\"based-on\":") != std::string::npos);
}
