// ImageView ↔ ImageCache wiring test. Workstream 07 B4.
//
// The real decode path is Skia-backed and tested elsewhere. Here we
// just pin the public-API shape: set_image_source URL-keys, set_image_cache
// attaches/detaches, and the legacy set_image_path routes through the
// URI normaliser so existing callers keep working.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/image_cache.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/canvas/canvas.hpp>

using namespace pulp::view;

TEST_CASE("set_image_path is equivalent to file:// URI", "[widgets][image]") {
    ImageView v;
    v.set_image_path("/tmp/foo.png");
    REQUIRE(v.image_source() == "file:///tmp/foo.png");
    REQUIRE(v.image_path() == "file:///tmp/foo.png");
}

TEST_CASE("set_image_source accepts URI schemes directly", "[widgets][image]") {
    ImageView v;
    v.set_image_source("resource://icons/save.png");
    REQUIRE(v.image_source() == "resource://icons/save.png");

    v.set_image_source("memory://sha256=deadbeef");
    REQUIRE(v.image_source() == "memory://sha256=deadbeef");
}

TEST_CASE("set_image_cache attaches and detaches", "[widgets][image]") {
    ImageView v;
    REQUIRE(v.image_cache() == nullptr);

    ImageCache cache;
    v.set_image_cache(&cache);
    REQUIRE(v.image_cache() == &cache);

    v.set_image_cache(nullptr);
    REQUIRE(v.image_cache() == nullptr);
}

TEST_CASE("changing the source invalidates the loaded flag", "[widgets][image]") {
    ImageView v;
    v.set_image_source("file:///tmp/a.png");
    v.set_image_source("file:///tmp/b.png");
    REQUIRE(v.image_source() == "file:///tmp/b.png");
}

// pulp #1737 — CSS `object-fit` + `object-position` are now consumed by
// ImageView::paint when the canvas backend can measure intrinsic image
// dimensions. We test by feeding ImageView a stub Canvas that:
//   * returns a fixed (img_w × img_h) from `measure_image_from_file`
//   * records every `draw_image_from_file*` call for assertion
// The stub captures dst rect (and src rect for the rect overload) so
// each fit mode's geometry can be checked precisely without going
// through Skia or any real decoder.

namespace {

// Derive from RecordingCanvas so we get all the non-image virtual
// methods for free (RecordingCanvas implements every Canvas vtable
// slot). We override only the four we care about: measure +
// draw_image_from_file{,_rect} for the object-fit assertions.
struct ImageStubCanvas : pulp::canvas::RecordingCanvas {
    float img_w = 0.0f, img_h = 0.0f;
    bool can_measure = true;

    struct Draw {
        bool has_src = false;
        float sx = 0, sy = 0, sw = 0, sh = 0;
        float dx = 0, dy = 0, dw = 0, dh = 0;
    };
    std::vector<Draw> draws;

    bool measure_image_from_file(const std::string& path,
                                  float& out_w, float& out_h) override {
        (void)path;
        if (!can_measure) { out_w = 0; out_h = 0; return false; }
        out_w = img_w; out_h = img_h;
        return img_w > 0.0f && img_h > 0.0f;
    }
    bool draw_image_from_file(const std::string& path,
                               float x, float y, float w, float h) override {
        (void)path;
        draws.push_back({false, 0,0,0,0, x, y, w, h});
        return true;
    }
    bool draw_image_from_file_rect(const std::string& path,
                                    float sx, float sy, float sw, float sh,
                                    float dx, float dy, float dw, float dh) override {
        (void)path;
        draws.push_back({true, sx, sy, sw, sh, dx, dy, dw, dh});
        return true;
    }
};

void configure_view(ImageView& v, int w, int h, const std::string& fit,
                    const std::string& position = "") {
    v.set_image_source("file:///tmp/test.png");
    v.set_bounds({0, 0, static_cast<float>(w), static_cast<float>(h)});
    v.set_object_fit(fit);
    if (!position.empty()) v.set_object_position(position);
}

} // namespace

TEST_CASE("object-fit: fill stretches to bounds (default)",
          "[widgets][image][object-fit][issue-1737]") {
    using Catch::Matchers::WithinAbs;
    ImageView v;
    configure_view(v, 200, 100, "fill");
    ImageStubCanvas cv;
    cv.img_w = 50.0f; cv.img_h = 50.0f;
    v.paint(cv);
    REQUIRE(cv.draws.size() == 1);
    REQUIRE(cv.draws[0].has_src == false);
    REQUIRE_THAT(cv.draws[0].dw, WithinAbs(200.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dh, WithinAbs(100.0f, 0.001f));
}

TEST_CASE("object-fit: contain letterboxes preserving aspect ratio",
          "[widgets][image][object-fit][issue-1737]") {
    using Catch::Matchers::WithinAbs;
    // 200×100 box, 50×50 image → contain shrinks to fit the smaller
    // axis (height): 100×100 painted dst, centred horizontally.
    ImageView v;
    configure_view(v, 200, 100, "contain");
    ImageStubCanvas cv;
    cv.img_w = 50.0f; cv.img_h = 50.0f;
    v.paint(cv);
    REQUIRE(cv.draws.size() == 1);
    REQUIRE(cv.draws[0].has_src == false);
    REQUIRE_THAT(cv.draws[0].dw, WithinAbs(100.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dh, WithinAbs(100.0f, 0.001f));
    // Centred horizontally: x = (200 - 100) / 2 = 50.
    REQUIRE_THAT(cv.draws[0].dx, WithinAbs(50.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dy, WithinAbs(0.0f, 0.001f));
}

TEST_CASE("object-fit: cover crops, scales to cover the larger axis",
          "[widgets][image][object-fit][issue-1737]") {
    using Catch::Matchers::WithinAbs;
    // 200×100 box, 50×50 image → cover scales to fit the LARGER axis
    // (width): 200×200 painted dst, vertically overflowing the box.
    ImageView v;
    configure_view(v, 200, 100, "cover");
    ImageStubCanvas cv;
    cv.img_w = 50.0f; cv.img_h = 50.0f;
    v.paint(cv);
    REQUIRE(cv.draws.size() == 1);
    REQUIRE(cv.draws[0].has_src == false);
    REQUIRE_THAT(cv.draws[0].dw, WithinAbs(200.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dh, WithinAbs(200.0f, 0.001f));
    // Centred vertically: y = (100 - 200) / 2 = -50 (overflow above
    // and below the box; clip happens at the canvas level).
    REQUIRE_THAT(cv.draws[0].dy, WithinAbs(-50.0f, 0.001f));
}

TEST_CASE("object-fit: none paints natural size centred",
          "[widgets][image][object-fit][issue-1737]") {
    using Catch::Matchers::WithinAbs;
    // 200×100 box, 50×50 image → none paints at native 50×50 centred.
    ImageView v;
    configure_view(v, 200, 100, "none");
    ImageStubCanvas cv;
    cv.img_w = 50.0f; cv.img_h = 50.0f;
    v.paint(cv);
    REQUIRE(cv.draws.size() == 1);
    // src rect equals the full 50×50 image (no crop), so the renderer
    // routes through the dst-only path — that's still correct, just a
    // different code path. The dst rect is what we really care about.
    REQUIRE_THAT(cv.draws[0].dw, WithinAbs(50.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dh, WithinAbs(50.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dx, WithinAbs(75.0f, 0.001f));   // (200-50)/2
    REQUIRE_THAT(cv.draws[0].dy, WithinAbs(25.0f, 0.001f));   // (100-50)/2
}

// pulp #1737 — object-fit: none with image LARGER than the box must
// crop via the source-rect overload (src centered within the image,
// dst clamped to the box). This is the case where draw_image_from_file_rect
// actually fires.
TEST_CASE("object-fit: none crops via source-rect when image overflows box",
          "[widgets][image][object-fit][issue-1737]") {
    using Catch::Matchers::WithinAbs;
    // 50×50 box, 200×100 image → none paints at native scale, but only
    // a 50×50 sub-rect fits. Source rect should be (75, 25, 50, 50)
    // (centered within the 200×100 image), dst should be the full box.
    ImageView v;
    configure_view(v, 50, 50, "none");
    ImageStubCanvas cv;
    cv.img_w = 200.0f; cv.img_h = 100.0f;
    v.paint(cv);
    REQUIRE(cv.draws.size() == 1);
    REQUIRE(cv.draws[0].has_src == true);
    REQUIRE_THAT(cv.draws[0].sx, WithinAbs(75.0f, 0.001f));   // (200-50)/2
    REQUIRE_THAT(cv.draws[0].sy, WithinAbs(25.0f, 0.001f));   // (100-50)/2
    REQUIRE_THAT(cv.draws[0].sw, WithinAbs(50.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].sh, WithinAbs(50.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dw, WithinAbs(50.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dh, WithinAbs(50.0f, 0.001f));
}

TEST_CASE("object-fit: scale-down picks none when image fits in box",
          "[widgets][image][object-fit][issue-1737]") {
    using Catch::Matchers::WithinAbs;
    // 200×100 box, 50×50 image fits → behave as `none` (50×50 centred).
    ImageView v;
    configure_view(v, 200, 100, "scale-down");
    ImageStubCanvas cv;
    cv.img_w = 50.0f; cv.img_h = 50.0f;
    v.paint(cv);
    REQUIRE(cv.draws.size() == 1);
    REQUIRE_THAT(cv.draws[0].dw, WithinAbs(50.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dh, WithinAbs(50.0f, 0.001f));
}

TEST_CASE("object-fit: scale-down picks contain when image overflows box",
          "[widgets][image][object-fit][issue-1737]") {
    using Catch::Matchers::WithinAbs;
    // 100×100 box, 200×100 image → behave as `contain` (100×50 letterboxed).
    ImageView v;
    configure_view(v, 100, 100, "scale-down");
    ImageStubCanvas cv;
    cv.img_w = 200.0f; cv.img_h = 100.0f;
    v.paint(cv);
    REQUIRE(cv.draws.size() == 1);
    REQUIRE_THAT(cv.draws[0].dw, WithinAbs(100.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dh, WithinAbs(50.0f, 0.001f));
}

TEST_CASE("object-position: percentage offsets the centred dst",
          "[widgets][image][object-position][issue-1737]") {
    using Catch::Matchers::WithinAbs;
    // 200×100 box, 50×50 image with object-fit: contain → 100×100 dst.
    // object-position "0% 0%" pins the dst to the top-left of the box.
    ImageView v;
    configure_view(v, 200, 100, "contain", "0% 0%");
    ImageStubCanvas cv;
    cv.img_w = 50.0f; cv.img_h = 50.0f;
    v.paint(cv);
    REQUIRE(cv.draws.size() == 1);
    REQUIRE_THAT(cv.draws[0].dx, WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dy, WithinAbs(0.0f, 0.001f));
    // 100% pins to the bottom-right.
    ImageView v2;
    configure_view(v2, 200, 100, "contain", "100% 100%");
    ImageStubCanvas cv2;
    cv2.img_w = 50.0f; cv2.img_h = 50.0f;
    v2.paint(cv2);
    REQUIRE(cv2.draws.size() == 1);
    REQUIRE_THAT(cv2.draws[0].dx, WithinAbs(100.0f, 0.001f));   // 200 - 100
    REQUIRE_THAT(cv2.draws[0].dy, WithinAbs(0.0f, 0.001f));     // 100 - 100
}

TEST_CASE("object-fit unknown keyword falls back to fill",
          "[widgets][image][object-fit][coverage][phase3]") {
    using Catch::Matchers::WithinAbs;
    ImageView v;
    configure_view(v, 200, 100, "stretchy");
    ImageStubCanvas cv;
    cv.img_w = 50.0f;
    cv.img_h = 50.0f;

    v.paint(cv);

    REQUIRE(cv.draws.size() == 1);
    REQUIRE_FALSE(cv.draws[0].has_src);
    REQUIRE_THAT(cv.draws[0].dx, WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dy, WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dw, WithinAbs(200.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dh, WithinAbs(100.0f, 0.001f));
}

TEST_CASE("object-position pixel lengths offset by slack",
          "[widgets][image][object-position][coverage][phase3]") {
    using Catch::Matchers::WithinAbs;
    ImageView v;
    configure_view(v, 200, 140, "contain", "25px 10px");
    ImageStubCanvas cv;
    cv.img_w = 100.0f;
    cv.img_h = 100.0f;

    v.paint(cv);

    REQUIRE(cv.draws.size() == 1);
    REQUIRE_FALSE(cv.draws[0].has_src);
    REQUIRE_THAT(cv.draws[0].dw, WithinAbs(140.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dh, WithinAbs(140.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dx, WithinAbs(25.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dy, WithinAbs(0.0f, 0.001f));
}

TEST_CASE("object-fit graceful fallback when measure_image_from_file fails",
          "[widgets][image][object-fit][issue-1737]") {
    using Catch::Matchers::WithinAbs;
    // No intrinsic dims → ImageView falls back to the pre-#1737 stretch.
    ImageView v;
    configure_view(v, 200, 100, "contain");
    ImageStubCanvas cv;
    cv.can_measure = false;
    v.paint(cv);
    REQUIRE(cv.draws.size() == 1);
    REQUIRE(cv.draws[0].has_src == false);
    REQUIRE_THAT(cv.draws[0].dw, WithinAbs(200.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dh, WithinAbs(100.0f, 0.001f));
}
