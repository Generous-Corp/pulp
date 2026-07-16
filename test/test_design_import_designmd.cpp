// test_design_import_designmd.cpp — DESIGN.md import test suite
//
// Covers DESIGN.md import parsing, linting, diff, and export-gated behavior.
// Tag set: [view][import][designmd]

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
          "[view][import][designmd][parse]") {
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
          "[view][import][designmd][parse]") {
    auto text = hand_authored_fixture();
    auto result = parse_designmd(text);
    auto it = result.ir.tokens.strings.find("components.button-primary.backgroundColor");
    REQUIRE(it != result.ir.tokens.strings.end());
    // {colors.primary} → "#1A1C1E" per the fixture
    REQUIRE(it->second == "#1A1C1E");
}

// (3) ── composite typography refs inside components are preserved ─────
TEST_CASE("composite typography refs inside components are preserved verbatim",
          "[view][import][designmd][parse]") {
    auto text = hand_authored_fixture();
    auto result = parse_designmd(text);
    auto it = result.ir.tokens.strings.find("components.button-primary.typography");
    REQUIRE(it != result.ir.tokens.strings.end());
    // composite ref permitted only inside components; left as-is.
    REQUIRE(it->second == "{typography.body-md}");
}

// (4) ── group refs outside components emit a broken-ref diagnostic ────
TEST_CASE("non-component reference to a group emits broken-ref warning",
          "[view][import][designmd][parse]") {
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
          "[view][import][designmd][parse]") {
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
          "[view][import][designmd][parse]") {
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
          "[view][import][designmd][parse]") {
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
          "[view][import][designmd][parse]") {
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
          "[view][import][designmd][parse]") {
    auto manifest = load_manifest();
    auto snap = pulp::import_detect::snapshot_input(
        fs::path(PULP_REPO_ROOT) / "test/fixtures/imports/designmd/alpha/decoys/jekyll/blog-post.md");
    auto result = pulp::import_detect::detect(manifest, snap);
    REQUIRE(result.source.empty());  // no match
}

// (9b) ── detector rejects DESIGN.md missing `name:` key ───────────────
TEST_CASE("detector rejects DESIGN.md without name: key (missing-name decoy)",
          "[view][import][designmd][parse]") {
    auto manifest = load_manifest();
    auto snap = pulp::import_detect::snapshot_input(
        fs::path(PULP_REPO_ROOT) / "test/fixtures/imports/designmd/alpha/decoys/missing-name/DESIGN.md");
    auto result = pulp::import_detect::detect(manifest, snap);
    REQUIRE(result.source.empty());
}

// (9c) ── detector rejects DESIGN.md without any canonical token group ─
TEST_CASE("detector rejects DESIGN.md without token-group keys",
          "[view][import][designmd][parse]") {
    auto manifest = load_manifest();
    auto snap = pulp::import_detect::snapshot_input(
        fs::path(PULP_REPO_ROOT) / "test/fixtures/imports/designmd/alpha/decoys/missing-token-groups/DESIGN.md");
    auto result = pulp::import_detect::detect(manifest, snap);
    REQUIRE(result.source.empty());
}

// (10) ── prose-only DESIGN.md emits empty tokens, no error ────────────
TEST_CASE("DESIGN.md with no frontmatter produces empty tokens + info diagnostic",
          "[view][import][designmd][parse]") {
    std::string text = "# Brand Notes\n\nNo frontmatter; just prose.\n";
    auto result = parse_designmd(text);
    REQUIRE_FALSE(result.had_frontmatter);
    REQUIRE(result.ir.tokens.colors.empty());
    REQUIRE(result.ir.tokens.dimensions.empty());
    REQUIRE(has_diag_code(result.diagnostics, "no-frontmatter"));
}

TEST_CASE("parse_designmd mirrors YAML parse and shape errors into import diagnostics",
          "[view][import][designmd][parse]") {
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
          "[view][import][designmd][parse]") {
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
          "[view][import][designmd][parse]") {
    auto src = parse_design_source("designmd");
    REQUIRE(src.has_value());
    REQUIRE(*src == DesignSource::designmd);
    REQUIRE(std::string(design_source_name(DesignSource::designmd)) == "DESIGN.md");
}

// ── Lint, diff, Tailwind ───────────────────────────────────────────────

TEST_CASE("lint_designmd flags missing-primary when no primary color is defined",
          "[view][import][designmd][lint][colors]") {
    std::string text = "---\nname: NoPrimary\ncolors:\n  secondary: \"#888\"\n---\n";
    auto parsed = parse_designmd(text);
    auto findings = lint_designmd(parsed);
    bool found = false;
    for (const auto& d : findings) if (d.code == "missing-primary") { found = true; break; }
    REQUIRE(found);
}

TEST_CASE("lint_designmd flags missing-typography when colors but no typography",
          "[view][import][designmd][lint][typography]") {
    std::string text = "---\nname: ColorsOnly\ncolors:\n  primary: \"#000\"\n---\n";
    auto parsed = parse_designmd(text);
    auto findings = lint_designmd(parsed);
    bool found = false;
    for (const auto& d : findings) if (d.code == "missing-typography") { found = true; break; }
    REQUIRE(found);
}

TEST_CASE("lint_designmd promotes broken-ref to error severity",
          "[view][import][designmd][lint][references]") {
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
          "[view][import][designmd][lint][contrast]") {
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
          "[view][import][designmd][lint][tokens]") {
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
TEST_CASE("lint_designmd does NOT flag referenced color as orphan after resolution",
          "[view][import][designmd][lint][tokens][regression]") {
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
          "[view][import][designmd][lint][summary]") {
    auto parsed = parse_designmd(upstream_fixture());
    auto findings = lint_designmd(parsed);
    bool found = false;
    for (const auto& d : findings) if (d.code == "token-summary") { found = true; break; }
    REQUIRE(found);
}

TEST_CASE("lint_designmd flags section-order when sections are out of canonical order",
          "[view][import][designmd][lint][sections]") {
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
          "[view][import][designmd][diff][colors]") {
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
          "[view][import][designmd][diff][lint]") {
    auto before = parse_designmd(
        "---\nname: Clean\ncolors:\n  primary: \"#000\"\ntypography:\n  body-md:\n    fontFamily: Inter\n---\n");
    auto after = parse_designmd(
        "---\nname: Regressed\ncolors:\n  primary: \"#000\"\n  bad: \"{colors.missing}\"\n---\n");
    auto diff = diff_designmd(before, after);
    REQUIRE(diff.regression);
}

TEST_CASE("export_tailwind_v3_json emits theme.extend-shaped JSON",
          "[view][import][designmd][tailwind][v3]") {
    auto parsed = parse_designmd(
        "---\nname: TW3\ncolors:\n  primary: \"#1A1C1E\"\nrounded:\n  sm: 4px\nspacing:\n  md: 16px\n---\n");
    auto json = export_tailwind_v3_json(parsed);
    REQUIRE(json.find("\"primary\": \"#1A1C1E\"") != std::string::npos);
    REQUIRE(json.find("\"borderRadius\"") != std::string::npos);
    REQUIRE(json.find("\"sm\": \"4px\"") != std::string::npos);
    REQUIRE(json.find("\"spacing\"") != std::string::npos);
}

TEST_CASE("export_tailwind_v4_css emits @theme block with --color/--radius/--spacing vars",
          "[view][import][designmd][tailwind][v4]") {
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
TEST_CASE("export_tailwind_v3_json includes typography fontFamily/fontSize/fontWeight/etc",
          "[view][import][designmd][tailwind][v3][typography][regression]") {
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
          "[view][import][designmd][tailwind][v4][typography][regression]") {
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

// ── DESIGN.md export remains gated until native export is implemented ───

TEST_CASE("export_designmd throws std::logic_error while export is gated",
          "[view][import][designmd][export-gated]") {
    Theme t;
    DesignMdProseHints hints;
    REQUIRE_THROWS_AS(export_designmd(t, hints), std::logic_error);
}

// ── Frontmatter-less body-section token recovery ─────────────────────────
// Stitch/Brand-Kit DESIGN.md files are often authored as Markdown body prose
// with no YAML frontmatter; recover their tokens from the body sections so
// they don't import as an empty token set. Dark-mode colors use the same
// "<name>.dark" suffix convention as the Figma multi-mode token capture.

TEST_CASE("frontmatter-less DESIGN.md recovers list-form tokens incl. dark mode",
          "[view][import][designmd][body]") {
    const std::string md = R"md(# My Brand

## Colors

### Light Mode
- Primary: #6750A4
- On Surface: #1A1C1E

### Dark Mode
- Primary: #D0BCFF
- On Surface: #E3E2E6

## Spacing
- sm: 4px
- md: 8px

## Border Radius
- sm: 4px
- lg: 16px

## Shadows
- card: 0 1px 3px rgba(0,0,0,0.2)
)md";
    auto result = parse_designmd(md);
    REQUIRE_FALSE(result.had_frontmatter);
    // Light (default) → bare name; Dark → ".dark" suffix.
    REQUIRE(result.ir.tokens.colors.at("primary") == "#6750A4");
    REQUIRE(result.ir.tokens.colors.at("primary.dark") == "#D0BCFF");
    REQUIRE(result.ir.tokens.colors.at("on-surface") == "#1A1C1E");
    REQUIRE(result.ir.tokens.colors.at("on-surface.dark") == "#E3E2E6");
    // Spacing + radius → px dimensions under the same prefixes as frontmatter.
    REQUIRE(result.ir.tokens.dimensions.at("spacing-sm") == 4.0f);
    REQUIRE(result.ir.tokens.dimensions.at("spacing-md") == 8.0f);
    REQUIRE(result.ir.tokens.dimensions.at("rounded-sm") == 4.0f);
    REQUIRE(result.ir.tokens.dimensions.at("rounded-lg") == 16.0f);
    // Shadow → string token.
    REQUIRE(result.ir.tokens.strings.at("shadow-card").find("rgba") != std::string::npos);
    // Diagnostic reports recovery, not the empty-set message.
    REQUIRE(has_diag_code(result.diagnostics, "body-tokens"));
    REQUIRE_FALSE(has_diag_code(result.diagnostics, "no-frontmatter"));
}

TEST_CASE("frontmatter-less DESIGN.md parses table-form colors and skips the header row",
          "[view][import][designmd][body]") {
    const std::string md = R"md(# Brand

## Colors

| Token | Value |
| --- | --- |
| Primary | #112233 |
| Secondary | #445566 |
)md";
    auto result = parse_designmd(md);
    REQUIRE_FALSE(result.had_frontmatter);
    REQUIRE(result.ir.tokens.colors.at("primary") == "#112233");
    REQUIRE(result.ir.tokens.colors.at("secondary") == "#445566");
    // The "| Token | Value |" header row must NOT become a token (its value
    // cell isn't a color), and the "| --- | --- |" separator is skipped.
    REQUIRE(result.ir.tokens.colors.find("token") == result.ir.tokens.colors.end());
}

TEST_CASE("frontmatter-less DESIGN.md with no token sections still emits an empty set",
          "[view][import][designmd][body]") {
    const std::string md = "# Title\n\n## Overview\n\nSome prose, no tokens here.\n";
    auto result = parse_designmd(md);
    REQUIRE_FALSE(result.had_frontmatter);
    REQUIRE(result.ir.tokens.colors.empty());
    REQUIRE(has_diag_code(result.diagnostics, "no-frontmatter"));
    REQUIRE_FALSE(has_diag_code(result.diagnostics, "body-tokens"));
}

TEST_CASE("frontmatter-less DESIGN.md handles emphasis, slashes, rgb(), refs, and * bullets",
          "[view][import][designmd][body]") {
    const std::string md = R"md(# Brand

## Colors
* **Brand/Primary**: #abcdef
* Accent: rgba(10, 20, 30, 0.5)
* Link: {colors.brand-primary}
* Note this line has no value

## Spacing
- weird: not-a-size
)md";
    auto result = parse_designmd(md);
    REQUIRE_FALSE(result.had_frontmatter);
    // `*` bullet + `**emphasis**` stripped + `/` normalized to `-`.
    REQUIRE(result.ir.tokens.colors.at("brand-primary") == "#abcdef");
    // rgb()/rgba() and {token.reference} values are accepted verbatim.
    REQUIRE(result.ir.tokens.colors.at("accent").rfind("rgba", 0) == 0);
    REQUIRE(result.ir.tokens.colors.at("link") == "{colors.brand-primary}");
    // A list item with no ':' is not a token line.
    REQUIRE(result.ir.tokens.colors.find("note-this-line-has-no-value") == result.ir.tokens.colors.end());
    // A non-dimension spacing value is skipped, not stored.
    REQUIRE(result.ir.tokens.dimensions.find("spacing-weird") == result.ir.tokens.dimensions.end());
}

// ── Shadows: frontmatter group + additive body scan ──────────────────────
// Three cases pinned together, because the bug they cover was the gap BETWEEN
// them: a body `## Shadows` table was read only when the document had no
// frontmatter, and the frontmatter schema had no shadow key at all. Adding
// frontmatter to a prose DESIGN.md therefore deleted the only slot shadows
// had, silently. (A) and (C) are the two ways to author a shadow; (B) is the
// control that proves the body scanner itself works.

// (A) frontmatter present + body `## Shadows` table → body fills the gap.
TEST_CASE("DESIGN.md with frontmatter still recovers shadow tokens from a body section",
          "[view][import][designmd][body][shadows]") {
    const std::string md = R"md(---
name: Gap Filler
colors:
  primary: "#6750A4"
---

# Gap Filler

## Shadows

| Token | Value |
| --- | --- |
| Card | 0 1px 3px rgba(0,0,0,0.2) |
| Modal | 0 8px 24px rgba(0,0,0,0.4) |
)md";
    auto result = parse_designmd(md);
    REQUIRE(result.had_frontmatter);
    // Frontmatter parsed as usual.
    REQUIRE(result.ir.tokens.colors.at("primary") == "#6750A4");
    // ...and the body section is no longer dropped on the floor.
    REQUIRE(result.ir.tokens.strings.at("shadow-card") == "0 1px 3px rgba(0,0,0,0.2)");
    REQUIRE(result.ir.tokens.strings.at("shadow-modal") == "0 8px 24px rgba(0,0,0,0.4)");
    // The recovery is reported rather than happening silently.
    REQUIRE(has_diag_code(result.diagnostics, "body-tokens"));
}

// (A′) frontmatter wins on conflict; the body only fills what it never said.
TEST_CASE("DESIGN.md frontmatter values win over body values for the same token",
          "[view][import][designmd][body][shadows]") {
    const std::string md = R"md(---
name: Conflict
colors:
  primary: "#111111"
spacing:
  md: 8px
---

## Colors
- Primary: #999999
- Accent: #222222

## Spacing
- md: 999px
- lg: 16px
)md";
    auto result = parse_designmd(md);
    REQUIRE(result.had_frontmatter);
    // Conflicts resolve to the frontmatter value.
    REQUIRE(result.ir.tokens.colors.at("primary") == "#111111");
    REQUIRE(result.ir.tokens.dimensions.at("spacing-md") == 8.0f);
    // Gaps are filled from the body.
    REQUIRE(result.ir.tokens.colors.at("accent") == "#222222");
    REQUIRE(result.ir.tokens.dimensions.at("spacing-lg") == 16.0f);
}

// (C) a `shadows:` frontmatter group is a real slot, not an unknown key.
TEST_CASE("DESIGN.md parses a shadows frontmatter group into shadow-* string tokens",
          "[view][import][designmd][parse][shadows]") {
    const std::string yaml =
        "---\n"
        "name: Shadow Group\n"
        "shadows:\n"
        "  sm: \"0 1px 2px rgba(0,0,0,0.1)\"\n"
        "  lg: \"0 8px 24px rgba(0,0,0,0.4)\"\n"
        "  card:\n"
        "    rest: \"0 1px 3px rgba(0,0,0,0.2)\"\n"
        "---\n";
    auto result = parse_designmd(yaml);
    REQUIRE(result.had_frontmatter);
    // Same slot + prefix the body scanner uses, so the two forms agree.
    REQUIRE(result.ir.tokens.strings.at("shadow-sm") == "0 1px 2px rgba(0,0,0,0.1)");
    REQUIRE(result.ir.tokens.strings.at("shadow-lg") == "0 8px 24px rgba(0,0,0,0.4)");
    // Nested levels dot-join, matching rounded/spacing.
    REQUIRE(result.ir.tokens.strings.at("shadow-card.rest") == "0 1px 3px rgba(0,0,0,0.2)");
    // `shadows` is schema, so it must NOT be flagged as an unknown top-level key.
    for (const auto& d : result.diagnostics)
        REQUIRE_FALSE(d.code == "unknown-key");
}

// Prose under a shadow heading is not a shadow token. This guards the additive
// scan above: real DESIGN.md files put descriptive sentences under
// `## Elevation & Depth`, and those sentences quote sizes ("Blur: 20px-40px"),
// so a digit-anywhere test would import them as tokens.
TEST_CASE("DESIGN.md does not mistake prose under an elevation heading for a shadow token",
          "[view][import][designmd][body][shadows]") {
    const std::string md = R"md(---
name: Prose Body
---

## Elevation & Depth

- **Shadows:** Shadows are highly diffused and soft (Blur: 20px-40px, Opacity: 4-8%).
- **Interactions:** Elements lift on hover, increasing shadow spread for feedback.
- card: 0 1px 3px rgba(0,0,0,0.2)
)md";
    auto result = parse_designmd(md);
    // The sentences are skipped...
    REQUIRE(result.ir.tokens.strings.find("shadow-shadows") == result.ir.tokens.strings.end());
    REQUIRE(result.ir.tokens.strings.find("shadow-interactions") == result.ir.tokens.strings.end());
    // ...while a real shadow value in the same section still imports.
    REQUIRE(result.ir.tokens.strings.at("shadow-card") == "0 1px 3px rgba(0,0,0,0.2)");
}

// ── DESIGN.md 0.3.0 format coverage ─────────────────────────────────────
// The format spec was bumped 0.1.1 → 0.3.0. The changes relevant to Pulp's
// consumer (importer) side are: any-valid-CSS-color values, arbitrary-depth
// nested token declarations, bare-number spacing, and an unknown-top-level-key
// warning. Each is covered below in the structured-frontmatter path.

// (0.3.0) ── frontmatter colors accept any valid CSS color, no spurious warn ─
TEST_CASE("frontmatter colors accept named/functional CSS colors without a color-shape warning",
          "[view][import][designmd][parse][designmd030]") {
    const std::string yaml =
        "---\n"
        "name: CSS Colors\n"
        "colors:\n"
        "  a-hex6: \"#1A2B3C\"\n"
        "  a-hex8: \"#1A2B3C80\"\n"
        "  a-hex4: \"#abcd\"\n"
        "  a-named: cornflowerblue\n"
        "  a-transparent: transparent\n"
        "  a-rgb: \"rgb(10, 20, 30)\"\n"
        "  a-rgba: \"rgba(10, 20, 30, 0.5)\"\n"
        "  a-hsl: \"hsl(200 50% 40%)\"\n"
        "  a-oklch: \"oklch(0.7 0.1 200)\"\n"
        "  a-colormix: \"color-mix(in srgb, red 40%, blue)\"\n"
        "---\n";
    auto result = parse_designmd(yaml);
    // Every value above is a valid CSS color → no color-shape diagnostic at all.
    REQUIRE_FALSE(has_diag_code(result.diagnostics, "color-shape"));
    // Values are preserved verbatim (the spec keeps the original format).
    REQUIRE(result.ir.tokens.colors.at("a-named") == "cornflowerblue");
    REQUIRE(result.ir.tokens.colors.at("a-oklch") == "oklch(0.7 0.1 200)");
    REQUIRE(result.ir.tokens.colors.at("a-hex8") == "#1A2B3C80");
    REQUIRE(result.ir.tokens.colors.at("a-hex4") == "#abcd");
}

// (0.3.0) ── a genuinely invalid color value still warns ────────────────
TEST_CASE("frontmatter color that is not a CSS color or reference still warns",
          "[view][import][designmd][parse][designmd030]") {
    const std::string yaml =
        "---\nname: Bad\ncolors:\n  primary: \"#000000\"\n  oops: not-a-color\n---\n";
    auto result = parse_designmd(yaml);
    REQUIRE(has_diag_code(result.diagnostics, "color-shape"));
    // It is still stored (we never silently drop a token).
    REQUIRE(result.ir.tokens.colors.at("oops") == "not-a-color");
}

// (0.3.0) ── arbitrary-depth nested color tokens resolve via dot paths ───
TEST_CASE("nested color declarations emit dot-path tokens and resolve as references",
          "[view][import][designmd][parse][designmd030]") {
    const std::string yaml =
        "---\n"
        "name: Nested\n"
        "colors:\n"
        "  background:\n"
        "    light: \"#ffffff\"\n"
        "    dark: \"#000000\"\n"
        "  brand:\n"
        "    accent:\n"
        "      strong: \"#ff0066\"\n"
        "components:\n"
        "  panel:\n"
        "    backgroundColor: \"{colors.background.light}\"\n"
        "    accent: \"{colors.brand.accent.strong}\"\n"
        "---\n";
    auto result = parse_designmd(yaml);
    // Nested tokens are keyed by their dot-joined path.
    REQUIRE(result.ir.tokens.colors.at("background.light") == "#ffffff");
    REQUIRE(result.ir.tokens.colors.at("background.dark") == "#000000");
    REQUIRE(result.ir.tokens.colors.at("brand.accent.strong") == "#ff0066");
    // And a {colors.a.b.c} reference resolves to the nested primitive.
    REQUIRE(result.ir.tokens.strings.at("components.panel.backgroundColor") == "#ffffff");
    REQUIRE(result.ir.tokens.strings.at("components.panel.accent") == "#ff0066");
    REQUIRE_FALSE(has_diag_code(result.diagnostics, "broken-ref"));
}

// (0.3.0) ── nested + bare-number spacing/rounded resolve as dimensions ──
TEST_CASE("nested and unitless dimension tokens resolve in rounded/spacing",
          "[view][import][designmd][parse][designmd030]") {
    const std::string yaml =
        "---\n"
        "name: Dims\n"
        "spacing:\n"
        "  base: 8\n"            // bare number per spec → 8px
        "  scale:\n"
        "    lg: 40px\n"
        "rounded:\n"
        "  pill: 9999px\n"
        "components:\n"
        "  card:\n"
        "    padding: \"{spacing.scale.lg}\"\n"
        "    gap: \"{spacing.base}\"\n"
        "---\n";
    auto result = parse_designmd(yaml);
    REQUIRE(result.ir.tokens.dimensions.at("spacing-base") == 8.0f);
    REQUIRE(result.ir.tokens.dimensions.at("spacing-scale.lg") == 40.0f);
    REQUIRE(result.ir.tokens.dimensions.at("rounded-pill") == 9999.0f);
    // Nested + bare-number references both resolve to "<n>px" strings.
    REQUIRE(result.ir.tokens.strings.at("components.card.padding") == "40px");
    REQUIRE(result.ir.tokens.strings.at("components.card.gap") == "8px");
    REQUIRE_FALSE(has_diag_code(result.diagnostics, "broken-ref"));
}

// (0.3.0) ── unknown top-level frontmatter keys warn but don't error ─────
TEST_CASE("unknown top-level frontmatter key emits an unknown-key warning",
          "[view][import][designmd][parse][designmd030]") {
    const std::string yaml =
        "---\n"
        "name: Typo\n"
        "colors:\n"
        "  primary: \"#000000\"\n"
        "elevation:\n"            // not a schema key
        "  card: 0 2px 4px\n"
        "---\n";
    auto result = parse_designmd(yaml);
    bool found_elevation = false;
    for (const auto& d : result.diagnostics) {
        if (d.code == "unknown-key") {
            REQUIRE(d.severity == DesignMdSeverity::warning);  // never an error
            if (d.path == "elevation") found_elevation = true;
        }
    }
    REQUIRE(found_elevation);
    // The known keys parse normally alongside the warning.
    REQUIRE(result.ir.tokens.colors.at("primary") == "#000000");
}

// (0.3.0) ── the canonical schema keys never trip the unknown-key warning ─
TEST_CASE("all canonical frontmatter keys parse without an unknown-key warning",
          "[view][import][designmd][parse][designmd030]") {
    auto result = parse_designmd(upstream_fixture());
    REQUIRE_FALSE(has_diag_code(result.diagnostics, "unknown-key"));
}

// (0.3.0) ── numeric/boolean YAML scalars in component props don't crash ─
TEST_CASE("numeric and boolean component property scalars flow through as strings",
          "[view][import][designmd][parse][designmd030]") {
    const std::string yaml =
        "---\n"
        "name: Scalars\n"
        "components:\n"
        "  toggle:\n"
        "    fontWeight: 600\n"   // bare number
        "    enabled: true\n"     // boolean scalar
        "---\n";
    auto result = parse_designmd(yaml);  // must not throw
    REQUIRE(result.ir.tokens.strings.at("components.toggle.fontWeight") == "600");
    REQUIRE(result.ir.tokens.strings.at("components.toggle.enabled") == "true");
}
