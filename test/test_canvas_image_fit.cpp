// Canvas image-placement primitives: affine transform, preserve-aspect fit,
// and tiled fill. The geometry helper (`fit_image_rect`) is asserted directly;
// the draw helpers are asserted through RecordingCanvas, which now captures the
// opaque-handle `draw_image` as a `draw_image` command with an empty `text`.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/affine_transform.hpp>

using namespace pulp::canvas;
using Catch::Matchers::WithinAbs;

namespace {

// Count the handle draws (draw_image commands whose `text` is empty).
size_t handle_draw_count(const RecordingCanvas& c) {
    size_t n = 0;
    for (const auto& cmd : c.commands())
        if (cmd.type == DrawCommand::Type::draw_image && cmd.text.empty()) ++n;
    return n;
}

const DrawCommand* first_handle_draw(const RecordingCanvas& c) {
    for (const auto& cmd : c.commands())
        if (cmd.type == DrawCommand::Type::draw_image && cmd.text.empty()) return &cmd;
    return nullptr;
}

void* fake_handle() { return reinterpret_cast<void*>(0x1234); }

}  // namespace

TEST_CASE("fit_image_rect fill stretches to the destination box", "[canvas][image-fit]") {
    auto r = Canvas::fit_image_rect(200, 100, 10, 20, 100, 100, Canvas::ImageFit::fill);
    CHECK(r.x == 10.0f);
    CHECK(r.y == 20.0f);
    CHECK(r.w == 100.0f);
    CHECK(r.h == 100.0f);
}

TEST_CASE("fit_image_rect contain preserves aspect and centers", "[canvas][image-fit]") {
    // 2:1 image into a 100x100 box -> scale 0.5 -> 100x50, centered vertically.
    auto r = Canvas::fit_image_rect(200, 100, 10, 10, 100, 100, Canvas::ImageFit::contain);
    CHECK_THAT(r.w, WithinAbs(100.0f, 1e-4f));
    CHECK_THAT(r.h, WithinAbs(50.0f, 1e-4f));
    CHECK_THAT(r.x, WithinAbs(10.0f, 1e-4f));
    CHECK_THAT(r.y, WithinAbs(35.0f, 1e-4f));  // 10 + (100-50)/2
}

TEST_CASE("fit_image_rect cover fills the box and overflows one axis", "[canvas][image-fit]") {
    // 2:1 image into 100x100 -> scale 1.0 -> 200x100, overflowing horizontally.
    auto r = Canvas::fit_image_rect(200, 100, 10, 10, 100, 100, Canvas::ImageFit::cover);
    CHECK_THAT(r.w, WithinAbs(200.0f, 1e-4f));
    CHECK_THAT(r.h, WithinAbs(100.0f, 1e-4f));
    CHECK_THAT(r.x, WithinAbs(-40.0f, 1e-4f));  // 10 + (100-200)/2
    CHECK_THAT(r.y, WithinAbs(10.0f, 1e-4f));
}

TEST_CASE("fit_image_rect none keeps natural size and honors alignment", "[canvas][image-fit]") {
    // Top-left alignment leaves the image at the box origin.
    auto tl = Canvas::fit_image_rect(40, 40, 0, 0, 100, 100, Canvas::ImageFit::none, 0.0f, 0.0f);
    CHECK_THAT(tl.x, WithinAbs(0.0f, 1e-4f));
    CHECK_THAT(tl.y, WithinAbs(0.0f, 1e-4f));
    CHECK_THAT(tl.w, WithinAbs(40.0f, 1e-4f));

    // Bottom-right alignment pushes it to the far corner.
    auto br = Canvas::fit_image_rect(40, 40, 0, 0, 100, 100, Canvas::ImageFit::none, 1.0f, 1.0f);
    CHECK_THAT(br.x, WithinAbs(60.0f, 1e-4f));
    CHECK_THAT(br.y, WithinAbs(60.0f, 1e-4f));
}

TEST_CASE("fit_image_rect scale_down never upscales", "[canvas][image-fit]") {
    // Small image into a big box stays natural size (unlike contain).
    auto small = Canvas::fit_image_rect(40, 40, 0, 0, 100, 100, Canvas::ImageFit::scale_down);
    CHECK_THAT(small.w, WithinAbs(40.0f, 1e-4f));
    CHECK_THAT(small.h, WithinAbs(40.0f, 1e-4f));

    // Big image into a small box behaves like contain.
    auto big = Canvas::fit_image_rect(200, 100, 0, 0, 100, 100, Canvas::ImageFit::scale_down);
    CHECK_THAT(big.w, WithinAbs(100.0f, 1e-4f));
    CHECK_THAT(big.h, WithinAbs(50.0f, 1e-4f));
}

TEST_CASE("fit_image_rect guards degenerate inputs", "[canvas][image-fit]") {
    auto r = Canvas::fit_image_rect(0, 0, 5, 6, 100, 100, Canvas::ImageFit::contain);
    CHECK(r.x == 5.0f);
    CHECK(r.y == 6.0f);
    CHECK(r.w == 100.0f);
    CHECK(r.h == 100.0f);
}

TEST_CASE("draw_image_transformed brackets a concat with save and restore", "[canvas][image-fit]") {
    RecordingCanvas c;
    AffineTransform t = AffineTransform::translation(30.0f, 40.0f);
    c.draw_image_transformed(fake_handle(), 64.0f, 48.0f, t);

    const auto& cmds = c.commands();
    REQUIRE(cmds.size() == 4);
    CHECK(cmds[0].type == DrawCommand::Type::save);
    CHECK(cmds[1].type == DrawCommand::Type::concat_transform);
    CHECK(cmds[2].type == DrawCommand::Type::draw_image);
    CHECK(cmds[3].type == DrawCommand::Type::restore);

    // The concat carries the supplied matrix; the image draws its natural box.
    CHECK_THAT(cmds[1].f[4], WithinAbs(30.0f, 1e-4f));  // e (tx)
    CHECK_THAT(cmds[1].f[5], WithinAbs(40.0f, 1e-4f));  // f (ty)
    CHECK(cmds[2].text.empty());
    CHECK(cmds[2].f[0] == 0.0f);
    CHECK(cmds[2].f[1] == 0.0f);
    CHECK(cmds[2].f[2] == 64.0f);
    CHECK(cmds[2].f[3] == 48.0f);
}

TEST_CASE("draw_image_fitted clips to the box and draws the fitted rect", "[canvas][image-fit]") {
    RecordingCanvas c;
    // cover overflows -> the clip guarantees nothing paints outside the box.
    c.draw_image_fitted(fake_handle(), 200, 100, 10, 10, 100, 100, Canvas::ImageFit::cover);

    // Exactly one clip_rect covering the destination box.
    size_t clips = c.count(DrawCommand::Type::clip_rect);
    CHECK(clips == 1);
    for (const auto& cmd : c.commands()) {
        if (cmd.type == DrawCommand::Type::clip_rect) {
            CHECK(cmd.f[0] == 10.0f);
            CHECK(cmd.f[1] == 10.0f);
            CHECK(cmd.f[2] == 100.0f);
            CHECK(cmd.f[3] == 100.0f);
        }
    }

    const DrawCommand* draw = first_handle_draw(c);
    REQUIRE(draw != nullptr);
    auto expected = Canvas::fit_image_rect(200, 100, 10, 10, 100, 100, Canvas::ImageFit::cover);
    CHECK_THAT(draw->f[0], WithinAbs(expected.x, 1e-4f));
    CHECK_THAT(draw->f[1], WithinAbs(expected.y, 1e-4f));
    CHECK_THAT(draw->f[2], WithinAbs(expected.w, 1e-4f));
    CHECK_THAT(draw->f[3], WithinAbs(expected.h, 1e-4f));
}

TEST_CASE("draw_image_tiled lays a clipped grid of whole tiles", "[canvas][image-fit]") {
    RecordingCanvas c;
    // 100x100 box, 40x40 tiles, no offset -> starts at 0,40,80 on each axis.
    c.draw_image_tiled(fake_handle(), 40.0f, 40.0f, 0.0f, 0.0f, 100.0f, 100.0f);

    CHECK(c.count(DrawCommand::Type::clip_rect) == 1);
    CHECK(handle_draw_count(c) == 9);  // 3 columns x 3 rows

    // Every tile is one cell in size.
    for (const auto& cmd : c.commands()) {
        if (cmd.type == DrawCommand::Type::draw_image && cmd.text.empty()) {
            CHECK(cmd.f[2] == 40.0f);
            CHECK(cmd.f[3] == 40.0f);
        }
    }
}

TEST_CASE("draw_image_tiled offset shifts the first tile before the box edge",
          "[canvas][image-fit]") {
    RecordingCanvas c;
    // A positive offset scrolls the tile origin; the first tile must start at
    // or before the leading edge so no gap shows.
    c.draw_image_tiled(fake_handle(), 40.0f, 40.0f, 0.0f, 0.0f, 40.0f, 40.0f,
                       /*offset_x=*/10.0f, /*offset_y=*/10.0f);

    const DrawCommand* draw = first_handle_draw(c);
    REQUIRE(draw != nullptr);
    CHECK(draw->f[0] <= 0.0f);
    CHECK(draw->f[1] <= 0.0f);
    CHECK(draw->f[0] > -40.0f);
    CHECK(draw->f[1] > -40.0f);
}

TEST_CASE("draw_image_tiled ignores degenerate tile or box sizes", "[canvas][image-fit]") {
    RecordingCanvas c;
    c.draw_image_tiled(fake_handle(), 0.0f, 40.0f, 0.0f, 0.0f, 100.0f, 100.0f);
    c.draw_image_tiled(fake_handle(), 40.0f, 40.0f, 0.0f, 0.0f, 0.0f, 100.0f);
    CHECK(handle_draw_count(c) == 0);
}
