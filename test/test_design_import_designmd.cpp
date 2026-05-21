// test_design_import_designmd.cpp — DESIGN.md import test suite
//
// Covers the Phase 1 acceptance criteria from
// planning/2026-05-13-designmd-integration-plan.md. Tag set:
// [view][import][designmd][issue-1434]

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/design_import.hpp>
#include <pulp/view/design_export.hpp>

#include "import_detect.hpp"

#include <stdexcept>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

using namespace pulp::view;

namespace {

#ifndef PULP_REPO_ROOT
#define PULP_REPO_ROOT "."
#endif

std::string read_fixture(const std::string& rel_path) {
    fs::path full = fs::path(PULP_REPO_ROOT) / rel_path;
    std::ifstream f(full);
    if (!f.is_open()) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string upstream_fixture() {
    return read_fixture("test/fixtures/imports/designmd/alpha/DESIGN.md");
}

std::string hand_authored_fixture() {
    return read_fixture("test/fixtures/imports/designmd/alpha/hand-authored.md");
}

bool has_diag_code(const std::vector<DesignMdDiagnostic>& diags, const std::string& code) {
    for (const auto& d : diags) if (d.code == code) return true;
    return false;
}

pulp::import_detect::ImportsManifest load_manifest() {
    auto text = read_fixture("compat.json");
    auto m = pulp::import_detect::parse_compat_json(text);
    REQUIRE(m.has_value());
    return *m;
}

}  // namespace

// (1) ── parses paws-and-paths frontmatter into populated tokens ────────
TEST_CASE("parse_designmd populates colors/typography/rounded/spacing/components on paws-and-paths",
          "[view][import][designmd][issue-1434]") {
    auto text = upstream_fixture();
    REQUIRE_FALSE(text.empty());

    auto result = parse_designmd(text);
    REQUIRE(result.had_frontmatter);
    REQUIRE(result.ir.source == DesignSource::designmd);

    // Paws & Paths defines well over 40 colors across surface/primary/etc.
    REQUIRE(result.ir.tokens.colors.size() > 40);
    // Mapped typography levels show up under typography.<level>.<field> string keys.
    bool has_typography = false;
    for (const auto& [k, _] : result.ir.tokens.strings) {
        if (k.rfind("typography.", 0) == 0) { has_typography = true; break; }
    }
    REQUIRE(has_typography);
    // Components are present.
    bool has_components = false;
    for (const auto& [k, _] : result.ir.tokens.strings) {
        if (k.rfind("components.", 0) == 0) { has_components = true; break; }
    }
    REQUIRE(has_components);
}

// (2) ── resolves nested color references in components section ────────
TEST_CASE("token references inside components resolve to primitive values",
          "[view][import][designmd][issue-1434]") {
    auto text = hand_authored_fixture();
    auto result = parse_designmd(text);
    auto it = result.ir.tokens.strings.find("components.button-primary.backgroundColor");
    REQUIRE(it != result.ir.tokens.strings.end());
    // {colors.primary} → "#1A1C1E" per the fixture
    REQUIRE(it->second == "#1A1C1E");
}

// (3) ── composite typography refs inside components are preserved ─────
TEST_CASE("composite typography refs inside components are preserved verbatim",
          "[view][import][designmd][issue-1434]") {
    auto text = hand_authored_fixture();
    auto result = parse_designmd(text);
    auto it = result.ir.tokens.strings.find("components.button-primary.typography");
    REQUIRE(it != result.ir.tokens.strings.end());
    // composite ref permitted only inside components; left as-is.
    REQUIRE(it->second == "{typography.body-md}");
}

// (4) ── group refs outside components emit a broken-ref diagnostic ────
TEST_CASE("non-component reference to a group emits broken-ref warning",
          "[view][import][designmd][issue-1434]") {
    // colors.accent → {colors.primary}: this is a valid in-group ref to a
    // primitive, which the parser resolves. The "group-ref" rejection
    // case is exercised when a value points at a bare group name
    // ("{colors}") — exercise that with a synthetic fixture.
    std::string yaml =
        "---\nname: Synthetic\ncolors:\n  primary: \"#000000\"\n  bad: \"{colors}\"\n---\n";
    auto result = parse_designmd(yaml);
    REQUIRE(has_diag_code(result.diagnostics, "broken-ref"));
}

// (5) ── DTCG export round-trip on paws-and-paths is non-empty + valid ─
TEST_CASE("export_w3c_tokens emits DTCG JSON from a parsed DESIGN.md",
          "[view][import][designmd][issue-1434]") {
    auto text = upstream_fixture();
    auto result = parse_designmd(text);
    auto theme = ir_tokens_to_theme(result.ir.tokens);
    auto dtcg = export_w3c_tokens(theme);
    REQUIRE_FALSE(dtcg.empty());
    // Surface a primary color marker — Paws & Paths sets primary to #855300.
    REQUIRE(dtcg.find("855300") != std::string::npos);
}

// (6) ── unknown section headings preserved without erroring ────────────
TEST_CASE("unknown section headings are preserved and do not error",
          "[view][import][designmd][issue-1434]") {
    auto text = hand_authored_fixture();
    auto result = parse_designmd(text);
    bool seen_iconography = false;
    for (const auto& s : result.sections) {
        if (s == "Iconography") { seen_iconography = true; break; }
    }
    REQUIRE(seen_iconography);
    // No error-severity diagnostic for the unknown section.
    for (const auto& d : result.diagnostics) {
        if (d.code == "unknown-section") {
            REQUIRE(d.severity != DesignMdSeverity::error);
        }
    }
}

// (7) ── duplicate section headings reject the file ────────────────────
TEST_CASE("duplicate ## section heading reports error-severity diagnostic",
          "[view][import][designmd][issue-1434]") {
    std::string text =
        "---\nname: Dup\ncolors:\n  primary: \"#000\"\n---\n"
        "## Colors\nFirst body.\n\n## Colors\nSecond body — duplicate!\n";
    auto result = parse_designmd(text);
    bool found = false;
    for (const auto& d : result.diagnostics) {
        if (d.code == "duplicate-section" && d.severity == DesignMdSeverity::error) {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

// (8) ── detector identifies a DESIGN.md as `designmd` source ─────────
TEST_CASE("detector recognizes upstream DESIGN.md fixture",
          "[view][import][designmd][issue-1434]") {
    auto manifest = load_manifest();
    auto snap = pulp::import_detect::snapshot_input(
        fs::path(PULP_REPO_ROOT) / "test/fixtures/imports/designmd/alpha/DESIGN.md");
    auto result = pulp::import_detect::detect(manifest, snap);
    REQUIRE(result.ok);
    REQUIRE(result.source == "designmd");
    REQUIRE(result.format_version == "alpha");
    REQUIRE(result.parser_version == "0.1");
}

// (9a) ── detector rejects Jekyll-style blog post ─────────────────────
TEST_CASE("detector rejects generic Markdown frontmatter (Jekyll decoy)",
          "[view][import][designmd][issue-1434]") {
    auto manifest = load_manifest();
    auto snap = pulp::import_detect::snapshot_input(
        fs::path(PULP_REPO_ROOT) / "test/fixtures/imports/designmd/alpha/decoys/jekyll/blog-post.md");
    auto result = pulp::import_detect::detect(manifest, snap);
    REQUIRE(result.source.empty());  // no match
}

// (9b) ── detector rejects DESIGN.md missing `name:` key ───────────────
TEST_CASE("detector rejects DESIGN.md without name: key (missing-name decoy)",
          "[view][import][designmd][issue-1434]") {
    auto manifest = load_manifest();
    auto snap = pulp::import_detect::snapshot_input(
        fs::path(PULP_REPO_ROOT) / "test/fixtures/imports/designmd/alpha/decoys/missing-name/DESIGN.md");
    auto result = pulp::import_detect::detect(manifest, snap);
    REQUIRE(result.source.empty());
}

// (9c) ── detector rejects DESIGN.md without any canonical token group ─
TEST_CASE("detector rejects DESIGN.md without token-group keys",
          "[view][import][designmd][issue-1434]") {
    auto manifest = load_manifest();
    auto snap = pulp::import_detect::snapshot_input(
        fs::path(PULP_REPO_ROOT) / "test/fixtures/imports/designmd/alpha/decoys/missing-token-groups/DESIGN.md");
    auto result = pulp::import_detect::detect(manifest, snap);
    REQUIRE(result.source.empty());
}

// (10) ── prose-only DESIGN.md emits empty tokens, no error ────────────
TEST_CASE("DESIGN.md with no frontmatter produces empty tokens + info diagnostic",
          "[view][import][designmd][issue-1434]") {
    std::string text = "# Brand Notes\n\nNo frontmatter; just prose.\n";
    auto result = parse_designmd(text);
    REQUIRE_FALSE(result.had_frontmatter);
    REQUIRE(result.ir.tokens.colors.empty());
    REQUIRE(result.ir.tokens.dimensions.empty());
    REQUIRE(has_diag_code(result.diagnostics, "no-frontmatter"));
}

TEST_CASE("parse_designmd mirrors YAML parse and shape errors into import diagnostics",
          "[view][import][designmd][issue-1434]") {
    SECTION("parser error") {
        auto result = parse_designmd(
            "---\n"
            "name: [unterminated\n"
            "---\n"
            "# Body\n");

        REQUIRE(result.had_frontmatter);
        REQUIRE(has_diag_code(result.diagnostics, "yaml-parse"));
        REQUIRE(result.diagnostics[0].severity == DesignMdSeverity::error);
        REQUIRE(result.ir.diagnostics.size() == result.diagnostics.size());
        REQUIRE(result.ir.diagnostics[0].severity == ImportDiagnosticSeverity::error);
        REQUIRE(result.ir.diagnostics[0].kind == ImportDiagnosticKind::unsupported_property);
        REQUIRE(result.ir.diagnostics[0].code == "yaml-parse");
        REQUIRE(result.ir.diagnostics[0].property == "<frontmatter>");
    }

    SECTION("non-map frontmatter") {
        auto result = parse_designmd(
            "---\n"
            "- not\n"
            "- a\n"
            "- map\n"
            "---\n"
            "# Body\n");

        REQUIRE(result.had_frontmatter);
        REQUIRE(has_diag_code(result.diagnostics, "yaml-shape"));
        REQUIRE(result.diagnostics[0].severity == DesignMdSeverity::error);
        REQUIRE(result.ir.diagnostics.size() == result.diagnostics.size());
        REQUIRE(result.ir.diagnostics[0].severity == ImportDiagnosticSeverity::error);
        REQUIRE(result.ir.diagnostics[0].code == "yaml-shape");
        REQUIRE(result.ir.diagnostics[0].property == "<frontmatter>");
    }
}

// (11) ── fontFeature / fontVariation preserved verbatim ───────────────
TEST_CASE("fontFeature and fontVariation typography fields survive parse",
          "[view][import][designmd][issue-1434]") {
    auto text = hand_authored_fixture();
    auto result = parse_designmd(text);
    auto feat = result.ir.tokens.strings.find("typography.body-md.fontFeature");
    auto vari = result.ir.tokens.strings.find("typography.body-md.fontVariation");
    REQUIRE(feat != result.ir.tokens.strings.end());
    REQUIRE(vari != result.ir.tokens.strings.end());
    REQUIRE(feat->second == "ss01, cv11");
    REQUIRE(vari->second == "wght 400, slnt 0");
}

// (12) ── parse_design_source(\"designmd\") wires the enum value ──────
TEST_CASE("parse_design_source maps \"designmd\" → DesignSource::designmd",
          "[view][import][designmd][issue-1434]") {
    auto src = parse_design_source("designmd");
    REQUIRE(src.has_value());
    REQUIRE(*src == DesignSource::designmd);
    REQUIRE(std::string(design_source_name(DesignSource::designmd)) == "DESIGN.md");
}

// ── Phase 2: lint, diff, Tailwind ──────────────────────────────────────

TEST_CASE("lint_designmd flags missing-primary when no primary color is defined",
          "[view][import][designmd][phase2][issue-1434]") {
    std::string text = "---\nname: NoPrimary\ncolors:\n  secondary: \"#888\"\n---\n";
    auto parsed = parse_designmd(text);
    auto findings = lint_designmd(parsed);
    bool found = false;
    for (const auto& d : findings) if (d.code == "missing-primary") { found = true; break; }
    REQUIRE(found);
}

TEST_CASE("lint_designmd flags missing-typography when colors but no typography",
          "[view][import][designmd][phase2][issue-1434]") {
    std::string text = "---\nname: ColorsOnly\ncolors:\n  primary: \"#000\"\n---\n";
    auto parsed = parse_designmd(text);
    auto findings = lint_designmd(parsed);
    bool found = false;
    for (const auto& d : findings) if (d.code == "missing-typography") { found = true; break; }
    REQUIRE(found);
}

TEST_CASE("lint_designmd promotes broken-ref to error severity",
          "[view][import][designmd][phase2][issue-1434]") {
    std::string text =
        "---\nname: Broken\ncolors:\n  primary: \"#000\"\n  bad: \"{colors.missing}\"\n---\n";
    auto parsed = parse_designmd(text);
    auto findings = lint_designmd(parsed);
    bool error_found = false;
    for (const auto& d : findings) {
        if (d.code == "broken-ref" && d.severity == DesignMdSeverity::error) {
            error_found = true;
            break;
        }
    }
    REQUIRE(error_found);
}

TEST_CASE("lint_designmd flags low-contrast component pairs",
          "[view][import][designmd][phase2][issue-1434]") {
    // Light gray text on white = ~2:1 contrast → below WCAG AA 4.5:1.
    std::string text =
        "---\nname: LowContrast\n"
        "colors:\n  primary: \"#000000\"\n"
        "components:\n  callout:\n    backgroundColor: \"#ffffff\"\n    textColor: \"#cccccc\"\n---\n";
    auto parsed = parse_designmd(text);
    auto findings = lint_designmd(parsed);
    bool found = false;
    for (const auto& d : findings) {
        if (d.code == "contrast-ratio") { found = true; break; }
    }
    REQUIRE(found);
}

TEST_CASE("lint_designmd flags orphaned color tokens",
          "[view][import][designmd][phase2][issue-1434]") {
    std::string text =
        "---\nname: Orphan\n"
        "colors:\n  primary: \"#000\"\n  unused-accent: \"#f0f\"\n"
        "components:\n  btn:\n    backgroundColor: \"{colors.primary}\"\n---\n";
    auto parsed = parse_designmd(text);
    auto findings = lint_designmd(parsed);
    bool found = false;
    for (const auto& d : findings) {
        if (d.code == "orphaned-tokens" && d.path == "colors.unused-accent") {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

// Regression: parse_designmd resolves `{colors.primary}` into a literal
// hex before lint runs, so the orphaned-tokens rule MUST consume the
// parse-recorded reference set (`referenced_color_tokens`) rather than
// re-scanning post-resolution strings. Without the fix, `primary` would
// be flagged as orphaned despite being used by every component.
// (Codex P1 review on PR #1934.)
TEST_CASE("lint_designmd does NOT flag referenced color as orphan after resolution",
          "[view][import][designmd][phase2][regression][issue-1434]") {
    std::string text =
        "---\nname: ResolvedRef\n"
        "colors:\n  primary: \"#1A1C1E\"\n"
        "components:\n  btn:\n    backgroundColor: \"{colors.primary}\"\n---\n";
    auto parsed = parse_designmd(text);
    // Post-resolution, the component's backgroundColor is the literal hex:
    REQUIRE(parsed.ir.tokens.strings["components.btn.backgroundColor"] == "#1A1C1E");
    // And the parse step has recorded `primary` as referenced:
    REQUIRE(std::find(parsed.referenced_color_tokens.begin(),
                      parsed.referenced_color_tokens.end(),
                      std::string("primary")) != parsed.referenced_color_tokens.end());
    // Lint MUST NOT flag `primary` as orphaned.
    auto findings = lint_designmd(parsed);
    for (const auto& d : findings) {
        if (d.code == "orphaned-tokens") {
            REQUIRE(d.path != "colors.primary");
        }
    }
}

TEST_CASE("lint_designmd emits token-summary info diagnostic",
          "[view][import][designmd][phase2][issue-1434]") {
    auto parsed = parse_designmd(upstream_fixture());
    auto findings = lint_designmd(parsed);
    bool found = false;
    for (const auto& d : findings) if (d.code == "token-summary") { found = true; break; }
    REQUIRE(found);
}

TEST_CASE("lint_designmd flags section-order when sections are out of canonical order",
          "[view][import][designmd][phase2][issue-1434]") {
    std::string text =
        "---\nname: BadOrder\ncolors:\n  primary: \"#000\"\n---\n\n"
        "## Components\nthen\n\n## Colors\nlater\n";
    auto parsed = parse_designmd(text);
    auto findings = lint_designmd(parsed);
    bool found = false;
    for (const auto& d : findings) if (d.code == "section-order") { found = true; break; }
    REQUIRE(found);
}

TEST_CASE("diff_designmd reports added, removed, and modified color tokens",
          "[view][import][designmd][phase2][issue-1434]") {
    auto before = parse_designmd(
        "---\nname: V1\ncolors:\n  primary: \"#000\"\n  removed: \"#fff\"\n---\n");
    auto after = parse_designmd(
        "---\nname: V2\ncolors:\n  primary: \"#111\"\n  added: \"#abc\"\n---\n");
    auto diff = diff_designmd(before, after);
    REQUIRE(diff.colors.added.size() == 1);
    REQUIRE(diff.colors.added.front() == "added");
    REQUIRE(diff.colors.removed.size() == 1);
    REQUIRE(diff.colors.removed.front() == "removed");
    REQUIRE(diff.colors.modified.size() == 1);
    REQUIRE(diff.colors.modified.front() == "primary");
}

TEST_CASE("diff_designmd reports regression when after has more lint findings",
          "[view][import][designmd][phase2][issue-1434]") {
    auto before = parse_designmd(
        "---\nname: Clean\ncolors:\n  primary: \"#000\"\ntypography:\n  body-md:\n    fontFamily: Inter\n---\n");
    auto after = parse_designmd(
        "---\nname: Regressed\ncolors:\n  primary: \"#000\"\n  bad: \"{colors.missing}\"\n---\n");
    auto diff = diff_designmd(before, after);
    REQUIRE(diff.regression);
}

TEST_CASE("export_tailwind_v3_json emits theme.extend-shaped JSON",
          "[view][import][designmd][phase2][issue-1434]") {
    auto parsed = parse_designmd(
        "---\nname: TW3\ncolors:\n  primary: \"#1A1C1E\"\nrounded:\n  sm: 4px\nspacing:\n  md: 16px\n---\n");
    auto json = export_tailwind_v3_json(parsed);
    REQUIRE(json.find("\"primary\": \"#1A1C1E\"") != std::string::npos);
    REQUIRE(json.find("\"borderRadius\"") != std::string::npos);
    REQUIRE(json.find("\"sm\": \"4px\"") != std::string::npos);
    REQUIRE(json.find("\"spacing\"") != std::string::npos);
}

TEST_CASE("export_tailwind_v4_css emits @theme block with --color/--radius/--spacing vars",
          "[view][import][designmd][phase2][issue-1434]") {
    auto parsed = parse_designmd(
        "---\nname: TW4\ncolors:\n  primary: \"#1A1C1E\"\nrounded:\n  md: 8px\nspacing:\n  lg: 24px\n---\n");
    auto css = export_tailwind_v4_css(parsed);
    REQUIRE(css.find("@theme {") != std::string::npos);
    REQUIRE(css.find("--color-primary: #1A1C1E") != std::string::npos);
    REQUIRE(css.find("--radius-md: 8px") != std::string::npos);
    REQUIRE(css.find("--spacing-lg: 24px") != std::string::npos);
}

// Regression: Tailwind v3 + v4 export MUST include typography mappings
// (fontFamily, fontSize, fontWeight, lineHeight, letterSpacing).
// Without the fix these tokens are silently dropped from the output.
// (Codex P2 review on PR #1934.)
TEST_CASE("export_tailwind_v3_json includes typography fontFamily/fontSize/fontWeight/etc",
          "[view][import][designmd][phase2][regression][issue-1434]") {
    auto parsed = parse_designmd(
        "---\nname: TW3Typography\n"
        "colors:\n  primary: \"#000\"\n"
        "typography:\n"
        "  h1:\n    fontFamily: \"Public Sans\"\n    fontSize: 48px\n    fontWeight: 600\n"
        "    lineHeight: 1.1\n    letterSpacing: -0.02em\n"
        "  body-md:\n    fontFamily: \"Public Sans\"\n    fontSize: 16px\n---\n");
    auto json = export_tailwind_v3_json(parsed);
    REQUIRE(json.find("\"fontFamily\"") != std::string::npos);
    REQUIRE(json.find("\"h1\": \"Public Sans\"") != std::string::npos);
    REQUIRE(json.find("\"fontSize\"") != std::string::npos);
    REQUIRE(json.find("\"h1\": \"48px\"") != std::string::npos);
    REQUIRE(json.find("\"fontWeight\"") != std::string::npos);
    REQUIRE(json.find("\"h1\": \"600\"") != std::string::npos);
    REQUIRE(json.find("\"lineHeight\"") != std::string::npos);
    REQUIRE(json.find("\"letterSpacing\"") != std::string::npos);
}

TEST_CASE("export_tailwind_v4_css includes --font / --text / --leading / --tracking / --font-weight vars",
          "[view][import][designmd][phase2][regression][issue-1434]") {
    auto parsed = parse_designmd(
        "---\nname: TW4Typography\n"
        "colors:\n  primary: \"#000\"\n"
        "typography:\n"
        "  h1:\n    fontFamily: \"Public Sans\"\n    fontSize: 48px\n    fontWeight: 600\n"
        "    lineHeight: 1.1\n    letterSpacing: -0.02em\n---\n");
    auto css = export_tailwind_v4_css(parsed);
    REQUIRE(css.find("--font-h1: Public Sans") != std::string::npos);
    REQUIRE(css.find("--text-h1: 48px") != std::string::npos);
    REQUIRE(css.find("--leading-h1: 1.1") != std::string::npos);
    REQUIRE(css.find("--tracking-h1: -0.02em") != std::string::npos);
    REQUIRE(css.find("--font-weight-h1: 600") != std::string::npos);
}

// ── Phase 3: signature is fixed, body gated on pulp #1307 ─────────────

TEST_CASE("export_designmd throws std::logic_error until pulp #1307 lands",
          "[view][import][designmd][phase3][gated-1307][issue-1434]") {
    Theme t;
    DesignMdProseHints hints;
    REQUIRE_THROWS_AS(export_designmd(t, hints), std::logic_error);
}
