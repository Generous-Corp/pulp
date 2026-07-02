#include <catch2/catch_test_macros.hpp>
#include <pulp/view/widget_gallery.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/theme_presets.hpp>
#include <pulp/view/ui_components.hpp>

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

TEST_CASE("scrolling widget gallery wraps the board and clamps scroll",
          "[view][gallery]") {
    const float vw = 480.0f, vh = 600.0f;
    auto root = build_scrolling_widget_gallery(ink_signal(true), vw, vh);
    REQUIRE(root != nullptr);
    // Always a ScrollView with the full board as content — bigger than the
    // viewport in both axes, so a small host can still reach every widget.
    auto* sv = static_cast<ScrollView*>(root.get());
    REQUIRE(root->bounds().width == vw);
    REQUIRE(root->bounds().height == vh);
    REQUIRE(sv->content_size().width >= GALLERY_WIDTH);
    REQUIRE(sv->content_size().height > vh);

    // Scrolling past the end clamps to (content - viewport); it never overscrolls
    // past the content or below zero.
    sv->set_scroll(1e6f, 1e6f);
    sv->advance_animations(1.0f);
    const float max_y = sv->content_size().height - vh;
    REQUIRE(sv->scroll_y() >= 0.0f);
    REQUIRE(sv->scroll_y() <= max_y + 1.0f);
    REQUIRE(sv->scroll_y() > 0.0f);   // it did move toward the bottom
}
