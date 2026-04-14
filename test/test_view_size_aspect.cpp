// ViewSize::aspect_ratio field tests (workstream 07 slice 7.5b).

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/processor.hpp>

using namespace pulp::format;

TEST_CASE("ViewSize::aspect_ratio defaults to 0 (free resize)",
          "[format][view-size]") {
    ViewSize v;
    REQUIRE(v.aspect_ratio == 0.0);
}

TEST_CASE("plugins can declare 16:9 aspect lock", "[format][view-size]") {
    ViewSize v;
    v.preferred_width  = 1280;
    v.preferred_height = 720;
    v.min_width  = 320;
    v.min_height = 180;
    v.max_width  = 3840;
    v.max_height = 2160;
    v.aspect_ratio = 16.0 / 9.0;
    REQUIRE(v.aspect_ratio > 1.77);
    REQUIRE(v.aspect_ratio < 1.78);
}

TEST_CASE("aspect ratio is independent of min/max bounds",
          "[format][view-size]") {
    ViewSize a;
    a.preferred_width  = 800;
    a.preferred_height = 600;
    a.aspect_ratio = 4.0 / 3.0;

    ViewSize b = a;
    b.min_width  = 400;
    b.min_height = 300;

    REQUIRE(a.aspect_ratio == b.aspect_ratio);
    REQUIRE(a.min_width == 0);
    REQUIRE(b.min_width == 400);
}
