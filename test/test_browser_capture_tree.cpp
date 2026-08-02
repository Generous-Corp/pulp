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

    // Absolute placement is the whole design of this lane: Yoga must have
    // nothing left to solve, at every depth and not merely at the top.
    for (const auto& entry : all) {
        if (entry.node->attributes.count("paint_class") == 0) continue;
        REQUIRE(entry.node->style.position);
        CHECK(*entry.node->style.position == "absolute");
        CHECK(entry.node->style.width);
        CHECK(entry.node->style.height);
    }
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
        CHECK(entry.node->anchor_strategy.value_or("") == "path");
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

    const auto* image = find_node(root, [](const IRNode& node) {
        return node.type == "image";
    });
    REQUIRE(image != nullptr);
    CHECK(attribute(*image, "src") == "logo.png");
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

TEST_CASE("a child that paints before its parent is hoisted and flagged",
          "[browser-capture][native-lowering]") {
    // A nested painter draws a parent's own box before anything inside it, so
    // a negative-`z-index` child — or a descendant that escaped to an outer
    // stacking context — has an order that cannot be expressed in place. It is
    // moved out and SAID SO. Silently leaving it nested would paint it over the
    // parent it belongs under, which looks deliberate and is not.
    //
    // nodes: 0 #document, 1 HTML, 2 BODY, 3 DIV, 4 DIV inside it painting first
    const auto json = snapshot_with(
        /*names*/     "[0,1,2,3,3]",
        /*types*/     "[9,1,1,1,1]",
        /*parents*/   "[-1,0,1,2,3]",
        /*attributes*/"[[],[],[],[],[]]",
        /*layout*/    "[0,1,2,3,4]",
        /*styles*/    "[[11,14],[11,14],[11,14],[11,14],[11,14]]",
        /*bounds*/    "[[0,0,200,200],[0,0,200,200],[0,0,200,200],"
                      "[20,20,100,100],[30,30,40,40]]",
        /*paint*/     "[0,1,1,5,2]");
    const auto path = write_snapshot(json, "hoist");
    const auto index = CapturedStyleIndex::load(path);
    REQUIRE(index);

    IRNode root;
    const auto counts = lower_painted_tree(*index, 0.0, 0.0, root);
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
    fs::remove(path);
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

    const auto first = write_snapshot(
        snapshot_with(names, types, parents, attributes,
                      /*layout*/ "[0,1,2,3,4,5]", styles,
                      /*bounds*/ "[[0,0,200,200],[0,0,200,200],[0,0,200,200],"
                                 "[10,10,80,80],[100,10,80,80],[20,20,40,40]]",
                      /*paint*/ "[0,1,1,2,3,4]"),
        "anchors-a");
    const auto second = write_snapshot(
        snapshot_with(names, types, parents, attributes,
                      // node 4 serialized before node 3, and ranked differently
                      /*layout*/ "[0,1,2,4,3,5]", styles,
                      /*bounds*/ "[[0,0,200,200],[0,0,200,200],[0,0,200,200],"
                                 "[100,10,80,80],[10,10,80,80],[20,20,40,40]]",
                      /*paint*/ "[0,1,1,3,2,4]"),
        "anchors-b");

    const auto index_a = CapturedStyleIndex::load(first);
    const auto index_b = CapturedStyleIndex::load(second);
    REQUIRE(index_a);
    REQUIRE(index_b);

    IRNode root_a;
    IRNode root_b;
    const auto counts_a = lower_painted_tree(*index_a, 0.0, 0.0, root_a);
    const auto counts_b = lower_painted_tree(*index_b, 0.0, 0.0, root_b);
    CHECK(counts_a.lowered == 5);
    CHECK(counts_b.lowered == 5);

    const auto list_a = anchors(root_a);
    const auto list_b = anchors(root_b);
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

    fs::remove(first);
    fs::remove(second);
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
