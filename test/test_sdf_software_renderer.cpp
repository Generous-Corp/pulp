// End-to-end software render test for the SDF pipeline.
//
// Builds an SdfAtlas, computes quads for a short text run, software-
// rasterizes them into an A8 buffer, and verifies the output has
// non-zero coverage inside the text region and zero coverage far
// outside it. Together with `test_sdf_atlas.cpp` this gives a path
// from "atlas pixels" to "rendered raster" without a GPU context.

#include <pulp/canvas/sdf_atlas.hpp>
#include <pulp/canvas/sdf_software_renderer.hpp>
#include <pulp/canvas/sdf_text.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace pulp::canvas;

namespace {
struct TinyAtlas {
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> data;

    int width() const { return w; }
    int height() const { return h; }
    const std::uint8_t* pixels() const { return data.data(); }
};
}  // namespace

TEST_CASE("software SDF render produces non-zero coverage for glyphs",
          "[canvas][sdf][render]") {
    SdfAtlas atlas;
    REQUIRE(atlas.build("", {U'A', U'B', U'C'}, 32, 4, 1024));

    auto quads = build_text_quads(atlas, std::u32string(U"ABC"),
                                  /*x*/ 4.0f, /*y*/ 40.0f,
                                  /*render_size*/ 32.0f);
    REQUIRE(quads.size() == 3);

    constexpr int W = 128;
    constexpr int H = 48;
    std::vector<std::uint8_t> out(W * H, 0);
    render_sdf_text_software(atlas, quads, out.data(), W, H);

    // At least some pixels inside the text band must be non-zero.
    std::size_t inked = 0;
    for (int y = 8; y < 40; ++y) {
        for (int x = 4; x < 100; ++x) {
            if (out[y * W + x] > 0) ++inked;
        }
    }
    REQUIRE(inked > 0);

    // Corners far outside the rendered region stay zero.
    REQUIRE(out[0] == 0);
    REQUIRE(out[(H - 1) * W + (W - 1)] == 0);
}

TEST_CASE("software SDF render handles empty quad list",
          "[canvas][sdf][render]") {
    SdfAtlas atlas;
    REQUIRE(atlas.build("", {U'A'}, 16, 2, 256));
    std::vector<std::uint8_t> out(32 * 32, 0);
    render_sdf_text_software(atlas, {}, out.data(), 32, 32);
    for (auto v : out) REQUIRE(v == 0);
}

TEST_CASE("software SDF render leaves output untouched for invalid output bounds",
          "[canvas][sdf][render][coverage][issue-650]") {
    TinyAtlas atlas{1, 1, {255}};
    SdfTextQuad q;
    q.dst_w = 1.0f;
    q.dst_h = 1.0f;
    q.src_w = 1.0f;
    q.src_h = 1.0f;

    std::uint8_t out[1] = {11};
    render_sdf_text_software(atlas, {q}, out, 0, 1);
    REQUIRE(out[0] == 11);
    render_sdf_text_software(atlas, {q}, out, 1, 0);
    REQUIRE(out[0] == 11);
}

TEST_CASE("software SDF render leaves output untouched for empty atlas",
          "[canvas][sdf][render][issue-641]") {
    TinyAtlas atlas;
    std::vector<std::uint8_t> out(4, 9);
    SdfTextQuad q;
    q.dst_w = 1.0f;
    q.dst_h = 1.0f;
    q.src_w = 1.0f;
    q.src_h = 1.0f;

    render_sdf_text_software(atlas, {q}, out.data(), 2, 2);
    for (auto v : out) REQUIRE(v == 9);
}

TEST_CASE("software SDF render skips degenerate quads",
          "[canvas][sdf][render][issue-641]") {
    TinyAtlas atlas{1, 1, {255}};
    std::vector<SdfTextQuad> quads;

    SdfTextQuad zero_width;
    zero_width.dst_w = 0.0f;
    zero_width.dst_h = 1.0f;
    zero_width.src_w = 1.0f;
    zero_width.src_h = 1.0f;
    quads.push_back(zero_width);

    SdfTextQuad zero_height = zero_width;
    zero_height.dst_w = 1.0f;
    zero_height.dst_h = 0.0f;
    quads.push_back(zero_height);

    std::vector<std::uint8_t> out(4, 0);
    render_sdf_text_software(atlas, quads, out.data(), 2, 2);
    for (auto v : out) REQUIRE(v == 0);
}

TEST_CASE("software SDF render clips destination and source bounds",
          "[canvas][sdf][render][issue-641]") {
    TinyAtlas atlas{2, 2, {
        255, 0,
        0, 0,
    }};
    SdfTextQuad q;
    q.dst_x = -0.1f;
    q.dst_y = -0.1f;
    q.dst_w = 2.0f;
    q.dst_h = 2.0f;
    q.src_x = 0.0f;
    q.src_y = 0.0f;
    q.src_w = 2.0f;
    q.src_h = 2.0f;

    std::vector<std::uint8_t> out(2 * 2, 0);
    render_sdf_text_software(atlas, {q}, out.data(), 2, 2);

    REQUIRE(out[0] == 255);
    REQUIRE(out[1] == 0);
    REQUIRE(out[2] == 0);
    REQUIRE(out[3] == 0);
}

TEST_CASE("software SDF render skips source samples outside atlas",
          "[canvas][sdf][render][issue-641]") {
    TinyAtlas atlas{1, 1, {255}};
    SdfTextQuad q;
    q.dst_w = 1.0f;
    q.dst_h = 1.0f;
    q.src_x = -2.0f;
    q.src_w = 1.0f;
    q.src_h = 1.0f;

    std::uint8_t out[1] = {7};
    render_sdf_text_software(atlas, {q}, out, 1, 1);
    REQUIRE(out[0] == 7);
}

TEST_CASE("software SDF render keeps maximum alpha for overlapping quads",
          "[canvas][sdf][render][issue-641]") {
    TinyAtlas atlas{2, 1, {64, 255}};

    SdfTextQuad low;
    low.dst_w = 1.0f;
    low.dst_h = 1.0f;
    low.src_w = 1.0f;
    low.src_h = 1.0f;

    SdfTextQuad high = low;
    high.src_x = 1.0f;

    std::uint8_t out[1] = {0};
    render_sdf_text_software(atlas, {low, high}, out, 1, 1);
    REQUIRE(out[0] == 255);
}

TEST_CASE("software SDF render honors custom edge thresholds",
          "[canvas][sdf][render][coverage][issue-650]") {
    TinyAtlas atlas{2, 1, {64, 255}};
    SdfTextQuad q;
    q.dst_w = 2.0f;
    q.dst_h = 1.0f;
    q.src_w = 2.0f;
    q.src_h = 1.0f;

    std::uint8_t out[2] = {0, 0};
    SdfTextOptions opts;
    opts.edge = 1.0f;
    render_sdf_text_software(atlas, {q}, out, 2, 1, opts);

    REQUIRE(out[0] == 0);
    REQUIRE(out[1] > 0);
    REQUIRE(out[1] < 255);
}
