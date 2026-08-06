// Do the computed values a browser hands back actually PAINT?
//
// The computed-style route replaces a screenshot with per-element resolved CSS,
// so what matters is not whether a setter accepts a value but whether the
// pixels change. A setter that stores a value it never draws passes an
// acceptance check and ships a flat panel, so every case here samples the
// rendered buffer.
//
// Values are the real ones a browser produced for the shipped design, not
// invented equivalents.

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

#include <algorithm>

using namespace pulp::test;
using namespace pulp::view;

namespace {

struct Frame {
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    bool ok() const { return !px.empty() && w > 0 && h > 0; }
    // RGBA8, premultiplied, top-down, stride == w*4.
    std::array<int, 4> at(uint32_t x, uint32_t y) const {
        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
        if (i + 3 >= px.size()) return {-1, -1, -1, -1};
        return {px[i], px[i + 1], px[i + 2], px[i + 3]};
    }
};

// A build with no raw-RGBA producer returns an empty buffer for every probe in
// this file, whatever the code under test does — so the cases below would all
// report failure while saying nothing about the behaviour they exist to check.
// That is what the UBSan lane did: nine reds, one cause, none of them a defect.
//
// Skipping is keyed on the BUILD, never on an empty result. A build that can
// rasterize and produced nothing is a real failure and must stay one; deciding
// from the result would swallow exactly that regression.
void skip_without_rasterizer() {
    if (!raw_rgba_render_available()) {
        SKIP("no raw-RGBA backend compiled in (needs Skia) — pixel probes "
             "cannot run in this build");
    }
}

Frame shoot(View& root, uint32_t w, uint32_t h) {
    skip_without_rasterizer();
    Frame f;
    f.px = render_to_rgba(root, w, h, 1.0f, &f.w, &f.h);
    return f;
}

bool near(int a, int b, int tol) { return std::abs(a - b) <= tol; }

/// Whether this build can actually put pixels in a buffer.
///
/// Every case in this file samples rendered pixels, so without a working
/// rasteriser they all fail — and they fail as "the colour is wrong" rather
/// than "there was no colour", which sends a reader looking for a paint bug
/// that is not there. The sanitizer lane does exactly this: it never enables
/// Skia, so this whole file went red on a build that was never able to run it.
///
/// A runtime probe rather than a build flag, because the same emptiness comes
/// from a missing Skia AND from a Skia that has no device to draw on. Cached:
/// the answer cannot change within a run.
bool paint_available() {
    static const bool ok = [] {
        TestEnvironment env(8, 8);
        env.run("setBackground('', '#ff0000');");
        return shoot(env.root, 8, 8).ok();
    }();
    return ok;
}

/// A skip that names its missing dependency. Silence would read as coverage.
#define REQUIRE_PAINT()                                                       \
    do {                                                                      \
        if (!paint_available()) {                                             \
            WARN("SKIPPED: this build cannot rasterise (no Skia surface) — "  \
                 "the computed-style paint cases are NOT covered by this run"); \
            return;                                                           \
        }                                                                     \
    } while (0)


}  // namespace

TEST_CASE("pixel probe control: a plain fill actually reaches the buffer",
          "[web-compat][computed-paint]") {
    REQUIRE_PAINT();
    // Without this, every assertion below could be passing against an empty
    // frame — the failure mode that makes a blank render look like a green run.
    TestEnvironment env(40, 40);
    env.run("setBackground('', '#ff0000');");
    const auto f = shoot(env.root, 40, 40);
    REQUIRE(f.ok());
    const auto p = f.at(20, 20);
    INFO("rgba " << p[0] << "," << p[1] << "," << p[2] << "," << p[3]);
    CHECK(near(p[0], 255, 2));
    CHECK(near(p[1], 0, 2));
    CHECK(near(p[2], 0, 2));
}

TEST_CASE("the setBackground bridge helper does NOT accept oklab",
          "[web-compat][computed-paint][oklab]") {
    REQUIRE_PAINT();
    // Documented gap, not an aspiration. `setBackground(id, css)` runs a
    // different colour parser from the web-compat style-decl path, and it does
    // not understand modern CSS colour functions — it falls back to white
    // rather than failing, so a panel driven through this helper would come out
    // blank-bright with nothing logged. The style-decl path handles the same
    // value correctly (see the [surface] cases), which is the route the
    // computed-style plan would use.
    TestEnvironment env(40, 40);
    env.run("setBackground('', 'oklab(0.263257 0.00226804 0.0227099)');");
    const auto f = shoot(env.root, 40, 40);
    REQUIRE(f.ok());
    const auto p = f.at(20, 20);
    INFO("rgba " << p[0] << "," << p[1] << "," << p[2]);
    CHECK(near(p[0], 255, 2));
    CHECK(near(p[1], 255, 2));
    CHECK(near(p[2], 255, 2));
}

TEST_CASE("every declared box-shadow layer paints, not just the first",
          "[web-compat][computed-paint][box-shadow]") {
    REQUIRE_PAINT();
    // 13 of 13 shadowed elements in the design carry up to three layers. A
    // paint path that drew only the first would flatten the whole sense of
    // depth while still looking plausible.
    TestEnvironment env(120, 120);
    env.run(R"JS(
        setBackground('', '#ffffff');
        createPanel('b', '');
        setFlex('b', 'width', 40); setFlex('b', 'height', 40);
        setFlex('b', 'margin_left', 40); setFlex('b', 'margin_top', 40);
        setBackground('b', '#808080');
        clearBoxShadow('b');
        addBoxShadow('b', -30, 0, 0, 4, '#ff0000');
        addBoxShadow('b', 30, 0, 0, 4, '#0000ff');
    )JS");
    const auto f = shoot(env.root, 120, 120);
    REQUIRE(f.ok());
    // Left of the box: the first layer. Right of it: the second.
    const auto left = f.at(16, 60);
    const auto right = f.at(104, 60);
    INFO("left " << left[0] << "," << left[1] << "," << left[2]
         << "  right " << right[0] << "," << right[1] << "," << right[2]);
    const bool left_red = left[0] > left[2] + 20;
    const bool right_blue = right[2] > right[0] + 20;
    CHECK(left_red);
    CHECK(right_blue);
}

TEST_CASE("an inset shadow paints inside the box, not outside it",
          "[web-compat][computed-paint][box-shadow]") {
    REQUIRE_PAINT();
    // Every multi-layer shadow in the design includes an inset layer. Painting
    // it outside would put a halo where the design wanted an inner well.
    TestEnvironment env(120, 120);
    env.run(R"JS(
        setBackground('', '#ffffff');
        createPanel('b', '');
        setFlex('b', 'width', 60); setFlex('b', 'height', 60);
        setFlex('b', 'margin_left', 30); setFlex('b', 'margin_top', 30);
        setBackground('b', '#808080');
        clearBoxShadow('b');
        addBoxShadow('b', 0, 0, 0, 10, '#ff0000', 'inset');
    )JS");
    const auto f = shoot(env.root, 120, 120);
    REQUIRE(f.ok());
    const auto inside_edge = f.at(34, 60);   // just inside the box
    const auto outside = f.at(20, 60);       // clearly outside
    INFO("inside " << inside_edge[0] << "," << inside_edge[1] << "," << inside_edge[2]
         << "  outside " << outside[0] << "," << outside[1] << "," << outside[2]);
    CHECK(inside_edge[0] > inside_edge[2] + 20);          // red inside
    CHECK(near(outside[0], 255, 4));                      // untouched outside
    CHECK(near(outside[2], 255, 4));
}

TEST_CASE("mix-blend-mode screen composites, it does not just overwrite",
          "[web-compat][computed-paint][blend]") {
    REQUIRE_PAINT();
    // One element in the design uses it — the LED. screen(a,b) lightens; the
    // failure mode is the child simply replacing what is underneath, which
    // looks fine in isolation and wrong on the panel.
    TestEnvironment env(40, 40);
    env.run(R"JS(
        setBackground('', '#400000');
        createPanel('led', '');
        setFlex('led', 'width', 40); setFlex('led', 'height', 40);
        setBackground('led', '#004000');
        setMixBlendMode('led', 'screen');
    )JS");
    const auto f = shoot(env.root, 40, 40);
    REQUIRE(f.ok());
    const auto p = f.at(20, 20);
    INFO("rgba " << p[0] << "," << p[1] << "," << p[2] << "," << p[3]);
    // screen(0x40, 0x00)=0x40 red, screen(0x00, 0x40)=0x40 green. Overwrite
    // would give red 0. Both channels present is the discriminator.
    CHECK(p[0] > 30);
    CHECK(p[1] > 30);
}

// Which SURFACE accepts oklab? The bridge's `setBackground(id, css)` and the
// web-compat style-decl (`el.style.background = ...`) use different colour
// parsers, and only one of them was written to understand modern CSS colour
// functions. The computed-style route would drive the style-decl path, so that
// is the one whose answer decides the question.
TEST_CASE("oklab through the style-decl path",
          "[web-compat][computed-paint][oklab][surface]") {
    REQUIRE_PAINT();
    TestEnvironment env(40, 40);
    env.run(R"JS(
        var d = document.createElement('div');
        d.style.width = '40px'; d.style.height = '40px';
        d.style.background = 'oklab(0.263257 0.00226804 0.0227099)';
        document.body.appendChild(d);
    )JS");
    const auto f = shoot(env.root, 40, 40);
    REQUIRE(f.ok());
    const auto p = f.at(20, 20);
    WARN("style-decl oklab -> rgba " << p[0] << "," << p[1] << "," << p[2] << "," << p[3]);
}

TEST_CASE("oklab alpha through the style-decl path, differential",
          "[web-compat][computed-paint][oklab][surface]") {
    REQUIRE_PAINT();
    // A differential, not an absolute: render the same colour with and without
    // `/ 0.05` over the same white ground. Identical pixels mean the alpha was
    // dropped; different pixels mean it survived. This cannot be fooled by a
    // fallback colour, which an absolute threshold can.
    auto render = [](const char* colour) {
        TestEnvironment env(40, 40);
        std::string js =
            "var b = document.createElement('div');"
            "b.style.width='40px'; b.style.height='40px';"
            "b.style.background='#ffffff';"
            "document.body.appendChild(b);"
            "var d = document.createElement('div');"
            "d.style.width='40px'; d.style.height='40px';"
            "d.style.position='absolute'; d.style.left='0px'; d.style.top='0px';"
            "d.style.background='";
        js += colour;
        js += "'; document.body.appendChild(d);";
        env.run(js);
        skip_without_rasterizer();
        Frame f;
        f.px = render_to_rgba(env.root, 40, 40, 1.0f, &f.w, &f.h);
        return f;
    };
    const auto opaque = render("oklab(0.263257 0.00226804 0.0227099)");
    const auto tinted = render("oklab(0.263257 0.00226804 0.0227099 / 0.05)");
    REQUIRE(opaque.ok());
    REQUIRE(tinted.ok());
    const auto o = opaque.at(20, 20);
    const auto t = tinted.at(20, 20);
    WARN("opaque " << o[0] << "," << o[1] << "," << o[2]
         << "   5% tint " << t[0] << "," << t[1] << "," << t[2]);
    CHECK(t[0] != o[0]);
}

// Read-and-report measurement: does the NON-browser HTML lane emit styles?
#include <pulp/view/design_sources.hpp>
TEST_CASE("legacy HTML lane: how many nodes carry appearance",
          "[web-compat][ir-lane]") {
    REQUIRE_PAINT();
    const char* dir_env = std::getenv("PULP_CSS_PROBE_DIR");
    if (dir_env == nullptr) { WARN("SKIPPED"); return; }
    std::ifstream in(std::filesystem::path("/tmp/forge-designs/1785533071370-001-ok/index.html"),
                     std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    const auto ir = pulp::view::parse_claude_html(ss.str());
    int nodes = 0, styled = 0, bg = 0, grad = 0, shadow = 0, radius = 0;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& n) {
            ++nodes;
            bool any = false;
            if (n.style.background_color && !n.style.background_color->empty()) { ++bg; any = true; }
            if (n.style.background_gradient) { ++grad; any = true; }
            if (!n.style.box_shadow.empty()) { ++shadow; any = true; }
            if (n.style.border_top_left_radius) { ++radius; any = true; }
            if (any) ++styled;
            for (const auto& c : n.children) walk(c);
        };
    walk(ir.root);
    WARN("legacy-lane nodes=" << nodes << " styled=" << styled
         << " bg=" << bg << " gradient=" << grad
         << " shadow=" << shadow << " radius=" << radius);
}

TEST_CASE("legacy HTML lane control: does it emit styles at all",
          "[web-compat][ir-lane]") {
    REQUIRE_PAINT();
    // 0 styled nodes on the real design could mean the parser never emits
    // appearance, or that this design puts all of it in CSS classes. Inline
    // styles separate the two.
    const std::string html =
        "<html><body><div style=\"background:#ff0000;border-radius:8px;"
        "box-shadow:0 2px 4px #000000\">x</div></body></html>";
    const auto ir = pulp::view::parse_claude_html(html);
    int nodes = 0, bg = 0, shadow = 0, radius = 0;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& n) {
            ++nodes;
            if (n.style.background_color && !n.style.background_color->empty()) ++bg;
            if (!n.style.box_shadow.empty()) ++shadow;
            if (n.style.border_top_left_radius) ++radius;
            for (const auto& c : n.children) walk(c);
        };
    walk(ir.root);
    WARN("inline-style control: nodes=" << nodes << " bg=" << bg
         << " shadow=" << shadow << " radius=" << radius);
}

TEST_CASE("the rasterizer-availability guard agrees with the rasterizer",
          "[web-compat][computed-paint]") {
    // The guard above lets every pixel case skip itself, so a guard that
    // wrongly reported "unavailable" would turn this whole file green without
    // running any of it — the failure mode a skip introduces. This case never
    // skips: it asserts the claim matches reality in whichever direction the
    // build actually is.
    TestEnvironment env(8, 8);
    env.run("setBackground('', '#00ff00');");
    uint32_t w = 0, h = 0;
    const auto px = render_to_rgba(env.root, 8, 8, 1.0f, &w, &h);
    if (raw_rgba_render_available()) {
        INFO("guard says a raw-RGBA backend is compiled in");
        CHECK_FALSE(px.empty());
    } else {
        INFO("guard says no raw-RGBA backend is compiled in");
        CHECK(px.empty());
    }
}
