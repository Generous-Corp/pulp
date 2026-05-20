// test_design_import_w3c_tokens.cpp — extracted from test_design_import.cpp
// in the 2026-05 Phase 5 (P5-3 follow-up) refactor.
//
// W3C Design Tokens module — parse_w3c_tokens / export_w3c_tokens +
// composite token shapes (typography, shadow), alias resolution, math
// expression evaluation, group $type inheritance, and round-trips
// to / from Theme. Originally lived inside test_design_import.cpp under
// the "// ── W3C Design Tokens ──" section.

#include <catch2/catch_test_macros.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::view;

// ── W3C Design Tokens ───────────────────────────────────────────────────

TEST_CASE("parse_w3c_tokens reads W3C format", "[view][import]") {
    auto json = R"({
        "color": {
            "primary": { "$value": "#89B4FA", "$type": "color" },
            "bg": { "$value": "#1E1E2E", "$type": "color" }
        },
        "spacing": {
            "md": { "$value": "8", "$type": "dimension" },
            "lg": { "$value": "16", "$type": "dimension" }
        },
        "font": {
            "family": { "$value": "Inter", "$type": "fontFamily" }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    REQUIRE(theme.colors.count("color.primary") == 1);
    REQUIRE(theme.colors["color.primary"].r8() == 0x89);
    REQUIRE(theme.colors["color.primary"].g8() == 0xB4);
    REQUIRE(theme.colors["color.primary"].b8() == 0xFA);

    REQUIRE(theme.colors.count("color.bg") == 1);
    REQUIRE(theme.colors["color.bg"].r8() == 0x1E);

    REQUIRE(theme.dimensions.count("spacing.md") == 1);
    REQUIRE(theme.dimensions["spacing.md"] == 8.0f);
    REQUIRE(theme.dimensions["spacing.lg"] == 16.0f);

    REQUIRE(theme.strings.count("font.family") == 1);
    REQUIRE(theme.strings["font.family"] == "Inter");
}

TEST_CASE("export_w3c_tokens produces W3C format", "[view][import]") {
    Theme theme;
    theme.colors["bg.primary"] = color_from_hex(0x1E1E2E);
    theme.colors["accent.primary"] = color_from_hex(0x89B4FA);
    theme.dimensions["spacing.md"] = 8.0f;
    theme.strings["font.family"] = "Inter";

    auto json = export_w3c_tokens(theme);

    REQUIRE(json.find("\"$value\"") != std::string::npos);
    REQUIRE(json.find("\"$type\": \"color\"") != std::string::npos);
    REQUIRE(json.find("\"$type\": \"dimension\"") != std::string::npos);
    REQUIRE(json.find("\"$type\": \"string\"") != std::string::npos);
    REQUIRE(json.find("#1e1e2e") != std::string::npos);
}

TEST_CASE("W3C token round-trip preserves colors", "[view][import]") {
    Theme original;
    original.colors["bg.primary"] = color_from_hex(0x1A1A2E);
    original.colors["accent.primary"] = color_from_hex(0xE94560);
    original.dimensions["spacing.md"] = 8.0f;

    auto w3c = export_w3c_tokens(original);
    auto restored = parse_w3c_tokens(w3c);

    // Colors should round-trip (names get prefixed by group)
    REQUIRE(restored.colors.count("bg.primary") == 1);
    REQUIRE(restored.colors["bg.primary"].r8() == original.colors["bg.primary"].r8());
    REQUIRE(restored.colors["bg.primary"].g8() == original.colors["bg.primary"].g8());
    REQUIRE(restored.colors["bg.primary"].b8() == original.colors["bg.primary"].b8());
}

TEST_CASE("parse_w3c_tokens resolves aliases", "[view][import]") {
    auto json = R"({
        "color": {
            "$type": "color",
            "blue": { "$value": "#3B82F6" },
            "primary": { "$value": "{color.blue}" },
            "accent": { "$value": "{color.primary}" }
        },
        "spacing": {
            "$type": "dimension",
            "base": { "$value": "8" },
            "md": { "$value": "{spacing.base}" },
            "lg": { "$value": "16" }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    // Direct values
    REQUIRE(theme.colors.count("color.blue") == 1);
    REQUIRE(theme.colors["color.blue"].r8() == 0x3B);

    // Single alias: primary → blue
    REQUIRE(theme.colors.count("color.primary") == 1);
    REQUIRE(theme.colors["color.primary"].r8() == 0x3B);
    REQUIRE(theme.colors["color.primary"].g8() == 0x82);

    // Chained alias: accent → primary → blue
    REQUIRE(theme.colors.count("color.accent") == 1);
    REQUIRE(theme.colors["color.accent"].r8() == 0x3B);

    // Dimension alias: md → base
    REQUIRE(theme.dimensions["spacing.md"] == 8.0f);
    REQUIRE(theme.dimensions["spacing.lg"] == 16.0f);
}

TEST_CASE("parse_w3c_tokens inherits group $type", "[view][import]") {
    auto json = R"({
        "color": {
            "$type": "color",
            "bg": { "$value": "#1E1E2E" },
            "text": { "$value": "#CDD6F4" }
        },
        "size": {
            "$type": "dimension",
            "sm": { "$value": "4" },
            "md": { "$value": "8" }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    // Tokens inherit $type from parent group
    REQUIRE(theme.colors.count("color.bg") == 1);
    REQUIRE(theme.colors.count("color.text") == 1);
    REQUIRE(theme.dimensions.count("size.sm") == 1);
    REQUIRE(theme.dimensions["size.sm"] == 4.0f);
    REQUIRE(theme.dimensions["size.md"] == 8.0f);
}

TEST_CASE("parse_w3c_tokens handles composite typography tokens", "[view][import]") {
    auto json = R"({
        "heading": {
            "$type": "typography",
            "$value": {
                "fontFamily": "Inter",
                "fontSize": "24",
                "fontWeight": "700",
                "lineHeight": "1.2"
            }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    REQUIRE(theme.strings.count("heading.fontFamily") == 1);
    REQUIRE(theme.strings["heading.fontFamily"] == "Inter");
    REQUIRE(theme.dimensions["heading.fontSize"] == 24.0f);
    REQUIRE(theme.dimensions["heading.fontWeight"] == 700.0f);
    REQUIRE(theme.dimensions["heading.lineHeight"] == 1.2f);
}

TEST_CASE("parse_w3c_tokens handles composite shadow tokens", "[view][import]") {
    auto json = R"({
        "shadow": {
            "$type": "shadow",
            "card": {
                "$value": {
                    "color": "#00000040",
                    "offsetX": "0",
                    "offsetY": "4",
                    "blur": "8",
                    "spread": "0"
                }
            }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    REQUIRE(theme.colors.count("shadow.card.color") == 1);
    REQUIRE(theme.dimensions["shadow.card.offsetY"] == 4.0f);
    REQUIRE(theme.dimensions["shadow.card.blur"] == 8.0f);
}

TEST_CASE("parse_w3c_tokens evaluates math expressions", "[view][import]") {
    auto json = R"({
        "spacing": {
            "$type": "dimension",
            "base": { "$value": "8" },
            "double": { "$value": "8 * 2" },
            "half": { "$value": "8 / 2" },
            "sum": { "$value": "4 + 12" }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    REQUIRE(theme.dimensions["spacing.base"] == 8.0f);
    REQUIRE(theme.dimensions["spacing.double"] == 16.0f);
    REQUIRE(theme.dimensions["spacing.half"] == 4.0f);
    REQUIRE(theme.dimensions["spacing.sum"] == 16.0f);
}

TEST_CASE("parse_w3c_tokens resolves alias then evaluates math", "[view][import]") {
    auto json = R"({
        "spacing": {
            "$type": "dimension",
            "base": { "$value": "8" },
            "lg": { "$value": "{spacing.base} * 2" }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    // {spacing.base} resolves to "8", then "8 * 2" evaluates to 16
    REQUIRE(theme.dimensions["spacing.lg"] == 16.0f);
}

TEST_CASE("token parsers cover border composites inference and alternate sources",
          "[view][import][coverage]") {
    auto w3c = R"({
        "border": {
            "$type": "border",
            "focus": {
                "$value": {
                    "color": "#ff00aa80",
                    "width": "2px",
                    "style": "dashed"
                }
            }
        },
        "misc": {
            "implicitColor": { "$value": "#00ff00" },
            "implicitNumber": { "$value": "42" },
            "implicitUnitNumber": { "$value": "3.5rem" },
            "implicitString": { "$value": "not-a-number" },
            "partialNumber": { "$value": "12pxjunk" },
            "badTypedNumber": { "$type": "number", "$value": "1e999" },
            "unknownComposite": { "$type": "custom", "$value": { "x": 1 } }
        }
    })";

    auto theme = parse_w3c_tokens(w3c);
    REQUIRE(theme.colors["border.focus.color"].a8() == 0x80);
    REQUIRE(theme.dimensions["border.focus.width"] == 2.0f);
    REQUIRE(theme.strings["border.focus.style"] == "dashed");
    REQUIRE(theme.colors["misc.implicitColor"].g8() == 0xff);
    REQUIRE(theme.dimensions["misc.implicitNumber"] == 42.0f);
    REQUIRE(theme.dimensions["misc.implicitUnitNumber"] == 3.5f);
    REQUIRE(theme.strings["misc.implicitString"] == "not-a-number");
    REQUIRE(theme.strings["misc.partialNumber"] == "12pxjunk");
    REQUIRE(theme.dimensions.count("misc.badTypedNumber") == 0);
    REQUIRE(theme.strings.count("misc.unknownComposite") == 1);

    auto figma = R"([
        { "name": "color/alpha", "type": "color", "value": "#11223344" },
        { "name": "size/ratio", "type": "number", "value": "1.25" },
        { "name": "size/unit", "type": "number", "value": "2px" },
        { "name": "size/bad", "type": "number", "value": "4remjunk" },
        { "name": "copy/title", "type": "string", "value": "Hello" },
        { "name": "inferred/color", "resolvedValue": "#abcdef" },
        { "name": "inferred/number", "resolvedValue": "3.5" },
        { "name": "inferred/bad-number", "resolvedValue": "3.5junk" },
        { "name": "inferred/string", "resolvedValue": "wide" },
        { "type": "STRING", "resolvedValue": "missing name" }
    ])";

    auto figma_theme = parse_figma_variables(figma);
    REQUIRE(figma_theme.colors["color.alpha"].a8() == 0x44);
    REQUIRE(figma_theme.dimensions["size.ratio"] == 1.25f);
    REQUIRE(figma_theme.dimensions["size.unit"] == 2.0f);
    REQUIRE(figma_theme.dimensions.count("size.bad") == 0);
    REQUIRE(figma_theme.strings["copy.title"] == "Hello");
    REQUIRE(figma_theme.colors["inferred.color"].r8() == 0xab);
    REQUIRE(figma_theme.dimensions["inferred.number"] == 3.5f);
    REQUIRE(figma_theme.strings["inferred.bad-number"] == "3.5junk");
    REQUIRE(figma_theme.strings["inferred.string"] == "wide");

    auto stitch = R"({
        "colors": { "accent": "#12345678" },
        "roundness": "full",
        "spacing": 20
    })";

    auto stitch_theme = parse_stitch_design_system(stitch);
    REQUIRE(stitch_theme.colors["color.accent"].a8() == 0x78);
    REQUIRE(stitch_theme.dimensions["roundness"] == 999.0f);
    REQUIRE(stitch_theme.dimensions["spacing.base"] == 20.0f);
}

// ── IR ↔ Theme conversion ───────────────────────────────────────────────

TEST_CASE("ir_tokens_to_theme converts token maps to Theme", "[view][import]") {
    IRTokens tokens;
    tokens.colors["bg.primary"] = "#1a1a2e";
    tokens.dimensions["spacing.md"] = 8.0f;
    tokens.strings["font.family"] = "Inter";

    auto theme = ir_tokens_to_theme(tokens);

    REQUIRE(theme.colors.count("bg.primary") == 1);
    REQUIRE(theme.colors["bg.primary"].r8() == 0x1a);
    REQUIRE(theme.dimensions["spacing.md"] == 8.0f);
    REQUIRE(theme.strings["font.family"] == "Inter");
}

TEST_CASE("theme_to_ir_tokens converts Theme to token maps", "[view][import]") {
    Theme theme;
    theme.colors["bg.primary"] = color_from_hex(0x1A1A2E);
    theme.dimensions["spacing.md"] = 8.0f;
    theme.strings["font.family"] = "Inter";

    auto tokens = theme_to_ir_tokens(theme);

    REQUIRE(tokens.colors["bg.primary"] == "#1a1a2e");
    REQUIRE(tokens.dimensions["spacing.md"] == 8.0f);
    REQUIRE(tokens.strings["font.family"] == "Inter");
}

// ── Stitch HTML parsing ─────────────────────────────────────────────────

