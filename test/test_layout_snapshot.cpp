#include <pulp/view/layout_snapshot.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

#include <pulp/state/store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <choc/text/choc_JSON.h>

#include <memory>
#include <sstream>
#include <string>
#include <string_view>

using namespace pulp::view;

namespace {

struct FixtureIds {
    std::string panel = "panel";
    std::string title = "title";
    std::string fill = "fill";
};

std::unique_ptr<View> build_handwritten_fixture(const FixtureIds& ids = {}) {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 240, 80});

    auto panel = std::make_unique<View>();
    panel->set_id(ids.panel);
    panel->set_overflow(View::Overflow::hidden);
    panel->flex().direction = FlexDirection::row;
    panel->flex().preferred_width = 240.0f;
    panel->flex().preferred_height = 80.0f;
    panel->flex().padding = 10.0f;
    panel->flex().gap = 8.0f;

    auto title = std::make_unique<Label>("Rate");
    title->set_id(ids.title);
    title->flex().preferred_width = 80.0f;
    title->flex().preferred_height = 20.0f;

    auto fill = std::make_unique<View>();
    fill->set_id(ids.fill);
    fill->set_z_index(2);
    fill->flex().flex_grow = 1.0f;
    fill->flex().preferred_height = 20.0f;

    panel->add_child(std::move(title));
    panel->add_child(std::move(fill));
    root->add_child(std::move(panel));
    root->layout_children();
    return root;
}

std::string minimal_live_react_shim() {
    return R"JS(
(function() {
  function flatten(input, out) {
    if (Array.isArray(input)) {
      for (var i = 0; i < input.length; i++) flatten(input[i], out);
    } else if (input !== null && input !== undefined && input !== false && input !== true) {
      out.push(input);
    }
  }

  function createElement(type, props) {
    var children = [];
    for (var i = 2; i < arguments.length; i++) flatten(arguments[i], children);
    return { type: type, props: props || {}, children: children };
  }

  function cssValue(key, value) {
    if (value == null) return "";
    if (typeof value === "number") {
      if (key === "flexGrow" || key === "flexShrink" || key === "opacity" ||
          key === "zIndex" || key === "lineHeight") {
        return String(value);
      }
      return String(value) + "px";
    }
    return String(value);
  }

  function applyProps(el, props) {
    props = props || {};
    for (var key in props) {
      if (key === "children" || key === "key") continue;
      var value = props[key];
      if (key === "style" && value) {
        for (var styleKey in value) el.style[styleKey] = cssValue(styleKey, value[styleKey]);
      } else if (key === "id") {
        el.id = String(value);
        el.setAttribute("id", String(value));
      } else if (value !== false && value != null) {
        el.setAttribute(key, String(value));
      }
    }
  }

  function renderNode(node) {
    if (node == null || node === false || node === true) return null;
    if (typeof node === "string" || typeof node === "number") {
      return document.createTextNode(String(node));
    }
    if (typeof node.type === "function") {
      var props = Object.assign({}, node.props || {});
      props.children = node.children;
      return renderNode(node.type(props));
    }
    var el = document.createElement(node.type);
    applyProps(el, node.props);
    var scalarText = "";
    var nonScalarChildren = [];
    for (var i = 0; i < node.children.length; i++) {
      var child = node.children[i];
      if (typeof child === "string" || typeof child === "number") {
        scalarText += String(child);
      } else {
        nonScalarChildren.push(child);
      }
    }
    if (scalarText) el.textContent = scalarText;
    for (var j = 0; j < nonScalarChildren.length; j++) {
      var rendered = renderNode(nonScalarChildren[j]);
      if (rendered) el.appendChild(rendered);
    }
    return el;
  }

  globalThis.React = { createElement: createElement };
  globalThis.ReactDOM = {
    createRoot: function(mount) {
      return {
        render: function(element) {
          var node = renderNode(element);
          if (node) mount.appendChild(node);
        }
      };
    },
    flushSync: function(fn) { return fn(); }
  };
})();
)JS";
}

std::unique_ptr<View> build_live_react_fixture() {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 240, 80});

    ScriptEngine engine;
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, *root, store);

    bridge.load_script(minimal_live_react_shim());
    bridge.load_script(R"JS(
function Panel() {
  return React.createElement(
    'div',
    {
      id: 'panel',
      style: {
        display: 'flex',
        flexDirection: 'row',
        width: 240,
        height: 80,
        padding: 10,
        gap: 8,
        overflow: 'hidden'
      }
    },
    React.createElement(
      'span',
      { id: 'title', style: { width: 80, height: 20 } },
      'Rate'
    ),
    React.createElement(
      'div',
      { id: 'fill', style: { flexGrow: 1, height: 20, zIndex: 2 } }
    )
  );
}

ReactDOM.createRoot(document.body).render(React.createElement(Panel));
layout();
)JS");
    root->layout_children();
    return root;
}

std::string diff_messages(const LayoutTreeDiff& diff) {
    std::ostringstream out;
    for (const auto& message : diff.messages) out << message << '\n';
    return out.str();
}

bool diff_contains(const LayoutTreeDiff& diff, std::string_view needle) {
    for (const auto& message : diff.messages) {
        if (message.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST_CASE("dump_layout_tree emits deterministic semantic layout JSON",
          "[view][layout-snapshot]") {
    auto root = build_handwritten_fixture();
    auto json = dump_layout_tree(
        *root,
        {.surface = "phase2-test", .fixture = "handwritten", .viewport_width = 240, .viewport_height = 80});
    auto snapshot = choc::json::parse(json);

    REQUIRE(snapshot["schema_version"].getWithDefault<std::string>("") == "visual-layout-snapshot-v1");
    REQUIRE(snapshot["surface"].getWithDefault<std::string>("") == "phase2-test");
    REQUIRE(snapshot["fixture"].getWithDefault<std::string>("") == "handwritten");
    REQUIRE(snapshot["nodes"].isArray());
    REQUIRE(snapshot["nodes"].size() == 4);
    REQUIRE(snapshot["nodes"][1]["id"].getWithDefault<std::string>("") == "panel");
    REQUIRE(snapshot["nodes"][1]["overflow"].getWithDefault<std::string>("") == "hidden");
    REQUIRE(snapshot["nodes"][2]["kind"].getWithDefault<std::string>("") == "Label");
    REQUIRE(snapshot["nodes"][2]["measured_text_boxes"].isArray());
    REQUIRE(snapshot["nodes"][2]["measured_text_boxes"].size() == 1);
    REQUIRE(snapshot["nodes"][2]["measured_text_boxes"][0]["text"].getWithDefault<std::string>("") == "Rate");
}

TEST_CASE("layout-tree parity oracle compares live React render to handwritten native tree",
          "[view][layout-snapshot][import-design][phase-2]") {
    auto live = build_live_react_fixture();
    REQUIRE(live->child_count() == 1);
    auto* live_panel = live->child_at(0);
    REQUIRE(live_panel != nullptr);
    REQUIRE(live_panel->child_count() == 2);
    auto* live_title = live_panel->child_at(0);
    auto* live_fill = live_panel->child_at(1);
    REQUIRE(live_title != nullptr);
    REQUIRE(live_fill != nullptr);

    // The web-compat DOM keeps author `id` attributes in JS metadata while
    // the native View tree uses deterministic bridge ids. The layout oracle
    // compares native layout snapshots, so the handwritten equivalent mirrors
    // those native identities.
    auto handwritten = build_handwritten_fixture({
        .panel = live_panel->id(),
        .title = live_title->id(),
        .fill = live_fill->id(),
    });

    const LayoutTreeSnapshotOptions options{
        .surface = "phase2-parity",
        .fixture = "react-vs-handwritten",
        .viewport_width = 240,
        .viewport_height = 80,
    };
    const auto live_json = dump_layout_tree(*live, options);
    const auto handwritten_json = dump_layout_tree(*handwritten, options);

    LayoutTreeDiff diff;
    const bool equivalent = layout_tree_snapshots_equivalent(live_json, handwritten_json, {}, &diff);
    INFO(diff_messages(diff));
    INFO("live:\n" << live_json);
    INFO("handwritten:\n" << handwritten_json);
    REQUIRE(equivalent);
}

TEST_CASE("layout-tree parity oracle reports divergent snapshots",
          "[view][layout-snapshot]") {
    const std::string expected = R"JSON(
{
  "schema_version": "visual-layout-snapshot-v1",
  "surface": "phase2-test",
  "nodes": [
    {
      "id": "node-a",
      "kind": "View",
      "rect": { "x": 0, "y": 0, "w": 10, "h": 10 },
      "visible": true,
      "overflow": "visible",
      "z_order": { "paint": 0, "z_index": 0 },
      "clipping": { "rect": { "x": 0, "y": 0, "w": 10, "h": 10 } },
      "measured_text_boxes": [
        { "text": "Rate", "rect": { "x": 0, "y": 0, "w": 30, "h": 20 } }
      ]
    }
  ]
}
)JSON";

    const std::string actual = R"JSON(
{
  "schema_version": "visual-layout-snapshot-v1",
  "surface": "phase2-test",
  "nodes": [
    {
      "id": "node-b",
      "kind": "Label",
      "rect": { "x": 1, "y": 0, "w": 11, "h": 10 },
      "visible": false,
      "overflow": "hidden",
      "z_order": { "paint": 2, "z_index": 3 },
      "clipping": { "rect": { "x": 0, "y": 0, "w": 9, "h": 10 } },
      "measured_text_boxes": [
        { "text": "Gain", "rect": { "x": 0, "y": 0, "w": 33, "h": 22 } }
      ]
    }
  ]
}
)JSON";

    LayoutTreeDiff diff;
    REQUIRE_FALSE(layout_tree_snapshots_equivalent(actual, expected, {}, &diff));
    REQUIRE(diff_contains(diff, ".id expected"));
    REQUIRE(diff_contains(diff, ".kind expected"));
    REQUIRE(diff_contains(diff, ".visible expected"));
    REQUIRE(diff_contains(diff, ".overflow expected"));
    REQUIRE(diff_contains(diff, ".rect.x expected"));
    REQUIRE(diff_contains(diff, ".z_order.paint"));
    REQUIRE(diff_contains(diff, ".clipping.rect.w expected"));
    REQUIRE(diff_contains(diff, ".measured_text_boxes[0].text expected"));
    REQUIRE(diff_contains(diff, ".measured_text_boxes[0].rect.w expected"));
}

TEST_CASE("layout-tree parity oracle rejects invalid snapshot JSON",
          "[view][layout-snapshot]") {
    LayoutTreeDiff parse_diff;
    REQUIRE_FALSE(layout_tree_snapshots_equivalent("{", "{}", {}, &parse_diff));
    REQUIRE(diff_contains(parse_diff, "actual JSON parse failed"));

    LayoutTreeDiff shape_diff;
    REQUIRE_FALSE(layout_tree_snapshots_equivalent(
        R"JSON({"schema_version":"visual-layout-snapshot-v1","nodes":{}})JSON",
        R"JSON({"schema_version":"visual-layout-snapshot-v1","nodes":[]})JSON",
        {},
        &shape_diff));
    REQUIRE(diff_contains(shape_diff, "snapshot nodes missing array"));

    LayoutTreeDiff count_diff;
    REQUIRE_FALSE(layout_tree_snapshots_equivalent(
        R"JSON({"schema_version":"visual-layout-snapshot-v1","nodes":[]})JSON",
        R"JSON({
          "schema_version":"visual-layout-snapshot-v1",
          "nodes":[
            {
              "id":"",
              "kind":"View",
              "rect":{"x":0,"y":0,"w":1,"h":1},
              "visible":true,
              "overflow":"visible",
              "z_order":{"paint":0,"z_index":0},
              "clipping":{"rect":{"x":0,"y":0,"w":1,"h":1}},
              "measured_text_boxes":[]
            }
          ]
        })JSON",
        {},
        &count_diff));
    REQUIRE(diff_contains(count_diff, "node count expected 1 got 0"));
}
