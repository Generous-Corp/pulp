// Value-driven silhouette fill (design-import shape-fill — item 3).
//
// ImageView::set_fill_value() overlays a color from the bottom up to `value`
// of the height, masked to the image's own alpha via the canvas url() mask
// (SkiaCanvas::save_layer_with_mask). This exercises the full path on the Skia
// raster backend (no GPU window needed): set_fill_value -> save_layer_with_mask
// "url(<file>)" -> fill_rect -> restore. We assert the fill visibly changes the
// render and scales with the value.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/widgets.hpp>

#include <cstdlib>
#include <memory>
#include <string>

using namespace pulp::view;

namespace {

std::string tmp_path(const char* name) {
    const char* dir = std::getenv("TMPDIR");
    std::string base = dir && *dir ? dir : "/tmp/";
    if (base.back() != '/') base.push_back('/');
    return base + name;
}

// Render a plain opaque box to a PNG to use as the ImageView source. The fill
// masks to the image's alpha; an opaque box gives a full-box silhouette, which
// is enough to prove the fill overlays and scales (alpha-shaped clipping is the
// Skia mask's job, shared with the gradient-mask path).
std::string make_source_image() {
    auto box = std::make_unique<View>();
    box->set_background_color(Color::rgba8(210, 90, 90));
    std::string path = tmp_path("pulp-fill-source.png");
    render_to_file(*box, 48, 48, path, 1.0f, ScreenshotBackend::skia);
    return path;
}

std::vector<uint8_t> render_fill(const std::string& img, float value,
                                 const std::string& dump = "") {
    auto view = std::make_unique<ImageView>();
    view->set_image_path(img);
    if (value >= 0.0f) {
        view->set_fill_value(value);
        view->set_fill_color(Color::rgba(0.2f, 0.45f, 1.0f, 0.85f));
    }
    if (!dump.empty())
        render_to_file(*view, 48, 48, dump, 1.0f, ScreenshotBackend::skia);
    return render_to_png(*view, 48, 48, 1.0f, ScreenshotBackend::skia);
}

}  // namespace

TEST_CASE("ImageView value-driven silhouette fill scales with value",
          "[view][image][fill]") {
    const std::string img = make_source_image();
    auto base = render_fill(img, -1.0f);  // fill disabled
    if (base.empty()) {
        SKIP("Skia raster screenshot backend unavailable on this platform");
    }

    auto half = render_fill(img, 0.5f, tmp_path("pulp-fill-half.png"));
    auto full = render_fill(img, 1.0f, tmp_path("pulp-fill-full.png"));
    REQUIRE_FALSE(half.empty());
    REQUIRE_FALSE(full.empty());

    // The fill must visibly change the render vs. the un-filled baseline...
    const auto base_vs_half = compare_screenshots(base, half);
    const auto base_vs_full = compare_screenshots(base, full);
    REQUIRE(base_vs_half.valid);
    REQUIRE(base_vs_full.valid);
    CHECK(base_vs_half.similarity < 0.99f);
    CHECK(base_vs_full.similarity < 0.99f);

    // ...and a fuller fill must diverge from the baseline MORE than a half fill
    // (the overlay covers a larger fraction of the box as the value rises).
    CHECK(base_vs_full.similarity < base_vs_half.similarity);
}

TEST_CASE("ImageView per-shape gradient fill differs from a flat fill",
          "[view][image][fill]") {
    // The design-import shape-fill enhancement: each shape fills with ITS OWN
    // sampled gradient (ELYSIUM's cylinder purple, prism magenta, …) instead of
    // one generic color. set_fill_gradient(stops) ⇒ the silhouette fill paints a
    // bottom→top gradient revealed up to fill_value; <2 stops clears it.
    const std::string img = make_source_image();
    auto base = render_fill(img, -1.0f);  // fill disabled
    if (base.empty()) SKIP("Skia raster screenshot backend unavailable");

    auto flat = render_fill(img, 1.0f);   // single-color fill (the legacy path)
    REQUIRE_FALSE(flat.empty());
    // If fill compositing is a no-op in this build (a partial-Skia runner where
    // the mask url() path can't composite), flat == base — skip rather than
    // assert a fill that physically can't render here.
    if (compare_screenshots(base, flat).similarity >= 0.999f)
        SKIP("Skia fill compositing unavailable in this build");

    auto grad = std::make_unique<ImageView>();
    grad->set_image_path(img);
    grad->set_fill_value(1.0f);
    REQUIRE_FALSE(grad->has_fill_gradient());
    grad->set_fill_gradient({Color::rgba8(230, 40, 40),    // bottom
                             Color::rgba8(40, 40, 230)});   // top
    REQUIRE(grad->has_fill_gradient());
    auto grad_png = render_to_png(*grad, 48, 48, 1.0f, ScreenshotBackend::skia);
    REQUIRE_FALSE(grad_png.empty());

    // A two-color gradient fill must differ from the flat single-color fill.
    CHECK(compare_screenshots(flat, grad_png).similarity < 0.99f);

    // Fewer than two stops clears the gradient (back to the flat-color path).
    grad->set_fill_gradient({Color::rgba8(230, 40, 40)});
    CHECK_FALSE(grad->has_fill_gradient());
}
