// test_token_lock.cpp — Phase 4c token lock-to-source test suite.
//
// Covers planning/2026-05-18-inspector-direct-manipulation-roadmap.md
// Phase 4c: locking a token-typed inspector tweak rewrites exactly one
// token value in DESIGN.md, preserving every other byte (prose, comments,
// key order, indentation), and fails conservatively when the token
// cannot be located unambiguously.
//
// Tag set: [view][import][designmd][token-lock][issue-1307]

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/design_import.hpp>
#include <pulp/view/token_lock.hpp>

#include <string>

using namespace pulp::view;

namespace {

// A compact DESIGN.md fixture with known tokens across all four lockable
// groups, plus a `## prose` body and a YAML comment — the rewrite must
// leave both untouched.
const char* kFixture = R"(---
name: Synthwave
colors:
  primary: "#855300"   # the brand orange
  secondary: "#0058be"
  background: "#f9f9ff"
spacing:
  sm: 12px
  md: 24px
rounded:
  lg: 1rem
typography:
  body-md:
    fontFamily: Inter
    fontSize: 16px
    fontWeight: "400"
---

## Brand & Style

The palette centers on a warm orange. Primary is `#855300`.
)";

// Count occurrences of `needle` in `haystack` — used to assert the
// rewrite touched exactly one byte range.
int count_substr(const std::string& haystack, const std::string& needle) {
    int n = 0;
    std::string::size_type pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

}  // namespace

// ── Classification ─────────────────────────────────────────────────────

TEST_CASE("classify_token_tweak recognizes flat token property paths",
          "[view][token-lock][issue-1307]") {
    auto c = classify_token_tweak("", "colors.primary");
    REQUIRE(c.has_value());
    CHECK(c->group == TokenGroup::colors);
    CHECK(c->name == "primary");
    CHECK(c->field.empty());

    auto s = classify_token_tweak("", "spacing.md");
    REQUIRE(s.has_value());
    CHECK(s->group == TokenGroup::spacing);
    CHECK(s->name == "md");

    auto r = classify_token_tweak("", "rounded.lg");
    REQUIRE(r.has_value());
    CHECK(r->group == TokenGroup::rounded);
    CHECK(r->name == "lg");
}

TEST_CASE("classify_token_tweak recognizes nested typography paths",
          "[view][token-lock][issue-1307]") {
    auto t = classify_token_tweak("", "typography.body-md.fontSize");
    REQUIRE(t.has_value());
    CHECK(t->group == TokenGroup::typography);
    CHECK(t->name == "body-md");
    CHECK(t->field == "fontSize");

    // Typography without a field is NOT a lockable token target.
    CHECK_FALSE(classify_token_tweak("", "typography.body-md").has_value());
}

TEST_CASE("classify_token_tweak falls back to the designtoken: anchor",
          "[view][token-lock][issue-1307]") {
    // Element-shaped property path, but the anchor carries the token id.
    auto c = classify_token_tweak("designtoken:colors.secondary",
                                  "paint.backgroundColor");
    REQUIRE(c.has_value());
    CHECK(c->group == TokenGroup::colors);
    CHECK(c->name == "secondary");
}

TEST_CASE("classify_token_tweak rejects element-only tweaks",
          "[view][token-lock][issue-1307]") {
    // paint/layout/text paths are the Phase 4a/4b domain, not tokens.
    CHECK_FALSE(classify_token_tweak("anchor:abc123",
                                     "paint.backgroundColor").has_value());
    CHECK_FALSE(classify_token_tweak("anchor:abc123",
                                     "layout.padding").has_value());
    // `components` is deliberately excluded from Phase 4c.
    CHECK_FALSE(classify_token_tweak("", "components.button-primary.padding")
                    .has_value());
    // A flat group token whose name contains a dot is an element path.
    CHECK_FALSE(classify_token_tweak("", "colors.primary.alpha").has_value());
}

// ── Happy path: color token rewrite ────────────────────────────────────

TEST_CASE("lock_token_in_designmd rewrites a color token in place",
          "[view][token-lock][issue-1307]") {
    std::string md = kFixture;
    auto result = lock_token_in_designmd(md, "designtoken:colors.primary",
                                         "colors.primary", "#5a5a5a");
    REQUIRE(result.ok);
    CHECK(result.error.empty());
    CHECK(result.previous_value == "#855300");
    CHECK(result.line == 4);  // 1-based line of `  primary: ...`

    const std::string& out = result.updated_markdown;
    // The new value is present, the old hex literal is gone from the
    // token line (it still survives once in the prose body — see below).
    CHECK(count_substr(out, "primary: \"#5a5a5a\"") == 1);
    CHECK(count_substr(out, "primary: \"#855300\"") == 0);

    // Quote style preserved (the source value was double-quoted).
    CHECK(out.find("\"#5a5a5a\"") != std::string::npos);

    // The inline YAML comment on the same line is preserved verbatim.
    CHECK(out.find("# the brand orange") != std::string::npos);

    // Prose body untouched — `#855300` still appears in the Markdown.
    CHECK(out.find("Primary is `#855300`.") != std::string::npos);

    // Sibling tokens untouched.
    CHECK(out.find("secondary: \"#0058be\"") != std::string::npos);
    CHECK(out.find("background: \"#f9f9ff\"") != std::string::npos);

    // Exactly one byte range changed: re-locking with the original value
    // restores the file byte-for-byte.
    auto restored = lock_token_in_designmd(out, "designtoken:colors.primary",
                                           "colors.primary", "#855300");
    REQUIRE(restored.ok);
    CHECK(restored.updated_markdown == md);
}

TEST_CASE("lock_token_in_designmd rewrites a dimension token",
          "[view][token-lock][issue-1307]") {
    std::string md = kFixture;
    // spacing.md is `24px` unquoted — the rewrite keeps it unquoted.
    auto result = lock_token_in_designmd(md, "", "spacing.md", "32px");
    REQUIRE(result.ok);
    CHECK(result.previous_value == "24px");
    CHECK(result.updated_markdown.find("md: 32px") != std::string::npos);
    CHECK(result.updated_markdown.find("md: 24px") == std::string::npos);
    // No quotes added to a value that had none.
    CHECK(result.updated_markdown.find("md: \"32px\"") == std::string::npos);
    // The other spacing step is untouched.
    CHECK(result.updated_markdown.find("sm: 12px") != std::string::npos);
}

TEST_CASE("lock_token_in_designmd rewrites a nested typography field",
          "[view][token-lock][issue-1307]") {
    std::string md = kFixture;
    auto result = lock_token_in_designmd(
        md, "", "typography.body-md.fontSize", "18px");
    REQUIRE(result.ok);
    CHECK(result.previous_value == "16px");
    CHECK(result.updated_markdown.find("fontSize: 18px") != std::string::npos);
    CHECK(result.updated_markdown.find("fontSize: 16px") == std::string::npos);
    // Sibling typography fields in the same level are untouched.
    CHECK(result.updated_markdown.find("fontFamily: Inter") !=
          std::string::npos);
    CHECK(result.updated_markdown.find("fontWeight: \"400\"") !=
          std::string::npos);
}

// ── Failure paths (conservative — DESIGN.md unchanged) ─────────────────

TEST_CASE("lock_token_in_designmd fails when DESIGN.md has no frontmatter",
          "[view][token-lock][issue-1307]") {
    std::string md = "# Just prose\n\nNo frontmatter here.\n";
    auto result = lock_token_in_designmd(md, "", "colors.primary", "#000000");
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("no YAML frontmatter") != std::string::npos);
    // Unchanged-on-failure contract.
    CHECK(result.updated_markdown == md);
}

TEST_CASE("lock_token_in_designmd fails when the token group is absent",
          "[view][token-lock][issue-1307]") {
    std::string md = kFixture;
    // The fixture has no `colors.accent`.
    auto missing_token =
        lock_token_in_designmd(md, "", "colors.accent", "#abcdef");
    CHECK_FALSE(missing_token.ok);
    CHECK(missing_token.error.find("not found") != std::string::npos);
    CHECK(missing_token.updated_markdown == md);

    // A whole group that does not exist in this fixture.
    std::string no_group =
        "---\nname: Bare\ncolors:\n  primary: \"#000\"\n---\n";
    auto missing_group =
        lock_token_in_designmd(no_group, "", "spacing.md", "8px");
    CHECK_FALSE(missing_group.ok);
    CHECK(missing_group.error.find("no `spacing`") != std::string::npos);
    CHECK(missing_group.updated_markdown == no_group);
}

TEST_CASE("lock_token_in_designmd fails on a missing typography field",
          "[view][token-lock][issue-1307]") {
    std::string md = kFixture;
    // body-md exists, but has no `letterSpacing` field.
    auto result = lock_token_in_designmd(
        md, "", "typography.body-md.letterSpacing", "0.02em");
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("letterSpacing") != std::string::npos);
    CHECK(result.updated_markdown == md);
}

TEST_CASE("lock_token_in_designmd refuses a non-token (element) tweak",
          "[view][token-lock][issue-1307]") {
    std::string md = kFixture;
    auto result = lock_token_in_designmd(md, "anchor:abc123",
                                         "paint.backgroundColor", "#111111");
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("not a design token") != std::string::npos);
    CHECK(result.updated_markdown == md);
}

TEST_CASE("lock_token_in_designmd refuses a nested color palette",
          "[view][token-lock][issue-1307]") {
    // `brand` is a nested palette (shade map), not a scalar — Phase 4c
    // does not lock into nested palettes, it must fail clearly.
    std::string md =
        "---\nname: Nested\ncolors:\n  brand:\n    500: \"#ff0000\"\n---\n";
    auto result = lock_token_in_designmd(md, "", "colors.brand", "#00ff00");
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("not a scalar") != std::string::npos);
    CHECK(result.updated_markdown == md);
}

TEST_CASE("lock_token_in_designmd disambiguates same-named keys across groups",
          "[view][token-lock][issue-1307]") {
    // `md` exists in BOTH spacing and rounded. The lock must rewrite the
    // one in the requested group only — proving the locator keys off the
    // group, not just the leaf name.
    std::string md =
        "---\nname: Dup\nspacing:\n  md: 24px\nrounded:\n  md: 8px\n---\n";

    auto spacing = lock_token_in_designmd(md, "", "spacing.md", "30px");
    REQUIRE(spacing.ok);
    CHECK(spacing.updated_markdown.find("md: 30px") != std::string::npos);
    // The rounded `md` is untouched.
    CHECK(spacing.updated_markdown.find("md: 8px") != std::string::npos);

    auto rounded = lock_token_in_designmd(md, "", "rounded.md", "12px");
    REQUIRE(rounded.ok);
    CHECK(rounded.updated_markdown.find("md: 12px") != std::string::npos);
    // The spacing `md` is untouched.
    CHECK(rounded.updated_markdown.find("md: 24px") != std::string::npos);
}

// ── Integration: the rewritten file re-parses with the new value ──────

TEST_CASE("a locked DESIGN.md re-parses with the updated token",
          "[view][token-lock][issue-1307]") {
    std::string md = kFixture;
    auto result = lock_token_in_designmd(md, "", "colors.primary", "#123456");
    REQUIRE(result.ok);

    // Round-trip through the real DESIGN.md parser — the corrected token
    // is what a re-import would now see.
    auto parsed = parse_designmd(result.updated_markdown);
    REQUIRE(parsed.had_frontmatter);
    auto it = parsed.ir.tokens.colors.find("primary");
    REQUIRE(it != parsed.ir.tokens.colors.end());
    CHECK(it->second == "#123456");
}

// ── Finding 3: a lock preserves the ORIGINAL quote character ──────────
//
// A token written with single quotes must stay single-quoted after a
// rewrite; double stays double; unquoted stays unquoted. The locator
// records the actual quote char, not just a bool, so render_scalar can
// re-emit it verbatim.

TEST_CASE("lock_token_in_designmd keeps a single-quoted value single-quoted",
          "[view][token-lock][issue-1307]") {
    std::string md =
        "---\nname: Quotes\ncolors:\n  primary: '#855300'\n---\n";
    auto result = lock_token_in_designmd(md, "", "colors.primary", "#123456");
    REQUIRE(result.ok);
    // Single quotes preserved, double quotes never introduced.
    CHECK(result.updated_markdown.find("primary: '#123456'") !=
          std::string::npos);
    CHECK(result.updated_markdown.find("\"#123456\"") == std::string::npos);
}

TEST_CASE("lock_token_in_designmd keeps a double-quoted value double-quoted",
          "[view][token-lock][issue-1307]") {
    std::string md =
        "---\nname: Quotes\ncolors:\n  primary: \"#855300\"\n---\n";
    auto result = lock_token_in_designmd(md, "", "colors.primary", "#123456");
    REQUIRE(result.ok);
    CHECK(result.updated_markdown.find("primary: \"#123456\"") !=
          std::string::npos);
    CHECK(result.updated_markdown.find("'#123456'") == std::string::npos);
}

TEST_CASE("lock_token_in_designmd keeps an unquoted value unquoted",
          "[view][token-lock][issue-1307]") {
    std::string md =
        "---\nname: Quotes\nspacing:\n  md: 24px\n---\n";
    auto result = lock_token_in_designmd(md, "", "spacing.md", "30px");
    REQUIRE(result.ok);
    CHECK(result.updated_markdown.find("md: 30px") != std::string::npos);
    CHECK(result.updated_markdown.find("\"30px\"") == std::string::npos);
    CHECK(result.updated_markdown.find("'30px'") == std::string::npos);
}
