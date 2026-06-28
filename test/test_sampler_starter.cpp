#include <catch2/catch_test_macros.hpp>

#include <pulp/design/sampler_starter.hpp>
#include <pulp/design/design_system.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/widgets.hpp>

#include <cstdint>

using namespace pulp::design;
using namespace pulp::view;

// Sampler starter structure assertions run everywhere; the render assertion
// only where a screenshot provider exists. This is the end-to-end proof that
// the ingested design system composes into a real UI.

namespace {
// Count direct children of `root` whose dynamic type is T.
template <typename T>
int count_of(const View& root) {
    int n = 0;
    for (std::size_t i = 0; i < root.child_count(); ++i) {
        if (dynamic_cast<const T*>(root.child_at(i))) ++n;
    }
    return n;
}
}  // namespace

TEST_CASE("sampler starter builds from design-system components", "[design-system][sampler]") {
    auto root = build_sampler_starter(ink_signal_theme(true));
    REQUIRE(root != nullptr);
    REQUIRE(root->bounds().width == kSamplerWidth);
    REQUIRE(root->bounds().height == kSamplerHeight);

    // The components the catalog promises a sampler author, actually present.
    REQUIRE(count_of<Knob>(*root) == 5);          // Attack/Decay/Sustain/Release/Gain
    REQUIRE(count_of<WaveformView>(*root) == 1);
    REQUIRE(count_of<Stepper>(*root) == 1);        // voices
    REQUIRE(count_of<PanControl>(*root) == 1);     // balance
    REQUIRE(count_of<Meter>(*root) == 1);          // output
    REQUIRE(count_of<Badge>(*root) == 1);          // format chip
    REQUIRE(count_of<TextButton>(*root) == 2);     // load + play
}

TEST_CASE("sampler starter is reskinnable (light and dark resolve distinct tokens)",
          "[design-system][sampler]") {
    auto dark = build_sampler_starter(ink_signal_theme(true));
    auto light = build_sampler_starter(ink_signal_theme(false));
    auto dbg = dark->resolve_color("bg.primary", Color{});
    auto lbg = light->resolve_color("bg.primary", Color{});
    REQUIRE_FALSE(dbg == lbg);   // a theme swap restyles the panel
}

TEST_CASE("sampler starter renders to PNG", "[design-system][sampler][render]") {
    const uint32_t W = static_cast<uint32_t>(kSamplerWidth);
    const uint32_t H = static_cast<uint32_t>(kSamplerHeight);
    auto root = build_sampler_starter(ink_signal_theme(true));
    auto png = render_to_png(*root, W, H, 1.0f, ScreenshotBackend::default_backend);
    if (png.empty() && !has_screenshot_provider()) {
        SKIP("no screenshot backend on this platform (render-only assertion)");
    }
    REQUIRE_FALSE(png.empty());
}
