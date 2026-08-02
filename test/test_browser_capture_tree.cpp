// SPDX-License-Identifier: MIT
//
// Whole-tree lowering: every node Chrome painted becomes an IR node drawn at
// Chrome's own solved box, so a panel is drawn rather than photographed.
//
// These cases deliberately assert the SHAPE OF THE TREE — which nodes exist,
// what strings they carry, where they sit, what order they paint in — and never
// a similarity score. A panel can score well while being a photograph; that is
// exactly the defect this work exists to remove, so a score cannot be the
// instrument that proves it gone.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "tools/import-design/browser_capture_ir.hpp"
#include "tools/import-design/browser_capture_tree.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

using namespace pulp::import_design;
using pulp::view::IRNode;
using pulp::view::NodeRenderMode;

namespace {

namespace fs = std::filesystem;

fs::path fixture_envelope() {
    return fs::path(PULP_BROWSER_CAPTURE_STYLE_FIXTURE_DIR) / "capture.json";
}

BrowserCaptureIrResult lower_fixture(bool native) {
    BrowserCaptureIrOptions options;
    options.native_panel_lowering = native;
    return lower_browser_capture_to_ir(fixture_envelope(), options);
}

const IRNode* find_by_text(const IRNode& root, std::string_view text) {
    for (const auto& child : root.children)
        if (child.type == "text" && child.text_content == text) return &child;
    return nullptr;
}

/// Index of the first child matching a predicate, or -1.
template <typename Predicate>
int index_where(const IRNode& root, Predicate predicate) {
    for (size_t i = 0; i < root.children.size(); ++i)
        if (predicate(root.children[i])) return static_cast<int>(i);
    return -1;
}

std::string attribute(const IRNode& node, const std::string& key) {
    const auto it = node.attributes.find(key);
    return it == node.attributes.end() ? std::string{} : it->second;
}

bool any_capture_node(const IRNode& node) {
    if (node.render_mode == NodeRenderMode::faithful_capture) return true;
    return std::any_of(node.children.begin(), node.children.end(),
                       [](const IRNode& child) {
                           return any_capture_node(child);
                       });
}

}  // namespace

TEST_CASE("native lowering draws the panel's real nodes, not a bitmap",
          "[browser-capture][native-lowering]") {
    const auto native = lower_fixture(true);
    REQUIRE(native.error.empty());
    REQUIRE(native.design_ir);
    const auto& root = native.design_ir->root;

    // (1) The photograph is gone. Not "smaller", not "behind" — absent. A
    // faithful_capture anywhere in the tree means the panel is still a picture
    // with native nodes decorating it.
    CHECK_FALSE(any_capture_node(root));
    CHECK_FALSE(root.capture_asset_id.has_value());
    CHECK(attribute(root, "asset_ref").empty());

    // (2) The design's own nodes are present, carrying their real strings. The
    // fixture's two captions live inside the capture bitmap today; here they are
    // text the a11y tree and a translator can both reach.
    const auto* drive = find_by_text(root, "DRIVE");
    const auto* tone = find_by_text(root, "TONE");
    REQUIRE(drive != nullptr);
    REQUIRE(tone != nullptr);

    // (3) The frames are there too, not only the leaves: the panel body and the
    // two knob faces Chrome painted as their own layout objects.
    const int panel = index_where(root, [](const IRNode& node) {
        return node.name == "div.panel";
    });
    CHECK(panel >= 0);
    const auto faces = static_cast<int>(std::count_if(
        root.children.begin(), root.children.end(),
        [](const IRNode& node) { return node.name == "div.face"; }));
    CHECK(faces == 2);
}

TEST_CASE("native lowering places nodes at Chrome's solved boxes verbatim",
          "[browser-capture][native-lowering]") {
    const auto native = lower_fixture(true);
    REQUIRE(native.design_ir);
    const auto& root = native.design_ir->root;

    // The panel is cropped to its primary surface at (40, 40), so page
    // coordinates shift by -40 and the panel body lands at the origin.
    const int panel = index_where(root, [](const IRNode& node) {
        return node.name == "div.panel";
    });
    REQUIRE(panel >= 0);
    const auto& body = root.children[static_cast<size_t>(panel)];
    REQUIRE(body.style.left);
    REQUIRE(body.style.top);
    CHECK(*body.style.left == 0.0f);
    CHECK(*body.style.top == 0.0f);
    CHECK(*body.style.width == 576.0f);
    CHECK(*body.style.height == 179.0f);

    // Blink lays out on a 1/64px fixed-point grid, so a real design produces
    // fractional boxes. They are DATA: consumed exactly, never rounded to look
    // tidy — 93.921875 is 6011/64 and is what Chrome painted.
    const auto* drive = find_by_text(root, "DRIVE");
    REQUIRE(drive != nullptr);
    REQUIRE(drive->style.left);
    CHECK_THAT(*drive->style.left,
               Catch::Matchers::WithinAbs(93.921875 - 40.0, 1e-6));
    CHECK_THAT(*drive->style.top, Catch::Matchers::WithinAbs(176.0 - 40.0, 1e-6));

    // Absolute placement is the whole design of this lane: Yoga must have
    // nothing left to solve.
    for (const auto& child : root.children) {
        REQUIRE(child.style.position);
        CHECK(*child.style.position == "absolute");
        CHECK(child.style.width);
        CHECK(child.style.height);
    }
}

TEST_CASE("native lowering emits children in Chrome's paint order",
          "[browser-capture][native-lowering]") {
    const auto native = lower_fixture(true);
    REQUIRE(native.design_ir);
    const auto& root = native.design_ir->root;

    // The knob faces establish their own stacking contexts and Chrome ranks
    // them after the in-flow captions. Document order alone would emit them
    // first and a later overlap would paint the wrong node on top — which is
    // why paint order is CONSUMED from Chromium rather than re-derived here.
    const int first_face = index_where(root, [](const IRNode& node) {
        return node.name == "div.face";
    });
    const int drive = index_where(root, [](const IRNode& node) {
        return node.text_content == "DRIVE";
    });
    REQUIRE(first_face >= 0);
    REQUIRE(drive >= 0);
    CHECK(drive < first_face);

    // Emitted order is non-decreasing in paint order; ties keep document order.
    int previous = -2;
    for (const auto& child : root.children) {
        const auto order = attribute(child, "paint_order");
        if (order.empty()) continue;  // a control overlay, appended after
        const int value = std::stoi(order);
        CHECK(value >= previous);
        previous = value;
    }

    // Controls sit on top of the design they belong to, so they come last.
    const int last_painted = index_where(root, [](const IRNode& node) {
        return node.audio_widget != pulp::view::AudioWidgetType::none;
    });
    REQUIRE(last_painted >= 0);
    for (size_t i = static_cast<size_t>(last_painted);
         i < root.children.size(); ++i) {
        CHECK(root.children[i].audio_widget !=
              pulp::view::AudioWidgetType::none);
    }
}

TEST_CASE("a text node does not repaint its element's box",
          "[browser-capture][native-lowering]") {
    const auto native = lower_fixture(true);
    REQUIRE(native.design_ir);
    const auto* drive = find_by_text(native.design_ir->root, "DRIVE");
    REQUIRE(drive != nullptr);

    // A text run's computed style IS its parent element's. Folding the box half
    // onto it would paint a second copy of the parent's background, border and
    // shadow inside the caption's own rectangle — visible as a dark slab behind
    // every label, and invisible to any test that only counts nodes.
    CHECK_FALSE(drive->style.background_color.has_value());
    CHECK_FALSE(drive->style.background_gradient.has_value());
    CHECK(drive->style.box_shadow.empty());
    CHECK_FALSE(drive->style.border_width.has_value());
    // It keeps the typography it legitimately inherits.
    CHECK(drive->style.color.has_value());
    CHECK(drive->style.font_family.has_value());
    CHECK(drive->style.font_size.has_value());
}

TEST_CASE("native lowering reports a per-class census",
          "[browser-capture][native-lowering]") {
    const auto native = lower_fixture(true);
    REQUIRE(native.design_ir);
    const auto& root = native.design_ir->root;

    const auto number = [&root](const char* key) {
        const auto value = attribute(root, key);
        REQUIRE_FALSE(value.empty());
        return std::stoi(value);
    };
    const int painted = number("native_painted_nodes");
    const int lowered = number("native_nodes_lowered");
    const int native_nodes = number("native_nodes_native");
    const int image_asset = number("native_nodes_image_asset");
    const int fallback = number("native_nodes_element_capture_fallback");

    CHECK(painted == 12);              // every layout object Chrome reported
    CHECK(lowered == 11);              // less the document node itself
    CHECK(native_nodes + image_asset + fallback == lowered);
    CHECK(number("native_nodes_text") == 2);
    // This fixture is pure CSS, so it is entirely drawable — the claim the
    // whole plan rests on, asserted rather than assumed.
    CHECK(native_nodes == 11);
    CHECK(number("native_nodes_missing_paint_order") == 0);

    // The census counts the nodes actually emitted, not a hopeful number.
    const auto emitted = static_cast<int>(std::count_if(
        root.children.begin(), root.children.end(),
        [](const IRNode& node) {
            return node.attributes.count("paint_class") != 0;
        }));
    CHECK(emitted == lowered);
}

TEST_CASE("the faithful capture stays the A-side of the A/B",
          "[browser-capture][native-lowering]") {
    // Default options must be byte-for-byte the pipeline that shipped: a
    // capture backdrop with control overlays and nothing else. Native lowering
    // is opt-in precisely so the picture remains the permanent CI oracle.
    const auto capture = lower_fixture(false);
    REQUIRE(capture.design_ir);
    const auto& root = capture.design_ir->root;

    CHECK(any_capture_node(root));
    CHECK(attribute(root, "asset_ref") == "reference:browser");
    CHECK(attribute(root, "native_nodes_lowered").empty());
    for (const auto& child : root.children) {
        const bool is_capture =
            child.render_mode == NodeRenderMode::faithful_capture;
        const bool is_control =
            child.audio_widget != pulp::view::AudioWidgetType::none;
        CHECK((is_capture || is_control));
    }
}

TEST_CASE("a control over a native panel restates its body contract",
          "[browser-capture][native-lowering]") {
    // The value-geometry-vs-body split used to be enforced by the bitmap's
    // EXISTENCE. Remove the bitmap without re-stating it and every control
    // paints an opaque default body over the design it was placed on.
    const auto native = lower_fixture(true);
    REQUIRE(native.design_ir);
    int controls = 0;
    for (const auto& child : native.design_ir->root.children) {
        if (child.audio_widget == pulp::view::AudioWidgetType::none) continue;
        ++controls;
        CHECK(attribute(child, "designed_body") == "underlay");
    }
    CHECK(controls == 2);

    const auto capture = lower_fixture(false);
    REQUIRE(capture.design_ir);
    for (const auto& child : capture.design_ir->root.children) {
        if (child.audio_widget == pulp::view::AudioWidgetType::none) continue;
        CHECK(attribute(child, "designed_body") == "capture");
    }
}

// ── Classifier cases the committed Chrome fixture cannot reach ──────────────
//
// The fixture is pure CSS, so `<canvas>` / `<svg>` / `<img>` classification is
// exercised against hand-built snapshots. That split is deliberate: the cases
// above pin snapshot DECODING against what Chrome really serializes, and these
// pin the tree logic layered on top of the same loader.

namespace {

fs::path write_snapshot(const std::string& body, const std::string& name) {
    const auto path =
        fs::temp_directory_path() / ("pulp-native-lowering-" + name + ".json");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
    out.close();
    return path;
}

/// Build a DOMSnapshot carrying exactly the arrays whole-tree lowering reads.
/// The shared string table is, in index order:
///   0 #document  1 HTML  2 BODY  3 DIV  4 svg  5 path  6 CANVAS  7 IMG
///   8 src  9 logo.png  10 background-image  11 none  12 url("texture.png")
///   13 display  14 block
/// so a style row of [11,14] is `background-image: none; display: block`.
std::string snapshot_with(const std::string& node_names,
                          const std::string& node_types,
                          const std::string& parents,
                          const std::string& attributes,
                          const std::string& layout_nodes,
                          const std::string& styles,
                          const std::string& bounds,
                          const std::string& paint_orders) {
    std::string json;
    json += R"({"strings":["#document","HTML","BODY","DIV","svg","path",)";
    json += R"("CANVAS","IMG","src","logo.png","background-image","none",)";
    json += R"J("url(\"texture.png\")","display","block"],)J";
    json += R"("computedStyleNames":["background-image","display"],)";
    json += R"("documents":[{"nodes":{"parentIndex":)" + parents +
            R"(,"nodeType":)" + node_types +
            R"(,"nodeName":)" + node_names +
            R"(,"backendNodeId":)" + node_names +
            R"(,"attributes":)" + attributes + "}," +
            R"("layout":{"nodeIndex":)" + layout_nodes +
            R"(,"styles":)" + styles +
            R"(,"bounds":)" + bounds +
            R"(,"paintOrders":)" + paint_orders +
            R"(,"text":[)";
    // Every layout node in these snapshots is an element, so no text runs.
    std::string texts = "-1";
    const auto commas =
        static_cast<size_t>(std::count(layout_nodes.begin(),
                                       layout_nodes.end(), ','));
    for (size_t i = 0; i < commas; ++i) texts += ",-1";
    json += texts + "]}}]}";
    return json;
}

}  // namespace

TEST_CASE("an svg subtree pools into one capture-fallback node",
          "[browser-capture][native-lowering]") {
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 svg, 4 path, 5 path
    const auto json = snapshot_with(
        /*names*/     "[0,1,2,4,5,5]",
        /*types*/     "[9,1,1,1,1,1]",
        /*parents*/   "[-1,0,1,2,3,3]",
        /*attributes*/"[[],[],[],[],[],[]]",
        /*layout*/    "[0,1,2,3,4,5]",
        /*styles*/    "[[11,14],[11,14],[11,14],[11,14],[11,14],[11,14]]",
        /*bounds*/    "[[0,0,100,100],[0,0,100,100],[0,0,100,100],"
                      "[10,10,50,50],[12,12,10,10],[30,30,10,10]]",
        /*paint*/     "[0,1,1,2,3,4]");
    const auto path = write_snapshot(json, "svg");
    const auto index = CapturedStyleIndex::load(path);
    REQUIRE(index);

    IRNode root;
    const auto counts = lower_painted_tree(*index, 0.0, 0.0, root);
    CHECK(counts.painted == 6);
    CHECK(counts.skipped_non_visual == 1);      // the document node
    CHECK(counts.element_capture_fallback == 1);
    // Both <path> children are covered by the captured <svg> above them.
    // Emitting them too would draw the same artwork twice, once wrongly.
    CHECK(counts.pooled_into_fallback == 2);
    CHECK(counts.lowered == 3);                 // html, body, svg
    CHECK(counts.native == 2);

    const int svg = index_where(root, [](const IRNode& node) {
        return attribute(node, "paint_class") == "element-capture-fallback";
    });
    REQUIRE(svg >= 0);
    CHECK(attribute(root.children[static_cast<size_t>(svg)],
                    "capture_fallback_element") == "svg");
    fs::remove(path);
}

TEST_CASE("canvas and image elements classify away from native",
          "[browser-capture][native-lowering]") {
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 CANVAS, 4 IMG, 5 DIV(url bg)
    const auto json = snapshot_with(
        /*names*/     "[0,1,2,6,7,3]",
        /*types*/     "[9,1,1,1,1,1]",
        /*parents*/   "[-1,0,1,2,2,2]",
        /*attributes*/"[[],[],[],[],[8,9],[]]",
        /*layout*/    "[0,1,2,3,4,5]",
        /*styles*/    "[[11,14],[11,14],[11,14],[11,14],[11,14],[12,14]]",
        /*bounds*/    "[[0,0,200,200],[0,0,200,200],[0,0,200,200],"
                      "[0,0,116,116],[10,10,20,20],[40,40,60,60]]",
        /*paint*/     "[0,1,1,2,2,2]");
    const auto path = write_snapshot(json, "assets");
    const auto index = CapturedStyleIndex::load(path);
    REQUIRE(index);

    IRNode root;
    const auto counts = lower_painted_tree(*index, 0.0, 0.0, root);
    CHECK(counts.element_capture_fallback == 1);  // <canvas>: no styles to draw
    CHECK(counts.image_asset == 2);               // <img> and the url() fill
    CHECK(counts.native == 2);                    // html, body
    CHECK(counts.pooled_into_fallback == 0);      // the canvas has no children

    const int image = index_where(root, [](const IRNode& node) {
        return node.type == "image";
    });
    REQUIRE(image >= 0);
    CHECK(attribute(root.children[static_cast<size_t>(image)], "src") ==
          "logo.png");
    fs::remove(path);
}

TEST_CASE("a zero-area layout object is not lowered",
          "[browser-capture][native-lowering]") {
    const auto json = snapshot_with(
        /*names*/     "[0,1,2,3]",
        /*types*/     "[9,1,1,1]",
        /*parents*/   "[-1,0,1,2]",
        /*attributes*/"[[],[],[],[]]",
        /*layout*/    "[0,1,2,3]",
        /*styles*/    "[[11,14],[11,14],[11,14],[11,14]]",
        /*bounds*/    "[[0,0,50,50],[0,0,50,50],[0,0,50,50],[10,10,0,0]]",
        /*paint*/     "[0,1,1,2]");
    const auto path = write_snapshot(json, "empty");
    const auto index = CapturedStyleIndex::load(path);
    REQUIRE(index);

    IRNode root;
    const auto counts = lower_painted_tree(*index, 0.0, 0.0, root);
    CHECK(counts.skipped_empty_box == 1);
    CHECK(counts.lowered == 2);
    fs::remove(path);
}

TEST_CASE("native lowering refuses a capture with no DOM snapshot",
          "[browser-capture][native-lowering]") {
    // Falling back to the photograph here would report a native panel that is
    // a picture — the exact failure this work exists to end.
    const auto directory =
        fs::temp_directory_path() / "pulp-native-lowering-no-snapshot";
    fs::remove_all(directory);
    fs::create_directories(directory);
    for (const auto& name : {"browser.png", "semantic-report.json",
                             "tokens.json", "capture.json"}) {
        fs::copy_file(fs::path(PULP_BROWSER_CAPTURE_STYLE_FIXTURE_DIR) / name,
                      directory / name,
                      fs::copy_options::overwrite_existing);
    }

    BrowserCaptureIrOptions options;
    options.native_panel_lowering = true;
    const auto result =
        lower_browser_capture_to_ir(directory / "capture.json", options);
    CHECK_FALSE(result);
    CHECK(result.error.find("DOM snapshot") != std::string::npos);

    // The same capture still lowers on the A-side, so this is a native-mode
    // refusal rather than a broken envelope.
    const auto capture = lower_browser_capture_to_ir(
        directory / "capture.json", BrowserCaptureIrOptions{});
    CHECK(capture);
    fs::remove_all(directory);
}

// Census over a capture directory named by the environment, so the numbers in a
// report come from running the real path over a real design rather than from a
// fixture sized to be readable. Hidden by default (`[.]`) because it needs a
// capture that is not committed; selecting it without one FAILS rather than
// passing quietly, so it can never become a test that proves nothing.
// Tagged ONLY `[.real-design]`: it must not be swept up by a `[native-lowering]`
// or `[browser-capture]` filter, which would fail a clean run for want of a
// capture nobody asked it to supply.
//   PULP_NATIVE_LOWERING_CAPTURE=<capture-dir> \
//     pulp-test-browser-capture-import "[.real-design]" -s
TEST_CASE("whole-tree lowering census over a real captured design",
          "[.real-design]") {
    const char* directory = std::getenv("PULP_NATIVE_LOWERING_CAPTURE");
    REQUIRE(directory != nullptr);
    const auto envelope = fs::path(directory) / "capture.json";
    REQUIRE(fs::is_regular_file(envelope));

    BrowserCaptureIrOptions capture_options;
    const auto capture =
        lower_browser_capture_to_ir(envelope, capture_options);
    INFO("A-side error: " << capture.error);
    REQUIRE(capture.design_ir);

    BrowserCaptureIrOptions native_options;
    native_options.native_panel_lowering = true;
    const auto native = lower_browser_capture_to_ir(envelope, native_options);
    INFO("native error: " << native.error);
    REQUIRE(native.design_ir);

    const auto& before = capture.design_ir->root;
    const auto& after = native.design_ir->root;
    const auto report = [](const IRNode& root, const char* label) {
        WARN(label << " root children: " << root.children.size());
        for (const char* key : {"native_painted_nodes", "native_nodes_lowered",
                                "native_nodes_native",
                                "native_nodes_image_asset",
                                "native_nodes_element_capture_fallback",
                                "native_nodes_text", "native_nodes_pooled",
                                "native_nodes_missing_paint_order",
                                "native_nodes_skipped_empty_box",
                                "native_nodes_skipped_blank_text",
                                "native_nodes_skipped_non_visual",
                                "controls_lowered"}) {
            const auto it = root.attributes.find(key);
            if (it != root.attributes.end())
                WARN("  " << key << " = " << it->second);
        }
    };
    report(before, "capture");
    report(after, "native");

    CHECK_FALSE(any_capture_node(after));
    CHECK(after.children.size() > before.children.size());
}
