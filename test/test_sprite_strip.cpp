#include <catch2/catch_test_macros.hpp>
#include <pulp/view/sprite_strip.hpp>

using namespace pulp::view;

TEST_CASE("SpriteStrip frame selection", "[view][sprite]") {
    SpriteStrip strip;
    // Simulate a 32x320 vertical strip with 10 frames (32x32 each)
    std::vector<uint8_t> data(32 * 320 * 4, 128);  // RGBA8
    strip.load(data.data(), data.size(), 32, 320, 10);

    REQUIRE(strip.loaded());
    REQUIRE(strip.frame_count() == 10);
    REQUIRE(strip.frame_width() == 32);
    REQUIRE(strip.frame_height() == 32);

    // Value 0.0 → first frame
    REQUIRE(strip.frame_for_value(0.0f) == 0);
    // Value 1.0 → last frame
    REQUIRE(strip.frame_for_value(1.0f) == 9);
    // Value 0.5 → middle frame (index 4 or 5)
    int mid = strip.frame_for_value(0.5f);
    REQUIRE(mid >= 4);
    REQUIRE(mid <= 5);
}

TEST_CASE("SpriteStrip frame offset", "[view][sprite]") {
    SpriteStrip strip;
    std::vector<uint8_t> data(64 * 640 * 4, 0);  // 64x640, 10 frames
    strip.load(data.data(), data.size(), 64, 640, 10);

    int x, y;
    strip.frame_offset(0, x, y);
    REQUIRE(x == 0);
    REQUIRE(y == 0);

    strip.frame_offset(5, x, y);
    REQUIRE(x == 0);
    REQUIRE(y == 320);  // 5 * 64
}

TEST_CASE("SpriteStrip horizontal orientation", "[view][sprite]") {
    SpriteStrip strip;
    std::vector<uint8_t> data(640 * 64 * 4, 0);
    strip.load(data.data(), data.size(), 640, 64, 10, SpriteStrip::Orientation::horizontal);

    REQUIRE(strip.frame_width() == 64);
    REQUIRE(strip.frame_height() == 64);

    int x, y;
    strip.frame_offset(3, x, y);
    REQUIRE(x == 192);  // 3 * 64
    REQUIRE(y == 0);
}

TEST_CASE("SpriteStrip empty", "[view][sprite]") {
    SpriteStrip strip;
    REQUIRE_FALSE(strip.loaded());
    REQUIRE(strip.frame_for_value(0.5f) == 0);
}
