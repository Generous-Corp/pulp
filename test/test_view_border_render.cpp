// CSS borders, judged by the pixels they produce.
//
// Every expectation in this file is a pixel span read off Chrome's own render
// of the equivalent HTML, captured at dpr 2 through
// `tools/import-design/browser_capture` and measured out of `browser.png`. That
// matters more here than usual: a border is arithmetic (where does the ink
// start, how wide is it, which side owns the corner) and a hand-computed
// expectation would just be a second copy of the implementation's own opinion.
// Asserting the browser's numbers is what makes these tests able to disagree
// with the code.
//
// The defects that motivated the file, all found by rendering a real captured
// panel from its own nodes:
//
//   * a stroke centred on the box outline put HALF the border outside the
//     element — 4 device px adrift at dpr 2 for a 4px border, on every side of
//     every bordered node;
//   * a border set only on one edge painted NOTHING;
//   * four different edge colours at one width painted a black frame the
//     design never had, because the uniform colour slot was never written and
//     defaults to opaque black;
//   * four different edge widths at one colour painted nothing, because the
//     uniform width slot was never written and defaults to 0.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/screenshot.hpp>
#include <pulp/view/border_style.hpp>
#include <pulp/view/view.hpp>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using namespace pulp::view;

namespace {

// The fixture geometry, in CSS px: a 120x60 box on a dark plate, captured at
// dpr 2 so every span below is in device pixels and every CSS px is 2 of them.
constexpr int kBoxW = 120;
constexpr int kBoxH = 60;
constexpr float kScale = 2.0f;
constexpr int kDevW = kBoxW * static_cast<int>(kScale);
constexpr int kDevH = kBoxH * static_cast<int>(kScale);

// The ink colours, as the 8-bit triples Chrome reported for them. Pixels are
// compared as 8-bit RGB because that is the unit both sides are read in.
struct Rgb { int r, g, b; };

constexpr Rgb kPlate{20, 25, 30};
constexpr Rgb kRed{255, 0, 0};
constexpr Rgb kGreen{0, 255, 0};
constexpr Rgb kBlue{0, 0, 255};
constexpr Rgb kWhite{255, 255, 255};
constexpr Rgb kYellow{255, 255, 0};
constexpr Rgb kMagenta{255, 0, 255};
constexpr Rgb kAzure{0, 128, 255};
constexpr Rgb kOrange{255, 128, 0};

pulp::canvas::Color paint(Rgb c) {
    return pulp::canvas::Color::rgba8(static_cast<uint8_t>(c.r),
                                      static_cast<uint8_t>(c.g),
                                      static_cast<uint8_t>(c.b));
}

// Raw RGBA of the render, so spans can be read the same way they were read out
// of Chrome's PNG. `render_to_rgba` is the Skia raster path's own buffer, with
// no encode/decode round-trip to blur an edge by a level.
struct Pixels {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;

    Rgb at(int x, int y) const {
        const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width)
                          + static_cast<size_t>(x)) * 4;
        return Rgb{rgba[i], rgba[i + 1], rgba[i + 2]};
    }
};

Pixels render(View& v) {
    v.set_bounds({0.0f, 0.0f, static_cast<float>(kBoxW),
                  static_cast<float>(kBoxH)});
    v.set_background_color(paint(kPlate));
    Pixels p;
    uint32_t w = 0, h = 0;
    p.rgba = render_to_rgba(v, kBoxW, kBoxH, kScale, &w, &h);
    // An empty buffer scores as "every span missing", which would let a broken
    // rasteriser pass the negative assertions in this file.
    REQUIRE(!p.rgba.empty());
    p.width = static_cast<int>(w);
    p.height = static_cast<int>(h);
    REQUIRE(p.width == kDevW);
    REQUIRE(p.height == kDevH);
    return p;
}

bool same(Rgb a, Rgb b) {
    // A one-level tolerance per channel. The colours under test are primaries
    // on a flat plate with no blending, so this admits rounding through the
    // float colour pipeline and nothing else — it cannot absorb a wrong colour
    // or a missing border.
    const auto near = [](int x, int y) { return std::abs(x - y) <= 1; };
    return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b);
}

// The run of consecutive pixels matching `want`, starting at `from`, walking
// one axis of the box. Returns {start, length}; length 0 means never found.
struct Run { int start = -1; int length = 0; };

Run scan_row(const Pixels& p, int y, Rgb want, int from = 0) {
    Run r;
    for (int x = from; x < p.width; ++x) {
        if (same(p.at(x, y), want)) {
            if (r.start < 0) r.start = x;
            ++r.length;
        } else if (r.start >= 0) {
            break;
        }
    }
    return r;
}

// Every run of `want` along a row, so a dash cadence can be measured without
// depending on where the pattern happens to start.
std::vector<Run> scan_row_runs(const Pixels& p, int y, Rgb want) {
    std::vector<Run> runs;
    Run current;
    for (int x = 0; x < p.width; ++x) {
        if (same(p.at(x, y), want)) {
            if (current.start < 0) current.start = x;
            ++current.length;
        } else if (current.start >= 0) {
            runs.push_back(current);
            current = Run{};
        }
    }
    if (current.start >= 0) runs.push_back(current);
    return runs;
}

std::string describe(const std::vector<Run>& runs) {
    std::string out;
    for (const auto& r : runs) {
        out += std::to_string(r.start) + "+" + std::to_string(r.length) + " ";
    }
    return out;
}

Run scan_col(const Pixels& p, int x, Rgb want, int from = 0) {
    Run r;
    for (int y = from; y < p.height; ++y) {
        if (same(p.at(x, y), want)) {
            if (r.start < 0) r.start = y;
            ++r.length;
        } else if (r.start >= 0) {
            break;
        }
    }
    return r;
}

}  // namespace

// `border: 4px solid red` on a 120x60 box. Chrome paints device columns 0..7
// and 232..239 red — the ink is INSIDE the box on both sides, and the box is
// exactly 240 device px wide. A stroke centred on the outline would put the
// left run at -4..3 (clipped to 0..3, half its width lost off-canvas) and the
// right at 236..243, which is what this used to render.
TEST_CASE("a uniform border is painted inside the border box",
          "[view][border]") {
    View v;
    v.set_border(paint(kRed), 4.0f);
    const auto px = render(v);

    const auto left = scan_row(px, kDevH / 2, kRed);
    INFO("left run start=" << left.start << " length=" << left.length);
    CHECK(left.start == 0);
    CHECK(left.length == 8);

    const auto right = scan_row(px, kDevH / 2, kRed, kDevW / 2);
    INFO("right run start=" << right.start << " length=" << right.length);
    CHECK(right.start == 232);
    CHECK(right.length == 8);

    const auto top = scan_col(px, kDevW / 2, kRed);
    CHECK(top.start == 0);
    CHECK(top.length == 8);
    const auto bottom = scan_col(px, kDevW / 2, kRed, kDevH / 2);
    CHECK(bottom.start == 112);
    CHECK(bottom.length == 8);
}

// The same, one CSS pixel wide, because a hairline is where a half-width
// offset is hardest to see and most common in a real panel. Chrome puts the
// 2-device-px run at columns 0..1 and 238..239.
TEST_CASE("a hairline border is painted inside the border box",
          "[view][border]") {
    View v;
    v.set_border(paint(kGreen), 1.0f);
    const auto px = render(v);

    const auto left = scan_row(px, kDevH / 2, kGreen);
    INFO("left run start=" << left.start << " length=" << left.length);
    CHECK(left.start == 0);
    CHECK(left.length == 2);

    const auto right = scan_row(px, kDevH / 2, kGreen, kDevW / 2);
    INFO("right run start=" << right.start << " length=" << right.length);
    CHECK(right.start == 238);
    CHECK(right.length == 2);
}

// `border-top: 6px solid rgb(0,128,255)` and nothing else. Chrome paints the
// top 12 device rows and leaves the other three sides alone. This rendered as
// an empty box: nothing in the paint path read the per-side slots at all.
TEST_CASE("a border set on one edge only paints that edge", "[view][border]") {
    View v;
    v.set_border_top(paint(kAzure), 6.0f);
    const auto px = render(v);

    const auto top = scan_col(px, kDevW / 2, kAzure);
    INFO("top run start=" << top.start << " length=" << top.length);
    CHECK(top.start == 0);
    CHECK(top.length == 12);

    // The other three edges are the plate, not the border colour.
    CHECK(same(px.at(kDevW / 2, kDevH - 1), kPlate));
    CHECK(same(px.at(0, kDevH / 2), kPlate));
    CHECK(same(px.at(kDevW - 1, kDevH / 2), kPlate));
}

// `border-left: 6px solid yellow`, the mirror case, so a fix that hard-codes
// the top edge cannot pass. Chrome paints device columns 0..11.
TEST_CASE("a left-only border paints the left edge", "[view][border]") {
    View v;
    v.set_border_left(paint(kYellow), 6.0f);
    const auto px = render(v);

    const auto left = scan_row(px, kDevH / 2, kYellow);
    INFO("left run start=" << left.start << " length=" << left.length);
    CHECK(left.start == 0);
    CHECK(left.length == 12);
    CHECK(same(px.at(kDevW / 2, 0), kPlate));
    CHECK(same(px.at(kDevW / 2, kDevH - 1), kPlate));
}

// `border-bottom: 1px solid orange`. Chrome paints device rows 118..119 — the
// last two rows of the box, not two rows past it.
TEST_CASE("a bottom-only hairline lands on the last rows of the box",
          "[view][border]") {
    View v;
    v.set_border_bottom(paint(kOrange), 1.0f);
    const auto px = render(v);

    const auto bottom = scan_col(px, kDevW / 2, kOrange);
    INFO("bottom run start=" << bottom.start << " length=" << bottom.length);
    CHECK(bottom.start == 118);
    CHECK(bottom.length == 2);
}

// Four edge colours at one width. Chrome's mid-line spans: top red 0..15,
// bottom blue 104..119, left white 0..15, right green 224..239. This painted a
// solid BLACK frame — the uniform colour slot was never written, and it
// default-constructs to opaque black, so the box gained ink the design never
// asked for.
TEST_CASE("per-side border colours each paint their own edge",
          "[view][border]") {
    View v;
    v.set_border_width(8.0f);
    v.set_border_top_color(paint(kRed));
    v.set_border_right_color(paint(kGreen));
    v.set_border_bottom_color(paint(kBlue));
    v.set_border_left_color(paint(kWhite));
    const auto px = render(v);

    const auto top = scan_col(px, kDevW / 2, kRed);
    CHECK(top.start == 0);
    CHECK(top.length == 16);
    const auto bottom = scan_col(px, kDevW / 2, kBlue, kDevH / 2);
    CHECK(bottom.start == 104);
    CHECK(bottom.length == 16);
    const auto left = scan_row(px, kDevH / 2, kWhite);
    CHECK(left.start == 0);
    CHECK(left.length == 16);
    const auto right = scan_row(px, kDevH / 2, kGreen, kDevW / 2);
    CHECK(right.start == 224);
    CHECK(right.length == 16);
}

// Four edge widths at one colour: top 2, right 6, bottom 10, left 14. Chrome's
// mid-line spans, in device px: top 0..3 (4), bottom 100..119 (20), left 0..27
// (28), right 228..239 (12). This painted nothing at all — the uniform width
// slot was never written and defaults to 0, so the stroke was skipped.
TEST_CASE("per-side border widths each paint their own thickness",
          "[view][border]") {
    View v;
    v.set_border_color(paint(kMagenta));
    v.set_border_top_width(2.0f);
    v.set_border_right_width(6.0f);
    v.set_border_bottom_width(10.0f);
    v.set_border_left_width(14.0f);
    const auto px = render(v);

    const auto top = scan_col(px, kDevW / 2, kMagenta);
    INFO("top run start=" << top.start << " length=" << top.length);
    CHECK(top.start == 0);
    CHECK(top.length == 4);
    const auto bottom = scan_col(px, kDevW / 2, kMagenta, kDevH / 2);
    INFO("bottom run start=" << bottom.start << " length=" << bottom.length);
    CHECK(bottom.start == 100);
    CHECK(bottom.length == 20);
    const auto left = scan_row(px, kDevH / 2, kMagenta);
    INFO("left run start=" << left.start << " length=" << left.length);
    CHECK(left.start == 0);
    CHECK(left.length == 28);
    const auto right = scan_row(px, kDevH / 2, kMagenta, kDevW / 2);
    INFO("right run start=" << right.start << " length=" << right.length);
    CHECK(right.start == 228);
    CHECK(right.length == 12);
}

// An edge explicitly set to zero overrides the shorthand, which is the CSS
// rule the per-edge `set` flags exist for. Without the per-side paint path
// this could not be observed at all: the box painted four uniform edges.
TEST_CASE("an edge width of zero suppresses that edge", "[view][border]") {
    View v;
    v.set_border(paint(kRed), 4.0f);
    v.set_border_top_width(0.0f);
    const auto px = render(v);

    CHECK(same(px.at(kDevW / 2, 0), kPlate));
    CHECK(same(px.at(kDevW / 2, 1), kPlate));
    const auto left = scan_row(px, kDevH / 2, kRed);
    CHECK(left.start == 0);
    CHECK(left.length == 8);
}

// Dash cadence, read off Chrome's render of `border: 3px dashed`: 12 device px
// on, 6 off — 2w on, 1w off. The old pattern was 3w/3w, which is both twice as
// long and evenly split, so a dashed border read as a different design
// decision rather than as a defect.
//
// Cadence, not phase. Blink stretches the pattern so a whole number of dashes
// fits each edge exactly (its runs read 12,11,12,12 with gaps 6,6,6,5) and a
// single dash path effect cannot, so where the first dash STARTS is not
// something the two can be made to agree on. The repeat length is.
TEST_CASE("a dashed border repeats on Chrome's cadence", "[view][border]") {
    View v;
    v.set_border(paint(kWhite), 3.0f);
    v.set_border_style(View::BorderStyle::dashed);
    const auto px = render(v);

    // Along the top border band. The corner run is skipped: it is a mitre
    // joining two edges, not a dash.
    const auto runs = scan_row_runs(px, 3, kWhite);
    INFO("runs " << describe(runs));
    REQUIRE(runs.size() >= 4);
    for (size_t i = 1; i + 1 < runs.size(); ++i) {
        INFO("run " << i);
        CHECK(runs[i].length == 12);
        CHECK(runs[i].start - (runs[i - 1].start + runs[i - 1].length) == 6);
    }
}

// `border: 3px dotted` in Chrome is 6 device px on, 6 off — 1w/1w. The old
// pattern was 1w on / 2w off, so a dotted rule came out sparse.
TEST_CASE("a dotted border repeats on Chrome's cadence", "[view][border]") {
    View v;
    v.set_border(paint(kWhite), 3.0f);
    v.set_border_style(View::BorderStyle::dotted);
    const auto px = render(v);

    const auto runs = scan_row_runs(px, 3, kWhite);
    INFO("runs " << describe(runs));
    REQUIRE(runs.size() >= 4);
    for (size_t i = 1; i + 1 < runs.size(); ++i) {
        INFO("run " << i);
        CHECK(runs[i].length == 6);
        CHECK(runs[i].start - (runs[i - 1].start + runs[i - 1].length) == 6);
    }
}

// A dash pattern on ONE edge. This is the case the capture protocol added all
// four `border-*-style` names for, and it has to survive the per-side paint
// path too: a filled trapezoid cannot carry a dash, so a patterned side is
// stroked instead.
TEST_CASE("a dashed border on one edge stays dashed", "[view][border]") {
    View v;
    v.set_border_left(paint(kGreen), 4.0f);
    v.set_border_style(View::BorderStyle::dashed);
    const auto px = render(v);

    // Down the left band: alternating green and plate, not one solid bar.
    const auto first = scan_col(px, 3, kGreen);
    INFO("first dash start=" << first.start << " length=" << first.length);
    CHECK(first.length > 0);
    CHECK(first.length < kDevH);
    const auto second = scan_col(px, 3, kGreen, first.start + first.length);
    CHECK(second.start > first.start + first.length);
    // And the other three edges are still untouched.
    CHECK(same(px.at(kDevW - 1, kDevH / 2), kPlate));
}

// `none` and `hidden` suppress the stroke however the widths were set. The
// per-side path must honour the same short-circuit the uniform path does, or a
// design that turns a border off gets one back.
TEST_CASE("border-style none suppresses a per-side border", "[view][border]") {
    View v;
    v.set_border_top(paint(kRed), 6.0f);
    v.set_border_style(View::BorderStyle::none);
    const auto px = render(v);
    CHECK(same(px.at(kDevW / 2, 0), kPlate));
    CHECK(same(px.at(kDevW / 2, 5), kPlate));
}

// Opposite sides that together want more than the box has. Chrome gives the
// NEAR side its full width and the far side the remainder: `border-left: 50px`
// with `border-right: 50px` on a 60px-wide box paints 100 device px of left
// and 20 of right, read off its render. Unclamped, the inset stroke below is
// handed a negative rectangle.
TEST_CASE("opposite borders that overflow the box give the near side priority",
          "[view][border]") {
    View v;
    v.set_bounds({0.0f, 0.0f, 60.0f, 40.0f});
    v.set_background_color(paint(kPlate));
    v.set_border_left(paint(kAzure), 50.0f);
    v.set_border_right(paint(kYellow), 50.0f);
    Pixels p;
    uint32_t pw = 0, ph = 0;
    p.rgba = render_to_rgba(v, 60, 40, kScale, &pw, &ph);
    REQUIRE(!p.rgba.empty());
    p.width = static_cast<int>(pw);
    p.height = static_cast<int>(ph);
    REQUIRE(p.width == 120);

    const auto left = scan_row(p, p.height / 2, kAzure);
    INFO("left run start=" << left.start << " length=" << left.length);
    CHECK(left.start == 0);
    CHECK(left.length == 100);
    const auto right = scan_row(p, p.height / 2, kYellow);
    INFO("right run start=" << right.start << " length=" << right.length);
    CHECK(right.start == 100);
    CHECK(right.length == 20);
}

// A uniform border thicker than half the box. Chrome fills the box edge to
// edge — the content box collapses to zero and the four mitres divide what is
// left, which at any mid-line is the border's own colour.
TEST_CASE("a border thicker than the box fills it", "[view][border]") {
    View v;
    v.set_bounds({0.0f, 0.0f, 60.0f, 40.0f});
    v.set_background_color(paint(kPlate));
    v.set_border(paint(kRed), 40.0f);
    Pixels p;
    uint32_t pw = 0, ph = 0;
    p.rgba = render_to_rgba(v, 60, 40, kScale, &pw, &ph);
    REQUIRE(!p.rgba.empty());
    p.width = static_cast<int>(pw);
    p.height = static_cast<int>(ph);

    const auto row = scan_row(p, p.height / 2, kRed);
    INFO("row run start=" << row.start << " length=" << row.length);
    CHECK(row.start == 0);
    CHECK(row.length == p.width);
    const auto col = scan_col(p, p.width / 2, kRed);
    CHECK(col.start == 0);
    CHECK(col.length == p.height);
}

// ── Shaded and multi-line border styles ─────────────────────────────────────
//
// `outset`, `inset`, `groove` and `ridge` paint each side in a lighter or
// darker shade of its colour, and `double` paints two lines. Every shade below
// is the 8-bit colour Chrome 151 painted for that side colour, read out of its
// dpr-2 render; the set covers the special cases the rule has (pure and
// near-black, the too-dark-to-darken band up to rgb(32,32,32) by luminance,
// the not-lightened band above rgb(235,235,235)) and ordinary colours.

namespace {

struct ShadeCase {
    Rgb base;
    Rgb light;
    Rgb dark;
};

Rgb rgb8(pulp::canvas::Color c) {
    const auto level = [](float v) {
        return static_cast<int>(std::lround(v * 255.0f));
    };
    return Rgb{level(c.r), level(c.g), level(c.b)};
}

bool exactly(Rgb a, Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

}  // namespace

TEST_CASE("3D border shades match Chrome's per-colour shading",
          "[view][border][border-shading]") {
    const ShadeCase cases[] = {
        {{0, 0, 0}, {168, 168, 168}, {84, 84, 84}},
        {{1, 1, 1}, {169, 169, 169}, {85, 85, 85}},
        {{32, 32, 32}, {200, 200, 200}, {116, 116, 116}},
        {{33, 33, 33}, {117, 117, 117}, {0, 0, 0}},
        {{34, 31, 31}, {202, 185, 185}, {118, 108, 108}},
        {{33, 32, 32}, {117, 114, 114}, {0, 0, 0}},
        {{10, 20, 30}, {66, 132, 198}, {38, 76, 114}},
        {{60, 0, 0}, {229, 0, 0}, {144, 0, 0}},
        {{80, 0, 0}, {164, 0, 0}, {0, 0, 0}},
        {{0, 0, 100}, {0, 0, 255}, {0, 0, 184}},
        {{0, 0, 128}, {0, 0, 212}, {0, 0, 44}},
        {{84, 84, 84}, {168, 168, 168}, {0, 0, 0}},
        {{128, 128, 128}, {212, 212, 212}, {44, 44, 44}},
        {{20, 40, 160}, {30, 61, 245}, {9, 19, 76}},
        {{200, 150, 40}, {255, 191, 51}, {116, 87, 23}},
        {{235, 235, 235}, {255, 255, 255}, {151, 151, 151}},
        {{236, 236, 236}, {236, 236, 236}, {152, 152, 152}},
        {{250, 230, 240}, {250, 230, 240}, {166, 153, 159}},
        {{220, 245, 250}, {220, 245, 250}, {146, 163, 166}},
        {{255, 0, 0}, {255, 0, 0}, {171, 0, 0}},
        {{255, 255, 255}, {255, 255, 255}, {171, 171, 171}},
    };
    for (const auto& c : cases) {
        const auto shades = border_shades(paint(c.base));
        INFO("base " << c.base.r << "," << c.base.g << "," << c.base.b
             << " light " << rgb8(shades.light).r << ","
             << rgb8(shades.light).g << "," << rgb8(shades.light).b
             << " dark " << rgb8(shades.dark).r << ","
             << rgb8(shades.dark).g << "," << rgb8(shades.dark).b);
        CHECK(exactly(rgb8(shades.light), c.light));
        CHECK(exactly(rgb8(shades.dark), c.dark));
    }
    // Alpha is the side colour's own; only the channels are shaded.
    const auto translucent =
        border_shades(pulp::canvas::Color::rgba(0.0f, 0.0f, 1.0f, 0.5f));
    CHECK(translucent.light.a == 0.5f);
    CHECK(translucent.dark.a == 0.5f);
    CHECK(exactly(rgb8(translucent.dark), Rgb{0, 0, 171}));
}

TEST_CASE("each 3D style shades the sides it should",
          "[view][border][border-shading]") {
    using S = BorderEdge;
    using B = View::BorderStyle;
    const auto black = paint(Rgb{0, 0, 0});
    const Rgb light{168, 168, 168};
    const Rgb dark{84, 84, 84};
    const auto band = [&](B style, S side, bool outer) {
        return rgb8(border_band_color(style, side, outer, black, 6.0f));
    };
    // Outset lights the top and left, inset the bottom and right.
    CHECK(exactly(band(B::outset, S::top, true), light));
    CHECK(exactly(band(B::outset, S::left, true), light));
    CHECK(exactly(band(B::outset, S::bottom, true), dark));
    CHECK(exactly(band(B::outset, S::right, true), dark));
    CHECK(exactly(band(B::inset, S::top, true), dark));
    CHECK(exactly(band(B::inset, S::bottom, true), light));
    // A groove's outer half is shaded as inset and its inner half as outset;
    // a ridge is the reverse.
    CHECK(exactly(band(B::groove, S::top, true), dark));
    CHECK(exactly(band(B::groove, S::top, false), light));
    CHECK(exactly(band(B::groove, S::bottom, true), light));
    CHECK(exactly(band(B::groove, S::bottom, false), dark));
    CHECK(exactly(band(B::ridge, S::top, true), light));
    CHECK(exactly(band(B::ridge, S::right, false), light));
    // A 1px groove is not split, and paints the unshaded colour.
    CHECK(exactly(rgb8(border_band_color(B::groove, S::top, true, black,
                                               1.0f)),
                  Rgb{0, 0, 0}));
    // Solid and double are never shaded.
    CHECK(exactly(band(B::double_, S::top, true), Rgb{0, 0, 0}));
    CHECK(exactly(band(B::solid, S::bottom, true), Rgb{0, 0, 0}));
}

TEST_CASE("groove and double split a side into Chrome's bands",
          "[view][border][border-shading]") {
    using S = BorderEdge;
    using B = View::BorderStyle;
    // {width, top outer, bottom outer} for a groove, read off Chrome's render:
    // the top and left take the odd pixel on the outside.
    const float groove[][3] = {{1, 1, 1}, {2, 1, 1}, {3, 2, 1}, {4, 2, 2},
                               {9, 5, 4}, {12, 6, 6}};
    for (const auto& g : groove) {
        INFO("groove width " << g[0]);
        const auto top = border_bands(B::groove, S::top, g[0]);
        const auto bottom = border_bands(B::groove, S::bottom, g[0]);
        CHECK(top.outer == g[1]);
        CHECK(top.outer + top.gap + top.inner == g[0]);
        CHECK(bottom.outer == g[2]);
        CHECK(bottom.outer + bottom.gap + bottom.inner == g[0]);
    }
    // {width, line, gap} for double: below 3px it is one solid line.
    const float dbl[][3] = {{2, 2, 0}, {3, 1, 1}, {4, 1, 2}, {5, 2, 1},
                            {7, 2, 3}, {8, 3, 2}, {11, 4, 3}, {12, 4, 4}};
    for (const auto& d : dbl) {
        INFO("double width " << d[0]);
        const auto bands = border_bands(B::double_, S::right, d[0]);
        CHECK(bands.outer == d[1]);
        CHECK(bands.gap == d[2]);
        CHECK(bands.inner == (d[0] < 3 ? 0.0f : d[1]));
    }
}

// `border: 6px outset black` on the 120x60 box. Chrome paints 12 device px of
// rgb(168,168,168) down the top and left and of rgb(84,84,84) along the bottom
// and right; a flat black frame is what this lowered to before.
TEST_CASE("an outset border paints its light and dark sides",
          "[view][border][border-shading]") {
    View v;
    v.set_border(paint(Rgb{0, 0, 0}), 6.0f);
    v.set_border_style(View::BorderStyle::outset);
    const auto px = render(v);
    const Rgb light{168, 168, 168};
    const Rgb dark{84, 84, 84};

    const auto top = scan_col(px, kDevW / 2, light);
    CHECK(top.start == 0);
    CHECK(top.length == 12);
    const auto bottom = scan_col(px, kDevW / 2, dark, kDevH / 2);
    CHECK(bottom.start == kDevH - 12);
    CHECK(bottom.length == 12);
    const auto left = scan_row(px, kDevH / 2, light);
    CHECK(left.start == 0);
    CHECK(left.length == 12);
    const auto right = scan_row(px, kDevH / 2, dark, kDevW / 2);
    CHECK(right.start == kDevW - 12);
    CHECK(right.length == 12);
    // The top-right corner is mitred: light above the diagonal, dark below.
    CHECK(same(px.at(kDevW - 11, 1), light));
    CHECK(same(px.at(kDevW - 2, 10), dark));
}

// `border: 9px groove black`: Chrome paints 10 device px of the dark shade
// then 8 of the light one down from the top, and 8 light then 10 dark up from
// the bottom.
TEST_CASE("a groove border paints two shaded bands per side",
          "[view][border][border-shading]") {
    View v;
    v.set_border(paint(Rgb{0, 0, 0}), 9.0f);
    v.set_border_style(View::BorderStyle::groove);
    const auto px = render(v);
    const Rgb light{168, 168, 168};
    const Rgb dark{84, 84, 84};

    const auto top_outer = scan_col(px, kDevW / 2, dark);
    CHECK(top_outer.start == 0);
    CHECK(top_outer.length == 10);
    const auto top_inner = scan_col(px, kDevW / 2, light);
    CHECK(top_inner.start == 10);
    CHECK(top_inner.length == 8);
    const auto bottom_inner = scan_col(px, kDevW / 2, dark, kDevH / 2);
    CHECK(bottom_inner.start == kDevH - 18);
    CHECK(bottom_inner.length == 10);
    const auto bottom_outer = scan_col(px, kDevW / 2, light, kDevH / 2);
    CHECK(bottom_outer.start == kDevH - 8);
    CHECK(bottom_outer.length == 8);
}

// `border: 9px double white`: two 6-device-px lines with 6 px of the box's own
// background between them.
TEST_CASE("a double border paints two lines with a gap",
          "[view][border][border-shading]") {
    View v;
    v.set_border(paint(kWhite), 9.0f);
    v.set_border_style(View::BorderStyle::double_);
    const auto px = render(v);
    const auto runs = scan_row_runs(px, kDevH / 2, kWhite);
    INFO("runs " << describe(runs));
    REQUIRE(runs.size() == 4);
    CHECK(runs[0].start == 0);
    CHECK(runs[0].length == 6);
    CHECK(runs[1].start == 12);
    CHECK(runs[1].length == 6);
    CHECK(same(px.at(8, kDevH / 2), kPlate));
}

TEST_CASE("border_style_from_css names every CSS line style and nothing else",
          "[view][border][css]") {
    using B = View::BorderStyle;
    const std::pair<const char*, B> keywords[] = {
        {"solid", B::solid},   {"dashed", B::dashed}, {"dotted", B::dotted},
        {"double", B::double_}, {"groove", B::groove}, {"ridge", B::ridge},
        {"inset", B::inset},   {"outset", B::outset}, {"none", B::none},
        {"hidden", B::hidden},
    };
    for (const auto& [keyword, style] : keywords) {
        INFO(keyword);
        const auto parsed = border_style_from_css(keyword);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == style);
    }
    CHECK_FALSE(border_style_from_css("").has_value());
    CHECK_FALSE(border_style_from_css("wavy").has_value());
    CHECK_FALSE(border_style_from_css("Dashed").has_value());
}
