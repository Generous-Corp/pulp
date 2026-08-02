// Can Pulp's own web-compat engine draw an agent-authored panel's CSS — no
// Chromium, no screenshot, no bitmap?
//
// The imported panels ship as a captured PNG with hit-boxes over it, which
// means the CSS is discarded at import and every appearance problem becomes an
// image-pipeline problem. This probe answers whether that trade is necessary by
// running the SAME html + pack CSS through the DOM/CSS shim and Yoga, painting
// with CPU Skia, and writing a PNG that can be put beside Chrome's.
//
// Opt-in: it renders a specific design off disk, so it is inert unless
// PULP_CSS_PROBE_DIR names a directory holding the extracted `probe.css` and
// `probe.html`. Nothing about it runs in an ordinary suite.

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

#include <cstdlib>
#include <sstream>

using namespace pulp::test;
using namespace pulp::view;

namespace {

std::string slurp_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A JS string literal the shim will see byte-for-byte. The CSS carries quotes,
// backslashes and newlines; interpolating it raw would end the literal.
std::string js_string(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += "\"";
    return out;
}

}  // namespace

TEST_CASE("probe control: createElement builds views",
          "[web-compat][css-probe][.]") {
    TestEnvironment env(400, 300);
    env.run(R"JS(
        var a = document.createElement('div');
        a.style.background = '#ff0000';
        a.style.width = '100px'; a.style.height = '50px';
        document.body.appendChild(a);
        var b = document.createElement('div');
        b.style.background = '#00ff00';
        document.body.appendChild(b);
    )JS");
    int nodes = 0;
    std::function<void(const View&)> count = [&](const View& v) {
        ++nodes;
        for (std::size_t i = 0; i < v.child_count(); ++i)
            if (const auto* c = v.child_at(i)) count(*c);
    };
    count(env.root);
    WARN("createElement nodes: " << nodes);
}

TEST_CASE("probe control: innerHTML builds views",
          "[web-compat][css-probe][.]") {
    TestEnvironment env(400, 300);
    env.run(R"JS(document.body.innerHTML = "<div id='q'><span>hi</span></div>";)JS");
    int nodes = 0;
    std::function<void(const View&)> count = [&](const View& v) {
        ++nodes;
        for (std::size_t i = 0; i < v.child_count(); ++i)
            if (const auto* c = v.child_at(i)) count(*c);
    };
    count(env.root);
    WARN("innerHTML nodes: " << nodes);
}

TEST_CASE("probe control: innerHTML DOM vs native views",
          "[web-compat][css-probe][.]") {
    TestEnvironment env(400, 300);
    env.run(R"JS(document.body.innerHTML = "<div><span>hi</span></div>";)JS");
    auto dom = env.engine.evaluate("document.body.children.length");
    WARN("DOM children after innerHTML: "
         << dom.getWithDefault<double>(-1));
    auto tag = env.engine.evaluate(
        "document.body.children.length ? document.body.children[0].tagName : 'NONE'");
    WARN("first child tag: "
         << std::string(tag.getWithDefault<std::string_view>("?")));
    WARN("native root children: " << env.root.child_count());
}

TEST_CASE("web-compat draws an agent panel from its own CSS",
          "[web-compat][css-probe][.]") {
    const char* dir_env = std::getenv("PULP_CSS_PROBE_DIR");
    if (dir_env == nullptr || *dir_env == '\0') {
        WARN("SKIPPED: set PULP_CSS_PROBE_DIR to a directory with probe.css + probe.html");
        return;
    }
    const std::filesystem::path dir(dir_env);
    const std::string css = slurp_file(dir / "probe.css");
    const std::string html = slurp_file(dir / "probe.html");
    REQUIRE_FALSE(css.empty());
    REQUIRE_FALSE(html.empty());

    // Chrome captured this document at 1280x921 logical, 2x. Matching both means
    // the two PNGs can be diffed pixel-for-pixel instead of eyeballed.
    const int w = 1280, h = 921;
    const float scale = 2.0f;

    TestEnvironment env(static_cast<float>(w), static_cast<float>(h));

    std::string js;
    js += "var __s = document.createElement('style');\n";
    js += "__s.textContent = " + js_string(css) + ";\n";
    // `document.head` does not exist in the shim; a <style> is ingested on
    // append wherever it lands, so the document body is the available hook.
    js += "document.body.appendChild(__s);\n";
    js += "document.body.innerHTML = " + js_string(html) + ";\n";
    env.run(js);

    // CPU Skia, explicitly. The platform default is CoreGraphics, whose canvas
    // does not implement the same paint surface, and Graphite has an unrelated
    // image defect that would confound the result.
    // Logical size plus a scale factor — render_to_png applies the scale
    // itself. Passing pre-multiplied dimensions AND the scale renders at 4x
    // and silently invalidates any pixel comparison against the reference.
    auto png = render_to_png(env.root, static_cast<uint32_t>(w),
                             static_cast<uint32_t>(h), scale,
                             ScreenshotBackend::skia);
    REQUIRE_FALSE(png.empty());

    const auto out = dir / "pulp-native.png";
    std::ofstream o(out, std::ios::binary);
    o.write(reinterpret_cast<const char*>(png.data()),
            static_cast<std::streamsize>(png.size()));
    o.close();
    WARN("wrote " << out.string() << " (" << png.size() << " bytes)");

    // How much of the document actually became views. A shim that silently
    // dropped the markup would still write a valid, empty PNG.
    int nodes = 0;
    std::function<void(const View&)> count = [&](const View& v) {
        ++nodes;
        for (std::size_t i = 0; i < v.child_count(); ++i)
            if (const auto* c = v.child_at(i)) count(*c);
    };
    count(env.root);
    WARN("view nodes materialised: " << nodes);
    CHECK(nodes > 10);
}
