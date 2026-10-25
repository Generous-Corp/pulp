// CSS backdrop-filter under opacity, judged against Chrome's own render.
//
// The tree below is test/fixtures/import-differential/21-backdrop-opacity.html
// built by hand: five `backdrop-filter: blur(8px)` boxes over 1px gridlines.
// Every expected pixel was read off Chromium's render of that file by
// tools/import-validation/chrome_backdrop_opacity_oracle.py. Chrome is the
// oracle because the composite is not obvious from the spec: it draws the
// filtered backdrop at the element's opacity and then the element over it at
// that opacity, so a gridline under an opacity 0.5 box keeps about a third of
// its contrast rather than half, and none of it is lost at opacity 0.
//
// A gridline pixel and a gap pixel are sampled under each box. The two agree
// when the backdrop is fully blurred and differ by the gridline's full
// contrast when it is not blurred at all, so the pair says how much of the
// blur reached the screen.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::view;
using pulp::canvas::Color;

namespace {

constexpr int kW = 512;
constexpr int kH = 160;
constexpr int kRow = 80;

// Chrome and Skia blur with different kernels and round the composite
// differently, which moves a channel by a few levels. The defect this file
// covers moves a gridline under an opacity 0 box by more than 100.
constexpr int kTolerance = 6;

struct Sample {
    const char* box;
    int line_x;
    std::array<int, 3> line;
    int gap_x;
    std::array<int, 3> gap;
};

// chrome_backdrop_opacity_oracle.py output, Chrome 151.0.7922.47 and
// 156.0.8078.48 alike.
constexpr Sample kChrome[] = {
    {"opacity-0", 55, {128, 144, 160}, 50, {11, 15, 20}},
    {"opacity-half", 155, {59, 68, 78}, 150, {16, 20, 25}},
    {"opacity-1", 245, {17, 22, 28}, 240, {17, 22, 28}},
    {"group-half", 345, {99, 112, 125}, 340, {11, 15, 20}},
    {"group-0", 435, {128, 144, 160}, 430, {11, 15, 20}},
};

std::unique_ptr<View> box(float left, float top) {
    auto v = std::make_unique<View>();
    v->set_bounds({left, top, 80.0f, 80.0f});
    return v;
}

std::unique_ptr<View> frost(float left, float top, float opacity) {
    auto v = box(left, top);
    v->set_background_color(Color::rgba8(12, 16, 22, 128));
    v->set_backdrop_blur(8.0f);
    v->set_opacity(opacity);
    return v;
}

std::unique_ptr<View> group(float left, float opacity) {
    auto g = box(left, 40.0f);
    g->set_opacity(opacity);
    g->add_child(frost(0.0f, 0.0f, 1.0f));
    return g;
}

std::vector<uint8_t> render_fixture(uint32_t& w, uint32_t& h) {
    View root;
    root.set_background_color(Color::rgba8(11, 15, 20));
    for (int x = 5; x < kW; x += 10) {
        auto line = std::make_unique<View>();
        line->set_bounds({static_cast<float>(x), 0.0f, 1.0f, static_cast<float>(kH)});
        line->set_background_color(Color::rgba8(128, 144, 160));
        root.add_child(std::move(line));
    }
    root.add_child(frost(16.0f, 40.0f, 0.0f));
    root.add_child(frost(112.0f, 40.0f, 0.5f));
    root.add_child(frost(208.0f, 40.0f, 1.0f));
    root.add_child(group(304.0f, 0.5f));
    root.add_child(group(400.0f, 0.0f));
    return render_to_rgba(root, kW, kH, 1.0f, &w, &h);
}

void require_pixel(const std::vector<uint8_t>& rgba, uint32_t w, int x,
                   const std::array<int, 3>& chrome, const std::string& what) {
    const uint8_t* p = &rgba[(static_cast<size_t>(kRow) * w + x) * 4];
    for (int c = 0; c < 3; ++c) {
        INFO(what << " x=" << x << " channel " << c << ": got " << static_cast<int>(p[c])
                  << ", Chrome " << chrome[c]);
        REQUIRE(std::abs(static_cast<int>(p[c]) - chrome[c]) <= kTolerance);
    }
}

} // namespace

TEST_CASE("backdrop-filter is composited at the element's opacity, as Chrome does",
          "[view][canvas][backdrop-filter]") {
    if (!raw_rgba_render_available())
        SKIP("no raw-RGBA render backend in this build");
    uint32_t w = 0, h = 0;
    const auto rgba = render_fixture(w, h);
    REQUIRE(w == static_cast<uint32_t>(kW));
    REQUIRE(h == static_cast<uint32_t>(kH));
    for (const auto& s : kChrome) {
        require_pixel(rgba, w, s.line_x, s.line, std::string(s.box) + " gridline");
        require_pixel(rgba, w, s.gap_x, s.gap, std::string(s.box) + " gap");
    }
}
