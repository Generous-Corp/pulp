// Tests for the adherence lint (pulp::design::lint_adherence): the mechanical
// backstop that flags raw colors, unknown var(--token) references, and
// token-valued px literals in UI JS.

#include <catch2/catch_test_macros.hpp>

#include <pulp/design/design_adherence.hpp>
#include <pulp/design/design_manifest.hpp>

using namespace pulp::design;

namespace {

// A small hand-built contract so the assertions don't depend on the built-in
// system's exact token values.
DesignManifest test_manifest() {
    DesignManifest m;
    m.manifest_version = std::string(kManifestVersion);
    m.tokens = {
        {"accent.primary", "color", "#10b6a3"},
        {"color.bg", "color", "#14171c"},
        {"radius.md", "dimension", "8"},
    };
    return m;
}

bool has_kind_at(const std::vector<AdherenceFinding>& fs, AdherenceKind k, int line) {
    for (const auto& f : fs)
        if (f.kind == k && f.line == line) return true;
    return false;
}

}  // namespace

TEST_CASE("raw hex color is flagged and the matching token is named", "[design-adherence]") {
    auto fs = lint_adherence("el.style.background = '#14171c';\n", test_manifest());
    REQUIRE(fs.size() == 1);
    REQUIRE(fs[0].kind == AdherenceKind::raw_color);
    REQUIRE(fs[0].severity == AdherenceSeverity::error);
    REQUIRE(fs[0].line == 1);
    REQUIRE(fs[0].message.find("color.bg") != std::string::npos);  // value is a known token
}

TEST_CASE("a raw hex not in the system is still flagged, without a token name",
          "[design-adherence]") {
    auto fs = lint_adherence("const c = '#abcdef';\n", test_manifest());
    REQUIRE(fs.size() == 1);
    REQUIRE(fs[0].kind == AdherenceKind::raw_color);
    REQUIRE(fs[0].message.find("token `") == std::string::npos);  // no suggestion
}

TEST_CASE("unknown var(--token) is flagged; a valid one is clean", "[design-adherence]") {
    auto bad = lint_adherence("x = 'var(--totally-made-up)';\n", test_manifest());
    REQUIRE(bad.size() == 1);
    REQUIRE(bad[0].kind == AdherenceKind::unknown_token);
    REQUIRE(bad[0].severity == AdherenceSeverity::error);

    // accent.primary → --accent-primary is a real token, so no finding.
    auto good = lint_adherence("x = 'var(--accent-primary)';\n", test_manifest());
    REQUIRE(good.empty());
}

TEST_CASE("a px literal matching a dimension token is an info finding", "[design-adherence]") {
    auto fs = lint_adherence("el.style.padding = '8px';\n", test_manifest());
    REQUIRE(fs.size() == 1);
    REQUIRE(fs[0].kind == AdherenceKind::raw_dimension);
    REQUIRE(fs[0].severity == AdherenceSeverity::info);
    REQUIRE(fs[0].message.find("radius.md") != std::string::npos);
    // A px value the system does NOT define is not flagged (avoids noise).
    REQUIRE(lint_adherence("el.style.margin = '13px';\n", test_manifest()).empty());
    // 8.0px normalizes to the token's "8".
    REQUIRE_FALSE(lint_adherence("el.style.padding = '8.0px';\n", test_manifest()).empty());
}

TEST_CASE("comment content is ignored; string content is scanned", "[design-adherence]") {
    const std::string js =
        "// a comment with #14171c and var(--nope) and 8px\n"
        "/* block #ffffff var(--gone) 8px */\n"
        "const real = '#14171c';\n";
    auto fs = lint_adherence(js, test_manifest());
    // Only the line-3 string literal is a finding; the comments are inert.
    REQUIRE(fs.size() == 1);
    REQUIRE(fs[0].line == 3);
}

TEST_CASE("a 3-digit hex shorthand resolves to its 6-digit token value", "[design-adherence]") {
    DesignManifest m;
    m.tokens = {{"white", "color", "#ffffff"}};
    auto fs = lint_adherence("c = '#fff';\n", m);
    REQUIRE(fs.size() == 1);
    REQUIRE(fs[0].message.find("white") != std::string::npos);
}

TEST_CASE("an 8-digit (alpha) hex is flagged and named", "[design-adherence]") {
    DesignManifest m;
    m.tokens = {{"overlay", "color", "#00000080"}};
    auto fs = lint_adherence("el.style.background = '#00000080';\n", m);
    REQUIRE(fs.size() == 1);
    REQUIRE(fs[0].kind == AdherenceKind::raw_color);
    REQUIRE(fs[0].message.find("overlay") != std::string::npos);
}

TEST_CASE("a .dark-suffixed token maps to the same var(--x) as its base", "[design-adherence]") {
    // token_css_var strips ".dark"; a var(--color-bg) reference must resolve
    // whether the manifest carries the base or the dark-mode token.
    DesignManifest m;
    m.tokens = {{"color.bg.dark", "color", "#000000"}};
    REQUIRE(lint_adherence("x = 'var(--color-bg)';\n", m).empty());
}

TEST_CASE("a token name that already contains '-' round-trips through var(--x)",
          "[design-adherence]") {
    // "." → "-" must not corrupt a name that already has hyphens.
    DesignManifest m;
    m.tokens = {{"surface.container-high", "color", "#111111"}};
    REQUIRE(lint_adherence("x = 'var(--surface-container-high)';\n", m).empty());
    // A near-miss (dropped segment) is still flagged as unknown.
    REQUIRE_FALSE(lint_adherence("x = 'var(--surface-container)';\n", m).empty());
}

TEST_CASE("a comment marker inside a string does not end the scan", "[design-adherence]") {
    // The // and /* here are string content, not comments — the hex after them
    // is still real and must be flagged.
    DesignManifest m;
    m.tokens = {{"bg", "color", "#14171c"}};
    REQUIRE_FALSE(lint_adherence("url = 'http://x'; c = '#14171c';\n", m).empty());
    auto fs = lint_adherence("s = 'a // b'; c = '#14171c';\n", m);
    REQUIRE(fs.size() == 1);
}

TEST_CASE("hex and var inside a multiline template literal are attributed to their line",
          "[design-adherence]") {
    DesignManifest m;
    m.tokens = {{"bg", "color", "#14171c"}};
    const std::string js =
        "const css = `\n"
        "  background: #14171c;\n"
        "`;\n";
    auto fs = lint_adherence(js, m);
    REQUIRE(fs.size() == 1);
    REQUIRE(fs[0].line == 2);  // the color is on the second line of the template
}

TEST_CASE("findings come back in source order", "[design-adherence]") {
    const std::string js =
        "a = 'var(--nope)';\n"
        "b = '#14171c';\n"
        "c = '8px';\n";
    auto fs = lint_adherence(js, test_manifest());
    REQUIRE(fs.size() == 3);
    REQUIRE(fs[0].line == 1);
    REQUIRE(fs[1].line == 2);
    REQUIRE(fs[2].line == 3);
    REQUIRE(has_kind_at(fs, AdherenceKind::unknown_token, 1));
    REQUIRE(has_kind_at(fs, AdherenceKind::raw_color, 2));
    REQUIRE(has_kind_at(fs, AdherenceKind::raw_dimension, 3));
}

TEST_CASE("clean JS produces no findings", "[design-adherence]") {
    REQUIRE(lint_adherence("el.style.color = 'var(--accent-primary)';\n", test_manifest()).empty());
    REQUIRE(lint_adherence("", test_manifest()).empty());
}

TEST_CASE("kind slugs are stable", "[design-adherence]") {
    REQUIRE(std::string(adherence_kind_name(AdherenceKind::raw_color)) == "raw-color");
    REQUIRE(std::string(adherence_kind_name(AdherenceKind::unknown_token)) == "unknown-token");
    REQUIRE(std::string(adherence_kind_name(AdherenceKind::raw_dimension)) == "raw-dimension");
}
