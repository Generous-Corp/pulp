// SPDX-License-Identifier: MIT
//
// Phase 3 codecov batch: focused coverage for the tiny header-only JSON
// parser shared by CLI registry/import-design helpers. These tests stay on
// deterministic parser/accessor behavior and avoid tying the parser to any
// one registry schema.

#include <catch2/catch_test_macros.hpp>

#include "tools/cli/json_parser.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace json = pulp::cli::pkg;

namespace {

json::JsonValue parse_json(const std::string& text) {
    json::JsonParser parser{text, 0};
    return parser.parse();
}

}  // namespace

TEST_CASE("json parser: object lookup returns matching keys and null for missing keys",
          "[cli][json-parser][coverage][phase3-large]") {
    auto value = parse_json(R"({"name":"pulp","enabled":true})");
    REQUIRE(value.type == json::JsonValue::Object);
    REQUIRE(value.get("name") != nullptr);
    CHECK(value.get("name")->as_string() == "pulp");
    CHECK(value.get("missing") == nullptr);
}

TEST_CASE("json parser: get on non-objects is null-safe",
          "[cli][json-parser][coverage][phase3-large]") {
    auto value = parse_json(R"(["name","pulp"])");
    REQUIRE(value.type == json::JsonValue::Array);
    CHECK(value.get("name") == nullptr);
}

TEST_CASE("json parser: string escapes decode the supported JSON escape set",
          "[cli][json-parser][coverage][phase3-large]") {
    auto value = parse_json(R"({"s":"quote:\" slash:\/ backslash:\\ line:\n tab:\t cr:\r"})");
    auto* s = value.get("s");
    REQUIRE(s != nullptr);
    CHECK(s->as_string().find("quote:\"") != std::string::npos);
    CHECK(s->as_string().find("slash:/") != std::string::npos);
    CHECK(s->as_string().find("backslash:\\") != std::string::npos);
    CHECK(s->as_string().find("line:\n") != std::string::npos);
    CHECK(s->as_string().find("tab:\t") != std::string::npos);
    CHECK(s->as_string().find("cr:\r") != std::string::npos);
}

TEST_CASE("json parser: unicode escapes advance and use placeholder text",
          "[cli][json-parser][coverage][phase3-large]") {
    auto value = parse_json(R"({"symbol":"A\u2665B","after":"ok"})");
    auto* symbol = value.get("symbol");
    REQUIRE(symbol != nullptr);
    CHECK(symbol->as_string() == "A?B");
    REQUIRE(value.get("after") != nullptr);
    CHECK(value.get("after")->as_string() == "ok");
}

TEST_CASE("json parser: arrays preserve order across mixed values",
          "[cli][json-parser][coverage][phase3-large]") {
    auto value = parse_json(R"(["first",2,false,null,{"k":"v"}])");
    REQUIRE(value.type == json::JsonValue::Array);
    REQUIRE(value.arr().size() == 5);
    CHECK(value.arr()[0].as_string() == "first");
    CHECK(value.arr()[1].as_int() == 2);
    CHECK_FALSE(value.arr()[2].as_bool());
    CHECK(value.arr()[3].type == json::JsonValue::Null);
    REQUIRE(value.arr()[4].get("k") != nullptr);
    CHECK(value.arr()[4].get("k")->as_string() == "v");
}

TEST_CASE("json parser: empty object and array keep empty accessors stable",
          "[cli][json-parser][coverage][phase3-large]") {
    auto object = parse_json("{}");
    auto array = parse_json("[]");
    REQUIRE(object.type == json::JsonValue::Object);
    REQUIRE(array.type == json::JsonValue::Array);
    CHECK(object.obj().empty());
    CHECK(array.arr().empty());
}

TEST_CASE("json parser: numeric forms feed integer and floating accessors",
          "[cli][json-parser][coverage][phase3-large]") {
    auto value = parse_json(R"({"neg":-42,"frac":3.5,"exp":1.25e2})");
    REQUIRE(value.get("neg") != nullptr);
    REQUIRE(value.get("frac") != nullptr);
    REQUIRE(value.get("exp") != nullptr);
    CHECK(value.get("neg")->as_int() == -42);
    CHECK(std::fabs(value.get("frac")->num_val - 3.5) < 0.0001);
    CHECK(value.get("exp")->as_int() == 125);
}

TEST_CASE("json parser: string array accessor filters non-string entries",
          "[cli][json-parser][coverage][phase3-large]") {
    auto value = parse_json(R"(["alpha",1,"beta",true,{"ignored":"yes"},"gamma"])");
    CHECK(value.as_string_array() == std::vector<std::string>{"alpha", "beta", "gamma"});
}

TEST_CASE("json parser: scalar accessors return defaults for mismatched types",
          "[cli][json-parser][coverage][phase3-large]") {
    auto value = parse_json(R"({"s":"42","n":7,"b":true,"f":false,"nil":null})");
    CHECK(value.get("n")->as_string().empty());
    CHECK(value.get("s")->as_int() == 0);
    CHECK(value.get("b")->as_bool());
    CHECK_FALSE(value.get("f")->as_bool());
    CHECK(value.get("nil")->as_int() == 0);
}

TEST_CASE("json parser: nested object paths remain readable through get",
          "[cli][json-parser][coverage][phase3-large]") {
    auto value = parse_json(R"({"outer":{"inner":{"leaf":"value"}}})");
    auto* outer = value.get("outer");
    REQUIRE(outer != nullptr);
    auto* inner = outer->get("inner");
    REQUIRE(inner != nullptr);
    auto* leaf = inner->get("leaf");
    REQUIRE(leaf != nullptr);
    CHECK(leaf->as_string() == "value");
}

TEST_CASE("json parser: whitespace is accepted around tokens",
          "[cli][json-parser][coverage][phase3-large]") {
    auto value = parse_json(" \n\t { \r\n \"k\" \t : \n [ true , false ] \n } ");
    auto* k = value.get("k");
    REQUIRE(k != nullptr);
    REQUIRE(k->arr().size() == 2);
    CHECK(k->arr()[0].as_bool());
    CHECK_FALSE(k->arr()[1].as_bool());
}

TEST_CASE("json parser: invalid number tokens become zero-valued numbers",
          "[cli][json-parser][coverage][phase3-large]") {
    auto value = parse_json("-");
    CHECK(value.type == json::JsonValue::Number);
    CHECK(value.as_int() == 0);
}
