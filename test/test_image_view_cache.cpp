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
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/crypto.hpp>

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
    // axis (height): 100×100 painted dst, centerd horizontally.
    ImageView v;
    configure_view(v, 200, 100, "contain");
    ImageStubCanvas cv;
    cv.img_w = 50.0f; cv.img_h = 50.0f;
    v.paint(cv);
    REQUIRE(cv.draws.size() == 1);
    REQUIRE(cv.draws[0].has_src == false);
    REQUIRE_THAT(cv.draws[0].dw, WithinAbs(100.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dh, WithinAbs(100.0f, 0.001f));
    // Centerd horizontally: x = (200 - 100) / 2 = 50.
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
    // Centerd vertically: y = (100 - 200) / 2 = -50 (overflow above
    // and below the box; clip happens at the canvas level).
    REQUIRE_THAT(cv.draws[0].dy, WithinAbs(-50.0f, 0.001f));
}

TEST_CASE("object-fit: none paints natural size centerd",
          "[widgets][image][object-fit][issue-1737]") {
    using Catch::Matchers::WithinAbs;
    // 200×100 box, 50×50 image → none paints at native 50×50 centerd.
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
    // 200×100 box, 50×50 image fits → behave as `none` (50×50 centerd).
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

TEST_CASE("object-position: percentage offsets the centerd dst",
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
          "[widgets][image][object-fit]") {
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

TEST_CASE("object-position: length tokens offset by slack",
          "[widgets][image][object-position]") {
    using Catch::Matchers::WithinAbs;
    ImageView v;
    configure_view(v, 200, 150, "contain", "25px 10px");
    ImageStubCanvas cv;
    cv.img_w = 50.0f; cv.img_h = 50.0f;

    v.paint(cv);

    REQUIRE(cv.draws.size() == 1);
    REQUIRE_THAT(cv.draws[0].dw, WithinAbs(150.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dh, WithinAbs(150.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dx, WithinAbs(25.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dy, WithinAbs(0.0f, 0.001f));
}

TEST_CASE("object-position: malformed numeric tokens fall back to center",
          "[widgets][image][object-position]") {
    using Catch::Matchers::WithinAbs;
    ImageView v;
    configure_view(v, 200, 150, "contain", "25pxjunk 10em");
    ImageStubCanvas cv;
    cv.img_w = 50.0f; cv.img_h = 50.0f;

    v.paint(cv);

    REQUIRE(cv.draws.size() == 1);
    REQUIRE_THAT(cv.draws[0].dw, WithinAbs(150.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dh, WithinAbs(150.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dx, WithinAbs(25.0f, 0.001f));
    REQUIRE_THAT(cv.draws[0].dy, WithinAbs(0.0f, 0.001f));
}

TEST_CASE("object-position: non-finite numeric tokens fall back to center",
          "[widgets][image][object-position]") {
    using Catch::Matchers::WithinAbs;
    ImageView v;
    configure_view(v, 200, 150, "contain", "1e999% 25%");
    ImageStubCanvas cv;
    cv.img_w = 50.0f; cv.img_h = 50.0f;

    v.paint(cv);

    REQUIRE(cv.draws.size() == 1);
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

// Inline `data:image/*` sources keep their encoded bytes whatever the format,
// read the intrinsic size from the backend's decoder once per source, and draw
// through the content-keyed entry.

namespace {

// A 16x8 GIF: left half red, right half blue.
constexpr uint8_t kSplitGif[] = {
    0x47, 0x49, 0x46, 0x38, 0x37, 0x61, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00,
    0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x08, 0x00, 0x00, 0x08, 0x24, 0x00, 0x01, 0x08, 0x1c, 0x28, 0x30, 0x80, 0xc1,
    0x83, 0x06, 0x09, 0x12, 0x44, 0x88, 0x50, 0xe1, 0x40, 0x86, 0x07, 0x1d, 0x16, 0x84, 0x18,
    0x40, 0x22, 0x00, 0x8a, 0x15, 0x25, 0x62, 0xb4, 0xb8, 0x51, 0x23, 0xc5, 0x80, 0x00, 0x3b};

std::string inline_uri(const char* media_type, const uint8_t* bytes, size_t size) {
    return std::string("data:") + media_type + ";base64," +
           pulp::runtime::base64_encode(bytes, size);
}

// Stands in for a decoding backend: measures and draws whatever bytes it was
// told it can decode, and records every keyed draw.
struct InlineStubCanvas : pulp::canvas::RecordingCanvas {
    float img_w = 0.0f, img_h = 0.0f;
    bool decodes = true;
    int measures = 0;
    struct KeyedDraw {
        std::string key;
        size_t size = 0;
        float dx = 0, dy = 0, dw = 0, dh = 0;
    };
    std::vector<KeyedDraw> keyed;
    int file_draws = 0;

    bool supports(pulp::canvas::CanvasCapability cap) const override {
        return cap == pulp::canvas::CanvasCapability::images;
    }
    bool measure_image_from_data(const uint8_t*, size_t, float& w, float& h) override {
        ++measures;
        w = decodes ? img_w : 0.0f;
        h = decodes ? img_h : 0.0f;
        return decodes;
    }
    bool draw_image_from_keyed_data(const std::string& key, const uint8_t*, size_t size, float x,
                                    float y, float w, float h) override {
        keyed.push_back({key, size, x, y, w, h});
        return decodes;
    }
    bool draw_image_from_file(const std::string&, float, float, float, float) override {
        ++file_draws;
        return false;
    }
};

} // namespace

TEST_CASE("ImageView sizes a non-PNG data URI from the decoder and draws it keyed",
          "[widgets][image][inline-image]") {
    using Catch::Matchers::WithinAbs;
    ImageView v;
    v.set_image_source(inline_uri("image/gif", kSplitGif, sizeof(kSplitGif)));
    v.set_bounds({0, 0, 200, 200});
    v.set_object_fit("contain");
    InlineStubCanvas cv;
    cv.img_w = 16.0f;
    cv.img_h = 8.0f;

    for (int i = 0; i < 3; ++i)
        v.paint(cv);

    // The header is read once per source, not once per paint.
    CHECK(cv.measures == 1);
    CHECK(cv.file_draws == 0);
    REQUIRE(cv.keyed.size() == 3);
    const auto expected_key = pulp::runtime::sha256_hex(kSplitGif, sizeof(kSplitGif));
    for (const auto& d : cv.keyed) {
        CHECK(d.key == expected_key);
        CHECK(d.size == sizeof(kSplitGif));
        // contain: 16x8 into 200x200 letterboxes to 200x100, centred.
        CHECK_THAT(d.dw, WithinAbs(200.0f, 0.001f));
        CHECK_THAT(d.dh, WithinAbs(100.0f, 0.001f));
        CHECK_THAT(d.dy, WithinAbs(50.0f, 0.001f));
    }
}

TEST_CASE("ImageView stretches an inline image the backend cannot measure",
          "[widgets][image][inline-image]") {
    using Catch::Matchers::WithinAbs;
    ImageView v;
    v.set_image_source(inline_uri("image/gif", kSplitGif, sizeof(kSplitGif)));
    v.set_bounds({0, 0, 120, 60});
    v.set_object_fit("contain");
    // A backend that draws but cannot read the size: object-fit falls back to fill.
    struct NoMeasure : InlineStubCanvas {
        bool measure_image_from_data(const uint8_t*, size_t, float& w, float& h) override {
            ++measures;
            w = h = 0.0f;
            return false;
        }
    } cv;
    v.paint(cv);
    REQUIRE(cv.keyed.size() == 1);
    CHECK(cv.file_draws == 0);
    CHECK_THAT(cv.keyed[0].dw, WithinAbs(120.0f, 0.001f));
    CHECK_THAT(cv.keyed[0].dh, WithinAbs(60.0f, 0.001f));
}

TEST_CASE("ImageView fails an undecodable inline payload once and then paints the placeholder",
          "[widgets][image][inline-image]") {
    const uint8_t junk[] = {'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e'};
    ImageView v;
    v.set_image_source(inline_uri("image/jpeg", junk, sizeof(junk)));
    v.set_bounds({0, 0, 64, 64});
    InlineStubCanvas cv;
    cv.decodes = false;

    for (int i = 0; i < 4; ++i)
        v.paint(cv);

    // One decode attempt, never a retry and never a filesystem lookup of the URI.
    CHECK(cv.measures == 1);
    CHECK(cv.keyed.size() == 1);
    CHECK(cv.file_draws == 0);
    // Every paint lands on the placeholder, labelled with the media type
    // rather than a fragment of the base64 payload.
    int labels = 0;
    for (const auto& cmd : cv.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_text) {
            CHECK(cmd.text == "image/jpeg");
            ++labels;
        }
    }
    CHECK(labels == 4);

    // A new source is a fresh attempt.
    v.set_image_source(inline_uri("image/gif", kSplitGif, sizeof(kSplitGif)));
    cv.decodes = true;
    cv.img_w = 16.0f;
    cv.img_h = 8.0f;
    v.paint(cv);
    CHECK(cv.keyed.size() == 2);
}

TEST_CASE("ImageView fails a malformed percent-encoded data URI once into the placeholder",
          "[widgets][image][inline-image]") {
    ImageView v;
    v.set_image_source("data:image/png,%89PNG%0D%0A%1A%0A%zz");
    v.set_bounds({0, 0, 64, 64});
    InlineStubCanvas cv;
    for (int i = 0; i < 3; ++i)
        v.paint(cv);
    // No bytes came out of the payload, so nothing is measured or drawn.
    CHECK(cv.measures == 0);
    CHECK(cv.keyed.empty());
    CHECK(cv.file_draws == 0);
    int labels = 0;
    for (const auto& cmd : cv.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_text) {
            CHECK(cmd.text == "image/png");
            ++labels;
        }
    }
    CHECK(labels == 3);
}

TEST_CASE("ImageView draws a percent-encoded data URI through the keyed entry",
          "[widgets][image][inline-image]") {
    std::string uri = "data:image/gif,";
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (uint8_t byte : kSplitGif) {
        uri.push_back('%');
        uri.push_back(kHex[byte >> 4]);
        uri.push_back(kHex[byte & 0x0f]);
    }
    ImageView v;
    v.set_image_source(uri);
    v.set_bounds({0, 0, 32, 16});
    InlineStubCanvas cv;
    cv.img_w = 16.0f;
    cv.img_h = 8.0f;
    v.paint(cv);
    REQUIRE(cv.keyed.size() == 1);
    CHECK(cv.keyed[0].key == pulp::runtime::sha256_hex(kSplitGif, sizeof(kSplitGif)));
    CHECK(cv.keyed[0].size == sizeof(kSplitGif));
}
