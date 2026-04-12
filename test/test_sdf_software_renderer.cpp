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
