#include <catch2/catch_test_macros.hpp>
#include <pulp/view/widget_gallery.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/theme_presets.hpp>

using namespace pulp::view;

namespace {
Theme ink_signal(bool dark) {
    const ThemePreset* p = find_preset("ink-signal");
    REQUIRE(p != nullptr);
    return theme_from_preset(*p, dark);
}
}  // namespace

// Component gallery structure assertions run everywhere; the render assertions
// only where a screenshot provider exists (macOS in CI).
// This is the "gallery stays current, enforced" mechanism: if the gallery stops
// building or rendering, CI fails.

TEST_CASE("widget gallery builds a populated, sized board", "[view][gallery]") {
    auto root = build_widget_gallery(ink_signal(true));
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() > 20);            // many primitives present
    REQUIRE(root->bounds().width == GALLERY_WIDTH);
    REQUIRE(root->bounds().height > 600.0f);      // multi-section board
}

TEST_CASE("widget gallery renders to PNG in light and dark",
          "[view][gallery][render]") {
    // Apple renders via the native CoreGraphics path (no registered provider
    // needed); other platforms need a provider. Probe by rendering once — an
    // empty result means "no backend here", which is a SKIP, not a failure.
    const uint32_t W = static_cast<uint32_t>(GALLERY_WIDTH);
    auto probe = build_widget_gallery(ink_signal(true));
    auto first = render_to_png(*probe, W, static_cast<uint32_t>(probe->bounds().height),
                               1.0f, ScreenshotBackend::default_backend);
    if (first.empty() && !has_screenshot_provider()) {
        SKIP("no screenshot backend on this platform (render-only assertion)");
    }
    REQUIRE_FALSE(first.empty());
    auto light = build_widget_gallery(ink_signal(false));
    auto png = render_to_png(*light, W, static_cast<uint32_t>(light->bounds().height),
                             1.0f, ScreenshotBackend::default_backend);
    REQUIRE_FALSE(png.empty());
}
