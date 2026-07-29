#include <catch2/catch_test_macros.hpp>

#include <pulp/view/design_ir.hpp>
#include <pulp/view/design_tokens.hpp>

using namespace pulp::view;

TEST_CASE("a design's accent reaches its controls", "[design][tokens]") {
    // Tokens are copied into the theme by NAME, so a design that names
    // `primary` has no `knob.arc` — and the knob resolves `knob.arc`, falling
    // back to a built-in blue. The accent reached panels and text and never
    // the controls: every knob wore the same colour whatever the design said.
    IRTokens tokens;
    tokens.colors["primary"] = "#F2A65A";
    tokens.colors["muted"] = "#14323A";
    tokens.colors["foreground"] = "#F4FBFA";

    const auto theme = ir_tokens_to_theme(tokens);
    REQUIRE(theme.colors.count("knob.arc") == 1);
    const auto& derived = theme.colors.at("knob.arc");
    const auto& accent = theme.colors.at("primary");
    const bool matches = (derived.r == accent.r) && (derived.g == accent.g) &&
                         (derived.b == accent.b);
    CHECK(matches);
    CHECK(theme.colors.count("knob.arc.bg") == 1);
    CHECK(theme.colors.count("knob.thumb") == 1);
}

TEST_CASE("a design that names a widget token keeps it", "[design][tokens]") {
    // Derivation fills what is ABSENT. Overwriting a deliberate choice would
    // make the more specific instruction lose to the more general one.
    IRTokens tokens;
    tokens.colors["primary"] = "#F2A65A";
    tokens.colors["knob.arc"] = "#16DAC2";

    const auto theme = ir_tokens_to_theme(tokens);
    // The explicit teal must survive, so it must NOT equal the amber primary.
    const auto& arc = theme.colors.at("knob.arc");
    const auto& primary = theme.colors.at("primary");
    const bool same = (arc.r == primary.r) && (arc.g == primary.g) &&
                      (arc.b == primary.b);
    CHECK_FALSE(same);
}

TEST_CASE("no accent means no invented widget colour", "[design][tokens]") {
    // A design that says nothing must not acquire a colour from nowhere; the
    // widget's own default is the honest answer.
    IRTokens tokens;
    tokens.colors["background"] = "#07080B";

    const auto theme = ir_tokens_to_theme(tokens);
    CHECK(theme.colors.count("knob.arc") == 0);
}
