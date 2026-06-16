// test_w3c_tokens.cpp — the runtime-needed W3C token pair, tested through ONLY
// <pulp/view/w3c_tokens.hpp>. This both regression-guards parse/export and
// proves the header is self-contained (no design_import.hpp / design_ir.hpp), so
// it stays compilable when PULP_ENABLE_DESIGN_IMPORT gates the authoring cluster.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/w3c_tokens.hpp>  // intentionally the ONLY view header

#include <string>

using pulp::view::export_w3c_tokens;
using pulp::view::parse_w3c_tokens;
using pulp::view::Theme;

TEST_CASE("parse_w3c_tokens reads colors and dimensions", "[view][tokens][w3c]") {
    const std::string json = R"({
      "color": {
        "primary": { "$value": "#89b4fa", "$type": "color" },
        "bg":      { "$value": "#1e1e2e", "$type": "color" }
      },
      "dimension": {
        "spacing-md": { "$value": "8px", "$type": "dimension" }
      }
    })";

    Theme theme = parse_w3c_tokens(json);
    REQUIRE(theme.colors.count("color.primary") == 1);
    REQUIRE(theme.colors.count("color.bg") == 1);
    REQUIRE(theme.dimensions.count("dimension.spacing-md") == 1);
    REQUIRE(theme.dimensions.at("dimension.spacing-md") == 8.0f);
    // Hex parsed correctly (R of primary = 0x89).
    REQUIRE(theme.colors.at("color.primary").r8() == 0x89);
}

TEST_CASE("parse_w3c_tokens resolves aliases and simple math", "[view][tokens][w3c]") {
    const std::string json = R"({
      "spacing": {
        "base": { "$value": "4px", "$type": "dimension" },
        "lg":   { "$value": "{spacing.base} * 2", "$type": "dimension" }
      }
    })";
    Theme theme = parse_w3c_tokens(json);
    REQUIRE(theme.dimensions.at("spacing.lg") == 8.0f);
}

TEST_CASE("export_w3c_tokens round-trips through parse", "[view][tokens][w3c]") {
    Theme theme;
    theme.colors["color.accent"] = pulp::view::color_from_hex(0x12ab34);
    theme.dimensions["dimension.gap"] = 12.0f;

    const std::string json = export_w3c_tokens(theme);
    REQUIRE(json.find("\"$type\": \"color\"") != std::string::npos);

    Theme back = parse_w3c_tokens(json);
    REQUIRE(back.colors.count("color.accent") == 1);
    REQUIRE(back.colors.at("color.accent").r8() == 0x12);
    REQUIRE(back.dimensions.at("dimension.gap") == 12.0f);
}
