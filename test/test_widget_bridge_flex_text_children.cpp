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
#include <pulp/canvas/font_resolver.hpp>

#include <cstdint>
#include <cmath>
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


// ---------------------------------------------------------------------------
// A container whose ONLY content is its own text. CSS still wraps that text in
// an anonymous flex item, so `justify-content` / `align-items` on the container
// distribute it exactly as they would distribute an element child. Painting the
// string at the content-box origin instead leaves both properties inert.
// ---------------------------------------------------------------------------
namespace {

// A 136x26 black Label pinned at the origin of a 200x60 root, rendered at
// scale 2. The label therefore owns device x [0, 272) and y [0, 52): its ink
// centre is (136, 26) when the text is centred on both axes.
constexpr float kLabelCentreX = 136.0f;
constexpr float kLabelCentreY = 26.0f;

constexpr const char* kTextOnlyJustifyCenter = R"(
    createLabel('btn', 'COPY', 'root');
    setFlex('btn', 'width', 136);
    setFlex('btn', 'height', 26);
    setFlex('btn', 'direction', 'row');
    setFlex('btn', 'justify_content', 'center');
    setFlex('btn', 'align_items', 'center');
    setFontSize('btn', 12);
    setTextColor('btn', '#ffffff');
    setBackground('btn', '#000000');
)";

// Cross axis only: a row container that centres items vertically but packs
// them at the start horizontally.
constexpr const char* kTextOnlyAlignCenterOnly = R"(
    createLabel('btn', 'COPY', 'root');
    setFlex('btn', 'width', 136);
    setFlex('btn', 'height', 26);
    setFlex('btn', 'direction', 'row');
    setFlex('btn', 'align_items', 'center');
    setFontSize('btn', 12);
    setTextColor('btn', '#ffffff');
    setBackground('btn', '#000000');
)";

// Main axis only, on a row container.
constexpr const char* kTextOnlyJustifyEnd = R"(
    createLabel('btn', 'COPY', 'root');
    setFlex('btn', 'width', 136);
    setFlex('btn', 'height', 26);
    setFlex('btn', 'direction', 'row');
    setFlex('btn', 'justify_content', 'flex-end');
    setFontSize('btn', 12);
    setTextColor('btn', '#ffffff');
    setBackground('btn', '#000000');
)";

// No alignment of any kind: the shape every plain Label in the tree has.
constexpr const char* kTextOnlyPlain = R"(
    createLabel('btn', 'COPY', 'root');
    setFlex('btn', 'width', 136);
    setFlex('btn', 'height', 26);
    setFontSize('btn', 12);
    setTextColor('btn', '#ffffff');
    setBackground('btn', '#000000');
)";

// Control: the same alignment applied to a real element child.
constexpr const char* kChildItemJustifyCenter = R"(
    createLabel('btn', '', 'root');
    setFlex('btn', 'width', 136);
    setFlex('btn', 'height', 26);
    setFlex('btn', 'direction', 'row');
    setFlex('btn', 'justify_content', 'center');
    setFlex('btn', 'align_items', 'center');
    setBackground('btn', '#000000');
    createLabel('txt', 'COPY', 'btn');
    setFontSize('txt', 12);
    setTextColor('txt', '#ffffff');
)";

// Control: alignment expressed through text-align, which the widget layer
// already honors. Proves the capture can see a horizontal move.
constexpr const char* kTextOnlyTextAlignCenter = R"(
    createLabel('btn', 'COPY', 'root');
    setFlex('btn', 'width', 136);
    setFlex('btn', 'height', 26);
    setFontSize('btn', 12);
    setTextColor('btn', '#ffffff');
    setBackground('btn', '#000000');
    setTextAlign('btn', 'center');
)";

struct InkBox {
    // Whether `render_to_rgba` produced a buffer. Distinct from `count`, and the
    // distinction is the whole point: a build without a raw-RGBA capture path
    // yields no pixels to inspect, which is not the same finding as a capture
    // that came back with no white ink in it. Folding the two together reports
    // an unbuilt capability as a layout failure.
    bool captured = false;
    int count = 0;
    int min_x = std::numeric_limits<int>::max();
    int max_x = -1;
    int min_y = std::numeric_limits<int>::max();
    int max_y = -1;
    float centre_x = -1.0f;
    float centre_y = -1.0f;
};

constexpr const char* kNoCapture = "raw-RGBA capture unavailable in this build";

// White ink over the black label, in device pixels.
InkBox measure_white_ink(Harness& h) {
    uint32_t px_w = 0, px_h = 0;
    const auto rgba = render_to_rgba(h.root, 200, 60, 2.0f, &px_w, &px_h);
    InkBox r;
    if (rgba.empty()) return r;
    r.captured = true;
    for (uint32_t y = 0; y < px_h; ++y) {
        for (uint32_t x = 0; x < px_w; ++x) {
            const size_t i = (static_cast<size_t>(y) * px_w + x) * 4;
            if (rgba[i] > 160 && rgba[i + 1] > 160 && rgba[i + 2] > 160) {
                ++r.count;
                r.min_x = std::min(r.min_x, static_cast<int>(x));
                r.max_x = std::max(r.max_x, static_cast<int>(x));
                r.min_y = std::min(r.min_y, static_cast<int>(y));
                r.max_y = std::max(r.max_y, static_cast<int>(y));
            }
        }
    }
    if (r.count > 0) {
        r.centre_x = (r.min_x + r.max_x) * 0.5f;
        r.centre_y = (r.min_y + r.max_y) * 0.5f;
    }
    return r;
}

}  // namespace

TEST_CASE("justify-content centres a container's own text on the main axis",
          "[view][bridge][flex][anonymous-text]") {
    if (!has_screenshot_backend()) SKIP("no screenshot backend in this build");
    Harness h;
    h.run(kTextOnlyJustifyCenter);
    auto* btn = h.bridge.widget("btn");
    REQUIRE(btn != nullptr);
    // Positive control: a zero here would mean layout never ran, which would
    // make any claim about where the text sits meaningless.
    REQUIRE(btn->bounds().width == 136.0f);

    auto r = measure_white_ink(h);
    if (!r.captured) SKIP(kNoCapture);
    REQUIRE(r.count > 0);
    INFO("white ink x [" << r.min_x << ", " << r.max_x << "] centre_x="
         << r.centre_x << "; y [" << r.min_y << ", " << r.max_y
         << "] centre_y=" << r.centre_y);
    CHECK(std::abs(r.centre_x - kLabelCentreX) < 12.0f);
}

TEST_CASE("align-items centres a container's own text on the cross axis",
          "[view][bridge][flex][anonymous-text]") {
    if (!has_screenshot_backend()) SKIP("no screenshot backend in this build");
    Harness h;
    h.run(kTextOnlyAlignCenterOnly);
    auto* btn = h.bridge.widget("btn");
    REQUIRE(btn != nullptr);
    REQUIRE(btn->bounds().height == 26.0f);

    auto r = measure_white_ink(h);
    if (!r.captured) SKIP(kNoCapture);
    REQUIRE(r.count > 0);
    INFO("white ink y [" << r.min_y << ", " << r.max_y << "] centre_y="
         << r.centre_y << "; x [" << r.min_x << ", " << r.max_x << "]");
    CHECK(std::abs(r.centre_y - kLabelCentreY) < 6.0f);
    // The main axis is untouched, so the text still starts at the leading edge.
    CHECK(r.min_x < 8);
}

TEST_CASE("justify-content:flex-end packs a container's own text to the end",
          "[view][bridge][flex][anonymous-text]") {
    if (!has_screenshot_backend()) SKIP("no screenshot backend in this build");
    Harness h;
    h.run(kTextOnlyJustifyEnd);
    auto r = measure_white_ink(h);
    if (!r.captured) SKIP(kNoCapture);
    REQUIRE(r.count > 0);
    INFO("white ink x [" << r.min_x << ", " << r.max_x << "]");
    // The label's trailing edge is device x 272.
    CHECK(r.max_x > 264);
}

TEST_CASE("a Label with no alignment keeps painting its text at the content origin",
          "[view][bridge][flex][anonymous-text]") {
    if (!has_screenshot_backend()) SKIP("no screenshot backend in this build");
    Harness h;
    h.run(kTextOnlyPlain);
    auto* btn = dynamic_cast<Label*>(h.bridge.widget("btn"));
    REQUIRE(btn != nullptr);
    // No alignment can displace the text, so no anonymous item is built and
    // the Label stays the measured leaf it has always been.
    CHECK_FALSE(btn->has_own_text_box());

    auto r = measure_white_ink(h);
    if (!r.captured) SKIP(kNoCapture);
    REQUIRE(r.count > 0);
    INFO("white ink x [" << r.min_x << ", " << r.max_x << "] y [" << r.min_y
         << ", " << r.max_y << "]");
    CHECK(r.min_x < 8);
    CHECK(r.min_y < 12);
}

TEST_CASE("an aligned text-only Label reserves an anonymous item, a plain one does not",
          "[view][bridge][flex][anonymous-text]") {
    Harness aligned;
    aligned.run(kTextOnlyJustifyCenter);
    auto* aligned_btn = dynamic_cast<Label*>(aligned.bridge.widget("btn"));
    REQUIRE(aligned_btn != nullptr);
    CHECK(aligned_btn->has_own_text_box());
    // The item is the text's own size, inset from the leading edge by the
    // space justify-content distributed in front of it.
    CHECK(aligned_btn->own_text_box().width < 136.0f);
    CHECK(aligned_btn->own_text_box().x > 0.0f);

    Harness plain;
    plain.run(kTextOnlyPlain);
    auto* plain_btn = dynamic_cast<Label*>(plain.bridge.widget("btn"));
    REQUIRE(plain_btn != nullptr);
    CHECK_FALSE(plain_btn->has_own_text_box());
}

TEST_CASE("browser-captured line boxes keep their own alignment",
          "[view][bridge][flex][anonymous-text]") {
    // A capture records each line rectangle relative to the owning element's
    // content box, so the browser's own distribution is already inside
    // `left`. Re-resolving it natively would offset the line a second time,
    // which is why a captured Label never takes an anonymous item.
    Harness h;
    h.run(kTextOnlyJustifyCenter);
    auto* btn = dynamic_cast<Label*>(h.bridge.widget("btn"));
    REQUIRE(btn != nullptr);
    // Positive control: without a capture this Label does take one, so a
    // false below means the capture suppressed it rather than that the gate
    // never fires for this markup.
    REQUIRE(btn->has_own_text_box());

    std::vector<Label::CachedLineBox> boxes;
    Label::CachedLineBox line;
    line.left = 40.0f;
    line.top = 6.0f;
    line.width = 56.0f;
    line.height = 14.0f;
    line.start = 0;
    line.length = 4;  // "COPY"
    boxes.push_back(line);
    // A cache is only installed when its basis face is verifiable, and a
    // build with no font backend resolves every family to an empty identity.
    // The scenario below is then unreachable rather than broken, so guard on
    // the identity: a cache that stays empty with a face in hand is still a
    // failure.
    const auto basis_face = pulp::canvas::resolved_face_identity(
        btn->effective_font_family(),
        static_cast<float>(btn->effective_font_weight()),
        pulp::canvas::FontSlant::Normal);
    if (basis_face.empty()) SKIP("no resolvable font face in this build");

    btn->set_cached_line_boxes(boxes, 136.0f, basis_face);
    REQUIRE_FALSE(btn->cached_line_boxes().empty());

    h.root.layout_children();
    CHECK_FALSE(btn->has_own_text_box());
}

TEST_CASE("text-align still centres a container's own text",
          "[view][bridge][flex][anonymous-text]") {
    if (!has_screenshot_backend()) SKIP("no screenshot backend in this build");
    Harness h;
    h.run(kTextOnlyTextAlignCenter);
    auto r = measure_white_ink(h);
    if (!r.captured) SKIP(kNoCapture);
    REQUIRE(r.count > 0);
    INFO("white ink x [" << r.min_x << ", " << r.max_x << "] centre_x="
         << r.centre_x);
    CHECK(std::abs(r.centre_x - kLabelCentreX) < 12.0f);
}

TEST_CASE("justify-content centres an element child, not only bare text",
          "[view][bridge][flex][anonymous-text]") {
    if (!has_screenshot_backend()) SKIP("no screenshot backend in this build");
    Harness h;
    h.run(kChildItemJustifyCenter);
    auto* txt = h.bridge.widget("txt");
    REQUIRE(txt != nullptr);
    auto r = measure_white_ink(h);
    if (!r.captured) SKIP(kNoCapture);
    REQUIRE(r.count > 0);
    INFO("child bounds x=" << txt->bounds().x << " w=" << txt->bounds().width
         << "; white ink x [" << r.min_x << ", " << r.max_x << "] centre_x="
         << r.centre_x);
    CHECK(std::abs(r.centre_x - kLabelCentreX) < 12.0f);
}
