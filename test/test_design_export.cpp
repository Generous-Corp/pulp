#include <catch2/catch_test_macros.hpp>
#include <pulp/view/design_export.hpp>

using namespace pulp::view;
using namespace pulp::canvas;

static Theme make_test_theme() {
    Theme t;
    t.colors["background"] = Color::hex(0x1a1a2e);
    t.colors["accent"] = Color::hex(0xe94560);
    t.dimensions["knob_size"] = 60.0f;
    t.dimensions["padding"] = 8.0f;
    t.strings["font_family"] = "Inter";
    return t;
}

TEST_CASE("DesignExport to_json contains colors and dimensions", "[view][export]") {
    auto theme = make_test_theme();
    auto json = DesignExport::to_json(theme);

    REQUIRE(json.find("\"colors\"") != std::string::npos);
    REQUIRE(json.find("\"background\"") != std::string::npos);
    REQUIRE(json.find("\"accent\"") != std::string::npos);
    REQUIRE(json.find("\"dimensions\"") != std::string::npos);
    REQUIRE(json.find("\"knob_size\"") != std::string::npos);
    REQUIRE(json.find("\"strings\"") != std::string::npos);
    REQUIRE(json.find("\"font_family\"") != std::string::npos);
}

TEST_CASE("DesignExport to_css produces CSS custom properties", "[view][export]") {
    auto theme = make_test_theme();
    auto css = DesignExport::to_css(theme, "pulp");

    REQUIRE(css.find(":root {") != std::string::npos);
    REQUIRE(css.find("--pulp-background") != std::string::npos);
    REQUIRE(css.find("--pulp-accent") != std::string::npos);
    REQUIRE(css.find("--pulp-knob_size") != std::string::npos);
    REQUIRE(css.find("px;") != std::string::npos);
}

TEST_CASE("DesignExport to_css uses custom prefix", "[view][export]") {
    auto theme = make_test_theme();
    auto css = DesignExport::to_css(theme, "my-plugin");

    REQUIRE(css.find("--my-plugin-background") != std::string::npos);
}

TEST_CASE("DesignExport to_cpp_header produces valid C++", "[view][export]") {
    auto theme = make_test_theme();
    auto cpp = DesignExport::to_cpp_header(theme, "dark_theme");

    REQUIRE(cpp.find("#pragma once") != std::string::npos);
    REQUIRE(cpp.find("namespace dark_theme {") != std::string::npos);
    REQUIRE(cpp.find("constexpr uint32_t k") != std::string::npos);
    REQUIRE(cpp.find("constexpr float k") != std::string::npos);
    REQUIRE(cpp.find("0x") != std::string::npos); // hex color
}

TEST_CASE("DesignExport to_oklch_css produces OKLCH values", "[view][export]") {
    auto theme = make_test_theme();
    auto css = DesignExport::to_oklch_css(theme);

    REQUIRE(css.find("oklch(") != std::string::npos);
    REQUIRE(css.find("--pulp-background") != std::string::npos);
}

TEST_CASE("DesignExport to_wgsl_uniforms produces WGSL struct", "[view][export]") {
    auto theme = make_test_theme();
    auto wgsl = DesignExport::to_wgsl_uniforms(theme, "ThemeData");

    REQUIRE(wgsl.find("struct ThemeData {") != std::string::npos);
    REQUIRE(wgsl.find("vec4<f32>") != std::string::npos);
    REQUIRE(wgsl.find("f32") != std::string::npos);
    REQUIRE(wgsl.find("};") != std::string::npos);
}
