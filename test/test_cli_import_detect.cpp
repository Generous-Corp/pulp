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
    CHECK(manifest->compat_schema_version == "0.3");
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

TEST_CASE("parse_compat_json preserves sources with empty detected formats",
          "[cli][import-detect][coverage][requested]") {
    auto manifest = det::parse_compat_json(R"({
  "compat-schema-version": "1.0",
  "imports": {
    "empty": {"parser-version": "1.0", "detected-formats": []},
    "bad": {"parser-version": "1.0", "detected-formats": [42, []]},
    "good": {
      "parser-version": "1.0",
      "detected-formats": [{
        "format-version": "2026.05",
        "fingerprint": [{"kind": "filename", "regex": ".*\\.html"}]
      }]
    }
  }
})");

    REQUIRE(manifest.has_value());
    REQUIRE(manifest->sources.size() == 3);
    REQUIRE(manifest->sources[0].source == "empty");
    REQUIRE(manifest->sources[0].formats.empty());
    REQUIRE(manifest->sources[1].source == "bad");
    REQUIRE(manifest->sources[1].formats.empty());
    REQUIRE(manifest->sources[2].source == "good");
    REQUIRE(manifest->sources[2].formats.size() == 1);
}

TEST_CASE("parse_compat_json keeps optional format metadata and skips bad shapes",
          "[cli][import-detect][coverage]") {
    auto manifest = det::parse_compat_json(R"({
  "compat-schema-version": "0.9",
  "imports": {
    "bad-source": [],
    "demo": {
      "parser-version": "3.1",
      "detected-formats": [
        [],
        {
          "format-version": "2026.05",
          "introduced": "2026-05-01",
          "deprecated": "2026-12-31",
          "notes": "fixture notes",
          "match": "all-of",
          "min-confidence-pct": 80,
          "fingerprint": [
            [],
            {"kind": "filename", "regex": "(?i)^design\\.md$"}
          ]
        }
      ]
    }
  }
})");

    REQUIRE(manifest.has_value());
    REQUIRE(manifest->sources.size() == 1);
    const auto& source = manifest->sources.front();
    REQUIRE(source.source == "demo");
    REQUIRE(source.formats.size() == 1);
    const auto& fmt = source.formats.front();
    CHECK(fmt.parser_version == "3.1");
    CHECK(fmt.format_version == "2026.05");
    CHECK(fmt.introduced == "2026-05-01");
    CHECK(fmt.deprecated == "2026-12-31");
    CHECK(fmt.notes == "fixture notes");
    CHECK(fmt.match == "all-of");
    CHECK(fmt.min_confidence_pct == 80);
    REQUIRE(fmt.fingerprint.size() == 2);
    CHECK(fmt.fingerprint[0].kind == det::FingerprintClause::Kind::unknown);
    CHECK(fmt.fingerprint[1].kind == det::FingerprintClause::Kind::filename);
}

TEST_CASE("parse_compat_json ignores malformed optional fields without dropping formats",
          "[cli][import-detect][coverage]") {
    auto manifest = det::parse_compat_json(R"({
  "compat-schema-version": 3,
  "imports": {
    "demo": {
      "parser-version": 7,
      "detected-formats": [
        {
          "format-version": 2026,
          "introduced": false,
          "deprecated": null,
          "notes": [],
          "match": {},
          "min-confidence-pct": "90",
          "fingerprint": [
            {"kind": "frontmatter-key", "required": "name"},
            {"kind": "tailwind-config-token", "any-of": ["surface", 4]},
            {"kind": "new-future-kind", "value": "x"}
          ]
        }
      ]
    }
  }
})");

    REQUIRE(manifest.has_value());
    CHECK(manifest->compat_schema_version.empty());
    REQUIRE(manifest->sources.size() == 1);
    const auto& source = manifest->sources.front();
    CHECK(source.source == "demo");
    CHECK(source.parser_version.empty());
    REQUIRE(source.formats.size() == 1);
    const auto& fmt = source.formats.front();
    CHECK(fmt.parser_version.empty());
    CHECK(fmt.format_version.empty());
    CHECK(fmt.introduced.empty());
    CHECK(fmt.deprecated.empty());
    CHECK(fmt.notes.empty());
    CHECK(fmt.match.empty());
    CHECK(fmt.min_confidence_pct == 0);
    REQUIRE(fmt.fingerprint.size() == 3);
    CHECK(fmt.fingerprint[0].kind == det::FingerprintClause::Kind::frontmatter_key);
    CHECK(fmt.fingerprint[0].required == "name");
    CHECK(fmt.fingerprint[1].any_of == std::vector<std::string>{"surface"});
    CHECK(fmt.fingerprint[2].kind == det::FingerprintClause::Kind::unknown);
    CHECK(fmt.fingerprint[2].raw_kind == "new-future-kind");
}

TEST_CASE("find_compat_json walks parents and reports absence",
          "[cli][import-detect][coverage]") {
    auto dir = fs::temp_directory_path() / "pulp-import-detect-compat-walk";
    fs::remove_all(dir);
    fs::create_directories(dir / "repo" / "nested" / "child");

    const auto compat = dir / "repo" / "compat.json";
    {
        std::ofstream f(compat);
        f << "{\"imports\":{}}";
    }

    CHECK(det::find_compat_json(dir / "repo" / "nested" / "child") == compat);
    CHECK(det::find_compat_json(dir / "repo") == compat);

    fs::remove(compat);
    CHECK(det::find_compat_json(dir / "repo" / "nested" / "child").empty());

    fs::remove_all(dir);
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

TEST_CASE("clause matcher handles filename regex flags and invalid patterns",
          "[cli][import-detect][coverage][phase3-large]") {
    det::InputSnapshot snap;
    snap.filename = "DESIGN.md";
    snap.directory_basenames = {"DESIGN.md"};

    det::FingerprintClause c;
    c.kind = det::FingerprintClause::Kind::filename;
    c.regex = "(?i)^design\\.md$";
    CHECK(det::match_clause(c, snap));

    c.regex = "^design\\.md$";
    CHECK_FALSE(det::match_clause(c, snap));

    c.regex = "[unterminated";
    CHECK_FALSE(det::match_clause(c, snap));
}

TEST_CASE("clause matcher rejects empty filename and script clauses",
          "[cli][import-detect][coverage][phase3-large]") {
    det::InputSnapshot snap;
    det::FingerprintClause c;

    c.kind = det::FingerprintClause::Kind::filename;
    c.regex = ".*";
    CHECK_FALSE(det::match_clause(c, snap));

    c.kind = det::FingerprintClause::Kind::html_script_src;
    c.regex = "cdn";
    CHECK_FALSE(det::match_clause(c, snap));

    c.kind = det::FingerprintClause::Kind::html_script_type;
    c.value = "module";
    CHECK_FALSE(det::match_clause(c, snap));
}

TEST_CASE("clause matcher covers frontmatter fence and required key clauses",
          "[cli][import-detect][coverage][phase3-large]") {
    det::InputSnapshot snap;
    snap.has_frontmatter_fence = true;
    snap.frontmatter_keys = {"name", "colors", "typography"};

    det::FingerprintClause fence;
    fence.kind = det::FingerprintClause::Kind::frontmatter_fence;
    CHECK(det::match_clause(fence, snap));

    det::FingerprintClause required;
    required.kind = det::FingerprintClause::Kind::frontmatter_key;
    required.required = "name";
    CHECK(det::match_clause(required, snap));

    required.required = "layout";
    CHECK_FALSE(det::match_clause(required, snap));
}

TEST_CASE("clause matcher covers frontmatter any-of keys",
          "[cli][import-detect][coverage][phase3-large]") {
    det::InputSnapshot snap;
    snap.has_frontmatter_fence = true;
    snap.frontmatter_keys = {"tokens", "spacing"};

    det::FingerprintClause c;
    c.kind = det::FingerprintClause::Kind::frontmatter_key;
    c.any_of = {"colors", "spacing"};
    CHECK(det::match_clause(c, snap));

    c.any_of = {"colors", "typography"};
    CHECK_FALSE(det::match_clause(c, snap));
}

TEST_CASE("detect enforces all-of formats before considering a match",
          "[cli][import-detect][coverage][phase3-large]") {
    det::ImportsManifest manifest;
    det::SourceEntry source;
    source.source = "design-md";
    source.parser_version = "1.0";

    det::FormatEntry strict;
    strict.format_version = "2026.05";
    strict.parser_version = source.parser_version;
    strict.match = "all-of";

    det::FingerprintClause filename;
    filename.kind = det::FingerprintClause::Kind::filename;
    filename.raw_kind = "filename";
    filename.regex = "(?i)^design\\.md$";
    strict.fingerprint.push_back(filename);

    det::FingerprintClause frontmatter;
    frontmatter.kind = det::FingerprintClause::Kind::frontmatter_key;
    frontmatter.raw_kind = "frontmatter-key";
    frontmatter.required = "name";
    strict.fingerprint.push_back(frontmatter);

    source.formats.push_back(strict);
    manifest.sources.push_back(source);

    det::InputSnapshot snap;
    snap.filename = "DESIGN.md";
    auto partial = det::detect(manifest, snap);
    CHECK(partial.ok);
    CHECK(partial.source.empty());

    snap.has_frontmatter_fence = true;
    snap.frontmatter_keys = {"name"};
    auto full = det::detect(manifest, snap);
    CHECK(full.source == "design-md");
    CHECK(full.matched_clauses == 2);
}

TEST_CASE("detect enforces minimum confidence thresholds",
          "[cli][import-detect][coverage][phase3-large]") {
    det::ImportsManifest manifest;
    det::SourceEntry source;
    source.source = "threshold";
    source.parser_version = "1.0";

    det::FormatEntry fmt;
    fmt.format_version = "2026.05";
    fmt.parser_version = source.parser_version;
    fmt.min_confidence_pct = 75;

    det::FingerprintClause a;
    a.kind = det::FingerprintClause::Kind::directory_files;
    a.raw_kind = "directory-files";
    a.files = {"code.html"};
    fmt.fingerprint.push_back(a);

    det::FingerprintClause b = a;
    b.files = {"DESIGN.md"};
    fmt.fingerprint.push_back(b);

    det::FingerprintClause c = a;
    c.files = {"screen.png"};
    fmt.fingerprint.push_back(c);

    det::FingerprintClause d = a;
    d.files = {"tokens.json"};
    fmt.fingerprint.push_back(d);

    source.formats.push_back(fmt);
    manifest.sources.push_back(source);

    det::InputSnapshot snap;
    snap.is_directory = true;
    snap.directory_basenames = {"code.html", "DESIGN.md"};
    CHECK(det::detect(manifest, snap).source.empty());

    snap.directory_basenames.push_back("screen.png");
    auto accepted = det::detect(manifest, snap);
    CHECK(accepted.source == "threshold");
    CHECK(accepted.confidence_pct == 75);
}

TEST_CASE("detect prefers higher confidence when match counts tie",
          "[cli][import-detect][coverage][phase3-large]") {
    det::ImportsManifest manifest;
    det::SourceEntry source;
    source.source = "tie";
    source.parser_version = "1.0";

    det::FingerprintClause code;
    code.kind = det::FingerprintClause::Kind::directory_files;
    code.raw_kind = "directory-files";
    code.files = {"code.html"};

    det::FormatEntry broad;
    broad.format_version = "broad";
    broad.parser_version = source.parser_version;
    broad.fingerprint = {code};
    broad.fingerprint.push_back(code);
    broad.fingerprint.back().files = {"missing-one"};

    det::FormatEntry narrow;
    narrow.format_version = "narrow";
    narrow.parser_version = source.parser_version;
    narrow.fingerprint = {code};

    source.formats = {broad, narrow};
    manifest.sources.push_back(source);

    det::InputSnapshot snap;
    snap.is_directory = true;
    snap.directory_basenames = {"code.html"};
    auto result = det::detect(manifest, snap);
    CHECK(result.format_version == "narrow");
    CHECK(result.confidence_pct == 100);
}

TEST_CASE("snapshot_input extracts Markdown frontmatter only from markdown files",
          "[cli][import-detect][coverage][phase3-large]") {
    auto dir = fs::temp_directory_path() / "pulp-import-detect-phase3-frontmatter";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto cleanup = [&]() { fs::remove_all(dir); };

    auto md = dir / "DESIGN.md";
    {
        std::ofstream f(md);
        f << "---\nname: Demo\ncolors:\n  primary: blue\n---\n# Body\n";
    }
    auto snap = det::snapshot_input(md);
    CHECK(snap.has_frontmatter_fence);
    CHECK(snap.frontmatter_keys == std::vector<std::string>{"name", "colors"});

    auto html = dir / "DESIGN.html";
    {
        std::ofstream f(html);
        f << "---\nname: Demo\n---\n";
    }
    auto html_snap = det::snapshot_input(html);
    CHECK_FALSE(html_snap.has_frontmatter_fence);
    cleanup();
}

TEST_CASE("snapshot_input accepts CRLF frontmatter and filters invalid top-level keys",
          "[cli][import-detect][coverage]") {
    auto dir = fs::temp_directory_path() / "pulp-import-detect-frontmatter-crlf";
    fs::remove_all(dir);
    fs::create_directories(dir);

    auto md = dir / "design.md";
    {
        std::ofstream f(md, std::ios::binary);
        f << "---\r\n"
          << "name: Demo\r\n"
          << "_private: ok\r\n"
          << "color-set: ok\r\n"
          << "2bad: ignored\r\n"
          << "nested:\r\n"
          << "  child: ignored\r\n"
          << "# comment: ignored\r\n"
          << "---   \r\n"
          << "body: not-frontmatter\r\n";
    }

    auto snap = det::snapshot_input(md);
    CHECK(snap.has_frontmatter_fence);
    CHECK(snap.frontmatter_keys == std::vector<std::string>{"name", "_private", "color-set", "nested"});

    det::FingerprintClause required;
    required.kind = det::FingerprintClause::Kind::frontmatter_key;
    required.required = "color-set";
    CHECK(det::match_clause(required, snap));

    required.required = "child";
    CHECK_FALSE(det::match_clause(required, snap));

    fs::remove_all(dir);
}

TEST_CASE("snapshot_input rejects incomplete frontmatter fences",
          "[cli][import-detect][coverage]") {
    auto dir = fs::temp_directory_path() / "pulp-import-detect-frontmatter-reject";
    fs::remove_all(dir);
    fs::create_directories(dir);

    auto no_newline = dir / "no-newline.md";
    {
        std::ofstream f(no_newline);
        f << "---name: Demo\n---\n";
    }
    auto no_newline_snap = det::snapshot_input(no_newline);
    CHECK_FALSE(no_newline_snap.has_frontmatter_fence);
    CHECK(no_newline_snap.frontmatter_keys.empty());

    auto no_close = dir / "no-close.md";
    {
        std::ofstream f(no_close);
        f << "---\nname: Demo\n";
    }
    auto no_close_snap = det::snapshot_input(no_close);
    CHECK_FALSE(no_close_snap.has_frontmatter_fence);
    CHECK(no_close_snap.frontmatter_keys.empty());

    fs::remove_all(dir);
}

TEST_CASE("snapshot_input prefers index html when code html is absent",
          "[cli][import-detect][coverage]") {
    auto dir = fs::temp_directory_path() / "pulp-import-detect-index-html";
    fs::remove_all(dir);
    fs::create_directories(dir);

    {
        std::ofstream f(dir / "z-last.html");
        f << "<script src=\"https://example.invalid/last.js\"></script>";
    }
    {
        std::ofstream f(dir / "index.html");
        f << "<script src=https://example.invalid/index.js></script>"
          << "<script type=module></script>";
    }

    auto snap = det::snapshot_input(dir);
    CHECK(snap.is_directory);
    REQUIRE(snap.script_srcs.size() == 1);
    CHECK(snap.script_srcs[0].find("index.js") != std::string::npos);
    REQUIRE(snap.script_types.size() == 1);
    CHECK(snap.script_types[0] == "module");

    fs::remove_all(dir);
}

TEST_CASE("snapshot_input scrapes script attributes without case or prefix traps",
          "[cli][import-detect][coverage]") {
    auto dir = fs::temp_directory_path() / "pulp-import-detect-script-attrs";
    fs::remove_all(dir);
    fs::create_directories(dir);

    {
        std::ofstream f(dir / "code.html");
        f << "<SCRIPT DATA-SRC=\"ignored.js\" SRC='https://cdn.example/app.js' "
          << "TYPE=\" Module \"></SCRIPT>\n"
          << "<script data-type=\"ignored/module\" type=__bundler/template></script>\n"
          << "<scripture src=\"not-a-script.js\"></scripture>\n"
          << "<script src=https://cdn.example/extra.js></script>\n";
    }

    auto snap = det::snapshot_input(dir);
    CHECK(snap.is_directory);
    CHECK_FALSE(snap.html_text.empty());
    CHECK(snap.directory_basenames == std::vector<std::string>{"code.html"});
    REQUIRE(snap.script_srcs.size() == 2);
    CHECK(snap.script_srcs[0] == "https://cdn.example/app.js");
    CHECK(snap.script_srcs[1] == "https://cdn.example/extra.js");
    REQUIRE(snap.script_types.size() == 2);
    CHECK(snap.script_types[0] == " Module ");
    CHECK(snap.script_types[1] == "__bundler/template");

    det::FingerprintClause src;
    src.kind = det::FingerprintClause::Kind::html_script_src;
    src.regex = R"(cdn\.example/app\.js)";
    CHECK(det::match_clause(src, snap));
    src.regex = "ignored\\.js";
    CHECK_FALSE(det::match_clause(src, snap));
    src.regex = "not-a-script";
    CHECK_FALSE(det::match_clause(src, snap));

    det::FingerprintClause type;
    type.kind = det::FingerprintClause::Kind::html_script_type;
    type.value = "module";
    CHECK(det::match_clause(type, snap));
    type.value = "__BUNDLER/TEMPLATE";
    CHECK(det::match_clause(type, snap));
    type.value = "ignored/module";
    CHECK_FALSE(det::match_clause(type, snap));

    fs::remove_all(dir);
}

TEST_CASE("snapshot_input ignores script-like tags and prefixed attributes",
          "[cli][import-detect][coverage]") {
    auto dir = fs::temp_directory_path() / "pulp-import-detect-script-boundaries";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "code.html");
        f << R"HTML(
<html>
  <description src="not-a-script.js"></description>
  <scriptish src="also-not-a-script.js"></scriptish>
  <script data-src="not-src.js" type="ignored-prefixed"></script>
  <script src=unquoted.js type=module></script>
  <SCRIPT SRC='quoted.js' TYPE='application/json'></SCRIPT>
  <script>
    window.tailwind = { theme: { extend: { colors: {
      "surface-container": "#111",
      "brand-primary": "#222"
    }}}};
  </script>
</html>
)HTML";
    }

    auto snap = det::snapshot_input(dir);

    REQUIRE(snap.is_directory);
    REQUIRE(snap.html_text.find("<scriptish") != std::string::npos);
    REQUIRE(snap.script_srcs.size() == 2);
    REQUIRE(snap.script_srcs[0] == "unquoted.js");
    REQUIRE(snap.script_srcs[1] == "quoted.js");
    REQUIRE(std::find(snap.script_srcs.begin(), snap.script_srcs.end(),
                      "not-a-script.js") == snap.script_srcs.end());
    REQUIRE(std::find(snap.script_srcs.begin(), snap.script_srcs.end(),
                      "also-not-a-script.js") == snap.script_srcs.end());
    REQUIRE(std::find(snap.script_srcs.begin(), snap.script_srcs.end(),
                      "not-src.js") == snap.script_srcs.end());
    REQUIRE(snap.script_types.size() == 3);
    REQUIRE(snap.script_types[0] == "ignored-prefixed");
    REQUIRE(snap.script_types[1] == "module");
    REQUIRE(snap.script_types[2] == "application/json");
    REQUIRE(std::find(snap.tailwind_tokens.begin(), snap.tailwind_tokens.end(),
                      "surface-container") != snap.tailwind_tokens.end());
    REQUIRE(std::find(snap.tailwind_tokens.begin(), snap.tailwind_tokens.end(),
                      "brand-primary") != snap.tailwind_tokens.end());

    fs::remove_all(dir);
}

TEST_CASE("snapshot_input accepts script tag and attribute whitespace boundaries",
          "[cli][import-detect][coverage][requested]") {
    auto dir = fs::temp_directory_path() / "pulp-import-detect-script-whitespace-boundaries";
    fs::remove_all(dir);
    fs::create_directories(dir);

    {
        std::ofstream f(dir / "code.html");
        f << "<script></script>\n"
          << "<script/>\n"
          << "<script\tsrc=\"tab.js\"></script>\n"
          << "<script\rsrc=\"cr.js\"></script>\n"
          << "<script\nsrc=\"nl.js\"></script>\n"
          << "<script data-src=\"ignored.js\" src=\"space.js\"></script>\n"
          << "<script data-type=\"ignored/module\" type=\"space/module\"></script>\n"
          << "<script\ttype=\"tab/module\"></script>\n"
          << "<script\rtype=\"cr/module\"></script>\n"
          << "<script\ntype=\"nl/module\"></script>\n"
          << "<scripture src=\"not-a-script.js\"></scripture>\n";
    }

    auto snap = det::snapshot_input(dir);
    REQUIRE(snap.script_srcs.size() == 4);
    CHECK(snap.script_srcs[0] == "tab.js");
    CHECK(snap.script_srcs[1] == "cr.js");
    CHECK(snap.script_srcs[2] == "nl.js");
    CHECK(snap.script_srcs[3] == "space.js");
    CHECK(std::find(snap.script_srcs.begin(), snap.script_srcs.end(), "ignored.js") ==
          snap.script_srcs.end());
    CHECK(std::find(snap.script_srcs.begin(), snap.script_srcs.end(), "not-a-script.js") ==
          snap.script_srcs.end());

    REQUIRE(snap.script_types.size() == 4);
    CHECK(snap.script_types[0] == "space/module");
    CHECK(snap.script_types[1] == "tab/module");
    CHECK(snap.script_types[2] == "cr/module");
    CHECK(snap.script_types[3] == "nl/module");
    CHECK(std::find(snap.script_types.begin(), snap.script_types.end(), "ignored/module") ==
          snap.script_types.end());

    fs::remove_all(dir);
}

TEST_CASE("snapshot_input falls back to sorted html candidates with attribute forms",
          "[cli][import-detect][coverage]") {
    auto dir = fs::temp_directory_path() / "pulp-import-detect-script-fallback";
    fs::remove_all(dir);
    fs::create_directories(dir);

    {
        std::ofstream f(dir / "b-last.HTML");
        f << "<script src=\"https://cdn.example/last.js\"></script>";
    }
    {
        std::ofstream f(dir / "a-first.htm");
        f << "<ScRiPt type='application/json' src=/first.js></ScRiPt>";
    }

    auto snap = det::snapshot_input(dir);
    CHECK(snap.is_directory);
    CHECK(snap.directory_basenames == std::vector<std::string>{"a-first.htm", "b-last.HTML"});
    REQUIRE(snap.script_srcs.size() == 1);
    CHECK(snap.script_srcs[0] == "/first.js");
    REQUIRE(snap.script_types.size() == 1);
    CHECK(snap.script_types[0] == "application/json");

    auto file_snap = det::snapshot_input(dir / "b-last.HTML");
    CHECK_FALSE(file_snap.is_directory);
    CHECK(file_snap.filename == "b-last.HTML");
    CHECK(file_snap.directory_basenames == std::vector<std::string>{"b-last.HTML"});
    REQUIRE(file_snap.script_srcs.size() == 1);
    CHECK(file_snap.script_srcs[0] == "https://cdn.example/last.js");
    CHECK(file_snap.script_types.empty());

    fs::remove_all(dir);
}

TEST_CASE("new-format reports cap unknown tailwind tokens and keep fallbacks stable",
          "[cli][import-detect][coverage]") {
    det::ImportsManifest manifest;
    det::SourceEntry source;
    source.source = "stitch";
    source.parser_version = "1.0";

    det::FormatEntry fmt;
    fmt.format_version = "2026.05";
    fmt.parser_version = source.parser_version;
    det::FingerprintClause token;
    token.kind = det::FingerprintClause::Kind::tailwind_config_token;
    token.any_of = {"known-token"};
    fmt.fingerprint.push_back(token);
    source.formats.push_back(fmt);
    manifest.sources.push_back(source);

    det::DetectionResult closest;
    closest.source = "stitch";
    closest.format_version = "2026.05";

    det::InputSnapshot snap;
    snap.tailwind_tokens.push_back("known-token");
    for (int i = 0; i < 25; ++i)
        snap.tailwind_tokens.push_back("unknown-" + std::to_string(i));

    auto report = det::build_new_format_report(manifest, snap, closest);
    CHECK(report.candidate_source == "stitch");
    CHECK(report.candidate_format_version == "2026.05+next");
    CHECK(report.based_on_source == "stitch");
    CHECK(report.based_on_format_version == "2026.05");
    REQUIRE(report.additions.size() == 20);
    CHECK(report.additions.front() == "unknown-0");
    CHECK(report.additions.back() == "unknown-19");
    CHECK(std::find(report.additions.begin(), report.additions.end(), "known-token") ==
          report.additions.end());

    auto json = det::render_new_format_json(report);
    CHECK(json.find("\"candidate-source\": \"stitch\"") != std::string::npos);
    CHECK(json.find("\"candidate-format-version\": \"2026.05+next\"") != std::string::npos);
    CHECK(json.find("unknown-19") != std::string::npos);
    CHECK(json.find("unknown-20") == std::string::npos);

    det::DetectionResult none;
    auto fallback = det::build_new_format_report(manifest, {}, none);
    CHECK(fallback.candidate_format_version == "TODO-set-version");
    CHECK(fallback.additions.empty());
}

TEST_CASE("render_new_format_json includes removals and empty additions",
          "[cli][import-detect][coverage]") {
    det::NewFormatReport report;
    report.candidate_source = "stitch";
    report.candidate_format_version = "2026.06";
    report.based_on_source = "stitch";
    report.based_on_format_version = "2026.05";
    report.removals = {"old-token", "legacy-script"};

    auto json = det::render_new_format_json(report);
    CHECK(json.find("\"candidate-source\": \"stitch\"") != std::string::npos);
    CHECK(json.find("\"candidate-format-version\": \"2026.06\"") != std::string::npos);
    CHECK(json.find("\"fingerprint-additions\": []") != std::string::npos);
    CHECK(json.find("\"fingerprint-removals\": [") != std::string::npos);
    CHECK(json.find("\"old-token\"") != std::string::npos);
    CHECK(json.find("\"legacy-script\"") != std::string::npos);
    CHECK(json.find("\"based-on\": {\"source\": \"stitch\", \"format-version\": \"2026.05\"}") !=
          std::string::npos);
}

TEST_CASE("detect preserves manifest order when matches and confidence tie",
          "[cli][import-detect][coverage]") {
    det::ImportsManifest manifest;

    det::FingerprintClause script;
    script.kind = det::FingerprintClause::Kind::html_script_type;
    script.raw_kind = "html-script-type";
    script.value = "module";

    det::SourceEntry first;
    first.source = "first";
    first.parser_version = "1.0";
    det::FormatEntry first_format;
    first_format.format_version = "a";
    first_format.parser_version = first.parser_version;
    first_format.fingerprint = {script};
    first.formats.push_back(first_format);

    det::SourceEntry second;
    second.source = "second";
    second.parser_version = "2.0";
    det::FormatEntry second_format;
    second_format.format_version = "b";
    second_format.parser_version = second.parser_version;
    second_format.fingerprint = {script};
    second.formats.push_back(second_format);

    manifest.sources = {first, second};

    det::InputSnapshot snap;
    snap.script_types = {" Module "};
    auto result = det::detect(manifest, snap);

    CHECK(result.ok);
    CHECK(result.source == "first");
    CHECK(result.format_version == "a");
    CHECK(result.parser_version == "1.0");
    CHECK(result.matched_clauses == 1);
    CHECK(result.total_clauses == 1);
    REQUIRE(result.matched_kinds.size() == 1);
    CHECK(result.matched_kinds[0] == "html-script-type");
}

TEST_CASE("detect reports matched and unmatched clause kinds for partial matches",
          "[cli][import-detect][coverage]") {
    det::ImportsManifest manifest;
    det::SourceEntry source;
    source.source = "diagnostic";
    source.parser_version = "1.0";

    det::FormatEntry format;
    format.format_version = "2026.05";
    format.parser_version = source.parser_version;

    det::FingerprintClause files;
    files.kind = det::FingerprintClause::Kind::directory_files;
    files.raw_kind = "directory-files";
    files.files = {"code.html"};
    format.fingerprint.push_back(files);

    det::FingerprintClause token;
    token.kind = det::FingerprintClause::Kind::tailwind_config_token;
    token.raw_kind = "tailwind-config-token";
    token.any_of = {"surface"};
    format.fingerprint.push_back(token);

    det::FingerprintClause filename;
    filename.kind = det::FingerprintClause::Kind::filename;
    filename.raw_kind = "filename";
    filename.regex = "(?i)^design\\.md$";
    format.fingerprint.push_back(filename);

    source.formats.push_back(format);
    manifest.sources.push_back(source);

    det::InputSnapshot snap;
    snap.is_directory = true;
    snap.directory_basenames = {"code.html"};
    snap.tailwind_tokens = {"surface"};

    auto result = det::detect(manifest, snap);
    CHECK(result.source == "diagnostic");
    CHECK(result.matched_clauses == 2);
    CHECK(result.total_clauses == 3);
    CHECK(result.confidence_pct == 66);
    CHECK(result.matched_kinds == std::vector<std::string>{"directory-files",
                                                           "tailwind-config-token"});
    CHECK(result.unmatched_kinds == std::vector<std::string>{"filename"});
}

TEST_CASE("snapshot_input prefers code html over sorted html fallbacks",
          "[cli][import-detect][coverage]") {
    auto dir = fs::temp_directory_path() / "pulp-import-detect-code-html-priority";
    fs::remove_all(dir);
    fs::create_directories(dir);

    {
        std::ofstream f(dir / "a-first.html");
        f << "<script src=\"first.js\"></script>";
    }
    {
        std::ofstream f(dir / "code.html");
        f << "<script src=\"code.js\"></script><script type=\"module\"></script>";
    }
    {
        std::ofstream f(dir / "index.html");
        f << "<script src=\"index.js\"></script>";
    }

    auto snap = det::snapshot_input(dir);
    CHECK(snap.is_directory);
    CHECK(snap.directory_basenames == std::vector<std::string>{"a-first.html",
                                                              "code.html",
                                                              "index.html"});
    REQUIRE(snap.script_srcs.size() == 1);
    CHECK(snap.script_srcs[0] == "code.js");
    REQUIRE(snap.script_types.size() == 1);
    CHECK(snap.script_types[0] == "module");

    fs::remove_all(dir);
}

TEST_CASE("new-format reports keep empty additions when no baseline tokens exist",
          "[cli][import-detect][coverage]") {
    det::ImportsManifest manifest;
    det::SourceEntry source;
    source.source = "claude";
    det::FormatEntry fmt;
    fmt.format_version = "2024.10";
    det::FingerprintClause script;
    script.kind = det::FingerprintClause::Kind::html_script_type;
    script.value = "__bundler/template";
    fmt.fingerprint.push_back(script);
    source.formats.push_back(fmt);
    manifest.sources.push_back(source);

    det::DetectionResult closest;
    closest.source = "claude";
    closest.format_version = "2024.10";

    det::InputSnapshot snap;
    snap.tailwind_tokens = {"unexpected-a", "unexpected-b"};

    auto report = det::build_new_format_report(manifest, snap, closest);
    CHECK(report.candidate_source == "claude");
    CHECK(report.candidate_format_version == "2024.10+next");
    CHECK(report.based_on_source == "claude");
    CHECK(report.based_on_format_version == "2024.10");
    CHECK(report.additions.empty());

    auto json = det::render_new_format_json(report);
    CHECK(json.find("\"fingerprint-additions\": []") != std::string::npos);
    CHECK(json.find("unexpected-a") == std::string::npos);
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
    // cannot change the expected detector result. Markdown fixtures
    // (e.g., DESIGN.md) have no HTML payload — fall through to a
    // canonical filename match (DESIGN.md or design.md) so the
    // detector's frontmatter probe runs against the expected file.
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
        if (!html_payloads.empty()) {
            scan_target = html_payloads.front();
        } else {
            for (auto candidate : {"DESIGN.md", "design.md"}) {
                auto p = fixture_dir / candidate;
                if (fs::exists(p)) { scan_target = p; break; }
            }
        }
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

TEST_CASE("report-new-format caps token suggestions at twenty entries",
          "[cli][import-detect][coverage][phase3-large]") {
    det::ImportsManifest manifest;
    det::SourceEntry source;
    source.source = "stitch";
    det::FormatEntry fmt;
    fmt.format_version = "2025.04";
    det::FingerprintClause known_tokens;
    known_tokens.kind = det::FingerprintClause::Kind::tailwind_config_token;
    known_tokens.any_of = {"known-token"};
    fmt.fingerprint.push_back(known_tokens);
    source.formats.push_back(fmt);
    manifest.sources.push_back(source);

    det::DetectionResult closest;
    closest.source = "stitch";
    closest.format_version = "2025.04";

    det::InputSnapshot snap;
    for (int i = 0; i < 25; ++i)
        snap.tailwind_tokens.push_back("token-" + std::to_string(i));

    auto report = det::build_new_format_report(manifest, snap, closest);
    CHECK(report.additions.size() == 20);
}
