// Tests for the Claude Design project handoff-contract parser.

#include <pulp/design/design_handoff.hpp>

#include <catch2/catch_test_macros.hpp>

#include <choc/text/choc_JSON.h>

using namespace pulp::design;

namespace {

// A representative handoff README exercising every section.
constexpr const char* kReadme = R"MD(# Nebula Synth — Design Handoff

**Fidelity:** lo-fi (restyle into our system, don't trace pixels)
Bound to: ink-signal, aurora

## Screens

### Main Panel (screens/main.html)
- Background: #14171c
- Padding: 16px
- Knob size: 48px

### Settings (screens/settings.html)
- Background: #1a1d24

## Tokens

| Token | Value |
|-------|-------|
| color.bg | #14171c |
| radius.md | 8px |

## Interactions

- Knob drag: vertical, fine with shift
- Preset menu: click to open
)MD";

}  // namespace

TEST_CASE("parse_handoff_readme reads the fidelity declaration", "[handoff]") {
    auto c = parse_handoff_readme(kReadme, "proj/README.md");
    CHECK(c.fidelity == FidelityIntent::lofi);
    CHECK(fidelity_intent_name(c.fidelity) == "lofi");
    CHECK(c.source == "proj/README.md");
    CHECK(c.format_version == std::string(kHandoffFormatVersion));
}

TEST_CASE("parse_handoff_readme collects bound design systems sorted+unique", "[handoff]") {
    auto c = parse_handoff_readme(kReadme, "r");
    REQUIRE(c.design_systems.size() == 2);
    CHECK(c.design_systems[0] == "aurora");   // sorted
    CHECK(c.design_systems[1] == "ink-signal");
}

TEST_CASE("parse_handoff_readme parses screens with paths and specs", "[handoff]") {
    auto c = parse_handoff_readme(kReadme, "r");
    REQUIRE(c.screens.size() == 2);
    CHECK(c.screens[0].name == "Main Panel");
    CHECK(c.screens[0].path == "screens/main.html");
    REQUIRE(c.screens[0].specs.size() == 3);
    CHECK(c.screens[0].specs[0] == std::pair<std::string, std::string>{"Background", "#14171c"});
    CHECK(c.screens[0].specs[2].first == "Knob size");
    CHECK(c.screens[1].name == "Settings");
}

TEST_CASE("parse_handoff_readme parses the token table, skipping header/separator", "[handoff]") {
    auto c = parse_handoff_readme(kReadme, "r");
    REQUIRE(c.tokens.size() == 2);
    CHECK(c.tokens[0] == std::pair<std::string, std::string>{"color.bg", "#14171c"});
    CHECK(c.tokens[1].first == "radius.md");
}

TEST_CASE("parse_handoff_readme captures interaction notes", "[handoff]") {
    auto c = parse_handoff_readme(kReadme, "r");
    REQUIRE(c.interactions.size() == 2);
    CHECK(c.interactions[0] == "Knob drag: vertical, fine with shift");
}

TEST_CASE("fidelity_intent_from_text recognizes common phrasings", "[handoff]") {
    CHECK(fidelity_intent_from_text("hi-fi") == FidelityIntent::hifi);
    CHECK(fidelity_intent_from_text("pixel-perfect recreation") == FidelityIntent::hifi);
    CHECK(fidelity_intent_from_text("high fidelity") == FidelityIntent::hifi);
    CHECK(fidelity_intent_from_text("lo-fi") == FidelityIntent::lofi);
    CHECK(fidelity_intent_from_text("restyle to your design system") == FidelityIntent::lofi);
    CHECK(fidelity_intent_from_text("whatever") == FidelityIntent::unspecified);
    // An explicit lo-fi token wins over incidental "pixels" prose.
    CHECK(fidelity_intent_from_text("lo-fi (don't trace pixels)") == FidelityIntent::lofi);
    // Both explicitly declared -> ambiguous, not a coin flip.
    CHECK(fidelity_intent_from_text("hi-fi or lo-fi") == FidelityIntent::unspecified);
}

TEST_CASE("parse_handoff_readme is tolerant of a bare README", "[handoff]") {
    auto c = parse_handoff_readme("# Just a title\n\nSome prose.\n", "r");
    CHECK(c.fidelity == FidelityIntent::unspecified);
    CHECK(c.screens.empty());
    CHECK(c.tokens.empty());
    CHECK(c.design_systems.empty());
}

TEST_CASE("merge_design_systems folds disk slugs sorted+unique", "[handoff]") {
    auto c = parse_handoff_readme(kReadme, "r");  // has aurora, ink-signal
    merge_design_systems(c, {"ink-signal", "zephyr", ""});
    REQUIRE(c.design_systems.size() == 3);
    CHECK(c.design_systems[0] == "aurora");
    CHECK(c.design_systems[1] == "ink-signal");  // deduped
    CHECK(c.design_systems[2] == "zephyr");
}

TEST_CASE("handoff_contract_json is deterministic and complete", "[handoff]") {
    auto c = parse_handoff_readme(kReadme, "proj/README.md");
    auto json = handoff_contract_json(c);
    auto v = choc::json::parse(json);
    CHECK(v["format_version"].getString() == std::string(kHandoffFormatVersion));
    CHECK(v["fidelity"].getString() == "lofi");
    CHECK(v["design_systems"].size() == 2);
    CHECK(v["screens"].size() == 2);
    CHECK(v["screens"][0]["name"].getString() == "Main Panel");
    CHECK(v["screens"][0]["specs"]["Padding"].getString() == "16px");
    CHECK(v["tokens"].size() == 2);
    CHECK(v["tokens"][0]["name"].getString() == "color.bg");
    CHECK(v["interactions"].size() == 2);
    // Byte-stable across repeated serialization.
    CHECK(handoff_contract_json(c) == json);
}

TEST_CASE("parse_handoff_readme reads an inline `## Fidelity: hi-fi` heading", "[handoff]") {
    auto c = parse_handoff_readme("# T\n\n## Fidelity: hi-fi\n\nRecreate exactly.\n", "r");
    CHECK(c.fidelity == FidelityIntent::hifi);
}

TEST_CASE("parse_handoff_readme resolves contradictory cross-line fidelity to unspecified", "[handoff]") {
    // Two explicit but contradictory Fidelity declarations -> ambiguous, not first-wins.
    auto c = parse_handoff_readme("# T\n\nFidelity: hi-fi\n\nFidelity: lo-fi\n", "r");
    CHECK(c.fidelity == FidelityIntent::unspecified);
}

TEST_CASE("fidelity_intent_from_text stays unspecified on conflicting weak prose", "[handoff]") {
    // "no pixel matching; adapt" has a weak hi word (pixel) and a weak lo word
    // (adapt) but no explicit token -> unspecified rather than a first-word guess.
    CHECK(fidelity_intent_from_text("no pixel matching; adapt to our system") ==
          FidelityIntent::unspecified);
}

TEST_CASE("parse_handoff_readme does not steal a `* Design system:` screen bullet", "[handoff]") {
    // A bullet under a screen is a spec, not a global key line.
    auto c = parse_handoff_readme(
        "# T\n\n## Screens\n\n### Home\n- Design system: local-note\n- Padding: 8px\n", "r");
    REQUIRE(c.screens.size() == 1);
    REQUIRE(c.screens[0].specs.size() == 2);  // both bullets kept as specs
    CHECK(c.design_systems.empty());          // not hoisted into the global list
}

TEST_CASE("split screen heading keeps a non-path parenthetical in the name", "[handoff]") {
    auto c = parse_handoff_readme("# T\n\n## Screens\n\n### Login (mobile)\n- x: 1\n", "r");
    REQUIRE(c.screens.size() == 1);
    CHECK(c.screens[0].name == "Login (mobile)");  // "(mobile)" is not a path
    CHECK(c.screens[0].path.empty());
    // A real path IS extracted.
    auto d = parse_handoff_readme("# T\n\n## Screens\n\n### Home (screens/home.html)\n- x: 1\n", "r");
    CHECK(d.screens[0].path == "screens/home.html");
}

TEST_CASE("handoff_contract_json survives a repeated spec key without throwing", "[handoff]") {
    // A README may repeat a spec bullet key. addMember would throw on the second
    // "Background"; the serializer must fold it (last value wins), not abort.
    auto c = parse_handoff_readme(
        "# T\n\n## Screens\n\n### Home\n- Background: #111\n- Background: #222\n", "r");
    REQUIRE(c.screens.size() == 1);
    REQUIRE(c.screens[0].specs.size() == 2);  // both bullets parsed
    std::string json;
    REQUIRE_NOTHROW(json = handoff_contract_json(c));
    auto v = choc::json::parse(json);
    // Object collapses duplicate keys; last one wins.
    CHECK(v["screens"][0]["specs"]["Background"].getString() == "#222");
}

TEST_CASE("split screen heading rejects absolute and traversal paths", "[handoff]") {
    // An absolute path or a ".." segment would escape the project root a consumer
    // joins onto; keep the whole heading as the name and emit no path.
    auto abs = parse_handoff_readme("# T\n\n## Screens\n\n### Config (/etc/passwd)\n- x: 1\n", "r");
    REQUIRE(abs.screens.size() == 1);
    CHECK(abs.screens[0].name == "Config (/etc/passwd)");
    CHECK(abs.screens[0].path.empty());

    auto up = parse_handoff_readme(
        "# T\n\n## Screens\n\n### Secrets (../../etc/shadow)\n- x: 1\n", "r");
    CHECK(up.screens[0].name == "Secrets (../../etc/shadow)");
    CHECK(up.screens[0].path.empty());

    // A benign nested relative path is still accepted.
    auto ok = parse_handoff_readme(
        "# T\n\n## Screens\n\n### Deep (a/b/c/home.html)\n- x: 1\n", "r");
    CHECK(ok.screens[0].path == "a/b/c/home.html");
}
