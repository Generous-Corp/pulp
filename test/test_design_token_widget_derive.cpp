#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/view/design_ir.hpp>
#include <pulp/view/design_tokens.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

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

namespace {

// The colours Pulp's primitives fall back to when a widget key is unset.
// Asserting against these specific values is the point: "not blue" would pass
// for a widget that had merely swapped one invented colour for another.
constexpr std::uint8_t kFallbackFillR = 100, kFallbackFillG = 150, kFallbackFillB = 255;

bool is_rgb(const Color& c, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return c.r8() == r && c.g8() == g && c.b8() == b;
}

// A design captured from CSS, in the vocabulary every Pulp design pack shares.
IRTokens captured_css_design() {
    IRTokens tokens;
    tokens.colors["css/accent"] = "#C4622A";
    tokens.colors["css/line-strong"] = "#3C321E";
    tokens.colors["css/text-strong"] = "#2A2418";
    tokens.colors["css/signal-low"] = "#3FCF77";
    tokens.colors["css/signal-mid"] = "#F6B847";
    tokens.colors["css/signal-high"] = "#FF5C4D";
    return tokens;
}

}  // namespace

TEST_CASE("a CSS-captured design reaches the fader and the meter",
          "[design][tokens]") {
    // The fader and the meter do not read a control's design_* attributes the
    // way the knob does — they resolve theme keys. Tokens captured from CSS are
    // named `css/<custom-property>`, so copying by name left `control.fill`
    // unset and the fader painted its built-in blue on a cream faceplate.
    const auto theme = ir_tokens_to_theme(captured_css_design());

    REQUIRE(theme.colors.count("control.fill") == 1);
    CHECK(is_rgb(theme.colors.at("control.fill"), 0xC4, 0x62, 0x2A));
    CHECK_FALSE(is_rgb(theme.colors.at("control.fill"),
                       kFallbackFillR, kFallbackFillG, kFallbackFillB));

    REQUIRE(theme.colors.count("control.track") == 1);
    CHECK(is_rgb(theme.colors.at("control.track"), 0x3C, 0x32, 0x1E));
    REQUIRE(theme.colors.count("control.thumb") == 1);
    CHECK(is_rgb(theme.colors.at("control.thumb"), 0x2A, 0x24, 0x18));

    // The knob resolves the same three tokens through its own path; the two
    // must agree, because a fader and a knob disagreeing about the design's
    // accent is exactly the drift this mapping exists to prevent.
    CHECK(is_rgb(theme.colors.at("knob.arc"), 0xC4, 0x62, 0x2A));
    CHECK(is_rgb(theme.colors.at("knob.arc.bg"), 0x3C, 0x32, 0x1E));
    CHECK(is_rgb(theme.colors.at("knob.thumb"), 0x2A, 0x24, 0x18));
}

TEST_CASE("a meter's zones are the design's signal ramp", "[design][tokens]") {
    const auto theme = ir_tokens_to_theme(captured_css_design());

    REQUIRE(theme.colors.count("meter.green") == 1);
    CHECK(is_rgb(theme.colors.at("meter.green"), 0x3F, 0xCF, 0x77));
    REQUIRE(theme.colors.count("meter.yellow") == 1);
    CHECK(is_rgb(theme.colors.at("meter.yellow"), 0xF6, 0xB8, 0x47));
    REQUIRE(theme.colors.count("meter.red") == 1);
    CHECK(is_rgb(theme.colors.at("meter.red"), 0xFF, 0x5C, 0x4D));
}

TEST_CASE("a one-colour signal ramp yields a one-colour meter",
          "[design][tokens]") {
    // theme_presets.cpp derives a built-in theme's meter zones by rotating the
    // accent's hue. Doing that for an imported design would paint a green and
    // an amber zone the design never chose. A design whose ramp runs through a
    // single rust gets a meter in that single rust.
    IRTokens tokens;
    tokens.colors["css/signal-low"] = "#C4622A";
    tokens.colors["css/signal-mid"] = "#C4622A";
    tokens.colors["css/signal-high"] = "#C4622A";

    const auto theme = ir_tokens_to_theme(tokens);
    for (const char* key : {"meter.green", "meter.yellow", "meter.red"}) {
        REQUIRE(theme.colors.count(key) == 1);
        CHECK(is_rgb(theme.colors.at(key), 0xC4, 0x62, 0x2A));
    }
}

TEST_CASE("an unstated widget colour is left unset and reported",
          "[design][tokens]") {
    // The no-fallback contract: a design that never mentions a signal ramp has
    // no meter colours, and the gap is named rather than filled. A synthesized
    // colour here would be a second palette drifting from the first, and it
    // would be invisible — the render would simply carry a colour from nowhere.
    IRTokens tokens;
    tokens.colors["css/accent"] = "#C4622A";

    std::vector<std::string> unresolved;
    const auto theme = ir_tokens_to_theme(tokens, &unresolved);

    CHECK(theme.colors.count("meter.green") == 0);
    CHECK(theme.colors.count("meter.yellow") == 0);
    CHECK(theme.colors.count("meter.red") == 0);

    const auto reported = [&](const char* key) {
        return std::find(unresolved.begin(), unresolved.end(), key) != unresolved.end();
    };
    CHECK(reported("meter.green"));
    CHECK(reported("meter.yellow"));
    CHECK(reported("meter.red"));
    // What the design DID state must not be reported as a gap.
    CHECK_FALSE(reported("control.fill"));
    CHECK_FALSE(reported("knob.arc"));
}

TEST_CASE("a design's own widget colour outranks the derived one",
          "[design][tokens]") {
    // Same contract as the bare-name case, through the CSS vocabulary: the
    // design's explicit choice is the more specific instruction.
    IRTokens tokens = captured_css_design();
    tokens.colors["control.fill"] = "#16DAC2";

    const auto theme = ir_tokens_to_theme(tokens);
    CHECK(is_rgb(theme.colors.at("control.fill"), 0x16, 0xDA, 0xC2));
}

TEST_CASE("a CSS-captured design reaches text and surfaces too",
          "[design][tokens]") {
    // Same bug as the fader, one widget over: outline_api.cpp resolves
    // `text.primary` and would paint its built-in near-white the first time an
    // imported design set an outline. Nothing here is derived — `text.secondary`
    // is the design's own muted text, not a blend toward its muted colour.
    IRTokens tokens = captured_css_design();
    tokens.colors["css/text"] = "#2A2418";
    tokens.colors["css/text-muted"] = "#7A6E58";
    tokens.colors["css/text-faint"] = "#9C8F76";
    tokens.colors["css/accent-text"] = "#FFF4E8";
    tokens.colors["css/surface-app"] = "#232019";
    tokens.colors["css/surface-panel"] = "#EFE6D2";
    tokens.colors["css/surface-raised"] = "#F7F0E0";
    tokens.colors["css/line"] = "#3C321E";

    const auto theme = ir_tokens_to_theme(tokens);
    REQUIRE(theme.colors.count("text.primary") == 1);
    CHECK(is_rgb(theme.colors.at("text.primary"), 0x2A, 0x24, 0x18));
    // The near-white outline_api.cpp would otherwise have painted.
    CHECK_FALSE(is_rgb(theme.colors.at("text.primary"), 220, 220, 220));
    REQUIRE(theme.colors.count("text.secondary") == 1);
    CHECK(is_rgb(theme.colors.at("text.secondary"), 0x7A, 0x6E, 0x58));
    REQUIRE(theme.colors.count("text.disabled") == 1);
    CHECK(is_rgb(theme.colors.at("text.disabled"), 0x9C, 0x8F, 0x76));
    REQUIRE(theme.colors.count("accent.text") == 1);
    CHECK(is_rgb(theme.colors.at("accent.text"), 0xFF, 0xF4, 0xE8));

    REQUIRE(theme.colors.count("bg.primary") == 1);
    CHECK(is_rgb(theme.colors.at("bg.primary"), 0x23, 0x20, 0x19));
    REQUIRE(theme.colors.count("bg.surface") == 1);
    CHECK(is_rgb(theme.colors.at("bg.surface"), 0xEF, 0xE6, 0xD2));
    REQUIRE(theme.colors.count("bg.elevated") == 1);
    CHECK(is_rgb(theme.colors.at("bg.elevated"), 0xF7, 0xF0, 0xE0));
    REQUIRE(theme.colors.count("divider") == 1);
    CHECK(is_rgb(theme.colors.at("divider"), 0x3C, 0x32, 0x1E));

    // The unprefixed spellings a few older panels resolve directly.
    REQUIRE(theme.colors.count("surface") == 1);
    CHECK(is_rgb(theme.colors.at("surface"), 0xEF, 0xE6, 0xD2));
    REQUIRE(theme.colors.count("accent") == 1);
    CHECK(is_rgb(theme.colors.at("accent"), 0xC4, 0x62, 0x2A));
}

TEST_CASE("a translucent CSS token survives into the theme", "[design][tokens]") {
    // A design captured from CSS states a hairline as `rgba(60,50,30,0.18)`,
    // not as hex. Reading it with a hex-only parser yields a
    // default-constructed Color, which paints as BLACK — and the knob path
    // never saw it because that path resolves the same token through
    // parse_css_color. One design, two parsers, two answers; the fader's
    // track came out black on a cream faceplate.
    IRTokens tokens;
    tokens.colors["css/line-strong"] = "rgba(60,50,30,0.18)";

    const auto theme = ir_tokens_to_theme(tokens);
    REQUIRE(theme.colors.count("control.track") == 1);
    const auto& track = theme.colors.at("control.track");
    CHECK(is_rgb(track, 60, 50, 30));
    CHECK_FALSE(is_rgb(track, 0, 0, 0));
    CHECK(track.a == Catch::Approx(0.18f).margin(0.01f));
}

TEST_CASE("an unreadable colour token is not a colour", "[design][tokens]") {
    // parse_css_color answers opaque WHITE for a token it cannot read, which
    // is a colour from nowhere exactly as a fallback palette is. A token this
    // parser does not model must leave its key unset and be reported, so the
    // widget paints its own default and the gap has a name.
    IRTokens tokens;
    tokens.colors["css/line-strong"] = "oklab(0.6 0.1 0.05)";
    tokens.colors["css/accent"] = "rebeccapurple";

    std::vector<std::string> unresolved;
    const auto theme = ir_tokens_to_theme(tokens, &unresolved);

    CHECK(theme.colors.count("css/line-strong") == 0);
    CHECK(theme.colors.count("css/accent") == 0);
    CHECK(theme.colors.count("control.track") == 0);
    CHECK(theme.colors.count("control.fill") == 0);
    CHECK(std::find(unresolved.begin(), unresolved.end(), "control.track") !=
          unresolved.end());
    CHECK(std::find(unresolved.begin(), unresolved.end(), "control.fill") !=
          unresolved.end());
}

TEST_CASE("two designs give two sets of primitive colours", "[design][tokens]") {
    // A key reading a constant satisfies every single-design assertion above
    // and dies here.
    IRTokens warm;
    warm.colors["css/accent"] = "#C4622A";
    IRTokens cool;
    cool.colors["css/accent"] = "#5E78FF";

    const auto warm_theme = ir_tokens_to_theme(warm);
    const auto cool_theme = ir_tokens_to_theme(cool);
    for (const char* key : {"control.fill", "knob.arc", "slider.fill",
                            "accent.primary", "focus.ring"}) {
        REQUIRE(warm_theme.colors.count(key) == 1);
        REQUIRE(cool_theme.colors.count(key) == 1);
        CHECK(is_rgb(warm_theme.colors.at(key), 0xC4, 0x62, 0x2A));
        CHECK(is_rgb(cool_theme.colors.at(key), 0x5E, 0x78, 0xFF));
    }
}
