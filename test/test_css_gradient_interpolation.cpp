// CSS gradient colour-interpolation methods (`in oklab`, `in oklch longer hue`).
//
// The expected colours are Chrome's, read by
// tools/import-validation/chrome_gradient_oracle.py --colors: each case paints
// its two stops (red to blue unless the table says otherwise) left to right
// across a 101x16 box, and the number is the mean of column 50 (t = 0.5
// exactly), which also cancels Chrome's dither. Pulp paints the same CSS in the
// same box and is read the same way. A red-to-blue gradient that silently fell
// back to sRGB reads (127.5, 0, 127.5), which is what the planted negative
// below pins.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/gradient_interpolation.hpp>
#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/view/css_gradient.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace pulp::view;
using pulp::canvas::Color;
using pulp::canvas::DrawCommand;
using pulp::canvas::GradientColorSpace;
using pulp::canvas::GradientHueMethod;
using pulp::canvas::GradientInterpolation;

namespace {

constexpr int kW = 101;
constexpr int kH = 16;

// Skia and Chrome share a gradient implementation but not a build, and the
// column mean still carries a fraction of a code value of dither. 3 code values
// per channel holds every case below with room to spare, while the nearest
// wrong answer (plain sRGB against Oklab) is 83 away in green.
constexpr double kChromeTolerance = 3.0;

struct ChromeMidpoint {
    const char* css;
    GradientInterpolation method;
    std::array<double, 3> rgb;
    Color end = Color::rgba(0.0f, 0.0f, 1.0f, 1.0f); ///< the stop the ramp ends on
};

// chrome_gradient_oracle.py --colors, Chrome 156, column 50.
const ChromeMidpoint kChromeMidpoints[] = {
    {"linear-gradient(to right, #ff0000, #0000ff)", {}, {127.5, 0.0, 127.5}},
    {"linear-gradient(to right in oklab, #ff0000, #0000ff)",
     {GradientColorSpace::oklab},
     {140.25, 83.0, 162.25}},
    {"linear-gradient(to right in oklch, #ff0000, #0000ff)",
     {GradientColorSpace::oklch},
     {186.0, 0.0, 193.75}},
    {"linear-gradient(to right in oklch shorter hue, #ff0000, #0000ff)",
     {GradientColorSpace::oklch, GradientHueMethod::shorter},
     {186.0, 0.0, 193.75}},
    {"linear-gradient(to right in oklch longer hue, #ff0000, #0000ff)",
     {GradientColorSpace::oklch, GradientHueMethod::longer},
     {0.0, 146.75, 0.0}},
    {"linear-gradient(to right in oklch increasing hue, #ff0000, #0000ff)",
     {GradientColorSpace::oklch, GradientHueMethod::increasing},
     {0.0, 146.75, 0.0}},
    {"linear-gradient(to right in oklch decreasing hue, #ff0000, #0000ff)",
     {GradientColorSpace::oklch, GradientHueMethod::decreasing},
     {186.0, 0.0, 193.75}},
    {"linear-gradient(to right in srgb-linear, #ff0000, #0000ff)",
     {GradientColorSpace::srgb_linear},
     {187.5, 0.0, 187.5}},
    {"linear-gradient(to right in lab, #ff0000, #0000ff)",
     {GradientColorSpace::lab},
     {193.0, 0.0, 136.0}},
    {"linear-gradient(to right in hsl, #ff0000, #0000ff)",
     {GradientColorSpace::hsl},
     {255.0, 0.0, 255.0}},
    {"linear-gradient(to right in display-p3, #ff0000, #0000ff)",
     {GradientColorSpace::display_p3},
     {127.88, 9.75, 145.0}},
    // Red to yellow is a short arc, so `longer` must turn the long way round
    // through blue; red to blue above is already the long way for `longer`.
    {"linear-gradient(to right in oklch longer hue, #ff0000, #ffff00)",
     {GradientColorSpace::oklch, GradientHueMethod::longer},
     {0.0, 192.75, 255.0},
     Color::rgba(1.0f, 1.0f, 0.0f, 1.0f)},
    {"linear-gradient(to right in hsl longer hue, #ff0000, #ffff00)",
     {GradientColorSpace::hsl, GradientHueMethod::longer},
     {0.0, 127.5, 255.0},
     Color::rgba(1.0f, 1.0f, 0.0f, 1.0f)},
};

const ChromeMidpoint& chrome_oklab() {
    return kChromeMidpoints[1];
}

double max_channel_error(const std::array<double, 3>& got, const std::array<double, 3>& want) {
    double worst = 0.0;
    for (int i = 0; i < 3; ++i)
        worst = std::max(worst, std::fabs(got[i] - want[i]));
    return worst;
}

std::array<double, 3> column_mean(const std::vector<uint8_t>& px, int w, int h, int x) {
    std::array<double, 3> sum{};
    for (int y = 0; y < h; ++y) {
        const uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
        for (int i = 0; i < 3; ++i)
            sum[i] += p[i];
    }
    for (auto& v : sum)
        v /= h;
    return sum;
}

std::array<double, 3> raster_midpoint(View& v) {
    uint32_t w = 0, h = 0;
    const auto px = render_to_rgba(v, kW, kH, 1.0f, &w, &h);
    REQUIRE(w == static_cast<uint32_t>(kW));
    REQUIRE(h == static_cast<uint32_t>(kH));
    return column_mean(px, kW, kH, kW / 2);
}

std::string rgb_text(const std::array<double, 3>& c) {
    return "(" + std::to_string(c[0]) + ", " + std::to_string(c[1]) + ", " + std::to_string(c[2]) +
           ")";
}

// The interpolation the top layer of `css` parsed to; fails the test if the
// CSS was refused.
GradientInterpolation parsed(const std::string& css) {
    View v;
    v.set_bounds({0.0f, 0.0f, 160.0f, 100.0f});
    INFO(css);
    REQUIRE(apply_css_background_gradient(v, css));
    REQUIRE_FALSE(v.background_gradient_layers().empty());
    return v.background_gradient_layers().front().interpolation;
}

bool refused(const std::string& css) {
    View v;
    v.set_bounds({0.0f, 0.0f, 160.0f, 100.0f});
    return !apply_css_background_gradient(v, css);
}

} // namespace

TEST_CASE("Interpolation method parses in every prefix position",
          "[css][gradient][interpolation]") {
    const GradientInterpolation oklab{GradientColorSpace::oklab};
    const GradientInterpolation hsl{GradientColorSpace::hsl};
    const GradientInterpolation oklch{GradientColorSpace::oklch};

    // Alone, after the direction, before it, and after an angle.
    CHECK(parsed("linear-gradient(in oklab, #ff0000, #0000ff)") == oklab);
    CHECK(parsed("linear-gradient(to right in oklab, #ff0000, #0000ff)") == oklab);
    CHECK(parsed("linear-gradient(in oklab to right, #ff0000, #0000ff)") == oklab);
    CHECK(parsed("linear-gradient(90deg in oklab, #ff0000, #0000ff)") == oklab);
    CHECK(parsed("repeating-linear-gradient(in oklab, #ff0000 0px, #0000ff 10px)") == oklab);
    CHECK(parsed("radial-gradient(in hsl, #ff0000, #0000ff)") == hsl);
    CHECK(parsed("radial-gradient(circle in hsl, #ff0000, #0000ff)") == hsl);
    CHECK(parsed("radial-gradient(in hsl circle at 30% 60%, #ff0000, #0000ff)") == hsl);
    CHECK(parsed("conic-gradient(in oklch, #ff0000, #0000ff)") == oklch);
    CHECK(parsed("conic-gradient(from 90deg in oklch, #ff0000, #0000ff)") == oklch);
    CHECK(parsed("conic-gradient(in oklch from 90deg, #ff0000, #0000ff)") == oklch);
    // CSS keywords are ASCII case-insensitive.
    CHECK(parsed("linear-gradient(IN OKLab, #ff0000, #0000ff)") == oklab);
}

TEST_CASE("Removing the interpolation clause leaves the gradient geometry intact",
          "[css][gradient][interpolation]") {
    View plain, interp;
    for (View* v : {&plain, &interp})
        v->set_bounds({0.0f, 0.0f, 160.0f, 100.0f});

    REQUIRE(apply_css_background_gradient(plain, "linear-gradient(to right, #ff0000, #0000ff)"));
    REQUIRE(apply_css_background_gradient(interp,
                                          "linear-gradient(in oklab to right, #ff0000, #0000ff)"));
    const auto& a = plain.background_gradient_layers().front();
    const auto& b = interp.background_gradient_layers().front();
    CHECK(b.x0 == a.x0);
    CHECK(b.y0 == a.y0);
    CHECK(b.x1 == a.x1);
    CHECK(b.y1 == a.y1);
    CHECK(b.colors.size() == 2);

    REQUIRE(apply_css_background_gradient(plain, "radial-gradient(circle at 30% 60%, #f00, #00f)"));
    REQUIRE(apply_css_background_gradient(interp,
                                          "radial-gradient(circle at 30% 60% in hsl, #f00, #00f)"));
    CHECK(interp.background_gradient_layers().front().radial_circle);
    CHECK(interp.background_gradient_layers().front().x0 ==
          plain.background_gradient_layers().front().x0);

    REQUIRE(apply_css_background_gradient(plain, "conic-gradient(from 90deg, #f00, #00f)"));
    REQUIRE(
        apply_css_background_gradient(interp, "conic-gradient(from 90deg in oklch, #f00, #00f)"));
    CHECK(interp.background_gradient_layers().front().angle ==
          plain.background_gradient_layers().front().angle);
}

TEST_CASE("Hue methods parse on polar spaces", "[css][gradient][interpolation]") {
    const auto with_hue = [](const char* hue) {
        return parsed(std::string("linear-gradient(in oklch ") + hue + " hue, #ff0000, #0000ff)")
            .hue;
    };
    CHECK(with_hue("shorter") == GradientHueMethod::shorter);
    CHECK(with_hue("longer") == GradientHueMethod::longer);
    CHECK(with_hue("increasing") == GradientHueMethod::increasing);
    CHECK(with_hue("decreasing") == GradientHueMethod::decreasing);
    CHECK(parsed("linear-gradient(in hsl longer hue, #f00, #00f)") ==
          GradientInterpolation{GradientColorSpace::hsl, GradientHueMethod::longer});
    CHECK(parsed("linear-gradient(in oklch, #f00, #00f)").hue == GradientHueMethod::shorter);
}

TEST_CASE("Linear-light aliases resolve to srgb-linear", "[css][gradient][interpolation]") {
    // Chrome paints all four identically to `in srgb-linear` (oracle: 187.5,
    // 0, 187.5 at the midpoint), because each is a linear transform of it.
    for (const char* space : {"xyz", "xyz-d50", "xyz-d65", "display-p3-linear"}) {
        INFO(space);
        CHECK(parsed(std::string("linear-gradient(in ") + space + ", #f00, #00f)").space ==
              GradientColorSpace::srgb_linear);
    }
}

TEST_CASE("Unsupported interpolation methods are refused, as Chrome refuses them",
          "[css][gradient][interpolation]") {
    // Chrome drops each of these declarations and paints the backdrop.
    CHECK(refused("linear-gradient(to right in bogus, #f00, #00f)"));
    CHECK(refused("linear-gradient(to right in oklab longer hue, #f00, #00f)"));
    CHECK(refused("linear-gradient(in, #f00, #00f)"));
    CHECK(refused("linear-gradient(in oklch sideways hue, #f00, #00f)"));
    CHECK(refused("radial-gradient(circle in --custom-profile, #f00, #00f)"));
    CHECK(refused("conic-gradient(from 90deg in srgb longer hue, #f00, #00f)"));
}

TEST_CASE("A gradient that names no space defaults by its stops",
          "[css][gradient][interpolation]") {
    // Legacy sRGB stops blend in sRGB; one non-legacy stop moves the default
    // to Oklab, which is why Chrome's computed style omits `in oklab` there.
    CHECK(parsed("linear-gradient(#ff0000, rgb(0, 0, 255))") == GradientInterpolation{});
    CHECK(parsed("linear-gradient(90deg, oklab(0.6 0.2 0.1), rgb(0, 0, 255))").space ==
          GradientColorSpace::oklab);
    CHECK(parsed("radial-gradient(circle, oklch(0.6 0.2 30), #00f)").space ==
          GradientColorSpace::oklab);
    // An explicit space always wins.
    CHECK(parsed("linear-gradient(in srgb, oklab(0.6 0.2 0.1), #00f)") == GradientInterpolation{});
}

TEST_CASE("The reference interpolation matches Chrome's midpoints",
          "[canvas][gradient][interpolation]") {
    const Color red = Color::rgba(1.0f, 0.0f, 0.0f, 1.0f);
    for (const auto& c : kChromeMidpoints) {
        const Color m = pulp::canvas::interpolate_gradient_color(red, c.end, 0.5f, c.method);
        const std::array<double, 3> got{m.r * 255.0, m.g * 255.0, m.b * 255.0};
        INFO(c.css << "\n  reference " << rgb_text(got) << " Chrome " << rgb_text(c.rgb));
        CHECK(max_channel_error(got, c.rgb) <= kChromeTolerance);
    }
}

TEST_CASE("Resampling keeps the stops and adds the curve between them",
          "[canvas][gradient][interpolation]") {
    const Color stops[] = {Color::rgba(1, 0, 0, 1), Color::rgba(0, 0, 1, 1),
                           Color::rgba(0, 1, 0, 1)};
    const float positions[] = {0.0f, 0.5f, 0.5f}; // a hard stop at 0.5
    std::vector<Color> colors;
    std::vector<float> at;

    pulp::canvas::resample_gradient_stops(stops, positions, 3, {}, 8, colors, at);
    CHECK(colors.size() == 3);

    pulp::canvas::resample_gradient_stops(stops, positions, 3, {GradientColorSpace::oklab}, 8,
                                          colors, at);
    // Seven interior samples in the first segment, none across the hard stop.
    REQUIRE(colors.size() == 10);
    CHECK(at.front() == 0.0f);
    CHECK(at[4] == 0.25f);
    CHECK(colors[8] == stops[1]);
    CHECK(colors[9] == stops[2]);
    const Color mid = pulp::canvas::interpolate_gradient_color(stops[0], stops[1], 0.5f,
                                                               {GradientColorSpace::oklab});
    CHECK(colors[4] == mid);
}

TEST_CASE("The view hands a layer's interpolation to the canvas around its shader",
          "[css][gradient][interpolation]") {
    View v;
    v.set_bounds({0.0f, 0.0f, 100.0f, 20.0f});
    REQUIRE(apply_css_background_gradient(v, "linear-gradient(to right in oklch longer hue, "
                                             "#ff0000, #0000ff)"));
    pulp::canvas::RecordingCanvas rec;
    v.paint_all(rec);
    const auto& cmds = rec.commands();
    int set_at = -1, gradient_at = -1, reset_at = -1;
    for (int i = 0; i < static_cast<int>(cmds.size()); ++i) {
        const auto& c = cmds[static_cast<size_t>(i)];
        if (c.type == DrawCommand::Type::set_gradient_interpolation) {
            if (set_at < 0)
                set_at = i;
            else
                reset_at = i;
        }
        if (c.type == DrawCommand::Type::set_fill_gradient_linear && gradient_at < 0)
            gradient_at = i;
    }
    REQUIRE(set_at >= 0);
    REQUIRE(gradient_at > set_at);
    REQUIRE(reset_at > gradient_at);
    CHECK(cmds[static_cast<size_t>(set_at)].f[0] == static_cast<float>(GradientColorSpace::oklch));
    CHECK(cmds[static_cast<size_t>(set_at)].f[1] == static_cast<float>(GradientHueMethod::longer));
    CHECK(cmds[static_cast<size_t>(reset_at)].f[0] == static_cast<float>(GradientColorSpace::srgb));
    CHECK(rec.gradient_interpolation() == GradientInterpolation{});

    // A gradient that names no space leaves the command stream as it was.
    View plain;
    plain.set_bounds({0.0f, 0.0f, 100.0f, 20.0f});
    REQUIRE(apply_css_background_gradient(plain, "linear-gradient(to right, #ff0000, #0000ff)"));
    pulp::canvas::RecordingCanvas plain_rec;
    plain.paint_all(plain_rec);
    CHECK(plain_rec.count(DrawCommand::Type::set_gradient_interpolation) == 0);
    CHECK(plain_rec.count(DrawCommand::Type::set_fill_gradient_linear) == 1);
}

TEST_CASE("Skia raster midpoints match Chrome for every interpolation method",
          "[css][gradient][interpolation][skia]") {
    if (!raw_rgba_render_available())
        SKIP("No raster backend in this build.");
    for (const auto& c : kChromeMidpoints) {
        View v;
        v.set_bounds({0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)});
        REQUIRE(apply_css_background_gradient(v, c.css));
        const auto got = raster_midpoint(v);
        INFO(c.css << "\n  Pulp " << rgb_text(got) << " Chrome " << rgb_text(c.rgb));
        CHECK(max_channel_error(got, c.rgb) <= kChromeTolerance);
    }
}

// Planted negative: the same Oklab CSS with its layer forced back to sRGB must
// MISS Chrome's Oklab midpoint. Without this, a Skia mapping that ignored the
// space entirely would be indistinguishable from one that honoured it on a
// case whose sRGB and Oklab midpoints happened to agree.
TEST_CASE("A gradient forced to sRGB fails the Oklab midpoint",
          "[css][gradient][interpolation][skia]") {
    if (!raw_rgba_render_available())
        SKIP("No raster backend in this build.");
    View v;
    v.set_bounds({0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)});
    REQUIRE(apply_css_background_gradient(v, chrome_oklab().css));
    const auto honoured = raster_midpoint(v);

    auto layers = v.background_gradient_layers();
    REQUIRE(layers.front().interpolation.space == GradientColorSpace::oklab);
    layers.front().interpolation = {};
    v.set_background_gradient_layers(std::move(layers));
    const auto forced = raster_midpoint(v);

    INFO("Oklab " << rgb_text(honoured) << ", forced sRGB " << rgb_text(forced) << ", Chrome "
                  << rgb_text(chrome_oklab().rgb));
    CHECK(max_channel_error(honoured, chrome_oklab().rgb) <= kChromeTolerance);
    CHECK(max_channel_error(forced, chrome_oklab().rgb) > 10.0 * kChromeTolerance);
}

#ifdef __APPLE__
// test/png_rgba_decode_apple.cpp, linked into this group on Apple.
std::vector<uint8_t> decode_png_rgba(const std::vector<uint8_t>& png, size_t& w, size_t& h);

namespace {

std::array<double, 3> cg_midpoint(const std::string& css) {
    View v;
    v.set_bounds({0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)});
    INFO(css);
    REQUIRE(apply_css_background_gradient(v, css));
    const auto png = render_to_png(v, kW, kH, 1.0f, ScreenshotBackend::coregraphics);
    REQUIRE_FALSE(png.empty());
    size_t pw = 0, ph = 0;
    const auto px = decode_png_rgba(png, pw, ph);
    REQUIRE(pw == static_cast<size_t>(kW));
    REQUIRE(ph == static_cast<size_t>(kH));
    return column_mean(px, kW, kH, kW / 2);
}

} // namespace

// CoreGraphics blends only in sRGB, so it resamples the curve into extra stops;
// the midpoint is one of those samples and lands where Chrome's does.
//
// Only the red-to-blue cases are held here. CoreGraphics does not place a
// gradient's colours exactly per pixel: a plain red-to-blue ramp reads 128.7
// at column 50 where Chrome and Skia read 127.5, about half a pixel. On the
// red-to-yellow `longer` sweeps the ramp moves 13 to 23 code values per pixel,
// so the same placement error reads 6 to 11 off, and it does not shrink with
// denser resampling (8, 32 and 128 samples per segment read 6, 11 and 3.4 in
// red). That is CoreGraphics' sampling, not the resampled curve.
TEST_CASE("CoreGraphics midpoints match Chrome through resampling",
          "[css][gradient][interpolation][cg]") {
    const Color blue = Color::rgba(0.0f, 0.0f, 1.0f, 1.0f);
    for (const auto& c : kChromeMidpoints) {
        if (c.end != blue)
            continue;
        const auto got = cg_midpoint(c.css);
        INFO(c.css << "\n  CoreGraphics " << rgb_text(got) << " Chrome " << rgb_text(c.rgb));
        CHECK(max_channel_error(got, c.rgb) <= kChromeTolerance);
    }
}
#endif
