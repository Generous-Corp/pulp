// ViewSize::aspect_ratio field tests.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
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

TEST_CASE("ViewSize zero max bounds remain unbounded when aspect is locked",
          "[format][view-size]") {
    ViewSize v;
    v.preferred_width = 1024;
    v.preferred_height = 768;
    v.min_width = 320;
    v.min_height = 240;
    v.max_width = 0;
    v.max_height = 0;
    v.aspect_ratio = static_cast<double>(v.preferred_width) / v.preferred_height;

    REQUIRE(v.max_width == 0);
    REQUIRE(v.max_height == 0);
    REQUIRE(v.aspect_ratio > 1.33);
    REQUIRE(v.aspect_ratio < 1.34);
}

namespace {

struct RecordingViewportHost {
    int viewport_calls = 0;
    float design_width = 0.0f;
    float design_height = 0.0f;
    float aspect_ratio = 0.0f;

    void set_design_viewport(float width, float height) {
        ++viewport_calls;
        design_width = width;
        design_height = height;
    }
    void set_fixed_aspect_ratio(float ratio) { aspect_ratio = ratio; }
};

ViewSize authored_view_size() {
    return ViewSize{
        990, 645, 792, 516, 2640, 1720,
        1320.0 / 860.0, 1320, 860, ViewportPolicy::FixedDesign,
    };
}

}  // namespace

TEST_CASE("editor resize preserves an authored design viewport",
          "[format][view-size][viewport-commit]") {
    RecordingViewportHost host;
    commit_editor_requested_viewport(host, authored_view_size(), 1560, 1016);

    REQUIRE(host.viewport_calls == 1);
    REQUIRE(host.design_width == 1320.0f);
    REQUIRE(host.design_height == 860.0f);
    REQUIRE(host.aspect_ratio == Catch::Approx(1320.0 / 860.0));
}

TEST_CASE("editor resize does not mix partially authored coordinates",
          "[format][view-size][viewport-commit]") {
    auto hints = authored_view_size();
    hints.design_height = 0;
    RecordingViewportHost host;
    commit_editor_requested_viewport(host, hints, 1560, 1016);

    REQUIRE(host.viewport_calls == 1);
    REQUIRE(host.design_width == 1560.0f);
    REQUIRE(host.design_height == 1016.0f);
}

TEST_CASE("legacy pinned editor adopts the requested viewport",
          "[format][view-size][viewport-commit]") {
    auto hints = authored_view_size();
    hints.design_width = 0;
    hints.design_height = 0;
    RecordingViewportHost host;
    commit_editor_requested_viewport(host, hints, 1560, 1016);

    REQUIRE(host.viewport_calls == 1);
    REQUIRE(host.design_width == 1560.0f);
    REQUIRE(host.design_height == 1016.0f);
}

TEST_CASE("responsive editor resize does not commit a design viewport",
          "[format][view-size][viewport-commit]") {
    auto hints = authored_view_size();
    hints.viewport_policy = ViewportPolicy::Responsive;
    RecordingViewportHost host;
    commit_editor_requested_viewport(host, hints, 1560, 1016);

    REQUIRE(host.viewport_calls == 0);
    REQUIRE(host.aspect_ratio == Catch::Approx(1320.0 / 860.0));
}
