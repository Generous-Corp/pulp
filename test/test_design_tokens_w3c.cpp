// W3C Design Tokens (DTCG) emitter + validator for IRTokens
// (core/view/src/design_tokens_w3c.cpp). Every case round-trips the emitted
// text through choc::json::parse, so shape assertions double as
// valid-JSON proof. String policy under test: confident font-family names
// promote to $type "fontFamily"; every other string is parked losslessly
// under the document-root $extensions["dev.pulp.nonStandardTokens"]; no
// token ever carries a non-standard $type.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/view/design_tokens_w3c.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <string>
#include <vector>

using pulp::view::IRTokenIdentity;
using pulp::view::IRTokens;
using pulp::view::to_w3c_tokens_json;
using pulp::view::validate_dtcg;

static choc::value::Value emit_and_parse(const IRTokens& tokens, bool pretty = true) {
    auto json = to_w3c_tokens_json(tokens, pretty);
    return choc::json::parse(json);  // throws on invalid JSON
}

static bool mentions(const std::vector<std::string>& violations, const std::string& needle) {
    return std::any_of(violations.begin(), violations.end(), [&](const std::string& v) {
        return v.find(needle) != std::string::npos;
    });
}

TEST_CASE("color token emits DTCG $type/$value", "[design-tokens][w3c][import-design]") {
    IRTokens tokens;
    tokens.colors["primary"] = "#ff8800";

    auto root = emit_and_parse(tokens);
    auto token = root["colors"]["primary"];
    CHECK(std::string(token["$type"].toString()) == "color");
    CHECK(std::string(token["$value"].toString()) == "#ff8800");
    CHECK_FALSE(token.hasObjectMember("$extensions"));
}

TEST_CASE("dimension token emits the DTCG value/unit object", "[design-tokens][w3c][import-design]") {
    IRTokens tokens;
    tokens.dimensions["spacing"] = 12.5f;

    auto root = emit_and_parse(tokens);
    auto token = root["dimensions"]["spacing"];
    CHECK(std::string(token["$type"].toString()) == "dimension");
    REQUIRE(token["$value"].isObject());
    CHECK_THAT(token["$value"]["value"].getWithDefault<double>(0.0),
               Catch::Matchers::WithinAbs(12.5, 1e-6));
    CHECK(std::string(token["$value"]["unit"].toString()) == "px");
}

TEST_CASE("font-family names promote to $type fontFamily", "[design-tokens][w3c][import-design]") {
    IRTokens tokens;
    tokens.strings["font-family"] = "Inter \"Display\"\n";
    tokens.strings["typography/body/fontFamily"] = "Plus Jakarta Sans";
    tokens.strings["font.mono"] = "JetBrains Mono";  // "." segments count too
    tokens.strings["heading/typeface"] = "Georgia";

    auto root = emit_and_parse(tokens);
    auto direct = root["strings"]["font-family"];
    CHECK(std::string(direct["$type"].toString()) == "fontFamily");
    // Quotes and the newline must survive escaping + reparse byte-for-byte.
    CHECK(std::string(direct["$value"].toString()) == "Inter \"Display\"\n");
    CHECK(std::string(root["strings"]["typography"]["body"]["fontFamily"]["$type"].toString())
          == "fontFamily");
    CHECK(std::string(root["strings"]["font.mono"]["$value"].toString()) == "JetBrains Mono");
    CHECK(std::string(root["strings"]["heading"]["typeface"]["$value"].toString()) == "Georgia");
    // Nothing here was parked.
    CHECK_FALSE(root.hasObjectMember("$extensions"));
}

TEST_CASE("comma-separated font stack emits the DTCG array form", "[design-tokens][w3c][import-design]") {
    IRTokens tokens;
    tokens.strings["font/family"] = "Inter, SF Pro Text ,sans-serif";

    auto root = emit_and_parse(tokens);
    auto value = root["strings"]["font"]["family"]["$value"];
    REQUIRE(value.isArray());
    REQUIRE(value.size() == 3);
    CHECK(std::string(value[0].toString()) == "Inter");
    CHECK(std::string(value[1].toString()) == "SF Pro Text");  // trimmed
    CHECK(std::string(value[2].toString()) == "sans-serif");
}

TEST_CASE("non-font strings park under root $extensions, never dropped",
          "[design-tokens][w3c][import-design]") {
    IRTokens tokens;
    tokens.strings["motion.easing.enter"] = "ease_out_quad";
    tokens.strings["hero/title"] = "Welcome";
    IRTokenIdentity id;
    id.source_id = "VariableID:7:7";
    id.source_adapter = "figma-plugin";
    tokens.source_identity["strings.motion.easing.enter"] = id;

    auto root = emit_and_parse(tokens);
    // No fake token groups: nothing promoted, so no "strings" group at all.
    CHECK_FALSE(root.hasObjectMember("strings"));
    auto bucket = root["$extensions"]["dev.pulp.nonStandardTokens"];
    REQUIRE(bucket.isObject());
    auto easing = bucket["motion.easing.enter"];
    CHECK(std::string(easing["value"].toString()) == "ease_out_quad");
    CHECK(std::string(easing["id"].toString()) == "VariableID:7:7");
    CHECK(std::string(easing["adapter"].toString()) == "figma-plugin");
    CHECK_FALSE(easing.hasObjectMember("collection"));  // empty subfields omitted
    CHECK(std::string(bucket["hero/title"]["value"].toString()) == "Welcome");
}

TEST_CASE("mixed strings split into fontFamily tokens and parked extras",
          "[design-tokens][w3c][import-design]") {
    IRTokens tokens;
    tokens.strings["font.family"] = "Inter";
    tokens.strings["motion.easing.exit"] = "ease_in_quad";

    auto root = emit_and_parse(tokens);
    CHECK(std::string(root["strings"]["font.family"]["$type"].toString()) == "fontFamily");
    CHECK_FALSE(root["strings"].hasObjectMember("motion.easing.exit"));
    CHECK(std::string(root["$extensions"]["dev.pulp.nonStandardTokens"]
                          ["motion.easing.exit"]["value"].toString())
          == "ease_in_quad");
}

TEST_CASE("slash-separated names nest into DTCG groups", "[design-tokens][w3c][import-design]") {
    IRTokens tokens;
    tokens.colors["brand/primary"] = "#112233";
    tokens.colors["brand/accent/hover"] = "#445566";

    auto root = emit_and_parse(tokens);
    auto brand = root["colors"]["brand"];
    REQUIRE(brand.isObject());
    CHECK(std::string(brand["primary"]["$value"].toString()) == "#112233");
    CHECK(std::string(brand["accent"]["hover"]["$value"].toString()) == "#445566");
    // The group node itself is not a token.
    CHECK_FALSE(brand.hasObjectMember("$value"));
}

TEST_CASE("source_identity attaches dev.pulp.source under $extensions", "[design-tokens][w3c][import-design]") {
    IRTokens tokens;
    tokens.colors["brand/primary"] = "#112233";
    IRTokenIdentity id;
    id.source_id = "VariableID:1:23";
    id.source_collection = "Semantic";
    id.source_mode = "Dark";
    id.source_adapter = "figma-plugin";
    tokens.source_identity["colors.brand/primary"] = id;

    auto root = emit_and_parse(tokens);
    auto source = root["colors"]["brand"]["primary"]["$extensions"]["dev.pulp.source"];
    REQUIRE(source.isObject());
    CHECK(std::string(source["id"].toString()) == "VariableID:1:23");
    CHECK(std::string(source["collection"].toString()) == "Semantic");
    CHECK(std::string(source["mode"].toString()) == "Dark");
    CHECK(std::string(source["adapter"].toString()) == "figma-plugin");
}

TEST_CASE("empty identity subfields are omitted", "[design-tokens][w3c][import-design]") {
    IRTokens tokens;
    tokens.dimensions["gap"] = 8.0f;
    IRTokenIdentity id;
    id.source_id = "VariableID:9:9";  // collection/mode/adapter left empty
    tokens.source_identity["dimensions.gap"] = id;

    auto root = emit_and_parse(tokens);
    auto source = root["dimensions"]["gap"]["$extensions"]["dev.pulp.source"];
    CHECK(std::string(source["id"].toString()) == "VariableID:9:9");
    CHECK_FALSE(source.hasObjectMember("collection"));
    CHECK_FALSE(source.hasObjectMember("mode"));
    CHECK_FALSE(source.hasObjectMember("adapter"));
}

TEST_CASE("empty IRTokens emit an empty object and empty groups are omitted", "[design-tokens][w3c][import-design]") {
    auto root = emit_and_parse(IRTokens{});
    REQUIRE(root.isObject());
    CHECK(root.size() == 0);

    IRTokens colors_only;
    colors_only.colors["x"] = "#000000";
    auto partial = emit_and_parse(colors_only);
    CHECK(partial.hasObjectMember("colors"));
    CHECK_FALSE(partial.hasObjectMember("dimensions"));
    CHECK_FALSE(partial.hasObjectMember("strings"));
}

TEST_CASE("a name that is both token and group prefix merges", "[design-tokens][w3c][import-design]") {
    IRTokens tokens;
    tokens.colors["brand"] = "#101010";
    tokens.colors["brand/primary"] = "#202020";

    auto root = emit_and_parse(tokens);
    auto brand = root["colors"]["brand"];
    // One object carries the token's $ members AND the child token.
    CHECK(std::string(brand["$type"].toString()) == "color");
    CHECK(std::string(brand["$value"].toString()) == "#101010");
    CHECK(std::string(brand["primary"]["$value"].toString()) == "#202020");
}

TEST_CASE("degenerate separator-only names keep their raw spelling", "[design-tokens][w3c][import-design]") {
    IRTokens tokens;
    tokens.colors["//"] = "#303030";
    tokens.colors["a//b"] = "#404040";  // doubled separator: empty segment dropped

    auto root = emit_and_parse(tokens);
    CHECK(std::string(root["colors"]["//"]["$value"].toString()) == "#303030");
    CHECK(std::string(root["colors"]["a"]["b"]["$value"].toString()) == "#404040");
}

TEST_CASE("output parses as JSON in both pretty and compact modes", "[design-tokens][w3c][import-design]") {
    IRTokens tokens;
    tokens.colors["brand/primary"] = "#112233";
    tokens.dimensions["spacing/sm"] = 4.0f;
    tokens.strings["title"] = "hello";

    // emit_and_parse throws on invalid JSON; both modes must survive it and
    // agree on content.
    auto pretty = emit_and_parse(tokens, true);
    auto compact = emit_and_parse(tokens, false);
    CHECK(choc::json::toString(pretty) == choc::json::toString(compact));

    // Deterministic output across calls despite unordered_map sources.
    CHECK(to_w3c_tokens_json(tokens) == to_w3c_tokens_json(tokens));
}

// ── validate_dtcg ───────────────────────────────────────────────────────

TEST_CASE("validate_dtcg passes all emitter output", "[design-tokens][w3c][import-design]") {
    IRTokens tokens;
    tokens.colors["brand"] = "#101010";           // merge: token + group prefix
    tokens.colors["brand/primary"] = "#112233";
    tokens.dimensions["spacing/sm"] = 4.0f;
    tokens.strings["font/family"] = "Inter, sans-serif";  // fontFamily array form
    tokens.strings["font.mono"] = "JetBrains Mono";       // fontFamily plain
    tokens.strings["motion.easing.enter"] = "ease_out_quad";  // parked
    tokens.strings["hero/title"] = "Welcome";                 // parked
    IRTokenIdentity id;
    id.source_id = "VariableID:1:1";
    id.source_adapter = "figma-plugin";
    tokens.source_identity["colors.brand/primary"] = id;
    tokens.source_identity["strings.motion.easing.enter"] = id;

    auto violations = validate_dtcg(to_w3c_tokens_json(tokens));
    for (const auto& v : violations) UNSCOPED_INFO(v);
    CHECK(violations.empty());

    CHECK(validate_dtcg(to_w3c_tokens_json(IRTokens{})).empty());
    CHECK(validate_dtcg(to_w3c_tokens_json(tokens, false)).empty());
}

TEST_CASE("validate_dtcg accepts inherited group $type", "[design-tokens][w3c][import-design]") {
    auto violations = validate_dtcg(R"({
      "palette": {"$type": "color", "primary": {"$value": "#112233"}}
    })");
    for (const auto& v : violations) UNSCOPED_INFO(v);
    CHECK(violations.empty());
}

TEST_CASE("validate_dtcg reports a token with no resolvable $type", "[design-tokens][w3c][import-design]") {
    auto violations = validate_dtcg(R"({"a": {"$value": "#ffffff"}})");
    REQUIRE_FALSE(violations.empty());
    CHECK(mentions(violations, "no resolvable $type"));
    CHECK(mentions(violations, "'a'"));
}

TEST_CASE("validate_dtcg reports a non-standard $type", "[design-tokens][w3c][import-design]") {
    auto violations = validate_dtcg(R"({"a": {"$type": "string", "$value": "x"}})");
    REQUIRE_FALSE(violations.empty());
    CHECK(mentions(violations, "non-standard $type 'string'"));
}

TEST_CASE("validate_dtcg reports unknown reserved $-keys", "[design-tokens][w3c][import-design]") {
    auto violations = validate_dtcg(R"({
      "g": {"$foo": 1, "a": {"$type": "color", "$value": "#ffffff"}}
    })");
    REQUIRE_FALSE(violations.empty());
    CHECK(mentions(violations, "unknown reserved key '$foo'"));
}

TEST_CASE("validate_dtcg reports a dimension $value that is a bare string", "[design-tokens][w3c][import-design]") {
    auto violations = validate_dtcg(R"({"d": {"$type": "dimension", "$value": "12px"}})");
    REQUIRE_FALSE(violations.empty());
    CHECK(mentions(violations, "dimension $value must be an object"));

    auto partial = validate_dtcg(R"({"d": {"$type": "dimension", "$value": {"value": "12", "unit": 4}}})");
    CHECK(mentions(partial, "$value.value must be a number"));
    CHECK(mentions(partial, "$value.unit must be a string"));
}

TEST_CASE("validate_dtcg checks color and fontFamily value shapes", "[design-tokens][w3c][import-design]") {
    CHECK(mentions(validate_dtcg(R"({"c": {"$type": "color", "$value": 7}})"),
                   "color $value must be a string"));
    CHECK(mentions(validate_dtcg(R"({"f": {"$type": "fontFamily", "$value": 7}})"),
                   "fontFamily $value must be a string or array of strings"));
    CHECK(mentions(validate_dtcg(R"({"f": {"$type": "fontFamily", "$value": ["Inter", 3]}})"),
                   "fontFamily $value array must contain only strings"));
    CHECK(validate_dtcg(R"({"f": {"$type": "fontFamily", "$value": ["Inter", "sans-serif"]}})")
              .empty());
}

TEST_CASE("validate_dtcg checks $extensions shape and namespacing", "[design-tokens][w3c][import-design]") {
    CHECK(mentions(validate_dtcg(R"({"g": {"$extensions": 4}})"),
                   "$extensions must be an object"));
    CHECK(mentions(validate_dtcg(R"({"g": {"$extensions": {"pulp": {}}}})"),
                   "not namespaced"));
    CHECK(validate_dtcg(R"({"g": {"$extensions": {"dev.pulp.source": {}}}})").empty());
}

TEST_CASE("validate_dtcg reports non-object members and bad input", "[design-tokens][w3c][import-design]") {
    CHECK(mentions(validate_dtcg(R"({"g": {"a": 5}})"),
                   "neither a token nor a group"));
    CHECK(mentions(validate_dtcg("not json at all"), "invalid JSON"));
    CHECK(mentions(validate_dtcg(R"([1, 2, 3])"), "root must be an object"));
}
