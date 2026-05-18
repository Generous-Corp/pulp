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

TEST_CASE("path_to_sdf: zero dimensions return an empty field",
          "[canvas][sdf][path][issue-641]") {
    std::uint8_t mask[1] = {255};
    REQUIRE(path_to_sdf(mask, 0, 16, 4).empty());
    REQUIRE(path_to_sdf(mask, 16, 0, 4).empty());
}

TEST_CASE("path_to_sdf: non-positive spread preserves output shape as zeroes",
          "[canvas][sdf][path][issue-641]") {
    std::uint8_t mask[4] = {255, 0, 0, 255};
    auto zero_spread = path_to_sdf(mask, 2, 2, 0);
    auto negative_spread = path_to_sdf(mask, 2, 2, -3);

    REQUIRE(zero_spread.size() == 4);
    REQUIRE(negative_spread.size() == 4);
    for (auto v : zero_spread) REQUIRE(v == 0);
    for (auto v : negative_spread) REQUIRE(v == 0);
}

TEST_CASE("path_to_sdf: null mask returns a zero field without dereferencing",
          "[canvas][sdf][path]") {
    auto sdf = path_to_sdf(nullptr, 3, 2, 4);

    REQUIRE(sdf.size() == 6);
    for (auto v : sdf) REQUIRE(v == 0);
}

TEST_CASE("path_to_sdf: mask threshold treats 127 outside and 128 inside",
          "[canvas][sdf][path][issue-641]") {
    std::uint8_t mask[2] = {127, 128};
    auto sdf = path_to_sdf(mask, 2, 1, 1);

    REQUIRE(sdf.size() == 2);
    REQUIRE(sdf[0] == 0);
    REQUIRE(sdf[1] == 255);
}

TEST_CASE("path_to_sdf: one-pixel islands saturate in both directions",
          "[canvas][sdf][path][coverage][issue-650]") {
    std::uint8_t mask[9] = {
        0, 0, 0,
        0, 255, 0,
        0, 0, 0,
    };

    auto sdf = path_to_sdf(mask, 3, 3, 1);
    REQUIRE(sdf.size() == 9);
    REQUIRE(sdf[4] == 255);
    REQUIRE(sdf[0] == 0);
    REQUIRE(sdf[1] == 0);
}
