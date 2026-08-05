// CSS gradient strings, judged by the pixels they produce.
//
// A gradient setter accepting a string proves nothing — every defect this file
// covers was a string the parser ACCEPTED and then painted wrong. So each case
// renders the CSS under test beside a hand-written equivalent that means the
// same thing, and requires the two images to match. Where a form is expected to
// be refused, the refusal itself is asserted rather than inferred from a blank
// image.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/css_gradient.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/view.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace pulp::view;

namespace {

constexpr int kSize = 160;

// Renders a background gradient over a fixed box. The box is square so a conic
// sweep is radially symmetric and a missing wedge shows up as a flat region
// rather than as an artefact of the aspect ratio.
std::vector<uint8_t> render_css(const std::string& css, bool* applied = nullptr) {
    View v;
    v.set_bounds({0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize)});
    v.set_background_color({20, 20, 20, 255});
    const bool ok = apply_css_background_gradient(v, css);
    if (applied != nullptr) *applied = ok;
    return render_to_png(v, kSize, kSize, 1.0f, ScreenshotBackend::skia);
}

// Two CSS values that mean the same thing must paint the same pixels. Exact
// rather than tolerant: both sides run through one renderer on one machine, so
// any difference at all is the parser disagreeing with itself.
void require_same_render(const std::string& a, const std::string& b) {
    bool a_applied = false, b_applied = false;
    const auto lhs = render_css(a, &a_applied);
    const auto rhs = render_css(b, &b_applied);
    INFO("A: " << a << "\nB: " << b);
    REQUIRE(a_applied);
    REQUIRE(b_applied);
    // A pair of blank renders would satisfy any similarity check. Require the
    // reference to actually contain a gradient first.
    const auto content = analyze_screenshot_content(rhs);
    INFO("reference luminance stddev: " << content.luminance_stddev);
    REQUIRE(content.luminance_stddev > 1.0);
    const auto cmp = compare_screenshots(lhs, rhs);
    INFO("similarity " << cmp.similarity << " mean_error " << cmp.mean_error);
    CHECK(cmp.similarity > 0.999f);
}

// Eight bands of red-then-green across the box, written one band at a time.
// On a kSize box with the line running `to right`, that is what a 20px
// repeating band means: 20px is 12.5% of 160, and the colour flips halfway.
//
// The reference is written in PERCENT deliberately. A bare `px` stop outside a
// repeating gradient is still read as a raw 0..1 number, which is a separate
// defect — using that spelling as the reference would compare this fix against
// that bug and agree with both.
std::string eight_explicit_bands() {
    std::string css = "linear-gradient(to right";
    for (int i = 0; i < 8; ++i) {
        const double a = i * 12.5;
        css += ", #ff0000 " + std::to_string(a) + "%";
        css += ", #ff0000 " + std::to_string(a + 6.25) + "%";
        css += ", #00ff00 " + std::to_string(a + 6.25) + "%";
        css += ", #00ff00 " + std::to_string(a + 12.5) + "%";
    }
    return css + ")";
}

}  // namespace

// A sweep covers the whole circle. Skia clamps angles outside the shader's
// [start, end] window instead of wrapping, so passing the CSS rotation as the
// window's start left the rest of the turn painted flat in the last stop's
// colour: `from 0deg` — the default — lost a 90° wedge, because the parser
// applies a -90° correction to make 0deg point up. Rotating the shader keeps
// the window at a full turn, so no angle falls outside it.
TEST_CASE("a conic gradient covers the whole circle at any start angle",
          "[view][gradient][conic]") {
    // A pattern whose period is a quarter turn is unchanged by a quarter-turn
    // rotation, so every one of these must render the SAME image. That is what
    // makes the wedge visible without needing to rotate a bitmap to compare:
    // the gradient is rotation-invariant, the defect is not — it parks a flat
    // clamped region wherever the start angle happens to fall.
    //
    // The ramp is smooth rather than hard-banded on purpose. A rotation is
    // applied as a shader matrix, and resampling a hard colour edge through one
    // costs a pixel or two of difference along every edge — enough to force a
    // loose threshold that a real defect could hide under. With no edges to
    // resample the invariance is exact, so the comparison below can stay strict.
    std::string quarter_periodic = "conic-gradient(from ANGLE";
    for (int i = 0; i < 4; ++i) {
        const float a = static_cast<float>(i) * 25.0f;
        quarter_periodic += ", #ff0000 " + std::to_string(a) + "%";
        quarter_periodic += ", #00ff00 " + std::to_string(a + 12.5f) + "%";
    }
    quarter_periodic += ", #ff0000 100%)";

    const auto at = [&](const char* angle) {
        std::string css = quarter_periodic;
        css.replace(css.find("ANGLE"), 5, angle);
        bool applied = false;
        auto png = render_css(css, &applied);
        REQUIRE(applied);
        return png;
    };

    // 90deg is the one start angle that already worked — its -90° correction
    // lands on a zero offset, leaving the shader window at a full turn — so it
    // is the reference the other rotations are held to.
    const auto reference = at("90deg");
    REQUIRE(analyze_screenshot_content(reference).luminance_stddev > 1.0);
    for (const char* rotated : {"0deg", "180deg", "270deg"}) {
        INFO("from " << rotated);
        const auto cmp = compare_screenshots(at(rotated), reference);
        INFO("similarity " << cmp.similarity << " mean_error " << cmp.mean_error);
        CHECK(cmp.similarity > 0.999f);
    }
}

// A stop's position carries a unit and the unit decides its meaning. stof stops
// at the first non-digit, so `180deg` used to parse as the raw number 180 and
// land at 18000% — clamped to the end of the ramp — while `50%` parsed
// correctly. Every conic in the wild is authored in degrees.
TEST_CASE("conic gradient stops honour angular units",
          "[view][gradient][conic]") {
    SECTION("degrees are a fraction of a turn, not a raw number") {
        require_same_render(
            "conic-gradient(from 90deg, #ff0000 0deg, #00ff00 180deg, "
            "#ff0000 360deg)",
            "conic-gradient(from 90deg, #ff0000 0%, #00ff00 50%, "
            "#ff0000 100%)");
    }
    SECTION("turns") {
        require_same_render(
            "conic-gradient(from 90deg, #ff0000 0turn, #00ff00 0.25turn, "
            "#ff0000 1turn)",
            "conic-gradient(from 90deg, #ff0000 0%, #00ff00 25%, "
            "#ff0000 100%)");
    }
    SECTION("a degree stop is no longer the same as the bare number") {
        // The exact confusion being fixed: `180deg` and `180` must now differ.
        bool a = false, b = false;
        const auto with_unit = render_css(
            "conic-gradient(from 90deg, #ff0000 0deg, #00ff00 180deg, "
            "#ff0000 360deg)", &a);
        const auto bare = render_css(
            "conic-gradient(from 90deg, #ff0000 0, #00ff00 180, #ff0000 360)",
            &b);
        REQUIRE(a);
        REQUIRE(b);
        CHECK(compare_screenshots(with_unit, bare).similarity < 0.999f);
    }
}

// One stop, two positions: `#fff 0deg 2deg` is shorthand for the colour running
// from 0deg to 2deg. Only one trailing token used to be peeled, so the first
// position stayed glued to the colour and took the colour down with it.
TEST_CASE("a gradient stop may carry two positions",
          "[view][gradient][conic]") {
    SECTION("percent") {
        require_same_render(
            "conic-gradient(from 90deg, #ff0000 0% 50%, #00ff00 50% 100%)",
            "conic-gradient(from 90deg, #ff0000 0%, #ff0000 50%, "
            "#00ff00 50%, #00ff00 100%)");
    }
    SECTION("degrees") {
        require_same_render(
            "conic-gradient(from 90deg, #ff0000 0deg 180deg, "
            "#00ff00 180deg 360deg)",
            "conic-gradient(from 90deg, #ff0000 0deg, #ff0000 180deg, "
            "#00ff00 180deg, #00ff00 360deg)");
    }
    SECTION("linear gradients too") {
        require_same_render(
            "linear-gradient(to right, #ff0000 0% 50%, #00ff00 50% 100%)",
            "linear-gradient(to right, #ff0000 0%, #ff0000 50%, "
            "#00ff00 50%, #00ff00 100%)");
    }
}

// The repeat is Skia's: a sweep shader given a sub-turn window and a repeating
// tile mode tiles the band around the circle. Before this the whole value was
// refused by the prefix match and nothing painted at all.
TEST_CASE("repeating-conic-gradient tiles its band around the circle",
          "[view][gradient][conic]") {
    // Six 60° bands written out explicitly is what one 60° repeating band
    // means. Both sides are the same shader with the same stops; only the way
    // the repetition is expressed differs.
    std::string explicit_bands = "conic-gradient(from 90deg";
    for (int i = 0; i < 6; ++i) {
        const int a = i * 60;
        explicit_bands += ", #ff0000 " + std::to_string(a) + "deg";
        explicit_bands += ", #ff0000 " + std::to_string(a + 30) + "deg";
        explicit_bands += ", #00ff00 " + std::to_string(a + 30) + "deg";
        explicit_bands += ", #00ff00 " + std::to_string(a + 60) + "deg";
    }
    explicit_bands += ")";
    require_same_render(
        "repeating-conic-gradient(from 90deg, #ff0000 0deg 30deg, "
        "#00ff00 30deg 60deg)",
        explicit_bands);
}

// The declaration this cluster was found for, taken verbatim from the panel a
// design pass actually produced (the VELOUR CS-24 tick ring). It hits all four
// defects at once, which is why the ring came back as a screenshot instead of
// as vectors: a `from` rotation that clamped a 135° wedge, angular stop
// positions that parsed as raw fractions, two positions on every stop, and a
// `repeating-` prefix that matched no branch at all.
//
// `var(--line-strong)` is substituted with the value its pack resolves it to
// (tokens/semantic.css) because that substitution is Chromium's job, not this
// parser's — a computed value never contains a var().
TEST_CASE("the tick ring a design pass authored renders as vectors",
          "[view][gradient][conic]") {
    const std::string tick_ring =
        "repeating-conic-gradient(from 225deg, "
        "rgba(220,232,250,0.22) 0deg 1.4deg, transparent 1.4deg 15deg)";
    bool applied = false;
    const auto png = render_css(tick_ring, &applied);
    REQUIRE(applied);

    // 360 / 15 = 24 ticks. Written out one band at a time, it is the same ring.
    std::string explicit_ticks = "conic-gradient(from 225deg";
    for (int i = 0; i < 24; ++i) {
        const double a = i * 15.0;
        explicit_ticks += ", rgba(220,232,250,0.22) " + std::to_string(a) + "deg";
        explicit_ticks += ", rgba(220,232,250,0.22) " + std::to_string(a + 1.4) + "deg";
        explicit_ticks += ", transparent " + std::to_string(a + 1.4) + "deg";
        explicit_ticks += ", transparent " + std::to_string(a + 15.0) + "deg";
    }
    explicit_ticks += ")";
    bool explicit_applied = false;
    const auto reference = render_css(explicit_ticks, &explicit_applied);
    REQUIRE(explicit_applied);

    // Ticks this fine are mostly transparent, so luminance spread is the wrong
    // instrument — assert the ring is actually drawn by requiring some of the
    // box to be non-background.
    const auto stats = analyze_screenshot_content(png);
    INFO("tick coverage " << stats.non_background_coverage);
    CHECK(stats.non_background_coverage > 0.0);

    const auto cmp = compare_screenshots(png, reference);
    INFO("similarity " << cmp.similarity << " mean_error " << cmp.mean_error);
    CHECK(cmp.similarity > 0.999f);
}



// A `calc()` stop position resolves against the gradient's own length.
//
// `parse_stops` peels a position off the RIGHT of a stop by splitting on the
// last space, so `#fff calc(100% - 7px)` peels `7px)` — which used to parse as
// 7 and land the stop at 700%. That number is now rejected (it is not the whole
// token), but rejecting it leaves the calc text glued to the COLOUR, and
// `#fff calc(100% - 7px)` read as a colour is not `#fff`. Whitespace decides
// which of the two wrong renders you get, which is why the spacing variants
// below all have to agree with the same reference.
TEST_CASE("a calc() stop position resolves against the gradient length",
          "[view][gradient][calc]") {
    // The box is kSize wide and the gradient runs `to right`, so the gradient
    // line is kSize px and `calc(100% - 7px)` is (kSize-7)/kSize of it.
    const double pct = 100.0 * (kSize - 7.0) / kSize;
    const std::string reference =
        "linear-gradient(to right, #ff0000 " + std::to_string(pct) +
        "%, #0000ff " + std::to_string(pct) + "%)";

    // Every spacing a designer might write. CSS requires spaces around the
    // binary `-` inside calc(); the others are what tools actually emit.
    for (const char* expr : {"calc(100% - 7px)", "calc(100%  -  7px)"}) {
        const std::string css = std::string("linear-gradient(to right, #ff0000 ") +
                                expr + ", #0000ff " + expr + ")";
        INFO("expression: " << expr);
        require_same_render(css, reference);
    }
}

// The same position written as a bare percentage inside calc(), with no length
// unit to resolve — this needs no box at all and must simply work.
TEST_CASE("a calc() stop position with no length unit resolves",
          "[view][gradient][calc]") {
    require_same_render(
        "linear-gradient(to right, #ff0000 calc(50% + 10%), #0000ff calc(50% + 10%))",
        "linear-gradient(to right, #ff0000 60%, #0000ff 60%)");
}

// A calc() this parser cannot evaluate must be REFUSED, not approximated.
//
// Every other unevaluable position in this file is simply absent, and CSS
// spreads absent positions evenly. A stop that names calc() is not absent — it
// asked for somewhere specific — so spreading it evenly would put it where the
// author did not ask while looking like an ordinary render. That is the exact
// shape of defect this file exists to catch, so the refusal is asserted rather
// than inferred from a plausible image.
TEST_CASE("an unevaluable calc() stop refuses the gradient",
          "[view][gradient][calc]") {
    const auto refused = [](const std::string& css) {
        bool applied = false;
        render_css(css, &applied);
        INFO("css: " << css);
        CHECK_FALSE(applied);
    };

    // No whitespace around the binary `-`. Invalid CSS: without the spaces the
    // `-` belongs to the number, and a browser drops the declaration too.
    refused("linear-gradient(to right, #ff0000 calc(100%-7px), #0000ff 100%)");
    // Units this reader does not model, rather than a guess at the em size.
    refused("linear-gradient(to right, #ff0000 calc(100% - 2em), #0000ff 100%)");
    // Multiplication is not modelled.
    refused("linear-gradient(to right, #ff0000 calc(50% * 2), #0000ff 100%)");
    // A conic measures angles, so a px term has no length to resolve against
    // and must not silently become a fraction of something else.
    refused("conic-gradient(#ff0000 calc(100% - 7px), #0000ff 100%)");

    // The control: the same shape WITH a resolvable calc is accepted, so the
    // refusals above are the calc being judged and not the surrounding CSS.
    bool applied = false;
    render_css("linear-gradient(to right, #ff0000 calc(100% - 7px), #0000ff 100%)",
               &applied);
    CHECK(applied);
}

// The colour survives a position the parser cannot use.
//
// The peel splits a stop into colour and position. When it split on the last
// space it took `7px)` and left `#ff0000 calc(100% -` behind — read as a colour
// that is not #ff0000. Refusing the gradient hides that from the render, so
// assert it where it is visible: an unevaluable calc in ONE stop must not
// change what the OTHER stops paint.
TEST_CASE("a calc() position does not corrupt its own colour",
          "[view][gradient][calc]") {
    // `50%` and `calc(50% + 0%)` are the same position by two routes, so the
    // colours must land identically. If the calc text leaked into the colour
    // token, the left half would not be red.
    require_same_render(
        "linear-gradient(to right, #ff0000 calc(50% + 0%), #0000ff calc(50% + 0%))",
        "linear-gradient(to right, #ff0000 50%, #0000ff 50%)");
}

// CSS `background-image` takes a comma-separated LIST of layers, painted
// first-on-top. `View` had one gradient slot, so the top-level comma split
// never happened: the second layer's stops were swallowed into the first's
// stop list and neither rendered.
//
// Two assertions, because either alone passes in a broken state. The first
// pins the ORDER — an opaque top layer hides everything under it, so the pair
// must render as that layer alone; a parser that simply ignored everything
// after the first comma would also pass it. The second pins EXISTENCE — a
// fully transparent top layer must reveal the one beneath, which that same
// parser would render as nothing.
TEST_CASE("comma-separated background layers paint first-on-top",
          "[view][gradient][layers]") {
    require_same_render(
        "linear-gradient(to right, #ff0000 0%, #00ff00 100%), "
        "linear-gradient(to right, #0000ff 0%, #ffff00 100%)",
        "linear-gradient(to right, #ff0000 0%, #00ff00 100%)");
}

TEST_CASE("a transparent background layer reveals the one beneath it",
          "[view][gradient][layers]") {
    require_same_render(
        "linear-gradient(to right, transparent 0%, transparent 100%), "
        "linear-gradient(to right, #0000ff 0%, #ffff00 100%)",
        "linear-gradient(to right, #0000ff 0%, #ffff00 100%)");
}

// A CSS angle is not a colour. Reading `150deg` as the first stop returned the
// parser's opaque-white fallback and left the direction at its `to bottom`
// default, so every angled gradient in a captured design painted a white-to-
// something ramp straight down — a wrong picture that looks deliberate.
//
// The equivalences are asserted against the keyword forms rather than against
// stored endpoints: an endpoint pair can be arithmetically plausible and still
// paint the wrong way round, and only the pixels settle that.
TEST_CASE("an angled linear gradient means its keyword equivalent",
          "[view][gradient][linear]") {
    // 0deg points UP in CSS — toward the top — which is `to top`.
    require_same_render("linear-gradient(0deg, #ff0000, #0000ff)",
                        "linear-gradient(to top, #ff0000, #0000ff)");
    require_same_render("linear-gradient(90deg, #ff0000, #0000ff)",
                        "linear-gradient(to right, #ff0000, #0000ff)");
    require_same_render("linear-gradient(180deg, #ff0000, #0000ff)",
                        "linear-gradient(to bottom, #ff0000, #0000ff)");
    require_same_render("linear-gradient(270deg, #ff0000, #0000ff)",
                        "linear-gradient(to left, #ff0000, #0000ff)");
    // Other units reach the same place.
    require_same_render("linear-gradient(0.25turn, #ff0000, #0000ff)",
                        "linear-gradient(to right, #ff0000, #0000ff)");
}

// The angle must actually be applied, not merely tolerated. A parser that
// consumed `45deg` and then fell back to `to bottom` would pass every
// equivalence above that happens to be vertical, so this one requires a
// diagonal to differ from both axes it sits between.
TEST_CASE("a diagonal linear gradient is neither of its axes",
          "[view][gradient][linear]") {
    bool applied = false;
    const auto diagonal =
        render_css("linear-gradient(45deg, #ff0000, #0000ff)", &applied);
    REQUIRE(applied);
    for (const char* axis : {"linear-gradient(to top, #ff0000, #0000ff)",
                             "linear-gradient(to right, #ff0000, #0000ff)"}) {
        const auto other = render_css(axis);
        const auto cmp = compare_screenshots(diagonal, other);
        INFO("vs " << axis << " similarity " << cmp.similarity);
        CHECK(cmp.similarity < 0.99f);
    }
}

// `to bottom right` used to match the `to bottom` prefix test and paint
// straight down. On a square box its line is the diagonal, which is 135deg.
TEST_CASE("a corner keyword paints toward its corner",
          "[view][gradient][linear]") {
    require_same_render("linear-gradient(to bottom right, #ff0000, #0000ff)",
                        "linear-gradient(135deg, #ff0000, #0000ff)");
    require_same_render("linear-gradient(to top left, #ff0000, #0000ff)",
                        "linear-gradient(315deg, #ff0000, #0000ff)");
}

// The repeat is Skia's, exactly as it is for the conic above: a linear shader
// given a ONE-BAND span and a repeating tile mode tiles that band along the
// line. Before this the `repeating-` prefix matched no branch at all, so the
// value was refused — and a refusal takes the node's whole background-image
// stack with it, so the element painted nothing rather than painting one band.
TEST_CASE("repeating-linear-gradient tiles its band across the box",
          "[view][gradient][linear]") {
    // Two positions on each stop, in px, is the grid idiom this was found for.
    require_same_render(
        "repeating-linear-gradient(to right, #ff0000 0px 10px, "
        "#00ff00 10px 20px)",
        eight_explicit_bands());
}

// The same eight bands with the band stated as a FRACTION of the gradient line
// rather than a length. Same pixels, different unit reaching the painter — and
// only the painter can turn a fraction into a length, because the line is as
// long as the laid-out box makes it.
TEST_CASE("a repeating linear band may be a fraction of the gradient line",
          "[view][gradient][linear]") {
    require_same_render(
        "repeating-linear-gradient(to right, #ff0000 0% 6.25%, "
        "#00ff00 6.25% 12.5%)",
        eight_explicit_bands());
}

// The declaration this cluster was found for, verbatim from the header of a
// panel a design pass actually produced. Chrome draws a hairline every 7 CSS px
// across that header; we drew nothing at all.
//
// It is pinned at the parse rather than in pixels because 160 is not a multiple
// of 7, so there is no explicit equivalent to compare against without inventing
// a partial last band — and its colour is 12.6%-alpha over a dark ground, which
// is too faint for a coverage threshold to judge honestly. The band and the
// rescaled stops ARE the whole contract with the painter, and the two cases
// above already hold the pixels. Chrome's own 7px period is re-measured on the
// real panel by tools/import-validation/replay-agent-panel.sh.
TEST_CASE("the header pinstripe a design pass authored carries its band",
          "[view][gradient][linear]") {
    View v;
    v.set_bounds({0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize)});
    REQUIRE(apply_css_background_gradient(
        v,
        "repeating-linear-gradient(90deg, "
        "oklab(0.322354 0.00371338 0.035201 / 0.126275) 0px, "
        "oklab(0.322354 0.00371338 0.035201 / 0.126275) 1px, "
        "rgba(0, 0, 0, 0) 1px, rgba(0, 0, 0, 0) 7px)"));
    REQUIRE(v.background_gradient_layers().size() == 1);
    const auto& layer = v.background_gradient_layers().front();
    CHECK(layer.linear_repeat == 7.0f);
    CHECK(layer.linear_repeat_unit ==
          View::BackgroundGradient::RepeatUnit::px);
    // The stops are rescaled onto the band: the hairline occupies the first
    // seventh of every tile, not the first seventh of the whole header.
    REQUIRE(layer.positions.size() == 4);
    CHECK(layer.positions[0] == 0.0f);
    CHECK(std::abs(layer.positions[1] - 1.0f / 7.0f) < 1e-5f);
    CHECK(std::abs(layer.positions[2] - 1.0f / 7.0f) < 1e-5f);
    CHECK(layer.positions[3] == 1.0f);
}

// A repeating gradient whose band cannot be resolved must DEGRADE to a
// non-repeating one, never refuse.
//
// This is the contract that matters more than the tiling itself. A refusal
// discards the whole background-image stack, and a node with no stack is
// indistinguishable from a node that never had a background — so an
// unsupported gradient FORM reads downstream as a painter defect. These are
// therefore asserted to PAINT, not to tile.
TEST_CASE("an unresolvable repeating band degrades instead of dropping",
          "[view][gradient][linear]") {
    // Both spellings keep their stop positions monotonic on the degraded path.
    // A degrade hands the shader the stops as the reader left them, so a case
    // written to go BACKWARDS (`10px, 50%` — 10 then 0.5) would be testing
    // Skia's tolerance for unsorted stops rather than this contract.
    for (const char* css : {
             // No single unit for the band: 20% and 30px measure different
             // things and the last stop cannot speak for both.
             "repeating-linear-gradient(to right, #ff0000 20%, #0000ff 30px)",
             // A middle stop with no position of its own is spread across the
             // WHOLE line, so rescaling it onto the band would move it.
             "repeating-linear-gradient(to right, #ff0000 0%, #00ff00, "
             "#0000ff 50%)"}) {
        bool applied = false;
        render_css(css, &applied);
        INFO("css: " << css);
        CHECK(applied);
    }
}

// `repeating-radial-gradient` hits the same prefix miss and is admitted by the
// same change. Its band is NOT tiled — see parse_one_gradient — so it is held
// to the one case where tiled and clamped are the same picture: a band that
// already spans the whole shape. What this pins is that the value PAINTS,
// where before it took the node's entire background-image stack down with it.
TEST_CASE("repeating-radial-gradient paints instead of refusing the stack",
          "[view][gradient][radial]") {
    require_same_render(
        "repeating-radial-gradient(circle, #ff0000 0%, #0000ff 100%)",
        "radial-gradient(circle, #ff0000 0%, #0000ff 100%)");
}

// Chromium serializes every modern colour syntax as oklab()/oklch(), so this is
// the form a captured design actually arrives in. Unrecognised, it returned the
// opaque-white fallback: a dark faceplate rendered white, and a gradient stop
// that lands on white cannot be told apart from one the design asked for.
//
// The reference values are the pixels Chromium itself paints for these exact
// colour strings, read back off a screenshot of solid swatches. That makes this
// agreement with the browser rather than agreement with a second copy of these
// matrices — including the out-of-gamut oklch, where the two could plausibly
// have disagreed about gamut mapping and do not.
TEST_CASE("oklab and oklch resolve to their sRGB colours",
          "[view][gradient][color]") {
    struct Case { const char* modern; const char* srgb; };
    for (const auto& c : std::vector<Case>{
             {"oklab(0.245896 -0.00351807 -0.00888171)", "rgb(29, 33, 37)"},
             {"oklab(0.94828 -0.086092 0.0563978)", "rgb(192, 255, 198)"},
             {"oklab(0.913804 -0.143517 0.0939829)", "rgb(142, 255, 157)"},
             {"oklch(0.7 0.2 145)", "rgb(48, 189, 68)"},
             {"oklch(0.55 0.12 250)", "rgb(50, 117, 180)"},
             {"oklab(0 0 0)", "rgb(0, 0, 0)"}}) {
        const auto got = parse_css_color(c.modern);
        const auto want = parse_css_color(c.srgb);
        INFO(c.modern << " -> " << got.r << "," << got.g << "," << got.b
                      << "  want " << want.r << "," << want.g << "," << want.b);
        // One 8-bit step of slack: the reference is a rounded serialization.
        CHECK(std::abs(got.r - want.r) <= 1.5f / 255.0f);
        CHECK(std::abs(got.g - want.g) <= 1.5f / 255.0f);
        CHECK(std::abs(got.b - want.b) <= 1.5f / 255.0f);
        CHECK(got.a == 1.0f);
    }
    // The white fallback is what made the defect invisible, so require these
    // NOT to be white. Without this the case passes if every value above is
    // parsed as the fallback and the reference happens to be near-white.
    const auto dark = parse_css_color("oklab(0.245896 -0.00351807 -0.00888171)");
    CHECK(dark.r < 0.5f);
    CHECK(dark.g < 0.5f);
    CHECK(dark.b < 0.5f);
}

TEST_CASE("an oklab alpha survives the conversion", "[view][gradient][color]") {
    const auto c = parse_css_color("oklab(0 0 0 / 0.62)");
    CHECK(std::abs(c.a - 0.62f) < 0.005f);
    const auto pct = parse_css_color("oklch(50% 0.1 30 / 50%)");
    CHECK(std::abs(pct.a - 0.5f) < 0.01f);
}
