// SPDX-License-Identifier: MIT
//
// Whole-tree lowering: every node Chrome painted becomes an IR node drawn at
// Chrome's own solved box, inside a tree that mirrors the captured DOM, so a
// panel is drawn rather than photographed AND can still be edited a section at
// a time.
//
// These cases deliberately assert the SHAPE OF THE TREE — which nodes exist,
// what CONTAINS what, what strings they carry, where they sit, what order they
// paint in — and never a similarity score. A panel can score well while being a
// photograph; that is exactly the defect this work exists to remove, so a score
// cannot be the instrument that proves it gone. Containment is asserted the
// same way, and for the same reason: a flat tree holds every node a nested one
// does, so "the node is present" cannot prove the structure survived.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "tools/import-design/browser_capture_ir.hpp"
#include "tools/import-design/browser_capture_tree.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

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

/// Lower one of the real Chromium captures kept under `test/fixtures`, through
/// the same entry point a `pulp import-design` run uses.
BrowserCaptureIrResult lower_capture(std::string_view fixture) {
    BrowserCaptureIrOptions options;
    options.native_panel_lowering = true;
    return lower_browser_capture_to_ir(
        fs::path(PULP_BROWSER_CAPTURE_FIXTURE_ROOT) / fixture / "capture.json",
        options);
}

/// Depth-first over the whole subtree, not only the direct children: the point
/// of the amendment is that the interesting nodes are no longer siblings.
template <typename Predicate>
const IRNode* find_node(const IRNode& root, Predicate predicate) {
    for (const auto& child : root.children) {
        if (predicate(child)) return &child;
        if (const auto* found = find_node(child, predicate)) return found;
    }
    return nullptr;
}

template <typename Predicate>
int count_nodes(const IRNode& root, Predicate predicate) {
    int total = 0;
    for (const auto& child : root.children) {
        if (predicate(child)) ++total;
        total += count_nodes(child, predicate);
    }
    return total;
}

const IRNode* find_named(const IRNode& root, std::string_view name) {
    return find_node(root, [name](const IRNode& node) {
        return node.name == name;
    });
}

const IRNode* find_by_text(const IRNode& root, std::string_view text) {
    return find_node(root, [text](const IRNode& node) {
        return node.type == "text" && node.text_content == text;
    });
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

bool contains(const IRNode& ancestor, const IRNode* target) {
    if (target == nullptr) return false;
    for (const auto& child : ancestor.children) {
        if (&child == target || contains(child, target)) return true;
    }
    return false;
}

/// One node with the absolute position its offset chain composes to.
struct Placed {
    const IRNode* node = nullptr;
    double left = 0.0;
    double top = 0.0;
    int depth = 0;
};

/// Walk the tree summing parent-relative offsets. This is the arithmetic a
/// nested absolute layout performs, done by hand, so a test can compare it
/// against the box Chrome solved.
void compose(const IRNode& parent, double frame_left, double frame_top,
             int depth, std::vector<Placed>& out) {
    for (const auto& child : parent.children) {
        const double left = frame_left + child.style.left.value_or(0.0f);
        const double top = frame_top + child.style.top.value_or(0.0f);
        out.push_back({&child, left, top, depth});
        compose(child, left, top, depth + 1, out);
    }
}

std::vector<Placed> composed(const IRNode& root) {
    std::vector<Placed> out;
    compose(root, 0.0, 0.0, 1, out);
    return out;
}

const Placed* placed_for(const std::vector<Placed>& all, const IRNode* node) {
    for (const auto& entry : all)
        if (entry.node == node) return &entry;
    return nullptr;
}

/// Every lowered node's anchor, in composed order.
std::vector<std::string> anchors(const IRNode& root) {
    std::vector<std::string> out;
    for (const auto& entry : composed(root)) {
        if (entry.node->attributes.count("paint_class") == 0) continue;
        out.push_back(entry.node->stable_anchor_id.value_or("<none>"));
    }
    return out;
}


fs::path write_snapshot(const std::string& body, const std::string& name) {
    const auto path =
        fs::temp_directory_path() / ("pulp-native-lowering-" + name + ".json");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
    out.close();
    return path;
}

/// The parallel arrays of a DOMSnapshot that whole-tree lowering reads.
///
/// Named rather than positional: a snapshot IS the alignment of its arrays, and
/// a run of same-typed arguments makes a mis-slotted one describe a different
/// document than the case says it is about.
struct SnapshotSpec {
    std::string node_names;    ///< node index → string-table index
    std::string node_types;    ///< node index → DOM nodeType
    std::string parents;       ///< node index → parent index, -1 at the root
    std::string attributes;    ///< node index → flat name/value string indices
    std::string layout_nodes;  ///< layout index → node index
    std::string styles;        ///< layout index → one entry per computed name
    std::string bounds;        ///< layout index → [left, top, width, height]
    std::string paint_orders;  ///< layout index → Chrome's paint rank
    /// layout index → string-table index of the laid-out text, -1 for an
    /// element. Empty fills in `-1` for every layout node.
    std::string texts;
    /// The property list every style row is parallel to.
    std::string computed_names = R"(["background-image","display"])";
};

/// The shared string table, in index order:
///    0 #document  1 HTML   2 BODY   3 DIV   4 svg   5 path   6 CANVAS  7 IMG
///    8 src        9 logo.png       10 background-image      11 none
///   12 url("texture.png")          13 display               14 block
///   15 g         16 VIDEO 17 IFRAME 18 EMBED 19 OBJECT 20 math
///   21 image-set("texture.png" 1x)
///   22 -webkit-image-set("texture.png" 1x)
///   23 class     24 wrap  25 inner  26 leaf  27 item  28 #text
///   29 "   " (a collapsed whitespace run)    30 Level
///   31 rgb(9, 11, 16)
///   32 linear-gradient(180deg, rgb(40, 44, 52), rgb(9, 11, 16))
///   33 rgba(0, 0, 0, 0.6) 0px 8px 24px 0px   34 3px  35 rgb(240, 240, 240)
///   36 knob      37 DRIVE   38 TONE   39 MIX
///   40 "panel<TAB>active"   41 "<LF>  card  x"   42 " panel"
///   43 "w-1/2 p-4"          44 a      45 b      46 "a[0]/div.b"
///   47 matrix(0.707107, 0.707107, -0.707107, 0.707107, 0, 0)   — 45° rotation
///   48 matrix(2, 0, 0, 2, 0, 0)                                — 2× scale
///   49 SPAN
///   50 visible   51 hidden   52 static   53 relative   54 absolute
///   55 0px       56 1px      57 circle(50%)
///   58 rgba(0, 0, 0, 0.5) 0px 0px 30px 0px
/// so a style row of [11,14] is `background-image: none; display: block`.
constexpr std::string_view kSnapshotStrings =
    R"J("#document","HTML","BODY","DIV","svg","path","CANVAS","IMG",)J"
    R"J("src","logo.png","background-image","none","url(\"texture.png\")",)J"
    R"J("display","block","g","VIDEO","IFRAME","EMBED","OBJECT","math",)J"
    R"J("image-set(\"texture.png\" 1x)",)J"
    R"J("-webkit-image-set(\"texture.png\" 1x)",)J"
    R"J("class","wrap","inner","leaf","item","#text","   ","Level",)J"
    R"J("rgb(9, 11, 16)",)J"
    R"J("linear-gradient(180deg, rgb(40, 44, 52), rgb(9, 11, 16))",)J"
    R"J("rgba(0, 0, 0, 0.6) 0px 8px 24px 0px","3px","rgb(240, 240, 240)",)J"
    R"J("knob","DRIVE","TONE","MIX",)J"
    R"J("panel\tactive","\n  card  x"," panel",)J"
    R"J("w-1/2 p-4","a","b","a[0]/div.b",)J"
    R"J("matrix(0.707107, 0.707107, -0.707107, 0.707107, 0, 0)",)J"
    R"J("matrix(2, 0, 0, 2, 0, 0)","SPAN",)J"
    R"J("visible","hidden","static","relative","absolute",)J"
    R"J("0px","1px","circle(50%)","rgba(0, 0, 0, 0.5) 0px 0px 30px 0px")J";

/// The property list the clip cases are parallel to. `overflow` decides what
/// clips; `position` and `transform` decide what a clip applies TO; the border
/// widths turn a clipper's border box into the padding box it actually clips
/// to; `clip-path` is a clip whose region is a shape rather than a rectangle.
///
/// Row order: overflow, position, transform, clip-path, and the four border
/// widths clockwise from the top.
constexpr std::string_view kClipProperties =
    R"(["overflow","position","transform","clip-path",)"
    R"("border-top-width","border-right-width",)"
    R"("border-bottom-width","border-left-width"])";

/// A row for the clip property list: no clip of any kind, no border.
constexpr std::string_view kNoClipRow = "[50,52,11,11,55,55,55,55]";

/// Build a DOMSnapshot carrying exactly the arrays whole-tree lowering reads.
std::string build_snapshot(const SnapshotSpec& spec) {
    std::string texts = spec.texts;
    if (texts.empty()) {
        // No text runs in this snapshot, so one `-1` per layout node.
        const auto entries =
            static_cast<size_t>(std::count(spec.layout_nodes.begin(),
                                           spec.layout_nodes.end(), ',')) + 1;
        texts = "[-1";
        for (size_t i = 1; i < entries; ++i) texts += ",-1";
        texts += "]";
    }
    std::string json;
    json += R"({"strings":[)" + std::string(kSnapshotStrings) + "],";
    json += R"("computedStyleNames":)" + spec.computed_names + ",";
    json += R"("documents":[{"nodes":{"parentIndex":)" + spec.parents +
            R"(,"nodeType":)" + spec.node_types +
            R"(,"nodeName":)" + spec.node_names +
            R"(,"backendNodeId":)" + spec.node_names +
            R"(,"attributes":)" + spec.attributes + "}," +
            R"("layout":{"nodeIndex":)" + spec.layout_nodes +
            R"(,"styles":)" + spec.styles +
            R"(,"bounds":)" + spec.bounds +
            R"(,"paintOrders":)" + spec.paint_orders +
            R"(,"text":)" + texts + "}}]}";
    return json;
}

/// Load a hand-built snapshot, lower it, and clean the temp file up.
struct LoweredSnapshot {
    PaintedTreeCounts counts;
    IRNode root;
};

/// Lower into a root the caller already shaped — the panel frame a real caller
/// passes in, whose own size and `overflow` are part of what the tree lands in.
PaintedTreeCounts lower_into(const SnapshotSpec& spec, const std::string& name,
                             IRNode& root) {
    const auto path = write_snapshot(build_snapshot(spec), name);
    const auto index = CapturedStyleIndex::load(path);
    // The index holds no reference to the file, so it goes now rather than at
    // the end — a failing assertion below throws, and a cleanup after it would
    // leave the snapshot behind exactly on the runs someone wants to inspect.
    fs::remove(path);
    REQUIRE(index);
    return lower_painted_tree(*index, 0.0, 0.0, root);
}

LoweredSnapshot lower_snapshot(const SnapshotSpec& spec,
                               const std::string& name) {
    LoweredSnapshot out;
    out.counts = lower_into(spec, name, out.root);
    return out;
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
    CHECK(find_named(root, "div.panel") != nullptr);
    CHECK(count_nodes(root, [](const IRNode& node) {
              return node.name == "div.face";
          }) == 2);
}

TEST_CASE("native lowering keeps the design's hierarchy",
          "[browser-capture][native-lowering]") {
    // The reason the tree matters: an agent asked to "tweak the drive knob" has
    // to be able to name it and get its parts with it. Two `div.face` nodes
    // sitting as siblings under the root are indistinguishable, and there is no
    // knob to grab. Presence is therefore not the assertion — CONTAINMENT is,
    // because a flat lowering passes every presence check this file could make.
    const auto native = lower_fixture(true);
    REQUIRE(native.design_ir);
    const auto& root = native.design_ir->root;

    const auto* panel = find_named(root, "div.panel");
    REQUIRE(panel != nullptr);

    // The panel body is a group: both knobs are inside it.
    const auto knobs = count_nodes(*panel, [](const IRNode& node) {
        return node.name == "div.knob";
    });
    CHECK(knobs == 2);

    // Each knob owns its own face and its own caption. This is the assertion a
    // flat tree cannot satisfy: the two faces are identical by name, so only
    // their PARENT tells them apart.
    REQUIRE(panel->children.size() == 2);
    for (const auto& knob : panel->children) {
        CHECK(knob.name == "div.knob");
        CHECK(count_nodes(knob, [](const IRNode& node) {
                  return node.name == "div.face";
              }) == 1);
        CHECK(count_nodes(knob, [](const IRNode& node) {
                  return node.name == "div.cap";
              }) == 1);
        CHECK(count_nodes(knob, [](const IRNode& node) {
                  return node.type == "text";
              }) == 1);
    }

    // The two captions are under DIFFERENT knobs — the whole point.
    const auto* drive = find_by_text(root, "DRIVE");
    const auto* tone = find_by_text(root, "TONE");
    REQUIRE(drive != nullptr);
    REQUIRE(tone != nullptr);
    const auto& first = panel->children[0];
    const auto& second = panel->children[1];
    CHECK(contains(first, drive));
    CHECK_FALSE(contains(first, tone));
    CHECK(contains(second, tone));
    CHECK_FALSE(contains(second, drive));

    // The document's own frame survives too, so the panel is reachable by the
    // path an author would write rather than found by scanning a flat list.
    const auto* body = find_named(root, "body");
    REQUIRE(body != nullptr);
    CHECK(contains(*body, panel));
}

TEST_CASE("native lowering places nodes at Chrome's solved boxes verbatim",
          "[browser-capture][native-lowering]") {
    const auto native = lower_fixture(true);
    REQUIRE(native.design_ir);
    const auto& root = native.design_ir->root;
    const auto all = composed(root);

    // The panel is cropped to its primary surface at (40, 40), so page
    // coordinates shift by -40 and the panel body lands at the origin. It is
    // now nested three deep, so that origin is what its OFFSET CHAIN composes
    // to — which is the property that has to hold, not the value of any one
    // node's `left`.
    const auto* panel = find_named(root, "div.panel");
    REQUIRE(panel != nullptr);
    const auto* body = placed_for(all, panel);
    REQUIRE(body != nullptr);
    CHECK(body->left == 0.0);
    CHECK(body->top == 0.0);
    CHECK(*panel->style.width == 576.0f);
    CHECK(*panel->style.height == 179.0f);

    // Blink lays out on a 1/64px fixed-point grid, so a real design produces
    // fractional boxes. They are DATA: consumed exactly, never rounded to look
    // tidy — 93.921875 is 6011/64 and is what Chrome painted. Composing a chain
    // of such offsets is exact in binary32 (every value is a multiple of 1/64
    // well inside the mantissa), so this is asserted with no tolerance at all:
    // a rounded intermediate would show up here rather than hide in an epsilon.
    const auto* drive = find_by_text(root, "DRIVE");
    REQUIRE(drive != nullptr);
    const auto* drive_box = placed_for(all, drive);
    REQUIRE(drive_box != nullptr);
    CHECK(drive_box->left == 93.921875 - 40.0);
    CHECK(drive_box->top == 176.0 - 40.0);

    // A text run is sized by the box Chrome laid the RUN out in. Asserting only
    // that a size was written accepts 0×0, which draws nothing while satisfying
    // every count and placement check in this file.
    CHECK(*drive->style.width == 46.140625f);
    CHECK(*drive->style.height == 15.0f);

    // Absolute placement is the whole design of this lane: Yoga must have
    // nothing left to solve, at every depth and not merely at the top.
    for (const auto& entry : all) {
        if (entry.node->attributes.count("paint_class") == 0) continue;
        INFO("node " << entry.node->name);
        REQUIRE(entry.node->style.position);
        CHECK(*entry.node->style.position == "absolute");
        REQUIRE(entry.node->style.width);
        REQUIRE(entry.node->style.height);
        CHECK(*entry.node->style.width > 0.0f);
        CHECK(*entry.node->style.height > 0.0f);
    }
}

// A layout engine places an absolutely positioned child against its parent's
// PADDING box, so the parent's border width is added to every child offset.
// Chrome's boxes already include it once — they are measured from the page
// origin — so an offset taken from the parent's BORDER box gets it counted
// twice, and every descendant of a bordered node lands one border-width down
// and to the right of where the browser put it. On a real panel that is a
// card's fill sliding out from under its own frame.
TEST_CASE("a bordered parent does not shift its children",
          "[browser-capture][native-lowering]") {
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 DIV(3px border), 4 DIV(child).
    // The child's page box is (30,30); the parent's is (20,20), so the offset
    // between them is 10. A layout engine then adds the parent's 3px border
    // when it places the child against the padding box, so the value emitted
    // has to be 7 for the sum to land back on Chrome's 30.
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,3,3]",
            .node_types = "[9,1,1,1,1]",
            .parents = "[-1,0,1,2,3]",
            .attributes = "[[],[],[],[],[]]",
            .layout_nodes = "[0,1,2,3,4]",
            // Index 34 is "3px"; 11 is "none", which records no width at all.
            .styles = "[[11,14,11,11,11,11],[11,14,11,11,11,11],"
                      "[11,14,11,11,11,11],[11,14,34,34,34,34],"
                      "[11,14,11,11,11,11]]",
            .bounds = "[[0,0,200,200],[0,0,200,200],[0,0,200,200],"
                      "[20,20,100,100],[30,30,40,40]]",
            .paint_orders = "[0,1,1,2,3]",
            .computed_names =
                R"(["background-image","display","border-top-width",)"
                R"("border-right-width","border-bottom-width",)"
                R"("border-left-width"])",
        },
        "bordered-parent-offset");
    const auto* parent = find_node(lowered.root, [](const IRNode& n) {
        return n.style.border_top_width.value_or(0.0f) > 0.0f;
    });
    REQUIRE(parent != nullptr);
    REQUIRE(parent->children.size() == 1);
    const IRNode& child = parent->children.front();
    CHECK_THAT(child.style.left.value_or(-1.0f),
               Catch::Matchers::WithinAbs(7.0, 1e-4));
    CHECK_THAT(child.style.top.value_or(-1.0f),
               Catch::Matchers::WithinAbs(7.0, 1e-4));
    // And the parent itself is untouched — the compensation belongs to the
    // child's offset, not to the box the border is drawn on.
    CHECK_THAT(parent->style.width.value_or(0.0f),
               Catch::Matchers::WithinAbs(100.0, 1e-4));
}

TEST_CASE("parent-relative offsets compose to Chrome's absolute boxes",
          "[browser-capture][native-lowering]") {
    // Storing a child's box relative to its parent is what makes "move a
    // section and its children follow" true by construction. It is only worth
    // anything if the sum still lands exactly where Chrome put the node, so
    // every lowered node is checked against the snapshot's own bounds.
    const auto native = lower_fixture(true);
    REQUIRE(native.design_ir);
    const auto all = composed(native.design_ir->root);

    // Page coordinates from the committed Chrome capture, shifted by the crop.
    const struct { const char* name; double left; double top; } expected[]{
        {"html", 0.0, 0.0},
        {"body", 0.0, 40.0},
        {"div.panel", 40.0, 40.0},
        {"div.face", 68.0, 68.0},
        {"div.cap", 93.921875, 176.0},
    };
    for (const auto& want : expected) {
        const auto* node = find_named(native.design_ir->root, want.name);
        INFO("node " << want.name);
        REQUIRE(node != nullptr);
        const auto* box = placed_for(all, node);
        REQUIRE(box != nullptr);
        CHECK(box->left == want.left - 40.0);
        CHECK(box->top == want.top - 40.0);
    }

    // A nested node's own `left` is genuinely relative — if it were still the
    // absolute value the composition above would be wrong by the parent's
    // origin, and this is what tells the two apart.
    const auto* cap = find_named(native.design_ir->root, "div.cap");
    REQUIRE(cap != nullptr);
    CHECK(*cap->style.left == static_cast<float>(93.921875 - 68.0));
    CHECK(*cap->style.top == 176.0f - 68.0f);
}

TEST_CASE("moving a container moves everything inside it",
          "[browser-capture][native-lowering]") {
    // The owner's actual use case: grab a section, nudge it, and the section
    // moves as a unit. Asserted on the geometry rather than on the promise.
    auto native = lower_fixture(true);
    REQUIRE(native.design_ir);
    auto& root = native.design_ir->root;

    const auto* found = find_named(root, "div.panel");
    REQUIRE(found != nullptr);
    const auto before = composed(root);

    // A section is only a section if it HAS contents; a flat tree would pass a
    // "shifted by the same delta" check vacuously, over an empty set.
    const int inside = count_nodes(*found, [](const IRNode&) { return true; });
    REQUIRE(inside >= 4);

    // Translate by a deliberately awkward delta: whole pixels would survive a
    // rounding bug that fractions catch.
    const float dx = 17.5f;
    const float dy = -23.25f;
    auto* panel = const_cast<IRNode*>(found);
    panel->style.left = *panel->style.left + dx;
    panel->style.top = *panel->style.top + dy;

    const auto after = composed(root);
    REQUIRE(before.size() == after.size());
    int moved = 0;
    for (size_t i = 0; i < before.size(); ++i) {
        REQUIRE(before[i].node == after[i].node);
        const bool in_panel =
            before[i].node == found || contains(*found, before[i].node);
        const double expected_dx = in_panel ? dx : 0.0;
        const double expected_dy = in_panel ? dy : 0.0;
        INFO("node " << before[i].node->name);
        CHECK(after[i].left - before[i].left == expected_dx);
        CHECK(after[i].top - before[i].top == expected_dy);
        if (in_panel) ++moved;
    }
    CHECK(moved == inside + 1);  // the panel and everything under it
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
    // In a tree that order lives among SIBLINGS: the face is a later child of
    // its own knob than the caption is.
    const auto* panel = find_named(root, "div.panel");
    REQUIRE(panel != nullptr);
    REQUIRE_FALSE(panel->children.empty());
    const auto& knob = panel->children[0];
    const int cap = index_where(knob, [](const IRNode& node) {
        return node.name == "div.cap";
    });
    const int face = index_where(knob, [](const IRNode& node) {
        return node.name == "div.face";
    });
    REQUIRE(cap >= 0);
    REQUIRE(face >= 0);
    CHECK(cap < face);

    // Paint order is non-decreasing THROUGH the tree: a parent paints before
    // everything it contains, and siblings paint left to right. That is the
    // hierarchical form of the flat invariant, and it is what a nested painter
    // actually executes.
    const std::function<void(const IRNode&, int)> check_subtree =
        [&](const IRNode& node, int floor) {
            int previous = floor;
            for (const auto& child : node.children) {
                const auto order = attribute(child, "paint_order");
                if (order.empty()) continue;  // a control overlay
                const int value = std::stoi(order);
                INFO("node " << child.name << " paint_order " << value
                             << " under floor " << previous);
                CHECK(value >= previous);
                previous = value;
                check_subtree(child, value);
            }
        };
    check_subtree(root, -2);

    // Within a context that order is carried as z-index, so it survives any
    // consumer that sorts siblings rather than trusting the vector.
    const std::function<void(const IRNode&)> check_z = [&](const IRNode& node) {
        int expected = 0;
        for (const auto& child : node.children) {
            if (child.attributes.count("paint_class") == 0) continue;
            REQUIRE(child.style.z_index);
            CHECK(*child.style.z_index == expected++);
            check_z(child);
        }
    };
    check_z(root);

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

TEST_CASE("every lowered node carries a stable anchor",
          "[browser-capture][native-lowering]") {
    // An agent's edit is stored against an anchor and replayed after the design
    // is re-captured. An anchor keyed on a position in the capture's own
    // serialization identifies the node within one capture, which is the one
    // job it does not have.
    const auto native = lower_fixture(true);
    REQUIRE(native.design_ir);
    const auto& root = native.design_ir->root;

    int lowered = 0;
    std::set<std::string> unique;
    for (const auto& entry : composed(root)) {
        if (entry.node->attributes.count("paint_class") == 0) continue;
        ++lowered;
        INFO("node " << entry.node->name);
        REQUIRE(entry.node->stable_anchor_id);
        CHECK_FALSE(entry.node->stable_anchor_id->empty());
        // NOT `path`. `AnchorStrategy::path` is an existing, different scheme
        // — `Type[idx]` over the IR's own node types, no prefix — so a
        // consumer that re-derived one against these anchors would compute
        // `frame[0]/frame[0]` and match nothing. The name is asserted, and
        // asserted to be distinct, because nothing re-derives it today and a
        // silent collision would surface only once something did.
        CHECK(entry.node->anchor_strategy.value_or("") == "capture-path");
        CHECK(entry.node->anchor_strategy.value_or("") != "path");
        // Distinct nodes must not collide, or the tweaks layer applies a human
        // edit to whichever node it happened to find first.
        unique.insert(*entry.node->stable_anchor_id);
    }
    CHECK(lowered > 0);
    CHECK(static_cast<int>(unique.size()) == lowered);

    // It is a readable DOM path, so a human reviewing a tweaks file can tell
    // what was edited without resolving it against a capture.
    const auto* drive = find_by_text(root, "DRIVE");
    REQUIRE(drive != nullptr);
    CHECK(*drive->stable_anchor_id ==
          "capture:html[0]/body[0]/div.panel[0]/div.knob[0]/div.cap[0]/"
          "#text[0]");
    const auto* panel = find_named(root, "div.panel");
    REQUIRE(panel != nullptr);
    CHECK(contains(*panel, drive));
    CHECK(drive->stable_anchor_id->find(*panel->stable_anchor_id) == 0);

    // Same design, captured again: the same anchors.
    const auto again = lower_fixture(true);
    REQUIRE(again.design_ir);
    CHECK(anchors(again.design_ir->root) == anchors(root));
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

    // The fixture's caption element carries no fill of its own, so the checks
    // above hold even if the box half WERE folded on. A label over a decorated
    // slab is the case the concern is about, and the parent's decorations are
    // asserted present first — otherwise this is four more absences over an
    // element that never had anything to copy.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 DIV slab, 4 #text "Level".
    // The text run's style row IS the slab's, which is what Chrome serializes.
    const std::string plain = "[11,11,11,11,11,11,11,11,14]";
    const std::string slab_row = "[31,32,33,34,34,34,34,35,14]";
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,3,28]",
            .node_types = "[9,1,1,1,3]",
            .parents = "[-1,0,1,2,3]",
            .attributes = "[[],[],[],[],[]]",
            .layout_nodes = "[0,1,2,3,4]",
            .styles = "[" + plain + "," + plain + "," + plain + "," +
                      slab_row + "," + slab_row + "]",
            .bounds = "[[0,0,200,200],[0,0,200,200],[0,0,200,200],"
                      "[10,10,120,40],[14,14,80,20]]",
            .paint_orders = "[0,1,1,2,3]",
            .texts = "[-1,-1,-1,-1,30]",
            .computed_names =
                R"(["background-color","background-image","box-shadow",)"
                R"("border-top-width","border-right-width",)"
                R"("border-bottom-width","border-left-width","color",)"
                R"("display"])",
        },
        "decorated-label");

    const auto* slab = find_named(lowered.root, "div");
    REQUIRE(slab != nullptr);
    CHECK(slab->style.background_color.has_value());
    CHECK(slab->style.background_gradient.has_value());
    CHECK_FALSE(slab->style.box_shadow.empty());
    REQUIRE(slab->style.border_width.has_value());
    CHECK(*slab->style.border_width == 3.0f);

    const auto* label = find_by_text(lowered.root, "Level");
    REQUIRE(label != nullptr);
    CHECK_FALSE(label->style.background_color.has_value());
    CHECK_FALSE(label->style.background_gradient.has_value());
    CHECK(label->style.box_shadow.empty());
    CHECK_FALSE(label->style.border_width.has_value());
    CHECK(label->style.color.has_value());
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
    const auto emitted = count_nodes(root, [](const IRNode& node) {
        return node.attributes.count("paint_class") != 0;
    });
    CHECK(emitted == lowered);

    // The SHAPE is a number too. A depth of 1 is the flattened lowering that
    // an agent cannot edit a section of, and it would otherwise pass every
    // count above — so the census reports it and a reviewer sees it.
    CHECK(number("native_tree_root_children") == 1);   // <html>
    CHECK(number("native_tree_depth") == 6);           // …/div.cap/#text
    // Nothing in this design paints before its own parent, and nesting it
    // reordered no overlapping pair against Chrome, so neither diagnostic is
    // recorded at all.
    CHECK(attribute(root, "native_nodes_hoisted").empty());
    CHECK(attribute(root, "native_nodes_overlapping_reorders").empty());
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

// ── Cases the committed Chrome fixture cannot reach ─────────────────────────
//
// The fixture is pure CSS with nothing hidden, nothing unranked and nothing
// collapsed, so classification, pooling, elision and the diagnostics are
// exercised against hand-built snapshots. That split is deliberate: the cases
// above pin snapshot DECODING against what Chrome really serializes, and these
// pin the tree logic layered on top of the same loader.

TEST_CASE("an svg subtree pools into one capture-fallback node",
          "[browser-capture][native-lowering]") {
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 svg, 4 path, 5 path
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,4,5,5]",
            .node_types = "[9,1,1,1,1,1]",
            .parents = "[-1,0,1,2,3,3]",
            .attributes = "[[],[],[],[],[],[]]",
            .layout_nodes = "[0,1,2,3,4,5]",
            .styles = "[[11,14],[11,14],[11,14],[11,14],[11,14],[11,14]]",
            .bounds = "[[0,0,100,100],[0,0,100,100],[0,0,100,100],"
                      "[10,10,50,50],[12,12,10,10],[30,30,10,10]]",
            .paint_orders = "[0,1,1,2,3,4]",
        },
        "svg");
    const auto& counts = lowered.counts;
    const auto& root = lowered.root;
    CHECK(counts.painted == 6);
    CHECK(counts.skipped_non_visual == 1);      // the document node
    CHECK(counts.element_capture_fallback == 1);
    // Both <path> children are covered by the captured <svg> above them.
    // Emitting them too would draw the same artwork twice, once wrongly.
    CHECK(counts.pooled_into_fallback == 2);
    CHECK(counts.lowered == 3);                 // html, body, svg
    CHECK(counts.native == 2);

    const auto* svg = find_node(root, [](const IRNode& node) {
        return attribute(node, "paint_class") == "element-capture-fallback";
    });
    REQUIRE(svg != nullptr);
    CHECK(attribute(*svg, "capture_fallback_element") == "svg");
    // Pooling removes the shape children; it must not also strip the ancestry
    // that says WHERE the artwork sits.
    const auto* body = find_named(root, "body");
    REQUIRE(body != nullptr);
    CHECK(contains(*body, svg));
}

TEST_CASE("pooling reaches through a plain element inside the capture",
          "[browser-capture][native-lowering]") {
    // `<g>` is how real SVG groups its shapes, so the depth at which a shape
    // sits under its `<svg>` is arbitrary. Pooling therefore has to be an
    // ANCESTOR question: a parent-only test pools the `<g>` and then emits the
    // `<path>` natively ON TOP of the raster the `<svg>` was captured as, which
    // is the one outcome the capture-fallback class exists to prevent.
    //
    // The intermediate is deliberately NOT another `<svg>`: an inner `<svg>` is
    // itself capture-only, so it would pool under either rule and prove nothing.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 svg, 4 g, 5 path
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,4,15,5]",
            .node_types = "[9,1,1,1,1,1]",
            .parents = "[-1,0,1,2,3,4]",
            .attributes = "[[],[],[],[],[],[]]",
            .layout_nodes = "[0,1,2,3,4,5]",
            .styles = "[[11,14],[11,14],[11,14],[11,14],[11,14],[11,14]]",
            .bounds = "[[0,0,100,100],[0,0,100,100],[0,0,100,100],"
                      "[10,10,50,50],[12,12,30,30],[14,14,10,10]]",
            .paint_orders = "[0,1,1,2,3,4]",
        },
        "svg-group");
    const auto& counts = lowered.counts;
    const auto& root = lowered.root;

    CHECK(counts.element_capture_fallback == 1);
    CHECK(counts.pooled_into_fallback == 2);  // the <g> AND the <path> under it
    CHECK(counts.lowered == 3);               // html, body, svg
    CHECK(counts.native == 2);                // html, body

    // Said as the thing that actually goes wrong: no shape may be drawn over
    // the raster that already contains it.
    const auto drawn_over_the_raster =
        count_nodes(root, [](const IRNode& node) {
            const auto tag = attribute(node, "source_tag");
            return tag == "g" || tag == "path";
        });
    CHECK(drawn_over_the_raster == 0);

    // Same document, but Chrome ranks the shapes BEFORE the `<svg>` that
    // contains them, so the walk reaches them first. Pooling is answered from
    // the document tree for exactly this reason: an answer memoized in visit
    // order would record "not under a capture" for the whole chain and then
    // keep giving that answer once the `<svg>` finally arrived.
    const auto reversed = lower_snapshot(
        {
            .node_names = "[0,1,2,4,15,5]",
            .node_types = "[9,1,1,1,1,1]",
            .parents = "[-1,0,1,2,3,4]",
            .attributes = "[[],[],[],[],[],[]]",
            .layout_nodes = "[0,1,2,3,4,5]",
            .styles = "[[11,14],[11,14],[11,14],[11,14],[11,14],[11,14]]",
            .bounds = "[[0,0,100,100],[0,0,100,100],[0,0,100,100],"
                      "[10,10,50,50],[12,12,30,30],[14,14,10,10]]",
            // svg 4, g 3, path 2 — the shapes are visited first.
            .paint_orders = "[0,1,1,4,3,2]",
        },
        "svg-group-reversed");
    CHECK(reversed.counts.pooled_into_fallback == 2);
    CHECK(reversed.counts.lowered == 3);
    CHECK(count_nodes(reversed.root, [](const IRNode& node) {
              const auto tag = attribute(node, "source_tag");
              return tag == "g" || tag == "path";
          }) == 0);
}

TEST_CASE("canvas and image elements classify away from native",
          "[browser-capture][native-lowering]") {
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 CANVAS, 4 IMG, 5 DIV(url bg)
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,6,7,3]",
            .node_types = "[9,1,1,1,1,1]",
            .parents = "[-1,0,1,2,2,2]",
            .attributes = "[[],[],[],[],[8,9],[]]",
            .layout_nodes = "[0,1,2,3,4,5]",
            .styles = "[[11,14],[11,14],[11,14],[11,14],[11,14],[12,14]]",
            .bounds = "[[0,0,200,200],[0,0,200,200],[0,0,200,200],"
                      "[0,0,116,116],[10,10,20,20],[40,40,60,60]]",
            .paint_orders = "[0,1,1,2,2,2]",
        },
        "assets");
    const auto& counts = lowered.counts;
    const auto& root = lowered.root;
    CHECK(counts.element_capture_fallback == 1);  // <canvas>: no styles to draw
    CHECK(counts.image_asset == 2);               // <img> and the url() fill
    CHECK(counts.native == 2);                    // html, body
    CHECK(counts.pooled_into_fallback == 0);      // the canvas has no children

    const auto* image = find_node(root, [](const IRNode& node) {
        return node.type == "image";
    });
    REQUIRE(image != nullptr);
    CHECK(attribute(*image, "src") == "logo.png");
}

TEST_CASE("a fallback that carries no raster says it paints nothing",
          "[browser-capture][native-lowering]") {
    // The classification is honest and the RENDER is empty: nothing attaches a
    // raster to a fallback node, so the frame it emits paints only whatever
    // background the element's own styles carry — for a `<canvas>`, nothing.
    // Left at that, the node is counted among the lowered and reads downstream
    // as handled by a capture. It has to say otherwise on its face.
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,6]",
            .node_types = "[9,1,1,1]",
            .parents = "[-1,0,1,2]",
            .attributes = "[[],[],[],[]]",
            .layout_nodes = "[0,1,2,3]",
            .styles = "[[11,14],[11,14],[11,14],[11,14]]",
            .bounds = "[[0,0,200,100],[0,0,200,100],[0,0,200,100],"
                      "[0,0,40,20]]",
            .paint_orders = "[0,1,1,2]",
        },
        "unpainted-fallback");
    const auto* canvas = find_node(lowered.root, [](const IRNode& node) {
        return attribute(node, "capture_fallback_element") == "canvas";
    });
    REQUIRE(canvas != nullptr);
    CHECK(attribute(*canvas, "unpainted") == "true");
    // 40x20 of it, and nothing else in the document is unpainted.
    CHECK(lowered.counts.unpainted_fallback_area == 800.0);

    // A node that CAN be drawn from styles must not be tarred with it, or the
    // number stops meaning anything.
    const auto* body = find_node(lowered.root, [](const IRNode& node) {
        return attribute(node, "source_tag") == "body";
    });
    REQUIRE(body != nullptr);
    CHECK(body->attributes.count("unpainted") == 0);
}

TEST_CASE("unpainted area separates an icon-sized hole from a panel-sized one",
          "[browser-capture][native-lowering]") {
    // The count cannot rank these and the area can, which is the whole reason
    // the area exists. Measured on the real corpus: forge's EIGHTEEN fallback
    // nodes are inline `<svg>` icons covering 0.4% of the panel, while SPECTR's
    // TWO are full-window `<canvas>` elements covering it twice over. Read as
    // counts, the eighteen look nine times worse than the two.
    const auto icons = lower_snapshot(
        {
            .node_names = "[0,1,2,4,4,4]",
            .node_types = "[9,1,1,1,1,1]",
            .parents = "[-1,0,1,2,2,2]",
            .attributes = "[[],[],[],[],[],[]]",
            .layout_nodes = "[0,1,2,3,4,5]",
            .styles = "[[11,14],[11,14],[11,14],[11,14],[11,14],[11,14]]",
            .bounds = "[[0,0,400,200],[0,0,400,200],[0,0,400,200],"
                      "[0,0,16,16],[20,0,16,16],[40,0,16,16]]",
            .paint_orders = "[0,1,1,2,3,4]",
        },
        "unpainted-icons");
    const auto panel = lower_snapshot(
        {
            .node_names = "[0,1,2,6]",
            .node_types = "[9,1,1,1]",
            .parents = "[-1,0,1,2]",
            .attributes = "[[],[],[],[]]",
            .layout_nodes = "[0,1,2,3]",
            .styles = "[[11,14],[11,14],[11,14],[11,14]]",
            .bounds = "[[0,0,400,200],[0,0,400,200],[0,0,400,200],"
                      "[0,0,400,200]]",
            .paint_orders = "[0,1,1,2]",
        },
        "unpainted-panel");

    // Three holes against one: the count ranks the icons as the worse design.
    CHECK(icons.counts.element_capture_fallback == 3);
    CHECK(panel.counts.element_capture_fallback == 1);
    // The area ranks them the other way round, which is the true one — 768 px²
    // of a 80,000 px² document against all 80,000 of it.
    CHECK(icons.counts.unpainted_fallback_area == 768.0);
    CHECK(panel.counts.unpainted_fallback_area == 80000.0);
    CHECK(panel.counts.unpainted_fallback_area >
          icons.counts.unpainted_fallback_area * 100.0);
}

TEST_CASE("every element whose pixels are not CSS is captured, not styled",
          "[browser-capture][native-lowering]") {
    // `<canvas>` and `<svg>` are the two the other cases reach. The rest of the
    // set is here because classifying any of them `native` emits an empty
    // styled box where the design has a video, an embedded document, or a
    // formula — a hole nothing else in the pipeline can attribute.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 VIDEO, 4 IFRAME, 5 EMBED,
    //        6 OBJECT, 7 math
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,16,17,18,19,20]",
            .node_types = "[9,1,1,1,1,1,1,1]",
            .parents = "[-1,0,1,2,2,2,2,2]",
            .attributes = "[[],[],[],[],[],[],[],[]]",
            .layout_nodes = "[0,1,2,3,4,5,6,7]",
            .styles = "[[11,14],[11,14],[11,14],[11,14],[11,14],[11,14],"
                      "[11,14],[11,14]]",
            .bounds = "[[0,0,300,300],[0,0,300,300],[0,0,300,300],"
                      "[0,0,160,90],[0,100,160,90],[0,200,40,40],"
                      "[60,200,40,40],[120,200,40,40]]",
            .paint_orders = "[0,1,1,2,3,4,5,6]",
        },
        "capture-only-tags");
    CHECK(lowered.counts.element_capture_fallback == 5);
    CHECK(lowered.counts.native == 2);   // html, body — nothing else
    CHECK(lowered.counts.lowered == 7);

    for (const char* tag : {"video", "iframe", "embed", "object", "math"}) {
        INFO("tag " << tag);
        const auto* node = find_node(lowered.root, [tag](const IRNode& entry) {
            return attribute(entry, "capture_fallback_element") == tag;
        });
        REQUIRE(node != nullptr);
        CHECK(attribute(*node, "paint_class") == "element-capture-fallback");
    }
}

TEST_CASE("a fill that is not a gradient needs an asset whatever names it",
          "[browser-capture][native-lowering]") {
    // `image-set()` is the responsive-image idiom a real design system emits,
    // and it is not the only spelling of "this fill is a picture". Testing for
    // `url(` specifically classifies these `native` — a frame with no fill,
    // drawn as nothing, reported as drawn. Gradients are already split off
    // before this point, so ANY surviving `background-image` is an asset.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 DIV image-set(),
    //        4 DIV -webkit-image-set()
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,3,3]",
            .node_types = "[9,1,1,1,1]",
            .parents = "[-1,0,1,2,2]",
            .attributes = "[[],[],[],[],[]]",
            .layout_nodes = "[0,1,2,3,4]",
            .styles = "[[11,14],[11,14],[11,14],[21,14],[22,14]]",
            .bounds = "[[0,0,200,200],[0,0,200,200],[0,0,200,200],"
                      "[10,10,60,60],[80,10,60,60]]",
            .paint_orders = "[0,1,1,2,3]",
        },
        "image-set");
    CHECK(lowered.counts.image_asset == 2);
    CHECK(lowered.counts.native == 2);   // html and body, and nothing else
    CHECK(lowered.counts.lowered == 4);
    CHECK(count_nodes(lowered.root, [](const IRNode& node) {
              return attribute(node, "paint_class") == "image-asset";
          }) == 2);
}

TEST_CASE("a skipped wrapper elides without orphaning what it contained",
          "[browser-capture][native-lowering]") {
    // A zero-area wrapper is not lowered, and its painted descendants have to
    // land on the nearest ancestor that WAS. Reading only the immediate DOM
    // parent finds nothing for them and grafts the whole subtree onto the IR
    // root — the design's structure silently flattened at the first collapsed
    // box, which every count in this file would still agree with.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 DIV.wrap (0×0),
    //        4 DIV.inner, 5 DIV.leaf
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,3,3,3]",
            .node_types = "[9,1,1,1,1,1]",
            .parents = "[-1,0,1,2,3,4]",
            .attributes = "[[],[],[],[23,24],[23,25],[23,26]]",
            .layout_nodes = "[0,1,2,3,4,5]",
            .styles = "[[11,14],[11,14],[11,14],[11,14],[11,14],[11,14]]",
            .bounds = "[[0,0,200,200],[0,0,200,200],[0,0,200,200],"
                      "[10,10,0,0],[20,20,100,100],[30,30,40,40]]",
            .paint_orders = "[0,1,1,2,3,4]",
        },
        "elided-wrapper");
    const auto& root = lowered.root;
    CHECK(lowered.counts.skipped_empty_box == 1);
    CHECK(lowered.counts.lowered == 4);

    // Nothing escaped to the root: `<html>` is still the only thing there.
    REQUIRE(root.children.size() == 1);
    const auto* body = find_named(root, "body");
    REQUIRE(body != nullptr);
    REQUIRE(body->children.size() == 1);
    CHECK(body->children[0].name == "div.inner");
    REQUIRE(body->children[0].children.size() == 1);
    CHECK(body->children[0].children[0].name == "div.leaf");

    // Eliding is a placement accommodation, not a claim about the document:
    // the anchor still records the wrapper the node really lives in, so an
    // edit stored against it survives the wrapper gaining an area.
    CHECK(*body->children[0].stable_anchor_id ==
          "capture:html[0]/body[0]/div.wrap[0]/div.inner[0]");
}

TEST_CASE("a hidden sibling does not move its neighbour's anchor",
          "[browser-capture][native-lowering]") {
    // The merge layer keys a human's edits off these anchors. An ordinal
    // counted over the PAINTED siblings alone renumbers a node whenever a
    // sibling is hidden — a change to the page's state, not to its structure —
    // and the edit is then applied to the wrong node or reported as a conflict
    // that never happened.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 DIV.item, 4 DIV.item
    const std::string names = "[0,1,2,3,3]";
    const std::string types = "[9,1,1,1,1]";
    const std::string parents = "[-1,0,1,2,2]";
    const std::string attributes = "[[],[],[],[23,27],[23,27]]";

    // The first `.item` is `display: none`, so Chrome never lays it out and it
    // is absent from the layout array while remaining in the document.
    const auto hidden = lower_snapshot(
        {
            .node_names = names,
            .node_types = types,
            .parents = parents,
            .attributes = attributes,
            .layout_nodes = "[0,1,2,4]",
            .styles = "[[11,14],[11,14],[11,14],[11,14]]",
            .bounds = "[[0,0,200,200],[0,0,200,200],[0,0,200,200],"
                      "[70,10,50,50]]",
            .paint_orders = "[0,1,1,2]",
        },
        "ordinals-hidden");
    CHECK(hidden.counts.lowered == 3);
    const auto* survivor = find_named(hidden.root, "div.item");
    REQUIRE(survivor != nullptr);
    CHECK(*survivor->stable_anchor_id ==
          "capture:html[0]/body[0]/div.item[1]");

    // The same document with the sibling shown. The surviving node's anchor is
    // the SAME string — which is the whole property, so it is compared against
    // the hidden capture's answer rather than only against a literal.
    const auto shown = lower_snapshot(
        {
            .node_names = names,
            .node_types = types,
            .parents = parents,
            .attributes = attributes,
            .layout_nodes = "[0,1,2,3,4]",
            .styles = "[[11,14],[11,14],[11,14],[11,14],[11,14]]",
            .bounds = "[[0,0,200,200],[0,0,200,200],[0,0,200,200],"
                      "[10,10,50,50],[70,10,50,50]]",
            .paint_orders = "[0,1,1,2,3]",
        },
        "ordinals-shown");
    CHECK(shown.counts.lowered == 4);
    const auto* shown_body = find_named(shown.root, "body");
    REQUIRE(shown_body != nullptr);
    REQUIRE(shown_body->children.size() == 2);
    CHECK(*shown_body->children[0].stable_anchor_id ==
          "capture:html[0]/body[0]/div.item[0]");
    CHECK(*shown_body->children[1].stable_anchor_id ==
          *survivor->stable_anchor_id);
}

TEST_CASE("hiding a middle sibling misapplies no stored edit",
          "[browser-capture][native-lowering]") {
    // The consequence, rather than the mechanism. Three knobs; the MIDDLE one
    // is `display: none` in one capture and visible in the other, and nothing
    // structural changed between them. An ordinal counted over the painted set
    // renumbers MIX from `knob[2]` to `knob[1]` — so an edit stored against MIX
    // silently lands on TONE, and the merge reports a clean apply.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY,
    //        3 DIV.knob 4 #text DRIVE, 5 DIV.knob 6 #text TONE,
    //        7 DIV.knob 8 #text MIX
    const std::string names = "[0,1,2,3,28,3,28,3,28]";
    const std::string types = "[9,1,1,1,3,1,3,1,3]";
    const std::string parents = "[-1,0,1,2,3,2,5,2,7]";
    const std::string attributes =
        "[[],[],[],[23,36],[],[23,36],[],[23,36],[]]";

    const auto hidden = lower_snapshot(
        {
            .node_names = names,
            .node_types = types,
            .parents = parents,
            .attributes = attributes,
            .layout_nodes = "[0,1,2,3,4,7,8]",
            .styles = "[[11,14],[11,14],[11,14],[11,14],[11,14],[11,14],"
                      "[11,14]]",
            .bounds = "[[0,0,400,400],[0,0,400,400],[0,0,400,400],"
                      "[10,10,80,80],[10,10,80,20],"
                      "[190,10,80,80],[190,10,80,20]]",
            .paint_orders = "[0,1,1,2,3,4,5]",
            .texts = "[-1,-1,-1,-1,37,-1,39]",
        },
        "knobs-hidden");
    const auto shown = lower_snapshot(
        {
            .node_names = names,
            .node_types = types,
            .parents = parents,
            .attributes = attributes,
            .layout_nodes = "[0,1,2,3,4,5,6,7,8]",
            .styles = "[[11,14],[11,14],[11,14],[11,14],[11,14],[11,14],"
                      "[11,14],[11,14],[11,14]]",
            .bounds = "[[0,0,400,400],[0,0,400,400],[0,0,400,400],"
                      "[10,10,80,80],[10,10,80,20],"
                      "[100,10,80,80],[100,10,80,20],"
                      "[190,10,80,80],[190,10,80,20]]",
            .paint_orders = "[0,1,1,2,3,4,5,6,7]",
            .texts = "[-1,-1,-1,-1,37,-1,38,-1,39]",
        },
        "knobs-shown");
    CHECK(hidden.counts.lowered == 6);   // html, body, two knobs, two labels
    CHECK(shown.counts.lowered == 8);    // and the middle knob with its label

    // What an edit is stored against, keyed by the label a human recognises.
    const auto anchor_of_label = [](const IRNode& root) {
        std::map<std::string, std::string> out;
        for (const auto& entry : composed(root)) {
            if (entry.node->text_content.empty()) continue;
            out[entry.node->text_content] =
                entry.node->stable_anchor_id.value_or("<none>");
        }
        return out;
    };
    const auto before = anchor_of_label(hidden.root);
    const auto after = anchor_of_label(shown.root);

    // MIX keeps its ordinal across the sibling appearing, because the tally
    // counts the document's children and not the capture's.
    REQUIRE(before.count("MIX") == 1);
    CHECK(before.at("MIX") ==
          "capture:html[0]/body[0]/div.knob[2]/#text[0]");
    CHECK(after.at("MIX") == before.at("MIX"));
    CHECK(after.at("DRIVE") == before.at("DRIVE"));

    // Said as the failure it prevents: no anchor stored against one label may
    // resolve to a different label after the re-capture.
    std::map<std::string, std::string> label_at_anchor;
    for (const auto& [label, anchor] : after) label_at_anchor[anchor] = label;
    for (const auto& [label, anchor] : before) {
        INFO("anchor " << anchor << " stored against " << label);
        REQUIRE(label_at_anchor.count(anchor) == 1);
        CHECK(label_at_anchor.at(anchor) == label);
    }
}

TEST_CASE("a class list is split on any whitespace, not only a space",
          "[browser-capture][native-lowering]") {
    // Every HTML formatter wraps a long class list across lines, and
    // `prettier-plugin-tailwindcss` reorders one on save, so a tab or a newline
    // between class tokens is ordinary rather than exotic. Splitting on a
    // literal space alone leaves that whitespace INSIDE the node's signature —
    // and inside every anchor derived from it — so reformatting a file that
    // changed nothing renames the node and orphans its stored edits.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY,
    //        3 DIV class="panel<TAB>active", 4 DIV class="<LF>  card  x",
    //        5 DIV class=" panel"  (a template rendering an empty slot first)
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,3,3,3]",
            .node_types = "[9,1,1,1,1,1]",
            .parents = "[-1,0,1,2,2,2]",
            .attributes = "[[],[],[],[23,40],[23,41],[23,42]]",
            .layout_nodes = "[0,1,2,3,4,5]",
            .styles = "[[11,14],[11,14],[11,14],[11,14],[11,14],[11,14]]",
            .bounds = "[[0,0,300,300],[0,0,300,300],[0,0,300,300],"
                      "[10,10,80,80],[100,10,80,80],[190,10,80,80]]",
            .paint_orders = "[0,1,1,2,3,4]",
        },
        "class-whitespace");
    const auto& root = lowered.root;
    CHECK(lowered.counts.lowered == 5);

    // The tab-separated and the leading-space forms both name the same class
    // the plain form would.
    CHECK(count_nodes(root, [](const IRNode& node) {
              return node.name == "div.panel";
          }) == 2);
    CHECK(find_named(root, "div.card") != nullptr);

    // The two failures this replaces, said directly: whitespace never reaches a
    // name, and an empty leading token never degenerates one to a bare dot.
    CHECK(count_nodes(root, [](const IRNode& node) {
              return node.name.find_first_of(" \t\r\n\f\v") != std::string::npos;
          }) == 0);
    CHECK(count_nodes(root, [](const IRNode& node) {
              return node.name == "div.";
          }) == 0);
}

TEST_CASE("an anchor segment escapes the path's own delimiters",
          "[browser-capture][native-lowering]") {
    // `id` and `class` text is author-controlled and routinely contains the
    // characters the path is built out of. Concatenating it raw makes an anchor
    // that cannot be split back into segments, and — the part that costs a
    // human their edit — lets two different nodes spell the same anchor.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 DIV.a, 4 DIV.b inside it,
    //        5 DIV whose class is literally `a[0]/div.b`,
    //        6 DIV class="w-1/2 p-4"  (the everyday Tailwind spelling)
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,3,3,3,3]",
            .node_types = "[9,1,1,1,1,1,1]",
            .parents = "[-1,0,1,2,3,2,2]",
            .attributes = "[[],[],[],[23,44],[23,45],[23,46],[23,43]]",
            .layout_nodes = "[0,1,2,3,4,5,6]",
            .styles = "[[11,14],[11,14],[11,14],[11,14],[11,14],[11,14],"
                      "[11,14]]",
            .bounds = "[[0,0,300,300],[0,0,300,300],[0,0,300,300],"
                      "[10,10,80,80],[20,20,40,40],[100,10,80,80],"
                      "[190,10,80,80]]",
            .paint_orders = "[0,1,1,2,3,4,5]",
        },
        "anchor-escaping");
    const auto& root = lowered.root;
    CHECK(lowered.counts.lowered == 6);

    // The nested `div.b` and the node whose class merely SPELLS that nesting
    // are different nodes, so they must not be the same anchor. Unescaped they
    // are both `capture:html[0]/body[0]/div.a[0]/div.b[0]`.
    const auto* nested = find_named(root, "div.b");
    const auto* impostor = find_named(root, "div.a[0]/div.b");
    REQUIRE(nested != nullptr);
    REQUIRE(impostor != nullptr);
    CHECK(*nested->stable_anchor_id != *impostor->stable_anchor_id);
    CHECK(*nested->stable_anchor_id ==
          "capture:html[0]/body[0]/div.a[0]/div.b[0]");
    CHECK(*impostor->stable_anchor_id ==
          "capture:html[0]/body[0]/div.a\\[0\\]\\/div.b[0]");

    // A Tailwind fraction is the version of this that ships in real designs.
    const auto* fraction = find_named(root, "div.w-1/2");
    REQUIRE(fraction != nullptr);
    CHECK(*fraction->stable_anchor_id ==
          "capture:html[0]/body[0]/div.w-1\\/2[0]");

    // The invariant the collision breaks, restated over the whole tree.
    const auto all = anchors(root);
    const std::set<std::string> unique(all.begin(), all.end());
    CHECK(unique.size() == all.size());
}

TEST_CASE("a rotated element is not reported as drawn",
          "[browser-capture][native-lowering]") {
    // Snapshot bounds are post-transform, but for a rotation that box is the
    // axis-aligned BOUNDING box and not the element's shape: Chrome reports a
    // 100×20 bar at 45° as an 85×85 square. Drawing the box fills ~3.7× the ink
    // in the wrong outline — and classifying it `native` makes the census claim
    // the panel is fully drawn while it is not.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 DIV rotated 45°,
    //        4 SPAN inside it, 5 DIV scaled 2×
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,3,49,3]",
            .node_types = "[9,1,1,1,1,1]",
            .parents = "[-1,0,1,2,3,2]",
            .attributes = "[[],[],[],[],[],[]]",
            .layout_nodes = "[0,1,2,3,4,5]",
            .styles = "[[14,11],[14,11],[14,11],[14,47],[14,11],[14,48]]",
            .bounds = "[[0,0,400,400],[0,0,400,400],[0,0,400,400],"
                      "[157.5625,157.5625,84.875,84.875],"
                      "[163.21875,161.8125,27.84375,27.828125],"
                      "[95,35,100,100]]",
            .paint_orders = "[0,1,1,2,3,4]",
            .computed_names = R"(["display","transform"])",
        },
        "rotated");
    const auto& root = lowered.root;

    CHECK(lowered.counts.element_capture_fallback == 1);
    // The SPAN inside the rotated box is covered by the raster the box becomes,
    // so it pools rather than drawing over it — the same rule an `<svg>` gets.
    CHECK(lowered.counts.pooled_into_fallback == 1);
    CHECK(lowered.counts.lowered == 4);

    const auto* rotated = find_node(root, [](const IRNode& node) {
        return attribute(node, "paint_class") == "element-capture-fallback";
    });
    REQUIRE(rotated != nullptr);
    CHECK(attribute(*rotated, "capture_fallback_reason") == "transform");

    // A 2× scale stays native, because ITS bounding box really is its shape.
    // This is why the assumption reads as true until something rotates, so it
    // is pinned rather than left to be rediscovered.
    CHECK(lowered.counts.native == 3);   // html, body, and the scaled div
    const auto* scaled = find_node(root, [](const IRNode& node) {
        return attribute(node, "paint_class") == "native" &&
               node.style.width && *node.style.width == 100.0f;
    });
    REQUIRE(scaled != nullptr);
}

TEST_CASE("a parentIndex cycle terminates instead of hanging the importer",
          "[browser-capture][native-lowering]") {
    // The DOM snapshot is a sidecar of a bundle the pipeline otherwise treats
    // as untrusted, and every ancestry question here walks `parent_of` until it
    // goes negative. A node whose parent is itself never gets there: the walk
    // spins while its accumulator grows, and a self-parented node additionally
    // becomes its own child and recurses the tree walk until the stack is gone.
    //
    // Reaching this assertion at all is the result being asserted.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 DIV whose parent is itself
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,3]",
            .node_types = "[9,1,1,1]",
            .parents = "[-1,0,1,3]",
            .attributes = "[[],[],[],[]]",
            .layout_nodes = "[0,1,2,3]",
            .styles = "[[11,14],[11,14],[11,14],[11,14]]",
            .bounds = "[[0,0,200,200],[0,0,200,200],[0,0,200,200],"
                      "[10,10,50,50]]",
            .paint_orders = "[0,1,1,2]",
        },
        "parent-cycle");
    CHECK(lowered.counts.lowered == 3);

    // The cyclic node is still emitted, and it is NOT inside itself.
    const auto* cyclic = find_named(lowered.root, "div");
    REQUIRE(cyclic != nullptr);
    CHECK(cyclic->children.empty());
    CHECK(cyclic->stable_anchor_id.has_value());
}

// ── The clip model ──────────────────────────────────────────────────────────
//
// A tree carrying `overflow` clips by DOM parentage, because a renderer applies
// it to whatever the node's children turn out to be. CSS clips along the
// containing-block chain instead, and the two disagree in BOTH directions — an
// absolutely positioned node escapes an `overflow: hidden` ancestor it sits
// inside, and a hoisted node keeps a clip its old parent gave it — so no
// re-parenting fixes both. Lowering therefore resolves each node's real clip
// chain and stores the intersection as ONE RECTANGLE ON THE NODE, relative to
// the node. A rectangle attached to the node travels with the node, so where
// the node ends up in the tree cannot change what it clips.
//
// The two counters stay wired as the audit of that model, read back off the
// emitted tree rather than off the model that produced it.

TEST_CASE("a node escaping a clip along the containing-block chain is not clipped",
          "[browser-capture][native-lowering][clip-model]") {
    // `#esc` is absolutely positioned and its containing block is `#panel`, so
    // `#clip` — statically positioned, in between, `overflow: hidden` — does
    // not clip it: Chrome paints it in full, outside `#clip` entirely. It is
    // still emitted UNDER `#clip`, because the tree mirrors the DOM, so the
    // proof is that being there costs it nothing: no clip rectangle, and no
    // `overflow` left on the parent to impose one.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 DIV#panel (relative),
    //        4 DIV#clip (static, hidden), 5 DIV#esc (absolute)
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,3,3,3]",
            .node_types = "[9,1,1,1,1,1]",
            .parents = "[-1,0,1,2,3,4]",
            .attributes = "[[],[],[],[],[],[]]",
            .layout_nodes = "[0,1,2,3,4,5]",
            .styles = "[[50,52,11,11,55,55,55,55],[50,52,11,11,55,55,55,55],"
                      "[50,52,11,11,55,55,55,55],[50,53,11,11,55,55,55,55],"
                      "[51,52,11,11,55,55,55,55],[50,54,11,11,55,55,55,55]]",
            .bounds = "[[0,0,400,400],[0,0,400,400],[0,0,400,400],"
                      "[0,0,400,400],[40,40,100,100],[120,260,200,40]]",
            .paint_orders = "[0,1,1,2,3,4]",
            .computed_names = std::string(kClipProperties),
        },
        "clip-escape");
    CHECK(lowered.counts.lowered == 5);
    CHECK(lowered.counts.clip_over_applied == 0);
    CHECK(lowered.counts.clip_lost == 0);

    // The escaping node itself: full size, no clip rectangle at all.
    const auto* escaped = find_node(lowered.root, [](const IRNode& node) {
        return node.style.width.value_or(0.0f) == 200.0f &&
               node.style.height.value_or(0.0f) == 40.0f;
    });
    REQUIRE(escaped != nullptr);
    CHECK_FALSE(escaped->style.clip_rect.has_value());
    CHECK(attribute(*escaped, "clip_over_applied").empty());

    // And its emitted parent no longer carries the `overflow` that would clip
    // it by parentage. This is the assertion that makes the one above mean
    // something: an unclipped node under a still-clipping parent is clipped.
    const auto* clipper = find_node(lowered.root, [](const IRNode& node) {
        return node.style.width.value_or(0.0f) == 100.0f;
    });
    REQUIRE(clipper != nullptr);
    CHECK_FALSE(clipper->style.overflow.has_value());
    CHECK(contains(*clipper, escaped));
}

TEST_CASE("a hoisted node keeps the clip its DOM parent gave it",
          "[browser-capture][native-lowering][clip-model]") {
    // The mirror image. `#under` is absolutely positioned inside `#card`, whose
    // `overflow: hidden` DOES clip it — `#card` is its containing block. But
    // `#under` paints before its own parent, so it is hoisted out from under
    // `#card` and no emitted ancestor carries that clip any more. The rectangle
    // on the node is what survives the move.
    //
    // `#card` is 102x102 at (150,150) with a 1px border, so the padding box it
    // clips to is the 100x100 at (151,151) — NOT the box the snapshot reports.
    // `#under` sits at (91,171), so in its own space the clip starts at
    // x = 151-91 = 60, y = 151-171 = -20.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 DIV#panel (relative),
    //        4 DIV#card (absolute, hidden, 1px border),
    //        5 DIV#under (absolute) inside it
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,3,3,3]",
            .node_types = "[9,1,1,1,1,1]",
            .parents = "[-1,0,1,2,3,4]",
            .attributes = "[[],[],[],[],[],[]]",
            .layout_nodes = "[0,1,2,3,4,5]",
            .styles = "[[50,52,11,11,55,55,55,55],[50,52,11,11,55,55,55,55],"
                      "[50,52,11,11,55,55,55,55],[50,53,11,11,55,55,55,55],"
                      "[51,54,11,11,56,56,56,56],[50,54,11,11,55,55,55,55]]",
            .bounds = "[[0,0,400,400],[0,0,400,400],[0,0,400,400],"
                      "[0,0,400,400],[150,150,102,102],[91,171,200,40]]",
            // #under paints before #card, which is what fires the hoist.
            .paint_orders = "[0,1,1,2,5,3]",
            .computed_names = std::string(kClipProperties),
        },
        "clip-hoisted");
    CHECK(lowered.counts.lowered == 5);
    CHECK(lowered.counts.hoisted_escapes == 1);
    CHECK(lowered.counts.clip_lost == 0);
    CHECK(lowered.counts.clip_over_applied == 0);

    const auto* under = find_node(lowered.root, [](const IRNode& node) {
        return attribute(node, "paint_order_hoisted") == "1";
    });
    REQUIRE(under != nullptr);
    REQUIRE(under->style.clip_rect.has_value());
    CHECK(under->style.clip_rect->x == 60.0f);
    CHECK(under->style.clip_rect->y == -20.0f);
    CHECK(under->style.clip_rect->width == 100.0f);
    CHECK(under->style.clip_rect->height == 100.0f);
    // The node itself is still its full solved size — the clip constrains what
    // is drawn, it does not resize the box.
    CHECK(*under->style.width == 200.0f);
}

TEST_CASE("a clip that really does apply is carried, and a no-op one is not",
          "[browser-capture][native-lowering][clip-model]") {
    // Without this, the model could simply be "never clip anything" and both
    // cases above would still pass. Two shapes where a clip genuinely applies:
    // an in-flow child of a clipping parent, and an absolutely positioned child
    // whose containing block IS that parent — the case that looks like the
    // escape above and is not one. Both children stick out of their clipper, so
    // both must carry the rectangle.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY,
    //        3 DIV.wrap (static, hidden), 4 DIV.inner (static) inside it,
    //        5 DIV.item (relative, hidden), 6 DIV.leaf (absolute) inside it
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,3,3,3,3]",
            .node_types = "[9,1,1,1,1,1,1]",
            .parents = "[-1,0,1,2,3,2,5]",
            .attributes = "[[],[],[],[23,24],[23,25],[23,27],[23,26]]",
            .layout_nodes = "[0,1,2,3,4,5,6]",
            .styles = "[[50,52,11,11,55,55,55,55],[50,52,11,11,55,55,55,55],"
                      "[50,52,11,11,55,55,55,55],[51,52,11,11,55,55,55,55],"
                      "[50,52,11,11,55,55,55,55],[51,53,11,11,55,55,55,55],"
                      "[50,54,11,11,55,55,55,55]]",
            .bounds = "[[0,0,400,400],[0,0,400,400],[0,0,400,400],"
                      "[10,10,100,100],[20,20,140,40],"
                      "[200,10,100,100],[210,20,140,40]]",
            .paint_orders = "[0,1,1,2,3,4,5]",
            .computed_names = std::string(kClipProperties),
        },
        "clip-agrees");
    CHECK(lowered.counts.lowered == 6);
    CHECK(lowered.counts.clip_over_applied == 0);
    CHECK(lowered.counts.clip_lost == 0);

    // `.inner` is in flow inside `.wrap`, so `.wrap`'s box clips it: in its own
    // space the clip runs from (-10,-10) for 100x100.
    const auto* inner = find_named(lowered.root, "div.inner");
    REQUIRE(inner != nullptr);
    REQUIRE(inner->style.clip_rect.has_value());
    CHECK(inner->style.clip_rect->x == -10.0f);
    CHECK(inner->style.clip_rect->y == -10.0f);
    CHECK(inner->style.clip_rect->width == 100.0f);

    // `.leaf` is out of flow but `.item` IS its containing block, so the clip
    // applies to it too — the shape that must NOT be mistaken for an escape.
    const auto* leaf = find_named(lowered.root, "div.leaf");
    REQUIRE(leaf != nullptr);
    REQUIRE(leaf->style.clip_rect.has_value());
    CHECK(leaf->style.clip_rect->width == 100.0f);

    // The clippers themselves are inside nothing that clips, so they carry no
    // rectangle: a clip is stored where it BITES, not on every node under a
    // container that happens to have `overflow`.
    const auto* wrap = find_named(lowered.root, "div.wrap");
    REQUIRE(wrap != nullptr);
    CHECK_FALSE(wrap->style.clip_rect.has_value());
}

TEST_CASE("a clip that cannot bite is not written onto the node",
          "[browser-capture][native-lowering][clip-model]") {
    // A rectangle that already holds everything the node draws would make the
    // renderer install a clip every frame to change nothing, and would put a
    // clip on essentially every node of any panel with an outer
    // `overflow: hidden`. It is skipped — but only when the node's ink IS its
    // box. `.glow` carries a 30px-blur shadow that paints well outside its box,
    // and CSS clips that overspill, so its clip is kept even though its box
    // fits.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 DIV.wrap (static, hidden),
    //        4 DIV.inner (static, plain), 5 DIV.item (static, shadowed)
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,3,3,3]",
            .node_types = "[9,1,1,1,1,1]",
            .parents = "[-1,0,1,2,3,3]",
            .attributes = "[[],[],[],[23,24],[23,25],[23,27]]",
            .layout_nodes = "[0,1,2,3,4,5]",
            .styles = "[[50,52,11,11,55,55,55,55,11],"
                      "[50,52,11,11,55,55,55,55,11],"
                      "[50,52,11,11,55,55,55,55,11],"
                      "[51,52,11,11,55,55,55,55,11],"
                      "[50,52,11,11,55,55,55,55,11],"
                      "[50,52,11,11,55,55,55,55,58]]",
            .bounds = "[[0,0,400,400],[0,0,400,400],[0,0,400,400],"
                      "[10,10,200,200],[20,20,40,40],[80,80,40,40]]",
            .paint_orders = "[0,1,1,2,3,4]",
            .computed_names =
                R"(["overflow","position","transform","clip-path",)"
                R"("border-top-width","border-right-width",)"
                R"("border-bottom-width","border-left-width","box-shadow"])",
        },
        "clip-noop");
    CHECK(lowered.counts.lowered == 5);
    CHECK(lowered.counts.clip_over_applied == 0);
    CHECK(lowered.counts.clip_lost == 0);

    const auto* inner = find_named(lowered.root, "div.inner");
    REQUIRE(inner != nullptr);
    CHECK_FALSE(inner->style.clip_rect.has_value());

    const auto* shadowed = find_named(lowered.root, "div.item");
    REQUIRE(shadowed != nullptr);
    REQUIRE_FALSE(shadowed->style.box_shadow.empty());
    REQUIRE(shadowed->style.clip_rect.has_value());
    CHECK(shadowed->style.clip_rect->width == 200.0f);
}

TEST_CASE("a shape clip the rectangle cannot carry is counted as lost",
          "[browser-capture][native-lowering][clip-model]") {
    // `clip-path` clips everything painted inside the element, whatever its
    // position, and its region is a shape. One rectangle cannot stand in for a
    // circle, so the node keeps ink the browser cuts away — which is a real
    // `clip_lost`, and is what keeps that counter able to fire at all now that
    // the rectangular chain is resolved correctly. It is named on the node with
    // the reason, so it reads as a limit of the model rather than as a missing
    // rectangle someone could go add.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY,
    //        3 DIV.wrap (clip-path: circle(50%)), 4 DIV.inner inside it
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,3,3]",
            .node_types = "[9,1,1,1,1]",
            .parents = "[-1,0,1,2,3]",
            .attributes = "[[],[],[],[23,24],[23,25]]",
            .layout_nodes = "[0,1,2,3,4]",
            .styles = "[[50,52,11,11,55,55,55,55],[50,52,11,11,55,55,55,55],"
                      "[50,52,11,11,55,55,55,55],[50,52,11,57,55,55,55,55],"
                      "[50,52,11,11,55,55,55,55]]",
            .bounds = "[[0,0,400,400],[0,0,400,400],[0,0,400,400],"
                      "[10,10,100,100],[20,20,40,40]]",
            .paint_orders = "[0,1,1,2,3]",
            .computed_names = std::string(kClipProperties),
        },
        "clip-shape");
    CHECK(lowered.counts.lowered == 4);
    CHECK(lowered.counts.clip_lost == 1);
    CHECK(lowered.counts.clip_over_applied == 0);

    const auto* inner = find_named(lowered.root, "div.inner");
    REQUIRE(inner != nullptr);
    CHECK(attribute(*inner, "clip_lost") == "1");
    CHECK(attribute(*inner, "clip_inexpressible") == "clip-path");
}

TEST_CASE("the panel frame's crop is not counted as a clip disagreement",
          "[browser-capture][native-lowering][clip-model]") {
    // The root the caller lowers into is a window onto the page, and the crop
    // IS the panel — a node the frame cuts is out of frame, not mis-clipped.
    // Counting it would fire on `<html>` for every cropped capture and bury the
    // one node an ancestor inside the panel clipped by mistake, so the frame is
    // on both sides of the comparison and the verdict is unchanged by it.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 DIV.wrap — which runs 200px past
    // the right edge of the 200x200 frame the root declares.
    const SnapshotSpec spec{
        .node_names = "[0,1,2,3]",
        .node_types = "[9,1,1,1]",
        .parents = "[-1,0,1,2]",
        .attributes = "[[],[],[],[23,24]]",
        .layout_nodes = "[0,1,2,3]",
        .styles = "[[50,52,11,11,55,55,55,55],[50,52,11,11,55,55,55,55],"
                  "[50,52,11,11,55,55,55,55],[50,52,11,11,55,55,55,55]]",
        .bounds = "[[0,0,200,200],[0,0,200,200],[0,0,200,200],"
                  "[100,100,300,60]]",
        .paint_orders = "[0,1,1,2]",
        .computed_names = std::string(kClipProperties),
    };

    IRNode clipping_root;
    clipping_root.style.width = 200.0f;
    clipping_root.style.height = 200.0f;
    clipping_root.style.overflow = "hidden";
    const auto clipped = lower_into(spec, "clip-frame", clipping_root);
    CHECK(clipped.clip_over_applied == 0);
    CHECK(clipped.clip_lost == 0);

    // And the frame stays the ONLY thing in the emitted tree that clips by
    // parentage. `overflow` reappearing on a lowered node would put DOM
    // parentage back in charge underneath every per-node rectangle, which is
    // the defect the model exists to remove — so it is asserted structurally
    // rather than left to a counter to notice.
    const int with_overflow = count_nodes(clipping_root, [](const IRNode& n) {
        return n.style.overflow.has_value();
    });
    CHECK(with_overflow == 0);
}

// ── The clip model, against Chrome's own captures ───────────────────────────
//
// The cases above build the DOM snapshot by hand, which proves the algorithm
// and nothing about the data. These three run the SAME lowering over real
// `DOMSnapshot.captureSnapshot` output from Chrome, entered the way an import
// enters it, so the property spellings, the serialized `matrix(...)`, the
// sub-pixel boxes and the paint-order ties are Chrome's rather than ours.

TEST_CASE("a real capture of a containing-block escape clips nothing",
          "[browser-capture][native-lowering][clip-model]") {
    // `clip-escape/c.html`: `#esc` is `position: absolute` inside a
    // `position: static; overflow: hidden` box, and its containing block is the
    // `#panel` above that box. Chrome paints it in full at (120,260) — 120px
    // clear of the 100x100 clipper, which does not touch it. Clipped by DOM
    // parentage the node has no pixels left at all.
    const auto lowered = lower_capture("browser-capture-clip-escape");
    REQUIRE(lowered.error.empty());
    REQUIRE(lowered.design_ir);
    const auto& root = lowered.design_ir->root;

    // The counters are absent, not zero: they are recorded only when they fire.
    CHECK(attribute(root, "native_nodes_clip_over_applied").empty());
    CHECK(attribute(root, "native_nodes_clip_lost").empty());

    const auto* escaped = find_named(root, "div#esc");
    REQUIRE(escaped != nullptr);
    CHECK_FALSE(escaped->style.clip_rect.has_value());
    CHECK(*escaped->style.width == 200.0f);
    // It really is emitted under the clipper — the disagreement is resolved by
    // the clip travelling with the node, not by moving the node.
    const auto* clipper = find_named(root, "div#clip");
    REQUIRE(clipper != nullptr);
    CHECK(contains(*clipper, escaped));
    CHECK_FALSE(clipper->style.overflow.has_value());
}

TEST_CASE("a real capture of a hoist keeps the clip on the hoisted node",
          "[browser-capture][native-lowering][clip-model]") {
    // `clip-hoist/h.html`: `#under` has `z-index: -1`, so Chrome paints it
    // before the `#card` that contains it and lowering hoists it out. `#card`
    // is `position: absolute; overflow: hidden` with a 1px border, so it IS
    // `#under`'s containing block and DOES clip it — to its padding box, the
    // 100x100 at (151,151). `#under` is 200 wide at (91,171): without the clip
    // it paints roughly twice the ink Chrome shows.
    const auto lowered = lower_capture("browser-capture-clip-hoist");
    REQUIRE(lowered.error.empty());
    REQUIRE(lowered.design_ir);
    const auto& root = lowered.design_ir->root;

    CHECK(attribute(root, "native_nodes_clip_over_applied").empty());
    CHECK(attribute(root, "native_nodes_clip_lost").empty());
    CHECK(attribute(root, "native_nodes_hoisted") == "1");

    const auto* under = find_named(root, "div#under");
    REQUIRE(under != nullptr);
    CHECK(attribute(*under, "paint_order_hoisted") == "1");
    REQUIRE(under->style.clip_rect.has_value());
    CHECK(under->style.clip_rect->x == 60.0f);
    CHECK(under->style.clip_rect->y == -20.0f);
    CHECK(under->style.clip_rect->width == 100.0f);
    CHECK(under->style.clip_rect->height == 100.0f);
}

TEST_CASE("a real capture where the clip agrees reports no disagreement",
          "[browser-capture][native-lowering][clip-model]") {
    // `clip-transform/rot.html`: an `overflow: hidden` panel holding a rotated
    // bar and a scaled box, both `position: absolute` with the panel as their
    // containing block. Nothing escapes and nothing is hoisted, so both
    // counters must stay silent — the control that keeps the two cases above
    // from passing for the trivial reason that the audit never fires.
    const auto lowered = lower_capture("browser-capture-clip-transform");
    REQUIRE(lowered.error.empty());
    REQUIRE(lowered.design_ir);
    const auto& root = lowered.design_ir->root;

    CHECK(attribute(root, "native_nodes_clip_over_applied").empty());
    CHECK(attribute(root, "native_nodes_clip_lost").empty());
    CHECK(attribute(root, "native_nodes_hoisted").empty());

    // The rotated bar is still reported as un-drawable rather than painted as
    // its bounding box, which is the other claim this capture was made for.
    const auto* bar = find_named(root, "div#bar");
    REQUIRE(bar != nullptr);
    CHECK(attribute(*bar, "capture_fallback_reason") == "transform");
}

TEST_CASE("the reorder audit counts a real inversion and only a visible one",
          "[browser-capture][native-lowering]") {
    // The other cases assert this diagnostic is zero, which a counter wired to
    // zero also satisfies — deleting the audit outright passes them all. So it
    // is driven to a known non-zero value here, over a document whose subtrees
    // interleave in Chrome's numbering.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 DIV outer, 4 DIV inside it,
    //        5 DIV sibling of outer. Chrome paints outer(2), sibling(3),
    //        inner(4); nesting composes outer, inner, sibling — so the
    //        inner/sibling pair is emitted against Chrome's order.
    const std::string names = "[0,1,2,3,3,3]";
    const std::string types = "[9,1,1,1,1,1]";
    const std::string parents = "[-1,0,1,2,3,2]";
    const std::string attributes = "[[],[],[],[],[],[]]";
    const std::string styles =
        "[[11,14],[11,14],[11,14],[11,14],[11,14],[11,14]]";
    const std::string paint_orders = "[0,1,1,2,4,3]";

    const auto overlapping = lower_snapshot(
        {
            .node_names = names,
            .node_types = types,
            .parents = parents,
            .attributes = attributes,
            .layout_nodes = "[0,1,2,3,4,5]",
            .styles = styles,
            .bounds = "[[0,0,300,300],[0,0,300,300],[0,0,300,300],"
                      "[20,20,100,100],[40,40,60,60],[60,60,100,100]]",
            .paint_orders = paint_orders,
        },
        "reorder-visible");
    CHECK(overlapping.counts.lowered == 5);
    CHECK(overlapping.counts.hoisted_escapes == 0);  // not the same effect
    CHECK(overlapping.counts.overlapping_reorders == 1);

    // The identical inversion with the two boxes moved apart. A painter cannot
    // show a reorder of disjoint boxes, so the audit must NOT count it — which
    // is what makes the number above a fidelity measure rather than a tally of
    // how hierarchical the document happens to be.
    const auto disjoint = lower_snapshot(
        {
            .node_names = names,
            .node_types = types,
            .parents = parents,
            .attributes = attributes,
            .layout_nodes = "[0,1,2,3,4,5]",
            .styles = styles,
            .bounds = "[[0,0,300,300],[0,0,300,300],[0,0,300,300],"
                      "[20,20,100,100],[40,40,60,60],[200,200,50,50]]",
            .paint_orders = paint_orders,
        },
        "reorder-invisible");
    CHECK(disjoint.counts.lowered == 5);
    CHECK(disjoint.counts.overlapping_reorders == 0);
}

TEST_CASE("a layout object Chrome did not rank is counted, not assumed",
          "[browser-capture][native-lowering]") {
    // Absent paint order is reported rather than defaulted, because zero is a
    // legitimate rank: silently reading it as zero reorders the panel into
    // document order while every number still looks like real data. The other
    // cases assert this counter is zero, so it is driven non-zero here.
    //
    // nodes: 0 #document, 1 HTML (unranked), 2 BODY, 3 DIV
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,3]",
            .node_types = "[9,1,1,1]",
            .parents = "[-1,0,1,2]",
            .attributes = "[[],[],[],[]]",
            .layout_nodes = "[0,1,2,3]",
            .styles = "[[11,14],[11,14],[11,14],[11,14]]",
            .bounds = "[[0,0,50,50],[0,0,50,50],[0,0,50,50],[10,10,20,20]]",
            .paint_orders = "[0,-1,0,1]",
        },
        "unranked");
    CHECK(lowered.counts.missing_paint_order == 1);
    CHECK(lowered.counts.lowered == 3);
    CHECK(lowered.counts.hoisted_escapes == 0);
    // It is still lowered, carrying the rank Chrome did not give it, so a
    // consumer sees the gap rather than a plausible zero.
    const auto* unranked = find_named(lowered.root, "html");
    REQUIRE(unranked != nullptr);
    CHECK(attribute(*unranked, "paint_order") == "-1");
}

TEST_CASE("a collapsed whitespace run is dropped and a real one is not",
          "[browser-capture][native-lowering]") {
    // Chrome lays out the whitespace between elements as its own text run with
    // a real box. Emitting it adds an invisible node to every gap in the
    // design; dropping ALL text is the failure in the other direction, and a
    // counter only ever asserted at zero cannot tell the two apart.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 DIV, 4 #text "   ",
    //        5 DIV, 6 #text "Level"
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,3,28,3,28]",
            .node_types = "[9,1,1,1,3,1,3]",
            .parents = "[-1,0,1,2,3,2,5]",
            .attributes = "[[],[],[],[],[],[],[]]",
            .layout_nodes = "[0,1,2,3,4,5,6]",
            .styles = "[[11,14],[11,14],[11,14],[11,14],[11,14],[11,14],"
                      "[11,14]]",
            .bounds = "[[0,0,200,200],[0,0,200,200],[0,0,200,200],"
                      "[10,10,80,20],[10,10,80,20],[10,40,80,20],"
                      "[10,40,80,20]]",
            .paint_orders = "[0,1,1,2,3,4,5]",
            .texts = "[-1,-1,-1,-1,29,-1,30]",
        },
        "blank-text");
    CHECK(lowered.counts.skipped_blank_text == 1);
    // Not the zero-area gate: the whitespace run has a real box, and only the
    // blank-text test can be what dropped it.
    CHECK(lowered.counts.skipped_empty_box == 0);
    CHECK(lowered.counts.text == 1);
    CHECK(lowered.counts.lowered == 5);

    const auto* text = find_by_text(lowered.root, "Level");
    REQUIRE(text != nullptr);
    CHECK(count_nodes(lowered.root, [](const IRNode& node) {
              return node.type == "text";
          }) == 1);
}

TEST_CASE("a zero-area layout object is not lowered",
          "[browser-capture][native-lowering]") {
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,3]",
            .node_types = "[9,1,1,1]",
            .parents = "[-1,0,1,2]",
            .attributes = "[[],[],[],[]]",
            .layout_nodes = "[0,1,2,3]",
            .styles = "[[11,14],[11,14],[11,14],[11,14]]",
            .bounds = "[[0,0,50,50],[0,0,50,50],[0,0,50,50],[10,10,0,0]]",
            .paint_orders = "[0,1,1,2]",
        },
        "empty");
    CHECK(lowered.counts.skipped_empty_box == 1);
    CHECK(lowered.counts.lowered == 2);
}

TEST_CASE("a child that paints before its parent is hoisted and flagged",
          "[browser-capture][native-lowering]") {
    // A nested painter draws a parent's own box before anything inside it, so
    // a negative-`z-index` child — or a descendant that escaped to an outer
    // stacking context — has an order that cannot be expressed in place. It is
    // moved out and SAID SO. Silently leaving it nested would paint it over the
    // parent it belongs under, which looks deliberate and is not.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 DIV, 4 DIV inside it painting first
    const auto lowered = lower_snapshot(
        {
            .node_names = "[0,1,2,3,3]",
            .node_types = "[9,1,1,1,1]",
            .parents = "[-1,0,1,2,3]",
            .attributes = "[[],[],[],[],[]]",
            .layout_nodes = "[0,1,2,3,4]",
            .styles = "[[11,14],[11,14],[11,14],[11,14],[11,14]]",
            .bounds = "[[0,0,200,200],[0,0,200,200],[0,0,200,200],"
                      "[20,20,100,100],[30,30,40,40]]",
            .paint_orders = "[0,1,1,5,2]",
        },
        "hoist");
    const auto& counts = lowered.counts;
    const auto& root = lowered.root;
    CHECK(counts.lowered == 4);
    CHECK(counts.hoisted_escapes == 1);

    const auto* body = find_named(root, "body");
    REQUIRE(body != nullptr);
    // Both DIVs are now siblings under <body>, in Chrome's order.
    REQUIRE(body->children.size() == 2);
    const auto& escaped = body->children[0];
    const auto& outer = body->children[1];
    CHECK(attribute(escaped, "paint_order") == "2");
    CHECK(attribute(outer, "paint_order") == "5");

    // The diagnostic is on the node, naming what it was taken out of.
    CHECK(attribute(escaped, "paint_order_hoisted") == "1");
    CHECK(attribute(escaped, "hoisted_from") == *outer.stable_anchor_id);
    CHECK(attribute(outer, "paint_order_hoisted").empty());

    // Its anchor still records where it lives in the DOM. Hoisting is a
    // rendering accommodation, not a claim about the design's structure, and an
    // edit stored against it has to survive the accommodation going away.
    CHECK(*escaped.stable_anchor_id == "capture:html[0]/body[0]/div[0]/div[0]");

    // Hoisting is what keeps the composed order faithful, so nothing was
    // reordered against Chrome in the process.
    CHECK(counts.overlapping_reorders == 0);
}

TEST_CASE("anchors survive a differently serialized capture",
          "[browser-capture][native-lowering]") {
    // The same document, captured twice: the layout array is in a different
    // order and the paint orders differ, which is what a re-capture of a live
    // page routinely produces. The anchors an agent's edits are stored against
    // must not notice. An anchor keyed on a position within one capture's own
    // serialization — a layout index — fails exactly here.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 DIV, 4 DIV, 5 DIV inside node 3
    const std::string names = "[0,1,2,3,3,3]";
    const std::string types = "[9,1,1,1,1,1]";
    const std::string parents = "[-1,0,1,2,2,3]";
    const std::string attributes = "[[],[],[],[],[],[]]";
    const std::string styles =
        "[[11,14],[11,14],[11,14],[11,14],[11,14],[11,14]]";

    const auto a = lower_snapshot(
        {
            .node_names = names,
            .node_types = types,
            .parents = parents,
            .attributes = attributes,
            .layout_nodes = "[0,1,2,3,4,5]",
            .styles = styles,
            .bounds = "[[0,0,200,200],[0,0,200,200],[0,0,200,200],"
                      "[10,10,80,80],[100,10,80,80],[20,20,40,40]]",
            .paint_orders = "[0,1,1,2,3,4]",
        },
        "anchors-a");
    const auto b = lower_snapshot(
        {
            .node_names = names,
            .node_types = types,
            .parents = parents,
            .attributes = attributes,
            // node 4 serialized before node 3, and ranked differently
            .layout_nodes = "[0,1,2,4,3,5]",
            .styles = styles,
            .bounds = "[[0,0,200,200],[0,0,200,200],[0,0,200,200],"
                      "[100,10,80,80],[10,10,80,80],[20,20,40,40]]",
            .paint_orders = "[0,1,1,3,2,4]",
        },
        "anchors-b");
    CHECK(a.counts.lowered == 5);
    CHECK(b.counts.lowered == 5);

    const auto list_a = anchors(a.root);
    const auto list_b = anchors(b.root);
    const std::set<std::string> set_a(list_a.begin(), list_a.end());
    const std::set<std::string> set_b(list_b.begin(), list_b.end());
    CHECK(set_a == set_b);
    CHECK(set_a == std::set<std::string>{
                       "capture:html[0]",
                       "capture:html[0]/body[0]",
                       "capture:html[0]/body[0]/div[0]",
                       "capture:html[0]/body[0]/div[0]/div[0]",
                       "capture:html[0]/body[0]/div[1]",
                   });
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
        WARN("  total nodes in tree: "
             << count_nodes(root, [](const IRNode&) { return true; }));
        for (const char* key : {"native_painted_nodes", "native_nodes_lowered",
                                "native_nodes_native",
                                "native_nodes_image_asset",
                                "native_nodes_element_capture_fallback",
                                "native_nodes_text", "native_nodes_pooled",
                                "native_nodes_missing_paint_order",
                                "native_tree_root_children",
                                "native_tree_depth",
                                "native_nodes_hoisted",
                                "native_nodes_overlapping_reorders",
                                "native_nodes_clip_over_applied",
                                "native_nodes_clip_lost",
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
    CHECK(count_nodes(after, [](const IRNode&) { return true; }) >
          count_nodes(before, [](const IRNode&) { return true; }));

    // A real design is nested, and every node in it is anchorable. Both are
    // asserted here rather than only read off the report, so a regression that
    // reflattens the tree fails a test instead of changing a printed number
    // nobody diffs.
    const auto tree_depth = [&after]() {
        int deepest = 0;
        for (const auto& entry : composed(after))
            deepest = std::max(deepest, entry.depth);
        return deepest;
    }();
    CHECK(tree_depth >= 3);
    for (const auto& entry : composed(after)) {
        if (entry.node->attributes.count("paint_class") == 0) continue;
        INFO("node " << entry.node->name);
        REQUIRE(entry.node->stable_anchor_id);
        CHECK_FALSE(entry.node->stable_anchor_id->empty());
    }
}

// Writes the natively-lowered IR for a capture directory so the panel can be
// rendered from its own nodes and scored against Chrome's pixels. Separate tag
// from the census so neither drags the other into a run that has no capture.
//   PULP_NATIVE_LOWERING_CAPTURE=<capture-dir> \
//   PULP_NATIVE_LOWERING_IR_OUT=<path.ir.json> \
//     pulp-test-browser-capture-import "[.native-ir-emit]" -s
TEST_CASE("natively lowered IR is emitted for a real captured design",
          "[.native-ir-emit]") {
    const char* directory = std::getenv("PULP_NATIVE_LOWERING_CAPTURE");
    const char* out = std::getenv("PULP_NATIVE_LOWERING_IR_OUT");
    REQUIRE(directory != nullptr);
    REQUIRE(out != nullptr);
    const auto envelope = fs::path(directory) / "capture.json";
    REQUIRE(fs::is_regular_file(envelope));

    BrowserCaptureIrOptions native_options;
    native_options.native_panel_lowering = true;
    const auto native = lower_browser_capture_to_ir(envelope, native_options);
    INFO("native error: " << native.error);
    REQUIRE(native.design_ir);

    // The photograph must be gone from the tree before anything downstream
    // reads it. Asserted here as well as on the written artifact, because a
    // score over a composite that still contains the capture measures nothing.
    REQUIRE_FALSE(any_capture_node(native.design_ir->root));

    for (const char* key : {"native_painted_nodes", "native_nodes_native",
                            "native_nodes_image_asset",
                            "native_nodes_element_capture_fallback",
                            "native_nodes_text", "native_nodes_pooled",
                            "native_nodes_missing_paint_order",
                            "native_tree_depth", "native_nodes_hoisted",
                            "native_nodes_overlapping_reorders",
                            "native_nodes_clip_over_applied",
                            "native_nodes_clip_lost",
                            "native_nodes_skipped_empty_box",
                            "native_nodes_skipped_blank_text",
                            "native_nodes_skipped_non_visual"}) {
        const auto it = native.design_ir->root.attributes.find(key);
        if (it != native.design_ir->root.attributes.end())
            WARN("  " << key << " = " << it->second);
    }
    WARN("  root_width = " << native.design_ir->root.style.width.value_or(-1));
    WARN("  root_height = " << native.design_ir->root.style.height.value_or(-1));

    std::ofstream stream(out, std::ios::binary);
    REQUIRE(stream.good());
    stream << pulp::view::serialize_design_ir(*native.design_ir);
    stream.close();
    REQUIRE(fs::is_regular_file(out));
    REQUIRE(fs::file_size(out) > 0);
}
