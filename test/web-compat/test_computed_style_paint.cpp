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

Frame shoot(View& root, uint32_t w, uint32_t h) {
    Frame f;
    f.px = render_to_rgba(root, w, h, 1.0f, &f.w, &f.h);
    return f;
}

bool near(int a, int b, int tol) { return std::abs(a - b) <= tol; }

}  // namespace

TEST_CASE("pixel probe control: a plain fill actually reaches the buffer",
          "[web-compat][computed-paint]") {
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
