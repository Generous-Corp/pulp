// Tests for semantic knobs: parse a declared schema, select a position, split
// resolved writes into theme-token vs local-block by the bound manifest, and
// persist the local writes into the artifact's EDITMODE block.

#include <pulp/design/design_knobs.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace pulp::design;

namespace {

// The "theme" side of the token-vs-local rule: the bound theme's token names.
// "accent" and "radius" are real tokens; anything else a knob writes is local.
const std::vector<std::string> kThemeTokens = {"accent", "radius"};

// An artifact carrying an EDITMODE block with surrounding content that a rewrite
// must never disturb.
const std::string kArtifact =
    "export const ui = () => {\n"
    "  const p = /*EDITMODE-BEGIN*/{\"density\":\"cozy\"}/*EDITMODE-END*/;\n"
    "  return render(p);\n"
    "};\n";

// A minimalism slider: three detents, each moving a bundle. "accent" is a theme
// token; "shadow" and "density" are local overrides.
const std::string kSchemaJson = R"JSON({
  "knobs": [
    {
      "id": "minimalism",
      "label": "Minimalism",
      "kind": "slider",
      "positions": [
        {"label": "rich", "at": 1.0,
         "writes": [{"key": "shadow", "value": true}, {"key": "density", "value": "cozy"}]},
        {"label": "plain", "at": 0.0,
         "writes": [{"key": "shadow", "value": false}, {"key": "density", "value": "airy"},
                    {"key": "accent", "value": "#8899aa"}]}
      ]
    },
    {
      "id": "layout",
      "label": "Layout",
      "kind": "enum",
      "positions": [
        {"label": "stack", "writes": [{"key": "layout", "value": "stack"}]},
        {"label": "grid", "writes": [{"key": "layout", "value": "grid"}]}
      ]
    }
  ]
})JSON";

const KnobWrite* find_write(const std::vector<KnobWrite>& ws, std::string_view key) {
    for (const auto& w : ws)
        if (w.key == key) return &w;
    return nullptr;
}

}  // namespace

TEST_CASE("parse_knob_schema reads knobs, kinds, positions, writes", "[knobs]") {
    auto schema = parse_knob_schema(kSchemaJson);
    REQUIRE(schema.has_value());
    REQUIRE(schema->knobs.size() == 2);

    const KnobSpec* mini = find_knob(*schema, "minimalism");
    REQUIRE(mini != nullptr);
    CHECK(mini->label == "Minimalism");
    CHECK(mini->kind == KnobKind::Slider);
    REQUIRE(mini->positions.size() == 2);
    // Slider positions are sorted by `at` on parse: plain(0.0) before rich(1.0).
    CHECK(mini->positions[0].label == "plain");
    CHECK(mini->positions[1].label == "rich");

    // Write values keep their JSON text form (booleans bare, strings quoted).
    const KnobWrite* shadow = find_write(mini->positions[1].writes, "shadow");
    REQUIRE(shadow != nullptr);
    CHECK(shadow->json_value == "true");
    const KnobWrite* density = find_write(mini->positions[1].writes, "density");
    REQUIRE(density != nullptr);
    CHECK(density->json_value == "\"cozy\"");
}

TEST_CASE("parse_knob_schema hard-rejects a non-schema, degrades on soft errors", "[knobs]") {
    CHECK_FALSE(parse_knob_schema("not json").has_value());
    CHECK_FALSE(parse_knob_schema("[1,2,3]").has_value());       // not an object
    CHECK_FALSE(parse_knob_schema("{\"nope\": 1}").has_value());  // no knobs array

    // A knob with no id is skipped; a valid sibling still parses.
    auto schema = parse_knob_schema(
        R"({"knobs":[{"label":"nameless"},{"id":"ok","positions":[]}]})");
    REQUIRE(schema.has_value());
    REQUIRE(schema->knobs.size() == 1);
    CHECK(schema->knobs[0].id == "ok");
}

TEST_CASE("knob_schema_to_json round-trips", "[knobs]") {
    auto schema = parse_knob_schema(kSchemaJson);
    REQUIRE(schema.has_value());
    auto reparsed = parse_knob_schema(knob_schema_to_json(*schema));
    REQUIRE(reparsed.has_value());
    REQUIRE(reparsed->knobs.size() == schema->knobs.size());
    const KnobSpec* mini = find_knob(*reparsed, "minimalism");
    REQUIRE(mini != nullptr);
    CHECK(mini->kind == KnobKind::Slider);
    const KnobWrite* density = find_write(mini->positions[1].writes, "density");
    REQUIRE(density != nullptr);
    CHECK(density->json_value == "\"cozy\"");  // still a quoted string, not doubly-quoted
}

TEST_CASE("select_position picks the nearest slider anchor", "[knobs]") {
    auto schema = parse_knob_schema(kSchemaJson);
    REQUIRE(schema.has_value());
    const KnobSpec* mini = find_knob(*schema, "minimalism");
    REQUIRE(mini != nullptr);

    CHECK(select_position(*mini, "0.1")->label == "plain");
    CHECK(select_position(*mini, "0.9")->label == "rich");
    CHECK(select_position(*mini, "0.49")->label == "plain");
    CHECK(select_position(*mini, "0.51")->label == "rich");
    // A non-numeric value selects nothing on a slider.
    CHECK(select_position(*mini, "loud") == nullptr);
}

TEST_CASE("select_position matches enum by label then index", "[knobs]") {
    auto schema = parse_knob_schema(kSchemaJson);
    REQUIRE(schema.has_value());
    const KnobSpec* layout = find_knob(*schema, "layout");
    REQUIRE(layout != nullptr);

    CHECK(select_position(*layout, "grid")->label == "grid");
    CHECK(select_position(*layout, "0")->label == "stack");  // 0-based index fallback
    CHECK(select_position(*layout, "1")->label == "grid");
    CHECK(select_position(*layout, "9") == nullptr);        // out of range
    CHECK(select_position(*layout, "nope") == nullptr);     // no such label
}

TEST_CASE("select_position on a toggle matches by label like an enum", "[knobs]") {
    auto schema = parse_knob_schema(
        R"({"knobs":[{"id":"shadow","kind":"toggle","positions":[
             {"label":"off","writes":[{"key":"shadow","value":false}]},
             {"label":"on","writes":[{"key":"shadow","value":true}]}]}]})");
    REQUIRE(schema.has_value());
    const KnobSpec* t = find_knob(*schema, "shadow");
    REQUIRE(t != nullptr);
    CHECK(t->kind == KnobKind::Toggle);
    CHECK(select_position(*t, "on")->label == "on");
    CHECK(select_position(*t, "0")->label == "off");  // index fallback, same as enum
    CHECK(select_position(*t, "nope") == nullptr);
}

TEST_CASE("select_position rejects non-'.' locale forms and junk", "[knobs]") {
    auto schema = parse_knob_schema(kSchemaJson);
    REQUIRE(schema.has_value());
    const KnobSpec* mini = find_knob(*schema, "minimalism");
    REQUIRE(mini != nullptr);
    // JSON-number parsing is locale-independent and strict: trailing junk and a
    // bare-scalar-that-is-not-a-number both select nothing.
    CHECK(select_position(*mini, "0.5x") == nullptr);
    CHECK(select_position(*mini, "") == nullptr);
    CHECK(select_position(*mini, "true") == nullptr);
}

TEST_CASE("classify_write splits theme tokens from local overrides", "[knobs]") {
    CHECK(classify_write({"accent", "\"#123456\""}, kThemeTokens) == WriteTarget::ThemeToken);
    CHECK(classify_write({"radius", "12"}, kThemeTokens) == WriteTarget::ThemeToken);
    CHECK(classify_write({"shadow", "true"}, kThemeTokens) == WriteTarget::LocalBlock);
    CHECK(classify_write({"density", "\"airy\""}, kThemeTokens) == WriteTarget::LocalBlock);
}

TEST_CASE("resolve_knob returns writes split by target", "[knobs]") {
    auto schema = parse_knob_schema(kSchemaJson);
    const KnobSpec* mini = find_knob(*schema, "minimalism");
    REQUIRE(mini != nullptr);

    auto effect = resolve_knob(*mini, "0.0", kThemeTokens);  // the "plain" detent
    REQUIRE(effect.has_value());
    // accent -> theme token; shadow + density -> local.
    REQUIRE(effect->token_writes.size() == 1);
    CHECK(effect->token_writes[0].key == "accent");
    REQUIRE(effect->local_writes.size() == 2);
    CHECK(find_write(effect->local_writes, "shadow") != nullptr);
    CHECK(find_write(effect->local_writes, "density") != nullptr);

    CHECK_FALSE(resolve_knob(*mini, "not-a-number", kThemeTokens).has_value());
}

TEST_CASE("apply_knob persists local writes into the block, hands back token writes", "[knobs]") {
    auto schema = parse_knob_schema(kSchemaJson);
    const KnobSpec* mini = find_knob(*schema, "minimalism");
    REQUIRE(mini != nullptr);

    auto applied = apply_knob(kArtifact, *mini, "0.0", kThemeTokens);  // "plain"
    REQUIRE(applied.has_value());

    // Token write is NOT written into the block; it is returned for the caller.
    REQUIRE(applied->token_writes.size() == 1);
    CHECK(applied->token_writes[0].key == "accent");

    // The block now carries the local writes; surrounding bytes are intact.
    auto params = read_edit_block(applied->text);
    REQUIRE(params.has_value());
    auto value_of = [&](std::string_view k) -> std::string {
        for (const auto& p : *params)
            if (p.key == k) return p.json_value;
        return "<absent>";
    };
    CHECK(value_of("shadow") == "false");
    CHECK(value_of("density") == "\"airy\"");   // updated in place (was "cozy")
    CHECK(value_of("accent") == "<absent>");    // token write did NOT leak into the block
    CHECK(applied->text.find("return render(p);") != std::string::npos);  // outside untouched
}

TEST_CASE("apply_knob with only token writes leaves the artifact unchanged", "[knobs]") {
    // A theme where "layout" IS a token makes the layout knob token-only.
    const std::vector<std::string> layout_theme = {"layout"};
    auto schema = parse_knob_schema(kSchemaJson);
    const KnobSpec* layout = find_knob(*schema, "layout");
    REQUIRE(layout != nullptr);

    auto applied = apply_knob(kArtifact, *layout, "grid", layout_theme);
    REQUIRE(applied.has_value());
    CHECK(applied->text == kArtifact);  // untouched — no block edit needed
    REQUIRE(applied->token_writes.size() == 1);
    CHECK(applied->token_writes[0].json_value == "\"grid\"");
}

TEST_CASE("apply_knob needs a block when there are local writes", "[knobs]") {
    auto schema = parse_knob_schema(kSchemaJson);
    const KnobSpec* mini = find_knob(*schema, "minimalism");
    REQUIRE(mini != nullptr);
    // No EDITMODE block to anchor the local writes -> nullopt.
    CHECK_FALSE(apply_knob("const x = 1;\n", *mini, "0.0", kThemeTokens).has_value());
}

TEST_CASE("builtin_theme_flip writes appearance locally", "[knobs]") {
    auto knob = builtin_theme_flip();
    CHECK(knob.id == "theme");
    auto effect = resolve_knob(knob, "dark", kThemeTokens);
    REQUIRE(effect.has_value());
    REQUIRE(effect->local_writes.size() == 1);
    CHECK(effect->local_writes[0].key == "appearance");
    CHECK(effect->local_writes[0].json_value == "\"dark\"");
    CHECK(effect->token_writes.empty());
}
