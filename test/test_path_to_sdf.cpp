#include <pulp/canvas/path_to_sdf.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using pulp::canvas::path_to_sdf;

TEST_CASE("path_to_sdf: centred square mask produces high centre, low corner",
          "[canvas][sdf][path]") {
    constexpr int W = 32, H = 32;
    std::vector<std::uint8_t> mask(W * H, 0);
    // Fill a 16x16 square in the centre.
    for (int y = 8; y < 24; ++y)
        for (int x = 8; x < 24; ++x)
            mask[y * W + x] = 255;

    auto sdf = path_to_sdf(mask.data(), W, H, 8);
    REQUIRE(sdf.size() == static_cast<std::size_t>(W * H));

    // Debug dump around the centre before asserting.
    INFO("sdf[16,16]=" << static_cast<int>(sdf[16 * W + 16])
         << " sdf[12,16]=" << static_cast<int>(sdf[16 * W + 12])
         << " sdf[20,16]=" << static_cast<int>(sdf[16 * W + 20])
         << " sdf[0]="     << static_cast<int>(sdf[0]));
    // Centre of the square: deep inside, near 255.
    REQUIRE(sdf[16 * W + 16] > 200);
    // Corner of the image: far outside, near 0.
    REQUIRE(sdf[0] < 40);
    // Edge of the square: near 128.
    const int edge = sdf[8 * W + 16];
    REQUIRE(edge > 100);
    REQUIRE(edge < 160);
}

TEST_CASE("path_to_sdf: empty mask yields all-outside",
          "[canvas][sdf][path]") {
    std::vector<std::uint8_t> mask(16 * 16, 0);
    auto sdf = path_to_sdf(mask.data(), 16, 16, 4);
    for (auto v : sdf) REQUIRE(v < 20);
}

TEST_CASE("path_to_sdf: fully-filled mask yields all-inside",
          "[canvas][sdf][path]") {
    std::vector<std::uint8_t> mask(16 * 16, 255);
    auto sdf = path_to_sdf(mask.data(), 16, 16, 4);
    for (auto v : sdf) REQUIRE(v > 200);
}
