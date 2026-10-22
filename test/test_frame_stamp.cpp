// Frame-identity stamp: FrameStampView paints an update index through the
// real view raster path, and decode_frame_stamp reads it back from the pixels.
// The hidden [frame-stamp-emit] case writes PNG sequences for the Python
// decoder (tools/motion/visual/frame_stamp_decode.py) so the two sides are
// checked against one painter.

#include <catch2/catch_test_macros.hpp>

#include "support/frame_stamp.hpp"

#include <pulp/view/screenshot.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using pulp::test_support::decode_frame_stamp;
using pulp::test_support::FrameStampLayout;
using pulp::test_support::FrameStampView;

namespace {

constexpr float kScale = 2.0f;

struct Raster {
    std::vector<uint8_t> rgba;
    uint32_t w = 0;
    uint32_t h = 0;
};

uint32_t logical_width(const FrameStampLayout& layout) {
    return static_cast<uint32_t>(layout.x + layout.bits * layout.cell) + 4;
}

uint32_t logical_height(const FrameStampLayout& layout) {
    return static_cast<uint32_t>(layout.y + 2 * layout.cell) + 4;
}

Raster render(FrameStampView& view, const FrameStampLayout& layout) {
    const uint32_t w = logical_width(layout);
    const uint32_t h = logical_height(layout);
    view.set_bounds({0, 0, static_cast<float>(w), static_cast<float>(h)});
    Raster out;
    out.rgba = pulp::view::render_to_rgba(view, w, h, kScale, &out.w, &out.h);
    return out;
}

void paint_cell(Raster& r, const FrameStampLayout& layout, int col, int row, uint8_t level) {
    const auto side = static_cast<uint32_t>(layout.cell * kScale);
    const auto x0 = static_cast<uint32_t>((layout.x + col * layout.cell) * kScale);
    const auto y0 = static_cast<uint32_t>((layout.y + row * layout.cell) * kScale);
    for (uint32_t y = y0; y < y0 + side; ++y)
        for (uint32_t x = x0; x < x0 + side; ++x) {
            uint8_t* p = r.rgba.data() + (static_cast<size_t>(y) * r.w + x) * 4;
            p[0] = p[1] = p[2] = level;
        }
}

} // namespace

TEST_CASE("Frame stamp painted through the view raster decodes to its index",
          "[frame-stamp][motion]") {
    if (!pulp::view::raw_rgba_render_available())
        SKIP("This build has no raw-RGBA raster backend.");

    const FrameStampLayout layouts[] = {FrameStampLayout{}, FrameStampLayout{6, 3, 5, 12}};
    for (const auto& layout : layouts) {
        FrameStampView view(layout);
        const uint32_t mask = (1u << layout.bits) - 1u;
        for (uint32_t index : {0u, 1u, 2u, 0x5A5u, 0xABCDu, mask, mask + 3u}) {
            view.set_update_index(index);
            const Raster r = render(view, layout);
            REQUIRE_FALSE(r.rgba.empty());
            INFO("bits " << layout.bits << " index " << index);
            CHECK(decode_frame_stamp(r.rgba.data(), r.w, r.h, kScale, layout) ==
                  static_cast<int>(index & mask));
        }
    }
}

TEST_CASE("Frame stamp decoding returns -1 instead of guessing", "[frame-stamp][motion]") {
    if (!pulp::view::raw_rgba_render_available())
        SKIP("This build has no raw-RGBA raster backend.");

    const FrameStampLayout layout;
    FrameStampView view(layout);
    view.set_update_index(0x1234);
    const Raster clean = render(view, layout);
    REQUIRE(decode_frame_stamp(clean.rgba.data(), clean.w, clean.h, kScale, layout) == 0x1234);

    // A mid-grey cell is neither bit value.
    Raster grey = clean;
    paint_cell(grey, layout, 7, 0, 128);
    CHECK(decode_frame_stamp(grey.rgba.data(), grey.w, grey.h, kScale, layout) == -1);

    // Bit 15 of 0x1234 is 0; lighting its bit row makes it agree with its
    // complement row. Reading row 0 alone would decode 0x9234.
    Raster agree = clean;
    paint_cell(agree, layout, 0, 0, 255);
    CHECK(decode_frame_stamp(agree.rgba.data(), agree.w, agree.h, kScale, layout) == -1);

    // A grid that does not fit the capture, and an empty capture.
    CHECK(decode_frame_stamp(clean.rgba.data(), clean.w / 2, clean.h, kScale, layout) == -1);
    std::vector<uint8_t> blank(static_cast<size_t>(clean.w) * clean.h * 4, 0);
    CHECK(decode_frame_stamp(blank.data(), clean.w, clean.h, kScale, layout) == -1);
}

// Writes two 12-frame PNG sequences under $PULP_FRAME_STAMP_OUT: `live`, where
// every frame carries the next update index, and `frozen`, where the scene
// never repaints after the first update (the planted held-frame defect).
TEST_CASE("Frame stamp sequences for the Python decoder", "[.][frame-stamp-emit]") {
    const char* out_env = std::getenv("PULP_FRAME_STAMP_OUT");
    REQUIRE(out_env != nullptr);
    REQUIRE(pulp::view::raw_rgba_render_available());
    const std::filesystem::path out{out_env};

    const FrameStampLayout layout;
    const uint32_t w = logical_width(layout);
    const uint32_t h = logical_height(layout);
    for (const char* name : {"live", "frozen"}) {
        const auto dir = out / name;
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        FrameStampView view(layout);
        view.set_bounds({0, 0, static_cast<float>(w), static_cast<float>(h)});
        const bool frozen = std::string(name) == "frozen";
        for (uint32_t frame = 0; frame < 12; ++frame) {
            if (!frozen || frame == 0)
                view.set_update_index(40 + frame);
            char file[32];
            std::snprintf(file, sizeof(file), "frame_%04u.png", frame);
            REQUIRE(pulp::view::render_to_file(view, w, h, (dir / file).string(), kScale,
                                               pulp::view::ScreenshotBackend::skia));
        }
    }
}
