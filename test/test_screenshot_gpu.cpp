// P1 — smart/unified view capture (capture_view) + offscreen-GPU path.
// Proves the "always-capturable" contract: a native overlay is refused with a
// reason, a blank/clear-only frame fails the content floor, and a real widget
// tree captures non-blank.
#include <catch2/catch_test_macros.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/canvas/canvas.hpp>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::view;

namespace {
// A view that owns a "native overlay" and returns a caller-supplied PNG from its
// in-process snapshot hook — the stand-in for a real WebView pane (whose
// capture_native_overlay_png forwards WKWebView takeSnapshot). Empty bytes model
// "no in-process snapshot available".
class SnapshotOverlayView : public View {
public:
    explicit SnapshotOverlayView(std::vector<uint8_t> png) : png_(std::move(png)) {
        set_contains_native_overlay(true);
    }
    std::vector<uint8_t> capture_native_overlay_png(uint32_t, uint32_t) override {
        return png_;
    }
private:
    std::vector<uint8_t> png_;
};
}  // namespace

TEST_CASE("capture_view refuses a native-overlay subtree with a reason",
          "[view][screenshot][gpu]") {
    View root;
    auto pane = std::make_unique<View>();
    pane->set_contains_native_overlay(true);  // e.g. a WebView pane
    root.add_child(std::move(pane));

    const CaptureResult r = capture_view(root, 200, 120, 1.0f);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.reason.find("overlay") != std::string::npos);  // honest, not a blank
}

TEST_CASE("capture_view rejects a blank / clear-only frame (content floor)",
          "[view][screenshot][gpu]") {
    View root;  // nothing painted but the background fill → 1 unique color
    const CaptureResult r = capture_view(root, 200, 120, 1.0f);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.reason.find("blank") != std::string::npos);
}

// A reliably content-rich tree: a diagonal gradient background (hundreds of
// interpolated colors at full, opaque coverage — clears the content floor
// regardless of child flex layout) plus a Label. Exercises paint_all → the
// capture canvas → the content floor with real painted content.
static std::unique_ptr<View> make_capture_tree() {
    auto root = std::make_unique<View>();
    root->set_theme(Theme::dark());
    root->set_background_gradient_linear(
        0.0f, 0.0f, 1.0f, 1.0f,
        {pulp::canvas::Color::rgba8(28, 31, 46), pulp::canvas::Color::rgba8(96, 150, 240),
         pulp::canvas::Color::rgba8(168, 140, 250)},
        {0.0f, 0.55f, 1.0f});
    auto label = std::make_unique<Label>("Promptable Accompanist");
    label->set_bounds({16.0f, 80.0f, 480.0f, 32.0f});
    root->add_child(std::move(label));
    return root;
}

// A real but SPARSE UI: a solid background with one small label covering well
// under 5% of the frame. This is the regression guard for the content-floor
// parameter-position bug — passes_content_floor's signature is
// (min_unique_colors, min_luminance_stddev, min_non_background_coverage,
// min_opaque_coverage). A 3-argument call silently left min_non_background_coverage
// at its strict 0.05 default, so this sparse-but-valid UI was wrongly rejected as
// "blank". capture_view must accept it.
TEST_CASE("capture_view accepts a sparse-but-real UI (content-floor leniency)",
          "[view][screenshot][gpu]") {
    // Solid dark background + one small painted element covering ~1% of the frame
    // (well under the strict 5% non-background default). A gradient fill — not text
    // — so the assertion holds on raster backends that don't render glyphs.
    View root;
    root.set_theme(Theme::dark());
    auto chip = std::make_unique<View>();
    // Size the chip through the flex layout, not set_bounds: capture_view runs
    // layout_children() (Yoga), which computes child bounds from flex styles and
    // overrides any set_bounds on a child — so a set_bounds chip lays out to 0x0
    // and never paints, making the frame genuinely blank (the failure this test
    // was hitting). An explicit preferred width/height survives the layout pass.
    // 80x40 = 3200 / 240000 ≈ 1.3% of 600x400 — well under the strict 5%
    // non-background floor, so this still guards the content-floor leniency.
    chip->flex().preferred_width = 80.0f;
    chip->flex().preferred_height = 40.0f;
    chip->set_background_gradient_linear(
        0.0f, 0.0f, 1.0f, 1.0f,
        {pulp::canvas::Color::rgba8(240, 120, 60), pulp::canvas::Color::rgba8(80, 200, 250)},
        {0.0f, 1.0f});
    root.add_child(std::move(chip));

    const CaptureResult r = capture_view(root, 600, 400, 1.0f);
    if (r.png.empty() || r.used != ScreenshotBackend::skia) {
        // The CoreGraphics raster path renders root-level fills but not child
        // sub-element gradients, so the sparse-coverage distinction is only
        // observable on the Skia backend. Skip elsewhere rather than assert a
        // backend limitation.
        SUCCEED("sparse-content floor is only observable on the Skia raster backend");
        return;
    }
    INFO("sparse capture reason: " << r.reason);
    REQUIRE(r.ok);  // false under the buggy 3-arg call (strict 5% non-background floor)
}

TEST_CASE("capture_view passes a non-blank widget tree (raster)", "[view][screenshot][gpu]") {
    auto root = make_capture_tree();
    const CaptureResult r = capture_view(*root, 520, 200, 1.0f);
    INFO("capture_view reason: " << r.reason << " (backend " << static_cast<int>(r.used) << ")");
    REQUIRE(r.ok);
    REQUIRE_FALSE(r.png.empty());
    REQUIRE(r.png.size() > 8);
    REQUIRE(r.png[1] == 'P');  // PNG magic "\x89PNG"
}

TEST_CASE("capture_view returns a non-blank native-overlay snapshot instead of refusing",
          "[view][screenshot][gpu]") {
    // A real WebView pane returns an in-process snapshot (WKWebView takeSnapshot).
    // Model it with content-rich PNG bytes produced by the raster path. capture_view
    // must surface that snapshot (used == default_backend) rather than refuse.
    auto content = make_capture_tree();
    auto png = render_to_png(*content, 520, 200, 1.0f, ScreenshotBackend::skia);
    if (png.empty()) {
        SUCCEED("no raster backend in this build — cannot synthesize an overlay snapshot");
        return;
    }
    View root;
    root.add_child(std::make_unique<SnapshotOverlayView>(std::move(png)));

    const CaptureResult r = capture_view(root, 520, 200, 1.0f);
    INFO("overlay-snapshot reason: " << r.reason);
    REQUIRE(r.ok);
    REQUIRE(r.used == ScreenshotBackend::default_backend);  // native-overlay snapshot
    REQUIRE_FALSE(r.png.empty());
    REQUIRE(r.png[1] == 'P');
}

TEST_CASE("capture_view refuses an essentially-blank native-overlay snapshot",
          "[view][screenshot][gpu]") {
    // A WebView that snapshots to a single flat color (e.g. not yet loaded) must
    // be refused with a "blank" reason, not silently accepted.
    View flat;  // nothing painted but the background fill → one color
    auto blank_png = render_to_png(flat, 64, 64, 1.0f, ScreenshotBackend::skia);
    if (blank_png.empty()) {
        SUCCEED("no raster backend in this build — cannot synthesize a blank snapshot");
        return;
    }
    View root;
    root.add_child(std::make_unique<SnapshotOverlayView>(std::move(blank_png)));

    const CaptureResult r = capture_view(root, 64, 64, 1.0f);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.reason.find("blank") != std::string::npos);
}

TEST_CASE("capture_view routes a requires_gpu_host tree to the GPU backend",
          "[view][screenshot][gpu]") {
    if (!has_gpu_capture()) {
        SUCCEED("GPU capture not compiled in (no pulp-render)");
        return;
    }
    auto root = make_capture_tree();
    root->set_requires_gpu_host(true);  // forces the GPU (HeadlessSurface) path

    const CaptureResult r = capture_view(*root, 520, 200, 1.0f);
    INFO("gpu capture_view reason: " << r.reason);
    // The GPU surface may be absent on a headless CI adapter; if it produced a
    // frame it must pass the content floor (proving the GPU path paints content).
    if (!r.png.empty()) {
        REQUIRE(r.used == ScreenshotBackend::gpu);
        REQUIRE(r.ok);
        REQUIRE(r.png[1] == 'P');
    }
}

// Hidden demo (run with the [.demo] tag): writes a real GPU-captured PNG to disk
// so the capture pipeline can be eyeballed. Not part of the default suite.
TEST_CASE("demo: write a GPU-captured PNG to disk", "[.][demo]") {
    auto root = make_capture_tree();
    root->set_requires_gpu_host(true);
    const CaptureResult r = capture_view(*root, 520, 200, 2.0f);
    INFO("reason: " << r.reason);
    REQUIRE(r.ok);
    const char* out = std::getenv("P1_DEMO_PNG");
    std::ofstream f(out ? out : "/tmp/p1-gpu-capture.png", std::ios::binary);
    f.write(reinterpret_cast<const char*>(r.png.data()),
            static_cast<std::streamsize>(r.png.size()));
}
