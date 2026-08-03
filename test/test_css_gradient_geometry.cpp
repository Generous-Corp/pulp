// CSS gradient GEOMETRY, judged against Chrome's own render of the same string.
//
// Every expected number in this file was read off a Chromium screenshot of the
// exact CSS under test, at the exact box size, by
// tools/import-validation/chrome_gradient_oracle.py. That matters more here
// than anywhere else in the gradient tests: the defects this file covers are
// wrong ARITHMETIC, and a fixture computed from the same formula as the code
// agrees with it by construction — including when both are wrong. Chrome is an
// independent implementation of the same spec, so it can disagree.
//
// The gradients are hard-edged: two stops of one colour up to a boundary and a
// different colour past it, so the colour boundary traces the ending shape
// exactly and a scanline scan reads its position to the pixel.
//
// The box is 160x100 — deliberately NOT square. Every defect covered here is
// invisible on a square box: an aspect-1 box makes an ellipse a circle and
// makes a 45-degree angle its own reflection.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/css_gradient.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <memory>
#include <vector>

using namespace pulp::view;

namespace {

constexpr int kW = 160;
constexpr int kH = 100;

// Chrome rasterises the boundary with antialiasing, and so does Skia, but the
// two need not pick the same side of a half-covered pixel. One pixel of slack
// absorbs that without absorbing any defect this file exists to catch: the
// smallest error here is the ~28% radius shortfall of the old constant, which
// is tens of pixels.
constexpr int kSlack = 1;

// The two gradient colours are pure red and pure blue, so `inside` is simply
// whichever channel dominates — an antialiased boundary pixel is a blend of the
// two and falls on the side it is mostly on.
//
// The view's own background is GREEN, which is neither, so a gradient that
// failed to paint shows up as green rather than being read as one enormous
// shape. That is not hypothetical: with a white backdrop and a red-vs-not test,
// an unpainted view reads as 100% inside on every scanline — which looks
// exactly like a gradient that covers the box, and would have made a blank
// render pass the `farthest-corner` cases.
struct Frame {
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    const uint8_t* at(int x, int y) const {
        return &rgba[(static_cast<size_t>(y) * w + x) * 4];
    }
    bool inside(int x, int y) const {
        const uint8_t* p = at(x, y);
        return p[0] > p[2];
    }
    /// Neither gradient colour reached this pixel. Red and blue are the only
    /// two things the layer paints and an antialiased boundary is a blend of
    /// them, so r+b stays high across the whole box wherever it painted at
    /// all — while the backdrop, a cleared surface and a transparent pixel are
    /// all low. Testing for the backdrop's own colour would miss the last two.
    bool unpainted(int x, int y) const {
        const uint8_t* p = at(x, y);
        return static_cast<int>(p[0]) + static_cast<int>(p[2]) < 100;
    }
};

// Order matters: `layout_first` applies the CSS to a view that already knows
// its size, `style_first` applies it before the box exists — which is what the
// design importer does, because the whole style pass runs ahead of Yoga. Any
// geometry resolved when the CSS is parsed is resolved against a degenerate
// box in the second order, so both orders must produce the same picture.
enum class Order { layout_first, style_first };

std::optional<Frame> render_css(const std::string& css, Order order) {
    View v;
    v.set_background_color({0, 255, 0, 255});
    if (order == Order::layout_first)
        v.set_bounds({0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)});
    if (!apply_css_background_gradient(v, css)) return std::nullopt;
    if (order == Order::style_first)
        v.set_bounds({0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)});
    Frame f;
    f.rgba = render_to_rgba(v, kW, kH, 1.0f, &f.w, &f.h);
    if (f.rgba.empty()) return std::nullopt;
    return f;
}

// First and last x on row `y` that is inside the ending shape, or {-1,-1} when
// the row is entirely outside it.
std::pair<int, int> row_span(const Frame& f, int y) {
    int lo = -1, hi = -1;
    for (int x = 0; x < static_cast<int>(f.w); ++x) {
        if (!f.inside(x, y)) continue;
        if (lo < 0) lo = x;
        hi = x;
    }
    return {lo, hi};
}

std::pair<int, int> col_span(const Frame& f, int x) {
    int lo = -1, hi = -1;
    for (int y = 0; y < static_cast<int>(f.h); ++y) {
        if (!f.inside(x, y)) continue;
        if (lo < 0) lo = y;
        hi = y;
    }
    return {lo, hi};
}

void require_near(int actual, int expected, const char* what, int at) {
    INFO(what << " at " << at << ": got " << actual << ", Chrome " << expected);
    REQUIRE(actual >= expected - kSlack);
    REQUIRE(actual <= expected + kSlack);
}

struct Span { int at, lo, hi; };

// Assert a whole scan against Chrome's, in BOTH application orders. A defect
// that only shows up pre-layout is the one this file was written for, so a
// pass in one order is not a pass.
void require_geometry(const std::string& css,
                      const std::vector<Span>& rows,
                      const std::vector<Span>& cols) {
    for (const Order order : {Order::layout_first, Order::style_first}) {
        INFO("css: " << css << "\norder: "
                     << (order == Order::layout_first ? "layout first"
                                                      : "style first"));
        const auto frame = render_css(css, order);
        REQUIRE(frame.has_value());
        // A gradient covers its whole box. Any surviving backdrop means the
        // layer did not paint, and every span below would then be measuring
        // the absence of a gradient rather than its geometry.
        size_t unpainted = 0;
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x)
                if (frame->unpainted(x, y)) ++unpainted;
        INFO("unpainted (backdrop-coloured) pixels: " << unpainted);
        REQUIRE(unpainted == 0);
        for (const auto& r : rows) {
            const auto got = row_span(*frame, r.at);
            require_near(got.first, r.lo, "row lo", r.at);
            require_near(got.second, r.hi, "row hi", r.at);
        }
        for (const auto& c : cols) {
            const auto got = col_span(*frame, c.at);
            require_near(got.first, c.lo, "col lo", c.at);
            require_near(got.second, c.hi, "col hi", c.at);
        }
    }
}

}  // namespace

// A two-value size is an ELLIPSE with one radius per axis, and the radii are
// percentages of the box's width and height independently. Modelling it as a
// circle of a fixed fraction of the larger side gets the shape, the size and
// the aspect all wrong at once — and this is the form that dominates real
// captured panels, where a soft screen-sized wash is written exactly this way.
TEST_CASE("an explicitly sized radial gradient is an ellipse over the box",
          "[view][gradient][radial][geometry]") {
    require_geometry(
        "radial-gradient(90% 70% at 50% 30%, #ff0000 0%, #ff0000 99.9%, #0000ff 100%)",
        /*rows=*/{{99, 63, 96}},
        /*cols=*/{{0, 0, 87}, {40, 0, 96}, {80, 0, 99}, {120, 0, 96},
                  {159, 0, 87}});
}

// `closest-side` measures from the gradient's OWN centre to the nearest edge on
// each axis, so an off-centre `at` gives it a different radius on all four
// sides. A constant fraction of the box cannot express that at all.
TEST_CASE("closest-side measures from the gradient's centre, not the box's",
          "[view][gradient][radial][geometry]") {
    require_geometry(
        "radial-gradient(closest-side at 30% 60%, #ff0000 0%, #ff0000 99.9%, #0000ff 100%)",
        /*rows=*/{{25, 24, 71}, {50, 1, 94}, {75, 4, 91}, {99, 41, 54}},
        /*cols=*/{{0, 54, 65}, {40, 21, 98}, {80, 31, 88}});
}

TEST_CASE("farthest-side reaches the far edge on each axis",
          "[view][gradient][radial][geometry]") {
    require_geometry(
        "radial-gradient(farthest-side at 30% 60%, #ff0000 0%, #ff0000 99.9%, #0000ff 100%)",
        /*rows=*/{{0, 34, 61}, {25, 0, 139}, {50, 0, 158}, {75, 0, 155},
                  {99, 0, 131}},
        /*cols=*/{{0, 6, 99}, {120, 14, 99}, {159, 55, 64}});
}

// The corner keywords are NOT the distance to the corner. The spec gives the
// ending ellipse the aspect ratio of the matching `-side` keyword and then
// scales it until it passes through that corner, which works out to exactly
// sqrt(2) on each axis — noticeably larger than the corner distance.
TEST_CASE("closest-corner keeps the closest-side aspect and reaches the corner",
          "[view][gradient][radial][geometry]") {
    require_geometry(
        "radial-gradient(closest-corner at 30% 60%, #ff0000 0%, #ff0000 99.9%, #0000ff 100%)",
        /*rows=*/{{25, 0, 101}, {50, 0, 114}, {75, 0, 112}, {99, 0, 96}},
        /*cols=*/{{0, 20, 99}, {40, 4, 99}, {80, 10, 99}});
}

// farthest-corner — the CSS default — always covers the whole box, so its size
// is only observable partway along the gradient line. The boundary here sits at
// half the ending shape. The old code approximated this as 0.7071*max(w,h),
// which on this box is 113px against a true horizontal radius of 170px.
TEST_CASE("farthest-corner is an ellipse scaled to the far corner",
          "[view][gradient][radial][geometry]") {
    require_geometry(
        "radial-gradient(farthest-corner at 25% 40%, #ff0000 0%, #ff0000 49.9%, #0000ff 50%)",
        /*rows=*/{{0, 9, 70}, {25, 0, 119}, {50, 0, 121}, {75, 0, 85}},
        /*cols=*/{{0, 2, 77}, {40, 0, 81}, {80, 3, 76}, {120, 27, 52}});
}

// With no prefix at all a radial gradient is `farthest-corner ellipse at
// center` — so the default path is an ellipse too, not a circle.
TEST_CASE("a bare radial gradient defaults to a farthest-corner ellipse",
          "[view][gradient][radial][geometry]") {
    require_geometry(
        "radial-gradient(#ff0000 0%, #ff0000 49.9%, #0000ff 50%)",
        /*rows=*/{{25, 39, 120}, {50, 23, 136}, {75, 41, 118}},
        /*cols=*/{{40, 25, 74}, {80, 15, 84}, {120, 25, 74}});
}

// `circle` opts out of the per-axis radii: one radius, measured as a true
// distance rather than per-axis. It must NOT pick up the ellipse squash.
TEST_CASE("a circle keyword keeps one radius on both axes",
          "[view][gradient][radial][geometry]") {
    require_geometry(
        "radial-gradient(circle farthest-corner at 25% 40%, "
        "#ff0000 0%, #ff0000 49.9%, #0000ff 50%)",
        /*rows=*/{{0, 0, 93}, {25, 0, 104}, {50, 0, 105}, {75, 0, 96},
                  {99, 9, 70}},
        /*cols=*/{{0, 0, 93}, {40, 0, 99}, {80, 0, 92}});
}

TEST_CASE("an explicit circle radius is honoured in pixels",
          "[view][gradient][radial][geometry]") {
    require_geometry(
        "radial-gradient(circle 40px at 50% 50%, #ff0000 0%, #ff0000 99.9%, #0000ff 100%)",
        /*rows=*/{{25, 48, 111}, {50, 40, 119}, {75, 49, 110}},
        /*cols=*/{{40, 44, 55}, {80, 10, 89}});
}

// An angled linear gradient's endpoints are a function of the box: the line
// runs through the centre and its length is the box's projection onto it. The
// endpoints used to be baked when the CSS was parsed, which — in the importer's
// order, where style runs before layout — meant baking them against a 1:1 box,
// so every angled gradient on a non-square box pointed the wrong way. That is
// why each case here runs in both orders.
TEST_CASE("an angled linear gradient resolves against the laid-out box",
          "[view][gradient][linear][geometry]") {
    require_geometry(
        "linear-gradient(45deg, #ff0000 0%, #ff0000 49.9%, #0000ff 50%)",
        /*rows=*/{{0, 0, 29}, {25, 0, 54}, {50, 0, 79}, {75, 0, 104},
                  {99, 0, 128}},
        /*cols=*/{});
    require_geometry(
        "linear-gradient(150deg, #ff0000 0%, #ff0000 49.9%, #0000ff 50%)",
        /*rows=*/{{0, 0, 159}, {25, 0, 121}, {50, 0, 78}, {75, 0, 35}},
        /*cols=*/{});
}

// A corner keyword's angle is itself a function of the aspect ratio — the
// gradient line is perpendicular to the other diagonal so the end colour lands
// exactly on the named corner — so it cannot be one of four fixed vectors and
// cannot be resolved before layout either.
TEST_CASE("a to-corner linear gradient resolves against the laid-out box",
          "[view][gradient][linear][geometry]") {
    require_geometry(
        "linear-gradient(to bottom right, #ff0000 0%, #ff0000 49.9%, #0000ff 50%)",
        /*rows=*/{{0, 0, 158}, {25, 0, 118}, {50, 0, 78}, {75, 0, 38}},
        /*cols=*/{});
}

// CSS interpolates gradient colours in PREMULTIPLIED space (css-images-3 §3.4.2)
// precisely so that a fade to `transparent` fades only the alpha. Interpolating
// unpremultiplied instead drags the colour toward the transparent stop's RGB —
// which is black for the `rgba(0, 0, 0, 0)` that Chromium serializes
// `transparent` into — so the ramp greys out and the wash lands far too dark.
//
// This is invisible on the opaque-stop gradients the rest of the suite uses,
// and it is exactly the form real captured panels use for a soft screen-sized
// wash: a tinted centre fading to a fully transparent edge.
//
// The gradient under test is layered over an opaque white layer of its own, so
// the composited colour is read without depending on what the surface behind it
// was cleared to.
TEST_CASE("a fade to transparent keeps its hue",
          "[view][gradient][linear][geometry]") {
    const std::string css =
        "linear-gradient(to right, rgba(0, 0, 255, 1) 0%, rgba(0, 0, 0, 0) 100%), "
        "linear-gradient(to right, rgb(255, 255, 255) 0%, rgb(255, 255, 255) 100%)";
    // Chromium's own render of that string, sampled along the middle row.
    const std::vector<std::array<int, 4>> expected = {
        {0, 1, 1, 255}, {40, 65, 65, 255}, {80, 128, 128, 255},
        {120, 192, 192, 255}, {159, 255, 255, 255},
    };
    for (const Order order : {Order::layout_first, Order::style_first}) {
        INFO("order: " << (order == Order::layout_first ? "layout first"
                                                        : "style first"));
        const auto frame = render_css(css, order);
        REQUIRE(frame.has_value());
        for (const auto& e : expected) {
            const uint8_t* p = frame->at(e[0], kH / 2);
            INFO("x=" << e[0] << " got " << int(p[0]) << "," << int(p[1]) << ","
                      << int(p[2]) << "  Chrome " << e[1] << "," << e[2] << ","
                      << e[3]);
            // The blue channel is the discriminator: premultiplied holds it at
            // 255 the whole way, unpremultiplied decays it toward 0.
            REQUIRE(std::abs(int(p[2]) - e[3]) <= 2);
            REQUIRE(std::abs(int(p[0]) - e[1]) <= 2);
        }
    }
}
