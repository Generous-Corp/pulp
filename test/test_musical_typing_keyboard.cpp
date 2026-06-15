// MusicalTypingKeyboard — Ink & Signal catalog component, faithful Figma SVG
// rendered via DesignFrameView (the figma-plugin faithful-vector lane). These
// pin: the embedded SVG loads (non-empty panel), the component renders
// headlessly, and it is discoverable in the pulp::design catalog.

#include <catch2/catch_test_macros.hpp>

#include <pulp/design/design_system.hpp>
#include <pulp/view/musical_typing_keyboard.hpp>
#include <pulp/view/screenshot.hpp>

using namespace pulp::view;

TEST_CASE("MusicalTypingKeyboard loads its embedded faithful SVG", "[view][musical-typing]") {
    MusicalTypingKeyboard kbd;
    // DesignFrameView auto-detects the panel from the SVG's largest rect; a
    // non-empty panel proves the embedded base64 SVG decoded and parsed.
    REQUIRE(kbd.panel_width() > 0.0f);
    REQUIRE(kbd.panel_height() > 0.0f);
}

TEST_CASE("MusicalTypingKeyboard renders headlessly", "[view][musical-typing]") {
    MusicalTypingKeyboard kbd;
    kbd.set_bounds({0.0f, 0.0f, 900.0f, 300.0f});
    auto png = render_to_png(kbd, 900, 300, 1.0f, ScreenshotBackend::skia);
    REQUIRE(png.size() > 1000);  // a real PNG, not an empty/error buffer
}

TEST_CASE("MusicalTyping is registered in the pulp::design catalog", "[design][catalog]") {
    const auto* info = pulp::design::find("MusicalTyping");
    REQUIRE(info != nullptr);
    REQUIRE(info->native_class == "pulp::view::MusicalTypingKeyboard");
    REQUIRE(info->category == pulp::design::Category::audio);
}
