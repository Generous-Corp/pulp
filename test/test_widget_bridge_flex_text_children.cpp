// Flex containers that carry BOTH their own text and element children.
//
// CSS wraps a container's bare text in an anonymous inline box that occupies
// its own slot on the flex line. A widget layer that instead paints the
// container's text at the container's content origin — while its element
// children lay out from that same origin — renders the two on top of each
// other, and the text carries zero weight in the container's own size.

#include <catch2/catch_test_macros.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

#include <cstdint>
#include <limits>
#include <algorithm>
#include <vector>

using namespace pulp::view;
using namespace pulp::state;

namespace {

// A 20x20 red dot followed by the container's own text "A". The container is
// pinned to `flex-start` on the cross axis so its main size hugs its content:
// its resolved width is therefore a direct readout of how much room the text
// was given on the flex line.
constexpr const char* kDotAndOwnTextScript = R"(
    createCol('outer', 'root');
    setFlex('outer', 'width', 200);
    setFlex('outer', 'height', 60);
    setFlex('outer', 'align_items', 'flex-start');
    setFlex('outer', 'padding_left', 10);
    setFlex('outer', 'padding_top', 10);
    setBackground('outer', '#000000');

    createLabel('box', 'A', 'outer');
    setFlex('box', 'direction', 'row');
    setFlex('box', 'align_items', 'center');
    setFlex('box', 'gap', 4);
    setFontSize('box', 16);
    setTextColor('box', '#ffffff');

    createRow('dot', 'box');
    setFlex('dot', 'width', 20);
    setFlex('dot', 'height', 20);
    setBackground('dot', '#ff0000');
)";

struct Harness {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge;

    Harness() : root(), store(), bridge(engine, root, store) {
        root.set_bounds({0, 0, 200, 60});
    }

    void run(const char* js) {
        bridge.load_script(js);
        root.layout_children();
    }
};

struct InkExtents {
    int count = 0;
    int min_x = std::numeric_limits<int>::max();
    int max_x = -1;
};

}  // namespace

TEST_CASE("a container's own text takes its own slot on the flex line",
          "[view][bridge][flex][anonymous-text]") {
    Harness h;
    h.run(kDotAndOwnTextScript);

    auto* box = h.bridge.widget("box");
    auto* dot = h.bridge.widget("dot");
    REQUIRE(box != nullptr);
    REQUIRE(dot != nullptr);

    // Positive control: the dot itself is measured, so a zero here would mean
    // the layout pass never ran rather than that the text was dropped.
    REQUIRE(dot->bounds().width == 20.0f);

    // The container hugs its content, so its width is dot + gap + text. If the
    // text is painted at the content origin instead of occupying a slot, the
    // container stops at the dot's trailing edge.
    REQUIRE(box->bounds().width > 24.0f);
}

TEST_CASE("a container's own text does not paint under its element children",
          "[view][bridge][flex][anonymous-text]") {
    if (!has_screenshot_backend()) {
        SKIP("no screenshot backend in this build");
    }

    Harness h;
    h.run(kDotAndOwnTextScript);

    uint32_t px_w = 0;
    uint32_t px_h = 0;
    const auto rgba = render_to_rgba(h.root, 200, 60, 2.0f, &px_w, &px_h);
    if (rgba.empty()) {
        SKIP("raw-RGBA capture unavailable in this build");
    }
    REQUIRE(px_w > 0);
    REQUIRE(px_h > 0);
    REQUIRE(rgba.size() == static_cast<size_t>(px_w) * px_h * 4);

    InkExtents red;
    InkExtents white;
    for (uint32_t y = 0; y < px_h; ++y) {
        for (uint32_t x = 0; x < px_w; ++x) {
            const size_t i = (static_cast<size_t>(y) * px_w + x) * 4;
            const uint8_t r = rgba[i];
            const uint8_t g = rgba[i + 1];
            const uint8_t b = rgba[i + 2];
            InkExtents* bucket = nullptr;
            if (r > 180 && g < 80 && b < 80) bucket = &red;
            else if (r > 180 && g > 180 && b > 180) bucket = &white;
            if (bucket == nullptr) continue;
            ++bucket->count;
            bucket->min_x = std::min(bucket->min_x, static_cast<int>(x));
            bucket->max_x = std::max(bucket->max_x, static_cast<int>(x));
        }
    }

    // Positive control for the pixel classifier: the dot is opaque red, so a
    // zero red count means the capture is blank and no absence claim about the
    // text below would mean anything.
    REQUIRE(red.count > 0);

    // The text must survive as visible ink. When it paints at the content
    // origin the opaque dot is drawn over it and it disappears entirely.
    REQUIRE(white.count > 0);

    // And it must sit beside the dot, not across it. Which side is
    // deliberately unasserted: a container's own text has no source position
    // among its element children, so the slot's placement on the line is not
    // something the widget model can derive. Disjointness is the invariant.
    INFO("red x [" << red.min_x << ", " << red.max_x << "], white x ["
         << white.min_x << ", " << white.max_x << "]");
    REQUIRE((white.max_x < red.min_x || white.min_x > red.max_x));
}
