// CSS backdrop-filter under opacity, judged against Chrome's own render.
//
// The document below is test/fixtures/import-differential/22-backdrop-opacity.html
// as DesignIR, built into native Views the way the importer does: five
// `backdrop-filter: blur(8px)` boxes over 1px gridlines. Every expected pixel
// was read off Chromium's render of that file by
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

#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::view;

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

IRNode absolute(const char* name, float left, float top, float width, float height) {
    IRNode node;
    node.type = "frame";
    node.name = name;
    node.style.position = "absolute";
    node.style.left = left;
    node.style.top = top;
    node.style.width = width;
    node.style.height = height;
    return node;
}

IRNode frost(float left, float top, float opacity) {
    IRNode node = absolute("frost", left, top, 80.0f, 80.0f);
    node.style.background_color = "rgba(12, 16, 22, 0.5)";
    node.style.backdrop_filter = "blur(8px)";
    if (opacity < 1.0f)
        node.style.opacity = opacity;
    return node;
}

IRNode group(float left, float opacity) {
    IRNode node = absolute("group", left, 40.0f, 80.0f, 80.0f);
    node.style.opacity = opacity;
    node.children.push_back(frost(0.0f, 0.0f, 1.0f));
    return node;
}

std::vector<uint8_t> render_fixture(uint32_t& w, uint32_t& h) {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "panel";
    ir.root.style.width = static_cast<float>(kW);
    ir.root.style.height = static_cast<float>(kH);
    ir.root.style.background_color = "#0B0F14";
    for (int x = 5; x < kW; x += 10) {
        IRNode line = absolute("grid", static_cast<float>(x), 0.0f, 1.0f, static_cast<float>(kH));
        line.style.background_color = "#8090A0";
        ir.root.children.push_back(std::move(line));
    }
    ir.root.children.push_back(frost(16.0f, 40.0f, 0.0f));
    ir.root.children.push_back(frost(112.0f, 40.0f, 0.5f));
    ir.root.children.push_back(frost(208.0f, 40.0f, 1.0f));
    ir.root.children.push_back(group(304.0f, 0.5f));
    ir.root.children.push_back(group(400.0f, 0.0f));
    auto root = build_native_view_tree(ir, ir.asset_manifest);
    REQUIRE(root != nullptr);
    return render_to_rgba(*root, kW, kH, 1.0f, &w, &h);
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

// A RecordingCanvas records the backdrop chain only as its summed blur, so it
// cannot tell a plain blur from a blur that ends in opacity(). This one keeps
// the list it was handed.
class ChainRecordingCanvas final : public pulp::canvas::RecordingCanvas {
  public:
    void save_backdrop_filter_chain(float x, float y, float w, float h,
                                    const FilterChainEntry* chain, int count) override {
        ++chain_calls;
        last_chain.assign(chain, chain + count);
        RecordingCanvas::save_backdrop_filter_chain(x, y, w, h, chain, count);
    }
    int chain_calls = 0;
    std::vector<FilterChainEntry> last_chain;
};

// Inert, so the allocation probe sees only what View::paint_all allocates.
class NoOpCanvas final : public pulp::canvas::Canvas {
  public:
    void save() override {}
    void restore() override {}
    void translate(float, float) override {}
    void scale(float, float) override {}
    void rotate(float) override {}
    void clip_rect(float, float, float, float) override {}
    void set_fill_color(pulp::canvas::Color) override {}
    void set_stroke_color(pulp::canvas::Color) override {}
    void set_line_width(float) override {}
    void set_line_cap(pulp::canvas::LineCap) override {}
    void set_line_join(pulp::canvas::LineJoin) override {}
    void fill_rect(float, float, float, float) override {}
    void stroke_rect(float, float, float, float) override {}
    void fill_rounded_rect(float, float, float, float, float) override {}
    void stroke_rounded_rect(float, float, float, float, float) override {}
    void fill_circle(float, float, float) override {}
    void stroke_circle(float, float, float) override {}
    void stroke_arc(float, float, float, float, float) override {}
    void stroke_line(float, float, float, float) override {}
    void set_font(const std::string&, float) override {}
    void set_text_align(pulp::canvas::TextAlign) override {}
    void fill_text(const std::string&, float, float) override {}
    float measure_text(const std::string&) override {
        return 0.0f;
    }
};

} // namespace

TEST_CASE("backdrop-filter is composited at the element's opacity as Chrome does",
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

// The opacity() step makes an opacity 0 backdrop vanish on Skia, but a backend
// without the filter chain drops that step and keeps the blur. Not opening the
// layer at all is what keeps such a backend right at opacity 0, and it skips
// a blur pass nobody sees.
TEST_CASE("an opacity 0 view opens no backdrop layer on any backend",
          "[view][canvas][backdrop-filter]") {
    using pulp::canvas::DrawCommand;
    for (const float opacity : {0.0f, 0.5f, 1.0f}) {
        View view;
        view.set_bounds({0.0f, 0.0f, 80.0f, 80.0f});
        view.set_backdrop_blur(8.0f);
        view.set_opacity(opacity);
        pulp::canvas::RecordingCanvas canvas;
        view.paint_all(canvas);
        INFO("opacity " << opacity);
        REQUIRE(canvas.count(DrawCommand::Type::save_backdrop_filter) ==
                (opacity > 0.0f ? 1u : 0u));
    }
}

TEST_CASE("a backdrop under opacity hands the canvas blur then opacity and no fade at 1",
          "[view][canvas][backdrop-filter]") {
    using Kind = pulp::canvas::Canvas::FilterChainEntry::Kind;
    using pulp::canvas::DrawCommand;
    auto paint = [](float opacity, bool saturate) {
        auto view = std::make_unique<View>();
        view->set_bounds({0.0f, 0.0f, 80.0f, 80.0f});
        view->set_backdrop_blur(8.0f);
        if (saturate) {
            View::FilterOp op;
            op.kind = View::FilterOp::Kind::saturate;
            op.amount = 0.5f;
            view->set_backdrop_filter_chain({op});
        }
        view->set_opacity(opacity);
        auto canvas = std::make_unique<ChainRecordingCanvas>();
        view->paint_all(*canvas);
        return canvas;
    };

    // Opacity 1 takes the plain blur and never sees an opacity() step.
    auto opaque = paint(1.0f, false);
    REQUIRE(opaque->chain_calls == 0);
    REQUIRE(opaque->count(DrawCommand::Type::save_backdrop_filter) == 1);

    // A colour list at opacity 1 is handed over as written, with no fade added.
    auto saturated = paint(1.0f, true);
    REQUIRE(saturated->chain_calls == 1);
    REQUIRE(saturated->last_chain.size() == 1);
    REQUIRE(saturated->last_chain[0].kind == Kind::saturate);

    // Below 1 the blur comes first and the element's opacity last.
    auto half = paint(0.5f, false);
    REQUIRE(half->chain_calls == 1);
    REQUIRE(half->last_chain.size() == 2);
    REQUIRE(half->last_chain[0].kind == Kind::blur);
    REQUIRE(half->last_chain[0].amount == 8.0f);
    REQUIRE(half->last_chain[1].kind == Kind::opacity);
    REQUIRE(half->last_chain[1].amount == 0.5f);

    // Under one 8-bit level of coverage the element paints nothing, and
    // neither does its backdrop.
    auto faint = paint(0.003f, false);
    REQUIRE(faint->chain_calls == 0);
    REQUIRE(faint->count(DrawCommand::Type::save_backdrop_filter) == 0);
}

TEST_CASE("painting a backdrop under opacity does not allocate",
          "[view][canvas][backdrop-filter][rt-safety]") {
    View root;
    root.set_bounds({0.0f, 0.0f, 160.0f, 80.0f});
    auto plain = std::make_unique<View>();
    plain->set_bounds({0.0f, 0.0f, 80.0f, 80.0f});
    plain->set_backdrop_blur(8.0f);
    plain->set_opacity(0.5f);
    root.add_child(std::move(plain));
    auto listed = std::make_unique<View>();
    listed->set_bounds({80.0f, 0.0f, 80.0f, 80.0f});
    View::FilterOp blur;
    blur.kind = View::FilterOp::Kind::blur;
    blur.amount = 8.0f;
    View::FilterOp saturate;
    saturate.kind = View::FilterOp::Kind::saturate;
    saturate.amount = 0.5f;
    listed->set_backdrop_filter_chain({blur, saturate});
    listed->set_opacity(0.5f);
    root.add_child(std::move(listed));

    NoOpCanvas canvas;
    root.paint_all(canvas); // Warm any non-steady View state before probing.
    pulp::test::RtAllocationProbe probe;
    for (int i = 0; i < 32; ++i)
        root.paint_all(canvas);
    CHECK_FALSE(probe.saw_allocation());
}
