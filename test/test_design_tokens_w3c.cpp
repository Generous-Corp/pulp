// W3C Design Tokens (DTCG) emitter for IRTokens
// (core/view/src/design_tokens_w3c.cpp). Every case round-trips the emitted
// text through choc::json::parse, so shape assertions double as
// valid-JSON proof.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/view/design_tokens_w3c.hpp>

#include <choc/text/choc_JSON.h>

using pulp::view::IRTokenIdentity;
using pulp::view::IRTokens;
using pulp::view::to_w3c_tokens_json;

static choc::value::Value emit_and_parse(const IRTokens& tokens, bool pretty = true) {
    auto json = to_w3c_tokens_json(tokens, pretty);
    return choc::json::parse(json);  // throws on invalid JSON
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

TEST_CASE("string token emits $type string", "[design-tokens][w3c][import-design]") {
    IRTokens tokens;
    tokens.strings["font-family"] = "Inter \"Display\"\n";

    auto root = emit_and_parse(tokens);
    auto token = root["strings"]["font-family"];
    CHECK(std::string(token["$type"].toString()) == "string");
    // Quotes and the newline must survive escaping + reparse byte-for-byte.
    CHECK(std::string(token["$value"].toString()) == "Inter \"Display\"\n");
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
