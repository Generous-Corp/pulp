// Metamorphic inert-style suite for the native DesignIR materializer.
//
// A style declaration that restates the initial value (`opacity: 1`,
// `transform: none`, `clip-path: none`, ...) must not change what a node
// paints. The hazard is not the value itself but what a materializer or paint
// path does on seeing the property at all: open a layer it then composites at
// full strength, push a clip that covers everything, save a canvas state it
// restores in the wrong place. None of those shows as a wrong colour, so no
// fidelity scorer looks for them, and a leaked save or clip changes the node
// that paints NEXT rather than the one that declared it.
//
// Each case builds the same scene twice through the IR JSON reader, which is
// the path real imports take, with and without the declaration on the subject
// node, and requires:
//   * an identical recorded drawlist for the whole scene,
//   * an identical drawlist from the following sibling onward, so a leak into
//     the next node is named as such rather than as a generic mismatch,
//   * an identical Skia raster.
//
// Each case also carries a live value of the same property that must change
// the drawlist. Without it a property the materializer ignores entirely would
// pass as "inert" while the suite measured nothing.

#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/drawlist_format.hpp>
#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir_io.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::view;

namespace {

constexpr float kSceneWidth = 160.0f;
constexpr float kSceneHeight = 100.0f;

// The sibling's fill, #cc3333 in the scene below. It is the only use of this
// colour in the scene, so the first command that sets it marks where the
// sibling starts painting.
const pulp::canvas::Color kSiblingFill = pulp::canvas::Color::hex(0xcc3333);

// The subject overlaps its following sibling (so stacking order is visible)
// and has a child that overflows its box (so a clip is visible). `extra` is
// spliced into the subject's style object.
std::string scene_json(const std::string& extra) {
    std::string subject_style =
        R"("position": "absolute", "left": 10, "top": 10, "width": 60, "height": 60,
           "backgroundColor": "#3366cc")";
    if (!extra.empty())
        subject_style += ", " + extra;
    return std::string(R"json({
        "type": "frame", "name": "Scene",
        "style": {"width": 160, "height": 100, "backgroundColor": "#202020"},
        "children": [
          {"type": "frame", "name": "Subject",
           "style": {)json") +
           subject_style + R"json(},
           "children": [
             {"type": "frame", "name": "Overflowing",
              "style": {"position": "absolute", "left": 40, "top": 40,
                        "width": 40, "height": 40, "backgroundColor": "#ffcc00"}}
           ]},
          {"type": "frame", "name": "Sibling",
           "style": {"position": "absolute", "left": 50, "top": 30,
                     "width": 60, "height": 50, "backgroundColor": "#cc3333"}}
        ]
    })json";
}

struct Rendered {
    std::string drawlist;
    std::string from_sibling;
    std::vector<uint8_t> png;
};

// The drawlist from the sibling's own `save` onward, headed by how many saves
// are still open when it starts. A declaration that leaves a save, layer or
// clip open on the subject changes that count even when the sibling's own
// commands are textually identical.
std::string sibling_suffix(const std::vector<pulp::canvas::DrawCommand>& commands) {
    using Type = pulp::canvas::DrawCommand::Type;
    std::size_t fill = commands.size();
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const auto& c = commands[i];
        if (c.type == Type::set_fill_color && c.color.r == kSiblingFill.r &&
            c.color.g == kSiblingFill.g && c.color.b == kSiblingFill.b &&
            c.color.a == kSiblingFill.a) {
            fill = i;
            break;
        }
    }
    if (fill == commands.size())
        FAIL("the sibling's fill colour never reached the canvas");
    std::size_t start = fill;
    while (start > 0 && commands[start].type != Type::save)
        --start;

    int open = 0;
    for (std::size_t i = 0; i < start; ++i) {
        const std::string_view name = pulp::canvas::draw_command_name(commands[i].type);
        if (name.substr(0, 4) == "save")
            ++open;
        else if (name == "restore")
            --open;
    }
    return "open saves before the sibling: " + std::to_string(open) + "\n" +
           pulp::canvas::format_commands(
               {commands.begin() + static_cast<std::ptrdiff_t>(start), commands.end()});
}

Rendered render_scene(const std::string& extra) {
    const auto ir = parse_design_ir_json(scene_json(extra));
    auto root = build_native_view_tree(ir, ir.asset_manifest, {});
    REQUIRE(root != nullptr);
    root->set_bounds({0.0f, 0.0f, kSceneWidth, kSceneHeight});
    root->layout_children();

    Rendered out;
    pulp::canvas::RecordingCanvas canvas;
    root->paint_all(canvas);
    out.drawlist = pulp::canvas::format_commands(canvas.commands());
    out.from_sibling = sibling_suffix(canvas.commands());
    out.png = render_to_png(*root, static_cast<uint32_t>(kSceneWidth),
                            static_cast<uint32_t>(kSceneHeight), 1.0f, ScreenshotBackend::skia);
    return out;
}

struct InertCase {
    const char* property;
    std::string inert;
    // A value of the same property that must change the drawlist. Empty when
    // the native materializer does not paint the property at all; the case
    // then proves only that the declaration costs nothing.
    std::string live;
};

const std::vector<InertCase>& inert_cases() {
    static const std::vector<InertCase> cases = {
        {"opacity", R"css("opacity": 1)css", R"css("opacity": 0.5)css"},
        {"transform none", R"css("transform": "none")css", R"css("transform": "rotate(20deg)")css"},
        {"transform identity rotate", R"css("transform": "rotate(0deg)")css",
         R"css("transform": "rotate(20deg)")css"},
        {"filter", R"css("filter": "none")css", R"css("filter": "blur(4px)")css"},
        {"box-shadow", R"css("boxShadow": "none")css",
         R"css("boxShadow": "0px 4px 8px rgba(0, 0, 0, 0.5)")css"},
        {"border-width", R"css("borderWidth": 0, "borderColor": "#ffffff")css",
         R"css("borderWidth": 3, "borderColor": "#ffffff")css"},
        {"overflow", R"css("overflow": "visible")css", R"css("overflow": "hidden")css"},
        {"z-index", R"css("zIndex": 0)css", R"css("zIndex": 5)css"},
        {"mix-blend-mode", R"css("mixBlendMode": "normal")css",
         R"css("mixBlendMode": "multiply")css"},
        {"backdrop-filter", R"css("backdropFilter": "none")css",
         R"css("backdropFilter": "blur(6px)")css"},
        {"clip-path", R"css("clipPath": "none")css",
         R"css("clipPath": "polygon(0% 0%, 100% 0%, 0% 100%)")css"},
        // CSS keywords are case-insensitive, and an attribute value can carry
        // whitespace the stylesheet parser would have trimmed.
        {"clip-path keyword spelling", R"css("clipPath": " None ")css",
         R"css("clipPath": "polygon(0% 0%, 100% 0%, 0% 100%)")css"},
        {"background-image empty", R"css("backgroundImage": "")css", ""},
        {"background-image none", R"css("backgroundImage": "none")css", ""},
    };
    return cases;
}

} // namespace

TEST_CASE(
    "an inert style declaration leaves the native drawlist, raster and next sibling unchanged",
    "[view][import][native-materializer][inert-style]") {
    const auto baseline = render_scene("");
    REQUIRE_FALSE(baseline.drawlist.empty());

    for (const auto& c : inert_cases()) {
        DYNAMIC_SECTION(c.property) {
            const auto with = render_scene(c.inert);
            INFO("declaration: " << c.inert);
            // The sibling comparison is first so a leak is reported as one.
            CHECK(with.from_sibling == baseline.from_sibling);
            CHECK(with.drawlist == baseline.drawlist);
            if (!baseline.png.empty() && !with.png.empty())
                CHECK(with.png == baseline.png);
        }
    }
}

TEST_CASE("a live value of each inert-style property changes the native drawlist",
          "[view][import][native-materializer][inert-style]") {
    // The control for the case above: if the reader or the materializer drops a
    // property, its inert value and its live value both match the baseline, and
    // only this case notices.
    const auto baseline = render_scene("");
    for (const auto& c : inert_cases()) {
        if (c.live.empty())
            continue;
        DYNAMIC_SECTION(c.property) {
            const auto with = render_scene(c.live);
            INFO("declaration: " << c.live);
            CHECK(with.drawlist != baseline.drawlist);
        }
    }
}

TEST_CASE("the inert-style raster comparison sees a one-node colour change",
          "[view][import][native-materializer][inert-style]") {
    // The raster half of the suite compares encoded PNG bytes. This proves the
    // comparison is live in this build rather than two empty buffers agreeing.
    const auto baseline = render_scene("");
    if (baseline.png.empty())
        SKIP("Skia raster screenshot backend unavailable");
    const auto changed = render_scene(R"css("borderWidth": 3, "borderColor": "#ffffff")css");
    REQUIRE_FALSE(changed.png.empty());
    CHECK(changed.png != baseline.png);
}

TEST_CASE("the inert-style sibling comparison reports a save the subject left open",
          "[view][import][native-materializer][inert-style]") {
    // The sibling's own commands are the same in both recordings; only the
    // unbalanced save before it differs. A comparison of the sibling's commands
    // alone would call these equal.
    auto record = [](bool leak) {
        pulp::canvas::RecordingCanvas canvas;
        canvas.save();
        if (leak)
            canvas.save();
        canvas.fill_rect(0, 0, 10, 10);
        if (!leak)
            canvas.restore();
        canvas.save();
        canvas.translate(50, 30);
        canvas.set_fill_color(kSiblingFill);
        canvas.fill_rect(0, 0, 60, 50);
        canvas.restore();
        return sibling_suffix(canvas.commands());
    };
    CHECK(record(true) != record(false));
}
